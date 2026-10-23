# 88. Setup screens: the flow, the engine's line protocol, and the first screen (2026-10-22)

The fourth part of the player setup (docs/81 section 6, item 4): the RmlUi screens in the prebuilt host that run setup's engine (`tools/setup/des_setup`) as a child process and show its progress, its errors and its resume. docs/52 sections 1 and 4 are the spec.

Code: the port branch `feat/setup-screens`, from master af06c0f. It has the engine's `run` command with the protocol, the host's process runner, the protocol reader, and the progress screen with its failure, stop and done states. The screens before a run (start, "Where is your game?", the game folder, the help page and Ready to build) are on `feat/setup-start-screens`, on the setup shell of docs/89: built, and seen in screenshots with a stand-in engine (sections 2a and 11). The launch hook (section 3) is ps3recomp #104 and the port's `feat/setup-launch`: built and measured (section 11). The rest is section 10.

Labels: **measured** (a run says so), **read** (seen in the code or a doc), **inferred** (reasoned, not run).

## Short answer

- **Built differently (10-22): the screens run in the game's own window, with no game under it** (the window alone, section 3), so no second window or D3D12 loop was needed. What follows was the reasoning before.
- **The screens run in a window of the host's own, before the game boots** (inferred from the boot order, read). Without `des_game.dll` the boot stops at `ps3_game_load` (boot_main.cpp:2300 on `feat/dll-split`), before the frame ticker opens the game's window, and `main` needs the ELF path on its command line (boot_main.cpp:2216). So setup cannot be an overlay on the game's window, as the menus are. docs/52 1.2 already says this: its own D3D12 device, as the crash dialog has.
- **A weak hook at the top of `main` decides** (needs a ps3recomp change, section 3). With no ELF on the command line (the player's double click), the title's hook reads `setup/ready.txt`. Ready: it sets the ELF, `PS3_GAME_MODULE` and `PS3_VFS_ROOT`, and the boot goes on. Not ready: it shows the setup window. A developer's run passes the ELF and never sees setup.
- **The host finds the DLL through `PS3_GAME_MODULE`, set by that hook** from `ready.txt`, unless it is already set. `game_host.c` stays as it is (section 7). This answers docs/81 section 7's open question.
- **The protocol is lines on standard output** (section 4): `@setup`, `@plan`, `@step`, `@progress`, `@overall`, `@check`, `@awake`, `@log`, `@error`, `@done`. Other lines are for people. `python -m des_setup run` prints them; the host reads them with `parse_line` (setup_protocol.cpp).
- **The first Python comes from Windows' own `curl.exe` and `tar.exe`** (section 5), run by the same process runner, with the size and SHA-256 checked by the host before anything is unpacked. On Linux, the system's `python3`.
- **Cancel ends the engine and everything it started at once** (a job object on Windows, a process group on Linux). The journal makes that safe: the next run carries on at the step that was running (section 6).
- **Tested** (measured): the 50 setup tests pass in 7.0 s, 17 of them new. The C++ was not built or run (section 11).
- **The screens before a run** (section 2a) are built and seen, in the setup design's order: the start screen, "Where is your game?", the game folder (or the help page), Ready to build. The likely places are found by the host in C++, the folder dialog is the shell's on Windows and the desktop portal on Linux (the Linux build has SDL2, so not SDL3's dialog), and Before Start's time is a model from one PC's measured build.

## 1. What was there

- **The engine** (read): `check`, `stage`, `lift`, `status`, `tools`, `fetch` (`tools/setup/des_setup/__main__.py`), a journal with fingerprints (journal.py), `@progress <step> n/m` lines from `lift` (`__main__.py:128-132` on master). Logs in `<data>/setup/logs/<step>.log` (lift.py:65-83).
- **The compile** (`feat/setup-compile`, section 9): `compile.py` with `Compile(...).run_all()` returning `game/des_game.dll`, its own `@progress compile n/m`, and a function for the pinned LLVM.
- **The host's loader** (read, ps3recomp `feat/dll-split`): `runtime/ppu/game_host.c` loads the file named by `PS3_GAME_MODULE`, else `PS3_GAME_MODULE_NAME` beside the exe (game_host.c:84-96); the port sets that name to `des_game.dll` (port/CMakeLists.txt:506 there).
- **The UI** (read): every document is drawn by the overlay hook on the game's D3D12 device (des_ui.cpp, host.cpp). The one exception is the crash dialog: a second process of the same exe, picked by `--crash-dialog` in a startup hook before `main` (crash_report.cpp:400-415, 483), with a Win32 window and a D3D12 device of its own (crash_dialog.cpp:111-205). It has no Linux version (crash_report_posix.cpp:1-4).

## 2. The flow and each screen

The flow (docs/52 1.2), with what this patch has:

| # | Screen | Shows | The player can | In this patch |
|---|---|---|---|---|
| 1 | Start | By the state at start (section 3). The first time: "Set up Demon's Souls" and three rows (your copy of the game, the build tools downloaded once, stop and come back). Else "Welcome back" and what setup found: interrupted, a new version, setup's files missing, the game folder moved. Or "Ready to play" | Start; Resume setup or Start over; Update now; Repair; Choose the game folder; Play. Then Settings, Quit | **yes** (setup_screens.cpp, section 2a) |
| 1a | Where is your game? | A card for each way to the game's files: "I have decrypted files" and "Show me how". The RPCS3 card comes with the RPCS3 route (docs/89 section 6) | pick a card, Back | **yes** |
| 2 | Your game folder | The folder (the system's dialog, and the copies found in the usual places), then the check's rows: the game, and the 27 decrypted files (a count, and up to 6 files to fix) | Choose folder, pick a copy found, choose the decrypted files' folder, How do I make these?, Check again, Continue, Back | **yes**; the rows read `run --check-only`'s `@check` lines |
| 2a | Getting your game's files | Copy your disc; decrypt the 27 files with RPCS3's Decrypt PS3 Binaries | I have decrypted files, Back | **yes** |
| 3 | Ready to build | Rows: the game; the disk space ("Setup needs about 4 GB free on C: while it builds."); this PC, its time and how many files at a time; On battery or On a laptop | Start building, Back | **yes** |
| 4 | Progress | The percent, the time left and one bar, then the tasks, each with a spinner, a tick or a cross and its count (docs/89 section 3); "Your PC stays awake until setup is done." when the engine says so; Details: the engine's last 12 lines | Stop (asks first: "Stop the build?"), Details | **yes** (setup_window.cpp) |
| 5 | Failed | A page: "The build stopped" or "Some game files need fixing", a red callout with the engine's sentence (and the files to fix after the game check), the engine's last lines | Report the problem and Try again (step and internal errors), else Try again or Check again; Show the full log; Close | **yes** |
| 6 | Stopped | "The build is stopped" | Resume, Close | **yes** |
| 7 | Done | "Ready to play" | Play | **yes**; Play hands over to the boot (section 7) |
| 8 | Shaders | The same progress screen, one more task (`shaders`, docs/52 5) | as 4 | the label only |

Every screen is a page of the setup shell (docs/89 section 2): the whole window, the rail of steps on the left. The pad: Left and Right move along a line of buttons, Up and Down between the lines, Cross presses one, Circle goes back. On the start screen Circle asks before it quits; on the progress screen it asks to stop. L1 and R1 (Page Up and Page Down) scroll the game folder and help pages. When a question or a page above the start screen closes, the focus goes back to the button that opened it. The pad's buttons and what they do show small at the bottom. Start and F1 do nothing here.

On-screen text is short and plain. Step names, file names and counts come from the engine; their words come from the host (`kTasks`, `count_text` in setup_window.cpp).

### 2a. The screens before a run: what was decided (10-22)

Code (`feat/setup-start-screens`): `port/ui/src/setup_screens.cpp` (the five pages, each a `SetupPage`), `setup_pc.cpp` (this PC's facts), `folder_picker.cpp` (the system's folder dialog), and `host::window()` for the dialog's owner. Built, and seen in screenshots with the stand-in engine (section 11).

**How they open and stack.**
- `DES_UI_OPEN=setup` now opens the start screen, with `DES_SETUP_GAME` (and the rest of `request_from_env`) as the folders to begin with. `DES_UI_OPEN=setup-run` opens the progress screen at once, as `setup` did before; section 11's test uses it now.
- Each page opens the next over itself: the start screen opens "Where is your game?", whose cards open the game folder or the help page; the game folder opens Ready to build. Start building closes the pages above the start screen without uncovering anything and opens the progress screen over the start screen. When the progress screen closes, the start screen comes back and reads `launch_state()` again.
- The main buttons: Start and Start over go to "Where is your game?", then the game folder, then Ready to build. Choose the game folder (GameMoved) goes to the game folder, then straight to the run: nothing is rebuilt, so no Ready to build. Resume setup, Update now and Repair start the run at once, with the folders the last run saved (`request.json`). Play calls `Props::onPlay`, then the setup screens close; without an `onPlay` (over the game, `DES_UI_OPEN=setup`) Play only closes them.
- **Start over deletes nothing.** It lets the player choose the folders again; the journal still skips each step whose inputs are the same. A clean rebuild would need an engine option (a `run --fresh`), not in this patch.
- **Settings** opens the settings window as the start screen over the game does (`SettingsWindow(true)`, prelaunch.cpp). **Quit** and Circle ask first ("Quit setup?", "What setup has done so far is kept.", with Quit in red and Cancel focused), then call `app().quit`. Not the menu's question: its sentence is about the game's saves.

**The likely places: in the host, in C++** (setup_pc.cpp `likely_places`), not in the engine.
- Why: the screen shows them when it opens, before any Python exists (on Windows the host fetches Python before the first run, section 5) and with no process to start. It is a few directory listings. The usual folders come from the system (`SHGetKnownFolderPath`; `user-dirs.dirs` on Linux, which holds their local names).
- The verdict stays the engine's. The host reads only `TITLE_ID` and `APP_VER` from `PARAM.SFO`, in the layout sfo.py reads, to label each dump ("BLUS30443 version 01.00 in D:\Games\...") and put BLUS30443 first. Other titles show with "(not supported)"; picking one gets the engine's sentence that names it.
- Where: on Windows, Downloads, Documents and Desktop, then the roots of the fixed and removable drives (not A:, B:, network or optical drives). On Linux, Downloads, Documents, Desktop and the home folder, then each folder under `/mnt`, `/media/<user>` and `/run/media/<user>`.
- How deep: each place, its folders, and the folders inside folders with a usual name for games (PS3, PS3 Games, Games, ISO, ISOs, Dumps, Backups, RPCS3), two such levels at most (so `RPCS3/games/<dump>` too). Hidden folders and Windows' `$` folders are skipped. At most 400 entries a folder and 12 dumps; 4 are shown.
- It runs on a thread of its own: a sleeping disk wakes up for it. The screen says "Looking for the game in the usual places..." until it ends. Not measured: how long it takes on a PC with many drives.

**The folder dialog** (folder_picker.cpp: one function for each platform, the thread shared).
- Windows: `IFileOpenDialog` with `FOS_PICKFOLDERS`, `FOS_FORCEFILESYSTEM` and `FOS_PATHMUSTEXIST`, on a thread of its own in a single-threaded COM apartment, owned by the game's window.
- Linux: **not SDL3's `SDL_ShowOpenFolderDialog`: the Linux build links SDL2** (read, port/CMakeLists.txt `find_package(SDL2)`, port/ui/CMakeLists.txt, docs/76), which has no dialog, and SDL3 would be a new dependency. It does what SDL3 does instead: the desktop portal (`org.freedesktop.portal.FileChooser.OpenFile` with `directory`) over D-Bus, with `libdbus-1.so.3` opened by `dlopen` when the dialog first opens. Nothing to build against; every desktop with a portal has libdbus, and SDL2 opens it the same way. Without a portal: zenity, then kdialog, as SDL3 falls back. The dialog has no parent window (that needs the X11 or Wayland handle).
- The answer comes back through a mailbox the screen reads once a frame, so the UI draws on while the dialog is open.

**The check's rows** read `run --check-only`'s `@check` lines, with no change to the protocol. A spinner row shows while the check runs, with the MB read. The host sorts the lines by the table name:
- `PS3_GAME`, `PARAM.SFO`, `TITLE_ID` and `APP_VER` are the folder and the title: one row, "Demon's Souls, US disc, version 1.00" with the folder, or "This is not a copy setup can use" with the engine's sentence.
- Other paths, not decrypted files, are the disc's own: a sentence on that row ("The disc's 4 files match.", or the first that does not).
- Names ending in `.elf`, `.unedat` or `.prx` are the files the player decrypts: a count ("24 of 27 found", or "All 27 found, all match"), then up to 6 files to fix, each by its path under USRDIR (Missing, Still encrypted, Does not match), then "And N more".

Continue is pressable only after a check that ended `ok`; it then has the focus. "They are in another folder" chooses a second folder, passed as `--decrypted`.

**"How do I make these?"** (and "Show me how") opens the help page, "Getting your game's files", with two callouts: Copy your disc (PS3 Disc Dumper with a Blu-ray drive, or multiMAN or IRISMAN on a PS3 with custom firmware), and Decrypt the 27 files (RPCS3's Utilities, Decrypt PS3 Binaries; EBOOT.BIN, then the `.sdat` files in chr, chr/c0000, menu, script and script/talk; RPCS3 writes each next to its original). No links: there is no player guide page yet. It names RPCS3, as the engine's own messages already do (gamefiles.py). docs/51 section 8 question 2 (whether a guide may name it) is still open.

**The firmware** (`libsre.prx`, while libsre stays LLE) has no picker on the screen; `DES_SETUP_FIRMWARE` still passes it in a test. docs/51's HLE removes it. Until then a run from the screens lifts no libsre.

**Ready to build's numbers** (setup_pc.cpp).
- Disk: 4 GB (`kNeedBytes`), docs/52 10.2: about 2.5 GB at the peak, and it asks for 4 GB, after docs/45. Inferred from measured parts; no whole run has been measured. The free space is `std::filesystem::space` on the nearest folder of the data folder that exists. The disk is named by its drive letter, or by the folder on Linux, as the engine's own sentence names it. Below 4 GB the row turns red, with Check again, but Start building stays: the engine checks each phase less what its folders already hold (section 6).
- Time: no player's PC has been measured, so it is a range from named constants. 75 s for the check, the copy and the lift (docs/53 section 1), plus the compile: about 2,250 s of CPU (the 14 chunks without `/Zi`, 2,128 s, docs/53 section 2, plus about 2 minutes for SPU and libsre, docs/45 3.1) over the jobs, but never less than the longest job (chunk 008, 261 s). Jobs: the logical CPUs, at most (RAM - 4 GB) / 2.5 GB (docs/53 7.3). The low end is that PC's speed per core (measured there); the high end takes cores 2 times slower on a desktop and 4 times on a laptop, one with a battery (inferred, from docs/45 3.1's guesses: a 6-core desktop about 20 minutes and a 4-core laptop 35 to 50, against 10.7 measured). Rounded to 5 minutes, or 10 above an hour. The compiler's download is not in it.
- What the model gives (inferred, not run): this PC (16 threads, 31 GB, 10 jobs) "about 5 to 10 minutes"; a 4-core laptop with 16 GB (4 jobs) "10 to 45 minutes"; one with 8 GB (1 job) "40 minutes to 2 hours 40 minutes". docs/45 guessed 1 to 2 hours for the last.
- The compile's own job count is compile.py's (on `feat/setup-compile`); when it merges, its rule and this one should be the same.
- "On battery: Plug it in first" shows while `power().onBattery` ("On a laptop" while plugged in): on Windows `GetSystemPowerStatus` (a battery, and `ACLineStatus` 0); on Linux `/sys/class/power_supply` (a battery of scope System, a `Mains` or `USB` supply, and none of them online). It is asked again every 2 s.

## 3. Setup or the game: the state at start

The host decides from files in setup's data folder, never from timing (read, setup_engine.cpp `launch_state`). It runs no Python and hashes nothing, so the check costs nothing at every start (inferred).

`setup/ready.txt` is written by `run` only at the end of a run that built the game module, and deleted at the start of every run (pipeline.py). It is `key=value` lines, so the host reads it without a JSON parser:

```
# Written by des_setup when setup finished; the host reads it at every start (docs/88).
format=1
app=<the host's version>
game=<the dump folder>
eboot=<data>\inputs\PS3_GAME\USRDIR\EBOOT.elf
module=<data>\game\des_game.dll
module_size=<bytes>
module_sha256=<sum>
```

| State | When (in this order) | Start screen |
|---|---|---|
| Damaged | `ready.txt` names a module whose size differs, or that is gone, or an EBOOT that is gone | Repair: a run, which rebuilds only what is missing or changed |
| Outdated | `app` is not this host's version, or `format` is not 1 | Set up again: a run with the kept inputs, nothing to pick |
| GameMoved | the `game` folder is gone | Choose the folder again; nothing to rebuild |
| Ready | otherwise | Play |
| Interrupted | no `ready.txt`, but `setup/journal.json` exists | Resume setup (and Start over) |
| Missing | neither | Set up the game |

- The full SHA-256 of the module is in the file for Repair and for reports; the start check uses the size only (inferred: a module that antivirus removed or a disk error cut short changes its size; a same-size change is not a case we expect).
- `python -m des_setup status --json [--app VERSION]` prints the same state from the engine's side (`ready`, `damaged`, `outdated`, `game-moved`, `running`, `interrupted`, `failed`, `partial`, `missing`), with the journal's steps and the saved folders. It is for tools, tests and reports; the host does not need it at start.
- `run` saves the folders the player chose in `setup/request.json`. A `run` with no game folder takes them, so Resume, Set up again and Repair ask for nothing.

**The launch hook, as built (10-22; ps3recomp #104, the port's `feat/setup-launch`).**

- **ps3recomp** (`boot_main.cpp`): two weak hooks.
  - `ps3_title_launch(int* argc, char*** argv)` runs first in `main`. It returns `PS3_LAUNCH_BOOT` (-1) to boot, `PS3_LAUNCH_WINDOW` (-2) for the window alone, or 0 or more to exit with.
  - The window alone runs `ps3_title_window_only_init` in place of the options init, then only the present thread (`vblank_ticker`): no game module, ELF, HLE, guest thread, SPU or audio.
  - That thread presents with no guest frame, as it does before the first flip, so the overlay draws setup's screens until the window closes. Each pass presents, paced by vsync.
- **The port** (`des_ui_launch` in des_ui.cpp, called by `ps3_title_launch` in des_options.cpp), in this order:
  - `DES_UI=0`: boot, as before.
  - `DES_UI_OPEN=setup` or `setup-run`: the window alone.
  - An ELF on the command line (a developer's run): boot.
  - `launch_state()` is Ready: it sets `PS3_GAME_MODULE` (unless set), `PS3_VFS_ROOT` and the ELF argument (section 7), then boots. Anything else: the window alone.
- **Setup alone** (`des_ui_setup_only_init`):
  - settings.ini, the overlay and the start screen (or setup-run's progress page);
  - no menu bar, start screen, trophies or frame watch, and the close button asks nothing;
  - it sets `RSX_VSYNC=1` (vsync paces each pass), `PS3_PSO_LIBRARY=0` (the last build's pipeline library stays shut) and a `PS3_TITLE` (no PARAM.SFO is read).
- **Play** on setup alone starts the game in a new process (section 7) and closes this one. Over a running game (a runtime without the hook), Play only closes the screens, as before.
- **Known limits:**
  - The menu sounds are silent on setup alone: they come from the game's sound engine (read).
  - On Linux the gamepad does nothing on setup alone, since cellPad opens the pads only at the game's `cellPadInit` (read; not built there).
  - The ELF path from `ready.txt` goes to `main` as a narrow (ANSI) string, so a dump folder with characters outside the system's code page may fail (inferred, not tried).

The plan as written before it was built:

**The launch hook (needs ps3recomp).** `main` (boot_main.cpp:2200 on `feat/dll-split`) gets a weak hook first thing, as it has `ps3_title_prework` last (boot_main.cpp:76-81, 2386):

```c
/* Optional hook, before anything else in main: the title's launcher. It may change argc/argv (an ELF path for a
 * player's start without one) and the environment (ps3_setenv), or end here with its exit status. Weak default:
 * returns -1, go on. */
int ps3_title_launch(int* argc, char*** argv);
```

The port's version: an ELF on the command line, go on (a developer's run). Otherwise `launch_state()`: Ready fills in the ELF and the environment (section 7); anything else opens the setup window. The alternative, the startup hook the crash dialog uses, runs before `main` on Windows only and cannot hand `main` an ELF (inferred), so it is not used.

## 4. The protocol between the host and the engine

The engine prints one line for each event, UTF-8, ending in `\n` (a `\r` before it is dropped). A line is the protocol's when it starts with `@` and a keyword; anything else is a line for people. The engine's side is `des_setup/protocol.py`, the host's `setup_protocol.cpp`.

| Line | Meaning |
|---|---|
| `@setup <version>` | The first line: the protocol's version, 1. The host stops an engine of another version and says the setup files do not match. |
| `@plan <phase>...` | The phases of this run, in order: `check fetch stage lift compile`, or `check` alone. |
| `@step <phase> start\|done\|skip` | A phase starts, ends, or ends with nothing to do (all its journal steps were done with the same inputs). |
| `@progress <step> <n>/<m>` | The current step's own count: `check` and `download` in bytes, `ppu` in functions, `spu` in programs, `compile` in objects. Printed when the per cent changes. |
| `@overall <n>/1000` | The whole run, by phase weights from CPU time (check 2, fetch 4, stage 1, lift 13, compile 80; docs/52 4.3). Never goes back within a run. |
| `@check <state> <name> <text>` | One checked file: `ok`, `unmeasured`, `missing`, `mismatch`, `encrypted` or `wrong`; its table name; gamefiles.py's sentence. |
| `@awake` | The engine has asked Windows to keep the PC awake until it ends. |
| `@log <path>` | The log of the step that failed. |
| `@error <code> <step> <text>` | What failed, in one sentence for the player. Codes: `game`, `busy`, `disk`, `download`, `memory`, `step`, `internal`. `<step>` is the journal step, or `-`. |
| `@done ok\|fix\|failed` | The last line. |

- Names have no spaces; `<text>` and `<path>` are the rest of the line.
- A plain line that starts with `@` is printed with a space first, so it never reads as the protocol.
- An unknown keyword is a plain line to the host, so a new kind of line needs no new version; a changed meaning does.
- The exit status says the same as `@done`: 0 ok, 1 fix or failed, 2 usage. An engine that ends without `@done` has crashed: the host shows "Setup stopped unexpectedly." with the exit status under Details.
- Every line also goes to `<data>/setup/logs/run.log` (the last run's is `run.prev.log`), so a run from a terminal leaves the same record.
- Two tests keep the sides together (test_pipeline.py `HostAgreesTest`): the C++ parser knows exactly protocol.py's keywords, and the C++ table of error codes is exactly protocol.py's.

An example, a run resumed after the lift's PPU step failed once:

```
@setup 1
@awake
@plan check fetch stage lift
@step check start
@progress check 104857600/1442840576
@check ok PS3_GAME/USRDIR/EBOOT.BIN EBOOT.BIN matches.
...
@step check done
@step fetch start
@step fetch skip
@step stage start
@step stage skip
@step lift start
@progress ppu 47000/94017
@progress spu 14/14
@step lift done
@overall 1000/1000
@done ok
```

`run` options: `run [GAME] [--decrypted DIR]... [--firmware FILE] [--data DIR] [-j N] [--app VERSION] [--check-only]`.

## 5. Running the engine

**The process** (child_process.cpp; one function per platform for start, read, stop, wait and close; the thread and the line splitting are shared):
- Windows: `CreateProcessW` with `CREATE_NO_WINDOW`, standard output and error on one anonymous pipe, standard input on `NUL`, and only those two handles handed down (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). It starts suspended, goes into a job object with kill-on-close, then runs, so nothing it starts is outside the job. The lifter's worker processes and the compiler inherit its hidden console, so no window opens (inferred: `CREATE_NO_WINDOW` gives a console without a window, which children inherit; `DETACHED_PROCESS` would not).
- Linux: `posix_spawn` (or `posix_spawnp` for a bare name) into a process group of its own, standard input from `/dev/null`, `SIGPIPE` back to its default.
- Lines longer than 64 KB without a break are passed on in pieces.

**The command.** `<python> -X utf8 -u <install>/tools/setup/run_setup.py run ... --data <data> --app <version>`.
- `-X utf8`: the tools' prints and outputs are UTF-8 whatever the code page (docs/52 13.3, measured there). `-u`: each line arrives as it is printed.
- `run_setup.py` puts its own folder on `sys.path` first, because the embeddable Python's `._pth` file leaves the script's folder off it (docs/52 13.3, measured there).
- The script is `DES_SETUP_SCRIPT` when that is set (a developer's source tree, for a real build from a build folder elsewhere). Else it is found beside the exe in a release (the install mirrors the source tree, docs/81 section 7), or up to three folders up in a developer's build.
- The engine lowers its own priority to below normal (Windows; `nice` on Linux), which everything it starts inherits, and asks Windows to keep the PC awake until it ends (`SetThreadExecutionState`), then prints `@awake` (pipeline.py `quiet_and_awake`). In the engine, not the host, so a run from a terminal does the same (docs/52 4.1).

**Which Python.** `DES_SETUP_PYTHON` (a developer's); else on Windows the embeddable Python in `<data>/tools/python-embed-3.13.9/` once its stamp names the pinned SHA-256 (the stamp `fetch.ensure` writes and `fetch.unpacked` checks); on Linux `python3` from `PATH`. The pin is in setup_engine.hpp as well as toolchain.json; a test checks that they agree.

**Which compiler.** The pinned LLVM download (docs/86), which the fetch phase fetches. Or a developer's: `run --cc CLANG-CL --ld LLD-LINK`, or `DES_SETUP_CC` and `DES_SETUP_LD` in the environment, which the host's runs inherit. With one, the fetch phase skips the download (compile.py `local_toolchain`). So a developer can run a real build from the setup screens before the download is pinned: `DES_SETUP_PYTHON`, `DES_SETUP_SCRIPT` (a tree with `ps3recomp/` at the pinned commit, since the lift runs its tools), `DES_SETUP_CC`, `DES_SETUP_LD` and a scratch `DES_SETUP_DATA`.

**The first Python, before there is a Python.** The host has to fetch it itself. Options:

| Way | Code in the host | Verdict |
|---|---|---|
| Windows' own `curl.exe` and `tar.exe` (System32, Windows 10 1803 and later), run through the same process runner | the SHA-256 (about 100 lines, portable), the steps | **chosen** |
| WinHTTP and a zip reader in the host | HTTP with resume and redirects, inflate, the zip directory: several hundred lines, Windows-only HTTP | no: more code to own for a 10.9 MB file |
| Ship the zip in the release | none | no: docs/81's decision is that every tool is downloaded |
| PowerShell's `Invoke-WebRequest` and `Expand-Archive` | the steps | no: slower, and execution policies vary |

The steps (inferred design; section 10 item 3):
1. `curl.exe --fail --location --proto =https --tlsv1.2 -C - -o <data>/downloads/python-embed-3.13.9.zip.part <url>`, from the System folder by full path (never through `PATH`). The bar is the `.part` file's size against the pinned size, read every quarter second. Cancel ends it like the engine; the next try resumes.
2. The host checks the size and the SHA-256, deletes a mismatch, and renames the file to `python-embed-3.13.9.zip`: the same name and folder as fetch.py's `archive_path`.
3. `tar.exe -tf` lists it, and the host refuses absolute names, drive letters and `..` (as fetch.py's `unpack` does); then `tar.exe -xf ... -C <dir>.tmp`, a rename, and the `.des-setup-sha256` stamp. From then on fetch.py sees the tool as unpacked.

## 6. Cancel, resume, errors and reports

- **Cancel** asks first, then ends the engine and everything it started (`ChildProcess::stop`). The journal step that was running stays `running`; the next run's fingerprint check reruns it, and every finished step is skipped (journal.py `is_done`). A killed run leaves no lock: `setup/run.lock` is held through the system's file locks, which end with the process (pipeline.py `lock`). A second run while one is going gets `@error busy`.
- **Closing the window or a host crash** end the engine the same way (the job's kill-on-close; the process group on Linux).
- **Errors.** The failure window's first sentence is the engine's: what failed ("Converting the game's code failed.", "Setup needs about 1.0 GB free on C:. 0.4 GB is free."). The second is the host's, by the code:

| Code | Second sentence | Buttons |
|---|---|---|
| game | Then check again. (after the files and what to do about each, up to 5) | Check again, Details, Close |
| busy | Wait until it has finished, then try again. | Try again, Details, Close |
| disk | Free some space, then try again. Setup carries on where it stopped. | Try again, Details, Close |
| download | Check the internet connection, then try again. The download carries on where it stopped. | Try again, Details, Close |
| memory | Close other programs, then try again. Setup carries on where it stopped. | Try again, Details, Close |
| step, internal | Try again. If it fails again, please report it. | Try again, Report, Details, Close |

- **Disk** is checked before each phase that writes much (fetch 0.5 GB, lift 1 GB, compile 2 GB from scratch, docs/52 10.2), less what that phase's folders already hold, so a resumed run is not refused for space it has already used. A full disk during a step is caught from the error text or `ENOSPC`.
- **Details** shows the engine's last 20 lines and the failed step's log path. The full logs are in `<data>/setup/logs/`.
- **Report** writes a zip in the reports folder through report.hpp, as the menu's report does, and opens the issue form: `system.txt` (this PC's facts), `setup.log` (the run's log), `step.log` (the failed step's log: its first 20 lines and last 64 KB) and `journal.json`, all through `report::redact`. No game file, no lift, no module. The step logs hold step names, counts, addresses and the tools' messages (read, lift.py; inferred for a compiler error, which is why the compile passes `-fno-caret-diagnostics`, section 9). Nothing is sent: the player attaches the zip.

## 7. Where the DLL goes, and how the host loads it

- **Where.** `compile.py` writes `<data>/game/des_game.dll` (compile.py:338 there). `run` records the path `run_all` returns in `ready.txt`, so the host has no name of its own to keep in step.
- **How** (decided here). On a player's start the launch hook (section 3) reads `ready.txt` and, through `ps3_setenv` (boot_main.cpp:202, which sets the process environment and the getenv cache), sets:
  - `PS3_GAME_MODULE=<module>`, unless it is already set (a developer's choice wins);
  - `PS3_VFS_ROOT=<game>`, the dump, since the staged `EBOOT.elf` is not beside the game's files (derive_vfs_root, boot_main.cpp:278-282);
  - and the ELF argument, the staged `EBOOT.elf`.
  Then `main` goes on, and `ps3_game_load` loads the module as it does today (game_host.c:85-96).
- **Why not the other two ways.** Teaching `game_host.c` to look in this title's data folder puts the port's folder names into ps3recomp. A separate launcher exe that sets `PS3_GAME_MODULE` is a second program to ship and sign, for what the host's own hook does in a few lines (inferred).
- **Play, as built** (`crash_report::start_again`, 10-22):
  - It starts this exe again with the same command line and the environment the process started with (saved before `main`, so the setup-alone variables are not in it), minus `DES_UI_OPEN`, plus `DES_SKIP_UI=1` (no start screen: Play was the choice) and `DES_LAUNCH_AFTER=<this pid>`. Then this one quits.
  - The new process waits up to 10 s for that pid to end before it opens its log, so it gets des.log and the only window.
  - Not `ChildProcess`: its job object would end the game when setup's process ends.
  - Measured in section 11.
- **Play** after setup starts the host again, as a new process with the same command line, and ends this one. The new one finds Ready and boots. That avoids starting RmlUi and a second D3D12 device again inside one process (inferred: a fresh process is the case every start already tests). It costs a second or so (inferred).
- **The libsre glue's absolute image path** (docs/81 section 5) stays as it is; it goes with milestone 2.

## 8. Linux

- Portable as it stands: the protocol reader, the screen, the engine command, the data folder (`$XDG_DATA_HOME/DemonsSoulsRecomp`, else `~/.local/share/...`), and the process runner (`posix_spawn`, a process group).
- Facts that differ, one function each: the local data folder and which Python (setup_engine.cpp), and the process calls (child_process.cpp). The Python is the system's `python3`, since python.org's embeddable package is for Windows only (read, docs/52 6.2); no first download there.
- Not done there: keeping the PC awake (it needs the desktop's session bus; `systemd-inhibit` as a wrapper is the likely way, inferred) and the setup window itself (SDL with vkd3d, as the game's window, docs/76). The compile has no Linux commands yet (compile.py's description is clang-cl's).
- Written for Linux with the screens before a run (section 2a), not built there: the folder dialog (the desktop portal through libdbus opened at run time, then zenity or kdialog), the usual folders (`user-dirs.dirs`, the mounted drives), the memory (`sysconf`) and the battery (`/sys/class/power_supply`). The link line gains `CMAKE_DL_LIBS` for `dlopen`.

## 9. The compile step's hooks (done with `feat/setup-compile`)

`run`'s compile phase is `des_setup/compile.py` (docs/81 section 7). What it needed from that branch, now in it:

1. **`compile.toolchain(data, cc=None, ld=None) -> (cc command, ld path)`**: the given pair, or the pinned compiler
   unpacked under the data folder. `run` calls it with the data folder only; `python -m des_setup compile --cc --ld`
   passes its own.
2. **One name for the compiler's entry**: `compile.LLVM_ENTRY` (`"llvm-win64"`); `pipeline.BUILD_TOOLS` is made from
   it, so the fetch phase downloads what the compile phase looks for.
3. **An out-of-memory failure says so**: after its one retry with a job fewer, an object whose compiler ran out of
   memory fails with "the compiler ran out of memory", also when only the exit status told (0xC0000017,
   0xC000012D), so `run` reports the `memory` code (pipeline.py `OOM`).
4. **No source lines in the logs**: the compile adds `-clang:-fno-caret-diagnostics` to every object's command, so
   a failed compile's log never quotes lifted source lines; reports attach its tail.
5. The compile phase is always in `run`'s plan now (the fallback for a tree without compile.py is gone).

Tested (measured): the unit tests (section 11), with three new ones for these hooks (the exit-status one on Windows
only). Not run yet: `run` with the real compiler download, which waits for the archive's first release (docs/86).

## 10. Next steps, in order

1. **The launch hook** in the ps3recomp fork (section 3), and the port's `ps3_title_launch`: Ready fills in the ELF and the environment (section 7); otherwise the setup window; Play restarts the host. **Built and measured** (10-22, section 11).
2. ~~**The window of its own.**~~ Not needed: the window alone (section 3) shows the screens in the game's own window.
3. **The first Python download** (section 5): the host's SHA-256, then curl and tar through `ChildProcess`.
4. **The start screen** (section 3's states), **"Where is your game?"**, **the game folder** and **the help page**: the folder picker (a function for each platform: the shell's folder dialog on Windows, the desktop portal on Linux), the scan of likely places, the check's rows from `run --check-only`. **Built and seen** with the stand-in engine (section 11). Next: the folder dialog and a real dump; the RPCS3 card (docs/89 section 6).
5. **Ready to build**: the free space and time estimate, and "Plug it in first" on battery (docs/52 1.2 point 3). **Built and seen.** The time is a model from one PC's numbers; a whole setup on a second PC would check it.
6. **The shaders phase** (docs/52 section 5): one more phase in `run`'s plan; the screen already has its label.
7. **Keeping Linux awake.**
8. **A test of the C++ protocol reader** that runs in CI (`parse_line`, `Status`, `Eta` are pure). port/ui has no C++ test target yet.

## 11. Tested

- **Unit tests** (measured): `cd tools/setup && python -m unittest discover -s tests`, 50 tests in 7 to 8 s, all pass. The 17 new ones (test_pipeline.py), on test_setup's made-up dump and test_lift's fake tools:
  - a full run, its protocol lines, the bar never going back, `run.log` equal to what was printed; then a second run with every phase skipped and no tool called;
  - `--check-only`; missing decrypted files (`@error game`, `@done fix`, a `@check missing` line); the saved folders; no folders at all;
  - a failed lifter (`@error step ppu`, `@log`), then a resume that starts at the lifter;
  - a run while another holds the lock; not enough disk;
  - `ready.txt` from a fake compile phase, then `status --json`'s ready, outdated and damaged; a later failed run leaves no `ready.txt`; interrupted and missing;
  - the emitter's lines; the two checks that the C++ agrees with the Python (keywords, error codes, the protocol version, the Python pin).
- **The command** (measured): `run_setup.py status --json` on an empty folder says `missing`; `run` with no folders says `@error game check Choose the game folder first.`; `run --check-only` on a folder with no dump prints the game check's sentence as `@check` and `@error` lines and exits 1. The engine printed `@awake` on this PC.
- **Built and seen** (measured 10-22): `des_ui` and `des_boot` build with clang-cl, no warning in the new files. `des_boot` started with `DES_UI_OPEN=setup` (now `DES_UI_OPEN=setup-run`: `setup` opens the start screen, section 2a), a scratch `DES_SETUP_DATA`, `DES_SETUP_PYTHON=python` and no game folder: the host started the engine under Python, which ended with `@done fix` and the `game` error. The screen showed "Some game files need fixing", "Choose the game folder first.", "Then check again." and the buttons Check again (focused), Details and Close, steady from frame 240 to 900.
- **Not run yet:** a run with a real dump (check, stage, lift; the fetch then stops while the compiler entry has no sums), Cancel during a run, the gamepad and mouse on the buttons, and the Linux build.
- **The screens before a run, built and seen** (measured 10-22). `des_ui` and `des_boot` build with clang-cl on Windows, with no warning. Two scripted runs with `DES_UI_OPEN=setup`, the stand-in engine (docs/89 section 5) and a made-up `DES_SETUP_GAME`:
  - A first start: the welcome, "Where is your game?", the help page, the game folder with the check's rows (3 decrypted files missing), and the quit question.
  - An interrupted start (a `journal.json` left in the data folder): "Welcome back" with Resume setup and Start over; Start over, "Where is your game?", the game folder with every file found (Continue gets the focus and scrolls into view), Ready to build, the progress page, "Ready to play", and the start screen again, now Ready.
  The user's save was the same after each run.
- **Not exercised yet:** the folder dialog (a test cannot press it), the places scan on a PC with copies in the usual places, the battery rows, the pad and mouse by hand, and the Linux build: libdbus's `dbus_*` calls are declared by hand in folder_picker.cpp, from dbus-message.h and dbus-bus.h as remembered, and no compiler has checked them.
- One log line to look at: `[ui] a window's close transition did not run; hidden without it` came after the run ended (inferred: a window closed while the setup window was open; harmless in the shots).
- **A whole setup on a real dump** (measured 10-22). des_host from master (b14200d, built as host.yml builds it), `DES_UI_OPEN=setup-run` on the US 1.00 dump, with the decrypted files from earlier unlocks and a developer's compiler (`DES_SETUP_CC`/`DES_SETUP_LD`, LLVM 22.1.8, section 5):
  - check, fetch (skipped: a developer's compiler), stage, lift and compile all passed, in 27 minutes. The compile ran one job at a time: the job count comes from free memory at the start (`compile.default_jobs`), and this PC had about 5 GB of 31 GB free.
  - "The game is built" with the time, then a 289 MB `des_game.dll` and `ready.txt`.
  - A player's start from that `ready.txt` (no arguments, an empty environment for the port's knobs, a working folder holding a copy of the pinned save) loaded the module (94,017 functions) and set the VFS root, then **hung on the loading screen**. Its log has no `[libsre] LLE load` line: libsre was never loaded. The same start with the run defaults of `tools/des/deslib.sh` (`LIBSRE_LLE`, `LIBSRE_LLE_ONLY`, `SPURS_NO_ROUND_NOTIFY`, `VP_VB_VERTS`) reached the Tutorial. The host now sets those itself, unless the environment has them (`run_defaults`, des_options.cpp; docs/45 1.3).
  - With that change, the same player's start with none of them in the environment loads libsre (`[libsre] LLE load: ok=1`, the `[LLE-ONLY]` lines) and reaches the Tutorial.
- **The launch hook** (measured 10-22). The build was the port's launch commit with ps3recomp #104. The test used an empty working folder, a stand-in `ready.txt` (Ready) and a settings.ini with Enhanced, `RSX_SCALE = 150` and `DES_FPS = 60`:
  - `DES_UI_OPEN=setup`, no key pressed: the start screen, "Ready to play", in 26 log lines, with no game boot (no module load, no guest thread).
  - The same with Enter on Play: the log says "play: the game starts in a new process", and the setup process exits 0. The new process boots the game and applies the stored settings ("render targets at 150%", "FlipMode 60FPS(NO SKIP)"), then stops on `EXIT_AFTER_MS`.
  - A player's start: no arguments, and no `DES_UI_OPEN`, `PS3_GAME_MODULE` or `PS3_VFS_ROOT`, with a `ready.txt` naming the real module and game folder. The log says "launch: the game setup built", the module loads from the path in `ready.txt`, and the VFS root is its game folder. It boots with the stored settings.
  - Not run: Linux.

## 12. Open questions

1. **The launch hook in ps3recomp** (section 3), a weak `ps3_title_launch(int* argc, char*** argv)` at the top of `main`. Built (ps3recomp #104). Recommendation: yes. It is the one way that works on both platforms and lets the host start with no arguments.
2. **The first Python through Windows' `curl.exe` and `tar.exe`** (section 5), with the host checking the SHA-256. Recommendation: yes. The other way is a few hundred lines of HTTP and zip code in the host.
3. **Play restarts the host** after setup (section 7). Built that way. Recommendation: yes, for a clean boot. The other way keeps one process and starts RmlUi and D3D12 a second time in it.
4. **Reports from setup include the failed step's log tail** (section 6), with names and addresses but no game code once the compile runs with `-fno-caret-diagnostics`. Recommendation: yes; the player sees the zip before attaching it.
