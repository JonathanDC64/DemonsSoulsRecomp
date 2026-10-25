// Problem reports (see report.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's report.cpp (the zip writer, the
// redaction, the system facts, the issue link and the log tee); the data folder follows FFCCEOT-Recomp's main.cpp.
#include "report.hpp"

#include "host.hpp"
#include "portable.hpp"
#include "settings.hpp"
#include "version.hpp"

#include <stb_image_write.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// (after windows.h)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <fcntl.h>
#include <intrin.h>
#include <psapi.h>
#include <xinput.h>
#include <io.h>
#include <objbase.h>
#include <shellapi.h>
#include <share.h>
#include <shlobj.h>
#else
#include "platform.hpp"

#include <SDL.h>  // (the displays, for system_info)
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>

extern char** environ;
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache
extern "C" void ps3_diag_report(FILE* out, unsigned sample_sec);   // boot_main.cpp: what every thread is doing
extern "C" void* rsx_d3d12_device(void);                           // the D3D12 backend's device, or null
extern "C" int pad_hid_sony_poll(void* out, int max);              // the DualSense / DualShock 4 reader
extern "C" unsigned ppu_boot_frames_presented(void);               // boot_main.cpp: the game's frames so far

namespace ui::report {

namespace {

std::filesystem::path g_data_dir;
bool g_data_dir_resolved = false;
std::filesystem::path g_log_path;

// Replaces every occurrence of `what` in `text` (ignoring case) with `with`. With `whole_word`, only where it is not
// part of a longer word ("jon" in "JonathanDC64" stays).
void replace_all(std::string& text, const std::string& what, const std::string& with, bool whole_word = false) {
    if (what.size() < 3) return;
    std::string lower = text, needle = what;
    for (char& c : lower) c = char(std::tolower((unsigned char)c));
    for (char& c : needle) c = char(std::tolower((unsigned char)c));
    const auto word = [&](size_t i) { return i < lower.size() && (std::isalnum((unsigned char)lower[i]) || lower[i] == '_'); };
    std::string out;
    size_t at = 0;
    for (size_t f = lower.find(needle); f != std::string::npos; f = lower.find(needle, f + 1)) {
        if (f < at) continue;
        if (whole_word && ((f > 0 && word(f - 1)) || word(f + needle.size()))) continue;
        out.append(text, at, f - at);
        out += with;
        at = f + needle.size();
    }
    out.append(text, at, std::string::npos);
    text = std::move(out);
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

unsigned long long env_u64(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtoull(v, nullptr, 10) : 0;
}

#ifdef _WIN32
std::filesystem::path exe_path() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

// %APPDATA%\DemonsSoulsRecomp\DemonsSouls (what SDL_GetPrefPath("DemonsSoulsRecomp", "DemonsSouls") gives).
std::filesystem::path pref_dir() {
    PWSTR roaming = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming)
        out = std::filesystem::path(roaming) / L"DemonsSoulsRecomp" / L"DemonsSouls";
    CoTaskMemFree(roaming);
    return out;
}

// The command line's arguments (the program first), and the command line as the process got it.
std::vector<std::string> command_line_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; argv && i < argc; i++) args.push_back(narrow(argv[i]));
    if (argv) LocalFree(argv);
    return args;
}

std::string command_line_text() {
    return narrow(GetCommandLineW());
}

// The environment's NAME=value strings.
std::vector<std::string> environment_strings() {
    std::vector<std::string> out;
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) out.push_back(narrow(p));
        FreeEnvironmentStringsW(env);
    }
    return out;
}

unsigned long process_id() {
    return GetCurrentProcessId();
}

// Cuts the open file `f` to `size` bytes; positions and sizes past 2 GB.
void truncate_file(FILE* f, long long size) {
    _chsize_s(_fileno(f), __int64(size));
}

int seek_file(FILE* f, long long offset, int origin) {
    return _fseeki64(f, offset, origin);
}

long long tell_file(FILE* f) {
    return _ftelli64(f);
}

// COM on a report's thread (ShellExecute).
void begin_report_thread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

void end_report_thread() {
    CoUninitialize();
}

// The user's account and the PC's name (reports leave them out).
std::string user_name() {
    return env("USERNAME");
}

std::string pc_name() {
    return env("COMPUTERNAME");
}

#else
// The program's path as it was started. /proc/self/exe follows the running file wherever it goes: once an update
// has moved this version into .previous/ (updates.cpp), it names that copy, and a start from it would run the old
// version again. So it is read once, at the start, as GetModuleFileNameW keeps the path the exe was loaded from.
std::filesystem::path exe_path() {
    static const std::filesystem::path exe = [] {
        std::error_code ec;
        const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::path{} : p;
    }();
    return exe;
}
[[maybe_unused]] const std::filesystem::path g_exe_at_start = exe_path();  // (read before main, before any update)

// $XDG_DATA_HOME/DemonsSoulsRecomp/DemonsSouls, else ~/.local/share/... (what SDL_GetPrefPath gives there).
std::filesystem::path pref_dir() {
    const std::string xdg = env("XDG_DATA_HOME"), home = env("HOME");
    const std::filesystem::path base = !xdg.empty()    ? std::filesystem::path(xdg)
                                       : !home.empty() ? std::filesystem::path(home) / ".local" / "share"
                                                       : std::filesystem::path{};
    return base.empty() ? base : base / "DemonsSoulsRecomp" / "DemonsSouls";
}

// The command line's arguments (the program first), and the command line as the process got it (quoted where an
// argument holds a space).
std::vector<std::string> command_line_args() {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> args;
    for (std::string a; std::getline(f, a, '\0');) args.push_back(a);
    return args;
}

std::string command_line_text() {
    std::string out;
    for (const std::string& a : command_line_args()) {
        if (!out.empty()) out += ' ';
        out += a.find(' ') != std::string::npos ? "\"" + a + "\"" : a;
    }
    return out;
}

// The environment's NAME=value strings.
std::vector<std::string> environment_strings() {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; e++) out.push_back(*e);
    return out;
}

unsigned long process_id() {
    return (unsigned long)getpid();
}

// Cuts the open file `f` to `size` bytes; positions and sizes past 2 GB.
void truncate_file(FILE* f, long long size) {
    if (ftruncate(fileno(f), off_t(size)) != 0) {}
}

int seek_file(FILE* f, long long offset, int origin) {
    return fseeko(f, off_t(offset), origin);
}

