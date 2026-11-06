# 74. Problem reports: what they hold, and reporting a frozen game

docs/32 describes the data folder, the log, the crash dialog and **Report an
issue** as first built. This page covers what was added on 2026-10-21:

- more in each report, so a problem can be debugged from the zip alone;
- the menu still opens when the game freezes, so a frozen game can be
  reported too;
- the rules that keep private data and the game's content out of reports.

Since 2026-11-03 both kinds of report also hold the player's save, so a
problem can be loaded where it happened instead of played up to again.

Code:
- port: `port/ui/src/report.cpp`, `crash_report.cpp`, `crash_dialog.cpp`,
  `des_ui.cpp` (watch thread), `settings.cpp`;
- ps3recomp: `rsx_d3d12_overlay_wake` (`libs/video/rsx_d3d12_backend.c`),
  `ps3_diag_report` and `ps3_ms_since_frame` (`runtime/ppu/tests/boot_main.cpp`).

## What a report holds

**Report an issue** (menu bar) zips `reports\issue-<time>.zip`:

| File | What |
|---|---|
| `screenshot.png` | the game's picture, taken with the menu closed |
| `system.txt` | port version (and whether it is a developer build), ps3recomp version, the game dump's SHA-256, the renderer that runs (with a runtime that has the route switch: `d3d12`, `vulkan`, ..., and why it is D3D12 when Vulkan was asked for; not in the crash dialog's), Windows version, CPU (cores, threads, instruction sets), memory (the PC's and this process's), each GPU with its driver (and the one in use with its memory budget), each display (size, refresh, primary, HDR), power (battery or not), free disk space where the game and the data are, gamepads (XInput, DualSense/DS4), keyboard layout, and other software loaded into the game (overlays, hooks) |
| `game.txt` | time running; game frames, with "no new frame for N s: the game had stopped" when it has; frames per second for each of the last 60 s; overlay frames; window or fullscreen, size and DPI; picture size; the save (its folder, how many files, how many minutes before the report it was saved); crash and hang reports on this PC in the last 7 days (names only) |
| `settings.ini` | every setting with its value, `# changed; default: X` where it differs from the default, and `# set in the environment: X` where the environment overrides it |
| `environment.txt` | the command line and every port or runtime option set in the environment (`DES_`, `PS3_`, `RSX_`, `SPU_`, `SPURS_`, `GCM_`, `PAD_`, `PPU_`, `AUDIO_`, `VBLANK`, `FIFO_`, `LIBSRE_`, `SAIL_`, `TROPHY_`, `WATCHDOG_`) |
| `runtime.txt` | the runtime's snapshot (below), sampled over 2 s |
| `des.log` | the first 120 lines of the log and its last 512 KB |
| `des.prev.log` | the end of the previous run's log, when that run was in the last 24 h: a freeze or crash the player restarted after |
| `save/<folder>/...` | the player's save (`report::save_files`): each save folder in the game's save root (`PS3_SAVEDATA_ROOT`, else `gamedata/dev_hdd0/home/00000001/savedata`, as `cellSaveData.c` finds it). Every file except the folder's two pictures (`ICON0.PNG`, `PIC1.PNG`: the game's art, and a save loads without them) and a file the runtime is still writing (`.<name>.tmp`). If the game saved while it was read, it is read again, up to twice, half a second apart. At most 8 MB a file and 32 MB in all; a Demon's Souls save is ten 256 KB files |

To load a reported save, put its folder in a save root of its own and start
with `PS3_SAVEDATA_ROOT=<that root>`. Copy in `ICON0.PNG` and `PIC1.PNG` from
any other save if the game's load screen wants them.

