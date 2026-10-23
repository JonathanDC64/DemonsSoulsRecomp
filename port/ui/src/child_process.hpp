// Written for Demon's Souls. A child process whose output is read line by line on a thread of its own: the setup
// engine (docs/88). The same code everywhere, except one small function for each thing the platforms do differently:
// start the process with its output on a pipe, read the pipe, stop it with everything it started, and wait for it
// (CreateProcessW in a job object on Windows, posix_spawn in a process group of its own elsewhere).
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui {

class ChildProcess {
public:
    // Each line of its output (standard output and error together), without the line break; then, once the pipe has
    // closed and the process has ended, its exit status and whether stop() ended it. Both on the reading thread.
    using LineFn = std::function<void(std::string line)>;
    using ExitFn = std::function<void(int status, bool stopped)>;

    ChildProcess();
    ~ChildProcess();  // stop(), then waits for the reading thread
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `program` (a path, or a name looked up in PATH) with `args` (UTF-8): standard input empty, no window of
    // its own, in `folder` when one is given (Windows only so far: the first Python's tools, setup_python.hpp; nothing
    // elsewhere passes one). False if it could not start: why() says why.
    bool start(const std::filesystem::path& program, const std::vector<std::string>& args, LineFn on_line,
               ExitFn on_exit, const std::filesystem::path& folder = {});
    // Ends it and every process it started, at once (any thread). The reading thread then sees the pipe close.
    void stop();
    bool running() const { return mRunning.load(); }
    const std::string& why() const { return mWhy; }

    struct Os;  // what each platform keeps of a running process (child_process.cpp)

private:
    void read_loop(LineFn on_line, ExitFn on_exit);

    std::unique_ptr<Os> mOs;
    std::thread mThread;
    std::mutex mLock;  // stop() against the reading thread closing the process
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mStopped{false};
    std::string mWhy;
};

}  // namespace ui