long long tell_file(FILE* f) {
    return (long long)ftello(f);
}

void begin_report_thread() {}
void end_report_thread() {}

// The user's account and the PC's name (reports leave them out).
std::string user_name() {
    return env("USER");
}

std::string pc_name() {
    char host[256] = {};
    return gethostname(host, sizeof host - 1) == 0 ? std::string(host) : std::string();
}

#endif

// The PSN name given on the command line (--username NAME or --username=NAME), if any.
std::string psn_name() {
    const std::vector<std::string> args = command_line_args();
    std::string name;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i].rfind("--username=", 0) == 0) name = args[i].substr(11);
        else if (args[i] == "--username" && i + 1 < args.size()) name = args[i + 1];
    }
    return name;
}

// The online name (Settings, Network), which the runtime's NP messages print: the environment's, else the setting,
// else settings.ini's line (the crash dialog's process has not read the file).
std::string online_name() {
    for (const char* var : {"PS3_NP_ONLINE_ID", "DES_NET_NAME"})
        if (std::string v = env(var); !v.empty()) return v;
    if (std::string v = settings::get("DES_NET_NAME"); !v.empty()) return v;
    if (const std::string& file = settings::path(); !file.empty()) {
        if (FILE* f = open_file(widen(file), "r")) {
            char line[512];
            std::string name;
            while (name.empty() && std::fgets(line, sizeof line, f)) {
                std::string l = line;
                if (l.rfind("DES_NET_NAME", 0) != 0) continue;
                const size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                l = l.substr(eq + 1);
                const size_t a = l.find_first_not_of(" \t"), b = l.find_last_not_of(" \t\r\n");
                if (a != std::string::npos && b >= a) name = l.substr(a, b - a + 1);
            }
            std::fclose(f);
            return name;
        }
    }
    return {};
}

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string& s, uint32_t v) {
    s += char(v & 0xFF);
    s += char((v >> 8) & 0xFF);
}
void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

