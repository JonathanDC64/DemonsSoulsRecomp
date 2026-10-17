// The port's settings (see settings.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's settings.ini handling.
#include "settings.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache

namespace ui::settings {
namespace {

constexpr Def kDefs[] = {
    {"DES_FPS", "", Apply::Restart},           // "" = the game's own 30 fps mode
    {"DES_VSYNC", "1", Apply::Live},           // RSX_VSYNC, live
    {"DES_SKIP_INTRO", "0", Apply::Restart},
    {"PS3_SAIL_MOVIES", "1", Apply::Live},     // read as each movie opens
    {"DES_AUTO_CONTINUE", "0", Apply::Restart},
    {"AUDIO_MUTE", "0", Apply::Live},
    {"ui_scale", "0", Apply::UiOnly},          // 0 = Auto, else percent
    {"show_start_screen", "1", Apply::UiOnly},
    {"menu_hint", "1", Apply::UiOnly},
    {"pad_menu_chord", "0", Apply::UiOnly},    // 0 = L3 + R3, 1 = Select + Start
};

std::map<std::string, std::string, std::less<>> sValues;
std::set<std::string, std::less<>> sPinned;
std::map<std::string, std::string, std::less<>> sPinnedValues;
std::string sPath;
bool sPathResolved = false;

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string exe_dir() {
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, DWORD(sizeof buf));
    if (n == 0 || n >= sizeof buf) return {};
    std::string p(buf, n);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

bool file_exists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// %APPDATA%\DemonsSoulsRecomp\DemonsSouls (what SDL_GetPrefPath("DemonsSoulsRecomp", "DemonsSouls") gives).
std::string pref_dir() {
    PWSTR roaming = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        char buf[MAX_PATH * 3];
        const int n = WideCharToMultiByte(CP_UTF8, 0, roaming, -1, buf, int(sizeof buf), nullptr, nullptr);
        if (n > 0) out = std::string(buf) + "\\DemonsSoulsRecomp\\DemonsSouls";
    }
    CoTaskMemFree(roaming);
    return out;
}

void make_dirs(const std::string& file) {
    for (size_t i = 3; i < file.size(); i++) {
        if (file[i] == '\\' || file[i] == '/') CreateDirectoryA(file.substr(0, i).c_str(), nullptr);
    }
}

// The environment variables a setting drives (DES_VSYNC also sets RSX_VSYNC, which the D3D12 backend reads live).
void apply_env(std::string_view key, const std::string& value) {
    const std::string k(key);
    if (value.empty()) return;  // (an unset option stays unset)
    ps3_setenv(k.c_str(), value.c_str());
    if (key == "DES_VSYNC") ps3_setenv("RSX_VSYNC", value == "0" ? "0" : "1");
}

}  // namespace

const Def* find(std::string_view key) {
    for (const Def& d : kDefs) {
        if (key == d.key) return &d;
    }
    return nullptr;
}

bool harness_run() {
    const char* kind = std::getenv("DES_KIND");
    return env_set("PS3_PIDFILE") && !(kind && std::strcmp(kind, "play") == 0);
}

bool scripted_run() {
    const char* ac = std::getenv("DES_AUTO_CONTINUE");
    return harness_run() || (ac && *ac && std::strcmp(ac, "0") != 0) || env_set("PAD_FILE") ||
           env_set("EXIT_AFTER_MS");
}

const std::string& path() {
    if (sPathResolved) return sPath;
    sPathResolved = true;
    if (const char* p = std::getenv("DES_SETTINGS"); p && *p) {
        sPath = p;
    } else if (harness_run()) {
        sPath.clear();  // test runs keep their options to themselves
    } else if (const std::string dir = exe_dir(); !dir.empty() && file_exists(dir + "\\portable.txt")) {
        sPath = dir + "\\settings.ini";
    } else if (const std::string dir = pref_dir(); !dir.empty()) {
        sPath = dir + "\\settings.ini";
    }
    return sPath;
}

void load_and_apply_env() {
    for (const Def& d : kDefs) {
        if (d.apply == Apply::UiOnly) continue;
        if (env_set(d.key) || (std::strcmp(d.key, "DES_VSYNC") == 0 && env_set("RSX_VSYNC"))) {
            sPinned.insert(d.key);
            const char* v = std::getenv(d.key);
            if (!(v && *v)) v = std::getenv("RSX_VSYNC");  // DES_VSYNC pinned through RSX_VSYNC
            sPinnedValues[d.key] = v ? v : "";
        }
    }
    const std::string& file = path();
    if (file.empty()) return;
    FILE* f = std::fopen(file.c_str(), "r");
    if (!f) return;
    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof line, f)) {
        const std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
        const Def* d = find(key);
        if (!d) continue;
        sValues[key] = value;
        if (d->apply != Apply::UiOnly && !sPinned.count(key) && !value.empty()) {
            apply_env(key, value);
            applied++;
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "[ui] settings: %s (%d option%s applied)\n", file.c_str(), applied, applied == 1 ? "" : "s");
}

std::string get(std::string_view key) {
    if (const auto it = sValues.find(key); it != sValues.end()) return it->second;
    const Def* d = find(key);
    return d ? d->def : "";
}

int get_int(std::string_view key) {
    return std::atoi(get(key).c_str());
}

bool is_default(std::string_view key) {
    const Def* d = find(key);
    return d && get(key) == d->def;
}

void set(std::string_view key, const std::string& value) {
    const Def* d = find(key);
    if (!d) return;
    sValues[std::string(key)] = value;
    if (d->apply == Apply::Live && !pinned(key)) apply_env(key, value.empty() ? d->def : value);
    save();
}

bool pinned(std::string_view key) {
    return sPinned.count(key) != 0;
}

std::string pinned_value(std::string_view key) {
    const auto it = sPinnedValues.find(key);
    return it == sPinnedValues.end() ? std::string() : it->second;
}

void save() {
    const std::string& file = path();
    if (file.empty()) return;
    make_dirs(file);
    const std::string tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f,
                 "# Demon's Souls PC port settings (key = value), written by the in-game menu.\n"
                 "# The option keys are the environment variables of the same name; a variable set in the\n"
                 "# environment wins over this file.\n");
    for (const Def& d : kDefs) std::fprintf(f, "%s = %s\n", d.key, get(d.key).c_str());
    std::fclose(f);
    MoveFileExA(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace ui::settings
