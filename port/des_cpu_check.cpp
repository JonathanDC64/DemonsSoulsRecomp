// Before anything else runs: does this processor have what the program is built for? The runtime and the game module
// are compiled with SSE4.1 (the lifted PowerPC and SPU code leans on it, docs/98), so without it the first such
// instruction ends the program with c000001d, in no particular place. A player's Pentium E5400 did that when the window
// opened (issues #349, #350), with no word on what was wrong. It says so here instead, before setup spends any time.
//
// It has to come before the C++ static initializers, as those can be compiled with SSE4.1 too. So it runs from the
// initializer list's early part on Windows (.CRT$XCB, the usual ones are .CRT$XCU), and as the first constructor on
// Linux. It uses only the system's own calls, and nothing of the C++ library.
//
// DES_TEST_OLD_CPU=1 acts as if the processor had no SSE4.1. PS3_NO_USER_INPUT=1 leaves out the dialog.
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>
#else
#include <SDL.h>
#include <cpuid.h>
#include <unistd.h>
#endif

namespace {

constexpr char kTitle[] = "Demon's Souls";
constexpr char kText[] =
    "Your processor is too old to run the game.\n\n"
    "The game needs a processor with SSE4.1. Most have it since about 2011, but this one doesn't.";

#if defined(_WIN32)

bool has_sse41() {
    int r[4];
    __cpuid(r, 1);
    return (r[2] & (1 << 19)) != 0;
}

bool env_set(const char* name) {
    char v[8];
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v && !(v[0] == '0' && v[1] == 0);
}

void say(bool dialog) {
    DWORD written = 0;
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        WriteFile(err, "[des] ", 6, &written, nullptr);
        WriteFile(err, kText, DWORD(sizeof kText - 1), &written, nullptr);
        WriteFile(err, "\n", 1, &written, nullptr);
    }
    if (dialog) MessageBoxA(nullptr, kText, kTitle, MB_OK | MB_ICONERROR);
}

[[noreturn]] void leave() { ExitProcess(1); }

#else

bool has_sse41() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & bit_SSE4_1) != 0;
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && !(v[0] == '0' && v[1] == 0);
}

void say(bool dialog) {
    std::fprintf(stderr, "[des] %s\n", kText);
    if (dialog) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, kText, nullptr);
}

[[noreturn]] void leave() { _exit(1); }

#endif

void cpu_check() {
    if (has_sse41() && !env_set("DES_TEST_OLD_CPU")) return;
    say(!env_set("PS3_NO_USER_INPUT"));
    leave();
}

}  // namespace

#if defined(_WIN32)
extern "C" {
static void __cdecl cpu_check_entry() { cpu_check(); }
using CpuCheckFn = void(__cdecl*)();
#pragma section(".CRT$XCB", long, read)
__declspec(allocate(".CRT$XCB")) extern const CpuCheckFn des_cpu_check_entry;
const CpuCheckFn des_cpu_check_entry = cpu_check_entry;
}
#pragma comment(linker, "/include:des_cpu_check_entry")
#else
__attribute__((constructor(101))) static void cpu_check_entry() { cpu_check(); }
#endif