// A zip of stored (uncompressed) files: the screenshot is a PNG already, and the text is small.
bool write_zip(const std::filesystem::path& path, const std::vector<File>& files) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ui::local_time(t, tm);
    const uint32_t dos_time = uint32_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const uint32_t dos_date = uint32_t(std::max(0, tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    FILE* out = open_file(path, "wb");
    if (!out) return false;
    std::string central;
    uint32_t offset = 0;
    bool ok = true;
    for (const File& f : files) {
        const uint32_t crc = crc32(f.data.data(), f.data.size());
        std::string head;
        put32(head, 0x04034B50);
        put16(head, 20);      // version needed
        put16(head, 0x0800);  // names are UTF-8
        put16(head, 0);       // stored
        put16(head, dos_time);
        put16(head, dos_date);
        put32(head, crc);
        put32(head, uint32_t(f.data.size()));
        put32(head, uint32_t(f.data.size()));
        put16(head, uint32_t(f.name.size()));
        put16(head, 0);
        head += f.name;
        ok &= std::fwrite(head.data(), 1, head.size(), out) == head.size();
        if (!f.data.empty()) ok &= std::fwrite(f.data.data(), 1, f.data.size(), out) == f.data.size();
        put32(central, 0x02014B50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put16(central, dos_time);
        put16(central, dos_date);
        put32(central, crc);
        put32(central, uint32_t(f.data.size()));
        put32(central, uint32_t(f.data.size()));
        put16(central, uint32_t(f.name.size()));
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, 0);  // external attributes
        put32(central, offset);
        central += f.name;
        offset += uint32_t(head.size() + f.data.size());
    }
    std::string end;
    put32(end, 0x06054B50);
    put16(end, 0);
    put16(end, 0);
    put16(end, uint32_t(files.size()));
    put16(end, uint32_t(files.size()));
    put32(end, uint32_t(central.size()));
    put32(end, offset);
    put16(end, 0);
    ok &= std::fwrite(central.data(), 1, central.size(), out) == central.size();
    ok &= std::fwrite(end.data(), 1, end.size(), out) == end.size();
    ok &= std::fclose(out) == 0;
    return ok;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// "C:\dev\x\work\disc\PS3_GAME" -> "<path>\PS3_GAME": the folders of this PC are left out, the file's name kept.
std::string replace_paths(const std::string& text, const std::regex& re, const char* keep_prefix_group) {
    std::string out;
    size_t at = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        out.append(text, at, size_t(m.position(0)) - at);
        std::string prefix = keep_prefix_group ? m.str(1) : std::string();
        std::string path = m.str(keep_prefix_group ? 2 : 0);
        while (!path.empty() && (path.back() == '\\' || path.back() == '/' || path.back() == '.' || path.back() == ','))
            path.pop_back();
        const size_t slash = path.find_last_of("\\/");
        const std::string base = slash == std::string::npos ? std::string() : path.substr(slash + 1);
        static const std::string sep(1, char(std::filesystem::path::preferred_separator));  // (the platform's)
        out += prefix + "<path>" + (base.empty() ? "" : sep + base);
        out.append(m.str(keep_prefix_group ? 2 : 0), path.size(), std::string::npos);  // what was trimmed
        at = size_t(m.position(0) + m.length(0));
    }
    out.append(text, at, std::string::npos);
    return out;
}

// ---- The log: standard output and error through pipes, to where they went and to des.log ----

constexpr uint64_t kLogCap = 128ull << 20;   // past this the log keeps its first kLogKeep and goes on from there
constexpr uint64_t kLogKeep = 2ull << 20;

std::mutex g_log_lock;
FILE* g_log_file = nullptr;
uint64_t g_log_bytes = 0;
int g_tees = 0;
std::atomic<int> g_tee_idle{0};  // tee threads waiting for output with everything before it in the log file

void log_write(const char* data, size_t n) {
    std::lock_guard<std::mutex> lock(g_log_lock);
    if (g_log_file) {
        if (g_log_bytes + n > kLogCap) {
            static const char kNote[] =
                "\n[log] (the log passed 128 MB: what followed its first 2 MB was dropped; it goes on from here)\n";
            std::fflush(g_log_file);
            truncate_file(g_log_file, (long long)kLogKeep);
            seek_file(g_log_file, (long long)kLogKeep, SEEK_SET);
            std::fwrite(kNote, 1, sizeof kNote - 1, g_log_file);
            g_log_bytes = kLogKeep + sizeof kNote - 1;
        }
        std::fwrite(data, 1, n, g_log_file);
        g_log_bytes += n;
    }
}

void log_flush_now() {
    std::lock_guard<std::mutex> lock(g_log_lock);
    if (g_log_file) std::fflush(g_log_file);
}

#ifdef _WIN32
struct Tee {
    HANDLE read;
    HANDLE out;  // where the output was going (a copy of that handle), or null
};

// One per pipe: copies what the program prints to where it was going and into the log. The log file is buffered and
// flushed whenever the pipe has nothing more waiting, so a burst of messages costs one file write, not one per line.
DWORD WINAPI tee_thread(LPVOID arg) {
    Tee t = *static_cast<Tee*>(arg);
    delete static_cast<Tee*>(arg);
    std::vector<char> buf(1 << 16);
    bool idle = true;
    g_tee_idle.fetch_add(1);
    for (;;) {
        DWORD n = 0;
        const BOOL got = ReadFile(t.read, buf.data(), DWORD(buf.size()), &n, nullptr);
        if (idle) g_tee_idle.fetch_sub(1);
        idle = false;
        if (!got || n == 0) break;
        if (t.out) {
            const char* p = buf.data();
            for (DWORD left = n; left;) {
                DWORD w = 0;
                if (!WriteFile(t.out, p, left, &w, nullptr) || w == 0) {
                    t.out = nullptr;  // (that console or file is gone: the log goes on)
                    break;
                }
                p += w;
                left -= w;
            }
        }
        log_write(buf.data(), n);
        DWORD waiting = 0;
        if (PeekNamedPipe(t.read, nullptr, 0, nullptr, &waiting, nullptr) && waiting == 0) {
            log_flush_now();
            idle = true;  // (only this thread may look at the pipe: another's look waits for this thread's read)
            g_tee_idle.fetch_add(1);
        }
    }
    return 0;
}

bool valid(HANDLE h) {
    return h && h != INVALID_HANDLE_VALUE;
}

HANDLE dup_handle(HANDLE h) {
    HANDLE out = nullptr;
    if (!valid(h) ||
        !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return out;
}

// A pipe whose writing end takes over the C runtime's descriptors `fds` (1, 2 or both) and the matching standard
// handles; a thread copies what comes out of it to `original` and the log.
bool tee(std::initializer_list<int> fds, HANDLE original, int slot) {
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 1 << 20)) return false;  // (a burst of messages fits without waiting)
    // A copy of its own: the _dup2 calls below close the C runtime's descriptors, and with them the handle that was
    // there, whose number the pipe's end could then take (the tee would then read its own output, without end).
    HANDLE out = dup_handle(original);
    // Text mode, as the descriptors were: "\n" is still written as "\r\n" (play.sh's play.txt is unchanged).
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(write), _O_TEXT);
    if (fd < 0) {
        CloseHandle(read);
        CloseHandle(write);
        if (out) CloseHandle(out);
        return false;
    }
    for (int target : fds) {
        FILE* stream = target == 1 ? stdout : stderr;
        std::fflush(stream);
        _dup2(fd, target);
        SetStdHandle(target == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, write);
        // A program started without standard output has its streams closed (descriptor -2): printf would write
        // nothing even with the descriptor on the pipe. Open the stream (on NUL) so that it has a descriptor.
        if (_fileno(stream) < 0) std::freopen("NUL", "w", stream);
        if (_fileno(stream) >= 0 && _fileno(stream) != target) _dup2(fd, _fileno(stream));
        std::setvbuf(stream, nullptr, _IONBF, 0);
    }
    (void)slot;
    g_tees++;
    HANDLE thread = CreateThread(nullptr, 0, tee_thread, new Tee{read, out}, 0, nullptr);
    if (thread) CloseHandle(thread);
    return thread != nullptr;
}
#else
// One per pipe, as on Windows: what the program prints, to where it was going (a copy of that descriptor, or -1) and
// into the log, which is flushed whenever the pipe has nothing more waiting.
void tee_thread(int read_fd, int out) {
    std::vector<char> buf(1 << 16);
    bool idle = true;
    g_tee_idle.fetch_add(1);
    for (;;) {
        ssize_t n;
        do n = ::read(read_fd, buf.data(), buf.size());
        while (n < 0 && errno == EINTR);
        if (idle) g_tee_idle.fetch_sub(1);
        idle = false;
        if (n <= 0) break;
        for (const char* p = buf.data(); out >= 0 && p < buf.data() + n;) {
            const ssize_t w = ::write(out, p, size_t(buf.data() + n - p));
            if (w <= 0 && errno != EINTR) out = -1;  // (that terminal or file is gone: the log goes on)
            if (w > 0) p += w;
        }
        log_write(buf.data(), size_t(n));
        int waiting = 0;
        if (ioctl(read_fd, FIONREAD, &waiting) == 0 && waiting == 0) {
            log_flush_now();
            idle = true;
            g_tee_idle.fetch_add(1);
        }
    }
}

// A pipe that takes over descriptors `fds` (1, 2 or both); a thread copies what comes out of it to `original` (a
// descriptor of its own, or -1) and the log.
bool tee(std::initializer_list<int> fds, int original) {
    int p[2];
    if (pipe(p) != 0) {
        if (original >= 0) close(original);
        return false;
    }
    fcntl(p[1], F_SETPIPE_SZ, 1 << 20);  // (a burst of messages fits without waiting)
    for (int target : fds) {
        FILE* stream = target == 1 ? stdout : stderr;
        std::fflush(stream);
        dup2(p[1], target);
        std::setvbuf(stream, nullptr, _IONBF, 0);
    }
    close(p[1]);  // (the descriptors above hold it)
    g_tees++;
    std::thread(tee_thread, p[0], original).detach();
    return true;
}
#endif

void flush_at_exit() {
    flush_log();
}

// ---- Issue reports ----

std::atomic<bool> g_report_busy{false};

// The picture as a PNG, through stb_image_write. On the report's own thread.
std::vector<uint8_t> encode_png(const std::vector<uint8_t>& rgba, int width, int height) {
    std::vector<uint8_t> rgb(size_t(width) * height * 3), png;
    for (size_t i = 0, n = size_t(width) * height; i < n; i++) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    stbi_write_png_to_func(
        [](void* out, void* data, int size) {
            const auto* b = static_cast<const uint8_t*>(data);
            static_cast<std::vector<uint8_t>*>(out)->insert(static_cast<std::vector<uint8_t>*>(out)->end(), b, b + size);
        },
        &png, width, height, 3, rgb.data(), width * 3);
    return png;
}

