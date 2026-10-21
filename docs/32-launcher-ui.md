# 32. The wrapper UI (start screen, in-game menu, settings)

The same wrapper UI as FFCCEOT-Recomp (which took it from Dusklight), with a
Demon's Souls theme. Code in `port/ui/src`, styles and fonts in
`port/ui/res`, the embedding helper in `port/cmake/EmbedFiles.cmake`,
licences in `port/ui/NOTICE.md`.

## What it does

- **Start screen** over the game as it boots: "DEMON'S SOULS", a silver
  rule, *PC port*, then **Play**, **Settings**, **Trophies** and **Quit**. The game's
  logos and notices run behind it, darkened from the left; it gets no input
  until Play. Esc or Circle on the start screen asks to quit.
- **In-game menu**: F1 or Esc on the keyboard, or **L3 + R3** on a gamepad
  (Settings > Interface can switch this to Select + Start). It is a bar at
  the top with **Settings**, **Trophies**, **Report an issue** and **Quit**. Esc, Circle,
  F1 or Start close it.
- **Quit** asks first ("Return to the desktop? Progress since the game last
  saved will be lost.") in a blood-red window, then posts WM_CLOSE to the
  game window. The window's close button and Alt+F4 ask the same question
  (the UI consumes their WM_CLOSE). The window closes at once only in a run
  nobody plays: a tools/des test run, `PAD_FILE` or `EXIT_AFTER_MS`.
- **Settings**: tabs Display, Game, Controls, Interface, About, the left
  pane showing the value, the right pane showing the choices and help. Rows
  that only apply at the next start say so. A row whose option was set in
  the environment shows that value, can't be changed, and its help says why.
- **Trophies** (`trophies_window.cpp`, docs/44 section 5.4): the trophy set's
  summary (unlocked n of 38, the percentage, the count per grade) over a list
  of every trophy: its icon (dimmed while locked), name, description, grade,
  and the unlock date or "Locked". The list comes from the sceNpTrophy HLE
  (`sceNpTrophy_snapshot`, which works before the game registers its
  trophies, so the window opens from the start screen too) and is read again
  on each unlock. A hidden trophy that is still locked shows as "Hidden
  trophy", with no name, description or icon; pressing its row (Cross, Enter
  or a click) reveals it until the window closes, and the first row, *Show
  hidden trophies*, shows them all. Demon's Souls has no hidden trophy, so
  that row is disabled ("None in this game"). Up and Down move between rows,
  L1 and R1 (Page Up, Page Down) six rows at a time.
- After Play, a toast at the top says how to open the menu (setting *Menu
  hint*).

While any of it is showing, the game sees neutral pads
(`cellPad_set_host_hold(1)`) and gets the input back only once every game
key, F1, Esc, the mouse buttons and every pad button and stick are back at
rest, so the press that closed the menu never reaches the game. Toasts (the
menu hint, trophy pop-ups) are the exception: while one shows, the game
keeps its input, Start included, and F1, Esc or the gamepad chord still
open the menu.

## Settings

`settings.ini` (`key = value`) lives in the data folder (below), or wherever
`DES_SETTINGS` points. Keys are the run options' own environment variable
names. At startup, before the game reads its options, each value becomes its
environment variable unless that variable is already set: **explicit
environment always wins.** Changes are saved when the settings window closes
and on every change.

