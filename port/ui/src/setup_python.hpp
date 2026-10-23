// Written for Demon's Souls. The first Python (docs/88 section 5). On Windows the setup engine runs on python.org's
// embeddable Python, which no PC has at first, and there is no Python yet to run the engine's own download with. So the
// host fetches it itself before the engine's first run: Windows' own curl.exe and tar.exe (System32, run by full path
// through the process runner, so Cancel ends them and they get no window), checked with the host's own SHA-256
// (sha256.hpp). It lands where des_setup's fetch.py puts it, with the same stamp, so from then on the host's lookup
// (setup_engine.hpp python()) and the engine's fetch both see it as unpacked. Elsewhere the engine runs on the system's
// python3: no first download.
#pragma once

#include "child_process.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui::setup {

// The step's name in the run's plan, for the setup screen's task list. Its count is "@progress download", as the fetch
// phase's is.
inline constexpr const char* kPythonStep = "python";
// Its share of the whole run's bar, per mille: 1%, about its share of a run's time (inferred: its 11 MB against the
// compiler's 69 MB, which the fetch phase's weight of 4 in 100 covers). The engine's own bar fills the rest.
inline constexpr int kPythonShare = 10;
// The engine's own free-space figure for its fetch phase (pipeline.py NEED["fetch"], 0.5 GB), less what the downloads
// and tools folders already hold, as the engine counts it, so its check after this one asks for the same.
inline constexpr unsigned long long kFetchBytes = 536870912ull;
// fetch.py's names: the archive in <data>/downloads (archive_path), unpacked in <data>/tools/<name>-<version> with the
// stamp file that names the archive's SHA-256 (ensure, unpacked). tools/setup/tests/test_pipeline.py checks all four.
inline constexpr const char* kDownloadsFolder = "downloads";
inline constexpr const char* kToolsFolder = "tools";
inline constexpr const char* kStampName = ".des-setup-sha256";

// <data>/downloads/python-embed-3.13.9.zip
std::filesystem::path python_archive();
// <data>/tools/python-embed-3.13.9
std::filesystem::path python_folder();
// Its stamp names the pinned SHA-256 (fetch.unpacked's test).
bool python_unpacked();

// One try at the first Python, on a thread of its own: room for it, the download (resumed from the .part file a
// stopped try left), its size and SHA-256, the archive's list of names, the unpacking and the stamp. It speaks the
// engine's protocol (setup_protocol.hpp), so the screens draw it as a step of the run: "@plan python", the step's
// start and end, "@progress download n/m" and "@overall" (the .part file's size, read every quarter second), and a
// failure as "@error download|disk|internal python <sentence>" then "@done failed". The tools' own lines are plain
// ones, for Details.
class PythonFetch {
public:
    using LineFn = std::function<void(std::string line)>;
    using EndFn = std::function<void(bool ok, bool stopped)>;  // ok: Python is in place; stopped: stop() ended it

    PythonFetch() = default;
    ~PythonFetch();  // stop(), then waits for its thread
    PythonFetch(const PythonFetch&) = delete;
    PythonFetch& operator=(const PythonFetch&) = delete;

    // Starts a try (after ending the last one). Both callbacks come on its threads.
    void start(LineFn on_line, EndFn on_end);
    // Cancel: ends curl or tar at once (any thread). The .part file stays, so the next try carries on from it.
    void stop();
    bool running() const { return mRunning.load(); }

private:
    enum class Result { Ok, Failed, Stopped };
    struct Ran {
        bool started = false;
        int status = 0;
    };

    Result steps();
    Result download(const std::filesystem::path& part);
    Result unpack();
    Ran run_tool(const char* name, const std::vector<std::string>& args, std::vector<std::string>* listing,
                 const std::function<void()>& each);
    bool rename_into_place(const std::filesystem::path& from, const std::filesystem::path& to);
    Result fail(const char* code, const std::string& sentence);
    void progress(unsigned long long done);
    void say(std::string line);
    bool stopping() const { return mStop.load(); }

    LineFn mOnLine;
    int mShownPercent = -1;
    std::string mToolWhy;  // why the last tool did not start

    std::mutex mLock;  // the tool's end and its listing, from its reading thread; the waits
    std::condition_variable mWake;
    bool mToolEnded = false;
    int mToolStatus = 0;
    std::vector<std::string> mListing;
    std::atomic<bool> mStop{false};
    std::atomic<bool> mRunning{false};
    std::thread mThread;
    ChildProcess mTool;  // curl or tar, one at a time (last: destroyed first, with its reading thread)
};

}  // namespace ui::setup