// The crash and hang reports of the last week in the crashes folder, by name, newest first.
std::string recent_reports() {
    std::error_code ec;
    const auto dir = data_dir() / L"crashes";
    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != L".txt" || now - e.last_write_time(ec) > std::chrono::hours(24 * 7)) continue;
        found.emplace_back(e.last_write_time(ec), narrow(e.path().filename().wstring()));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out = "Crash and hang reports on this PC, the last 7 days: ";
    if (found.empty()) return out + "none\n";
    for (size_t i = 0; i < found.size() && i < 10; i++) out += (i ? ", " : "") + found[i].second;
    return out + (found.size() > 10 ? " and " + std::to_string(found.size() - 10) + " more\n" : "\n");
}

// The runtime's snapshot (ps3_diag_report) through a file of its own in the reports folder.
std::string runtime_snapshot() {
    std::error_code ec;
    const auto dir = data_dir() / L"reports";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (L"runtime-" + std::to_wstring(process_id()) + L".tmp");
    FILE* f = open_file(path, "w+");
    if (!f) return "(could not be written)\n";
    ps3_diag_report(f, 2);
    std::fclose(f);
    const auto b = read_file(path);
    std::filesystem::remove(path, ec);
    std::string text(b.begin(), b.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

void make_issue_report(std::vector<uint8_t> rgba, int width, int height, std::string session,
                       std::string settings_text) {
    begin_report_thread();
    const std::string stamp = timestamp();
    std::vector<File> files;
    if (!rgba.empty()) {
        if (auto png = encode_png(rgba, width, height); !png.empty()) files.push_back({"screenshot.png", std::move(png)});
    }
    const std::string system = redact(system_info());
    files.push_back({"system.txt", bytes(system)});
    const std::string game = redact(session + "Picture: " + std::to_string(width) + "x" + std::to_string(height) + "\n" +
                                    recent_reports());
    files.push_back({"game.txt", bytes(game)});
    files.push_back({"settings.ini", bytes(redact(settings_text))});
    files.push_back({"environment.txt", bytes(redact(environment_info()))});
    files.push_back({"runtime.txt", bytes(redact(runtime_snapshot()))});
    if (!g_log_path.empty()) {
        flush_log();
        files.push_back({"des.log", bytes(redact(read_head_tail(g_log_path, 120, 512 << 10)))});
        std::error_code ec;
        const auto prev = g_log_path.parent_path() / L"des.prev.log";
        if (std::filesystem::exists(prev, ec) &&
            std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(prev, ec) <
                std::chrono::hours(24))
            files.push_back({"des.prev.log", bytes(redact(read_head_tail(prev, 40, 192 << 10)))});
    }
    const auto zip = write_bundle("issue-" + stamp, files);
    std::string body = "## What happened\n(Please describe the problem and what you were doing.)\n\n## What you expected\n\n"
                       "## System\n```\n" + system + game + "```\n\n## Attachments\n";
    body += zip.empty() ? "(The report files could not be saved.)\n"
                        : "Drag `" + narrow(zip.filename().wstring()) + "` (its folder is open) into this issue: it has "
                          "a screenshot, your settings, what each part of the game was doing and the game's recent "
                          "messages.\n";
    std::fprintf(stderr, "[report] issue report: %s\n", zip.empty() ? "could not be saved" : narrow(zip.wstring()).c_str());
    open_issue(issue_url("", body), zip);
    end_report_thread();
    g_report_busy.store(false);
}

std::filesystem::path exe_dir() { return exe_path().parent_path(); }

}  // namespace

std::filesystem::path program_dir() { return exe_dir(); }
std::filesystem::path program_path() { return exe_path(); }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring w(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), size);
    return w;
}

std::string narrow(const std::wstring& w) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string s(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), size, nullptr, nullptr);
    return s;
}

FILE* open_file(const std::filesystem::path& path, const char* mode) {
    return _wfopen(path.c_str(), widen(mode).c_str());
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
#else
// UTF-8 and wchar_t's UTF-32.
std::wstring widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned c = (unsigned char)s[i++];
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= more == 3 ? 0x07u : more == 2 ? 0x0Fu : more == 1 ? 0x1Fu : 0x7Fu;
        while (more-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) c = (c << 6) | ((unsigned char)s[i++] & 0x3Fu);
        w += wchar_t(c);
    }
    return w;
}

std::string narrow(const std::wstring& w) {
    std::u16string u;
    for (wchar_t c : w) {
        if (unsigned(c) >= 0x10000) {
            u += char16_t(0xD800 + ((unsigned(c) - 0x10000) >> 10));
            u += char16_t(0xDC00 + ((unsigned(c) - 0x10000) & 0x3FF));
        } else {
            u += char16_t(c);
        }
    }
    return utf16_to_utf8(u);
}

FILE* open_file(const std::filesystem::path& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    return std::rename(from.c_str(), to.c_str()) == 0;
}
#endif

const std::filesystem::path& data_dir() {
    if (g_data_dir_resolved) return g_data_dir;
    g_data_dir_resolved = true;
    std::error_code ec;
    if (const char* given = std::getenv("DES_DATA_DIR"); given && *given) {
        g_data_dir = std::filesystem::path(widen(given));
    } else if (settings::harness_run()) {
        g_data_dir.clear();  // test runs keep nothing
    } else if (const auto dir = exe_dir(); !dir.empty() && std::filesystem::exists(dir / L"portable.txt", ec)) {
        g_data_dir = dir / L"data";
    } else {
        g_data_dir = pref_dir();
    }
    return g_data_dir;
}

void set_data_dir(const std::filesystem::path& dir) {
    g_data_dir = dir;
    g_data_dir_resolved = true;
}

std::filesystem::path log_path() { return g_log_path; }

