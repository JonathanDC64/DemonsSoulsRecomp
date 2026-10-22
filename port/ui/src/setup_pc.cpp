// What the setup screens ask of this PC (see setup_pc.hpp). Three facts differ by platform, each a small function:
// the folders to look in, the memory, and the power. The scan, the PARAM.SFO reader, the free space and the time
// estimate are the same everywhere.
#include "setup_pc.hpp"

#include "setup_engine.hpp"

#ifdef _WIN32
#include <windows.h>
// (after windows.h)
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>
#include <thread>

namespace ui::setup {
namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- What differs by platform: the folders to look in, the memory, the power ----

#ifdef _WIN32

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// The user's Downloads, Documents and Desktop, then the roots of the fixed and removable drives (a USB drive with a
// dump on it). Not A: and B:, which are floppy drives, nor network and optical drives: slow to ask.
std::vector<std::filesystem::path> search_roots() {
    // (a card reader with no card in it: an error for the scan, never a "no disk" message box)
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
    std::vector<std::filesystem::path> out = {known_folder(FOLDERID_Downloads), known_folder(FOLDERID_Documents),
                                              known_folder(FOLDERID_Desktop)};
    const DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) out.emplace_back(root);
    }
    return out;
}

unsigned long long memory_bytes() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

// BatteryFlag 128: no battery, 255: not known. ACLineStatus 0: not plugged in (1: plugged in, 255: not known).
Power platform_power() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return {};
    Power out;
    out.battery = s.BatteryFlag != 128 && s.BatteryFlag != 255;
    out.onBattery = out.battery && s.ACLineStatus == 0;
    return out;
}

#else

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// A user folder (Downloads and the like) from user-dirs.dirs, which holds its name in the desktop's language; else
// the English name.
std::filesystem::path user_dir(const std::filesystem::path& home, const char* key, const char* fallback) {
    const std::string config = env("XDG_CONFIG_HOME");
    std::ifstream f((config.empty() ? home / ".config" : std::filesystem::path(config)) / "user-dirs.dirs");
    const std::string want = std::string(key) + "=\"";
    for (std::string line; std::getline(f, line);) {
        if (line.rfind(want, 0) != 0) continue;
        std::string v = line.substr(want.size());
        if (const size_t quote = v.find('"'); quote != std::string::npos) v.resize(quote);
        if (v.rfind("$HOME", 0) == 0) return home / std::filesystem::path(v.substr(5)).relative_path();
        if (!v.empty() && v[0] == '/') return v;
    }
    return home / fallback;
}

// The user's Downloads, Documents and Desktop, the home folder, then the mounted drives: each folder under /mnt,
// /media/<user> and /run/media/<user> is a drive's root.
std::vector<std::filesystem::path> search_roots() {
    std::vector<std::filesystem::path> out;
    if (const std::string home = env("HOME"); !home.empty()) {
        out = {user_dir(home, "XDG_DOWNLOAD_DIR", "Downloads"), user_dir(home, "XDG_DOCUMENTS_DIR", "Documents"),
               user_dir(home, "XDG_DESKTOP_DIR", "Desktop"), home};
    }
    std::vector<std::filesystem::path> mounts = {"/mnt"};
    if (const std::string user = env("USER"); !user.empty()) {
        mounts.push_back(std::filesystem::path("/media") / user);
        mounts.push_back(std::filesystem::path("/run/media") / user);
    }
    for (const auto& m : mounts) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(m, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code dirEc;
            if (it->is_directory(dirEc)) out.push_back(it->path());
        }
    }
    return out;
}

unsigned long long memory_bytes() {
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? (unsigned long long)pages * (unsigned long long)size : 0;
}

// /sys/class/power_supply: a laptop's charger is a "Mains" supply ("USB" for USB-C), with online 1 while plugged in;
// its battery a "Battery" of scope System (a pad's or a mouse's battery says Device). On battery: a system battery, a
// charger, and no charger online. A desktop has neither.
Power platform_power() {
    bool battery = false, charger = false, online = false;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/sys/class/power_supply", ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto read = [&](const char* name) {
            std::ifstream f(it->path() / name);
            std::string v;
            f >> v;
            return v;
        };
        const std::string type = read("type");
        if (type == "Battery" && read("scope") != "Device") {
            battery = true;
        } else if (type == "Mains" || type == "USB") {
            charger = true;
            online = online || read("online") == "1";
        }
    }
    Power out;
    out.battery = battery;
    out.onBattery = battery && charger && !online;
    return out;
}

#endif

// ---- The same everywhere ----

constexpr size_t kMaxPlaces = 4;
constexpr size_t kMaxFound = 12;     // the scan stops after this many
constexpr size_t kMaxEntries = 400;  // folders looked at in one folder, at most

// Folder names whose folders are looked in too (case ignored): the usual places for a few games.
constexpr const char* kGameFolders[] = {"PS3", "PS3 Games", "Games", "ISO", "ISOs", "Dumps", "Backups", "RPCS3"};

