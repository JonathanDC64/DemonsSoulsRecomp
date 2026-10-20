// Problem reports (see report.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's report.cpp (the zip writer, the
// redaction, the system facts, the issue link and the log tee); the data folder follows FFCCEOT-Recomp's main.cpp.
#include "report.hpp"

#include "host.hpp"
#include "settings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// (after windows.h)
#include <dxgi.h>
#include <fcntl.h>
#include <io.h>
#include <objbase.h>
#include <shellapi.h>
#include <share.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <thread>

#ifndef DES_VERSION
#define DES_VERSION "dev"
#endif

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache

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

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
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

// The PSN name given on the command line (--username NAME or --username=NAME), if any.
std::string psn_name() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string name;
    for (int i = 1; argv && i < argc; i++) {
        const std::wstring a = argv[i];
        if (a.rfind(L"--username=", 0) == 0) name = narrow(a.substr(11));
        else if (a == L"--username" && i + 1 < argc) name = narrow(argv[i + 1]);
    }
    if (argv) LocalFree(argv);
    return name;
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
    localtime_s(&tm, &t);
    const uint32_t dos_time = uint32_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const uint32_t dos_date = uint32_t(std::max(0, tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    FILE* out = _wfopen(path.c_str(), L"wb");
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
        out += prefix + "<path>" + (base.empty() ? "" : "\\" + base);
        out.append(m.str(keep_prefix_group ? 2 : 0), path.size(), std::string::npos);  // what was trimmed
        at = size_t(m.position(0) + m.length(0));
    }
    out.append(text, at, std::string::npos);
    return out;
}

// ---- The log: standard output and error through pipes, to where they went and to des.log ----

constexpr uint64_t kLogCap = 128ull << 20;   // past this the log keeps its first kLogKeep and goes on from there
constexpr uint64_t kLogKeep = 2ull << 20;

SRWLOCK g_log_lock = SRWLOCK_INIT;
FILE* g_log_file = nullptr;
uint64_t g_log_bytes = 0;
int g_tees = 0;
std::atomic<int> g_tee_idle{0};  // tee threads waiting for output with everything before it in the log file

void log_write(const char* data, size_t n) {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file) {
        if (g_log_bytes + n > kLogCap) {
            static const char kNote[] =
                "\n[log] (the log passed 128 MB: what followed its first 2 MB was dropped; it goes on from here)\n";
            std::fflush(g_log_file);
            _chsize_s(_fileno(g_log_file), __int64(kLogKeep));
            _fseeki64(g_log_file, __int64(kLogKeep), SEEK_SET);
            std::fwrite(kNote, 1, sizeof kNote - 1, g_log_file);
            g_log_bytes = kLogKeep + sizeof kNote - 1;
        }
        std::fwrite(data, 1, n, g_log_file);
        g_log_bytes += n;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void log_flush_now() {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file) std::fflush(g_log_file);
    ReleaseSRWLockExclusive(&g_log_lock);
}

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

void flush_at_exit() {
    flush_log();
}

// ---- Issue reports ----

std::atomic<bool> g_report_busy{false};

// The picture as a PNG, through the Windows Imaging Component (part of Windows). On the report's own thread.
std::vector<uint8_t> encode_png(const std::vector<uint8_t>& rgba, int width, int height) {
    std::vector<uint8_t> png;
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(UINT(width), UINT(height)))) {
        // (falls through to the release below)
    } else {
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
            const UINT stride = UINT(width) * 3;
            std::vector<uint8_t> bgr(size_t(stride) * height);
            for (size_t i = 0, n = size_t(width) * height; i < n; i++) {
                bgr[i * 3 + 0] = rgba[i * 4 + 2];
                bgr[i * 3 + 1] = rgba[i * 4 + 1];
                bgr[i * 3 + 2] = rgba[i * 4 + 0];
            }
            HGLOBAL global = nullptr;
            if (SUCCEEDED(frame->WritePixels(UINT(height), stride, UINT(bgr.size()), bgr.data())) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()) &&
                SUCCEEDED(GetHGlobalFromStream(stream, &global))) {
                STATSTG stat{};
                stream->Stat(&stat, STATFLAG_NONAME);
                if (const void* p = GlobalLock(global)) {
                    const auto* b = static_cast<const uint8_t*>(p);
                    png.assign(b, b + size_t(stat.cbSize.QuadPart));
                    GlobalUnlock(global);
                }
            }
        }
    }
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return png;
}

void make_issue_report(std::vector<uint8_t> rgba, int width, int height, unsigned long long frames,
                       std::string settings_text) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // (WIC, ShellExecute)
    const std::string stamp = timestamp();
    std::vector<File> files;
    if (!rgba.empty()) {
        if (auto png = encode_png(rgba, width, height); !png.empty()) files.push_back({"screenshot.png", std::move(png)});
    }
    const std::string system = redact(system_info());
    files.push_back({"system.txt", bytes(system)});
    const std::string game = "Presented frames: " + std::to_string(frames) + "\nPicture: " + std::to_string(width) +
                             "x" + std::to_string(height) + "\n";
    files.push_back({"game.txt", bytes(game)});
    files.push_back({"settings.ini", bytes(redact(settings_text))});
    if (!g_log_path.empty()) {
        flush_log();
        files.push_back({"des.log", bytes(redact(read_head_tail(g_log_path, 80, 192 << 10)))});
    }
    const auto zip = write_bundle("issue-" + stamp, files);
    std::string body = "## What happened\n(Please describe the problem and what you were doing.)\n\n## What you expected\n\n"
                       "## System\n```\n" + system + game + "```\n\n## Attachments\n";
    body += zip.empty() ? "(The report files could not be saved.)\n"
                        : "Drag `" + narrow(zip.filename().wstring()) + "` (its folder is open) into this issue: it has "
                          "a screenshot, the settings and the game's recent messages.\n";
    std::fprintf(stderr, "[report] issue report: %s\n", zip.empty() ? "could not be saved" : narrow(zip.wstring()).c_str());
    open_issue(issue_url("", body), zip);
    CoUninitialize();
    g_report_busy.store(false);
}

}  // namespace

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

