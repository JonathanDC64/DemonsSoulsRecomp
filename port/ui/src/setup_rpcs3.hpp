// Written for Demon's Souls. The RPCS3 route's part in the host (docs/90 section 3.6): finding the player's RPCS3
// before any Python exists, for the "Import from RPCS3" card and its page, and reading the one line of RPCS3's game
// list that the card shows. The engine (tools/setup/des_setup/rpcs3.py) reads the rest when the page checks. Only
// reads: nothing is ever written in RPCS3's folders. Where to look is the one fact that differs by platform (Windows:
// shortcuts, a running RPCS3, Windows' record of programs started, the usual folders; Linux: PATH and AppImages, since
// the engine has no Flatpak route yet); the rest is the same everywhere.
#pragma once

#include <filesystem>
#include <string>

namespace ui::setup {

// The player's RPCS3, as the card and the page show it.
struct Rpcs3 {
    enum class Library { None, Folder, Iso };

    std::filesystem::path folder;   // the folder that holds the program; empty: no RPCS3
    std::filesystem::path program;  // what the engine runs (--rpcs3): rpcs3.exe, rpcs3 or an AppImage
    bool portable = false;          // a portable folder beside the program: setup starts it only while it is closed
    Library library = Library::None;  // Demon's Souls (BLUS30443) in RPCS3's game list
    std::filesystem::path game;       // that entry: the folder that holds PS3_GAME, or the ISO file

    bool found() const { return !folder.empty(); }
};

// The RPCS3 to use: RPCS3_DIR (a developer's), `remembered` (settings.ini's rpcs3_dir, the one setup used last, read
// by the caller on the UI's thread), then where this platform keeps a record of it or puts it. The first RPCS3 with
// Demon's Souls in its game list wins, else the first RPCS3 found. Looks in folders, so it runs off the UI's thread (a
// sleeping disk wakes up for it).
Rpcs3 find_rpcs3(const std::filesystem::path& remembered);

// `path` (a folder, or the program in it) as an RPCS3: found() is false if it holds none. For the player's pick.
Rpcs3 rpcs3_at(const std::filesystem::path& path);

// Where find_rpcs3 looks, as the page that found none says it: "RPCS3's shortcuts, ...".
std::string rpcs3_looked_in();

// What the engine is given as --rpcs3 for it: the program's path.
std::string rpcs3_arg(const Rpcs3& r);

}  // namespace ui::setup
