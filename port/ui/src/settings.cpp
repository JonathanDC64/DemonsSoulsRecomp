// The port's settings (see settings.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's settings.ini handling.
#include "settings.hpp"

#include "report.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache

namespace ui::settings {
namespace {

constexpr Def kDefs[] = {
    {"DES_FPS", "", Apply::Live},              // "" = the game's own 30 fps mode; read at each flip
    {"fps_custom", "90", Apply::UiOnly},       // Frame rate's Custom number, kept while a fixed rate is chosen
    {"DES_MOTION_BLUR", "1", Apply::Live},     // des_motion_blur, read each frame
    {"DES_VSYNC", "1", Apply::Live},           // RSX_VSYNC, live
    {"DES_SKIP_INTRO", "0", Apply::Restart},   // 1 = no logos, 2 = the last logo only
    {"DES_SKIP_INTRO_MOVIE", "0", Apply::Live},  // PS3_SAIL_SKIP, read as each movie opens
    {"DES_AUTO_CONTINUE", "0", Apply::Restart},
    {"AUDIO_MUTE", "0", Apply::Live},
    {"DES_DOF", "1", Apply::Live},             // des_dof, read each frame
    {"RSX_SCALE", "100", Apply::Live},         // the D3D12 backend remakes its render targets at the next batch
    {"RSX_TEXFILTER", "0", Apply::Live},       // 0 = the game's filters; the backend's sampler key, per draw
    {"RSX_ANISO", "0", Apply::Live},           // 0 = the game's own level
    {"DES_ASPECT", "", Apply::Restart},        // "" = 16:9; des_aspect.cpp writes the cameras' aspect at start
    {"DES_HUD", "", Apply::Restart},           // "" = centred at 16:9, "stretch"; the HUD canvas is recorded at boot
    {"ui_scale", "0", Apply::UiOnly},          // 0 = Auto, else percent
    {"show_start_screen", "1", Apply::UiOnly},
    {"menu_hint", "1", Apply::UiOnly},
    {"pad_menu_chord", "0", Apply::UiOnly},    // 0 = L3 + R3, 1 = Select + Start
    {"PAD_MAP", "", Apply::Live},              // cellPad, gamepad buttons (controls_window.cpp)
    {"PAD_KEYS", "", Apply::Live},             // cellPad, the keyboard pad's keys
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

void make_dirs(const std::string& file) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(report::widen(file)).parent_path(), ec);
}

// The environment variables a setting drives (DES_VSYNC also sets RSX_VSYNC, which the D3D12 backend reads live;
// DES_SKIP_INTRO_MOVIE sets PS3_SAIL_SKIP, which cellSail reads as each movie opens).
void apply_env(std::string_view key, const std::string& value) {
    const std::string k(key);
    // (an unset option stays unset; DES_FPS "" is the game's mode, PAD_MAP and PAD_KEYS "" the game's buttons)
    if (value.empty() && key != "DES_FPS" && key != "PAD_MAP" && key != "PAD_KEYS") return;
    ps3_setenv(k.c_str(), value.c_str());
    if (key == "DES_VSYNC") ps3_setenv("RSX_VSYNC", value == "0" ? "0" : "1");
    if (key == "DES_SKIP_INTRO_MOVIE") ps3_setenv("PS3_SAIL_SKIP", value == "0" ? "" : "ds_advertise.pam");
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
    // DES_AUTO_CONTINUE counts only from the command line: from settings.ini it waits for Play instead
    // (des_ui_start_screen_up).
    const char* ac = std::getenv("DES_AUTO_CONTINUE");
    const bool auto_continue = pinned("DES_AUTO_CONTINUE") && ac && *ac && std::strcmp(ac, "0") != 0;
    return harness_run() || auto_continue || env_set("PAD_FILE") ||
           env_set("EXIT_AFTER_MS");
}

const std::string& path() {
    if (sPathResolved) return sPath;
    sPathResolved = true;
    if (const char* p = std::getenv("DES_SETTINGS"); p && *p) {
        sPath = p;
    } else if (const auto& dir = report::data_dir(); !dir.empty()) {
        sPath = report::narrow((dir / L"settings.ini").wstring());  // (empty in a test run: no settings kept)
    }
    return sPath;
}

std::string describe() {
    std::string out;
    for (const Def& d : kDefs) {
        out += std::string(d.key) + " = " + get(d.key);
        if (pinned(d.key)) out += "    # set in the environment: " + pinned_value(d.key);
        out += '\n';
    }
    return out;
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
    FILE* f = _wfopen(report::widen(file).c_str(), L"r");
    if (!f) return;
    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof line, f)) {
        const std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
        if (key == "PS3_SAIL_MOVIES") {  // the older "Intro movies" row: off skipped every movie
            if (sValues.count("DES_SKIP_INTRO_MOVIE")) continue;
            key = "DES_SKIP_INTRO_MOVIE";
            value = value == "0" ? "1" : "0";
        }
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
    FILE* f = _wfopen(report::widen(tmp).c_str(), L"w");
    if (!f) return;
    std::fprintf(f,
                 "# Demon's Souls PC port settings (key = value), written by the in-game menu.\n"
                 "# The option keys are the environment variables of the same name; a variable set in the\n"
                 "# environment wins over this file.\n");
    for (const Def& d : kDefs) std::fprintf(f, "%s = %s\n", d.key, get(d.key).c_str());
    std::fclose(f);
    MoveFileExW(report::widen(tmp).c_str(), report::widen(file).c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace ui::settings
