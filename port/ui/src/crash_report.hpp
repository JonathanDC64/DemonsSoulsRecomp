// Crash reports (docs/32): when the game ends on an unhandled exception (an access violation and the like) or an
// abort, writes crash-<time>.txt (what happened and a symbolized stack), crash-<time>.dmp (a minidump) and
// crash-<time>.log (the end of the log) in the data folder's crashes folder. Then a second process of this program
// (des_boot.exe --crash-dialog ...) tells the player: restart the game, or report the problem on GitHub (an issue with
// the details filled in, and a zip of the report to attach). Adapted for Demon's Souls from FFCCEOT-Recomp's
// crash_report.hpp.
//
// Only a truly unhandled exception counts: the runtime's vectored handlers (guest memory committed on first touch,
// its guard pages) run first and keep handling what they handle, and the runtime's own top-level filter and abort
// handler still run (they print the guest state). Test harness runs (no data folder) install nothing.
#pragma once

namespace ui::crash_report {

// From des_ui_early_init (after boot_main installed its own filter and abort handler, which are chained). Nothing
// without a data folder. DES_NO_CRASH_DIALOG=1 writes the report but shows no dialog.
void install();

// The game has stopped (no new frame for `ms_since_frame` ms) and the render thread cannot draw the menu either:
// writes hang-<time>.txt (the runtime's snapshot: what every thread is doing) and hang-<time>.log (the end of the
// log) beside the crash reports, then starts the dialog in its hang form (keep waiting, close the game, or report
// it). Nothing without a data folder. Takes a few seconds (the snapshot samples the threads), on the caller's
// thread: the watch thread (des_ui.cpp), which first asks hang_dialog_open().
void report_hang(long long ms_since_frame);
// A hang dialog report_hang started is still open.
bool hang_dialog_open();

// Play on setup alone (des_ui.cpp): starts this program again as a new process, with the command line and the
// environment it started with, less DES_UI_OPEN (setup's opener), plus DES_SKIP_UI=1 (setup's Play was the start screen)
// and DES_LAUNCH_AFTER=<this process's id>: the new one waits for this one to end before it starts its log, so one
// window shows. The environment it started with, not this process's: settings.ini put its values in this one's, and
// the new one would take them as set from outside, over what setup stored since. False if it could not start.
bool start_again();

}  // namespace ui::crash_report