bool same_text(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

bool is_game_folder(const std::string& name) {
    for (const char* n : kGameFolders)
        if (same_text(name, n)) return true;
    return false;
}

// TITLE_ID and APP_VER from a PARAM.SFO, in the layout des_setup/sfo.py reads: a 20-byte header (the magic, the
// version, the key and data tables' offsets, the count), then 16-byte entries (the key's offset, the format, the
// length, the room, the data's offset), all little-endian. False if it is not one.
bool read_sfo(const std::filesystem::path& file, std::string& title, std::string& version) {
    std::ifstream f(file, std::ios::binary);
    std::vector<unsigned char> b(64 * 1024);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    const size_t n = size_t(f.gcount());
    if (n < 20 || std::memcmp(b.data(), "\0PSF", 4) != 0) return false;
    const auto u16 = [&](size_t at) { return size_t(b[at]) | size_t(b[at + 1]) << 8; };
    const auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    const size_t keys = u32(8), data = u32(12), count = u32(16);
    if (count > 4096 || 20 + count * 16 > n) return false;
    for (size_t i = 0; i < count; ++i) {
        const size_t e = 20 + i * 16;
        const size_t key = keys + u16(e), format = u16(e + 2), length = u32(e + 4), at = data + u32(e + 12);
        if (format == 0x0404 || key >= n || at > n || length > n - at) continue;  // (0x0404: a number)
        const char* k = reinterpret_cast<const char*>(b.data() + key);
        const std::string name(k, strnlen(k, n - key));
        std::string value(reinterpret_cast<const char*>(b.data() + at), length);
        value.resize(std::min(value.size(), value.find('\0')));
        if (name == "TITLE_ID") title = value;
        else if (name == "APP_VER") version = value;
    }
    return !title.empty();
}

struct Scan {
    std::vector<Place> found;
    std::set<std::string> seen;

    bool full() const { return found.size() >= kMaxFound; }

    // `dir` is a dump if it holds PS3_GAME/PARAM.SFO.
    void check(const std::filesystem::path& dir) {
        std::error_code ec;
        const auto sfo = dir / "PS3_GAME" / "PARAM.SFO";
        if (full() || !std::filesystem::is_regular_file(sfo, ec)) return;
        if (!seen.insert(utf8(dir.lexically_normal())).second) return;
        Place p{dir, {}, {}};
        if (read_sfo(sfo, p.title, p.version)) found.push_back(std::move(p));
    }

    // The folders in `dir` (not hidden ones, nor Windows' own such as $Recycle.Bin), and in those of them with a
    // usual name for a folder of games, `hops` levels of those at most.
    void look_in(const std::filesystem::path& dir, int hops) {
        std::error_code ec;
        size_t entries = 0;
        constexpr auto kSkipDenied = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::directory_iterator it(dir, kSkipDenied, ec), end;
             !ec && it != end && entries < kMaxEntries && !full(); it.increment(ec), ++entries) {
            std::error_code dirEc;
            if (!it->is_directory(dirEc)) continue;
            const std::string name = utf8(it->path().filename());
            if (name.empty() || name[0] == '.' || name[0] == '$') continue;
            check(it->path());
            if (hops > 0 && is_game_folder(name)) look_in(it->path(), hops - 1);
        }
    }
};

}  // namespace

std::vector<Place> likely_places() {
    Scan scan;
    for (const auto& root : search_roots()) {
        if (root.empty()) continue;
        scan.check(root);
        scan.look_in(root, 2);  // (so <root>/RPCS3/games/<dump> too)
    }
    std::stable_partition(scan.found.begin(), scan.found.end(), [](const Place& p) { return p.title == kTitleId; });
    if (scan.found.size() > kMaxPlaces) scan.found.resize(kMaxPlaces);
    return scan.found;
}

Space data_space() {
    Space out;
    const std::filesystem::path& dir = data_dir();
    out.where = utf8(dir.root_name());
    if (out.where.empty()) out.where = utf8(dir);
    std::error_code ec;
    std::filesystem::path p = dir;
    while (!p.empty() && !std::filesystem::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
    if (p.empty()) return out;
    const auto info = std::filesystem::space(p, ec);
    if (!ec) {
        out.known = true;
        out.free = info.available;
    }
    return out;
}

// The build's numbers, measured on one PC (docs/53: 8 cores, 16 threads, 31 GB). The game part's compile is about
// 2,250 s of CPU: the 14 PPU chunks without /Zi, 2,128 s (docs/53 section 2), and the SPU programs and libsre, about
// 2 minutes (docs/45 3.1). Its longest job, chunk 008, takes 261 s, which no number of jobs shortens. The check, the
// copy and the lift add about 75 s (docs/53 section 1). The jobs are counted as compile.py's default_jobs counts them
// (docs/52 4.4: 3 GB a job, 2 GB kept free), here from the PC's memory, not its free memory. No other PC has been
// measured. The range's low end is that PC's speed per core; its high end takes cores 2 times slower on a desktop
// and 4 times on a laptop (inferred, from docs/45 3.1's guesses: a 6-core desktop at 4 jobs about 20 minutes, a
// 4-core laptop 35 to 50, against 10.7 measured on that PC). The compiler's download is not in it.
constexpr double kCompileCpuSeconds = 2250.0;
constexpr double kLongestJobSeconds = 261.0;
constexpr double kOtherSeconds = 75.0;
constexpr unsigned long long kJobBytes = 3ull << 30;
constexpr unsigned long long kKeptBytes = 2ull << 30;
constexpr double kSlowerDesktop = 2.0, kSlowerLaptop = 4.0;

Estimate setup_time() {
    Estimate out;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned long long ram = memory_bytes();
    unsigned long long jobs = threads;
    if (ram > kKeptBytes) jobs = std::min(jobs, std::max(1ull, (ram - kKeptBytes) / kJobBytes));
    else if (ram != 0) jobs = 1;
    out.jobs = unsigned(jobs);
    const double seconds = kOtherSeconds + std::max(kCompileCpuSeconds / double(jobs), kLongestJobSeconds);
    const double slower = platform_power().battery ? kSlowerLaptop : kSlowerDesktop;
    out.lowMinutes = int(seconds / 60.0 + 0.5);
    out.highMinutes = int(seconds * slower / 60.0 + 0.5);
    return out;
}

Power power() {
    return platform_power();
}

}  // namespace ui::setup
