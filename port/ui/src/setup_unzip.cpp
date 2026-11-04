// The first Python's zip, unpacked by the host (see setup_unzip.hpp). The layout is PKWARE's APPNOTE.TXT: 4.3.7 (an
// entry's local header), 4.3.12 (its central directory header) and 4.3.16 (the end of the central directory).
#include "setup_unzip.hpp"

#include "crc32.hpp"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace ui::setup {
namespace {

struct Entry {
    std::string name;
    uint16_t method = 0;  // 0 stored, 8 deflated
    uint32_t crc = 0;
    uint32_t packed = 0;  // its size in the archive
    uint32_t size = 0;    // and unpacked
    uint32_t local = 0;   // where its local header is
};

uint16_t u16(const std::vector<uint8_t>& b, size_t at) {
    return uint16_t(b[at] | b[at + 1] << 8);
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

bool read_all(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize(size_t(n));
    f.seekg(0);
    return n == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

// The central directory's entries. Each offset is checked against the archive's size before it is read.
bool entries(const std::vector<uint8_t>& b, std::vector<Entry>& out, std::string& why) {
    // The end record: 22 bytes, then a comment of up to 65,535; the last one whose comment ends the file is it.
    if (b.size() < 22) {
        why = "too short for a zip";
        return false;
    }
    size_t end = SIZE_MAX;
    const size_t lowest = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
    for (size_t at = b.size() - 22;; at--) {
        if (u32(b, at) == 0x06054b50u && at + 22 + u16(b, at + 20) == b.size()) {
            end = at;
            break;
        }
        if (at == lowest) break;
    }
    if (end == SIZE_MAX) {
        why = "no end of central directory";
        return false;
    }
    const uint16_t count = u16(b, end + 10);
    const uint32_t dirSize = u32(b, end + 12), dirAt = u32(b, end + 16);
    if (u16(b, end + 4) != 0 || u16(b, end + 6) != 0 || u16(b, end + 8) != count) {
        why = "a zip on several disks";
        return false;
    }
    if (count == 0xFFFF || dirSize == 0xFFFFFFFFu || dirAt == 0xFFFFFFFFu) {
        why = "a zip64 archive";
        return false;
    }
    if (uint64_t(dirAt) + dirSize > end) {
        why = "its central directory is outside it";
        return false;
    }
    size_t at = dirAt;
    for (unsigned i = 0; i < count; i++) {
        if (at + 46 > end || u32(b, at) != 0x02014b50u) {
            why = "entry " + std::to_string(i) + " has no central directory header";
            return false;
        }
        Entry e;
        const uint16_t flags = u16(b, at + 8);
        e.method = u16(b, at + 10);
        e.crc = u32(b, at + 16);
        e.packed = u32(b, at + 20);
        e.size = u32(b, at + 24);
        const size_t nameLen = u16(b, at + 28), extraLen = u16(b, at + 30), commentLen = u16(b, at + 32);
        e.local = u32(b, at + 42);
        if (at + 46 + nameLen + extraLen + commentLen > end) {
            why = "entry " + std::to_string(i) + " runs past its central directory";
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(&b[at + 46]), nameLen);
        if (flags & 1) {
            why = e.name + " is encrypted";
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            why = e.name + " uses compression method " + std::to_string(e.method);
            return false;
        }
        if (e.packed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || e.local == 0xFFFFFFFFu) {
            why = e.name + " is a zip64 entry";
            return false;
        }
        out.push_back(std::move(e));
        at += 46 + nameLen + extraLen + commentLen;
    }
    if (out.empty()) {
        why = "it has no entries";
        return false;
    }
    return true;
}

// An entry's bytes, from past its local header (whose name and extra field can differ in length from the central
// directory's), inflated if they are deflated, and checked against its size and CRC-32.
bool data_of(const std::vector<uint8_t>& b, const Entry& e, std::vector<uint8_t>& out, std::string& why) {
    const size_t at = e.local;
    if (uint64_t(at) + 30 > b.size() || u32(b, at) != 0x04034b50u) {
        why = e.name + " has no local header";
        return false;
    }
    const size_t from = at + 30 + u16(b, at + 26) + u16(b, at + 28);
    if (uint64_t(from) + e.packed > b.size()) {
        why = e.name + " runs past the archive's end";
        return false;
    }
    out.assign(e.size, 0);
    if (e.method == 0) {
        if (e.packed != e.size) {
            why = e.name + " is stored with two sizes";
            return false;
        }
        std::copy(b.begin() + from, b.begin() + from + e.size, out.begin());
    } else {
        if (e.size > INT_MAX || e.packed > INT_MAX) {
            why = e.name + " is too large";
            return false;
        }
        char none;  // (an empty file's buffer)
        const int got = stbi_zlib_decode_noheader_buffer(out.empty() ? &none : reinterpret_cast<char*>(out.data()),
                                                         int(e.size), reinterpret_cast<const char*>(&b[from]),
                                                         int(e.packed));
        if (got != int(e.size)) {
            why = e.name + " does not inflate to its size";
            return false;
        }
    }
    if (crc32(out.data(), out.size()) != e.crc) {
        why = e.name + "'s CRC-32 does not match";
        return false;
    }
    return true;
}

bool read_entries(const std::filesystem::path& archive, std::vector<uint8_t>& b, std::vector<Entry>& list,
                  std::string& why) {
    if (!read_all(archive, b)) {
        why = "it could not be read";
        return false;
    }
    return entries(b, list, why);
}

}  // namespace

bool zip_names(const std::filesystem::path& archive, std::vector<std::string>& names, std::string& why) {
    std::vector<uint8_t> b;
    std::vector<Entry> list;
    if (!read_entries(archive, b, list, why)) return false;
    names.clear();
    for (const Entry& e : list) names.push_back(e.name);
    return true;
}

bool zip_unpack(const std::filesystem::path& archive, const std::filesystem::path& dest, std::string& why) {
    std::vector<uint8_t> b;
    std::vector<Entry> list;
    if (!read_entries(archive, b, list, why)) return false;
    std::vector<uint8_t> data;
    for (const Entry& e : list) {
        // (the caller has checked the names; this only keeps a name it did not check inside `dest`)
        const std::filesystem::path name =
            std::filesystem::path(std::u8string(e.name.begin(), e.name.end())).lexically_normal();
        if (name.empty() || name.has_root_path() || *name.begin() == "..") {
            why = e.name + " is outside the folder";
            return false;
        }
        const std::filesystem::path to = dest / name;
        std::error_code ec;
        if (e.name.back() == '/' || e.name.back() == '\\') {  // a folder
            std::filesystem::create_directories(to, ec);
            if (ec) {
                why = e.name + ": no folder for it (" + ec.message() + ")";
                return false;
            }
            continue;
        }
        if (!data_of(b, e, data, why)) return false;
        std::filesystem::create_directories(to.parent_path(), ec);
        std::ofstream f(to, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())) || !f.flush()) {
            why = e.name + " could not be written";
            return false;
        }
    }
    return true;
}

}  // namespace ui::setup
