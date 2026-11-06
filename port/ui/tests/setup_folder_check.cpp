// setup_folder.cpp on its own: where setup builds, the usual folder or the one the player chose on setup's screens
// (docs/88 section 3, "The build folder"). The folder is found in this order: DES_SETUP_DATA, the choice in
// <usual>/setup-folder.txt, a portable install's build in the shared folder from before, the usual folder. A choice is
// kept in full, or relative in a portable install, and goes when the usual folder is chosen again. A folder picked
// that holds other things gets a folder of setup's own in it, and the program's own folders are refused.
//
// Each case runs in made-up folders under the system's temporary folder, removed at the end. Prints one line a case
// and "setup_folder_check: N passed, M failed"; exit 1 on a failure. The same on every platform (CI runs it on Windows
// and Linux, host.yml). Built on demand: cmake --build <build> --target setup_folder_check.
#include "setup_folder.hpp"

#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace ui::setup;

namespace {

int g_passed = 0, g_failed = 0;
fs::path g_root;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// A case: its own folder under the root, made empty first.
void run(const char* name, const std::function<std::string(const fs::path&)>& body) {
    std::error_code ec;
    const fs::path dir = g_root / name;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string problem;
    try {
        problem = body(dir);
    } catch (const std::exception& e) {
        problem = std::string("threw: ") + e.what();
    }
    if (problem.empty()) {
        ++g_passed;
        std::printf("ok    %s\n", name);
    } else {
        ++g_failed;
        std::printf("FAIL  %s: %s\n", name, problem.c_str());
    }
}

// "" when a and b are the same folder, else what differs.
std::string same(const char* what, const fs::path& got, const fs::path& want) {
    if (same_folder(got, want)) return {};
    return std::string(what) + " is " + utf8(got) + ", not " + utf8(want);
}

std::string all(std::initializer_list<std::string> problems) {
    for (const std::string& p : problems)
        if (!p.empty()) return p;
    return {};
}

}  // namespace

