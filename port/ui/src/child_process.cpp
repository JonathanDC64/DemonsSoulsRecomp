// A child process read line by line (see child_process.hpp). The reading thread and the line splitting are the same
// everywhere; below them, one function for each platform starts the process, reads its pipe, stops it and waits for it.
//
// Windows: CreateProcessW with no console window (CREATE_NO_WINDOW), standard output and error on one anonymous pipe,
// and only that pipe and NUL handed down (not every inheritable handle the game has). It starts suspended and goes into
// a job object that kills everything in it when its last handle closes, so nothing it starts (the lifter's workers,
// the compiler) can be left running: not after Cancel, and not if the host crashes.
// Elsewhere: posix_spawn into a process group of its own (stop() signals the whole group), SIGPIPE back to its default.
#include "child_process.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;
#endif

#include <utility>

namespace ui {
namespace {

constexpr size_t kMaxLine = 64 * 1024;  // a longer line without a break is passed on in pieces

}  // namespace

// ---- What differs by platform: start, read, stop, wait, close ----

#ifdef _WIN32

struct ChildProcess::Os {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE out = nullptr;  // the pipe's reading end
};

namespace {

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
    return out;
}

// One argument as CommandLineToArgvW (and the C runtime) read it back: quoted when it has a space, a tab or a quote,
// with the backslashes before a quote, and before the closing quote, doubled.
void append_arg(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

void close_handle(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

bool os_start(ChildProcess::Os& os, const std::filesystem::path& program, const std::vector<std::string>& args,
              const std::filesystem::path& folder, std::string& why) {
    SECURITY_ATTRIBUTES inheritable{sizeof inheritable, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        why = "no pipe, error " + std::to_string(GetLastError());
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0,
                             nullptr);

    // Only the pipe and NUL go to the child.
    HANDLE handed[2] = {writeEnd, nul};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    const bool listMade = size && InitializeProcThreadAttributeList(list, 1, 0, &size);
    bool ok = listMade && nul != INVALID_HANDLE_VALUE &&
              UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed, sizeof handed, nullptr,
                                        nullptr);

    os.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ok = ok && os.job && SetInformationJobObject(os.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = list;
    std::wstring command;
    append_arg(command, program.wstring());
    for (const std::string& a : args) append_arg(command, wide(a));
    PROCESS_INFORMATION pi{};
    // (suspended until it is in the job, so that nothing it starts can be outside it; a bare name is looked up in
    // PATH, as the command line's first word)
    ok = ok && CreateProcessW(program.has_parent_path() ? program.c_str() : nullptr, command.data(), nullptr, nullptr,
                              TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                              folder.empty() ? nullptr : folder.c_str(), &startup.StartupInfo, &pi);
    const DWORD error = ok ? 0 : GetLastError();
    if (listMade) DeleteProcThreadAttributeList(list);
    close_handle(writeEnd);  // (the child's copy is its own: the pipe ends when the child and its children close it)
    close_handle(nul);
    if (ok && !AssignProcessToJobObject(os.job, pi.hProcess)) {
        why = "not started in a job, error " + std::to_string(GetLastError());
        TerminateProcess(pi.hProcess, 1);
        close_handle(pi.hThread);
        close_handle(pi.hProcess);
        ok = false;
    } else if (!ok) {
        why = "error " + std::to_string(error);
    }
    if (!ok) {
        close_handle(readEnd);
        close_handle(os.job);
        return false;
    }
    ResumeThread(pi.hThread);
    close_handle(pi.hThread);
    os.process = pi.hProcess;
    os.out = readEnd;
    return true;
}

// Bytes read, or -1 once every writer has closed the pipe.
long long os_read(ChildProcess::Os& os, char* buffer, size_t size) {
    DWORD got = 0;
    if (!ReadFile(os.out, buffer, DWORD(size), &got, nullptr)) return -1;  // (ERROR_BROKEN_PIPE: the end)
    return got;
}

void os_stop(ChildProcess::Os& os) {
    if (os.job) TerminateJobObject(os.job, 1);
}

int os_wait(ChildProcess::Os& os) {
    WaitForSingleObject(os.process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(os.process, &code);
    return int(code);
}

// Closing the job ends anything still in it.
void os_close(ChildProcess::Os& os) {
    close_handle(os.process);
    close_handle(os.job);
    close_handle(os.out);
}

}  // namespace

#else

struct ChildProcess::Os {
    pid_t pid = -1;  // also its process group
    int out = -1;    // the pipe's reading end
};

namespace {

bool os_start(ChildProcess::Os& os, const std::filesystem::path& program, const std::vector<std::string>& args,
              const std::filesystem::path& folder, std::string& why) {
    if (!folder.empty()) {  // (posix_spawn has no standard way to start in another folder, and nothing here needs one)
        why = "no working folder here";
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        why = std::string("no pipe: ") + std::strerror(errno);
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);  // (the game may ignore it; ignored signals are inherited)
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setpgroup(&attributes, 0);  // a group of its own: stop() ends it and all it started
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> words{program.string()};
    words.insert(words.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = program.has_parent_path()
                       ? posix_spawn(&pid, words[0].c_str(), &actions, &attributes, argv.data(), environ)
                       : posix_spawnp(&pid, words[0].c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        why = std::strerror(rc);
        return false;
    }
    os.pid = pid;
    os.out = fds[0];
    return true;
}

long long os_read(ChildProcess::Os& os, char* buffer, size_t size) {
    ssize_t n;
    do {
        n = read(os.out, buffer, size);
    } while (n < 0 && errno == EINTR);
    return n > 0 ? n : -1;
}

void os_stop(ChildProcess::Os& os) {
    if (os.pid > 0) kill(-os.pid, SIGKILL);
}

// Waits without reaping it, so that its process group cannot be reused before os_close.
int os_wait(ChildProcess::Os& os) {
    siginfo_t info{};
    while (waitid(P_PID, id_t(os.pid), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR) {
    }
    return info.si_code == CLD_EXITED ? info.si_status : -1;
}

// Ends whatever of its group is left (as closing the job does on Windows), then reaps it.
void os_close(ChildProcess::Os& os) {
    if (os.pid > 0) {
        kill(-os.pid, SIGKILL);
        int status = 0;
        while (waitpid(os.pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    os.pid = -1;
    if (os.out >= 0) close(os.out);
    os.out = -1;
}

}  // namespace

#endif

// ---- The same everywhere ----

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess() {
    stop();
    if (mThread.joinable()) mThread.join();
}

bool ChildProcess::start(const std::filesystem::path& program, const std::vector<std::string>& args, LineFn on_line,
                         ExitFn on_exit, const std::filesystem::path& folder) {
    if (mThread.joinable()) {  // (the last run's thread)
        stop();
        mThread.join();
    }
    mWhy.clear();
    mStopped = false;
    mOs = std::make_unique<Os>();
    if (!os_start(*mOs, program, args, folder, mWhy)) {
        mOs.reset();
        return false;
    }
    mRunning = true;
    mThread = std::thread(&ChildProcess::read_loop, this, std::move(on_line), std::move(on_exit));
    return true;
}

void ChildProcess::stop() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mRunning.load() || !mOs) return;
    mStopped = true;
    os_stop(*mOs);
}

void ChildProcess::read_loop(LineFn on_line, ExitFn on_exit) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        const long long n = os_read(*mOs, buffer, sizeof buffer);
        if (n < 0) break;
        pending.append(buffer, size_t(n));
        size_t from = 0;
        for (size_t nl; (nl = pending.find('\n', from)) != std::string::npos; from = nl + 1) {
            size_t end = nl;
            if (end > from && pending[end - 1] == '\r') --end;
            on_line(pending.substr(from, end - from));
        }
        pending.erase(0, from);
        if (pending.size() > kMaxLine) {
            on_line(pending);
            pending.clear();
        }
    }
    if (!pending.empty()) on_line(pending);
    const int status = os_wait(*mOs);
    {
        std::lock_guard<std::mutex> lock(mLock);
        os_close(*mOs);
        mRunning = false;
    }
    on_exit(status, mStopped.load());
}

}  // namespace ui
