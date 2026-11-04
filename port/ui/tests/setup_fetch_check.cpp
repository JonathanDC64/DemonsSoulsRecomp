// The first Python's fetch on its own: setup_python.cpp with setup_http.cpp and setup_unzip.cpp, the data folder and
// the free space stubbed below. What Wine and CrossOver get (DES_SETUP_NO_SYSTEM_TOOLS=1: no curl or tar, so WinHTTP
// and the host's own unzip) against what Windows gets, a stopped download carried on, and the zip reader's refusals:
//   system    curl and tar, as on Windows
//   own       the host's own download and unzip
//   resume    the same, stopped once the .part file has 2 MB, then started again: it carries on from the file
//   same      system's and own's folders hold the same files, byte for byte
//   refusals  the zip reader refuses a cut archive and a changed byte, and unpacks the archive as it is
//
//   setup_fetch_check <a new folder>
//
// Each case has its own data folder under it. Prints one line a case and "setup_fetch_check: N passed, M failed"; exit
// 1 on a failure. It downloads python.org's embeddable zip three times (11 MB each). Built on demand: cmake --build
// <build> --target setup_fetch_check.
#include "setup_engine.hpp"
#include "setup_pc.hpp"
#include "setup_python.hpp"
#include "setup_unzip.hpp"
#include "sha256.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {
fs::path g_data;  // the case's data folder
}

// What setup_python.cpp needs of setup_engine.cpp and setup_pc.cpp.
namespace ui::setup {
const fs::path& data_dir() {
    return g_data;
}
Space data_space() {
    return {};
}
}  // namespace ui::setup

namespace {

struct Try {
    std::vector<std::string> lines;
    bool ok = false, stopped = false;
};

// One try at the first Python in `data`, to its end. stopAt > 0: stop() once the .part file has that many bytes.
Try run(const fs::path& data, unsigned long long stopAt) {
    std::error_code ec;
    fs::create_directories(data, ec);
    g_data = data;
    Try t;
    {
        std::mutex lock;
        std::condition_variable wake;
        bool ended = false;
        ui::setup::PythonFetch fetch;
        fetch.start(
            [&](std::string line) {
                std::lock_guard<std::mutex> l(lock);
                t.lines.push_back(std::move(line));
            },
            [&](bool ok, bool stopped) {
                std::lock_guard<std::mutex> l(lock);
                t.ok = ok;
                t.stopped = stopped;
                ended = true;
                wake.notify_all();
            });
        fs::path part = ui::setup::python_archive();
        part += ".part";
        std::unique_lock<std::mutex> l(lock);
        while (!ended) {
            wake.wait_for(l, std::chrono::milliseconds(50));
            if (stopAt && !ended) {
                l.unlock();
                const auto have = fs::file_size(part, ec);
                if (!ec && have >= stopAt) {
                    fetch.stop();
                    stopAt = 0;
                }
                l.lock();
            }
        }
    }  // (the fetch's thread is joined here)
    return t;
}

bool has_line(const Try& t, const std::string& part) {
    for (const std::string& l : t.lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

void show(const Try& t) {
    for (const std::string& l : t.lines)
        if (l.rfind("@progress", 0) != 0 && l.rfind("@overall", 0) != 0) std::printf("      | %s\n", l.c_str());
}

void no_system_tools(bool on) {
    _putenv_s("DES_SETUP_NO_SYSTEM_TOOLS", on ? "1" : "");
}

// Each file under `root`, by its name below it: its SHA-256.
std::map<std::string, std::string> sums(const fs::path& root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) out[fs::relative(it->path(), root).generic_string()] = ui::sha256_file(it->path());
    return out;
}

int g_passed = 0, g_failed = 0;

void result(const char* name, bool ok, const std::string& note) {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, note.empty() ? "" : ": ", note.c_str());
    (ok ? g_passed : g_failed)++;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: setup_fetch_check <a new folder>\n");
        return 2;
    }
    const fs::path root = fs::absolute(argv[1]);
    std::error_code ec;
    if (fs::exists(root, ec)) {
        std::fprintf(stderr, "setup_fetch_check: %s exists; give it a new folder\n", root.string().c_str());
        return 2;
    }

    // system
    no_system_tools(false);
    Try t = run(root / "system", 0);
    const bool systemOk = t.ok && ui::setup::python_unpacked() && !has_line(t, "instead");
    result("system", systemOk, systemOk ? "" : "curl and tar did not fetch it");
    if (!systemOk) show(t);
    const fs::path systemFolder = ui::setup::python_folder(), systemArchive = ui::setup::python_archive();

    // own
    no_system_tools(true);
    t = run(root / "own", 0);
    const bool ownOk = t.ok && ui::setup::python_unpacked() && has_line(t, "downloading with WinHTTP instead") &&
                       has_line(t, "unpacking with the host's own unzip instead");
    result("own", ownOk, "");
    if (!ownOk) show(t);
    const fs::path ownFolder = ui::setup::python_folder();

    // resume
    t = run(root / "resume", 2u << 20);
    fs::path part = ui::setup::python_archive();
    part += ".part";
    const auto left = fs::file_size(part, ec);
    const bool stoppedOk = t.stopped && !t.ok && !ec && left >= (2u << 20) && left < ui::setup::kPythonSize;
    Try again = run(root / "resume", 0);
    const bool resumeOk = stoppedOk && again.ok && ui::setup::python_unpacked() && has_line(again, "carried on from");
    result("resume", resumeOk, "stopped at " + std::to_string(ec ? 0 : left) + " bytes");
    if (!resumeOk) {
        show(t);
        show(again);
    }
    no_system_tools(false);

    // same
    const auto a = sums(systemFolder), b = sums(ownFolder);
    const bool sameOk = !a.empty() && a == b;
    result("same", sameOk, std::to_string(a.size()) + " files from tar, " + std::to_string(b.size()) + " from the unzip");

    // refusals
    std::vector<char> bytes;
    {
        std::ifstream f(systemArchive, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const fs::path cut = root / "cut.zip", changed = root / "changed.zip", whole = root / "whole.zip";
    const auto write = [](const fs::path& p, const char* data, size_t n) {
        std::ofstream(p, std::ios::binary).write(data, std::streamsize(n));
    };
    write(cut, bytes.data(), bytes.size() / 2);
    std::vector<char> flipped = bytes;
    flipped[flipped.size() / 2] ^= 0x5A;  // (inside python313.zip's data, most of the archive)
    write(changed, flipped.data(), flipped.size());
    write(whole, bytes.data(), bytes.size());
    std::vector<std::string> names;
    std::string whyCut, whyChanged, whyWhole;
    const bool cutRefused = !ui::setup::zip_names(cut, names, whyCut);
    fs::create_directories(root / "changed", ec);
    fs::create_directories(root / "whole", ec);
    const bool changedRefused = !ui::setup::zip_unpack(changed, root / "changed", whyChanged);
    auto unpacked = a;  // (what the archive holds: setup's folder less its stamp)
    unpacked.erase(ui::setup::kStampName);
    const bool wholeOk = ui::setup::zip_unpack(whole, root / "whole", whyWhole) && sums(root / "whole") == unpacked;
    result("refusals", cutRefused && changedRefused && wholeOk,
           "cut: " + whyCut + "; changed: " + whyChanged + (wholeOk ? "; as it is: unpacked" : "; as it is: " + whyWhole));

    std::printf("setup_fetch_check: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