int main() {
    std::error_code ec;
    g_root = fs::temp_directory_path(ec) / ("setup_folder_check-" + std::to_string(getpid()));
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    // ---- The order ----
    run("given_wins", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, utf8(t / "chosen") + "\n");
        const Folders f = find_folders(t / "given", t / "shared", {});
        return all({same("dir", f.dir, t / "given"), f.usual.empty() ? "" : "a usual folder beside a given one",
                    f.chosen ? "given counted as chosen" : ""});
    });
    run("usual_without_choice", [](const fs::path& t) {
        const Folders f = find_folders({}, t / "shared", {});
        return all({same("dir", f.dir, t / "shared"), same("usual", f.usual, t / "shared"),
                    f.chosen ? "chosen with no choice" : ""});
    });
    run("full_choice", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, "# a note\n" + utf8(t / "D" / "build") + "\n");
        const Folders f = find_folders({}, t / "shared", {});
        return all({same("dir", f.dir, t / "D" / "build"), same("usual", f.usual, t / "shared"),
                    f.chosen ? "" : "not counted as chosen"});
    });
    run("relative_choice", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, "../build\n");
        return same("dir", find_folders({}, t / "shared", {}).dir, t / "build");
    });
    run("bom_crlf_spaces_comments", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, "\xEF\xBB\xBF# saved by Notepad\r\n\r\n   " + utf8(t / "x") + "  \r\n");
        return same("dir", chosen_folder(t / "shared"), t / "x");
    });
    run("only_comments", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, "# nothing\n\n");
        const Folders f = find_folders({}, t / "shared", {});
        return all({same("dir", f.dir, t / "shared"), f.chosen ? "chosen from comments" : ""});
    });
    run("the_usual_named", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, utf8(t / "shared") + "\n");
        const Folders f = find_folders({}, t / "shared", {});
        return all({same("dir", f.dir, t / "shared"), f.chosen ? "the usual folder counted as chosen" : ""});
    });
    run("no_shared_folder", [](const fs::path&) {
        const Folders f = find_folders({}, {}, {});
        return f.dir.empty() && f.usual.empty() ? std::string() : "a folder from nothing: " + utf8(f.dir);
    });

    // ---- A portable install ----
    run("portable_usual", [](const fs::path& t) {
        const Folders f = find_folders({}, t / "shared", t / "prog");
        return all({same("dir", f.dir, t / "prog" / "data" / "setup-data"),
                    same("usual", f.usual, t / "prog" / "data" / "setup-data")});
    });
    run("portable_choice", [](const fs::path& t) {
        write(t / "prog" / "data" / "setup-data" / kChoiceFile, "../../build\n");
        write(t / "shared" / kChoiceFile, utf8(t / "elsewhere") + "\n");  // (the shared folder's is not this one's)
        const Folders f = find_folders({}, t / "shared", t / "prog");
        return all({same("dir", f.dir, t / "prog" / "build"), f.chosen ? "" : "not counted as chosen"});
    });
    run("portable_built_in_shared_before", [](const fs::path& t) {
        write(t / "shared" / "setup" / "ready.txt", "format=1\n");
        const Folders f = find_folders({}, t / "shared", t / "prog");
        return all({same("dir", f.dir, t / "shared"), same("usual", f.usual, t / "prog" / "data" / "setup-data"),
                    f.chosen ? "counted as chosen" : ""});
    });
    run("portable_choice_over_shared_build", [](const fs::path& t) {
        write(t / "shared" / "setup" / "ready.txt", "format=1\n");
        write(t / "prog" / "data" / "setup-data" / kChoiceFile, utf8(t / "D") + "\n");
        return same("dir", find_folders({}, t / "shared", t / "prog").dir, t / "D");
    });

    // ---- Keeping the choice ----
    run("keep_in_full", [](const fs::path& t) {
        std::string why;
        if (!keep_choice(t / "shared", t / "D" / "build", t / "prog", why)) return "not kept: " + why;
        const std::string text = read(t / "shared" / kChoiceFile);
        if (text.find(utf8(t / "D" / "build")) == std::string::npos) return "not in full: " + text;
        return all({same("dir", find_folders({}, t / "shared", {}).dir, t / "D" / "build"),
                    fs::exists(t / "shared" / (std::string(kChoiceFile) + ".part")) ? "its .part file is left" : ""});
    });
    run("keep_relative_in_a_portable_install", [](const fs::path& t) {
        std::string why;
        const auto usual = t / "prog" / "data" / "setup-data";
        if (!keep_choice(usual, t / "prog" / "build", t / "prog", why)) return "not kept: " + why;
        const std::string text = read(usual / kChoiceFile);
        if (text.find("\n../../build\n") == std::string::npos) return "not relative: " + text;
        fs::create_directories(t / "prog" / "build");
        fs::rename(t / "prog", t / "prog moved");  // (the whole install renamed: the choice goes along)
        return same("dir", find_folders({}, t / "shared", t / "prog moved").dir, t / "prog moved" / "build");
    });
    run("keep_outside_a_portable_install_in_full", [](const fs::path& t) {
        std::string why;
        const auto usual = t / "prog" / "data" / "setup-data";
        if (!keep_choice(usual, t / "D", t / "prog", why)) return "not kept: " + why;
        return read(usual / kChoiceFile).find(utf8(t / "D")) == std::string::npos ? std::string("not in full")
                                                                                     : std::string();
    });
    run("the_usual_again_removes_the_file", [](const fs::path& t) {
        std::string why;
        if (!keep_choice(t / "shared", t / "D", {}, why)) return "not kept: " + why;
        if (!keep_choice(t / "shared", t / "shared", {}, why)) return "the usual folder not kept: " + why;
        return all({fs::exists(t / "shared" / kChoiceFile) ? "setup-folder.txt is still there" : "",
                    same("dir", find_folders({}, t / "shared", {}).dir, t / "shared")});
    });
    run("keep_a_name_in_other_letters", [](const fs::path& t) {
        std::string why;
        const auto folder = t / from_utf8("Jeux \xC3\xA9t\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");  // "Jeux été 日本"
        if (!keep_choice(t / "shared", folder, {}, why)) return "not kept: " + why;
        return same("dir", find_folders({}, t / "shared", {}).dir, folder);
    });

    // ---- A pick ----
    run("pick_a_new_folder", [](const fs::path& t) { return same("folder", folder_for_pick(t / "new"), t / "new"); });
    run("pick_an_empty_folder", [](const fs::path& t) {
        fs::create_directories(t / "empty");
        return same("folder", folder_for_pick(t / "empty"), t / "empty");
    });
    run("pick_a_folder_with_things_in_it", [](const fs::path& t) {
        write(t / "Games" / "notes.txt", "mine\n");
        return same("folder", folder_for_pick(t / "Games"), t / "Games" / kFolderName);
    });
    run("pick_a_folder_moved_by_hand", [](const fs::path& t) {
        write(t / "moved" / "setup" / "ready.txt", "format=1\n");
        write(t / "part" / "setup" / "journal.json", "{}\n");
        return all({same("built", folder_for_pick(t / "moved"), t / "moved"),
                    same("part built", folder_for_pick(t / "part"), t / "part")});
    });
    run("pick_the_usual_folder_again", [](const fs::path& t) {
        write(t / "shared" / kChoiceFile, utf8(t / "D") + "\n");  // (and setup's leftovers beside it)
        write(t / "shared" / "setup" / "logs" / "run.log", "\n");
        return same("folder", folder_for_pick(t / "shared"), t / "shared");
    });
    run("pick_a_folder_named_for_setup", [](const fs::path& t) {
        write(t / "Games" / kFolderName / "tools" / "x", "\n");  // (a first check's Python, say)
        return same("folder", folder_for_pick(t / "Games" / kFolderName), t / "Games" / kFolderName);
    });
    run("pick_a_drive", [](const fs::path& t) {
        const auto root = t.root_path();
        return same("folder", folder_for_pick(root), root / kFolderName);
    });

    // ---- Folders setup refuses ----
    run("can_use_a_folder", [](const fs::path& t) {
        const std::string why = cannot_use(t / "D" / "build", t / "prog");
        if (!why.empty()) return "refused: " + why;
        return all({fs::is_directory(t / "D" / "build") ? "" : "not made",
                    fs::is_empty(t / "D" / "build") ? "" : "its check's file is left"});
    });
    run("refuses_the_program_and_its_own_folders", [](const fs::path& t) {
        write(t / "prog" / "setup" / "release-files.txt", "des_host.exe\nlicenses/ffmpeg/LICENSE\nsetup/release-files.txt\n");
        for (const auto& f : {t / "prog", t / "prog" / "setup" / "x", t / "prog" / "licenses" / "y",
                              t / "prog" / ".previous" / "z"})
            if (cannot_use(f, t / "prog").empty()) return "not refused: " + utf8(f);
        for (const auto& f : {t / "prog" / "build", t / "prog" / "data" / "build", t / "prog" / kFolderName})
            if (const std::string why = cannot_use(f, t / "prog"); !why.empty()) return "refused " + utf8(f) + ": " + why;
        return std::string();
    });
    run("refuses_what_cannot_be_made", [](const fs::path& t) {
        write(t / "a file", "x");
        return cannot_use(t / "a file" / "build", {}).empty() ? std::string("not refused") : std::string();
    });
    run("refuses_a_relative_folder", [](const fs::path&) {
        return cannot_use("build", {}).empty() ? std::string("not refused") : std::string();
    });

    // ---- What differs by platform ----
    run("same_folder_by_case", [](const fs::path& t) {
        const bool same_case = same_folder(t / "Not There" / "Build", t / "not there" / "build");
#ifdef _WIN32
        return same_case ? std::string() : std::string("Windows' names differ by case");
#else
        return same_case ? std::string("names that differ by case taken as one") : std::string();
#endif
    });

    fs::remove_all(g_root, ec);
    std::printf("setup_folder_check: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
