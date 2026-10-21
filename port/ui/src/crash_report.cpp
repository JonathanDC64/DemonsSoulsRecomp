// Crash reports (see crash_report.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's crash_report.cpp: the report,
// the stack walk, the minidump, the second process and the dialog's actions are FFCCEOT-Recomp's; chaining to the
// runtime's own filter and abort handler, the start before main() and the saved environment are this port's.
#include "crash_report.hpp"

#include "crash_dialog.hpp"
#include "mouse.hpp"
#include "report.hpp"
#include "version.hpp"

#include <windows.h>
// (after windows.h)
#include <dbghelp.h>
#include <shellapi.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern "C" uint32_t g_last_hle_nid;          // ppu_hle.cpp: the last HLE call (any thread)
extern "C" const char* g_last_hle_name;
extern "C" void ps3_diag_report(FILE* out, unsigned sample_sec);  // boot_main.cpp: what every thread is doing

namespace ui::crash_report {

namespace {

wchar_t g_dir[MAX_PATH];  // where the reports go (with a trailing backslash)
char g_stem[64];
wchar_t g_txt_path[MAX_PATH];
bool g_installed = false;
bool g_dialog = false;  // show the crash dialog (a second process) when the game ends
volatile LONG g_reported = 0, g_dialog_shown = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;  // boot_main's (prints the guest state)
void (*g_prev_abort)(int) = nullptr;                    // boot_main's (prints a backtrace, then _exit(3))
std::vector<wchar_t> g_environment;                     // the environment as the game started (the dialog's, and a restart's)

std::wstring path_of(const char* extension) {
    return std::wstring(g_dir) + std::wstring(g_stem, g_stem + std::strlen(g_stem)) +
           std::wstring(extension, extension + std::strlen(extension));
}

void open_stem() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(g_stem, sizeof g_stem, "crash-%Y%m%d-%H%M%S", &tm);
}

// Both to stderr (and with it the log) and to the report file.
struct Report {
    FILE* file = nullptr;
    void operator()(const char* fmt, ...) {
        va_list ap, again;
        va_start(ap, fmt);
        va_copy(again, ap);
        std::vfprintf(stderr, fmt, ap);
        if (file) std::vfprintf(file, fmt, again);
        va_end(again);
        va_end(ap);
    }
};

void frame_line(Report& out, HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD line_displacement = 0;
    char module[MAX_PATH] = "?";
    const DWORD64 base = SymGetModuleBase64(process, address);
    if (base) {
        GetModuleFileNameA(HMODULE(base), module, sizeof module);
        if (const char* slash = std::strrchr(module, '\\')) std::memmove(module, slash + 1, std::strlen(slash));
    }
    if (SymFromAddr(process, address, &displacement, symbol)) {
        if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
            const char* file = std::strrchr(line.FileName, '\\');  // (the file's name, not this PC's folders)
            out("  %016llx %s!%s+0x%llx  (%s:%lu)\n", (unsigned long long)address, module, symbol->Name,
                (unsigned long long)displacement, file ? file + 1 : line.FileName, line.LineNumber);
        } else {
            out("  %016llx %s!%s+0x%llx\n", (unsigned long long)address, module, symbol->Name,
                (unsigned long long)displacement);
        }
    } else {
        out("  %016llx %s+0x%llx\n", (unsigned long long)address, module, (unsigned long long)(address - base));
    }
}

