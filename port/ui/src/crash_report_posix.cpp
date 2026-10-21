// Written for Demon's Souls. Crash reports off Windows (docs/76; crash_report.hpp, and crash_report.cpp for Windows):
// the log starts here (des_ui_early_init), and when the game ends on a fault nothing handled or on an abort, this
// writes crash-<time>.txt (what happened, the last HLE call and a backtrace) and crash-<time>.log (the end of the log)
// in the data folder's crashes folder; a hang (report_hang) writes hang-<time>.txt and .log beside them. No dialog yet:
// the report is on disk, and the player is told where in the log.
//
// The faults come through the runtime's Win32 exception shim (runtime/platform/win32_compat.h): its unhandled filter
// runs only after the runtime's own handlers passed on the fault, as SetUnhandledExceptionFilter does on Windows.
// The runtime's previous filter and abort handler still run afterwards (they print the guest state).
#include "crash_report.hpp"

#include "mouse.hpp"
#include "report.hpp"
#include "version.hpp"

#include <execinfo.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

extern "C" uint32_t g_last_hle_nid;  // ppu_hle.cpp: the last HLE call (any thread)
extern "C" const char* g_last_hle_name;
extern "C" void ps3_diag_report(FILE* out, unsigned sample_sec);  // boot_main.cpp: what every thread is doing

// The shim's exception types (win32_compat.h), as far as they are read here.
namespace {
struct ExceptionRecord {
    uint32_t code;
    uint32_t flags;
    ExceptionRecord* record;
    void* address;
    uint32_t parameters;
    size_t information[15];
};
struct ExceptionPointers {
    ExceptionRecord* record;
    void* context;
};
using Filter = int32_t (*)(ExceptionPointers*);
}  // namespace

extern "C" Filter SetUnhandledExceptionFilter(Filter filter);

namespace ui::crash_report {

namespace {

std::string g_dir;  // where the reports go (with a trailing slash)
bool g_installed = false;
volatile sig_atomic_t g_reported = 0;
Filter g_prev_filter = nullptr;            // boot_main's, if any
void (*g_prev_abort)(int) = nullptr;       // boot_main's (prints a backtrace, then exits)

// "<kind>-<local time>" in the crashes folder.
std::string stem_of(const char* kind) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stem[64];
    std::snprintf(stem, sizeof stem, "%s-", kind);
    std::strftime(stem + std::strlen(stem), sizeof stem - std::strlen(stem), "%Y%m%d-%H%M%S", &tm);
    return g_dir + stem;
}

// The end of the log, into `path`.
void write_log_tail(const std::string& path) {
    report::flush_log();
    if (const auto log = report::log_path(); !log.empty()) {
        const std::string tail = report::read_head_tail(log, 40, 192 << 10);
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fwrite(tail.data(), 1, tail.size(), f);
            std::fclose(f);
        }
    }
}

// The report, once: what happened, then a backtrace, then the end of the log beside it.
void write_report(const char* what) {
    if (g_reported) return;
    g_reported = 1;
    mouse::release_now();
    const std::string base = stem_of("crash");
    std::fprintf(stderr, "[crash] %s; last HLE call %s (0x%08X)\n", what, g_last_hle_name ? g_last_hle_name : "?",
                 g_last_hle_nid);
    if (FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
        std::fprintf(f, "DemonsSoulsRecomp %s crashed: %s\nLast HLE call: %s (0x%08X)\n\n", kVersion, what,
                     g_last_hle_name ? g_last_hle_name : "?", g_last_hle_nid);
        std::fflush(f);
        void* frames[64];
        backtrace_symbols_fd(frames, backtrace(frames, 64), fileno(f));
        std::fclose(f);
    }
    write_log_tail(base + ".log");
    std::fprintf(stderr, "[crash] report: %s.txt\n", base.c_str());
    report::flush_log();
}

int32_t on_unhandled(ExceptionPointers* e) {
    char what[160];
    const ExceptionRecord* r = e ? e->record : nullptr;
    if (r && r->code == 0xC0000005u && r->parameters >= 2)
        std::snprintf(what, sizeof what, "access violation %s %p at %p", r->information[0] ? "writing" : "reading",
                      reinterpret_cast<void*>(r->information[1]), r->address);
    else
        std::snprintf(what, sizeof what, "exception 0x%08X at %p", r ? r->code : 0u, r ? r->address : nullptr);
    write_report(what);
    return g_prev_filter ? g_prev_filter(e) : 0;  // (0: EXCEPTION_CONTINUE_SEARCH, the process ends as it would have)
}

void on_abort(int sig) {
    write_report("abort");
    if (g_prev_abort && g_prev_abort != SIG_DFL && g_prev_abort != SIG_IGN) g_prev_abort(sig);
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

}  // namespace

bool hang_dialog_open() { return false; }  // (no dialog off Windows yet)

void report_hang(long long ms_since_frame) {
    if (!g_installed) return;
    const std::string base = stem_of("hang");
    FILE* out = std::fopen((base + ".txt").c_str(), "w");
    if (!out) return;
    std::fprintf(out, "\n=== NOT RESPONDING: no new frame for %lld s ===\n", ms_since_frame / 1000);
    std::fprintf(out, "version %s, ps3recomp %s, game dump %s\n", kVersion, kPs3recompVersion,
                 *kDumpSha256 ? kDumpSha256 : "unknown");
    std::fprintf(out, "The menu could not open: the render thread did not answer.\n\n");
    ps3_diag_report(out, 2);
    std::fclose(out);
    write_log_tail(base + ".log");
    std::fprintf(stderr, "[report] hang report: %s.txt\n", base.c_str());
}

void install() {
    if (g_installed || report::data_dir().empty()) return;  // test runs: nothing
    g_installed = true;
    report::start_log();
    std::error_code ec;
    const auto dir = report::data_dir() / "crashes";
    std::filesystem::create_directories(dir, ec);
    g_dir = dir.string() + "/";
    g_prev_filter = SetUnhandledExceptionFilter(on_unhandled);
    g_prev_abort = std::signal(SIGABRT, on_abort);
}

}  // namespace ui::crash_report
