# 88. Setup screens: the flow, the engine's line protocol, and the first screen (2026-10-22)

The fourth part of the player setup (docs/81 section 6, item 4): the RmlUi screens in the prebuilt host that run setup's engine (`tools/setup/des_setup`) as a child process and show its progress, its errors and its resume. docs/52 sections 1 and 4 are the spec.

Code: the port branch `feat/setup-screens`, from master af06c0f. It has the engine's `run` command with the protocol, the host's process runner, the protocol reader, and the progress screen with its failure, stop and done states. The rest of the screens are section 10.

Labels: **measured** (a run says so), **read** (seen in the code or a doc), **inferred** (reasoned, not run).

## Short answer

- **The screens run in a window of the host's own, before the game boots** (inferred from the boot order, read). Without `des_game.dll` the boot stops at `ps3_game_load` (boot_main.cpp:2300 on `feat/dll-split`), before the frame ticker opens the game's window, and `main` needs the ELF path on its command line (boot_main.cpp:2216). So setup cannot be an overlay on the game's window, as the menus are. docs/52 1.2 already says this: its own D3D12 device, as the crash dialog has.
- **A weak hook at the top of `main` decides** (needs a ps3recomp change, section 3). With no ELF on the command line (the player's double click), the title's hook reads `setup/ready.txt`. Ready: it sets the ELF, `PS3_GAME_MODULE` and `PS3_VFS_ROOT`, and the boot goes on. Not ready: it shows the setup window. A developer's run passes the ELF and never sees setup.
- **The host finds the DLL through `PS3_GAME_MODULE`, set by that hook** from `ready.txt`, unless it is already set. `game_host.c` stays as it is (section 7). This answers docs/81 section 7's open question.
- **The protocol is lines on standard output** (section 4): `@setup`, `@plan`, `@step`, `@progress`, `@overall`, `@check`, `@awake`, `@log`, `@error`, `@done`. Other lines are for people. `python -m des_setup run` prints them; the host reads them with `parse_line` (setup_protocol.cpp).
- **The first Python comes from Windows' own `curl.exe` and `tar.exe`** (section 5), run by the same process runner, with the size and SHA-256 checked by the host before anything is unpacked. On Linux, the system's `python3`.
- **Cancel ends the engine and everything it started at once** (a job object on Windows, a process group on Linux). The journal makes that safe: the next run carries on at the step that was running (section 6).
- **Tested** (measured): the 50 setup tests pass in 7.0 s, 17 of them new. The C++ was not built or run (section 11).

## 1. What was there

- **The engine** (read): `check`, `stage`, `lift`, `status`, `tools`, `fetch` (`tools/setup/des_setup/__main__.py`), a journal with fingerprints (journal.py), `@progress <step> n/m` lines from `lift` (`__main__.py:128-132` on master). Logs in `<data>/setup/logs/<step>.log` (lift.py:65-83).
- **The compile** (read, `feat/setup-compile`, not merged): `compile.py` with `Compile(...).run_all()` returning `game/des_game.dll` (compile.py:154, 228, 338), its own `@progress compile n/m` (compile.py:272, 294), and `__main__.compiler()` for the pinned LLVM (`__main__.py:146-161` there).
- **The host's loader** (read, ps3recomp `feat/dll-split`): `runtime/ppu/game_host.c` loads the file named by `PS3_GAME_MODULE`, else `PS3_GAME_MODULE_NAME` beside the exe (game_host.c:84-96); the port sets that name to `des_game.dll` (port/CMakeLists.txt:506 there).
- **The UI** (read): every document is drawn by the overlay hook on the game's D3D12 device (des_ui.cpp, host.cpp). The one exception is the crash dialog: a second process of the same exe, picked by `--crash-dialog` in a startup hook before `main` (crash_report.cpp:400-415, 483), with a Win32 window and a D3D12 device of its own (crash_dialog.cpp:111-205). It has no Linux version (crash_report_posix.cpp:1-4).

## 2. The flow and each screen

The flow (docs/52 1.2), with what this patch has:

| # | Screen | Shows | The player can | In this patch |
|---|---|---|---|---|
| 1 | Start | One status line and one main button, by the state at start (section 3): Set up the game, Resume setup (with Start over), Set up again, Repair, Play; and Choose the game folder again when it moved | press the main button, Settings, Quit | no |
| 2 | Game files | The game folder (picker, and the likely places found), the 27 decrypted files as a checklist (found, missing, wrong), "How do I make these?" | pick folders, Check again, Start | no; the engine side is there (`run --check-only` prints a `@check` line per file) |
| 3 | Before Start | "Setup needs about N GB free on C: and takes about T." | Start, Back | no |
| 4 | Progress | The phases, each with a mark (to do, running, done, already done, failed); the current step and its count ("Convert the game's code: 1,200 of 94,017 functions") with its bar; the whole run's bar and the time left; "Your PC stays awake until setup is done." when the engine says so; Details: the engine's last 12 lines | Details, Stop (asks first: "It carries on from here the next time.") | **yes** (setup_window.cpp) |
| 5 | Failed | A danger window: what failed (the engine's sentence), what to do (the host's, by the error's code), the files to fix when it was the game check | Try again or Check again, Report (step and internal errors), Details, Close | **yes** |
| 6 | Stopped | "Setup stopped. It carries on from here the next time." | Resume, Close | **yes** |
| 7 | Done | "The game is ready." | Play | **yes**; Play hands over to the boot (section 7) |
| 8 | Shaders | The same progress screen, one more phase (`shaders`, docs/52 5) | as 4 | the label only |

On every screen the pad works as in the other windows (read, document.cpp, modal.cpp): Left and Right move between the buttons, Cross presses one, Circle asks to stop or closes. A row under the buttons shows the PS3 buttons and what they do, as the on-screen keyboard's does (osk_window.cpp:45-53, 127-136). Start and F1 do nothing here.

On-screen text is short and plain. Step names, file names and counts come from the engine; their words come from the host (`phase_label`, `count_text` in setup_window.cpp).

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
- The script is found beside the exe in a release (the install mirrors the source tree, docs/81 section 7), or up to three folders up in a developer's build.
- The engine lowers its own priority to below normal (Windows; `nice` on Linux), which everything it starts inherits, and asks Windows to keep the PC awake until it ends (`SetThreadExecutionState`), then prints `@awake` (pipeline.py `quiet_and_awake`). In the engine, not the host, so a run from a terminal does the same (docs/52 4.1).

**Which Python.** `DES_SETUP_PYTHON` (a developer's); else on Windows the embeddable Python in `<data>/tools/python-embed-3.13.9/` once its stamp names the pinned SHA-256 (the stamp `fetch.ensure` writes and `fetch.unpacked` checks); on Linux `python3` from `PATH`. The pin is in setup_engine.hpp as well as toolchain.json; a test checks that they agree.

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
- **Report** writes a zip in the reports folder through report.hpp, as the menu's report does, and opens the issue form: `system.txt` (this PC's facts), `setup.log` (the run's log), `step.log` (the failed step's log: its first 20 lines and last 64 KB) and `journal.json`, all through `report::redact`. No game file, no lift, no module. The step logs hold step names, counts, addresses and the tools' messages (read, lift.py; inferred for a compiler error, which is why section 9 asks for `-fno-caret-diagnostics`). Nothing is sent: the player attaches the zip.

## 7. Where the DLL goes, and how the host loads it

- **Where.** `compile.py` writes `<data>/game/des_game.dll` (compile.py:338 there). `run` records the path `run_all` returns in `ready.txt`, so the host has no name of its own to keep in step.
- **How** (decided here). On a player's start the launch hook (section 3) reads `ready.txt` and, through `ps3_setenv` (boot_main.cpp:202, which sets the process environment and the getenv cache), sets:
  - `PS3_GAME_MODULE=<module>`, unless it is already set (a developer's choice wins);
  - `PS3_VFS_ROOT=<game>`, the dump, since the staged `EBOOT.elf` is not beside the game's files (derive_vfs_root, boot_main.cpp:278-282);
  - and the ELF argument, the staged `EBOOT.elf`.
  Then `main` goes on, and `ps3_game_load` loads the module as it does today (game_host.c:85-96).
- **Why not the other two ways.** Teaching `game_host.c` to look in this title's data folder puts the port's folder names into ps3recomp. A separate launcher exe that sets `PS3_GAME_MODULE` is a second program to ship and sign, for what the host's own hook does in a few lines (inferred).
- **Play** after setup starts the host again, as a new process with the same command line, and ends this one. The new one finds Ready and boots. That avoids starting RmlUi and a second D3D12 device again inside one process (inferred: a fresh process is the case every start already tests). It costs a second or so (inferred).
- **The libsre glue's absolute image path** (docs/81 section 5) stays as it is; it goes with milestone 2.

## 8. Linux

- Portable as it stands: the protocol reader, the screen, the engine command, the data folder (`$XDG_DATA_HOME/DemonsSoulsRecomp`, else `~/.local/share/...`), and the process runner (`posix_spawn`, a process group).
- Facts that differ, one function each: the local data folder and which Python (setup_engine.cpp), and the process calls (child_process.cpp). The Python is the system's `python3`, since python.org's embeddable package is for Windows only (read, docs/52 6.2); no first download there.
- Not done there: keeping the PC awake (it needs the desktop's session bus; `systemd-inhibit` as a wrapper is the likely way, inferred) and the setup window itself (SDL with vkd3d, as the game's window, docs/76). The compile has no Linux commands yet (compile.py's description is clang-cl's).

## 9. Hooks needed from `feat/setup-compile` (not copied into this branch)

`run` takes the compile phase from `des_setup/compile.py` when it is there (`pipeline.compile_step`), and leaves the phase out of the plan when it is not. Once that branch merges, `run` needs:

1. **`compile.toolchain(data) -> (cc command, ld path)`**: `__main__.compiler(a, data)` (`__main__.py:149-161` there) moved into compile.py without the argparse namespace. `run`'s compile phase calls it, then `Compile(data, jr, cc, ld, jobs=jobs, progress=...).run_all()` as it is (compile.py:154, 228).
2. **One name for the compiler's entry**: `LLVM_ENTRY = "llvm-win64"` (`__main__.py:146` there) and `pipeline.BUILD_TOOLS` should be the same constant, in compile.py. `run`'s fetch phase downloads it with a bar; `toolchain()` then finds it unpacked.
3. **An out-of-memory failure says so.** After its one retry with a job fewer, `_cc`'s `StepFailed` message should contain "out of memory" also when only the exit status told (0xC0000017, 0xC000012D; compile.py:38-39, 318-321), so `run` reports the `memory` code (pipeline.py `OOM`).
4. **`-fno-caret-diagnostics`** on the compile commands (game_build.json), so a failed compile's log never quotes lifted source lines; reports attach its tail.
5. **The merge itself**: `__main__.py` meets at the import lines only (both add a module); keep both. journal.py's thread-safe version is compatible: `run` reads `jr.data["steps"]` only between phases.

## 10. Next steps, in order

1. **The launch hook** in the ps3recomp fork (section 3), and the port's `ps3_title_launch`: Ready fills in the ELF and the environment (section 7); otherwise the setup window; Play restarts the host.
2. **The window of its own.** Move the crash dialog's window and D3D12 loop (crash_dialog.cpp:111-205, 210-345) into one shared file, used by both; then its Linux version on SDL and vkd3d. `SetupWindow` runs in it unchanged.
3. **The first Python download** (section 5): the host's SHA-256, then curl and tar through `ChildProcess`.
4. **The start screen** (section 3's states) and **the game files screen**: the folder picker (a function for each platform: the shell's folder dialog on Windows, the desktop portal on Linux), the scan of likely places, the checklist from `run --check-only`, the guide link.
5. **Before Start**: the free space and time estimate, and "Plug in first" on battery (docs/52 1.2 point 3).
6. **The shaders phase** (docs/52 section 5): one more phase in `run`'s plan; the screen already has its label.
7. **Keeping Linux awake.**
8. **A test of the C++ protocol reader** that runs in CI (`parse_line`, `Status`, `Eta` are pure). port/ui has no C++ test target yet.

## 11. Tested

- **Unit tests** (measured): `cd tools/setup && python -m unittest discover -s tests`, 50 tests in 7.0 s, all pass. The 17 new ones (test_pipeline.py), on test_setup's made-up dump and test_lift's fake tools:
  - a full run, its protocol lines, the bar never going back, `run.log` equal to what was printed; then a second run with every phase skipped and no tool called;
  - `--check-only`; missing decrypted files (`@error game`, `@done fix`, a `@check missing` line); the saved folders; no folders at all;
  - a failed lifter (`@error step ppu`, `@log`), then a resume that starts at the lifter;
  - a run while another holds the lock; not enough disk;
  - `ready.txt` from a fake compile phase, then `status --json`'s ready, outdated and damaged; a later failed run leaves no `ready.txt`; interrupted and missing;
  - the emitter's lines; the two checks that the C++ agrees with the Python (keywords, error codes, the protocol version, the Python pin).
- **The command** (measured): `run_setup.py status --json` on an empty folder says `missing`; `run` with no folders says `@error game check Choose the game folder first.`; `run --check-only` on a folder with no dump prints the game check's sentence as `@check` and `@error` lines and exits 1. The engine printed `@awake` on this PC.
- **Not built, not run**: the C++ (child_process, setup_engine, setup_protocol, setup_window, the des_ui.cpp and report.cpp changes) was written without a compiler; the frame-rate A/B on this PC had the build slots. To see the screen: build `des_boot` as usual, then start it with `DES_UI_OPEN=setup DES_SETUP_GAME=<dump folder> DES_SETUP_DATA=<a scratch folder>` (and `DES_SETUP_PYTHON=python` if the embeddable Python is not fetched). The screen comes up over the game's start; a run with every step already done takes seconds, a fresh one about 80 s for the lift (docs/81 section 2).

## 12. Open questions

1. **The launch hook in ps3recomp** (section 3), a weak `ps3_title_launch(int* argc, char*** argv)` at the top of `main`. Recommendation: yes. It is the one way that works on both platforms and lets the host start with no arguments.
2. **The first Python through Windows' `curl.exe` and `tar.exe`** (section 5), with the host checking the SHA-256. Recommendation: yes. The other way is a few hundred lines of HTTP and zip code in the host.
3. **Play restarts the host** after setup (section 7). Recommendation: yes, for a clean boot. The other way keeps one process and starts RmlUi and D3D12 a second time in it.
4. **Reports from setup include the failed step's log tail** (section 6), with names and addresses but no game code once the compile runs with `-fno-caret-diagnostics`. Recommendation: yes; the player sees the zip before attaching it.