The crash and hang dialog's **Report on GitHub** zips `crash.txt`,
`hang.txt` or `graphics-lost.txt`, `system.txt`, `settings.ini`, `environment.txt` (the game's
command line, saved with the report), `des.log` and the save (as above; its
line is in the issue's System block). The dialog runs as a
process of its own and reads no settings. So the game keeps
`logs\settings.txt` up to date: the same listing as above, written at
startup and after each change. The dialog uses that file, and falls back to
`settings.ini` only if it is missing. Without it, a player who had never
changed a setting would send no settings at all.

### The runtime snapshot

`ps3_diag_report(out, seconds)` writes, in one go, what the debug console's
`stat`, `cpu`, `waits`, `threads`, `spu`, `spuhist` and `knobs` would show:

- frames so far, the time since the last one, the last HLE call;
- every guest thread: cia, lr, r3, the last syscall (r11, by name), and the
  HLE call it is inside;
- SPU program runs per lifted image;
- CPU per host thread over the sample window, with the hot function;
- the calls each guest thread finished in that window;
- the host threads; the last 60 SPU history events; the knobs that are set.

These are names, counters and addresses only. It never reads guest memory.

The hang watchdog also writes one snapshot to the log after 10 s without a
new frame (`PS3_FREEZE_SNAPSHOT=<s>`, 0 = off), between `[freeze]` lines. So
even a log from a game the player killed shows where it stopped.

## When the game freezes

The game can stop in two ways, and each has its own path:

**The game's threads stop, the render thread runs** (the usual hang: a
deadlock, a wait that never ends).
1. The runtime's ticker keeps running, and it calls the overlay's tick.
2. F1, or the gamepad chord (read by the port's watch thread, since the game
   no longer reads the pad), calls `rsx_d3d12_overlay_wake`. A quarter of a
   second with no Present means the game has stopped. The tick then copies
   the last frame shown out of its render target and calls the frame hook
   once over it.
3. The menu opens over the frozen frame, repaints come on, and **Report an
   issue** works as usual. Its `game.txt` says the game had stopped, and its
   `runtime.txt` shows where each thread is.
4. After 15 s without a frame, a notice comes up by itself: "The game is not
   responding. No new frame for N s. Press F1 or <chord> to open the menu,
   then choose Report an issue." It stays 30 s, with repaints on.

**The render thread stops too** (the overlay cannot draw).
1. The watch thread sees that a menu key it passed on got no frame within
   2 s.
2. It writes `crashes\hang-<time>.txt` (the snapshot), `.log` and
   `.cmdline`, and starts the dialog in its hang form: "The game is not
   responding", with **Keep waiting / Report on GitHub / Show details / Close
   the game**.
3. The dialog opens once per freeze by itself (through the 15 s notice's
   wake). After **Keep waiting**, only another F1 or chord opens it again.

