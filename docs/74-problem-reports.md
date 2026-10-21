# 74. Problem reports: what they hold, and reporting a frozen game

docs/32 describes the data folder, the log, the crash dialog and **Report an
issue** as first built. This page covers what was added on 2026-10-21:

- more in each report, so a problem can be debugged from the zip alone;
- the menu still opens when the game freezes, so a frozen game can be
  reported too;
- the rules that keep private data and the game's content out of reports.

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
| `system.txt` | port version (and whether it is a developer build), ps3recomp version, the game dump's SHA-256, Windows version, CPU (cores, threads, instruction sets), memory (the PC's and this process's), each GPU with its driver (and the one in use with its memory budget), each display (size, refresh, primary, HDR), power (battery or not), free disk space where the game and the data are, gamepads (XInput, DualSense/DS4), keyboard layout, and other software loaded into the game (overlays, hooks) |
| `game.txt` | time running; game frames, with "no new frame for N s: the game had stopped" when it has; frames per second for each of the last 60 s; overlay frames; window or fullscreen, size and DPI; picture size; crash and hang reports on this PC in the last 7 days (names only) |
| `settings.ini` | every setting with its value, `# changed; default: X` where it differs from the default, and `# set in the environment: X` where the environment overrides it |
| `environment.txt` | the command line and every port or runtime option set in the environment (`DES_`, `PS3_`, `RSX_`, `SPU_`, `SPURS_`, `GCM_`, `PAD_`, `PPU_`, `AUDIO_`, `VBLANK`, `FIFO_`, `LIBSRE_`, `SAIL_`, `TROPHY_`, `WATCHDOG_`) |
| `runtime.txt` | the runtime's snapshot (below), sampled over 2 s |
| `des.log` | the first 120 lines of the log and its last 512 KB |
| `des.prev.log` | the end of the previous run's log, when that run was in the last 24 h: a freeze or crash the player restarted after |

The crash and hang dialog's **Report on GitHub** zips `crash.txt` or
`hang.txt`, `system.txt`, `settings.ini`, `environment.txt` (the game's
command line, saved with the report) and `des.log`. The dialog runs as a
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

One fix was needed for the first path: a repaint used to wait until the
previous Present was taken. A window the compositor is not showing (behind
another window, or covered) never takes it, so the overlay froze there. Once
the game has stopped, the repaint no longer waits.

## Privacy and copyright

Reports are posted on a public GitHub issue, so:

- **Nothing of the game.** No game files, saves or guest memory. The runtime
  snapshot holds thread names, counters and addresses only. The log holds the
  runtime's and the port's own messages. The only game content is the
  screenshot: one frame the player was looking at, which they can see before
  they submit. *Open question for the maintainer: keep it, or leave it out.*
- **Nothing about the player.** `report::redact` replaces the profile folder,
  user name, computer name, e-mail addresses and every absolute path (down to
  its last name). It also replaces the online name, because the runtime logs
  it (`[sceNp] Init(... username=...)`): `<online name>`. When no name is
  set, the runtime's default (`PS3Player`) stays.
- The online name, matchmaking password and custom server address appear
  only as `(set)`, in `settings.ini` (both report kinds) and in
  `environment.txt` (`DES_NET_NAME`, `DES_NET_PASSWORD`, `DES_NET_CUSTOM`,
  `PS3_NP_ONLINE_ID`, `PS3_NET_PASSWORD`).
- Nothing is sent by the game. The zip is saved and the issue form opens in
  the browser. The player decides whether to attach the zip and submit.

## Test knobs

All are live: the debug console's `setenv` sets them in a running game.

| Knob | Effect |
|---|---|
| `DES_HANG_AT_FRAME=n` | the game's flip thread stops for good once n frames were presented (a game hang; not on the render thread) |
| `DES_HANG_RENDER_AT_FRAME=n` | the render thread stops for good (the hang dialog's case) |
| `DES_REPORT_AT_FRAME=n` | starts Report an issue at frame n, once per value |
| `DES_UI_PRESS=KEY` | presses F1, ESC, ENTER, UP/DOWN/LEFT/RIGHT, TAB, SPACE as the window procedure would, then clears itself. Test runs set `PS3_NO_USER_INPUT=1`, which drops keys posted to the window. |
| `DES_REPORT_NO_OPEN=1` | no browser or Explorer; the issue link goes to `<zip>.url.txt` |
| `DES_CRASH_DIALOG_REPORT=1`, `_SHOT=<bmp>`, `_EXIT_MS=<ms>` | the dialog presses Report by itself, saves its picture, closes after that long |
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
