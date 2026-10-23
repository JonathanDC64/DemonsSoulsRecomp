// The RPCS3 route's part in the host (see setup_rpcs3.hpp). One function for each platform says what RPCS3's program
// is called, where it keeps its settings and where to look for it; checking a folder and reading the game list's line
// are the same everywhere.
#include "setup_rpcs3.hpp"

#ifdef _WIN32
#include <windows.h>
// (after windows.h)
#include <psapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace ui::setup {
namespace {

constexpr const char* kTitle = "BLUS30443";  // the one title the port supports (setup_pc.hpp's kTitleId)
constexpr size_t kMaxEntries = 400;           // folders looked at in one folder, at most (as setup_pc.cpp's scan)

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool is_dir(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_directory(p, ec);
}

bool is_file(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

// The folders in `dir`, at most kMaxEntries, not hidden ones nor Windows' own ($Recycle.Bin).
std::vector<std::filesystem::path> folders_in(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    size_t entries = 0;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end && entries < kMaxEntries; it.increment(ec), ++entries) {
        std::error_code dirEc;
        const std::string name = utf8(it->path().filename());
        if (it->is_directory(dirEc) && !name.empty() && name[0] != '.' && name[0] != '$') out.push_back(it->path());
    }
    return out;
}

// ---- What differs by platform ----

#ifdef _WIN32

// RPCS3's program in `folder`, or empty.
std::filesystem::path program_in(const std::filesystem::path& folder) {
    const auto exe = folder / "rpcs3.exe";
    return is_file(exe) ? exe : std::filesystem::path{};
}

bool is_program_name(const std::filesystem::path& p) {
    return lower(utf8(p.filename())) == "rpcs3.exe";
}

// A folder RPCS3 has run in: it made one of these at its first start (or the player made portable/).
bool has_run(const std::filesystem::path& folder) {
    return is_dir(folder / "config") || is_dir(folder / "portable") || is_dir(folder / "dev_hdd0");
}

// Where RPCS3 keeps its settings (File.cpp get_config_dir; des_setup/rpcs3.py player_dirs): portable/ beside the
// program first, then RPCS3_CONFIG_DIR cut after its last separator, then the program's folder.
std::filesystem::path config_dir(const std::filesystem::path& program) {
    const auto home = program.parent_path();
    if (is_dir(home / "portable")) return home / "portable";
    std::string v = env("RPCS3_CONFIG_DIR");
    std::replace(v.begin(), v.end(), '\\', '/');
    if (const size_t cut = v.rfind('/'); cut != std::string::npos) return std::filesystem::path(v.substr(0, cut + 1));
    return home;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// The programs that the shortcuts in `dir` start (and in its folders, `depth` levels down), kept when one is RPCS3.
void shortcut_targets(const std::filesystem::path& dir, int depth, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    size_t entries = 0;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end && entries < kMaxEntries; it.increment(ec), ++entries) {
        std::error_code dirEc;
        if (it->is_directory(dirEc)) {
            if (depth > 0) shortcut_targets(it->path(), depth - 1, out);
            continue;
        }
        if (lower(utf8(it->path().extension())) != ".lnk") continue;
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
        IPersistFile* file = nullptr;
        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (FAILED(file->Load(it->path().c_str(), STGM_READ)) || FAILED(link->GetPath(target, MAX_PATH, nullptr, 0)))
                target[0] = 0;
            file->Release();
        }
        link->Release();
        if (target[0] && is_program_name(target)) out.emplace_back(target);
    }
}

// The running RPCS3s' programs, by each process's image path (no process is opened for more than that).
void running_programs(std::vector<std::filesystem::path>& out) {
    std::vector<DWORD> pids(4096);
    DWORD got = 0;
    if (!EnumProcesses(pids.data(), DWORD(pids.size() * sizeof(DWORD)), &got)) return;
    for (DWORD i = 0; i < got / sizeof(DWORD); ++i) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!h) continue;
        wchar_t image[MAX_PATH * 4];
        DWORD size = DWORD(std::size(image));
        if (QueryFullProcessImageNameW(h, 0, image, &size) && is_program_name(image)) out.emplace_back(image);
        CloseHandle(h);
    }
}

