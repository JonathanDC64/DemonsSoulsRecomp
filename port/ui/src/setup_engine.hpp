// Written for Demon's Souls. The host's side of the setup engine (tools/setup; docs/81, docs/88): where setup's data
// folder is, what state it is in at start (read from setup/ready.txt, without Python), which Python runs the engine,
// and one run of it read through the line protocol (setup_protocol.hpp).
#pragma once

#include "child_process.hpp"
#include "setup_protocol.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace ui::setup {

// The embeddable Python the engine runs on, on Windows, as tools/setup/des_setup/toolchain.json pins it
// (tools/setup/tests/test_pipeline.py checks that the two agree). The host fetches it before the first run, since
// there is no Python to run the engine's own download with (docs/88 section 5).
inline constexpr const char* kPythonName = "python-embed";
inline constexpr const char* kPythonVersion = "3.13.9";
inline constexpr const char* kPythonUrl = "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip";
inline constexpr unsigned long long kPythonSize = 10926675ull;
inline constexpr const char* kPythonSha256 = "91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a";

// Setup's data folder (docs/81): DES_SETUP_DATA, else %LOCALAPPDATA%\DemonsSoulsRecomp, on Linux
// $XDG_DATA_HOME/DemonsSoulsRecomp (or ~/.local/share/...), as des_setup's own default; the host always passes it to
// the engine. Not the player's folder of settings, logs and reports (report::data_dir): this one holds the copied
// inputs, the lift and the build.
const std::filesystem::path& data_dir();

// What the player chose. An empty game folder: the folders the last run used (the engine keeps them).
struct Request {
    std::filesystem::path game;                    // the folder of the disc dump
    std::vector<std::filesystem::path> decrypted;  // more folders to look in for the decrypted files
    std::filesystem::path firmware;                // the decrypted libsre.prx, while libsre is lifted
};
// For DES_UI_OPEN=setup and setup-run (a developer's test of the screens): DES_SETUP_GAME, DES_SETUP_DECRYPTED (folders
// split by ';') and DES_SETUP_FIRMWARE.
Request request_from_env();

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

// The engine's Python: DES_SETUP_PYTHON (a developer's), else on Windows the downloaded one (empty until the host has
// fetched it), elsewhere python3 from PATH.
std::filesystem::path python();
// tools/setup/run_setup.py: beside the exe in a release (the install mirrors the source tree), or up to three folders
// up in a developer's build. Empty if it is not there.
std::filesystem::path engine_script();

// One run of the engine: `run_setup.py run ...` under setup's Python, its lines read into a Status.
class Engine {
public:
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts a run (or a check of the game files only). False if it could not start: why() says why, for the player.
    bool start(const Request& request, bool check_only = false);
    // Cancel: ends the engine and everything it started. The journal keeps what was done, so the next run resumes.
    void stop() { mProcess.stop(); }
    bool running() const { return mProcess.running(); }
    const std::string& why() const { return mWhy; }
    // What the engine said since the last call, applied to `status`, then its end once it has ended (the UI thread,
    // once a frame).
    void poll(Status& status);

private:
    std::string mWhy;
    std::mutex mLock;  // the lines and the end, from the reading thread
    std::vector<std::string> mLines;
    bool mEnded = false, mStopped = false;
    int mExit = 0;
    unsigned mRun = 0;  // which run the reading thread's callbacks are for
    ChildProcess mProcess;  // (last: destroyed first, so its thread has ended before what it writes to goes)
};

}  // namespace ui::setup
