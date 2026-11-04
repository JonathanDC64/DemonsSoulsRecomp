// Written for Demon's Souls. The first Python's zip unpacked by the host itself, for a Windows with no tar.exe in its
// System folder (Wine and CrossOver have none): setup_python.cpp. Only what python.org's embeddable zip holds: stored
// and deflated entries (inflated by stb_image's zlib, stb_impl.cpp), on one disk, with no zip64 and no encryption. By
// then the archive is the pinned one (its size and SHA-256 are checked first), and each file's CRC-32 is checked too.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ui::setup {

// The entries' names in the archive's order, as tar -tf lists them (a folder's ends in '/'). False, with why, for an
// archive this does not read.
bool zip_names(const std::filesystem::path& archive, std::vector<std::string>& names, std::string& why);

// Every entry into the folder `dest`, which exists. The names are the caller's to check first (setup_python.cpp's
// safe_entry, as for tar's). False, with why, at the first entry that fails.
bool zip_unpack(const std::filesystem::path& archive, const std::filesystem::path& dest, std::string& why);

}  // namespace ui::setup
