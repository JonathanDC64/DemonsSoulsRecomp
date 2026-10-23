// The host's side of the setup engine (see setup_engine.hpp). Two facts differ by platform, each a small function:
// where the local data folder is, and which Python runs the engine.
#include "setup_engine.hpp"

#include "report.hpp"
#include "version.hpp"

#ifdef _WIN32
#include <windows.h>
// (after windows.h)
#include <shlobj.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

namespace ui::setup {
namespace {

constexpr const char* kFolderName = "DemonsSoulsRecomp";  // des_setup's NAME

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// ---- What differs by platform ----

#ifdef _WIN32

// %LOCALAPPDATA%: not the roaming folder, since the build is large and belongs to this PC.
std::filesystem::path local_data_base() {
    PWSTR local = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) out = local;
    CoTaskMemFree(local);
    return out;
}

// The embeddable Python, once it is unpacked where des_setup's fetch.ensure puts it, with the stamp that names the
// archive's SHA-256 (setup_python.hpp; fetch.unpacked checks the same stamp); empty before that.
std::filesystem::path platform_python() {
    return python_unpacked() ? python_folder() / "python.exe" : std::filesystem::path{};
}

#else

std::filesystem::path local_data_base() {
    if (const std::string xdg = env("XDG_DATA_HOME"); !xdg.empty()) return xdg;
    const std::string home = env("HOME");
    return home.empty() ? std::filesystem::path{} : std::filesystem::path(home) / ".local" / "share";
}

// The system's Python 3: python.org's embeddable package is for Windows only.
std::filesystem::path platform_python() {
    return "python3";
}

#endif

// ---- The same everywhere ----

// key=value lines (ready.txt); '#' starts a comment line.
std::map<std::string, std::string> read_key_values(const std::filesystem::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

std::vector<std::filesystem::path> split_paths(const std::string& s) {
    std::vector<std::filesystem::path> out;
    size_t from = 0;
    while (from <= s.size()) {
        const size_t semi = std::min(s.find(';', from), s.size());
        if (semi > from) out.push_back(from_utf8(s.substr(from, semi - from)));
        from = semi + 1;
    }
    return out;
}

}  // namespace

const std::filesystem::path& data_dir() {
    static const std::filesystem::path dir = [] {
        if (const std::string given = env("DES_SETUP_DATA"); !given.empty()) return from_utf8(given);
        const auto base = local_data_base();
        return base.empty() ? base : base / kFolderName;
    }();
    return dir;
}

Request request_from_env() {
    Request r;
    r.game = from_utf8(env("DES_SETUP_GAME"));
    r.decrypted = split_paths(env("DES_SETUP_DECRYPTED"));
    r.firmware = from_utf8(env("DES_SETUP_FIRMWARE"));
    r.rpcs3 = env("DES_SETUP_RPCS3");
    for (std::string s = env("DES_SETUP_IMPORT"); !s.empty();) {
        const size_t comma = s.find(',');
        if (comma) r.imports.push_back(s.substr(0, comma));
        s = comma == std::string::npos ? std::string() : s.substr(comma + 1);
    }
    return r;
}

std::filesystem::path gamedata_dir() {
    const auto dir = report::program_dir();
    return dir.empty() ? dir : dir / "gamedata";
}

Launch launch_state() {
    Launch out;
    const auto setup = data_dir() / "setup";
    std::error_code ec;
    auto ready = read_key_values(setup / "ready.txt");
    if (ready.empty()) {
        out.state = std::filesystem::exists(setup / "journal.json", ec) ? State::Interrupted : State::Missing;
        return out;
    }
    out.game = from_utf8(ready["game"]);
    out.eboot = from_utf8(ready["eboot"]);
    out.module = from_utf8(ready["module"]);
    const auto size = std::filesystem::file_size(out.module, ec);
    if (ec || std::to_string(size) != ready["module_size"] || !std::filesystem::is_regular_file(out.eboot, ec))
        out.state = State::Damaged;
    else if (ready["format"] != "1" || ready["app"] != kVersion)
        out.state = State::Outdated;
    else if (!std::filesystem::is_directory(out.game, ec))
        out.state = State::GameMoved;
    else
        out.state = State::Ready;
    return out;
}

std::filesystem::path python() {
    if (const std::string given = env("DES_SETUP_PYTHON"); !given.empty()) return from_utf8(given);
    return platform_python();
}

std::filesystem::path engine_script() {
    std::error_code ec;
    if (const std::string given = env("DES_SETUP_SCRIPT"); !given.empty()) {
        const auto script = from_utf8(given);
        return std::filesystem::is_regular_file(script, ec) ? script : std::filesystem::path{};
    }
    std::filesystem::path dir = report::program_dir();
    for (int up = 0; up <= 3 && !dir.empty(); ++up) {
        const auto script = dir / "tools" / "setup" / "run_setup.py";
        if (std::filesystem::is_regular_file(script, ec)) return script;
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

bool Engine::start(const Request& request, bool check_only, bool unlock_only) {
    mWhy.clear();
    if (engine_script().empty() || data_dir().empty()) {
        mWhy = "The setup files are missing from the game's folder. Extract the download again.";
        return false;
    }
    mRequest = request;
    mCheckOnly = check_only;
    mUnlockOnly = unlock_only && !check_only;
    mStopAsked = false;
    unsigned run = 0;
    {
        std::lock_guard<std::mutex> lock(mLock);
        run = ++mRun;
        mLines.clear();
        mEnded = mStopped = false;
        mFetchEnded = mFetchOk = mFetchStopped = false;
        mExit = 0;
    }
    mFetching = python().empty();
    if (!mFetching) return start_engine(run);

    // No Python yet (Windows, before the first run): the host's download of it first; poll() starts the engine after.
    // (each callback is for its own run, as below)
    mFromEngine = false;
    mProcess.stop();
    mFetch.start(
        [this, run](std::string line) {
            std::lock_guard<std::mutex> lock(mLock);
            if (run == mRun) mLines.push_back(std::move(line));
        },
        [this, run](bool ok, bool stopped) {
            std::lock_guard<std::mutex> lock(mLock);
            if (run != mRun) return;
            mFetchEnded = true;
            mFetchOk = ok;
            mFetchStopped = stopped;
        });
    std::fprintf(stderr, "[setup] started: the first Python's download, then the engine %s\n",
                 check_only ? "(check only)" : "");
    return true;
}

// The engine itself, under the Python there is now.
bool Engine::start_engine(unsigned run) {
    const auto py = python();
    const auto script = engine_script();
    if (py.empty() || script.empty()) {
        mWhy = py.empty() ? "Python for setup is not downloaded yet."
                          : "The setup files are missing from the game's folder. Extract the download again.";
        return false;
    }
    // -X utf8: the tools' prints and files are UTF-8 whatever the code page; -u: each line as it is printed (docs/52
    // 13.3).
    std::vector<std::string> args = {"-X", "utf8", "-u", utf8(script), "run"};
    if (!mRequest.game.empty()) {
        args.push_back(utf8(mRequest.game));
        for (const auto& d : mRequest.decrypted) {
            args.push_back("--decrypted");
            args.push_back(utf8(d));
        }
        if (!mRequest.firmware.empty()) {
            args.push_back("--firmware");
            args.push_back(utf8(mRequest.firmware));
        }
    }
    if (!mRequest.rpcs3.empty()) {  // the RPCS3 route (docs/90 3.2): without a game folder, the game in its library
        args.insert(args.end(), {"--rpcs3", mRequest.rpcs3});
        if (const auto gamedata = gamedata_dir(); !gamedata.empty())
            args.insert(args.end(), {"--gamedata", utf8(gamedata)});
    }
    if (!mRequest.imports.empty()) {
        std::string items;
        for (const auto& i : mRequest.imports) items += (items.empty() ? "" : ",") + i;
        args.insert(args.end(), {"--import", items});
    }
    if (mRequest.noImport) args.push_back("--no-import");
    args.insert(args.end(), {"--data", utf8(data_dir()), "--app", kVersion});
    // The shaders phase starts this program again, with no window (`--shaders`, des_prework.cpp).
    if (const auto self = report::program_path(); !self.empty()) args.insert(args.end(), {"--host", utf8(self)});
    if (mCheckOnly) args.push_back("--check-only");
    else if (mUnlockOnly) args.push_back("--unlock-only");
    mFromEngine = true;
    // (each callback is for its own run: the last run's thread may still be ending while this one starts)
    const bool started = mProcess.start(
        py, args,
        [this, run](std::string line) {
            std::lock_guard<std::mutex> lock(mLock);
            if (run == mRun) mLines.push_back(std::move(line));
        },
        [this, run](int status, bool stopped) {
            std::lock_guard<std::mutex> lock(mLock);
            if (run != mRun) return;
            mEnded = true;
            mStopped = stopped;
            mExit = status;
        });
    if (!started) mWhy = "The setup program could not start (" + mProcess.why() + ").";
    std::fprintf(stderr, "[setup] %s: %s %s%s\n", started ? "started" : "could not start", utf8(py).c_str(),
                 mCheckOnly ? "(check only)" : mUnlockOnly ? "(check and unlock)" : "",
                 mRequest.rpcs3.empty() ? "" : " with RPCS3");
    return started;
}

void Engine::stop() {
    mStopAsked = true;
    mFetch.stop();
    mProcess.stop();
}

void Engine::poll(Status& status) {
    std::vector<std::string> lines;
    bool ended = false, stopped = false, fetched = false, fetchOk = false, fetchStopped = false;
    int exit = 0;
    unsigned run = 0;
    {
        std::lock_guard<std::mutex> lock(mLock);
        lines.swap(mLines);
        ended = std::exchange(mEnded, false);
        stopped = mStopped;
        exit = mExit;
        fetched = std::exchange(mFetchEnded, false);
        fetchOk = mFetchOk;
        fetchStopped = mFetchStopped;
        run = mRun;
    }
    // (one batch is all the download's or all the engine's: the engine starts below, after the download's last line)
    const bool after = mFetching && mFromEngine;
    for (const std::string& text : lines) {
        Line l = parse_line(text);
        if (after && l.kind == Line::Kind::Overall) {  // the engine's bar fills what the download's share left
            l.n = kPythonShare * l.m + (1000 - kPythonShare) * l.n;
            l.m *= 1000;
        }
        status.apply(l);
        if (after && l.kind == Line::Kind::Plan)  // the download stays the run's first step, done
            status.steps.insert(status.steps.begin(), Status::Step{kPythonStep, Status::State::Done});
    }
    if (status.protocol != 0 && status.protocol != kProtocol && mProcess.running()) mProcess.stop();
    if (ended) status.ended(exit, stopped);
    if (!fetched) return;
    if (!fetchOk || mStopAsked) {
        status.ended(1, fetchStopped || mStopAsked);  // (a failure has said its @error and @done already)
    } else if (!start_engine(run)) {
        status.result = "failed";
        status.errorCode = "internal";
        status.errorStep = "-";
        status.errorText = mWhy;
    }
}

}  // namespace ui::setup
