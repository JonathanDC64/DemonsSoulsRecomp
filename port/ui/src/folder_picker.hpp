// Written for Demon's Souls. The system's own dialog for choosing a folder, for the setup screens (docs/88 section
// 2a). The dialog is the one fact here that differs by platform, so it is one function for each: on Windows the
// shell's folder dialog (IFileOpenDialog with FOS_PICKFOLDERS), elsewhere the desktop portal's file chooser over
// D-Bus, with zenity or kdialog where there is no portal. The rest is the same everywhere.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace ui {

struct PickedFolder {
    std::filesystem::path folder;  // the folder chosen; empty if the dialog was closed without one
    bool failed = false;           // no dialog could be shown: `why` says why, for the log
    std::string why;
};

// Opens the dialog and returns at once. `done` runs once the dialog has closed, on a thread of its own (not the UI's,
// which draws on meanwhile). start: the folder it opens in, if there is one. title: UTF-8. owner: the window it
// belongs to (host::window()); used on Windows, where the dialog stays in front of it; null for none.
void pick_folder(const std::filesystem::path& start, const std::string& title, void* owner,
                 std::function<void(PickedFolder)> done);

}  // namespace ui