void write_dump(EXCEPTION_POINTERS* info) {
    const HANDLE file = CreateFileW(path_of(".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    // (no data segments or heap: the game's 4 GB guest memory and the GPU driver's data would make it huge)
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules),
                      info ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

// The end of the log, beside the report (the log is des.log in the data folder's logs folder).
void write_log_tail(const std::wstring& path) {
    const auto log = report::log_path();
    if (log.empty()) return;
    report::flush_log();
    const std::string tail = report::read_head_tail(log, 60, 256 << 10);
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return;
    std::fwrite(tail.data(), 1, tail.size(), f);
    std::fclose(f);
}

// What to start again with (the dialog's restart button).
void write_cmdline(const std::wstring& path) {
    FILE* cmd = nullptr;
    _wfopen_s(&cmd, path.c_str(), L"wb");
    if (cmd) {
        const wchar_t* line = GetCommandLineW();
        std::fwrite(line, sizeof(wchar_t), std::wcslen(line), cmd);
        std::fclose(cmd);
    }
}

// Writes the report: what happened, then the stack from `context`.
void write_report(const char* what, CONTEXT* context, EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&g_reported, 1)) return;  // one report per run
    open_stem();
    const std::wstring txt = path_of(".txt");
    std::wcsncpy(g_txt_path, txt.c_str(), MAX_PATH - 1);
    Report out;
    _wfopen_s(&out.file, txt.c_str(), L"w");
    write_cmdline(path_of(".cmdline"));
    out("\n=== CRASH: %s ===\n", what);
    out("version %s, last HLE call 0x%08X (%s), thread %lu\n", kVersion, g_last_hle_nid,
        g_last_hle_name ? g_last_hle_name : "", GetCurrentThreadId());
    out("ps3recomp %s, game dump %s\n", kPs3recompVersion, *kDumpSha256 ? kDumpSha256 : "unknown");
    const HANDLE process = GetCurrentProcess();
    SymRefreshModuleList(process);  // (the modules loaded since install)
    CONTEXT ctx = *context;
    out("rip %016llx rsp %016llx rbp %016llx\nrax %016llx rbx %016llx rcx %016llx rdx %016llx\n"
        "rsi %016llx rdi %016llx r8  %016llx r9  %016llx\n",
        ctx.Rip, ctx.Rsp, ctx.Rbp, ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8, ctx.R9);
    out("stack:\n");
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        frame_line(out, process, frame.AddrPC.Offset);
    }
    write_dump(info);
    out("report: %s.txt, minidump: %s.dmp, log: %s.log (in the data folder's crashes folder)\n", g_stem, g_stem, g_stem);
    std::fflush(stderr);
    if (out.file) std::fclose(out.file);
    write_log_tail(path_of(".log"));
}