std::string version() { return DES_VERSION; }

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string redact(std::string text) {
    const std::string home = env("USERPROFILE");
    if (!home.empty()) {
        std::string slashed = home;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        replace_all(text, home, "%USERPROFILE%");
        replace_all(text, slashed, "%USERPROFILE%");
    }
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    text = std::regex_replace(text, email, "<e-mail>");
    // Folders of this PC: C:\... or C:/... and UNC \\server\..., and Git Bash's /c/... (only the last name is kept).
    // (not "https://": a drive letter is not preceded by a letter)
    static const std::regex win_path(R"((^|[^A-Za-z0-9])((?:[A-Za-z]:[\\/]|\\\\[A-Za-z0-9]|%USERPROFILE%[\\/])(?:[^\s"'<>|*?]|'(?=[A-Za-z]))*))");
    text = replace_paths(text, win_path, "1");
    static const std::regex msys_path(R"((^|[\s"'=(])(/[a-zA-Z]/(?:[^\s"'<>|*?]|'(?=[A-Za-z]))*))");
    text = replace_paths(text, msys_path, "1");
    // (after the folders: a name inside a path goes with its path)
    replace_all(text, psn_name(), "<psn name>", true);
    replace_all(text, env("USERNAME"), "<user>", true);
    replace_all(text, env("COMPUTERNAME"), "<pc>", true);
    return text;
}

std::string system_info() {
    std::ostringstream o;
#if DES_DEV
    o << "Developer build (cheats compiled in)\n";
#endif
    o << "DemonsSoulsRecomp " << version() << "\n";
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")))
            fn(&v);
        o << "OS: Windows " << v.dwMajorVersion << "." << v.dwMinorVersion << " build " << v.dwBuildNumber << "\n";
    }
    {
        char cpu[128] = "?";
        DWORD size = sizeof cpu;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &size);
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        o << "CPU: " << cpu << " (" << si.dwNumberOfProcessors << " threads)\n";
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) o << "Memory: " << (ms.ullTotalPhys >> 20) << " MB\n";
    }
    {
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
                     << ((info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&m));
    }
    return o.str();
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::vector<uint8_t> out;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return out;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

std::string read_head_tail(const std::filesystem::path& path, size_t head, size_t tail_bytes) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);  // (the game may still be writing it)
    if (!f) return {};
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
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
        _fseeki64(f, from, SEEK_SET);
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

void open_url(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void reveal(const std::filesystem::path& path) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void open_issue(const std::string& url, const std::filesystem::path& zip) {
    if (env_on("DES_REPORT_NO_OPEN")) {
        auto where = zip.empty() ? data_dir() / L"reports" / L"last-issue.zip" : zip;
        where += L".url.txt";
        if (FILE* f = _wfopen(where.c_str(), L"wb")) {
            std::fwrite(url.data(), 1, url.size(), f);
            std::fclose(f);
        }
        std::fprintf(stderr, "[report] DES_REPORT_NO_OPEN: the issue link is in %s\n", narrow(where.wstring()).c_str());
        return;
    }
    open_url(url);
    if (!zip.empty()) reveal(zip);
}

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
        if (TryAcquireSRWLockExclusive(&g_log_lock)) {
            std::fflush(g_log_file);
            ReleaseSRWLockExclusive(&g_log_lock);
            return;
        }
        Sleep(10);
    }
}

namespace {
volatile uintptr_t g_nowhere = 0;  // (a null pointer the compiler cannot see)
}

bool frame_hook(unsigned long long frame) {
    static const unsigned long long crash_at = env_u64("DES_CRASH_AT_FRAME"), abort_at = env_u64("DES_ABORT_AT_FRAME"),
                                    report_at = env_u64("DES_REPORT_AT_FRAME");
    if (frame == crash_at && crash_at) {
        std::fprintf(stderr, "[des] DES_CRASH_AT_FRAME=%llu: crashing on purpose (test)\n", frame);
        *reinterpret_cast<volatile int*>(g_nowhere) = 1;
    }
    if (frame == abort_at && abort_at) {
        std::fprintf(stderr, "[des] DES_ABORT_AT_FRAME=%llu: aborting on purpose (test)\n", frame);
        std::abort();
    }
    return report_at && frame == report_at;
}

void send_issue_report(void* back_buffer, unsigned index, int width, int height, unsigned long long frame) {
    if (data_dir().empty()) {
        std::fprintf(stderr, "[report] no data folder (a test run without DES_DATA_DIR): no issue report\n");
        return;
    }
    if (g_report_busy.exchange(true)) return;  // (one at a time)
    std::vector<uint8_t> rgba;
    if (!host::capture(back_buffer, index, width, height, rgba)) rgba.clear();
    std::thread(make_issue_report, std::move(rgba), width, height, frame, settings::describe()).detach();
}

}  // namespace ui::report
