// The launcher's update check (see updates.hpp; the engine's side is tools/setup/des_setup/update.py, docs/105).
#include "updates.hpp"

#include "app.hpp"
#include "crash_report.hpp"
#include "report.hpp"
#include "settings.hpp"
#include "setup_engine.hpp"
#include "version.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <system_error>

namespace ui::updates {
namespace {

#ifdef _WIN32
constexpr const char* kExe = "des_host.exe";  // the player's zip's (tools/release/package.py, update.py's EXE)
#else
constexpr const char* kExe = "des_host";
#endif

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

// X.Y.Z, digits only (update.py's version_of, without its "v"): a release's version. Its numbers, or false.
bool plain_version(const std::string& v, long long out[3] = nullptr) {
    long long n[3] = {0, 0, 0};
    int part = 0;
    bool digit = false;
    for (const char c : v) {
        if (c >= '0' && c <= '9') {
            n[part] = n[part] * 10 + (c - '0');
            digit = true;
            if (n[part] > 1000000) return false;
        } else if (c == '.' && digit && part < 2) {
            ++part;
            digit = false;
        } else {
            return false;
        }
    }
    if (part != 2 || !digit) return false;
    if (out) for (int i = 0; i < 3; ++i) out[i] = n[i];
    return true;
}

struct Check {
    setup::Engine engine;
    setup::Status status;
    bool running = false;
    State state = State::Off;
    Release found;
};

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// `p` is an absolute path in `root` or below it, compared as text once both are normalised (update.py's inside()):
// no ".." can climb out. Names compare without case on Windows, as its file system does.
bool inside(std::filesystem::path root, const std::filesystem::path& p) {
    if (root.empty() || !p.is_absolute()) return false;
    std::error_code ec;
    if (auto c = std::filesystem::weakly_canonical(root, ec); !ec) root = c;  // (the engine's install is resolved too)
    root = root.lexically_normal();
    if (!root.has_filename()) root = root.parent_path();  // (a trailing separator)
    const auto q = p.lexically_normal();
    auto qi = q.begin();
    for (auto ri = root.begin(); ri != root.end(); ++ri, ++qi) {
        if (qi == q.end()) return false;
#ifdef _WIN32
        std::wstring a = ri->native(), b = qi->native();
        for (auto* s : {&a, &b})
            for (wchar_t& c : *s) c = static_cast<wchar_t>(std::towlower(c));
        if (a != b) return false;
#else
        if (*ri != *qi) return false;
#endif
    }
    return true;
}

// Removes `p` if it is a folder of empty folders (update.py's _clear_empty). False if something is left.
bool clear_empty(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec) || std::filesystem::is_symlink(p, ec)) return false;
    std::filesystem::directory_iterator it(p, ec), end;
    for (; !ec && it != end; it.increment(ec))
        if (!clear_empty(it->path())) return false;
    return !ec && std::filesystem::remove(p, ec);
}

std::atomic<bool> g_hold_close{false};

// (never destroyed: its reading thread may still be ending when the program exits)
Check& check() {
    static Check* c = new Check();
    return *c;
}

}  // namespace

std::string this_version() {
    if (const char* as = std::getenv("DES_UPDATE_AS_VERSION"); as && *as) return as;
    return kVersion;
}

std::filesystem::path install_dir() {
    return report::program_dir();
}

bool possible() {
    if (!plain_version(this_version())) return false;
    const bool test = env_set("DES_UPDATE_FEED");
    if (settings::harness_run() && !test) return false;
    std::error_code ec;
    const auto dir = install_dir();
    if (dir.empty()) return false;
    if (!test && (!std::filesystem::is_regular_file(dir / "release-info.txt", ec) ||
                  std::filesystem::exists(dir / ".git", ec)))
        return false;
    return !setup::python().empty() && !setup::engine_script().empty() && !setup::data_dir().empty();
}

bool wanted() {
    return settings::get_int("update_check") != 0 && possible();
}

namespace {
bool g_over_game = false;
}

void set_over_game(bool over) {
    g_over_game = over;
}

bool over_game() {
    return g_over_game;
}

void start_check() {
    Check& c = check();
    if (c.running || !possible()) return;
    c.status = {};
    if (!c.engine.start_command({"update-check", "--app", this_version(), "--data", utf8(setup::data_dir())})) {
        c.state = State::Failed;
        std::fprintf(stderr, "[update] the check could not start: %s\n", c.engine.why().c_str());
        return;
    }
    c.running = true;
    c.state = State::Checking;
}

