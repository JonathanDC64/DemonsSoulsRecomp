// The first Python (see setup_python.hpp). One fact differs by platform, in one small function: where the system's
// curl and tar are (only Windows downloads a Python). The steps, the checks and the stamp are the same code everywhere.
//
// The tools run in the data folder and are given names relative to it (downloads\python-embed-3.13.9.zip.part and so
// on, all ASCII), so a data folder whose path has characters outside the system's code page still reaches them intact
// (inferred: a tool that reads its arguments in the code page would get '?' for those characters).
#include "setup_python.hpp"

#include "setup_engine.hpp"
#include "setup_pc.hpp"
#include "sha256.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace ui::setup {
namespace {

constexpr double kGB = double(1ull << 30);
// What failed, for the player: the engine's sentences for the same failures (pipeline.py), and two of the host's.
constexpr const char* kDownloadFailed = "A download did not finish.";
constexpr const char* kDiskFull = "The disk is full.";
constexpr const char* kStartFailed = "Setup could not start its first download.";
constexpr const char* kUnpackFailed = "Setup could not unpack Python.";

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- What differs by platform ----

#ifdef _WIN32

// Windows' own curl.exe and tar.exe (Windows 10 1803 and later), by full path from the System folder: never a copy
// that PATH would find first.
std::filesystem::path system_tool(const char* name) {
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(dir) / (std::string(name) + ".exe");
}

#else

// None: the engine runs on the system's python3 here (setup_engine.cpp), so nothing asks for these.
std::filesystem::path system_tool(const char*) {
    return {};
}

#endif

// ---- The same everywhere ----

// fetch.py's <name>-<version>.
std::string base_name() {
    return std::string(kPythonName) + "-" + kPythonVersion;
}

// The archive, from the data folder: fetch.py's archive_path (the URL's suffix is .zip; the test checks it).
std::filesystem::path relative_archive() {
    return std::filesystem::path(kDownloadsFolder) / (base_name() + ".zip");
}

unsigned long long size_of(const std::filesystem::path& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : (unsigned long long)n;
}

// What a folder's files take (pipeline.py used_bytes).
unsigned long long used_bytes(const std::filesystem::path& dir) {
    unsigned long long total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code one;
        if (!it->is_regular_file(one)) continue;
        const auto n = it->file_size(one);
        if (!one) total += n;
    }
    return total;
}

// fetch.py unpack's rule for a name in the archive: no absolute name, no drive letter in its first part and no '..'
// part, so nothing lands outside the folder.
bool safe_entry(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (!name.empty() && name[0] == '/') return false;
    bool first = true;
    for (size_t from = 0; from <= name.size();) {
        const size_t slash = std::min(name.find('/', from), name.size());
        const std::string_view part(name.data() + from, slash - from);
        if (!part.empty() && part != ".") {  // (as PurePosixPath's parts: empty and '.' parts are not parts)
            if (part == ".." || (first && part.find(':') != std::string_view::npos)) return false;
            first = false;
        }
        from = slash + 1;
    }
    return true;
}

}  // namespace

std::filesystem::path python_archive() {
    return data_dir() / relative_archive();
}

std::filesystem::path python_folder() {
    return data_dir() / kToolsFolder / base_name();
}

bool python_unpacked() {
    if (data_dir().empty()) return false;
    std::ifstream stamp(python_folder() / kStampName);
    std::string sum;
    return (stamp >> sum) && sum == kPythonSha256;
}

PythonFetch::~PythonFetch() {
    stop();
    if (mThread.joinable()) mThread.join();
}

void PythonFetch::start(LineFn on_line, EndFn on_end) {
    if (mThread.joinable()) {  // (the last try's thread)
        stop();
        mThread.join();
    }
    mStop = false;
    mOnLine = std::move(on_line);
    mShownPercent = -1;
    mRunning = true;
    mThread = std::thread([this, on_end = std::move(on_end)] {
        const Result result = steps();
        mRunning = false;
        on_end(result == Result::Ok, result == Result::Stopped);
    });
}

