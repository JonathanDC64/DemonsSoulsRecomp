// settings.cpp on its own (its report and privacy calls stubbed below): settings.ini files and environments in, the
// environment the game's runtime then reads out. For the Renderer row (Settings > Display, docs/120): RSX_BACKEND from
// the row reaches the runtime the way every Apply::Restart row does, an RSX_BACKEND from the environment wins, the data
// folder becomes RSX_STATE_DIR, and the row exists only in a build whose runtime has the Vulkan route.
//
//   settings_check --row      a build with the row (DES_RSX_VULKAN: port/CMakeLists.txt found the Vulkan route)
//   settings_check --no-row   a build without it: nothing of it, and settings.ini's RSX_BACKEND is not applied
//
// Each case runs in a process of its own (the settings are read once a process), in a folder under the system's
// temporary folder that it removes. Prints one line a case and "settings_check: N passed, M failed"; exit 1 on a
// failure. Built on demand: cmake --build <build> --target settings_check.
#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

extern "C" void ps3_setenv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value && *value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// What settings.cpp needs of report.cpp and privacy.cpp. The data folder is TEST_DATA_DIR ("" = a test run's none).
namespace ui::report {
namespace {
std::filesystem::path g_dir;
bool g_dir_set = false;
}  // namespace
const std::filesystem::path& data_dir() {
    if (!g_dir_set) {
        const char* d = std::getenv("TEST_DATA_DIR");
        g_dir = d && *d ? std::filesystem::path(d) : std::filesystem::path();
        g_dir_set = true;
    }
    return g_dir;
}
void set_data_dir(const std::filesystem::path& dir) {
    g_dir = dir;
    g_dir_set = true;
}
std::string this_pc() { return "test-pc"; }
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // (ASCII paths only here)
std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += char(c);
    return s;
}
FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(path.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
}
}  // namespace ui::report

namespace ui::privacy {
int version() { return 1; }
void set_needed(bool) {}
}  // namespace ui::privacy

namespace {

namespace settings = ui::settings;

bool g_row = false;  // --row
int g_fail = 0;

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("    FAIL: %s\n", what.c_str());
        g_fail++;
    }
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// One case, in this process: the case's folder is the working one (the parent made it, with settings.ini when the case
// has one, and set the environment).
int run_case(const std::string& name) {
    const std::string data = env("TEST_DATA_DIR");
    settings::load_and_apply_env();
    const bool has = settings::find("RSX_BACKEND") != nullptr;
    check(has == g_row, std::string("the Renderer row is ") + (has ? "there" : "not there"));
    if (name == "no-ini") {
        check(env("RSX_BACKEND").empty(), "RSX_BACKEND unset (the runtime's default, d3d12): " + env("RSX_BACKEND"));
        check(settings::get("RSX_BACKEND") == (g_row ? "d3d12" : ""), "the row's default: " + settings::get("RSX_BACKEND"));
        check(env("RSX_STATE_DIR") == (g_row ? data : ""), "RSX_STATE_DIR: " + env("RSX_STATE_DIR"));
        const std::string d = settings::describe();
        check((d.find("RSX_BACKEND = d3d12\n") != std::string::npos) == g_row, "settings.txt's line");
    } else if (name == "ini-vulkan") {
        check(env("RSX_BACKEND") == (g_row ? "vulkan" : ""), "RSX_BACKEND from settings.ini: " + env("RSX_BACKEND"));
        check(!settings::pinned("RSX_BACKEND"), "the row is not pinned");
        check(env("RSX_STATE_DIR") == (g_row ? data : ""), "RSX_STATE_DIR: " + env("RSX_STATE_DIR"));
    } else if (name == "ini-d3d12") {
        check(env("RSX_BACKEND") == (g_row ? "d3d12" : ""), "RSX_BACKEND from settings.ini: " + env("RSX_BACKEND"));
    } else if (name == "env-wins") {  // RSX_BACKEND=d3d12 in the environment, vulkan in settings.ini
        check(env("RSX_BACKEND") == "d3d12", "the environment's RSX_BACKEND kept: " + env("RSX_BACKEND"));
        check(settings::pinned("RSX_BACKEND") == g_row, "the row shows as set from outside");
        check(!g_row || settings::pinned_value("RSX_BACKEND") == "d3d12", "its value: " + settings::pinned_value("RSX_BACKEND"));
    } else if (name == "env-vulkan") {  // RSX_BACKEND=vulkan in the environment, no settings.ini
        check(env("RSX_BACKEND") == "vulkan", "the environment's RSX_BACKEND kept: " + env("RSX_BACKEND"));
    } else if (name == "env-hal") {  // a developer's RSX_BACKEND=hal-d3d12 (no row value of its own)
        check(env("RSX_BACKEND") == "hal-d3d12", "the environment's RSX_BACKEND kept: " + env("RSX_BACKEND"));
    } else if (name == "state-env-wins") {  // RSX_STATE_DIR in the environment
        check(env("RSX_STATE_DIR") == "elsewhere", "the environment's RSX_STATE_DIR kept: " + env("RSX_STATE_DIR"));
    } else if (name == "test-run") {  // no data folder
        check(env("RSX_STATE_DIR").empty(), "no RSX_STATE_DIR in a test run: " + env("RSX_STATE_DIR"));
    } else if (name == "set-saves") {  // the row changed in the menu: kept for the next start, not applied now
        settings::set("RSX_BACKEND", "vulkan");
        check(env("RSX_BACKEND").empty(), "not applied before a restart: " + env("RSX_BACKEND"));
        const std::string ini = read_text(std::filesystem::path(data) / "settings.ini");
        check((ini.find("RSX_BACKEND = vulkan\n") != std::string::npos) == g_row, "settings.ini's line");
    } else {
        std::printf("    unknown case %s\n", name.c_str());
        return 2;
    }
    return g_fail ? 1 : 0;
}

