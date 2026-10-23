// Problem reports (docs/32): the data folder, the log, and what a bug report needs (version, system, settings, the
// recent log, a screenshot or a crash trace), zipped for attaching to a GitHub issue, with the issue form opened and
// its text filled in. Used by the crash dialog (crash_report.cpp) and the menu's "Report an issue". Adapted for
// Demon's Souls from FFCCEOT-Recomp's report.hpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace ui::report {

constexpr const char* kRepositoryUrl = "https://github.com/JonathanDC64/DemonsSoulsRecomp";

struct File {
    std::string name;  // in the zip
    std::vector<uint8_t> data;
};

// The folder of this player's data (settings.ini, logs, crashes, reports): DES_DATA_DIR, else <exe dir>\data when
// portable.txt is next to the exe, else %APPDATA%\DemonsSoulsRecomp\DemonsSouls. Empty for a test harness run
// (tools/des scripts other than play.sh) without DES_DATA_DIR: those keep no settings, log or reports. The game's
// saves are not here: they stay in the PS3 file system (gamedata/dev_hdd0).
const std::filesystem::path& data_dir();
void set_data_dir(const std::filesystem::path& dir);  // (the crash dialog process: the crashed game's)
// The folder of the running exe, and the exe itself; empty if it cannot be told.
std::filesystem::path program_dir();
std::filesystem::path program_path();

// The port's version (version.hpp): "0.1.0", or "0.1.0-dev.<commits>+g<commit>" between releases.
std::string version();
// The port's version, the ps3recomp and the game dump it was built with, then the operating system, processor,
// memory (the machine's and this process's), graphics adapters (the one in use, its memory budget), displays,
// power, free disk space, gamepads, the keyboard layout and the other software loaded into the game (overlays and
// the like), one fact per line.
std::string system_info();
// The environment variables of the port's and the runtime's own options (DES_, PS3_, RSX_, SPU_ and the like) that
// are set, and the command line (this process's, or `command_line`): what a run was started with, beyond settings.ini.
// The online name and password show only as set.
std::string environment_info(const std::string& command_line = {});
// Hides the user's name, profile folder, computer name, PSN name, e-mail addresses and the folders of this PC from
// text that is going to be posted.
std::string redact(std::string text);

// Sends this process's standard output and error to logs\des.log in the data folder as well as where they went (the
// previous run's log is kept as des.prev.log). Called once, before main() (crash_report.cpp). Nothing without a data
// folder.
void start_log();
std::filesystem::path log_path();
// Waits (briefly) until what was printed so far is in the log file (a crash, before reading the log).
void flush_log();
// The first `head` lines and the last `tail_bytes` of a text file, with a note where text was left out.
std::string read_head_tail(const std::filesystem::path& path, size_t head, size_t tail_bytes);
std::vector<uint8_t> read_file(const std::filesystem::path& path);
inline std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Writes `files` to a new zip in the data folder's reports folder, named `<stem>.zip`; the path, or empty on failure.
std::filesystem::path write_bundle(const std::string& stem, const std::vector<File>& files);
std::string timestamp();  // "20261017-153012", for file names

// The new-issue page of the repository with the title and text filled in (cut to what a link can hold).
std::string issue_url(const std::string& title, const std::string& body);
// Opens the issue form in the browser and shows `zip` in Explorer. With DES_REPORT_NO_OPEN=1 (tests) neither is
// opened: the link is written to <zip>.url.txt instead.
void open_issue(const std::string& url, const std::filesystem::path& zip);
void open_url(const std::string& url);
void reveal(const std::filesystem::path& path);

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// A file, as fopen (on Windows through _wfopen, so that any name opens), and a rename over an existing file.
FILE* open_file(const std::filesystem::path& path, const char* mode);
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to);

// The test knobs read at each presented frame (the overlay's frame hook): DES_CRASH_AT_FRAME=n crashes (an access
// violation) and DES_ABORT_AT_FRAME=n aborts at frame n, DES_HANG_RENDER_AT_FRAME=n stops the render thread for good
// there (the hang dialog's case); true at DES_REPORT_AT_FRAME=n (start an issue report). The last two are read again
// every 64 frames, so the debug console's setenv can set them in a running game (DES_HANG_RENDER_AT_FRAME=1: now).
bool frame_hook(unsigned long long frame);
// The test knob read at each of the game's flip requests (des_options.cpp, on the game's thread): with
// DES_HANG_AT_FRAME=n that thread stops for good once n frames were presented, as a hang of the game itself would
// (the render thread goes on, so F1 opens the menu over the last frame).
void flip_hook();

// "Report an issue", a few frames after the menu closed: takes the game's picture from the back buffer (render
// thread, inside the frame hook), then zips the report on a thread of its own and opens the issue form. `session` is
// what des_ui.cpp says about this run (uptime, frames, frame rate, window). The zip holds:
//   screenshot.png  the game's picture (the menu closed)
//   system.txt      system_info()
//   game.txt        `session`, and the crash and hang reports of the last week on this PC (names only)
//   settings.ini    every setting, with its default where it differs, and those set in the environment
//   environment.txt environment_info()
//   runtime.txt     the runtime's snapshot: what every thread is doing (ps3_diag_report, 2 s of sampling)
//   des.log         the log's start and its last 512 KB; des.prev.log the end of the previous run's (a freeze or
//                   crash the player restarted after), when it is from the last day
// Nothing of the game's files, saves or memory: names, numbers, settings and the log only.
void send_issue_report(void* back_buffer, unsigned index, int width, int height, std::string session);

}  // namespace ui::report