| Row (tab) | Key | Values | Applies |
|---|---|---|---|
| Frame rate (Display) | `DES_FPS` | "" = 30 fps original, 30, 60, 120, 144, 165, 240 | at once (read at each flip request) |
| Cutscene frame rate (Display) | `DES_CUTFPS` | 0 = 30 fps (original), 1 = the frame rate, the camera blended between its keys | at once (read at each flip request) |
| V-sync (Display) | `DES_VSYNC` (sets `RSX_VSYNC`) | 1 / 0 | at once (live knob) |
| Boot logos (Game) | `DES_SKIP_INTRO` | 0 = show, 1 = skip, 2 = last logo only | next start |
| Skip opening movie (Game) | `DES_SKIP_INTRO_MOVIE` (sets `PS3_SAIL_SKIP`) | 0 / 1 | from the next movie |
| Continue automatically (Game) | `DES_AUTO_CONTINUE` | 0 / 1 | next start; from the file it waits for Play on the start screen |
| Sound (Game) | `AUDIO_MUTE` (inverted) | 0 / 1 | at once (live knob) |
| UI scale (Interface) | `ui_scale` | 0 = auto (the display's scale), 75, 100, 125, 150 (% of it) | at once |
| Start screen (Interface) | `show_start_screen` | 1 / 0 | next start |
| Menu hint (Interface) | `menu_hint` | 1 / 0 | at once |
| Trophy pop-ups (Interface) | `trophy_popups` | 1 / 0 (off hides the toast; the unlock is still kept) | at once |
| Show hidden trophies (the Trophies window) | `trophies_show_hidden` | 0 / 1 (1 lists hidden trophies with their names before they are unlocked) | at once |
| Gamepad menu buttons (Interface) | `pad_menu_chord` | 0 = L3 + R3, 1 = Select + Start | at once |

Live values go through `ps3_setenv` (the runtime's getenv cache and
PS3_KNOB). Controls is information only (the menu keys, the keyboard pad
map); rebinding is not done. About has the version (git short hash at
configure time), the licence, the settings file's path and the credits.

`play.sh` sets `AUDIO_MUTE=0` for every session, so under play.sh the Sound
row shows as set from outside. Launching the exe directly, it is a normal
setting.

### Frame rate, live

des_options.cpp reads `DES_FPS` (a PS3_KNOB) at every flip request, so the
row, or the debug console's `setenv DES_FPS=60`, takes effect on the next
frame. A change sets the game's FlipMode bytes, the real frame step and the
vblank (61-240: the vblank at that rate, so it is the cap) as the startup
path did; the empty value or 0 puts back the bytes and step the game had
before the first change and hands the vblank back to the PS3's 59.94 Hz. A
`VBLANK_MHZ` given at start still wins over the cap. Cutscenes still drop to
30 while they play.

## The window

- **Its own thread.** The window and its message loop run on a thread of
  their own (`RSX_WINDOW_THREAD=1`, the default). The vblank thread used to
  pump the messages, so while Windows ran its modal loop for a drag or a
  resize the vblank stopped and the game froze: 0 flips during a 4 s move
  before, 128 after.
- **Any size.** The window can be resized (a drag stops at a 640x360 client
  area, at the display's scale). The game still draws at 1280x720; each
  frame is scaled with a linear filter into a swap chain the size of the
  window, letterboxed to 16:9. The UI draws over that at the window's size.
- **UI size by the display, not the window**, as in FFCCEOT-Recomp
  (Dusklight's rule): 1 dp is the display's scale (the window's DPI / 96)
  times the UI scale setting. A larger window gives the menus more room
  around them; their text stays the same size. The window is per-monitor
  DPI aware, so on a display scaled above 100% it is drawn at the
  display's resolution (not stretched by Windows) and the menus are as
  large as other programs' text; moving it to a display of another scale
  takes the size Windows suggests. (It used to be the window's height /
  720, which made everything grow with the window.)
- It opens centred on the primary display (its size the title's 1280x720
  at the display's scale, made smaller if it would not fit), with a dark
  title bar (`RSX_DARK_TITLE=0` for the system's).
- **Alt+Enter** switches to borderless fullscreen on the window's monitor
  and back.
- A resize happens between frames: the backend waits for the GPU, calls the
  overlay's `release` hook (the UI finishes its work on the buffers and drops
  its views of them), then ResizeBuffers.

## The on-screen keyboard

When the game asks for text (the character's name), the UI shows its own
keyboard in place of the small Windows input box. ps3recomp's cellOskDialog
hands the dialog to the port (`cellOskDialog_set_host`), and
`osk_window.cpp` draws it. The window shows the game's prompt, the text
with a counter against the game's limit and a cursor, five rows of keys, and
a row of the pad's buttons with what they do.

- **Gamepad.** The buttons are the PS3 keyboard's (as RPCS3 has them too).
  The D-pad moves to the nearest key in the next row, and Cross types the key.
  Square is Backspace, Triangle types a space, Select is Shift, L1/R1 (and
  L2/R2) move the cursor, Start is Enter and Circle is Back. Square and
  Triangle repeat while held.
- **Keyboard.** Typing goes straight into the text at the cursor. Backspace
  and Delete delete, Home/End move the cursor, Enter finishes and Esc goes
  back. The arrows move on the grid.

The game gets no pad input while the keyboard is up, as with any menu. With
`DES_UI=0`, or when the UI fails to start, the Windows input box is used, or
the initial text is given back. `PS3_OSK_TEXT` still answers at once for
scripted runs.

## Startup work: the shader cache

The window opens as soon as the program starts (about 0.1-0.2 s), and the
vblank thread presents before the game's first flip, so the UI can draw
while nothing else does. The work the game needs before it runs happens
there, behind a progress bar: `port/des_prework.cpp`, called through
boot_main's `ps3_title_prework` hook (weak, empty for other titles) just
before the game's entry point.

Today that work is the shader cache. It reads the game's shader archives
(`USRDIR/shader/*.shaderbnd`, BND3) and hands every Cg program to
ps3recomp's `rsx_shader_precompile` (one decompile thread feeding a compile
thread per logical processor but one), which fills `gamedata/shader_cache`
as `tools/des/precompile_shaders.sh` did before the game started. The
**Preparing the shaders** window (`prework_modal.cpp`) shows the bar, the
count compiled and already prepared, and **Skip** (Esc or Circle too):
what is left then compiles as the game draws it.

`precompile.stamp` in the cache records what a full pass saw: the
executable (its PE link time and image size, since tools/des copies the exe
each run), the texture-unit kinds, each archive's size and time, and
`fp_variants.txt`. When nothing changed the pass is skipped and no window
shows; the game adds to fp_variants.txt as it draws, so the next start
builds the new variants. A skipped or failed pass writes no stamp.

| Knob | Effect |
|---|---|
| `DES_PRECOMPILE=0` | No startup work. |
| `DES_PRECOMPILE=force` | Run the pass whatever the stamp says. |

play.sh no longer runs precompile_shaders.sh; the script still fills a
cache from a shell.

## Data folder, log and reports

One folder holds everything the port keeps for the player, as in
FFCCEOT-Recomp (`report::data_dir()`, report.cpp):

- normally `%APPDATA%\DemonsSoulsRecomp\DemonsSouls\` (where
  SDL_GetPrefPath would put it);
- with a `portable.txt` next to `des_boot.exe`: `<exe folder>\data\`;
- `DES_DATA_DIR=<folder>` overrides both (tests).

| In the data folder | What |
|---|---|
| `settings.ini` | the settings (unless `DES_SETTINGS` names another file) |
| `logs\des.log`, `logs\des.prev.log` | this run's output and the previous run's |
| `crashes\crash-<time>.txt/.dmp/.log/.cmdline` | a crash: what happened and the stack, a minidump, the end of the log, the command line (for Restart) |
| `reports\crash-<time>.zip`, `reports\issue-<time>.zip` | what a GitHub issue gets attached |

The game's saves are not in it: they stay in the PS3 file system the runtime
maps (`gamedata/dev_hdd0/home/00000001/savedata`), as before.

A tools/des test run (`PS3_PIDFILE` set and `DES_KIND` not `play`) has no
data folder unless `DES_DATA_DIR` is given: no settings.ini, no log, no
crash handler, no reports, exactly as before. play.sh sessions and a
directly started exe have one.

### The log

Started before `main()` (the startup hook in crash_report.cpp, see below):
stdout and stderr go into pipes; a thread per pipe writes what comes out to
where the output was going before (the console, or play.sh's
`work/tmp/play.txt`, unchanged, still CRLF) and to `logs\des.log`. One pipe
for both when they went to the same place, so the log keeps their order.
The log file is buffered (64 KB) and flushed whenever its pipe is empty; the
pipes hold 1 MB, so a burst of messages doesn't wait on the console. The
previous run's log is kept as `des.prev.log`; a second copy of the game
running at the same time writes `des.<pid>.log`. Past 128 MB the log keeps
its first 2 MB and goes on from there.

The runtime turns its per-event messages on when stderr is not a console
(ps3_log.h). Since stderr is now a pipe, the tee sets `PS3_VERBOSE=0` when
it was a console and `PS3_VERBOSE` is unset, which keeps the old behaviour.

### Crash reports

`crash_report::install()` (first thing in `des_ui_early_init`, also with
`DES_UI=0`) chains a top-level exception filter and a SIGABRT handler after
boot_main's own. No vectored handler: the runtime commits guest memory and
runs its guard pages through access violations that its vectored handlers
resolve, and those never reach the filter. Only an exception nothing
handled does. On one:

1. boot_main's filter (or abort handler) runs first, as before: `[CRASH]`
   lines with the guest state go to the log.
2. `crashes\crash-<time>.txt`: what happened, the last HLE call, registers
   and the stack (dbghelp; function names only from a matching .pdb,
   otherwise `des_boot.exe+<rva>`, which `des_boot.map` resolves); `.dmp` (a
   small minidump: threads, stacks, referenced memory; not the 4 GB guest
   space), `.log` (the head and last 256 KB of des.log), `.cmdline`.
3. The crash dialog: `des_boot.exe --crash-dialog <txt> <pid> <data folder>`
   is started (no console window, with the environment the game started
   with), and the game ends as it did before (boot_main's filter's result;
   `_exit(3)` after an abort).

The dialog process never starts the game: a startup hook in the C runtime's
initializer table (`.CRT$XCV`, after every ordinary static initializer,
before `main()`) sees `--crash-dialog`, shows the dialog and exits. The same
hook in a normal run saves the environment block and starts the log. The
dialog is the menu's own modal ("The game has crashed", the error, Restart
game / Report on GitHub / Show details / Close) drawn by the same host
(host.cpp) on a D3D12 device and swap chain of its own, with the mouse, the
keyboard or an XInput pad. **Report on GitHub** zips `crash.txt`,
`system.txt`, `settings.ini` and `des.log` (all redacted) into
`reports\crash-<time>.zip`, opens a new issue on
https://github.com/JonathanDC64/DemonsSoulsRecomp with the title, the error,
the top of the stack and the system filled in, and shows the zip in
Explorer. **Restart game** waits for the crashed process to end and starts
the saved command line again.

### Report an issue

What the reports hold now (more than below), and how a frozen game is
reported: docs/74.

The menu bar's **Report an issue** asks first (what is sent, and that
nothing is sent until the issue is submitted). **Report** closes the menu;
4 presented frames later, inside the frame hook and before the UI is drawn,
the back buffer is read (`host::capture`), so the picture is the game's, not
the menu's. A thread then writes the PNG (stb_image_write), zips `screenshot.png`, `system.txt`, `game.txt` (frames, picture
size), `settings.ini` (the settings in use, with the values the environment
set noted) and the head and last 192 KB of `des.log` into
`reports\issue-<time>.zip`, opens the issue form filled in and shows the
zip. Without a data folder (a test run) there is no report.

What goes into a zip or a link is redacted (`report::redact`): the profile
folder, user name, computer name, the PSN name given with `--username`,
e-mail addresses, and every absolute path (drive, UNC, `%USERPROFILE%` and
Git Bash `/c/...` forms) down to its last name (`<path>\EBOOT.ELF`). The
issue text is cut to fit a link (about 6.5 KB encoded); the zip has the rest.

## Run options (knobs)

| Knob | Effect |
|---|---|
| `DES_UI=0` | The UI is not installed at all (no overlay, no settings.ini). |
| `DES_SKIP_UI=1` | No start screen this run (the menu is still on F1). |
| `DES_UI_START=1` | Start screen even on a scripted or test run. |
| `DES_SETTINGS=<path>` | Use this settings.ini (also makes test runs read and write one). |
| `DES_DATA_DIR=<folder>` | The data folder (settings.ini, log, crashes, reports); also gives a test run one. |
| `DES_NO_CRASH_DIALOG=1` | A crash writes its report but shows no dialog. |
| `DES_CRASH_AT_FRAME=<n>` / `DES_ABORT_AT_FRAME=<n>` | Crash (access violation) / abort at presented frame n (tests). |
| `DES_REPORT_AT_FRAME=<n>` | Start an issue report at presented frame n, as the menu's Report does. |
| `DES_REPORT_NO_OPEN=1` | Reports open neither the browser nor Explorer; the issue link goes to `<zip>.url.txt`. |
| `DES_CRASH_DIALOG_REPORT=1` | The crash dialog presses Report on GitHub by itself (its frame 10). |
| `DES_CRASH_DIALOG_SHOT=<file.bmp>` | The crash dialog writes its picture (its frame 40). |
| `DES_CRASH_DIALOG_EXIT_MS=<ms>` | The crash dialog closes itself after that long. |
| `DES_UI_OPEN=menu` / `settings` / `trophies` / `osk` | Open the menu bar, the settings window, the Trophies window or a test of the on-screen keyboard at startup. |
| `DES_UI_SHOT=<a.bmp>[,frame][;<b.bmp>,frame...]` | Write the back buffer, UI included, at that presented frame (default 300). |
| `DES_UI_KEYS=<frame>:<KEY>[,...]` | Press keys at presented frames: F1 ESC UP DOWN LEFT RIGHT ENTER PGUP PGDN TAB SPACE. OSK opens the on-screen keyboard as the game would, and a digit (0-9) is typed, for a number field. With DES_UI_SHOT, a test can walk the menus. |
| `DES_UI_NO_ANIM=1` | No transitions. |
| `DES_UI_RENDER=d3d11` | The menus and the crash dialog draw through D3D11On12, as before the D3D12 renderer (Windows; kept for one release, with no settings row). |
| `RSX_OVERLAY_REPAINT=0` | The UI moves only with the game's frames, not at the display's refresh rate (live; "The UI's frame rate" below). |
| `DES_UI_SOUND=0` | No UI sounds. |
| `DES_PRECOMPILE=0` / `force` | No startup shader pass / one whatever the stamp says (above). |
| `DES_TROPHY_TEST=<id>` | Unlock trophy `<id>` (0-127) through the sceNpTrophy HLE as the game would (unlock file, platinum rule, pop-up), once the game has registered its trophies. Also live: the debug console's `setenv DES_TROPHY_TEST=<id>`; each new value unlocks once. It is a real unlock in the unlock file, so point `PS3_TROPHY_DIR=<folder>` at a scratch folder. |
| `DES_TROPHY_HIDDEN=<id>[,<id>...]` | The Trophies window treats these trophies as hidden, as if the trophy set said so (Demon's Souls has none), so a hidden row and revealing it can be seen and tested. UI only: the HLE, the pop-ups and the unlock file don't see it. Read when the window opens. |

No start screen when the run is scripted: a tools/des test run
(`PS3_PIDFILE` set and `DES_KIND` not `play`), `DES_AUTO_CONTINUE` on the
command line, `PAD_FILE` or `EXIT_AFTER_MS`. `DES_AUTO_CONTINUE` from
settings.ini keeps the start screen: its presses wait until Play
(`des_ui_start_screen_up`). The close button closes at once only in a test
run, with `PAD_FILE` or with `EXIT_AFTER_MS` (`unattended_run`); with
`DES_AUTO_CONTINUE` on the command line it still asks. Test runs also neither read nor write
settings.ini unless `DES_SETTINGS` (or `DES_DATA_DIR`) is given, so regress,
levelcap, exeab and friends see exactly the options they pass; they also
keep no log and install no crash handler.

A crash test that leaves nothing open (the dialog child is a des_boot.exe
too: run the game from a copy outside the tools/des staging folders, or
des_kill_own takes the dialog down with the game):

```
DES_DATA_DIR=<scratch>/data DES_CRASH_AT_FRAME=300 DES_CRASH_DIALOG_REPORT=1 DES_REPORT_NO_OPEN=1 \
  DES_CRASH_DIALOG_SHOT=<scratch>/dialog.bmp DES_CRASH_DIALOG_EXIT_MS=10000
```

and a report through the menu (no transitions, so the keys land in time):

```
DES_DATA_DIR=<scratch>/data DES_UI_NO_ANIM=1 DES_REPORT_NO_OPEN=1 \
  "DES_UI_KEYS=300:F1,320:RIGHT,340:ENTER,380:RIGHT,400:ENTER"
```

Example, the screenshots used while building it (desrun, locked session):

```
DES_SCRATCH=<scratch> RSX_PRESENT=0 bash tools/des/desrun.sh 75 <scratch>/run.txt DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 \
  DES_UI_START=1 DES_SETTINGS=<scratch>/settings.ini \
  "DES_UI_SHOT=<scratch>/start.bmp,120;<scratch>/set60.bmp,380" \
  "DES_UI_KEYS=140:DOWN,160:ENTER,220:DOWN,280:ENTER,300:DOWN,320:DOWN,340:ENTER,400:ESC"
```

## How it is built

| FFCCEOT | Here |
|---|---|
| GL3 renderer in the game's GL context | RmlUi's D3D11 renderer rewritten for D3D12 (`render_d3d12.cpp`, docs/76) on the game's own device and queue: the UI is drawn into its own layer and composited (premultiplied) over the back buffer, one command list a frame, submitted ahead of Present. For one release, `DES_UI_RENDER=d3d11` draws the old way: RmlUi's stock D3D11 renderer (`Backends/RmlUi_Renderer_DX11`) on a D3D11On12 device (`render_d3d11.cpp`), each swap-chain buffer wrapped (`CreateWrappedResource`, PRESENT in and out) with an RTV; per frame: Acquire, BeginFrame, Render, EndFrame(rtv), Release, Flush. See "The menu renderer" below. |
| SDL3 window events and gamepads | **No SDL.** The window procedure hook queues keyboard and mouse messages (RmlUi's Win32 helpers convert the keys); the gamepads are XInput plus the runtime's `pad_hid_sony_poll`, the same readers and mapping the game's pad uses. |
| `Presenter::present` callback | `rsx_d3d12_set_overlay` (ps3recomp): `wndproc` (on the window thread), `frame` (on the vblank thread, with the window-sized back buffer) and `release` (before a resize) hooks. |
| data folder: the pref dir, or `data\` beside the exe with portable.txt; settings.ini, logs, crashes, reports | the same; `DES_DATA_DIR` in place of `--data-dir`; settings mapped onto the DES_* / PS3_* options, env wins; saves stay in the PS3 file system |
| `report::start_log` (tee), `crash_report` (filter, SIGABRT, minidump, `--crash-dialog` second process), report zip, redaction, issue link | the same, chained after boot_main's filter and abort handler and started from a `.CRT$XCV` hook (boot_main is ps3recomp's); the dialog in its own window and D3D12 swap chain instead of SDL + GL, drawn by the same renderer as the menus (it starts with the game's environment, `DES_UI_RENDER` included); the PNG through stb_image_write |
| start screen over generated art | over the game itself, booting behind it; gradient and vignette in RCSS, no images |
| ROM setup, Wii setup, presets, spell ring, update check, ui_script | not taken |
| game SFX through `cfie::queue_sound` | `g_des_ui_sound(int id)` (des_ui.h): des_menu_sfx.cpp queues the game's menu sound for the main thread (docs/33) |

Files:

- `des_ui.cpp`: `des_ui_early_init()` (called first in
  `ps3_title_options_init`, des_options.cpp), the overlay hooks, the input
  queue, gamepad reads, the input hold, the debug knobs.
- `host.cpp`: RmlUi's system and file interfaces (the embedded files),
  fonts, scale, the images given to `add_png`, the BMP screenshot, and the
  renderer (`renderer.hpp`): `render_d3d12.cpp`, or on Windows
  `render_d3d11.cpp` with `DES_UI_RENDER=d3d11`. What differs by platform is
  three small pieces at its top: the system interface (RmlUi's Win32 one, or
  SDL's cursor), the window's DPI and the renderers there are. Images are
  decoded with stb_image.
- `trophy_toasts.cpp`: trophy pop-ups (docs/44). The sceNpTrophy HLE's unlock
  hook queues on the game's thread; `frame` wakes for it and shows a toast
  with the trophy's icon, grade and name. `DES_TROPHY_TEST`. It also counts
  the unlocks and keeps the grade names and the icon lookup (each icon read
  from the set once) that the Trophies window uses.
- `trophies_window.cpp`: the Trophies window (docs/44, section 5.4): the
  summary, the rows, hidden trophies and revealing them,
  `DES_TROPHY_HIDDEN`; `trophies.rcss` is its style.
- `settings.cpp`: settings.ini, the option table, pinned (env) detection,
  harness detection.
- `report.cpp`: the data folder, the log tee, system facts, redaction, the
  zip writer, the issue link, the PNG, "Report an issue" and the test knobs.
- `crash_report.cpp`: the startup hook, the filter and abort handler, the
  report and minidump, the dialog process. `crash_dialog.cpp`: its window,
  D3D12 swap chain and modal.
- `settings_window.cpp`, `prelaunch.cpp`, `menu_bar.cpp`, `overlay.cpp`,
  `prework_modal.cpp`: the documents. `input.cpp`: pad state to UI keys with repeat. The rest is
  Dusklight's framework (document stack, windows, panes, buttons, modals,
  tabs), unchanged apart from its headers.
- `des_ui` is its own static library linked into des_boot: des_boot's
  compile flags don't change (the lifted chunks don't rebuild). Third-party
  code is built with `/O2` (the port configures with an empty build type).

### Cost when hidden

The frame hook's idle path is a few loads: not awake, no queued input, no
fresh chord in `g_pad0_buttons` (pad 0 as the game last read it), no
screenshot due. It then returns: no RmlUi update, no pad reads, no GPU work.
After the last document hides it stays awake about 0.6 s for the closing
transitions and while the hint toast shows, then sleeps again. The renderer
and RmlUi are created lazily, the first time the UI wakes (or on the
first frame when the start screen shows).

Measured with `levelcap` A/B (the same exe, `DES_UI=0` against the UI
installed and hidden), see the end of this file.

### The UI's frame rate

The UI is drawn in the overlay's `frame` hook, so on its own it would move only
when the game presents a frame: 30 times a second at the original frame rate,
and not at all through a hitch. While a document is up (and through its
closing fade), `frame` turns on `rsx_d3d12_overlay_repaint` (ps3recomp): the
render thread then shows the last game frame again between the game's own,
with the UI drawn on it, so the menu moves at the display's refresh rate.
This is what FFCCEOT-Recomp does as well: its game thread presents once per
display refresh, with the 3D blended between emulated frames, and the UI is
drawn on every present.

How the runtime does it:

- Each frame the game presents is copied to a held texture before it is
  scaled into the window (one copy per game frame, only while repaints are on).
- At the render thread's idle point (the ticker, between FIFO drains) a
  repaint is made when a refresh has no frame yet and is at least half over,
  and the swap chain has no frame waiting to be shown. A frame the game
  presents earlier in that refresh shows instead, so the game's frames are
  never held back. The refresh period and phase come from DWM
  (`DwmGetCompositionTimingInfo`), else from the display mode.
- A repaint does not count as a game frame: not in the title bar's FPS, not
  in `FRAME_LOG`, not in `RSX_CAP`'s frame numbers. `GCM_RATES=1` prints
  them as `repaints=` per second.
- `RSX_OVERLAY_REPAINT=0` (live) turns repaints off. Scripted UI runs
  (`DES_UI_KEYS`, `DES_UI_SHOT`) leave them off, as they count frames.

A hitch on the render thread itself (a shader compiled there) still stops
the UI; a hitch anywhere else (the game's threads, loading) does not.

Measured 2026-10-20 on a 144 Hz display, the Settings window open
(`DES_UI_OPEN=settings`, `GCM_RATES=1`): at 30 fps, 30 presents and about
114 repaints a second; at `DES_FPS=120`, 120 and 24. The game's rate was
the same with repaints off.

### Gamepad chord

L3 + R3 by default: Start and Select open the game's own menus, L3 is not
used by Demon's Souls, and R3 (lock-on) is the only game action the chord
touches; the game sees that press before the menu opens. While the UI is
showing, Start also toggles the menu.

### The menu renderer

Since 2026-10-21 the menus draw through the same D3D12 renderer on Windows
as on Linux (`render_d3d12.cpp`, docs/76), on the game's own device and
queue. Before, Windows drew them with RmlUi's stock D3D11 renderer on a
D3D11On12 device. That path stays for one release (`render_d3d11.cpp`),
behind the run option `DES_UI_RENDER=d3d11`, in case the D3D12 one shows a
problem on some PC; the release after, it goes. It has no settings row, so
players are not asked to choose. If the renderer asked for cannot start, the
other one is tried. The crash and hang dialog starts with the game's
environment, so it follows the option too.

Compared before the switch (2026-10-21, one exe, D3D12 against
`DES_UI_RENDER=d3d11`):

- **Shots** (`DES_UI_SHOT` at the same frames, over the game's still
  auto-save notice with its pulsing OK button masked). Pixel-identical: the
  settings window's seven tabs, Change Buttons, the menu bar, Trophies, the
  quit prompt over the game, the on-screen keyboard, the start screen, a
  closing fade, the crash and hang dialogs (their own swap chain, cleared),
  and the settings window after the window was resized to 1600x900 and
  960x540. The quit prompt over the start screen differs by at most 1 level
  in its box shadow, plus a 4x2 px dark spot that only D3D11 draws, at the
  left edge of the shadow over the title's rule (D3D12 leaves the background
  as it was there). A trophy pop-up in the title screen matches to 1 level. Shots in
  the middle of a transition differ with timing between any two runs.
- **Reports.** The screenshot is the game's frame without the menu on both.
  With `DES_UI_RENDER=d3d11` on the command line the hang dialog draws
  through D3D11On12 too (it loads `d3d11on12.dll`); with the default it does
  not.
- **Resize.** With the settings window open, the swap chain followed each
  size (1600x900, 960x540, 1280x720) with no errors, on both. Alt+Enter was
  not sent (test runs drop key messages), but it ends in the same resize
  (`swap_resize`, which calls the overlay's `release`).
- **Frame rate**, the settings window open over the deep Palace save,
  uncapped (vsync off, `DES_FPS=unlocked`, vblank 960 kHz), 6 rounds
  alternating, 2 windows of 8 s each: D3D12 130.6 fps, D3D11 130.6 fps (3
  rounds D3D12 first: 125.4 against 127.5; 3 rounds D3D11 first: 135.7
  against 133.6). The present phase, where the menu is drawn, took 0.31 to
  0.41 ms a frame through D3D12 and 0.52 to 0.59 ms through D3D11, in every
  window. The few long frames on either side were the game handing its
  frame in late, not the menu.

## Theme

`theme.rcss` tokens: text bone (215, 210, 195), accent near-white bone
(236, 232, 220), borders silver (148, 150, 154), surfaces near-black
(11, 11, 12) at 80-96 %, focus soul-white (200, 215, 225) at low alpha,
danger blood red (#6E1A17 border, #A8241D heading), and the trophy grades
(the pop-ups and the Trophies window): platinum (180, 205, 225), gold
(222, 184, 84), silver (192, 194, 198), bronze (196, 128, 72). Borders 1 dp, corners
2-4 dp. Cinzel for headings and the title, Alegreya Sans for body text,
Material Symbols for icons. No `backdrop-filter`: with it, the F1 bar and
the other translucent panels flickered over the game, so panels are plain
translucent fills. `prelaunch.rcss` is rewritten (no picture, no
golds): menu buttons in bone that light up white with a soul-white left
edge and a fading band.

## Not done

- Sounds are the game's own menu sounds (docs/33, `port/des_menu_sfx.cpp`),
  played on the game's main thread; they need the `0x724E40=des_menu_tick`
  override in the lift. Not yet checked by ear.
- Rebinding; display rows in the settings (Alt+Enter gives borderless
  fullscreen, but there is no row for it, nor for the render resolution or
  AA); the other items of the future-features list.
- The gamepad path is untested on hardware here (no pad on the test
  machine); keyboard, scripted keys and the hold are tested.
- The fetched sources need network access at configure time; the future
  patcher (no installs) will need them vendored or downloaded by its own
  script.
- The D3D11On12 menu renderer ("The menu renderer" above) goes one release
  after the switch: `render_d3d11.cpp` and `DES_UI_RENDER`, and with them
  RmlUi's D3D11 renderer in the build.

## Verification (2026-10-17)

All on a locked session (RSX_PRESENT=0; the frame hook still runs), with
scripted keys and DES_UI_SHOT:

- Start screen, Settings from it, every tab, a choice changed (Frame rate to
  60), back out, Play (the hint toast shows, the screen fades), F1 opens the
  menu bar, Settings from the bar, Esc twice closes everything: all drawn as
  expected.
- settings.ini written on change (DES_SETTINGS in the scratch dir) with
  `DES_FPS = 60`; rows set from the command line (DES_SKIP_INTRO,
  PS3_SAIL_MOVIES, AUDIO_MUTE from deslib) show their value and can't be
  changed.
- Quit, then Quit Game in the question: `[boot] window closed -- exiting`,
  exit code 0.
- Test harness runs (levelcap) print no `[ui]` line at all: no settings
  file, no start screen, the UI asleep.
- Cost hidden, levelcap A/B of the same exe, Palace (pinned regression
  save), DES_FPS=60, vblank at 2000 Hz so nothing caps, RSX_PRESENT=0,
  4 windows of 8 s each: `DES_UI=0` 367.7 fps, UI installed and hidden
  368.7 fps (noise). At the usual VBLANK_MHZ=240000 both arms sit on the cap
  (239.9 / 240.0).

Not tested: a real gamepad (the chord, the stick and button mapping in the
menus, the hold release), the mouse, a visible window (RSX_PRESENT=1), and
window sizes other than 1280x720.

### Data folder, log, crash and issue reports (2026-10-17)

Same setup (locked session, pinned regression save, the exe run from a copy
outside the tools/des staging folders, `DES_DATA_DIR` in the scratch dir):

- Crash: `DES_CRASH_AT_FRAME=300 DES_CRASH_DIALOG_REPORT=1
  DES_REPORT_NO_OPEN=1 DES_CRASH_DIALOG_SHOT=... DES_CRASH_DIALOG_EXIT_MS=10000`.
  boot_main's `[CRASH]` lines print as before and the exit code is the same
  (139 from bash); `crashes\` gets the .txt, .dmp (3-5 MB), .log and
  .cmdline; the dialog's picture shows the modal after it pressed Report on
  GitHub; `reports\crash-<time>.zip` and its `.url.txt` are written; the
  dialog closes itself and no process is left. The stack's
  `des_boot.exe+<rva>` frames resolve through `tools/des/sym.py --map
  port/build/des_boot.map` to `ui::report::frame_hook` and `frame`.
- Abort: `DES_ABORT_AT_FRAME=300 DES_NO_CRASH_DIALOG=1`: exit 3, boot_main's
  `[ABORT]` backtrace still printed, the crash files written, no dialog.
- Report through the menu: `DES_UI_NO_ANIM=1 DES_REPORT_NO_OPEN=1
  "DES_UI_KEYS=300:F1,320:RIGHT,340:ENTER,380:RIGHT,400:ENTER"`:
  `reports\issue-<time>.zip` with screenshot.png (1280x720, the game's
  picture without the menu), system.txt, game.txt, settings.ini (pinned rows
  noted) and des.log. `DES_REPORT_AT_FRAME=400` gives the same zip.
- Redaction: every file of both zips and both links scanned for the user
  name, computer name, profile folder, e-mail and drive or `/c/` paths;
  nothing found but the repository's own name in the link. Paths read
  `<path>\Demon's Souls (USA)`.
- Log: the redirected output (play.sh's way) and `des.log` of the same run
  are byte-identical (CRLF, 8309 lines). Portable mode (`portable.txt` next
  to the exe, no `DES_DATA_DIR`) writes `<exe folder>\data\logs\des.log`.
  Test runs without `DES_DATA_DIR` left `%APPDATA%\DemonsSoulsRecomp`
  untouched.
- Cost of the log, exeab-style A/B, Palace, uncapped (vblank 2000 Hz,
  RSX_PRESENT=0, RSX_VSYNC=0), 4 windows of 8 s: no log 362.8 fps, log
  367.7 fps (noise). Capped at 240: 239.77 / 239.80.

Not tested: Restart game, the dialog with a pad, a crash through
`__fastfail` or an invalid CRT parameter (those end the process without
reaching the filter).

### Window, live frame rate, startup work (2026-10-17)

- Window, at the title screen (32 flips a second), a PowerShell script
  driving the window with messages, flips counted over each step, the old
  build against the new:

  | Step | Old | New |
  |---|---|---|
  | inside the modal move loop (SC_MOVE), 4 s | 0 | 128 |
  | inside the modal size loop (SC_SIZE), 4 s | 16 | 128 |
  | resized to 1684x761, 404x291, 684x961 (portrait), 20 quick resizes | flips go on, picture stretched | flips go on, letterboxed |
  | Alt+Enter, and back | - | 2560x1440 borderless, then 1684x761 |

  The screenshots (DES_UI_SHOT) show the game letterboxed and the UI at the
  window's size. SetWindowPos is not held to the 640x360 floor; a drag is (WM_GETMINMAXINFO).
  regress.sh 4/4 ok.
- Frame rate: `setenv DES_FPS=` 60, 120, 30, 0, 144, 60 in the debug
  console at the title screen: each prints its `[des] DES_FPS=...` line at
  the next flip ("vblank at the cap" for 120 and 144, "the game's own 30
  fps mode" for 0) and the flip rate follows.
- Startup: the window ~90-160 ms after the process starts. Empty cache:
  3535 shaders in 28 s on 15 threads with the window and its bar up, then
  the game. Skip after 352: the game went on at once. Next start: `cache up
  to date`, no window, first flip at 955 ms.
- F1 bar: no flicker without backdrop-filter.

Not tested: a person dragging the window (the script drives the move and
size loops with messages), Restart game after a resize.

### Display scale, centred window, close button (2026-10-17)

- Scale: the same resize script with the settings window open over the
  start screen. At 1684x761 and 1587x922 the settings window is 1087 px
  wide, as at 1280x720; at 640x360 it fills the window (the last tab is
  cut there).
- Window: centred (632 px each side and 316/317 above and below on a
  2560x1392 work area), `DWMWA_USE_IMMERSIVE_DARK_MODE` reads 1.
- Close: a play-kind run (`DES_KIND=play`, so not scripted), WM_CLOSE as
  the X button sends it: the window stays and the Quit Game question
  shows; a second WM_CLOSE with it up changes nothing; Right, Enter (Quit
  Game) exits with `[boot] window closed -- exiting`.
- Continue automatically from settings.ini with the start screen: the game
  waits at its auto-save notice behind the start screen; after Play (frame
  700) it pressed through into the save by itself.
- regress.sh 4/4 ok.

Not tested: a display scaled above 100% (this one is at 100%), moving the
window between displays of different scales.

### Trophies window (2026-10-19)

Run tw1: the pinned save, a scratch `PS3_TROPHY_DIR` whose unlock file
starts with trophies 1, 2, 4 and 5, a scratch settings file,
`DES_TROPHY_HIDDEN=3,4,20`, `DES_TROPHY_TEST=7`, `DES_UI_START=1` and
`DES_UI_NO_ANIM=1`, with scripted keys and DES_UI_SHOT, on a locked
session:

- Start screen: Down, Down, Enter opens the window. It reads "4 of 38
  unlocked", 11 %, the count per grade and "3 hidden"; locked icons are
  dimmed, unlock dates in local time.
- Trophy 3 (hidden, locked) shows as "Hidden trophy" with the hidden
  symbol. Enter reveals it (its name, the Hidden tag, Locked); Enter again
  hides it. Trophy 4 (hidden, unlocked) shows like any other, with the tag.
- Show hidden trophies on: trophies 3 and 20 are listed by name. R1 three
  times and Down twice reach trophy 20; Enter there changes nothing; L1
  four times goes back to the top, and the toggle goes off again
  (settings.ini ends with `trophies_show_hidden = 0`).
- Esc, Esc closes the window, and the focus is back on the Trophies
  button: Up, Up, Enter is Play. With `DES_UI_NO_ANIM=1` the button is not
  highlighted then: RmlUi gives the focus back without `:focus-visible`.
  With transitions on, the start screen's `Show` should focus it again
  visibly, since it runs while the window is still closing (not checked).
- In the game: F1, Right, Enter opens it from the menu bar ("5 of 38":
  trophy 7 was unlocked by `DES_TROPHY_TEST` once the game registered its
  trophies). Trophy 9, unlocked through the debug console while the
  window was open (present 2193), showed at once: the summary went to 6
  of 38, and its row, reached with Down, R1 and Down while the list was
  read again, has the date. The pop-up showed over the window. Esc, Esc
  goes back to the menu bar, with Trophies focused.
- The unlock file ends with 1 2 4 5 7 9. The user's save is unchanged.

Not tested: a real gamepad or mouse (scripted keys only), transitions on,
a display scaled above 100 %, a trophy set that has hidden trophies of its
own (the knob stands in for one).