struct Case {
    const char* name;
    const char* ini;  // settings.ini's text, or null for none
    const char* env[2][2];
    bool data = true;  // a data folder (else a test run's none)
};

const Case kCases[] = {
    {"no-ini", nullptr, {}},
    {"ini-vulkan", "RSX_BACKEND = vulkan\n", {}},
    {"ini-d3d12", "RSX_BACKEND = d3d12\n", {}},
    {"env-wins", "RSX_BACKEND = vulkan\n", {{"RSX_BACKEND", "d3d12"}}},
    {"env-vulkan", nullptr, {{"RSX_BACKEND", "vulkan"}}},
    {"env-hal", "RSX_BACKEND = vulkan\n", {{"RSX_BACKEND", "hal-d3d12"}}},
    {"state-env-wins", nullptr, {{"RSX_STATE_DIR", "elsewhere"}}},
    {"test-run", nullptr, {}, false},
    {"set-saves", nullptr, {}},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "--row") && std::strcmp(argv[1], "--no-row"))) {
        std::fprintf(stderr, "usage: settings_check --row|--no-row\n");
        return 2;
    }
    g_row = !std::strcmp(argv[1], "--row");
    if (argc >= 4 && !std::strcmp(argv[2], "--case")) return run_case(argv[3]);

    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec) / ("settings_check_" + std::to_string(getpid()));
    int passed = 0, failed = 0;
    for (const Case& c : kCases) {
        const auto dir = root / c.name;
        std::filesystem::create_directories(dir, ec);
        if (c.ini) std::ofstream(dir / "settings.ini") << c.ini;
        for (const char* k : {"RSX_BACKEND", "RSX_STATE_DIR", "DES_SETTINGS", "DES_DATA_DIR", "PS3_PIDFILE"})
            ps3_setenv(k, "");
        ps3_setenv("TEST_DATA_DIR", c.data ? dir.string().c_str() : "");
        ps3_setenv("DES_SETTINGS", c.data ? (dir / "settings.ini").string().c_str() : "");
        for (const auto& e : c.env)
            if (e[0]) ps3_setenv(e[0], e[1]);
#ifdef _WIN32
        const std::string cmd = "\"\"" + std::string(argv[0]) + "\" " + argv[1] + " --case " + c.name + "\"";  // (cmd /c)
#else
        const std::string cmd = "'" + std::string(argv[0]) + "' " + argv[1] + " --case " + c.name;
#endif
        const int rc = std::system(cmd.c_str());
        std::printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", c.name);
        (rc == 0 ? passed : failed)++;
    }
    std::filesystem::remove_all(root, ec);
    std::printf("settings_check (%s): %d passed, %d failed\n", g_row ? "with the Renderer row" : "without it", passed,
                failed);
    return failed ? 1 : 0;
}