std::string version() { return kVersion; }

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ui::local_time(t, tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

namespace {
#ifdef _WIN32
void redact_home(std::string& text) {
    const std::string home = env("USERPROFILE");
    if (!home.empty()) {
        std::string slashed = home;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        replace_all(text, home, "%USERPROFILE%");
        replace_all(text, slashed, "%USERPROFILE%");
    }
}
#else
void redact_home(std::string& text) {
    if (const std::string home = env("HOME"); home.size() > 1) replace_all(text, home, "~");
}
#endif
}  // namespace

std::string redact(std::string text) {
    redact_home(text);
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    text = std::regex_replace(text, email, "<e-mail>");
    // Folders of this PC: C:\... or C:/... and UNC \\server\..., and Git Bash's /c/... (only the last name is kept).
    // (not "https://": a drive letter is not preceded by a letter)
    static const std::regex win_path(R"((^|[^A-Za-z0-9])((?:[A-Za-z]:[\\/]|\\\\[A-Za-z0-9]|%USERPROFILE%[\\/])(?:[^\s"'<>|*?]|'(?=[A-Za-z]))*))");
    text = replace_paths(text, win_path, "1");
    static const std::regex msys_path(R"((^|[\s"'=(])(/[a-zA-Z]/(?:[^\s"'<>|*?]|'(?=[A-Za-z]))*))");
    text = replace_paths(text, msys_path, "1");
#ifndef _WIN32
    // The home folder (~) and the usual places of a Linux system's own folders.
    static const std::regex unix_path(
        R"((^|[\s"'=(])((?:~|/(?:home|root|mnt|media|tmp|run|var|opt|srv|usr))/(?:[^\s"'<>|*?]|'(?=[A-Za-z]))*))");
    text = replace_paths(text, unix_path, "1");
#endif
    // (after the folders: a name inside a path goes with its path)
    replace_all(text, psn_name(), "<psn name>", true);
    replace_all(text, online_name(), "<online name>", true);
    replace_all(text, user_name(), "<user>", true);
    replace_all(text, pc_name(), "<pc>", true);
    return text;
}

namespace {

#ifdef _WIN32
// Is HDR on for the display `mon` (DXGI's output description)?
bool hdr_on(HMONITOR mon) {
    bool on = false;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !on && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput* output = nullptr;
        for (UINT k = 0; !on && adapter->EnumOutputs(k, &output) != DXGI_ERROR_NOT_FOUND; k++) {
            IDXGIOutput6* o6 = nullptr;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&o6)))) {
                if (SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == mon)
                    on = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                o6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return on;
}

// The software loaded into the game that is neither Windows' nor the game's: overlays, recorders, injectors (their
// file names; "beside the game" for one in the game's folder, such as a ReShade dxgi.dll).
std::string other_modules() {
    HMODULE mods[1024];
    DWORD bytes_needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes_needed)) return {};
    wchar_t windir[MAX_PATH] = L"";
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::wstring win = std::filesystem::path(windir).wstring(), game = exe_dir().wstring();
    const HMODULE self = GetModuleHandleW(nullptr);
    const auto under = [](const std::wstring& path, const std::wstring& dir) {
        return !dir.empty() && path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0 &&
               (path[dir.size()] == L'\\' || path[dir.size()] == L'/');
    };
    std::vector<std::string> names;
    for (DWORD i = 0; i < std::min<DWORD>(bytes_needed / sizeof(HMODULE), 1024); i++) {
        if (mods[i] == self) continue;
        wchar_t path[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(mods[i], path, DWORD(std::size(path)));
        if (!n) continue;
        const std::wstring p(path, n);
        if (under(p, win)) continue;
        const std::wstring dir = std::filesystem::path(p).parent_path().wstring();
        const bool beside = _wcsicmp(dir.c_str(), game.c_str()) == 0;
        names.push_back(narrow(std::filesystem::path(p).filename().wstring()) + (beside ? " (beside the game)" : ""));
    }
    std::sort(names.begin(), names.end());
    std::string out = "Other software in the game (" + std::to_string(names.size()) + "):";
    for (const auto& name : names) out += " " + name + ";";
    if (!names.empty()) out.pop_back();
    return out + "\n";
}