// Windows' own record of the programs started on this PC, read only, for an rpcs3.exe in it: the shell's MuiCache (a
// value named "<program>.FriendlyAppName" for each program it has shown) and the Program Compatibility Assistant's
// store (a value named after each program started). RPCS3 has no installer and writes no registry key of its own
// (docs/90 3.6), so this is how setup finds one started from a folder it does not look in.
void recorded_programs(std::vector<std::filesystem::path>& out) {
    struct Record {
        const wchar_t* key;
        const wchar_t* tail;  // what follows the program's path in a value's name
    };
    constexpr Record kRecords[] = {
        {L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", L".FriendlyAppName"},
        {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store", L""},
    };
    std::vector<wchar_t> name(32768);
    for (const Record& r : kRecords) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, r.key, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        const std::wstring tail = r.tail;
        for (DWORD i = 0;; ++i) {
            DWORD n = DWORD(name.size());
            const LONG rc = RegEnumValueW(key, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;
            std::wstring v(name.data(), n);
            if (!tail.empty()) {
                if (v.size() <= tail.size() || v.compare(v.size() - tail.size(), tail.size(), tail) != 0) continue;
                v.resize(v.size() - tail.size());
            }
            if (is_program_name(v)) out.emplace_back(v);
        }
        RegCloseKey(key);
    }
}

// The usual folders, two levels deep: Desktop, Documents and Downloads, Scoop's RPCS3, and each fixed drive's root.
void usual_programs(std::vector<std::filesystem::path>& out) {
    std::vector<std::filesystem::path> roots = {known_folder(FOLDERID_Desktop), known_folder(FOLDERID_Documents),
                                                known_folder(FOLDERID_Downloads)};
    if (const auto profile = known_folder(FOLDERID_Profile); !profile.empty())
        out.push_back(profile / "scoop" / "apps" / "rpcs3" / "current" / "rpcs3.exe");
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        const wchar_t root[] = {wchar_t(L'A' + d), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) roots.emplace_back(root);
    }
    for (const auto& root : roots) {
        if (root.empty()) continue;
        out.push_back(root / "rpcs3.exe");
        for (const auto& one : folders_in(root)) {
            out.push_back(one / "rpcs3.exe");
            for (const auto& two : folders_in(one)) out.push_back(two / "rpcs3.exe");
        }
    }
}

// In the order find_rpcs3 tries them.
std::vector<std::filesystem::path> platform_programs() {
    std::vector<std::filesystem::path> out;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // (this thread's own, for the shortcuts)
    const std::filesystem::path pinned =
        known_folder(FOLDERID_RoamingAppData) / "Microsoft" / "Internet Explorer" / "Quick Launch" / "User Pinned";
    for (const auto& dir : {known_folder(FOLDERID_Desktop), known_folder(FOLDERID_PublicDesktop)})
        if (!dir.empty()) shortcut_targets(dir, 0, out);
    for (const auto& dir : {known_folder(FOLDERID_Programs), known_folder(FOLDERID_CommonPrograms), pinned})
        if (!dir.empty()) shortcut_targets(dir, 1, out);
    if (SUCCEEDED(com)) CoUninitialize();
    running_programs(out);
    recorded_programs(out);
    usual_programs(out);
    return out;
}

const char* kLookedIn =
    "RPCS3's shortcuts, a running RPCS3, the programs Windows has started, Desktop, Documents and Downloads, and two "
    "folders deep on each drive";

#else

// RPCS3's program in `folder`: rpcs3, or an rpcs3*.AppImage. Empty if none.
std::filesystem::path program_in(const std::filesystem::path& folder) {
    if (is_file(folder / "rpcs3")) return folder / "rpcs3";
    std::error_code ec;
    size_t entries = 0;
    for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end && entries < kMaxEntries; it.increment(ec), ++entries) {
        const std::string name = utf8(it->path().filename());
        if (lower(name).rfind("rpcs3", 0) == 0 && name.size() > 9 && name.substr(name.size() - 9) == ".AppImage" &&
            is_file(it->path()))
            return it->path();
    }
    return {};
}

bool is_program_name(const std::filesystem::path& p) {
    const std::string name = utf8(p.filename());
    return name == "rpcs3" || (lower(name).rfind("rpcs3", 0) == 0 && name.size() > 9 &&
                               name.substr(name.size() - 9) == ".AppImage");
}

std::filesystem::path home_dir() {
    return env("HOME");
}

// Where RPCS3 keeps its settings: portable/ beside the program (an AppImage's folder) first, else the XDG config
// folder's rpcs3/ (File.cpp get_config_dir).
std::filesystem::path config_dir(const std::filesystem::path& program) {
    if (is_dir(program.parent_path() / "portable")) return program.parent_path() / "portable";
    const std::string xdg = env("XDG_CONFIG_HOME");
    return (xdg.empty() ? home_dir() / ".config" : std::filesystem::path(xdg)) / "rpcs3";
}

// RPCS3 has run as this user: its settings folder is there.
bool has_run(const std::filesystem::path& folder) {
    const auto program = program_in(folder);
    return !program.empty() && is_dir(config_dir(program));
}

