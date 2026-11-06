// Where setup builds the game: the usual folder or the player's choice (see setup_folder.hpp). One fact differs by
// platform, in one small function: whether two file names that differ only in case are the same name.
#include "setup_folder.hpp"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <system_error>

#ifdef _WIN32
#include <cwctype>
#endif

namespace ui::setup {
namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// ---- What differs by platform ----

#ifdef _WIN32
// Windows' file names ignore case.
bool same_name(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(wint_t(a[i])) != std::towlower(wint_t(b[i]))) return false;
    return true;
}
#else
bool same_name(const std::string& a, const std::string& b) {
    return a == b;
}
#endif

// ---- The same everywhere ----

// The path without "." and "..", and without a separator at its end (but a root's).
std::filesystem::path tidy(const std::filesystem::path& p) {
    std::filesystem::path out = p.lexically_normal();
    if (!out.has_filename() && out.has_relative_path()) out = out.parent_path();
    return out;
}

// `path` is `root` or in it, by their names.
bool inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto rel = tidy(path).lexically_relative(tidy(root));
    return !rel.empty() && *rel.begin() != "..";
}

// In the program's folder, a name an update replaces: the engine's folder, the update's own, and the names the
// release lists in setup/release-files.txt (des_setup/update.py's program files).
bool program_name(const std::filesystem::path& program, const std::filesystem::path& top) {
    for (const char* name : {"setup", ".update", ".previous", ".previous.old"})
        if (same_name(top.native(), std::filesystem::path(name).native())) return true;
    std::ifstream f(program / "setup" / "release-files.txt", std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (same_name(top.native(), from_utf8(line.substr(0, line.find('/'))).native())) return true;
    }
    return false;
}

}  // namespace

std::filesystem::path chosen_folder(const std::filesystem::path& usual) {
    if (usual.empty()) return {};
    std::ifstream f(usual / kChoiceFile, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // (a byte order mark: a file saved by Notepad)
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        const auto p = from_utf8(line.substr(start));
        return tidy(p.is_absolute() ? p : usual / p);
    }
    return {};
}

Folders find_folders(const std::filesystem::path& given, const std::filesystem::path& shared,
                     const std::filesystem::path& portableProgram) {
    if (!given.empty()) return {given, {}, false};
    const auto usual = portableProgram.empty() ? shared : portableProgram / "data" / "setup-data";
    if (usual.empty()) return {};
    if (const auto chosen = chosen_folder(usual); !chosen.empty() && !same_folder(chosen, usual)) {
        std::fprintf(stderr, "[setup] setup's folder: %s, chosen on setup's screens (%s in %s)\n", utf8(chosen).c_str(),
                     kChoiceFile, utf8(usual).c_str());
        return {chosen, usual, true};
    }
    // A portable install (issue #364, docs/45 section 4.4) that was built in the shared folder before (0.1.0 put it
    // there) keeps using it until it is moved here, rather than build the game again.
    std::error_code ec;
    if (!portableProgram.empty() && !std::filesystem::exists(usual / "setup", ec) && !shared.empty() &&
        std::filesystem::exists(shared / "setup" / "ready.txt", ec)) {
        std::fprintf(stderr,
                     "[setup] portable install, built in %s before: setup's folder stays there (to take it along, "
                     "move it to %s)\n",
                     utf8(shared).c_str(), utf8(usual).c_str());
        return {shared, usual, false};
    }
    return {usual, usual, false};
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
        const bool same = std::filesystem::equivalent(a, b, ec);
        if (!ec) return same;
    }
    return same_name(tidy(a).native(), tidy(b).native());
}

bool holds_setup(const std::filesystem::path& folder) {
    std::error_code ec;
    return std::filesystem::exists(folder / "setup" / "ready.txt", ec) ||
           std::filesystem::exists(folder / "setup" / "journal.json", ec);
}

std::filesystem::path folder_for_pick(const std::filesystem::path& picked) {
    const auto p = tidy(picked);
    if (p.empty()) return p;
    std::error_code ec;
    // Setup's already: its work in it, the usual folder (the choice's file in it), or one with setup's own name.
    if (holds_setup(p) || std::filesystem::exists(p / kChoiceFile, ec) ||
        same_name(p.filename().native(), std::filesystem::path(kFolderName).native()))
        return p;
    // A drive's root is never a folder of setup's own, empty or not.
    const bool root = p == p.root_path();
    if (!root && !std::filesystem::exists(p, ec)) return p;
    if (!root && std::filesystem::is_directory(p, ec) && std::filesystem::is_empty(p, ec) && !ec) return p;
    return p / kFolderName;
}

std::string cannot_use(const std::filesystem::path& folder, const std::filesystem::path& program) {
    if (folder.empty() || !folder.is_absolute()) return "Choose a folder on this PC.";
    if (!program.empty()) {
        if (same_folder(folder, program)) return "Choose a folder of its own, not the program's folder.";
        if (inside(folder, program)) {
            const auto rel = tidy(folder).lexically_relative(tidy(program));
            if (program_name(program, *rel.begin()))
                return "This folder is part of the program, which an update replaces. Choose another folder.";
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (!std::filesystem::is_directory(folder, ec)) return "Setup cannot make this folder. Choose another one.";
    // A file of its own written in it, and removed at once.
    const auto probe = folder / ".setup-write-check";
    bool wrote = false;
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (f.is_open()) {
            f << "ok\n";
            f.flush();
            wrote = f.good();
        }
    }
    std::filesystem::remove(probe, ec);
    if (!wrote) return "Setup cannot write in this folder. Choose another one.";
    return {};
}

bool keep_choice(const std::filesystem::path& usual, const std::filesystem::path& folder,
                 const std::filesystem::path& program, std::string& why) {
    why.clear();
    std::error_code ec;
    const auto file = usual / kChoiceFile;
    if (usual.empty()) {
        why = "Setup's folder is set for this PC, so it cannot be changed here.";
        return false;
    }
    if (same_folder(folder, usual)) {  // (back to the usual folder: no choice to keep)
        std::filesystem::remove(file, ec);
        if (!std::filesystem::exists(file, ec)) return true;
        why = "Setup could not change its file in " + utf8(usual) + ".";
        return false;
    }
    std::string named = utf8(tidy(folder));
    if (!program.empty() && inside(usual, program) && inside(folder, program)) {
        const std::u8string rel = tidy(folder).lexically_relative(tidy(usual)).generic_u8string();
        named.assign(rel.begin(), rel.end());
    }
    std::filesystem::create_directories(usual, ec);
    const auto part = usual / (std::string(kChoiceFile) + ".part");
    bool wrote = false;
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        if (f.is_open()) {
            f << "# Setup builds the game in this folder, chosen on its screens (docs/88). Without this file it "
                 "builds here.\n"
              << named << "\n";
            f.flush();
            wrote = f.good();
        }
    }
    if (wrote) std::filesystem::rename(part, file, ec);
    if (!wrote || ec || !same_folder(chosen_folder(usual), folder)) {
        std::filesystem::remove(part, ec);
        why = "Setup could not save this choice in " + utf8(usual) + ".";
        return false;
    }
    return true;
}

}  // namespace ui::setup