State state() {
    Check& c = check();
    if (!c.running) return c.state;
    c.engine.poll(c.status);
    if (!c.status.finished()) return c.state;
    c.running = false;
    if (c.status.result != "ok") {
        c.state = State::Failed;
        std::fprintf(stderr, "[update] the check got no answer: %s\n", c.status.errorText.c_str());
    } else if (c.status.release.version.empty()) {
        c.state = State::Newest;
        std::fprintf(stderr, "[update] %s is the newest version\n", this_version().c_str());
    } else {
        c.found = {c.status.release.version, c.status.release.page, c.status.release.when, c.status.notes};
        c.state = State::Found;
        std::fprintf(stderr, "[update] version %s is out (this is %s)%s\n", c.found.version.c_str(),
                     this_version().c_str(), skipped() ? ", skipped" : "");
    }
    return c.state;
}

const Release& found() {
    return check().found;
}

bool skipped() {
    return !check().found.version.empty() && settings::get("update_skip") == check().found.version;
}

void skip() {
    if (!check().found.version.empty()) settings::store("update_skip", check().found.version);
}

std::string kept_version() {
    std::error_code ec;
    std::string best;
    long long bestN[3] = {-1, -1, -1};
    std::filesystem::directory_iterator it(install_dir() / ".previous", ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        long long n[3];
        std::error_code ec2;
        if (!plain_version(name, n) || !std::filesystem::is_regular_file(it->path() / kExe, ec2)) continue;
        if (std::lexicographical_compare(bestN, bestN + 3, n, n + 3)) {
            best = name;
            for (int i = 0; i < 3; ++i) bestN[i] = n[i];
        }
    }
    return best;
}

std::string status_line() {
    switch (state()) {
    case State::Off:
        if (!plain_version(this_version())) return "This build is not a release, so it does not look for updates.";
        if (!possible()) return "Updates start once setup has run.";
        return settings::get_int("update_check") ? "Not checked yet." : "Off. Check now looks once.";
    case State::Checking:
        return "Looking for a newer version...";
    case State::Newest:
        return "You have the newest version.";
    case State::Found:
        return "Version " + found().version + " is out." + (skipped() ? " You chose to skip it." : "");
    case State::Failed:
        return "The check could not reach GitHub. It tries again at the next start.";
    }
    return {};
}

bool restart_into(const char* open, const std::string& version) {
    std::vector<std::string> set = {std::string("DES_UI_OPEN=") + open};
    if (!version.empty()) set.push_back("DES_UPDATE_TO=" + version);
    if (!crash_report::start_again(set, false)) {
        std::fprintf(stderr, "[update] the program could not start again for %s\n", open);
        return false;
    }
    std::fprintf(stderr, "[update] the program starts again for %s %s\n", open, version.c_str());
    if (app().quit) app().quit();
    return true;
}

bool recover_swap() {
    std::error_code ec;
    const auto journal = install_dir() / ".update" / "swap.txt";
    if (install_dir().empty() || !std::filesystem::is_regular_file(journal, ec)) return false;
    std::vector<std::vector<std::string>> steps;  // {"made", folder} or {"move", from, to}, as they happened
    {
        std::ifstream f(journal, std::ios::binary);
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> fields;
            for (size_t from = 0;;) {
                const size_t tab = line.find('\t', from);
                fields.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                if (tab == std::string::npos) break;
                from = tab + 1;
            }
            if ((fields[0] == "made" && fields.size() == 2) || (fields[0] == "move" && fields.size() == 3))
                steps.push_back(std::move(fields));
        }
    }
    // Only renames inside the game's folder are undone: a journal that names anything else is left as it is (the
    // engine refuses it too, and says so).
    for (const auto& s : steps)
        for (size_t i = 1; i < s.size(); ++i)
            if (!inside(install_dir(), from_utf8(s[i]))) {
                std::fprintf(stderr, "[update] the swap journal names a path outside the game's folder; it is left as "
                                     "it is\n");
                return false;
            }
    bool ok = true;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if ((*it)[0] == "made") {
            clear_empty(from_utf8((*it)[1]));
            continue;
        }
        const auto src = from_utf8((*it)[1]), dst = from_utf8((*it)[2]);
        clear_empty(src);  // (a folder a later step made, emptied by the steps undone before)
        if (std::filesystem::exists(dst, ec) && !std::filesystem::exists(src, ec)) {
            std::filesystem::rename(dst, src, ec);
            if (ec) {
                ok = false;
                std::fprintf(stderr, "[update] could not move %s back: %s\n", (*it)[2].c_str(), ec.message().c_str());
            }
        }
    }
    if (!ok) {
        std::fprintf(stderr, "[update] the swap that was cut short is not fully undone; the next update tries again\n");
        return false;
    }
    std::filesystem::remove(journal, ec);
    std::fprintf(stderr, "[update] the swap that was cut short is undone (%zu steps)\n", steps.size());
    return true;
}

void hold_close(bool on) {
    g_hold_close.store(on);
}

bool close_held() {
    return g_hold_close.load();
}

}  // namespace ui::updates
