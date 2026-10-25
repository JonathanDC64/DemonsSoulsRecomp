// Written for Demon's Souls. The launcher's updates (docs/105): at each start the setup engine asks GitHub for the
// newest release (tools/setup/des_setup/update.py's update-check), off the UI's thread, and the start screen, setup's
// start screen and Settings, About show what it found. The update itself (download, check, swap, then setup's
// rebuild) is update_window.hpp's. Never in a test run or a developer's build, nor with Check for updates off.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ui::updates {

enum class State {
    Off,       // no check: not possible here, or Check for updates is off
    Checking,  // the engine is asking
    Newest,    // nothing newer
    Found,     // a newer release: found()
    Failed,    // no answer (offline, or GitHub's hourly limit): said nowhere but Settings, About
};

struct Release {
    std::string version;             // "0.2.0"
    std::string page;                // its GitHub page
    long long when = 0;              // published, in Unix seconds (0: not known)
    std::vector<std::string> notes;  // its notes, Markdown, a line each
};

// The version this program is, for the update: kVersion, or DES_UPDATE_AS_VERSION (a test of the update on a
// developer's build, which is never a release).
std::string this_version();
// The folder the player's zip was unpacked in: the exe's.
std::filesystem::path install_dir();
// Updates can run here: this program is a release (X.Y.Z), it runs from a player's install (release-info.txt beside
// it, not a git checkout), the setup engine and its Python are there (setup fetches the Python at its first run), and
// this is not a test run. DES_UPDATE_FEED (a test's list of releases, update.py) allows a test run too.
bool possible();
// The check at the start: possible() and Check for updates on.
bool wanted();
// The game runs below the UI (not setup's screens alone): its Update and Go back start the program again first
// (restart_into). des_ui.cpp says which at its start.
void set_over_game(bool over);
bool over_game();
// Starts a check unless one is under way (the start, when wanted(); Check now, when possible()).
void start_check();
// The check's state, with what the engine said since the last call taken in (the UI's thread, once a frame).
State state();
const Release& found();  // State::Found: the newer release
bool skipped();          // found() is the version the player chose to skip
void skip();             // "Skip this version": no notice for it again (Settings, About still offers it)
// The version the last update kept to go back to (its folder in <install>/.previous), or empty.
std::string kept_version();
// A short line for Settings, About: "Version 0.2.0 is out.", "You have the newest version.", ...
std::string status_line();
// The game cannot replace its own files while it runs: the program starts again with setup's screens alone, open on
// `open` (update-run: the update to `version`; update-back: going back), and this one closes. False if it could not
// start the new one.
bool restart_into(const char* open, const std::string& version);

// At the start: a swap cut short (a power cut while update.py moved the program files) undone from its journal
// (<install>/.update/swap.txt), without the engine, which may be half swapped with the rest. True if it undid one: the
// install holds the version before the update again, which may not be this program, so the caller starts it again.
bool recover_swap();
// While the update moves the program files (a second or so), the window's close button does nothing (des_ui.cpp's
// WM_CLOSE): ending the program then would end the engine half way.
void hold_close(bool on);
bool close_held();

}  // namespace ui::updates