// The OS, the CPU, the memory and the displays; on Windows also the gamepads and the power, and in the game's
// own process the other software loaded into it.
void platform_facts(std::ostringstream& o, bool game_process) {
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")))
            fn(&v);
        // "Windows 11 Pro 24H2" (the registry's product name says Windows 10 on 11: the build tells them apart)
        const auto reg = [](const char* name) {
            char buf[128] = "";
            DWORD size = sizeof buf;
            RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ,
                         nullptr, buf, &size);
            return std::string(buf);
        };
        std::string product = reg("ProductName");
        if (v.dwBuildNumber >= 22000 && product.rfind("Windows 10", 0) == 0) product.replace(0, 10, "Windows 11");
        DWORD ubr = 0, size = sizeof ubr;
        RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD,
                     nullptr, &ubr, &size);
        o << "OS: " << (product.empty() ? "Windows" : product) << " " << reg("DisplayVersion") << ", "
          << v.dwMajorVersion << "." << v.dwMinorVersion << " build " << v.dwBuildNumber << "." << ubr << "\n";
    }
    {
        char cpu[128] = "?";
        DWORD size = sizeof cpu;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &size);
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        // Cores, and on a hybrid processor how many of each kind (the highest efficiency class is the fast cores).
        int cores = 0, fast = 0, top = -1;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> buf(len);
        if (len && GetLogicalProcessorInformationEx(
                       RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
            for (DWORD at = 0; at < len;) {
                const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
                const int cls = p->Processor.EfficiencyClass;
                cores++;
                if (cls > top) top = cls, fast = 0;
                if (cls == top) fast++;
                at += p->Size;
            }
        }
        int r[4];
        __cpuid(r, 1);
        const bool sse42 = r[2] & (1 << 20), avx = r[2] & (1 << 28), fma = r[2] & (1 << 12);
        __cpuidex(r, 7, 0);
        const bool avx2 = r[1] & (1 << 5), avx512 = r[1] & (1 << 16), bmi2 = r[1] & (1 << 8);
        o << "CPU: " << cpu << " (" << cores << " cores";
        if (fast && fast < cores) o << ": " << fast << " fast, " << (cores - fast) << " efficient";
        o << ", " << si.dwNumberOfProcessors << " threads; " << (sse42 ? "SSE4.2" : "no SSE4.2") << (avx ? " AVX" : "")
          << (avx2 ? " AVX2" : "") << (fma ? " FMA" : "") << (bmi2 ? " BMI2" : "") << (avx512 ? " AVX-512" : "") << ")\n";
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms))
            o << "Memory: " << (ms.ullTotalPhys >> 20) << " MB, " << (ms.ullAvailPhys >> 20) << " MB free now; commit limit "
              << (ms.ullTotalPageFile >> 20) << " MB, " << (ms.ullAvailPageFile >> 20) << " MB free\n";
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        if (game_process &&
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm))
            o << "This process: " << (pm.WorkingSetSize >> 20) << " MB in memory (peak " << (pm.PeakWorkingSetSize >> 20)
              << " MB), " << (pm.PrivateUsage >> 20) << " MB committed\n";
    }
    {
        // The adapter the game draws with (the D3D12 device's), with its memory budget and this process's use.
        LUID used{};
        bool have_used = false;
        if (auto* device = static_cast<ID3D12Device*>(rsx_d3d12_device())) {
            used = device->GetAdapterLuid();
            have_used = true;
        }
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    o << "GPU: " << narrow(d.Description) << ", " << (d.DedicatedVideoMemory >> 20) << " MB";
                    LARGE_INTEGER driver{};
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
                        o << ", driver " << HIWORD(driver.HighPart) << "." << LOWORD(driver.HighPart) << "."
                          << HIWORD(driver.LowPart) << "." << LOWORD(driver.LowPart);
                    if (have_used && d.AdapterLuid.LowPart == used.LowPart && d.AdapterLuid.HighPart == used.HighPart) {
                        o << " (in use";
                        IDXGIAdapter3* a3 = nullptr;
                        DXGI_QUERY_VIDEO_MEMORY_INFO mem{};
                        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a3)))) {
                            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
                                o << ": this process uses " << (mem.CurrentUsage >> 20) << " MB of a " << (mem.Budget >> 20)
                                  << " MB budget";
                            a3->Release();
                        }
                        o << ")";
                    }
                    o << "\n";
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
    {
        struct Monitors {
            std::ostringstream* o;
            int n = 0;
        } m{&o};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
                auto& m = *reinterpret_cast<Monitors*>(p);
                MONITORINFOEXW info{};
                info.cbSize = sizeof info;
                if (!GetMonitorInfoW(mon, &info)) return TRUE;
                DEVMODEW mode{};
                mode.dmSize = sizeof mode;
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
                *m.o << "Display " << ++m.n << ": " << (info.rcMonitor.right - info.rcMonitor.left) << "x"
                     << (info.rcMonitor.bottom - info.rcMonitor.top) << " at " << mode.dmDisplayFrequency << " Hz"
                     << ((info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << (hdr_on(mon) ? ", HDR on" : "")
                     << "\n";
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&m));
    }
    {
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) && power.BatteryFlag != 128 && power.BatteryFlag != 255)
            o << "Power: " << (power.ACLineStatus == 1 ? "plugged in" : power.ACLineStatus == 0 ? "on battery" : "unknown")
              << ", battery " << int(power.BatteryLifePercent) << "%" << (power.SystemStatusFlag ? ", battery saver on" : "")
              << "\n";
        else
            o << "Power: no battery\n";
    }
    {
        const auto free_on = [](const std::filesystem::path& p) -> std::string {
            ULARGE_INTEGER avail{};
            if (p.empty() || !GetDiskFreeSpaceExW(p.root_path().c_str(), &avail, nullptr, nullptr)) return "?";
            return std::to_string(avail.QuadPart >> 30) + " GB";
        };
        o << "Free disk space: " << free_on(exe_dir()) << " where the game is, " << free_on(data_dir())
          << " where its data is\n";
    }
    {
        // Gamepads: XInput slots that answer, and the DualSense / DualShock 4 reader's pads.
        int xinput = 0;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS) xinput++;
        }
        uint8_t hid[4][16];
        const int sony = pad_hid_sony_poll(hid, 4);
        char layout[KL_NAMELENGTH] = "";
        GetKeyboardLayoutNameA(layout);
        o << "Gamepads: " << xinput << " XInput, " << sony << " DualSense or DualShock 4; keyboard layout " << layout << "\n";
    }
    if (game_process) o << other_modules();
}
#else
void platform_facts(std::ostringstream& o, bool) {
    struct utsname u {};
    if (uname(&u) == 0) o << "OS: " << u.sysname << " " << u.release << " " << u.machine << "\n";
    std::ifstream cpuinfo("/proc/cpuinfo"), meminfo("/proc/meminfo");
    std::string cpu = "?";
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            cpu = line.substr(line.find(':') + 2);
            break;
        }
    }
    o << "CPU: " << cpu << " (" << sysconf(_SC_NPROCESSORS_ONLN) << " threads)\n";
    for (std::string line; std::getline(meminfo, line);) {
        if (line.rfind("MemTotal:", 0) == 0) {
            o << "Memory: " << (std::strtoull(line.c_str() + 9, nullptr, 10) >> 10) << " MB\n";
            break;
        }
    }
    o << "GPU: in the log ([D3D12] vkd3d: ...)\n";
    for (int i = 0, n = SDL_GetNumVideoDisplays(); i < n; i++) {
        SDL_DisplayMode m{};
        if (SDL_GetCurrentDisplayMode(i, &m) == 0)
            o << "Display " << i + 1 << ": " << m.w << "x" << m.h << " at " << m.refresh_rate << " Hz\n";
    }
}
#endif

}  // namespace

std::string system_info() {
    std::ostringstream o;
    // The game's own process (its renderer is up), not the crash dialog's: only then are this process's memory and
    // the other software loaded into it the game's.
    const bool game_process = rsx_d3d12_device() != nullptr;
#if DES_DEV
    o << "Developer build (cheats compiled in)\n";
#endif
    o << "DemonsSoulsRecomp " << version() << "\n";
    o << "ps3recomp: " << kPs3recompVersion << "\n";
    o << "Game dump (EBOOT.ELF SHA-256): " << (*kDumpSha256 ? kDumpSha256 : "unknown") << "\n";
    platform_facts(o, game_process);
    return o.str();
}

