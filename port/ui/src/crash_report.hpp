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

}  // namespace ui::crash_report