**The graphics device is lost** (2026-10-27, issue #205; docs/111 section 8).
A driver reset (TDR) or a GPU that goes away leaves the game running with
nothing to draw with. No frame stops and the overlay cannot draw, so neither
path above sees it.
1. The watch thread asks `rsx_d3d12_device_lost()` (ps3recomp#142) every
   250 ms.
2. Once it says so, the thread writes `crashes\graphics-<time>.txt` (the
   reason, then the snapshot), `.log` and `.cmdline`. It then starts the
   dialog in its lost form: "The game can no longer draw", with
   **Restart game / Report on GitHub / Show details / Close the game**.
   Restart closes the game first. The report attaches it as
   `graphics-lost.txt`.
3. On Windows, every report's `system.txt` also counts the System log's
   graphics driver resets (`Display` event 4101) of the last 7 days, with the
   time of the last.

One fix was needed for the first path: a repaint used to wait until the
previous Present was taken. A window the compositor is not showing (behind
another window, or covered) never takes it, so the overlay froze there. Once
the game has stopped, the repaint no longer waits.

## Getting the zip attached (2026-11-06)

A link can fill in an issue's text, but GitHub has no way to attach a file
from one. So the player has to drag the zip in themselves, and most reports
arrived without it. Two parts on GitHub's side:

- **The report form**, `.github/ISSUE_TEMPLATE/report.yml`, has three boxes:
  - "What happened" (required);
  - "Report file (.zip)" (required, empty, so the issue can't be sent until
    something is in it);
  - "Details" (filled in by the program).
- **The reminder**, `.github/workflows/report-zip.yml` with
  `.github/scripts/report_zip.py`:
  - **Which issues:** the form's, plus older versions' reports, which name
    their zip (`setup-…`, `issue-…`, `crash-…`).
  - **A new report with no `.zip` attached** gets the label "needs report
    zip" and one comment saying where the file is.
  - **A zip attached later**, in the text or in a comment, takes the label
    off.
  - **Later events never add the label again**, so a label taken off by
    hand stays off.
  - **Local test:** `REPORT_ZIP_DRY=1` prints what the script would do, with
    the comments read from `REPORT_ZIP_COMMENTS`.

The form is read from the default branch only, so it takes effect once
merged. The game's links open it (`template=report.yml`) from the next
version on; older versions keep opening a blank issue with the text in it.

## Privacy and copyright

Reports are posted on a public GitHub issue, so:

- **The save, and nothing else of the game.** The save holds the player's
  characters and their names, and it is posted with the issue, so the
  report's texts say so (the menu's box, the crash dialog, the README). The
  port writes its own `PARAM.SFO` (setup's import too), so no PSN account is
  in it. Its two pictures are the game's art and stay out.
- **No game files or guest memory.** The runtime
  snapshot holds thread names, counters and addresses only. The log holds the
  runtime's and the port's own messages. The only game content is the
  screenshot: one frame the player was looking at, which they can see before
  they submit. *Open question for the maintainer: keep it, or leave it out.*
- **Nothing about the player.** `report::redact` replaces the profile folder,
  user name, computer name, e-mail addresses and every absolute path (down to
  its last name). It also replaces the online name, because the runtime logs
  it (`[sceNp] Init(... username=...)`): `<online name>`. When no name is
  set, the runtime's default (`PS3Player`) stays.
  In the logs (`des.log`, `des.prev.log`, setup's logs), `report::redact_log`
  also replaces the user and computer names inside longer words: two
  threads' lines can interleave inside a folder's name, and then no whole
  folder or word is left to find (seen 2026-11-04, a name glued to
  "Documents"). An issue's own text keeps whole words only, since there
  the user name can be part of the project's link.
- The online name, matchmaking password and custom server address (and its
  mark, `net_custom_here`) appear only as `(set)`, in `settings.ini` (both
  report kinds) and in
  `environment.txt` (`DES_NET_NAME`, `DES_NET_PASSWORD`, `DES_NET_CUSTOM`,
  `PS3_NP_ONLINE_ID`, `PS3_NET_PASSWORD`).
- Nothing is sent by the game. The zip is saved and the issue form opens in
  the browser. The player decides whether to attach the zip and submit.

## The report form, and the first report's question (2026-11-06)

**The links.** All three report links open the repository's report form,
`.github/ISSUE_TEMPLATE/report.yml` (`report::issue_url`):
- the title;
- "What happened": setup's error text, and left to the player for the
  game's and the crash dialog's reports;
- "Details": the system block, the crash's stack, and the zip's name.

The form's "Report file (.zip)" box is left empty, so GitHub won't send the
issue until the player has put something there. A link GitHub refuses as too
long is still cut, Details first.

**The question.** Before the first report on a PC, `report_ack.cpp` asks once:
- **The text:** GitHub can't attach the file, so drag it into the form's
  "Report file (.zip)" box; without it, a report can rarely be solved.
- **The tick:** "I'll attach the report file to my issue", a check box drawn
  with Material Symbols' `check_box` and `check_box_outline_blank`. The focus
  starts on it, and Continue waits for it.
- **Where it shows:** the menu's Report, the crash dialog's Report on GitHub,
  and setup's "Save and open the issue page".
- **The answer** is kept as `report_zip_ack.txt` in the data folder, which
  the game, the crash dialog and setup share.
  - It isn't a setting, because the crash dialog's process never reads
    `settings.ini` and must not write it.
  - A test run with no data folder keeps nothing, so it asks every time.
- **On a Steam Deck's Game Mode** the text says the link is saved beside the
  report, to open on a PC.

## Test knobs

All are live: the debug console's `setenv` sets them in a running game.

| Knob | Effect |
|---|---|
| `DES_HANG_AT_FRAME=n` | the game's flip thread stops for good once n frames were presented (a game hang; not on the render thread) |
| `DES_HANG_RENDER_AT_FRAME=n` | the render thread stops for good (the hang dialog's case) |
| `RSX_LOSE_DEVICE_AT_FRAME=n` | the D3D12 device is removed at the n-th frame, as a driver reset would lose it (the lost-graphics dialog's case; read at start, not live) |
| `RSX_FAIL_START_AT=<step>` | the new renderer's start fails at that step: `loader`, `instance`, `adapter`, `features`, `device`, `window`, `swap chain`, `targets`, `shaders` or `rings`. On Vulkan the game then starts on D3D12 and `system.txt` says why (docs/120; read at start, not live) |
| `DES_REPORT_AT_FRAME=n` | starts Report an issue at frame n, once per value |
| `DES_UI_PRESS=KEY` | presses F1, ESC, ENTER, UP/DOWN/LEFT/RIGHT, TAB, SPACE as the window procedure would, then clears itself. Test runs set `PS3_NO_USER_INPUT=1`, which drops keys posted to the window. |
| `DES_REPORT_NO_OPEN=1` | no browser or Explorer; the issue link goes to `<zip>.url.txt` |
| `GAMESCOPE_WAYLAND_DISPLAY` set, or `XDG_CURRENT_DESKTOP=gamescope` | not a knob: the session says so, as a Steam Deck's Game Mode does. Nothing is opened (no desktop to open it on, and a browser could take the screen); the issue link goes to `<zip>.url.txt`, and a toast says where the zip is and to send it from Desktop Mode. `system.txt` has a `Session:` line |
| `DES_CRASH_DIALOG_REPORT=1`, `_DETAILS=1`, `_SHOT=<bmp>`, `_EXIT_MS=<ms>` | the dialog makes the report (past the first report's question) or presses Show details by itself, saves its picture, closes after that long |
| `DES_CRASH_DIALOG_REPORT=ask`, `DES_CRASH_DIALOG_SHOT_FRAME=<n>` | the dialog presses Report on GitHub as the player does, so the first report's question shows when it hasn't been answered; the picture is taken at frame n (default 40) |
| `PS3_FREEZE_SNAPSHOT=<s>` | seconds without a frame before the log snapshot (default 10, 0 = off) |

With `PS3_NO_USER_INPUT=1` the hang dialog shows without taking the focus.

## Verification (2026-10-21)

Runs on the pinned regression save, in a scratch data folder each:

- **Game hang:** `setenv DES_HANG_AT_FRAME=1` in the level.
  - The log has the snapshot at 10 s (HighGraphics in
    `_cellGcmSetFlipCommand`; the ticker in `cellGcm_fifo_kick_wait`).
  - The notice was drawn over the frozen frame at 15 s, and its countdown
    kept moving.
  - `DES_REPORT_AT_FRAME=1` made a zip whose `game.txt` read "no new frame
    for 17.6 s: the game had stopped", with frames per second dropping from
    60 to 0.
  - `DES_UI_PRESS=F1` opened the menu over the frozen frame, with Report an
    issue in it.
- **Render thread hang:** `setenv DES_HANG_RENDER_AT_FRAME=1`, then F1.
  - "[ui] no new frame for 5 s, and the menu cannot open: the hang dialog",
    logged once.
  - The dialog opened, and Report made `hang-<time>.zip` with `hang.txt`,
    `system.txt`, `settings.ini` (from `logs\settings.txt`),
    `environment.txt` and `des.log`.
  - The issue link had the "Not responding" title and the guest thread table.
- **Normal play:** Report an issue made the full zip as listed above.
- **Redaction:** in every zip, a search for the user name, computer name,
  e-mail, `C:\Users` and the project path found nothing.
