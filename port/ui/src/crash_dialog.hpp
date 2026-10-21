// The crash dialog: a small window in the menu's style (RmlUi) that tells the player the game crashed and offers to
// restart it or report the problem. Runs in a second process started by crash_report.cpp, so it needs nothing from the
// crashed one. Adapted for Demon's Souls from FFCCEOT-Recomp's crash_dialog.hpp: drawn with the same host as the
// in-game menu (D3D11On12), here on a D3D12 device and swap chain of its own.
#pragma once

#include <functional>
#include <string>

namespace ui::crash_dialog {

struct Content {
    std::string what;     // "access violation writing 0000000000000000", or for a hang "no new frame for 14 s"
    std::string details;  // the top of the stack, or for a hang the guest threads
    bool hang = false;    // the game is not responding (still running), not crashed
};

struct Actions {
    // Makes the report and opens the issue form; the text to show the player afterwards (what was done and what to attach).
    std::function<std::string()> report;
    // Starts the game again (once the crashed process has ended).
    std::function<void()> restart;
    // A hang's "Close the game": ends the game's process.
    std::function<void()> close_game;
};

// Shows the dialog until it is closed. False if it could not be shown.
//
// Test knobs (from the crashed game's environment): DES_CRASH_DIALOG_REPORT=1 presses "Report on GitHub" by itself,
// DES_CRASH_DIALOG_SHOT=<file.bmp> writes the dialog's picture, DES_CRASH_DIALOG_EXIT_MS=<ms> closes it after that long.
bool show(const Content& content, const Actions& actions);

}  // namespace ui::crash_dialog