void PythonFetch::stop() {
    {
        std::lock_guard<std::mutex> lock(mLock);  // (so that a wait cannot miss it)
        mStop = true;
    }
    mWake.notify_all();
    mTool.stop();
}

PythonFetch::Result PythonFetch::steps() {
    say(std::string("@plan ") + kPythonStep);
    say(std::string("@step ") + kPythonStep + " start");
    std::fprintf(stderr, "[setup] no Python for setup yet: downloading %s first\n", base_name().c_str());
    if (!sha256_known_answers()) return fail("internal", "Setup could not check its download.");

    // Room for it: the engine's figure for its fetch phase, less what the two folders hold (pipeline.py
    // _short_of_space, with its sentence).
    const std::filesystem::path data = data_dir();
    const unsigned long long used = used_bytes(data / kDownloadsFolder) + used_bytes(data / kToolsFolder);
    if (const Space space = data_space(); used < kFetchBytes && space.known && space.free < kFetchBytes - used) {
        char sentence[160];
        std::snprintf(sentence, sizeof sentence, "Setup needs about %.1f GB free on %s. %.1f GB is free.",
                      double(kFetchBytes - used) / kGB, space.where.c_str(), double(space.free) / kGB);
        return fail("disk", sentence);
    }
    std::error_code ec;
    std::filesystem::create_directories(data / kDownloadsFolder, ec);
    std::filesystem::create_directories(data / kToolsFolder, ec);
    if (!std::filesystem::is_directory(data / kDownloadsFolder, ec) ||
        !std::filesystem::is_directory(data / kToolsFolder, ec)) {
        say("FIX  " + base_name() + ": no folder for it in " + utf8(data));
        return fail("internal", kStartFailed);
    }

    // The archive: a copy that matches is kept (the engine's fetch may have made it), anything else goes, as in
    // fetch.py. Else the download, then its size and SHA-256, then its own name, so the engine's fetch finds it.
    const std::filesystem::path archive = python_archive();
    std::filesystem::path part = archive;
    part += ".part";
    bool have = false;
    if (std::filesystem::is_regular_file(archive, ec)) {
        have = size_of(archive) == kPythonSize && sha256_file(archive) == kPythonSha256;
        if (!have) std::filesystem::remove(archive, ec);
    }
    if (!have) {
        if (const Result r = download(part); r != Result::Ok) return r;
        const unsigned long long got = size_of(part);
        if (got != kPythonSize) {
            if (got > kPythonSize) std::filesystem::remove(part, ec);
            say(std::string("FIX  ") + kPythonName +
                (got > kPythonSize ? ": the download is larger than the manifest says"
                                   : ": the download is incomplete; it resumes next time"));
            return fail("download", kDownloadFailed);
        }
        const std::string sum = sha256_file(part);
        if (sum != kPythonSha256) {
            std::filesystem::remove(part, ec);
            say(std::string("FIX  ") + kPythonName + ": the download does not match its pinned SHA-256 (got " + sum +
                "); deleted");
            return fail("download", kDownloadFailed);
        }
        std::filesystem::rename(part, archive, ec);
        if (ec) {
            say(std::string("FIX  ") + kPythonName + ": the download could not be renamed (" + ec.message() + ")");
            return fail("internal", kUnpackFailed);
        }
    }
    progress(kPythonSize);
    if (stopping()) return Result::Stopped;

    if (const Result r = unpack(); r != Result::Ok) return r;
    say(std::string("@step ") + kPythonStep + " done");
    std::fprintf(stderr, "[setup] Python for setup is in place: %s\n", utf8(python_folder()).c_str());
    return Result::Ok;
}