std::string environment_info(const std::string& command_line) {
    std::string out = "Command line: " + (command_line.empty() ? command_line_text() : command_line) + "\n";
    static const char* kPrefixes[] = {"DES_", "PS3_", "RSX_", "SPU_", "SPURS_", "GCM_", "PAD_", "PPU_", "AUDIO_",
                                      "VBLANK", "FIFO_", "LIBSRE_", "SAIL_", "TROPHY_", "WATCHDOG_"};
    static const char* kHidden[] = {"DES_NET_NAME", "DES_NET_PASSWORD", "DES_NET_CUSTOM", "PS3_NP_ONLINE_ID",
                                    "PS3_NET_PASSWORD"};
    std::vector<std::string> lines;
    for (const std::string& kv : environment_strings()) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = kv.substr(0, eq);
        bool ours = false;
        for (const char* prefix : kPrefixes) ours = ours || key.rfind(prefix, 0) == 0;
        if (!ours) continue;
        bool hide = false;
        for (const char* h : kHidden) hide = hide || key == h;
        lines.push_back(hide ? key + "=(set)" : kv);
    }
    std::sort(lines.begin(), lines.end());
    out += "Options set in the environment (" + std::to_string(lines.size()) + "):\n";
    for (const auto& l : lines) out += "  " + l + "\n";
    return out;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::vector<uint8_t> out;
    FILE* f = open_file(path, "rb");
    if (!f) return out;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

std::string read_head_tail(const std::filesystem::path& path, size_t head, size_t tail_bytes) {
    FILE* f = open_file(path, "rb");  // (the game may still be writing it)
    if (!f) return {};
    seek_file(f, 0, SEEK_END);
    const long long size = tell_file(f);
    seek_file(f, 0, SEEK_SET);
    std::string text;
    // The first `head` lines (from the first 64 KB).
    {
        std::string start(size_t(std::min<long long>(size, 64 << 10)), '\0');
        start.resize(std::fread(start.data(), 1, start.size(), f));
        size_t at = 0;
        for (size_t i = 0; i < head && at < start.size(); i++) {
            const size_t e = start.find('\n', at);
            at = e == std::string::npos ? start.size() : e + 1;
        }
        text = start.substr(0, at);
    }
    const long long head_end = (long long)text.size();
    long long from = std::max(head_end, size - (long long)tail_bytes);
    if (from < size) {
        seek_file(f, from, SEEK_SET);
        std::string tail(size_t(size - from), '\0');
        tail.resize(std::fread(tail.data(), 1, tail.size(), f));
        if (from > head_end) {
            const size_t nl = tail.find('\n');  // (from the start of a line)
            tail.erase(0, nl == std::string::npos ? 0 : nl + 1);
            text += "[... " + std::to_string(size - head_end - (long long)tail.size()) + " bytes left out ...]\n";
        }
        text += tail;
    }
    std::fclose(f);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::filesystem::path write_bundle(const std::string& stem, const std::vector<File>& files) {
    if (data_dir().empty()) return {};
    std::error_code ec;
    const auto dir = data_dir() / L"reports";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / widen(stem + ".zip");
    return write_zip(path, files) ? path : std::filesystem::path{};
}

std::string issue_url(const std::string& title, const std::string& body) {
    // (GitHub refuses very long links, and browsers cut them: keep the encoded text under about 7000 bytes)
    std::string text = body;
    while (url_encode(text).size() > 6500 && text.size() > 200) {
        text.resize(text.size() * 9 / 10);
        text += "\n[...cut: the attached report has the rest]";
    }
    return std::string(kRepositoryUrl) + "/issues/new?title=" + url_encode(title) + "&body=" + url_encode(text);
}

#ifdef _WIN32
void open_url(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void reveal(const std::filesystem::path& path) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
#else
// xdg-open, the desktop's "open this" (a browser for a link, the file manager for a folder).
void open_url(const std::string& url) {
    char prog[] = "xdg-open";
    std::string arg = url;
    char* argv[] = {prog, arg.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, prog, nullptr, nullptr, argv, environ) != 0)
        std::fprintf(stderr, "[report] could not run xdg-open for %s\n", url.c_str());
}

void reveal(const std::filesystem::path& path) {
    open_url(path.parent_path().string());  // (the folder: xdg-open cannot select a file in it)
}
#endif

void open_issue(const std::string& url, const std::filesystem::path& zip) {
    if (env_on("DES_REPORT_NO_OPEN")) {
        auto where = zip.empty() ? data_dir() / L"reports" / L"last-issue.zip" : zip;
        where += L".url.txt";
        if (FILE* f = open_file(where, "wb")) {
            std::fwrite(url.data(), 1, url.size(), f);
            std::fclose(f);
        }
        std::fprintf(stderr, "[report] DES_REPORT_NO_OPEN: the issue link is in %s\n", narrow(where.wstring()).c_str());
        return;
    }
    open_url(url);
    if (!zip.empty()) reveal(zip);
}

#ifndef _WIN32
void start_log() {
    if (data_dir().empty()) return;
    std::error_code ec;
    const auto dir = data_dir() / "logs";
    std::filesystem::create_directories(dir, ec);
    auto path = dir / "des.log";
    // Another copy of the game running at the same time holds a lock on des.log: this one then writes its own, which
    // its reports quote (those of finished copies are cleared away after a day). Otherwise des.log starts over and
    // the previous run's is kept as des.prev.log.
    const auto locked = [](const std::filesystem::path& p) {
        const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool busy = flock(fd, LOCK_SH | LOCK_NB) != 0;
        close(fd);
        return busy;
    };
    if (locked(path)) {
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("des.", 0) == 0 && name != "des.log" && name != "des.prev.log" &&
                now - e.last_write_time(ec) > std::chrono::hours(24))
                std::filesystem::remove(e.path(), ec);
        }
        path = dir / ("des." + std::to_string(getpid()) + ".log");
    } else {
        std::filesystem::rename(path, dir / "des.prev.log", ec);
    }
    g_log_file = std::fopen(path.c_str(), "wb");
    if (!g_log_file) return;
    flock(fileno(g_log_file), LOCK_EX | LOCK_NB);
    std::setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);

    // The runtime's chatty per-event messages are on when standard error is not a terminal (ps3_log.h: "someone is
    // capturing a log"). Standard error is a pipe from here on: keep what it decided on the terminal (quiet).
    if (!std::getenv("PS3_VERBOSE") && isatty(2)) ps3_setenv("PS3_VERBOSE", "0");
    // One pipe for both when they went to the same place (a terminal, or play.sh's file): the log then keeps their
    // order. Otherwise one each, so that each still goes where it went.
    struct stat so {}, se {};
    const bool same = (fstat(1, &so) == 0 && fstat(2, &se) == 0 && so.st_dev == se.st_dev && so.st_ino == se.st_ino) ||
                      (isatty(1) && isatty(2));
    bool ok;
    if (same) ok = tee({1, 2}, dup(2));
    else ok = tee({2}, dup(2)) && tee({1}, dup(1));
    if (!ok) return;
    g_log_path = path;
    std::atexit(flush_at_exit);
}

