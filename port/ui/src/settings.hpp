// Written for Demon's Souls (docs/32).
// The port's settings, as the UI edits them: settings.ini (key = value), keyed by the options' own environment
// variable names (DES_FPS, DES_VSYNC, ...) plus a few of the UI's own (ui_scale, show_start_screen, ...).
//
// At startup a value from settings.ini becomes its environment variable only where that variable is not set already:
// an explicit environment variable always wins (so every test script keeps its options), and the UI shows such a row
// as set from outside. Test harness runs (tools/des scripts other than play.sh) neither read nor write the file
// unless DES_SETTINGS names one.
#pragma once

#include <string>
#include <string_view>

namespace ui::settings {

enum class Apply {
    Live,     // applied at once (a live knob)
    Restart,  // read when the game starts
    UiOnly,   // the UI's own
    Session,  // live, but never read from or written to settings.ini: each start begins at the default (the
              // developer build's cheats, docs/55)
};

struct Def {
    const char* key;
    const char* def;  // default value ("" = unset)
    Apply apply;
};

// The file's location (UTF-8): DES_SETTINGS, else settings.ini in the data folder (report::data_dir(): DES_DATA_DIR,
// <exe dir>\data when portable.txt is there, else %APPDATA%\DemonsSoulsRecomp\DemonsSouls). Empty when the settings
// are not kept (a test harness run).
const std::string& path();

// The settings in use, as settings.ini lines, with the values the environment set noted (for an issue report).
std::string describe();

// Reads settings.ini and sets the environment variables it holds that are not set yet. Once, at startup, before the
// game's options are read.
void load_and_apply_env();

const Def* find(std::string_view key);
std::string get(std::string_view key);  // the value, or its default
int get_int(std::string_view key);
bool is_default(std::string_view key);

// Stores the value, applies it if it is live or per session (and not pinned) and saves settings.ini (where a
// per-session value is never written).
void set(std::string_view key, const std::string& value);

// The variable was set in the environment when the game started (not by settings.ini): it wins over the file.
bool pinned(std::string_view key);
std::string pinned_value(std::string_view key);

// True for a test harness run (PS3_PIDFILE set by tools/des, and not a play.sh session) and for any run whose input
// is scripted (DES_AUTO_CONTINUE, PAD_FILE, EXIT_AFTER_MS): no start screen there.
bool harness_run();
bool scripted_run();

void save();

}  // namespace ui::settings