// rpcs3 on PATH, then AppImages in the usual folders. (Flatpak's RPCS3 is not offered yet: the engine has no
// flatpak: route, docs/90 3.7.)
std::vector<std::filesystem::path> platform_programs() {
    std::vector<std::filesystem::path> out;
    const std::string path = env("PATH");
    for (size_t from = 0; from <= path.size();) {
        size_t colon = path.find(':', from);
        if (colon == std::string::npos) colon = path.size();
        if (colon > from) out.push_back(std::filesystem::path(path.substr(from, colon - from)) / "rpcs3");
        from = colon + 1;
    }
    const auto home = home_dir();
    for (const char* dir : {"Applications", "Downloads", "Desktop", ".local/bin"})
        if (const auto p = program_in(home / dir); !p.empty()) out.push_back(p);
    return out;
}

const char* kLookedIn = "the installed programs, and AppImages in Applications, Downloads, Desktop and .local/bin";

#endif

// ---- The same everywhere ----

// A YAML scalar as RPCS3's game list writes one: plain, 'single-quoted' ('' for a quote) or "double-quoted".
std::string yaml_scalar(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    size_t at = v.find_first_not_of(" \t");
    v = at == std::string::npos ? std::string() : v.substr(at);
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            out += v[i];
            if (v[i] == '\'' && v[i + 1] == '\'') ++i;
        }
        return out;
    }
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] == '\\' && i + 2 < v.size()) ++i;
            out += v[i];
        }
        return out;
    }
    return v;
}

// The BLUS30443 line of RPCS3's game list: config/games.yml (or games.yml, in builds before 2023) in its settings
// folder, "BLUS30443: <path>". The path is the disc copy's folder, or its PS3_GAME folder written with "/./" at the
// end, or an ISO file.
void read_library(Rpcs3& r) {
    const auto cfg = config_dir(r.program);
    for (const auto& list : {cfg / "config" / "games.yml", cfg / "games.yml"}) {
        std::ifstream in(list, std::ios::binary);
        if (!in) continue;
        const std::string head = std::string(kTitle) + ":";
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, head.size(), head) != 0) continue;
            std::string v = yaml_scalar(line.substr(head.size()));
            if (v.empty()) return;
            if (lower(v).size() > 4 && lower(v).substr(v.size() - 4) == ".iso") {
                r.library = Rpcs3::Library::Iso;
                r.game = from_utf8(v);
                return;
            }
            std::replace(v.begin(), v.end(), '\\', '/');
            while (v.size() > 1 && (v.back() == '/' || (v.size() >= 2 && v.compare(v.size() - 2, 2, "/.") == 0)))
                v.pop_back();
            std::filesystem::path game = from_utf8(v);
            if (lower(utf8(game.filename())) == "ps3_game") game = game.parent_path();
            r.library = Rpcs3::Library::Folder;
            r.game = game;
            return;
        }
        return;  // (the first list found is the one RPCS3 reads)
    }
}

// `program` as an RPCS3, if it is one. `ranOnly`: only one that has run (the searches; the player's pick need not).
Rpcs3 check(const std::filesystem::path& program, bool ranOnly) {
    Rpcs3 r;
    if (!is_file(program) || !is_program_name(program)) return r;
    const auto folder = program.parent_path();
    if (ranOnly && !has_run(folder)) return r;
    r.folder = folder;
    r.program = program;
    r.portable = is_dir(folder / "portable");
    read_library(r);
    return r;
}

std::filesystem::path program_for(const std::filesystem::path& path) {
    return is_dir(path) ? program_in(path) : path;
}

}  // namespace

Rpcs3 find_rpcs3(const std::filesystem::path& remembered) {
    std::vector<std::filesystem::path> programs;
    if (const std::string dev = env("RPCS3_DIR"); !dev.empty()) programs.push_back(program_for(dev));
    if (!remembered.empty()) programs.push_back(program_for(remembered));
    const size_t named = programs.size();  // (those two count even if RPCS3 has not run there yet)
    for (auto& p : platform_programs()) programs.push_back(std::move(p));
    Rpcs3 first;
    std::set<std::string> seen;
    for (size_t i = 0; i < programs.size(); ++i) {
        if (programs[i].empty() || !seen.insert(lower(utf8(programs[i].lexically_normal()))).second) continue;
        Rpcs3 r = check(programs[i], i >= named);
        if (!r.found()) continue;
        if (r.library != Rpcs3::Library::None) return r;
        if (!first.found()) first = r;
    }
    return first;
}

Rpcs3 rpcs3_at(const std::filesystem::path& path) {
    return check(program_for(path), false);
}

std::string rpcs3_looked_in() {
    return kLookedIn;
}

std::string rpcs3_arg(const Rpcs3& r) {
    return utf8(r.program);
}

}  // namespace ui::setup