void flush_log() {
    if (!g_log_file) return;
    // As on Windows: a moment for the tee threads to wake up for what was just printed, then until they have written
    // it all, never forever.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (int i = 0; i < 50 && g_tee_idle.load() < g_tees; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (int i = 0; i < 20; i++) {
        if (g_log_lock.try_lock()) {
            std::fflush(g_log_file);
            g_log_lock.unlock();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#else
void start_log() {
    if (data_dir().empty()) return;
    std::error_code ec;
    const auto dir = data_dir() / L"logs";
    std::filesystem::create_directories(dir, ec);
    auto path = dir / L"des.log";
    std::filesystem::rename(path, dir / L"des.prev.log", ec);
    g_log_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!g_log_file) {
        // Another copy of the game running at the same time keeps des.log: this one writes its own, which its reports
        // quote. (Those of finished copies are cleared away after a day.)
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::wstring name = e.path().filename().wstring();
            if (name.rfind(L"des.", 0) == 0 && name != L"des.log" && name != L"des.prev.log" &&
                now - e.last_write_time(ec) > std::chrono::hours(24))
                std::filesystem::remove(e.path(), ec);
        }
        path = dir / (L"des." + std::to_wstring(GetCurrentProcessId()) + L".log");
        g_log_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    }
    if (!g_log_file) return;
    std::setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);

    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    // The runtime's chatty per-event messages are on when standard error is not a console (ps3_log.h: "someone is
    // capturing a log"). Standard error is a pipe from here on: keep what it decided on the console (quiet).
    if (!std::getenv("PS3_VERBOSE") && (!valid(err) || _isatty(2))) ps3_setenv("PS3_VERBOSE", "0");
    // One pipe for both when they went to the same place (a console, or play.sh's file): the log then keeps their
    // order. Otherwise one each, so that each still goes where it went.
    using CompareFn = BOOL(WINAPI*)(HANDLE, HANDLE);  // (Windows 10 and later)
    const auto compare =
        reinterpret_cast<CompareFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
    const bool same = (valid(out) && valid(err) && (out == err || (compare && compare(out, err)))) ||
                      (GetFileType(out) == FILE_TYPE_CHAR && GetFileType(err) == FILE_TYPE_CHAR);
    bool ok;
    if (same) ok = tee({1, 2}, err, 0);
    else ok = tee({2}, err, 0) && tee({1}, out, 1);
    if (!ok) return;
    g_log_path = path;
    std::atexit(flush_at_exit);
}

void flush_log() {
    if (!g_log_file) return;
    // A moment for the tee threads to wake up for what was just printed, then until they have written it all (they
    // say so when their pipe is empty). Not forever: the game's other threads may go on printing.
    Sleep(30);
    for (int i = 0; i < 50 && g_tee_idle.load() < g_tees; i++) Sleep(10);
    for (int i = 0; i < 20; i++) {  // (not forever: the thread holding it may be the one that crashed)
        if (g_log_lock.try_lock()) {
            std::fflush(g_log_file);
            g_log_lock.unlock();
            return;
        }
        Sleep(10);
    }
}
#endif

namespace {
volatile uintptr_t g_nowhere = 0;  // (a null pointer the compiler cannot see)

// A test's stop: this thread sleeps for good.
[[noreturn]] void stop_here() {
    for (;;) std::this_thread::sleep_for(std::chrono::hours(24));
}
}  // namespace

// A frame count from the environment, read again every 64th call (the console's setenv can set it in a running game).
unsigned long long env_u64_live(const char* name, unsigned& calls, unsigned long long& value) {
    if ((calls++ & 63) == 0) value = env_u64(name);
    return value;
}

std::atomic<std::thread::id> g_render_thread{};  // frame_hook's thread: flip_hook leaves it alone

bool frame_hook(unsigned long long frame) {
    g_render_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    static const unsigned long long crash_at = env_u64("DES_CRASH_AT_FRAME"), abort_at = env_u64("DES_ABORT_AT_FRAME");
    static unsigned calls = 0, report_calls = 0;
    static unsigned long long hang_at = 0, report_at = 0, reported_at = 0;
    if (const auto at = env_u64_live("DES_HANG_RENDER_AT_FRAME", calls, hang_at); at && frame >= at) {
        std::fprintf(stderr, "[des] DES_HANG_RENDER_AT_FRAME=%llu: the render thread stops here (test)\n", at);
        stop_here();
    }
    if (frame == crash_at && crash_at) {
        std::fprintf(stderr, "[des] DES_CRASH_AT_FRAME=%llu: crashing on purpose (test)\n", frame);
        *reinterpret_cast<volatile int*>(g_nowhere) = 1;
    }
    if (frame == abort_at && abort_at) {
        std::fprintf(stderr, "[des] DES_ABORT_AT_FRAME=%llu: aborting on purpose (test)\n", frame);
        std::abort();
    }
    // (once per value: the console's setenv DES_REPORT_AT_FRAME=1 asks for one report now)
    if (const auto at = env_u64_live("DES_REPORT_AT_FRAME", report_calls, report_at); at && frame >= at && at != reported_at) {
        reported_at = at;
        return true;
    }
    return false;
}

void flip_hook() {
    static unsigned calls = 0;
    static unsigned long long hang_at = 0;
    // (the render thread decodes some flips from the FIFO: stopping it would be DES_HANG_RENDER_AT_FRAME's case)
    if (const auto at = env_u64_live("DES_HANG_AT_FRAME", calls, hang_at);
        at && ppu_boot_frames_presented() >= at &&
        std::this_thread::get_id() != g_render_thread.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "[des] DES_HANG_AT_FRAME=%llu: the game stops here (test)\n", at);
        stop_here();
    }
}

void send_issue_report(void* back_buffer, unsigned index, int width, int height, std::string session) {
    if (data_dir().empty()) {
        std::fprintf(stderr, "[report] no data folder (a test run without DES_DATA_DIR): no issue report\n");
        return;
    }
    if (g_report_busy.exchange(true)) return;  // (one at a time)
    std::vector<uint8_t> rgba;
    if (!host::capture(back_buffer, index, width, height, rgba)) rgba.clear();
    std::thread(make_issue_report, std::move(rgba), width, height, std::move(session), settings::describe()).detach();
}

}  // namespace ui::report

extern "C" void des_report_flip_hook(void) { ui::report::flip_hook(); }
