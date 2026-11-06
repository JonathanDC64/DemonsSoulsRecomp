// Written for Demon's Souls. Where setup builds the game (docs/88 section 3, "The build folder"): the usual folder, or
// one the player chose on setup's screens. The choice is a small file in the usual folder, setup-folder.txt, that
// names the chosen one, so that every later start (the game's own too) finds it. Only std::filesystem, so the same
// on every platform: setup_engine.cpp decides with these, and tests/setup_folder_check.cpp checks them on their own.
#pragma once

#include <filesystem>
#include <string>

namespace ui::setup {

// The choice's file, in the usual folder. des_setup reads the same file (pipeline.py CHOICE).
inline constexpr const char* kChoiceFile = "setup-folder.txt";
// des_setup's NAME: the shared folder's name, and the folder of its own that setup makes in a folder picked that holds
// other things.
inline constexpr const char* kFolderName = "DemonsSoulsRecomp";

// The folder that <usual>/setup-folder.txt names: its first line that is not empty or a comment ('#'), a full path or
// one relative to the usual folder. Empty when there is no such file or line.
std::filesystem::path chosen_folder(const std::filesystem::path& usual);

// Setup's folder, and the usual one that holds the choice.
struct Folders {
    std::filesystem::path dir;    // setup's data folder
    std::filesystem::path usual;  // where setup builds when nothing is chosen, and keeps the choice; empty when the
                                  // folder is given (DES_SETUP_DATA): nothing to choose then
    bool chosen = false;          // dir is the player's choice
};
// In this order: `given` (DES_SETUP_DATA) as it is; the folder chosen; then the usual one. The usual folder is
// `shared` (%LOCALAPPDATA%\DemonsSoulsRecomp, on Linux ~/.local/share/DemonsSoulsRecomp), or in a portable install
// (`portableProgram`: the program's folder, when portable.txt is beside it; else empty) data/setup-data beside the
// program. A portable install that was built in the shared folder before, and chose nothing, keeps using it (issue
// #364).
Folders find_folders(const std::filesystem::path& given, const std::filesystem::path& shared,
                     const std::filesystem::path& portableProgram);

// The same folder: the same one on disk when both exist, else the same path (on Windows, in any case).
bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b);
// Setup's work is in the folder: a build (setup/ready.txt) or part of one (setup/journal.json).
bool holds_setup(const std::filesystem::path& folder);
// The folder a pick in the folder dialog gives: the folder itself when it is new or empty, or setup's already (its
// work in it, a folder moved by hand; the usual folder, which holds the choice's file; or one named DemonsSoulsRecomp);
// else setup's own folder in it (DemonsSoulsRecomp), so that setup's folders (inputs, work, game, setup, ...) do not go
// among the player's files.
std::filesystem::path folder_for_pick(const std::filesystem::path& picked);
// Empty when setup can build in `folder`, else why not, for the player: it is the program's folder, or in one of the
// program's own folders, which an update replaces (setup, and the names in setup/release-files.txt); or setup cannot
// make it or write in it. It makes the folder. program: the program's folder (empty: not known).
std::string cannot_use(const std::filesystem::path& folder, const std::filesystem::path& program);
// Keeps the choice of `folder` in <usual>/setup-folder.txt, or removes that file when `folder` is the usual one. The
// path is written relative to the usual folder when both are in the program's folder (a portable install, which may
// be moved or renamed as one folder), else in full. False, with why (for the player), when the file cannot be written.
bool keep_choice(const std::filesystem::path& usual, const std::filesystem::path& folder,
                 const std::filesystem::path& program, std::string& why);

}  // namespace ui::setup