// The dialog's environment: the one the game started with, and DES_UI_RENDER as it is now (the dialog reads no
// settings, and draws with the renderer settings.ini chose for the menus).
std::vector<wchar_t> dialog_environment() {
    const char* render = std::getenv("DES_UI_RENDER");
    if (g_environment.empty() || !render) return g_environment;
    static constexpr wchar_t kName[] = L"DES_UI_RENDER=";
    constexpr size_t kLength = std::size(kName) - 1;
    std::vector<wchar_t> env;
    for (const wchar_t* p = g_environment.data(); *p; p += std::wcslen(p) + 1)
        if (_wcsnicmp(p, kName, kLength) != 0) env.insert(env.end(), p, p + std::wcslen(p) + 1);
    const std::wstring entry = kName + report::widen(render);
    env.insert(env.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
    env.push_back(L'\0');
    return env;
}

// Starts the dialog: a second process, since this one is in no state to show a window. It needs nothing of this one:
// the report's path, this process's id (a restart waits for it to end) and the data folder are on its command line,
// and it gets the environment the game started with (dialog_environment). Its process handle, or null.
HANDLE start_dialog(const std::wstring& txt) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring data = report::data_dir().wstring();
    while (!data.empty() && (data.back() == L'\\' || data.back() == L'/')) data.pop_back();
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --crash-dialog \"" + txt + L"\" " +
                           std::to_wstring(GetCurrentProcessId()) + L" \"" + data + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment = dialog_environment();
    // (no console window of its own: it is a window, and the game's console is going away)
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        environment.empty() ? nullptr : environment.data(), nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

void show_dialog() {
    if (!g_dialog || !g_txt_path[0] || InterlockedExchange(&g_dialog_shown, 1)) return;
    if (HANDLE process = start_dialog(g_txt_path)) CloseHandle(process);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    mouse::release_now();  // (mouse look: the cursor shown and free while the report is written)
    // The runtime's filter first: it prints the guest state (last HLE call, guest registers) into the log.
    const LONG result = g_prev_filter ? g_prev_filter(info) : EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& e = *info->ExceptionRecord;
    char text[160];
    if (e.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e.NumberParameters >= 2)
        std::snprintf(text, sizeof text, "access violation %s %016llx",
                      e.ExceptionInformation[0] == 0 ? "reading" : e.ExceptionInformation[0] == 1 ? "writing" : "executing",
                      (unsigned long long)e.ExceptionInformation[1]);
    else
        std::snprintf(text, sizeof text, "unhandled exception %08lx", e.ExceptionCode);
    write_report(text, info->ContextRecord, info);
    show_dialog();
    return result;
}

void on_abort(int sig) {
    mouse::release_now();
    CONTEXT context{};
    RtlCaptureContext(&context);
    write_report("abort (see the messages above it in the log)", &context, nullptr);
    show_dialog();
    if (g_prev_abort && g_prev_abort != SIG_DFL && g_prev_abort != SIG_IGN) g_prev_abort(sig);  // (its backtrace, _exit(3))
    _exit(3);
}

// ---- The dialog process ----

struct Dialog {
    std::filesystem::path txt, data;
    DWORD pid = 0;
    std::string what;
    bool hang = false;  // a hang report (report_hang): the game is still running
};

constexpr const char* kCrashMark = "=== CRASH: ";
constexpr const char* kHangMark = "=== NOT RESPONDING: ";

std::string read_text(const std::filesystem::path& path) {
    const auto b = report::read_file(path);
    std::string text(b.begin(), b.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());  // (the report was written in text mode)
    return text;
}

// "access violation writing 0000000000000000" or "no new frame for 14 s", from the first line of the report.
std::string what_happened(const std::string& txt) {
    std::string what = "The game stopped unexpectedly.";
    for (const char* mark : {kCrashMark, kHangMark}) {
        const size_t a = txt.find(mark);
        if (a == std::string::npos) continue;
        const size_t from = a + std::strlen(mark), b = txt.find(" ===", from);
        if (b != std::string::npos) what = txt.substr(from, b - from);
        break;
    }
    return what;
}

// The first `n` lines after the line `heading` starts in the report: the stack of a crash, the guest threads of a
// hang.
std::string top_of_stack(const std::string& txt, int n, const char* heading = "stack:\n") {
    const size_t a = txt.find(heading);
    if (a == std::string::npos) return {};
    std::string out;
    size_t at = txt.find('\n', a) + 1;
    for (int i = 0; i < n && at < txt.size(); i++) {
        const size_t e = txt.find('\n', at);
        out += txt.substr(at, e == std::string::npos ? std::string::npos : e - at + 1);
        if (e == std::string::npos) break;
        at = e + 1;
    }
    return out;
}

// settings.ini with the online name, the matchmaking password and the custom server's address only as set (as
// settings::describe() shows them; this process has not read the settings).
std::string hide_private(const std::string& ini) {
    static const char* kHidden[] = {"DES_NET_NAME", "DES_NET_PASSWORD", "DES_NET_CUSTOM"};
    std::string out;
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        if (end == std::string::npos) end = ini.size();
        std::string line = ini.substr(at, end - at);
        for (const char* key : kHidden) {
            const size_t eq = line.find('=');
            if (line.rfind(key, 0) == 0 && eq != std::string::npos &&
                line.find_first_not_of(" \t\r", eq + 1) != std::string::npos &&
                line.find_first_not_of(" \t", std::strlen(key)) == eq)
                line = std::string(key) + " = (set)";
        }
        out += line + "\n";
        at = end + 1;
    }
    return out;
}

