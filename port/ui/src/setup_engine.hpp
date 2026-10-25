// Written for Demon's Souls. The host's side of the setup engine (tools/setup; docs/81, docs/88): where setup's data
// folder is, what state it is in at start (read from setup/ready.txt, without Python), which Python runs the engine,
// and one run of it read through the line protocol (setup_protocol.hpp).
#pragma once

#include "child_process.hpp"
#include "setup_protocol.hpp"
#include "setup_python.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace ui::setup {

// The Python the engine runs on, as tools/setup/des_setup/toolchain.json pins it (tools/setup/tests/test_pipeline.py
// checks that the two agree): python.org's embeddable package on Windows; on Linux, which python.org has no such
// package for, a python-build-standalone build of the same 3.13 line (docs/87). The host fetches it before the first
// run, since there is no Python to run the engine's own download with (setup_python.hpp, docs/88 section 5). Also:
// the archive's suffix (fetch.py's archive_path) and where the program is in the unpacked folder.
#ifdef _WIN32
inline constexpr const char* kPythonName = "python-embed";
inline constexpr const char* kPythonVersion = "3.13.9";
inline constexpr const char* kPythonUrl = "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip";
inline constexpr unsigned long long kPythonSize = 10926675ull;
inline constexpr const char* kPythonSha256 = "91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a";
inline constexpr const char* kPythonSuffix = ".zip";
inline constexpr const char* kPythonProgram = "python.exe";
#else
inline constexpr const char* kPythonName = "python-linux";
inline constexpr const char* kPythonVersion = "3.13.17";
inline constexpr const char* kPythonUrl = "https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.13.17%2B20261021-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz";
inline constexpr unsigned long long kPythonSize = 35356674ull;
inline constexpr const char* kPythonSha256 = "f829adc9d888e76c8801b4799ed444188d35239fcae3a9808a99ec5cb0d8d92c";
inline constexpr const char* kPythonSuffix = ".tar.gz";
inline constexpr const char* kPythonProgram = "python/bin/python3";
#endif

// Setup's data folder (docs/81): DES_SETUP_DATA, else %LOCALAPPDATA%\DemonsSoulsRecomp, on Linux
// $XDG_DATA_HOME/DemonsSoulsRecomp (or ~/.local/share/...), as des_setup's own default; the host always passes it to
// the engine. Not the player's folder of settings, logs and reports (report::data_dir): this one holds the copied
// inputs, the lift and the build.
const std::filesystem::path& data_dir();

// What the player chose. An empty game folder: the folders the last run used (the engine keeps them), or on the RPCS3
// route the game in RPCS3's library.
struct Request {
    std::filesystem::path game;                    // the folder of the disc dump
    std::vector<std::filesystem::path> decrypted;  // more folders to look in for the decrypted files
    std::filesystem::path firmware;                // the decrypted libsre.prx, while libsre is lifted
    std::string rpcs3;                 // the RPCS3 route (docs/90): the RPCS3 that unlocks the files (setup_rpcs3.hpp's
                                       // rpcs3_arg); empty: the folder route
    std::vector<std::string> imports;  // what comes across from that RPCS3 after the build: save, trophies, settings
    bool noImport = false;             // drop an import still pending (its failure's "Go on without it")
};
// For DES_UI_OPEN=setup and setup-run (a developer's test of the screens): DES_SETUP_GAME, DES_SETUP_DECRYPTED (folders
// split by ';'), DES_SETUP_FIRMWARE, DES_SETUP_RPCS3 and DES_SETUP_IMPORT (save,trophies,settings).
Request request_from_env();

// The game's gamedata folder for this install: beside the exe, where a player's start works and keeps its saves (the
// launch hook's work_in_exe_folder). The RPCS3 route's import writes there (--gamedata).
std::filesystem::path gamedata_dir();

// The state at start, from setup/ready.txt and the journal's presence only: no Python and no hashing, so it costs
// nothing at every start (docs/88 section 3).
enum class State {
    Ready,        // the game module is built, for this version: Play
    Missing,      // nothing is set up: "Set up the game"
    Interrupted,  // a run stopped before its end, or failed: "Resume setup"
    Outdated,     // set up by another version: "Set up again", with nothing to pick
    Damaged,      // a file setup made is gone or has changed size: "Repair"
    GameMoved,    // the game folder is not where it was: pick it again, nothing to rebuild
};
struct Launch {
    State state = State::Missing;
    std::filesystem::path game, eboot, module;  // from ready.txt
};
Launch launch_state();

// The engine's Python: DES_SETUP_PYTHON (a developer's), else the downloaded one (empty until the host has fetched it:
// Engine::start fetches it first then).
std::filesystem::path python();
// tools/setup/run_setup.py: DES_SETUP_SCRIPT (a developer's, such as a source tree's, for a real build from a build
// folder elsewhere), else beside the exe in a release (the install mirrors the source tree), or up to three folders up
// in a developer's build. Empty if it is not there.
std::filesystem::path engine_script();

// One run of the engine: `run_setup.py run ...` under setup's Python, its lines read into a Status. With no Python yet
// (before the first run), the host's download of it (setup_python.hpp) comes first, as the run's first step: its lines
// go into the same Status, and the engine starts from poll() once it is in place.
class Engine {
public:
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts a run (or a check of the game files only, or on the RPCS3 route the check and the unlock only: the unlock
    // page's run). False if it could not start: why() says why, for the player.
    bool start(const Request& request, bool check_only = false, bool unlock_only = false);
    // Cancel: ends the download or the engine, and everything it started. The journal keeps what was done, and the
    // download's .part file what it got, so the next run carries on.
    void stop();
    bool running() const { return mProcess.running() || mFetch.running(); }
    const std::string& why() const { return mWhy; }
    // What the engine said since the last call, applied to `status`, then its end once it has ended (the UI thread,
    // once a frame). After the first Python's download, this starts the engine.
    void poll(Status& status);

private:
    bool start_engine(unsigned run);

    std::string mWhy;
    Request mRequest;          // this run's, for the engine's start after the download
    bool mCheckOnly = false;
    bool mUnlockOnly = false;
    bool mFetching = false;    // this run began with the first Python's download
    bool mFromEngine = false;  // the lines come from the engine now, not from the download
    bool mStopAsked = false;   // stop() came during this run
    std::mutex mLock;  // the lines and the ends, from the reading threads
    std::vector<std::string> mLines;
    bool mEnded = false, mStopped = false;
    bool mFetchEnded = false, mFetchOk = false, mFetchStopped = false;
    int mExit = 0;
    unsigned mRun = 0;  // which run the reading threads' callbacks are for
    PythonFetch mFetch;     // (destroyed before what its threads write to)
    ChildProcess mProcess;  // (last: destroyed first, so its thread has ended before what it writes to goes)
};

}  // namespace ui::setup