// curl into the .part file, carrying on from what a stopped try left there (-C -); the bar from the file's size.
PythonFetch::Result PythonFetch::download(const std::filesystem::path& part) {
    std::filesystem::path relative = relative_archive();
    relative += ".part";
    std::error_code ec;
    for (bool again = false;; again = true) {
        unsigned long long have = size_of(part);
        if (have > kPythonSize) {  // (larger than the pin: start over, as fetch.py does)
            std::filesystem::remove(part, ec);
            have = 0;
        }
        progress(have);
        if (have == kPythonSize) return Result::Ok;  // (all there: a try that stopped before its check)
        // -q first: no curlrc of the player's changes what it does. --silent --show-error: no meter on the pipe, only
        // what went wrong. The two limits end a stalled try after a minute, as fetch.py's timeout does.
        const Ran ran = run_tool("curl",
                                 {"-q", "--fail", "--location", "--proto", "=https", "--tlsv1.2", "--silent",
                                  "--show-error", "--connect-timeout", "60", "--speed-limit", "1", "--speed-time",
                                  "60", "-C", "-", "-o", utf8(relative), kPythonUrl},
                                 nullptr, [&] { progress(size_of(part)); });
        if (stopping()) return Result::Stopped;
        progress(size_of(part));
        if (!ran.started) {
            say("FIX  " + mToolWhy);
            return fail("internal", kStartFailed);
        }
        if (ran.status == 0) return Result::Ok;
        if (ran.status == 33 && !again) {  // (curl's "cannot resume": the server ignored the range; start over once)
            std::filesystem::remove(part, ec);
            continue;
        }
        say(std::string("FIX  ") + kPythonName + ": the download stopped (curl exit " + std::to_string(ran.status) +
            "); it resumes next time");
        // (23: curl could not write; with less room left than the download needs, the disk is full)
        const unsigned long long left = kPythonSize - std::min(kPythonSize, size_of(part));
        if (const Space space = data_space(); ran.status == 23 && space.known && space.free < left)
            return fail("disk", kDiskFull);
        return fail("download", kDownloadFailed);
    }
}