// Zips what a bug report needs and opens the issue form, filled in, and the folder with the zip to attach to it.
// The text to show the player afterwards.
std::string make_report(Dialog& d) {
    const std::string txt = report::redact(read_text(d.txt));
    const std::string system = report::redact(report::system_info());
    std::vector<report::File> files;
    files.push_back({d.hang ? "hang.txt" : "crash.txt", report::bytes(txt)});
    files.push_back({"system.txt", report::bytes(system)});
    // (their settings: as the game had them, with the defaults and what the environment set; else the file)
    if (auto in_use = report::read_file(d.data / L"logs" / L"settings.txt"); !in_use.empty())
        files.push_back({"settings.ini", report::bytes(report::redact(std::string(in_use.begin(), in_use.end())))});
    else if (auto ini = report::read_file(d.data / L"settings.ini"); !ini.empty())
        files.push_back({"settings.ini", report::bytes(report::redact(hide_private(std::string(ini.begin(), ini.end()))))});
    {
        auto cmd = d.txt;  // (the game's command line, saved beside the report: this process's is the dialog's)
        cmd.replace_extension(L".cmdline");
        const auto b = report::read_file(cmd);
        const std::wstring line(reinterpret_cast<const wchar_t*>(b.data()), b.size() / sizeof(wchar_t));
        files.push_back({"environment.txt", report::bytes(report::redact(report::environment_info(report::narrow(line))))});
    }
    auto log = d.txt;
    log.replace_extension(L".log");
    if (const std::string tail = read_text(log); !tail.empty()) files.push_back({"des.log", report::bytes(report::redact(tail))});
    const auto zip = report::write_bundle(report::narrow(d.txt.stem().wstring()), files);

    std::string body = std::string("## What happened\n(Please say what you were doing when the game ") +
                       (d.hang ? "stopped.)\n\n## Not responding\n" : "crashed.)\n\n## Crash\n") + d.what + "\n\n```\n" +
                       (d.hang ? top_of_stack(txt, 24, "--- guest threads") : top_of_stack(txt, 20)) +
                       "```\n\n## System\n```\n" + system + "```\n\n## Attachments\n";
    const std::string name = report::narrow(zip.filename().wstring());
    const char* holds = d.hang ? "what every thread was doing, your settings and the game's recent messages"
                               : "the full stack, your settings and the game's recent messages";
    body += zip.empty() ? "(The report files could not be saved.)\n"
                        : "Drag `" + name + "` (its folder is open) into this issue: it has " + holds + ".\n";
    report::open_issue(report::issue_url((d.hang ? "Not responding: " : "Crash: ") + d.what.substr(0, 80), body), zip);
    return zip.empty() ? "The report files could not be saved. The issue form is open in your browser."
                       : "The issue form is open in your browser, with the folder holding " + name +
                             ". Drag that file into the issue to attach the details: " + holds + ". Thank you!";
}

// A hang's "Close the game": the game is still running.
void close_game(const Dialog& d) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, d.pid)) {
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
}

void restart(const Dialog& d) {
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, d.pid)) {  // (the crashed process must be gone first)
        WaitForSingleObject(h, 10000);
        CloseHandle(h);
    }
    auto path = d.txt;
    path.replace_extension(L".cmdline");
    const auto b = report::read_file(path);
    if (b.size() < sizeof(wchar_t)) return;
    std::wstring command(reinterpret_cast<const wchar_t*>(b.data()), b.size() / sizeof(wchar_t));
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // (this process has the environment the game started with, and its working folder)
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

int run_dialog(int argc, LPWSTR* argv) {
    if (argc < 5) return 2;
    Dialog d;
    d.txt = argv[2];
    d.pid = DWORD(std::wcstoul(argv[3], nullptr, 10));
    d.data = argv[4];
    report::set_data_dir(d.data);
    const std::string txt = read_text(d.txt);
    d.what = what_happened(txt);
    d.hang = txt.find(kHangMark) != std::string::npos;

    crash_dialog::Content content{d.what, d.hang ? top_of_stack(txt, 30, "--- guest threads") : top_of_stack(txt, 14),
                                  d.hang};
    crash_dialog::Actions actions;
    actions.report = [&d] { return make_report(d); };
    actions.restart = [&d] { restart(d); };
    actions.close_game = [&d] { close_game(d); };
    if (!crash_dialog::show(content, actions)) {
        // (no window to be had: a plain message box)
        const std::wstring text = report::widen(d.what) + L"\n\nA report was saved in " + d.txt.parent_path().wstring() +
                                  L".\nTo report it, attach the files in that folder to a new issue at " +
                                  report::widen(report::kRepositoryUrl);
        MessageBoxW(nullptr, text.c_str(), d.hang ? L"Demon's Souls is not responding" : L"Demon's Souls has crashed",
                    MB_OK | MB_ICONERROR);
    }
    return 0;
}