// The archive's names first (tar -tf), refused as fetch.py's unpack refuses them; then tar -xf into <folder>.tmp, a
// rename into place, and the stamp. The archive is the pinned one by now, so a refusal means a changed pin (fetch.py
// says "download" for it too).
PythonFetch::Result PythonFetch::unpack() {
    const std::filesystem::path dest = python_folder();
    std::filesystem::path tmp = dest;
    tmp += ".tmp";
    const std::string archive = utf8(relative_archive());
    const std::string tmpRelative = utf8(std::filesystem::path(kToolsFolder) / (base_name() + ".tmp"));

    std::vector<std::string> names;
    Ran ran = run_tool("tar", {"-tf", archive}, &names, {});
    if (stopping()) return Result::Stopped;
    if (!ran.started || ran.status != 0 || names.empty()) {
        say("FIX  " + base_name() + ".zip: " +
            (ran.started ? "tar -tf ended with exit " + std::to_string(ran.status) : mToolWhy));
        return fail("internal", kUnpackFailed);
    }
    for (const std::string& name : names) {
        if (safe_entry(name)) continue;
        say("FIX  " + base_name() + ".zip: refusing the entry '" + name + "'");
        return fail("download", kDownloadFailed);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    const bool made = std::filesystem::create_directories(tmp, ec) && !ec;
    ran = made ? run_tool("tar", {"-xf", archive, "-C", tmpRelative}, nullptr, {}) : Ran{};
    if (stopping()) return Result::Stopped;
    if (!made || !ran.started || ran.status != 0 || !std::filesystem::is_regular_file(tmp / "python.exe", ec)) {
        say("FIX  " + base_name() + ".zip: " +
            (!made          ? std::string("no folder to unpack it in")
             : !ran.started ? mToolWhy
             : ran.status   ? "tar -xf ended with exit " + std::to_string(ran.status)
                            : std::string("no python.exe in it")));
        std::filesystem::remove_all(tmp, ec);
        return fail("internal", kUnpackFailed);
    }
    std::filesystem::remove_all(dest, ec);
    if (!rename_into_place(tmp, dest)) {
        if (stopping()) return Result::Stopped;
        say("FIX  " + base_name() + ": the unpacked folder could not be renamed into place");
        return fail("internal", kUnpackFailed);
    }

    // The stamp as fetch.py's ensure writes it: the SHA-256 and a line break, in text mode, so "\r\n" on Windows as
    // Python's write_text writes there. fetch.unpacked and python_unpacked() read it the same way.
    {
        std::ofstream stamp(dest / kStampName);
        stamp << kPythonSha256 << '\n';
        stamp.flush();
    }
    if (!python_unpacked()) {
        say("FIX  " + base_name() + ": its stamp could not be written");
        return fail("internal", kUnpackFailed);
    }
    return Result::Ok;
}

// One of the system's tools (curl, tar), run to its end in the data folder through the process runner: no window, and
// stop() ends it. Its lines are plain lines for Details, or go into `listing`. `each` runs every quarter second while
// it runs. It always waits for the tool's end, so no callback of the tool's comes after it returns.
PythonFetch::Ran PythonFetch::run_tool(const char* name, const std::vector<std::string>& args,
                                       std::vector<std::string>* listing, const std::function<void()>& each) {
    Ran out;
    std::error_code ec;
    const std::filesystem::path exe = system_tool(name);
    if (exe.empty() || !std::filesystem::is_regular_file(exe, ec)) {
        mToolWhy = std::string("no ") + name + ".exe in the System folder";
        return out;
    }
    {
        std::lock_guard<std::mutex> lock(mLock);
        mToolEnded = false;
        mToolStatus = 0;
        mListing.clear();
    }
    const bool keep = listing != nullptr;
    out.started = mTool.start(
        exe, args,
        [this, keep](std::string line) {
            if (keep) {
                std::lock_guard<std::mutex> lock(mLock);
                mListing.push_back(std::move(line));
            } else if (!line.empty() && line[0] == '@') {
                say(" " + line);  // (a tool's line never reads as the protocol)
            } else {
                say(std::move(line));
            }
        },
        [this](int status, bool) {
            {
                std::lock_guard<std::mutex> lock(mLock);
                mToolEnded = true;
                mToolStatus = status;
            }
            mWake.notify_all();
        },
        data_dir());
    if (!out.started) {
        mToolWhy = std::string(name) + ".exe did not start (" + mTool.why() + ")";
        return out;
    }
    if (stopping()) mTool.stop();  // (a stop() that came while it started)
    std::unique_lock<std::mutex> lock(mLock);
    while (!mToolEnded) {
        mWake.wait_for(lock, std::chrono::milliseconds(250));
        if (mToolEnded) break;
        lock.unlock();
        if (stopping()) mTool.stop();
        else if (each) each();
        lock.lock();
    }
    out.status = mToolStatus;
    if (listing) *listing = std::move(mListing);
    return out;
}

// A rename, tried again for up to two seconds: a virus scanner may still hold a file it has just seen (inferred).
bool PythonFetch::rename_into_place(const std::filesystem::path& from, const std::filesystem::path& to) {
    for (int tries = 0; tries < 8; ++tries) {
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (!ec) return true;
        std::unique_lock<std::mutex> lock(mLock);
        if (mWake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping(); })) return false;
    }
    return false;
}

PythonFetch::Result PythonFetch::fail(const char* code, const std::string& sentence) {
    std::fprintf(stderr, "[setup] the first Python: %s, %s\n", code, sentence.c_str());
    say(std::string("@error ") + code + " " + kPythonStep + " " + sentence);
    say("@done failed");
    return Result::Failed;
}

// The step's count and the run's bar, when the per cent changes (as the engine prints them).
void PythonFetch::progress(unsigned long long done) {
    done = std::min(done, kPythonSize);
    const int percent = int(done * 100 / kPythonSize);
    if (percent == mShownPercent) return;
    mShownPercent = percent;
    say("@progress download " + std::to_string(done) + "/" + std::to_string(kPythonSize));
    say("@overall " + std::to_string(done * kPythonShare / kPythonSize) + "/1000");
}

void PythonFetch::say(std::string line) {
    if (mOnLine) mOnLine(std::move(line));
}

}  // namespace ui::setup