// Before main(), after every other static initializer (see below): the crash dialog process runs the dialog here and
// ends, without ever starting the game; the game saves its environment and starts its log.
void startup_hook() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && std::wcscmp(argv[1], L"--crash-dialog") == 0) {
        const int rc = run_dialog(argc, argv);
        ExitProcess(UINT(rc));
    }
    if (argv) LocalFree(argv);
    if (LPWCH env = GetEnvironmentStringsW()) {
        const wchar_t* p = env;
        while (*p) p += std::wcslen(p) + 1;
        g_environment.assign(static_cast<const wchar_t*>(env), p + 1);
        FreeEnvironmentStringsW(env);
    }
    report::start_log();
}

HANDLE g_hang_dialog = nullptr;  // the hang dialog report_hang started, while it is open

}  // namespace

bool hang_dialog_open() {
    if (!g_hang_dialog) return false;
    if (WaitForSingleObject(g_hang_dialog, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(g_hang_dialog);
    g_hang_dialog = nullptr;
    return false;
}

void report_hang(long long ms_since_frame) {
    if (!g_installed || hang_dialog_open()) return;
    char stem[64];
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::strftime(stem, sizeof stem, "hang-%Y%m%d-%H%M%S", &tm);
    }
    const std::wstring base = std::wstring(g_dir) + report::widen(stem);
    FILE* out = nullptr;
    _wfopen_s(&out, (base + L".txt").c_str(), L"w");
    if (!out) return;
    std::fprintf(out, "\n%sno new frame for %lld s ===\n", kHangMark, ms_since_frame / 1000);
    std::fprintf(out, "version %s, ps3recomp %s, game dump %s\n", kVersion, kPs3recompVersion,
                 *kDumpSha256 ? kDumpSha256 : "unknown");
    std::fprintf(out, "The menu could not open: the render thread did not answer.\n\n");
    ps3_diag_report(out, 2);
    std::fclose(out);
    write_cmdline(base + L".cmdline");
    write_log_tail(base + L".log");
    std::fprintf(stderr, "[report] hang report: %s.txt (in the data folder's crashes folder)\n", stem);
    if (g_dialog) g_hang_dialog = start_dialog(base + L".txt");
}

void install() {
    if (g_installed || report::data_dir().empty()) return;  // test runs: nothing
    g_installed = true;
    const char* no_dialog = std::getenv("DES_NO_CRASH_DIALOG");
    g_dialog = !(no_dialog && *no_dialog && std::strcmp(no_dialog, "0") != 0);
    std::error_code ec;
    const auto dir = report::data_dir() / L"crashes";
    std::filesystem::create_directories(dir, ec);
    std::wstring d = dir.wstring();
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    std::wcsncpy(g_dir, d.c_str(), MAX_PATH - 1);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_EXACT_SYMBOLS);  // (never a stale .pdb)
    {  // (the .pdb beside the exe: the path the linker wrote into it is the build machine's)
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring dir = std::filesystem::path(std::wstring(exe, n)).parent_path().wstring();
        SymInitializeW(GetCurrentProcess(), dir.c_str(), TRUE);
    }
    // No vectored handler: the runtime commits guest memory on first touch through access violations, which are then
    // not crashes. Only what nothing handled reaches this filter.
    g_prev_filter = SetUnhandledExceptionFilter(on_unhandled);
    g_prev_abort = std::signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no "abort() has been called" dialog
}

}  // namespace ui::crash_report

// The startup hook in the C runtime's table of initializers: the linker sorts the .CRT$XC* sections by name, so
// .CRT$XCV runs after every ordinary static initializer (.CRT$XCU) and before main().
extern "C" void des_crash_startup(void) { ui::crash_report::startup_hook(); }
#pragma section(".CRT$XCV", read)
extern "C" __declspec(allocate(".CRT$XCV")) void (*const des_crash_startup_entry)(void) = des_crash_startup;
#pragma comment(linker, "/include:des_crash_startup_entry")
