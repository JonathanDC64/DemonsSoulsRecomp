# 90. The setup screens' RPCS3 route (2026-10-22)

The setup design's first card on "Where is your game?" is "Import from RPCS3". Setup finds the game in the player's RPCS3 library. It has the player's own RPCS3 unlock the 27 files the build needs. It can also bring the save, the trophies and some settings across. This note designs that route to the user's decisions of 10-22. It covers what each page says, what the engine does, what must be fixed first, and how to test it. The engine's unlock phase is built (section 8); the host's screens, the library reading and the import are not. One run of the player's RPCS3 (tests T1 and T3, section 5) has checked the redirect.

It builds on docs/88 (the engine, its line protocol, the screens before a run), docs/89 (the shell), docs/91 (the options pages) and docs/51. In docs/51's terms this is option (c+), "setup runs the player's RPCS3". That doc's section 8 question 1, for a lawyer, still stands. The RPCS3 facts come from RPCS3's source at 065b490, through the 10-22 research on `rpcs3 --decrypt` and on where RPCS3 keeps things. Where a design point rests on a single line of that source, the line was read again here.

Labels: **measured** (a run says so), **read** (seen in the code or a doc), **inferred** (reasoned, not run). Only section 5's first block is **measured**. Paths such as `C:\Games\RPCS3` are examples only. `<data>` is setup's data folder, `%LOCALAPPDATA%\DemonsSoulsRecomp` (Linux: `$XDG_DATA_HOME/DemonsSoulsRecomp`). `<R>` is the player's RPCS3 config folder.

## Short answer

- **Setup runs the player's own RPCS3 with `--decrypt` on copies of the 27 files**, kept in setup's own folder (`<data>/setup/unlock/`). Setup ships no keys and no decryptor. It reads only a few header bytes, to make sure RPCS3 gets the right files. It checks each result's size and SHA-256 against the table it already has: game_table.json holds all 27 sums (read). RPCS3's exit code is not trusted, because it is 0 after a failure too (read).
- **RPCS3 is given a folder of setup's own for its files.** On Windows this is **measured** (one run, section 5): RPCS3 wrote everything in setup's folder and nothing in its own. Linux is still inferred.
  - On Windows, `RPCS3_CONFIG_DIR` points at `<data>/setup/unlock/rpcs3/`. On Linux, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` do.
  - RPCS3 writes a log, a lock, a config file and a dev_hdd0 tree at every start. With this, all of them land in setup's folder.
  - So nothing is written in the player's RPCS3 folder, and an RPCS3 the player has open gives no "Another instance" dialog.
- **The folder is not under %TEMP%.** RPCS3 refuses to start when its folder is in a temporary location (read). With the redirect, its folder is setup's.
- **A `portable` folder next to rpcs3.exe beats the variable** (read). For those installs, setup asks the player to close RPCS3, and says that RPCS3 replaces its last log. Open question 1 offers a second way: copy RPCS3 instead.
- **PPU Debug's `exec.elf` counts as the decrypted EBOOT** when its SHA-256 is the known one. RPCS3 then unlocks only the 26 script files.
- **The import is a copy, done once, at the end of the build.**
  - The save goes into the folder the game reads today, after a backup of any save already there.
  - The trophies are merged in.
  - Four settings become Custom's starting values on the options page: the frame-rate patch, the resolution scale, motion blur and the intro skip.
  - RPCS3's fullscreen setting is not imported.
- **ISO entries in RPCS3's library are refused for now.** The page says so plainly.
- **With several RPCS3 users**, setup uses the active one and shows which one.
- **Before this ships, the runtime must stop writing `.sdat.dec` files beside the game's files** (edat.c:644, read). Nothing else may write into the dump either (section 4).
- **The engine gains two phases, `unlock` and `import`, and six protocol lines**: `@rpcs3`, `@user`, `@library`, `@found`, `@setting` and `@file`. The protocol becomes version 2. Built so far: the unlock, `@rpcs3`, `@file` and version 2 (section 8).

## 1. What the player sees

Each page is a `SetupPage` (docs/89 section 2). The texts below are what the screen shows. The rows use the design's `found` and `check` parts.

### 1.1 Welcome

No change, with one condition. The "Nothing to install" row says "Setup never changes your game folder or RPCS3." That stays true only if the redirect works (section 2). Question 1 may decide that portable installs are asked to close RPCS3. In that case the line becomes: "Setup never changes your game folder, or RPCS3's saves and settings."

### 1.2 "Where is your game?": the RPCS3 card

The title stays "Import from RPCS3". The first line also stays: "Setup finds your game in RPCS3's library, has your RPCS3 unlock the game files, and can bring your save, trophies and settings across."

| What the host found (section 3.6) | Tag | Second line |
|---|---|---|
| RPCS3, with Demon's Souls in its library as a folder | ok "Found on this PC" | "C:\Games\RPCS3. Demon's Souls is in its library." |
| RPCS3, with Demon's Souls as an ISO file | ok "Found on this PC" | "C:\Games\RPCS3. Demon's Souls is in its library as an ISO file, which setup cannot use yet." |
| RPCS3, without Demon's Souls | ok "Found on this PC" | "C:\Games\RPCS3. Demon's Souls is not in its library yet." |
| No RPCS3 | muted "Not found" | "Setup shows you how to get RPCS3 ready, or where to point it if it is somewhere else." |

The design's tag said "Not installed". Setup cannot know that, so the tag says "Not found".

### 1.3 "Your game in RPCS3" (RPCS3 found)

The page opens with a spinner row, "Reading your RPCS3...", while `run --check-only --rpcs3` runs (section 3.2). That takes about as long as the folder page's check, because it hashes the same files (inferred).

- **Heading:** "Your game in RPCS3".
- **Lede (normal case):** "Setup reads your game, save and settings from RPCS3. It never changes RPCS3's files or settings."
- **Lede (portable install, `@rpcs3 shared`):** "Setup reads your game, save and settings from RPCS3, and never changes them. To unlock the game files it starts your RPCS3 once, which replaces RPCS3's last log."

The design's "Setup only reads from RPCS3." is not quite true here, because setup runs RPCS3. The ledes above say what really happens.

**Panel rows:**

| Row | Mark | Key | Value | Right |
|---|---|---|---|---|
| RPCS3 | ok | RPCS3 | its folder (mono) | link "Change" |
| User (only when RPCS3 has 2 or more users) | ok | RPCS3 user | "Player (00000002). RPCS3 has 3 users; this is the one it uses now." | |
| Game, supported | ok | Demon's Souls, US disc, version 1.00 | `BLUS30443` · its folder | tag ok "Supported" |
| Game, ISO | err | Demon's Souls, as an ISO file | "Setup cannot use ISO files yet. Copy the game out of the ISO into a folder, add that folder to RPCS3's library, then press Check again." | tag err "Not yet", link "Check again" |
| Game, not in the library | warn | Demon's Souls is not in RPCS3's library | "Add it in RPCS3 (File, then Add Games), then press Check again." | link "Check again" |
| Game, other region or store copy | err | as the design (European disc, PlayStation Store version) | as the design | tag err "Not supported" |
| Game, damaged dump | err | Demon's Souls, US disc, version 1.00 | the check's sentence, for example "PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat does not match the US disc, version 1.00. The dump may be damaged: dump the disc again." | tag err "Needs fixing", link "Check again" |

**"Bring across from RPCS3"** (only when the game is supported). There are three checkbox rows, each ticked when its state is `ok`. A row whose state is not `ok` is unticked and cannot be ticked:

| Row | State | Line |
|---|---|---|
| Save data | ok | "3 characters, last saved 19 Oct 2026, 22:41. Copied, so RPCS3 keeps its own." When the character count is unknown: "Last saved 19 Oct 2026, 22:41. Copied, so RPCS3 keeps its own." |
| | ok, and the port already has a save | adds: "It replaces the save you have here. A backup is kept." |
| | none | "This RPCS3 user has no Demon's Souls save." |
| | other-user | "This RPCS3 user has no Demon's Souls save. Another user has one: switch to that user in RPCS3, then press Check again." |
| | busy | "RPCS3 is saving the game right now. Close the game in RPCS3, then press Check again." |
| Trophies | ok | "14 of 38 unlocked." |
| | none | "No trophies yet for this RPCS3 user." |
| Settings and patches | ok | "Frame rate 60 (Unlock FPS patch), render resolution 150%, motion blur off, intro skipped. After the build, the Custom options start from these." |
| | none | "RPCS3 runs the game with its own settings, so there is nothing to bring across." |

The settings line lists only the values that differ from the game's own, in this order: frame rate, render resolution, motion blur, intro skip. The words come from the host. The values come from `@setting` lines.

**Buttons:** Continue (primary, pressable only when the game is supported) and Back (quiet). Continue goes to the unlock page when some files still need unlocking. Otherwise it goes straight to Ready to build.

### 1.4 "RPCS3 is not installed" (RPCS3 not found)

The design's page stays, with its "Choose folder" button and the folder dialog. The folder must hold rpcs3.exe; on Linux it may instead hold an `rpcs3*.AppImage`. Its last line changes, because RPCS3 has no settings folder of its own to look in on Windows:

- **Windows:** "Looked in: RPCS3's shortcuts, Desktop, Documents, Downloads, the Games and Emulators folders, and the root of each drive."
- **Linux:** "Looked in: Flatpak, the installed programs, and AppImages in Applications, Downloads and Desktop."

A folder that holds no RPCS3 gets this on the page: "There is no RPCS3 in this folder. Pick the folder that holds rpcs3.exe."

### 1.5 "Unlocking the game files"

The page starts the unlock by itself when it opens. The exception is a portable install, below.

- **Heading:** "Unlocking the game files".
- **Lede, all 27 files:** "Your RPCS3 unlocks the game's code and its 26 script files. It works on copies in setup's own folder, and setup checks each result."
- **Lede, when the EBOOT was already there** (PPU Debug's copy, or one from an earlier run): "The game's code is already unlocked, so your RPCS3 unlocks the 26 script files. It works on copies in setup's own folder, and setup checks each result."
- **While running:** "File {n} of {m}", then the file's path under USRDIR (small, mono), then the bar. The numbers come from `@progress unlock n/m` and the name from `@file unlock <path>`.
- **Done:** the left text becomes "All {m} files unlocked and checked". A row follows: ok, "EBOOT.elf and 26 script files", "All match the known good version. The copies are deleted." Continue (primary) gets the focus.
- **Portable install, before starting:** a warn callout, titled "Close RPCS3 first": "Setup starts your RPCS3 once to unlock the files. Like any start of RPCS3, this replaces its last log." The buttons are Unlock (primary) and Back. Nothing starts until Unlock is pressed.
- **Failure:** a red callout with the engine's sentence (section 1.10), then the host's sentence for the `rpcs3` code: "Try again. If it keeps failing, you can unlock the files yourself." The buttons are Try again (primary), Show me how (the help page's "Unlock the files yourself"), Details and Back.

### 1.6 Ready to build

As the design, with the import line reworded.

- **First row:** "From RPCS3's library. All 27 unlocked files match."
- **Import row** (when save or trophies are ticked): ok, "From RPCS3". Its line names the ticked items, then "come across when the build is done." For example: "Your save (3 characters) and 14 trophies come across when the build is done."
- **Settings ticked:** the import row adds "Your RPCS3 settings are offered on the options page."
- **Back** returns to "Your game in RPCS3".

### 1.7 Building

The progress page (docs/89 section 3) gets two more task lines:

| Line | Name | The line under it | Count |
|---|---|---|---|
| unlock | Unlock the game files | Your RPCS3 does this, on copies | files |
| import | Bring across from RPCS3 | Your save and trophies | (none) |

The unlock page has already done the unlock, so in a build its line usually reads "Already done". A Repair after the kept files were lost runs it again here.

### 1.8 Options

docs/91 is unchanged, with these differences when "Settings and patches" was ticked:

- The Custom card says "Starts from your RPCS3 settings." This is the design's own text; docs/91 waited for this route to use it.
- When Custom is picked, the lead line of "A few choices" says "Set from your RPCS3 settings."
- The frame rate and render resolution start from RPCS3's values. A value that is not one of the offered choices shows as one more choice (docs/91 1.2).
- Finish with Custom also stores the imported values that have no row on "A few choices": motion blur and the intro skip.
- Original and Enhanced ignore the imported values. They are presets, and Original is the game as it was on the PS3.

### 1.9 Ready to play

These rows are added to docs/91's "Ready to play":

- **Save** (when it was imported): ok, "Your RPCS3 save", "3 characters brought across. RPCS3 keeps its own copy." When a save of the port's own was replaced, add: "Your old save is in save-backups."
- **Trophies** (when they were imported): ok, "Trophies", "14 of 38 unlocked."

### 1.10 The engine's sentences on this route

These are the first sentences of `@error rpcs3 unlock ...` (section 3.4) and `@error import import ...` (section 3.5). The host adds its own second sentence for each code (section 3.8).

| When | Sentence |
|---|---|
| The remembered RPCS3 is gone | "RPCS3 is not in C:\Games\RPCS3 any more." |
| Portable install while RPCS3 runs, or a build that ignored the redirect while RPCS3 was open | "RPCS3 is open. Close it so setup can start it." |
| `Unknown option` from RPCS3 | "This RPCS3 is too old to unlock the files. Update RPCS3." |
| No progress for 30 s (the watchdog) | "RPCS3 stopped answering while it unlocked chr/m02.chresdbnd.dcx.sdat." |
| An output is missing after a retry of that file alone | "RPCS3 could not unlock script/m03.luabnd.dcx.sdat." |
| RPCS3's log says `Failed to create` | "RPCS3 could not write in setup's folder. Your antivirus may be blocking it." |
| An output's size or SHA-256 is wrong | "EBOOT.elf came out different from the known good version." |
| RPCS3 ended on an abort (exit status 3 or 0xC0000409) | "RPCS3 could not start." |
| Import: RPCS3's save changed during the copy | "RPCS3 saved the game while setup was copying it." |
| Import: a copy, conversion or rename failed | "Bringing your save across failed." |

## 2. Running RPCS3 without touching its folder

This is the design point the route rests on. It was inferred from RPCS3's source. One run on Windows has since checked it (section 5): the redirect works, and RPCS3 writes no log in this mode. The Linux half is still inferred.

### 2.1 What one start of RPCS3 changes, `--decrypt` included (read)

All of these are made under RPCS3's config, cache or emulator folder:

- It overwrites `log/RPCS3.log`, `RPCS3.log.gz` and `TTY.log`.
- It creates `config/config.yml` if that file is missing or empty. On Windows it first moves an old top-level `config.yml` into `config/` (System.cpp:502-523).
- It writes `config/uuid` if missing.
- It creates the dev_hdd0, dev_hdd1, dev_flash, dev_usb000 and games trees, `home/00000001/localusername`, and the "Disc Games Can Be Put Here" file. It also creates the cache, captures, sounds and patches folders.
- It finishes or removes interrupted savedata backups, empties `dev_hdd0/tmp/` and removes `_GDATA_*` folders.
- It takes an exclusive lock on `<cache>/RPCS3.buf`. When the lock is held, RPCS3 shows a modal "Another instance of RPCS3 is running" dialog before headless mode starts, then exits 0 (rpcs3.cpp:610-640).

### 2.2 Where those folders are (read)

From `fs::get_config_dir` and `get_cache_dir` (File.cpp), `get_emu_dir` (system_utils.cpp:148-152) and `vfs_config.cpp`:

1. **`<exe folder>/portable/`, if it is a folder.** This is checked first, on every platform. For an AppImage, the exe folder is the AppImage file's folder.
2. **Windows:** `RPCS3_CONFIG_DIR`, if set. RPCS3 turns every `\` into `/`, then cuts the value after its last `/`. So the value must end in a separator; otherwise RPCS3 uses its parent folder. The value is read as UTF-16 and converted, so non-ASCII paths are fine.
3. **Windows:** otherwise the exe folder.
4. **Linux:** the config folder is `$XDG_CONFIG_HOME/rpcs3/`, else `~/.config/rpcs3/`. The cache folder is `$XDG_CACHE_HOME/rpcs3/`, else `$XDG_CONFIG_HOME/rpcs3/`, else `~/.cache/rpcs3/`. RPCS3 creates both.

Where the rest goes:
- On Windows the cache folder is the config folder, and the log is in `<config>/log/`. On Linux the log is in the cache folder.
- The lock is in the cache folder.
- The emulator folder (`$(EmulatorDir)`) is `vfs.yml`'s `emulator_dir`, else the config folder. dev_hdd0 is `$(EmulatorDir)dev_hdd0/` unless `vfs.yml` says otherwise.
- `vfs.yml` is read from `<config>/config/` on Windows.

### 2.3 The proposal (inferred)

Every path in 2.1 is derived from the config or cache folder (read). So the engine gives the RPCS3 child a fresh, empty folder of setup's own, `<data>/setup/unlock/rpcs3/`:

| Platform | Child's environment | Expected result |
|---|---|---|
| Windows | `RPCS3_CONFIG_DIR=<data>\setup\unlock\rpcs3\` (trailing backslash) | config, cache, log, lock and dev_hdd0 all in that folder |
| Linux (package, AppImage) | `XDG_CONFIG_HOME=<data>/setup/unlock/rpcs3/config`, `XDG_CACHE_HOME=<data>/setup/unlock/rpcs3/cache` | `.../config/rpcs3/` and `.../cache/rpcs3/` |
| Linux (Flatpak) | the same two, passed with `--env=` (section 3.7) | the same, if `--env` overrides Flatpak's own XDG values |

What this should give:
- **The player's folder is untouched.** The child still reads its exe folder (Qt and RPCS3's DLLs) and checks whether `portable/` exists, but writes nothing there.
- **The fresh `vfs.yml` is absent, so RPCS3 uses its defaults.** A `vfs.yml` of the player's that moves dev_hdd0 to another drive is never read.
- **No "Another instance" clash with an RPCS3 the player has open.** The lock is in setup's folder.
- **RPCS3 writes no log in this mode (measured).** The run left only an empty `log/TTY.log` in setup's folder, and no `RPCS3.log`, although decrypt_binaries.cpp logs each result (read). So the engine learns the result only from the output files. Its step log records each file's size and SHA-256, RPCS3's exit code, and RPCS3's version, read from the first line of the player's own `RPCS3.log` (read-only; `RPCS3 v0.0.43-20290-eda7621f Alpha | master` on the test PC). rpcs3.exe's version resource is empty (measured).

The player may have set `RPCS3_CONFIG_DIR` themselves. Setup then reads their data from that folder (section 3.1), and the child gets setup's value instead.

### 2.4 Why `<data>` and not %TEMP% (read, then inferred)

Unless `--allow-any-location` is passed, RPCS3 checks its emulator folder before the decrypt branch (rpcs3.cpp:996-1040). It checks the folder against Qt's temp locations, `/Rar$` and OneDrive's folders, and stops with a fatal error if the folder is inside one of them. That is an abort, exit status 3 or 0xC0000409. The check runs on Linux too, without the OneDrive part.

With the redirect, the emulator folder is setup's folder. So it must not be under %TEMP%. `<data>` is `%LOCALAPPDATA%\DemonsSoulsRecomp`, which is neither a temp nor a OneDrive folder (inferred). Controlled Folder Access does not guard it by default either (inferred).

Setup does not pass `--allow-any-location`: an older build may not know the option (inferred), and the check guards against a bad setup folder.

### 2.5 When the redirect cannot work: portable installs

`portable/` wins over the variable (read, step 1 in 2.2). For such an install:

- **Before any run, setup sees `<exe folder>/portable/`** and reports `@rpcs3 shared <path>`. The RPCS3 and unlock pages then show their portable variants (1.3, 1.5).
- **When Unlock is pressed, the engine looks for a running RPCS3 from that install**, by the process image path. It opens none of RPCS3's files to do this: a test open of `RPCS3.buf` would not see the lock on Windows, and `flock` on Linux takes it. A running copy gives "RPCS3 is open. Close it so setup can start it."
- **Then it runs RPCS3 in its own folder.** That replaces the log and empties `dev_hdd0/tmp`, as any start does, and the page has said so.

An RPCS3 build too old to know `RPCS3_CONFIG_DIR` behaves the same way, but setup only learns that afterwards (inferred: the variable is old, but its age was not checked). After every run the engine checks that `RPCS3.buf` and `log/` appeared in setup's folder. If they did not, it writes "RPCS3 ignored RPCS3_CONFIG_DIR" in the step log and treats this install as shared next time. If outputs are also missing, the likely cause is the "Another instance" dialog, so the sentence is "RPCS3 is open. Close it so setup can start it."

### 2.6 The console window (Windows)

The decrypt branch first calls `attach_console`. That tries `AttachConsole(ATTACH_PARENT_PROCESS)` and, if that fails, `AllocConsole()`. It then reopens stdout as `CONOUT$` and stdin as `CONIN$` (console.cpp:12-39, read). rpcs3.exe is a GUI program (read).

- **The engine has a console with no window.** The host starts it with `CREATE_NO_WINDOW` (docs/88 section 5). So RPCS3, the engine's child, should attach to that hidden console, and no window should open (inferred).
- **This only holds if the engine starts RPCS3 as a plain child.** It must not use `DETACHED_PROCESS`, which would make the attach fail and open a new console (inferred). `CREATE_NO_WINDOW` is ignored for GUI programs (inferred).
- **As a second guard, the engine starts RPCS3 with `STARTF_USESHOWWINDOW` and `SW_HIDE`.** That should hide a console from `AllocConsole` if the attach fails (inferred). Whether it does so with Windows Terminal as the default terminal app is unknown (inferred, test T7).
- **RPCS3's own lines go to the console, not to any pipe** (inferred). The engine's protocol stream stays clean. The engine reads the results from the files and RPCS3's log. It gets RPCS3 its own stdout and stderr pipe, never the engine's own stdout. That pipe carries what RPCS3 prints before the attach, such as Qt's "Unknown option".
- **From a developer's terminal**, RPCS3 attaches to that terminal and prints there. That is harmless.

### 2.7 The KLIC prompt, and the watchdog

- **The prompt (read).** When a file fails to decrypt, the program asks on the console `Enter KLIC of <file>` and reads `std::cin >> input`. The loop ends only when the input comes back empty, which `>>` gives only at end of input: an empty line is skipped (rpcs3.cpp:1111-1136).
  - On Windows stdin is `CONIN$`, so it waits forever on the hidden console (inferred).
  - On Linux `attach_console` does nothing, so stdin from `/dev/null` gives end of input and the process exits 0 (read and inferred).
- **What leads there (read).** A SELF or NPD file that does not decrypt with the built-in keys, or a file that is neither (decrypt_binaries.cpp:96-178). A failure also stops every file after it in the batch.
- **How setup avoids it.**
  - Before RPCS3 runs, each copy is hashed again. It must equal the table's sum for the encrypted original, which catches a copy error.
  - Its first four bytes must be `SCE\0` for EBOOT.BIN, or `NPD\0` for a `.sdat`. A `.sdat` must also have the SDAT flag, `0x01000000` in the big-endian flags word at 0x80. Its plain size, a big-endian u64 at 0x88, must equal the table's decrypted size.
  - A disc SELF decrypts from RPCS3's built-in key vault with no KLIC and no RAP. An SDAT's key is fixed, `dev_hash XOR SDAT_KEY`, and needs no file name check (unedat.cpp:811-816, read).
  - So with the known inputs, the prompt can only come from an RPCS3 build that cannot decrypt these exact files (inferred).
- **The watchdog.**
  - The engine polls every 250 ms. Progress is a new output file or a growing one. RPCS3's log cannot count: it writes none in this mode (measured).
  - After 30 s with none, it ends this child only: its own process handle on Windows (`TerminateProcess`), or its own process group on Linux. It then names the first file with no output.
  - The normal end is the process exiting, and the stall clock restarts at every sign of progress, so the 30 s never cuts off a slow but working PC (inferred).
  - The engine never ends RPCS3 processes by name, and never uses `flatpak kill`: either would take the player's RPCS3 too.
- **Cancel** ends the child as well, through the host's job object (docs/88 section 6). That covers RPCS3 too, as the engine's child (inferred). A Windows Terminal window opened on RPCS3's behalf, if one ever is, would be outside the job (inferred, test T7).

### 2.8 One process at a time, and what counts as success

- **One process at a time.**
  - The engine runs one RPCS3 process for all the files it needs, with one `--decrypt <absolute path>` pair each, in table order. It never uses `--decrypt=`: that form misses headless mode (read).
  - It retries files one by one only after that process has ended.
  - It never runs two RPCS3 children at once, and `run.lock` keeps a second engine out (docs/88).
- **Success is the files, not the exit code.** Exit 0 also follows a failed file, a missing KLIC and `Failed to create` (read). A bad SDAT block gives a short output that is still logged as "Decrypted" (read). So every output must exist, with the table's size and SHA-256.
- **Too old.** An RPCS3 without `--decrypt` should exit 1, with "Unknown option" on stderr. That should hold when `QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1` is set; without the variable, Qt shows a message box on Windows (inferred from Qt).

## 3. What the engine does

### 3.1 Reading the player's RPCS3 (read-only)

This is a new module, `des_setup/rpcs3.py`. Every file is opened for reading only, and nothing is ever written under `<R>` or the dump (docs/44 8.2).

| What | Where (Windows; Linux has no `config/` subfolder) | Used for |
|---|---|---|
| `<R>` | `<exe folder>/portable/`, else the player's `RPCS3_CONFIG_DIR`, else the exe folder. Linux: `$XDG_CONFIG_HOME/rpcs3/` or `~/.config/rpcs3/`. Flatpak: `~/.var/app/net.rpcs3.RPCS3/config/rpcs3/`. | everything below |
| games.yml | `<R>config/games.yml` (older builds: `<R>games.yml`) | the BLUS30443 entry: a disc root, a `PS3_GAME` path ending in `/./`, or an ISO file (System.cpp:2120-2170, game_enumeration.h:445-490, read) |
| vfs.yml | `<R>config/vfs.yml`, if present | `$(EmulatorDir)` and `/dev_hdd0/` |
| active user | `<R>GuiConfigs/persistent_settings.dat`, `[Users] active_user` (default 00000001) | which `home/<id>/`. If that folder is missing: 00000001, else the first user |
| users | `<hdd0>home/<8 digits>/localusername` | the count and the name shown |
| save | `<hdd0>home/<user>/savedata/BLUS30443DEMONSS005/` (PARAM.SFO read by sfo.py) | the save row; `.working_` or `.backup_` beside it means busy |
| trophies | `<hdd0>home/<user>/trophy/NPWR00881_00/TROPUSR.DAT` | the type-6 table, state 1 (docs/44 8.5) |
| settings | `<R>config/config.yml`, then `custom_configs/config_BLUS30443.yml` over it, then `patch_config.yml` (BLUS30443, 01.00, `Enabled: true`) | section 3.5. The console ID and GPU name are skipped; only key names are logged |
| PPU Debug EBOOT | `<cache>/ppu_progs/BLUS30443-*-EBOOT.BIN/exec.elf`, where the middle is a base57 of the module's SHA-1 (System.cpp:390-400, read) | accepted when its size and SHA-256 equal the table's `EBOOT.elf` |
| store copy | `<hdd0>game/NPUB30910/` | the "Not supported" row |

The YAML is read with a small subset reader: two-space indents, `key: value` and quoted scalars (docs/44 8.6). No new library is needed.

### 3.2 The command line and the plan

New `run` options, added to docs/88's `run [GAME] [--decrypted DIR]... [--data DIR] [-j N] [--app VERSION] [--check-only]`:

| Option | Meaning |
|---|---|
| `--rpcs3 PATH` | The RPCS3 to use: the folder with rpcs3.exe, an AppImage file, a command on `PATH`, or `flatpak:net.rpcs3.RPCS3`. Without GAME, the game is RPCS3's library entry. |
| `--unlock-only` | Plan `check unlock`. This is the unlock page's run. |
| `--import ITEMS` | `save`, `trophies` and/or `settings`, comma-separated: what to bring across at the end. |
| `--no-import` | Drop a pending import. This is the import failure's "Go on without it". |
| `--gamedata DIR` | The game's `gamedata` folder for this install, from the host. The save goes in `DIR/dev_hdd0/home/00000001/savedata/`, backups in `DIR/save-backups/<date-time>/`, and trophies in `$PS3_TROPHY_DIR`, else `DIR/trophies/NPWR00881.json`. |

The plans:

| Run | Plan |
|---|---|
| RPCS3 page (`--check-only --rpcs3`) | `check`, plus the RPCS3 lines (section 3.8) |
| unlock page (`--unlock-only`) | `check unlock` |
| Start building | `check unlock fetch stage lift compile import` |
| Resume, Repair, Update | the same plan, from `request.json`. Unlock skips while the kept files are good. Import runs only while one is pending. |

- **Weights:** unlock 1, import 1, beside docs/88's check 2, fetch 4, stage 1, lift 13 and compile 80.
- **Unlock comes before fetch.** An RPCS3 problem then shows before the large download.
- **`ready.txt` is written after `import`, as today after the last phase.** So the start screen is never Ready while an import the player asked for is still pending.
- **`request.json` gains `rpcs3` and `import`.** The import entry is removed once the import is done, or when `--no-import` is passed. A later Update or Repair therefore never brings an older RPCS3 save over progress made in the port.
- **The host remembers the RPCS3 path** in settings.ini as `rpcs3_dir`, a UI-only key (docs/44 8.2), and passes it as `--rpcs3`.

### 3.3 The check, with RPCS3

The check is gamefiles.py, with three changes:
- **Decrypted files are also looked for in `<data>/inputs/`**, at their table paths: the files an earlier run kept. Setup writes nothing beside the dump on this route, so this is where they live. A Repair or Update then needs neither RPCS3 nor the player.
- **On the RPCS3 route, PPU Debug's `exec.elf` is a place to look for `EBOOT.elf`.**
- **On the RPCS3 route, a missing decrypted file is not something to fix.** The unlock makes it. The `@check missing` line stays, with the sentence "Your RPCS3 unlocks it next." `--check-only` ends `@done ok` when the game itself is fine.

Two smaller changes go with these:
- **The stage skips an item whose `found` path already is its destination**, because `copyfile` onto itself fails.
- **An ISO entry gets `@library iso <path>`**, then `@error game check Setup cannot use ISO files yet.` and `@done fix`. The page shows the ISO row from `@library`.

### 3.4 The unlock phase, step by step

The phase is journal step `unlock`. Its fingerprint is the sorted (name, SHA-256) list of the encrypted originals it unlocks. Its outputs are the `<data>/inputs/` files.

1. **Which files.** The decrypted items the check did not find. With none, `@step unlock skip`.
2. **Which RPCS3.** The one from `--rpcs3`, or `request.json`, checked again. It is shared if `portable/` exists; shared and running gives `@error rpcs3` (2.5).
3. **A clean folder.** Delete a leftover `<data>/setup/unlock/` from a stopped run, then create `in/` and `rpcs3/`.
4. **The copies.** Copy each original to `in/<table path>`, keeping its name and the USRDIR folders. Hash the copy against the table, then check its magic, its SDAT flag and its plain size (2.7). A failure here is a setup bug or a disk error: `@error step`.
5. **One RPCS3 process.** The command is `[rpcs3, --decrypt, <abs>, --decrypt, <abs>, ...]`, started in `<data>/setup/unlock/`.
   - Environment: the engine's own, plus the redirect variables of 2.3 (unless shared) and `QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1`.
   - stdin from the null device, stdout and stderr to a pipe the engine reads into the step log.
   - Windows: `STARTF_USESHOWWINDOW` and `SW_HIDE`. Linux: a new session (its own process group).
6. **Watching.** Every 250 ms, print `@progress unlock <done>/<m>` and `@file unlock <path under USRDIR>` for the first input with no output yet. RPCS3 handles the inputs in order (decrypt_binaries.cpp), so a newer output means the one before it is finished (read and inferred). The watchdog of 2.7 runs alongside.
7. **After it ends.**
   - Copy the `DECRYPT:`, `SELF:` and `EDAT:` lines and RPCS3's version line from setup's RPCS3 log into `<data>/setup/logs/unlock.log`. (Built: RPCS3 writes no log in this mode, so the step log gets what RPCS3 printed before it took the console, its exit status, each output's size, and the version line of the player's own `RPCS3.log`, read only.)
   - Check that the redirect worked (2.5).
   - Read the exit status only for the abort and "Unknown option" cases.
8. **Checking.** Every output (`EBOOT.elf`, `<name>.sdat.unedat`) needs the table's size and SHA-256.
9. **One file at a time.** For each missing or wrong output, run RPCS3 again with that file alone, to name the one that fails. If it still fails, `@error rpcs3 unlock <sentence>` names it.
10. **Keeping the results.** Move each good output to `<data>/inputs/<table path>`: written to `.tmp`, then renamed, as the stage does. (Built differently: the outputs stay in `setup/unlock/in/` until the stage, which finds them there like a `--decrypted` folder and copies them into `inputs/`. Section 8.)
11. **Cleaning up.** Delete `<data>/setup/unlock/`, after a success or a failure: the encrypted copies, and RPCS3's config, log and dev_hdd0 tree. (Built: after a success the copies and RPCS3's folder go at once and the outputs after the stage; after a failure all of it.) The decrypted results are now in `inputs/`, kept for as long as the game is installed (docs/52 section 3).
12. **The check again,** for the unlocked items, so `rep.inputs()` holds them for the stage.

### 3.5 The import phase

This is journal steps `import-save` and `import-trophies`, plus the settings file. It runs only while `request.json` has a pending import. The game is not running during setup, so docs/44's counter is not needed here.

**The save** (docs/44 8.4, with decision 4: the folder the game reads today):
1. **Stop on a busy save.** A `.working_` or `.backup_` folder beside RPCS3's save means RPCS3 is mid-save.
2. **Note the newest data file's time.**
3. **Back up.** If the port already has a save, copy it to `<gamedata>/save-backups/<date-time>/BLUS30443DEMONSS005/`. Backups are never deleted. A failed copy stops the import.
4. **Stage.** Build `<gamedata>/import/BLUS30443DEMONSS005/`:
   - the data files, ICON0.PNG and PIC1.PNG, copied raw;
   - a text PARAM.SFO from the binary PSF's TITLE, SUB_TITLE, DETAIL, ATTRIBUTE and SAVEDATA_LIST_PARAM (sfo.py reads the PSF);
   - PARAM.PFD from the `*<file>` = 1 keys.
5. **Compare the newest time again.** If it changed, RPCS3 saved during the copy: `@error import`.
6. **Swap.** Rename the port's folder out of the way (its copy is already in the backup), then rename the staged folder in. Both are on one drive. RPCS3's copy is never moved.

**The trophies:**
- Copy `NPWR00881.json` aside first.
- Merge TROPUSR.DAT's unlocked trophies as a union, where the earlier time wins (docs/44 8.5).

**The settings:**
- Write `<data>/setup/rpcs3_settings.txt`, as `key=value` lines like `ready.txt`, so the host reads it without a JSON parser.
- The options page reads it for Custom (1.8). Finish deletes it once a preset is stored.

| RPCS3 | settings.ini | Rule |
|---|---|---|
| "Unlock FPS" patch, with Frame limit, Second Frame Limit and Vblank Rate | `DES_FPS` | Patch on: the lowest limit that is set. Off and Infinite count as no limit, and the default vblank of 60 gives 60. Patch off: not imported. |
| Video: Resolution Scale | `RSX_SCALE` | copied, kept within 50-800 |
| "Disable Motion Blur" patch | `DES_MOTION_BLUR=0` | |
| "Skip Intro Videos" patch | `DES_SKIP_INTRO=1` | Whether the patch also skips the opening movie is still open (docs/44 section 11) |
| Start games in fullscreen mode | none | not imported (decision 5) |

Question 2 asks about further settings.

### 3.6 Finding RPCS3 (in the host)

The card on "Where is your game?" shows before any Python exists. So the host finds RPCS3 in C++, as it finds the likely game folders (docs/88 2a), and reads only the BLUS30443 line of games.yml. The engine does all the rest. It checks the host's choice again before it uses it.

**Windows**, in this order:
1. **Remembered paths:** `RPCS3_DIR` (a developer's), then settings.ini's `rpcs3_dir`.
2. **RPCS3's own shortcuts:** the targets of the `.lnk` files RPCS3 makes, in Start menu `Programs\RPCS3\` and on the Desktop. A `BLUS30443` argument there also confirms the game.
3. **A running rpcs3.exe:** its image path.
4. **The usual folders:** two levels into Desktop, Documents, Downloads, the root of each fixed drive, `\Games` and `\Emulators`, plus Scoop's `apps\rpcs3\current`. A folder counts when it holds rpcs3.exe and one of `config\`, `portable\` or `dev_hdd0\`. RPCS3 writes no registry key and has no installer, so there is no record of its folder to read (rpcs3.cpp:127, update_manager.cpp:588-699, read).
5. **The player's pick.**

Two installs found: the one whose games.yml has BLUS30443 wins, and "Change" lets the player pick (inferred: this is enough).

**Linux:** Flatpak `net.rpcs3.RPCS3` (its folder under `~/.var/app/`, or `flatpak info`), then `rpcs3` on `PATH`. After that, AppImages named in RPCS3's own `~/.local/share/applications/RPCS3/*.desktop` files, then `rpcs3*.AppImage` in `~/Applications`, `~/Downloads`, `~/Desktop` and `~/.local/bin`, then the player's pick.

### 3.7 Linux: package, AppImage, Flatpak

- **Same code, no display.**
  - The decrypt path is a `QCoreApplication` and needs no display (inferred from Qt).
  - `QT_QPA_PLATFORM` is left alone: `offscreen` would make the "Another instance" dialog invisible and never-ending (inferred).
  - stdin is the null device, so a prompt ends at once (2.7).
  - RPCS3 also prints `Found AppImage path:` or `Found exec path:` lines (read). They are ignored.
- **AppImage.**
  - The engine runs the file itself, with the redirect variables.
  - A `portable` folder beside the AppImage makes it shared (read).
  - If it fails to start for want of FUSE, the engine retries once with `APPIMAGE_EXTRACT_AND_RUN=1` (inferred).
- **Flatpak.**
  - The command is `flatpak run --filesystem=<data>/setup/unlock --env=XDG_CONFIG_HOME=... --env=XDG_CACHE_HOME=... net.rpcs3.RPCS3 --decrypt ...`.
  - Inferred: `--env` overrides Flatpak's own XDG values, `--filesystem` shows setup's folder at the same path, and ending the `flatpak run` process group ends the sandbox.
  - The player's data is read from `~/.var/app/net.rpcs3.RPCS3/`.
- **The Linux setup window itself is not built yet** (docs/88 section 8). This route follows it there.

### 3.8 Protocol additions

These go into protocol.py, with the same rules as docs/88 section 4: names have no spaces, and `<text>` and `<path>` are the rest of the line. **The version becomes 2**, because the plan has new phases and `@error` has new codes. A v1 host would show those wrongly. The host and the engine ship together, and `HostAgreesTest` keeps their keyword and code tables equal.

| Line | Meaning |
|---|---|
| `@setup 2` | the protocol's version |
| `@plan ... unlock ... import` | two new phases (3.2) |
| `@progress unlock <n>/<m>` | files unlocked so far, out of those needed |
| `@file <step> <path>` | the file the step works on now; for `unlock`, its path under USRDIR |
| `@rpcs3 <state> <path>` | the RPCS3 in use. `own`: it runs with setup's folder. `shared`: a `portable` install, so starting it changes its log. `missing`: not found. Path: its folder, AppImage, command or `flatpak:<id>`. |
| `@user <id> <users> <name>` | RPCS3's active user: its 8-digit ID, how many users RPCS3 has, and the user's name |
| `@library <kind> <path>` | RPCS3's library entry for BLUS30443: `folder`, `iso`, `store` or `none` (path `-`) |
| `@found save <state> <characters> <saved>` | state `ok`, `none`, `other-user` or `busy`; characters is a count, or `-` when unknown; saved is Unix seconds, or `-` |
| `@found current-save <state> <saved>` | the port's own save, which an import would replace: `ok` or `none` |
| `@found trophies <state> <earned>/<total>` | `ok` or `none` |
| `@found settings <state> <n>` | `ok` or `none`, and how many `@setting` lines follow |
| `@setting <key> <value>` | one imported setting, by its settings.ini name: `@setting DES_FPS 60` |
| `@error rpcs3 <step> <text>` | RPCS3 could not unlock the files (1.10) |
| `@error import <step> <text>` | bringing the save or trophies across failed; the game is built |

The host's second sentence and buttons for the new codes, in the table of docs/88 section 6:

| Code | Second sentence | Buttons |
|---|---|---|
| rpcs3 | Try again. If it keeps failing, you can unlock the files yourself. | Try again, Show me how, Details, Back |
| import | The game is built, and nothing of yours was changed. Try again, or go on without it. | Try again, Go on without it (`run --no-import`), Details |

The host writes the words, and the engine supplies the facts: "3 characters, last saved 19 Oct 2026, 22:41" comes from `@found save ok 3 <seconds>`, dated in the player's locale.

An example, the unlock page's run:

```
@setup 2
@plan check unlock
@step check start
@rpcs3 own C:\Games\RPCS3
@library folder C:\Games\RPCS3\games\BLUS30443
@check ok TITLE_ID BLUS30443, version 01.00.
...
@check missing PS3_GAME/USRDIR/chr/m01.chresdbnd.dcx.sdat.unedat m01.chresdbnd.dcx.sdat.unedat is missing: your RPCS3 unlocks it next.
@step check done
@step unlock start
@file unlock EBOOT.BIN
@progress unlock 0/27
@file unlock chr/c0000/c0000.esd.dcx.sdat
@progress unlock 1/27
...
@progress unlock 27/27
@step unlock done
@overall 1000/1000
@done ok
```

## 4. Prerequisite fixes

These must land before the route ships. Items 1 and 2 also matter for the folder route, but here the dump sits in a library the player still uses.

1. **The runtime must never write `.sdat.dec` beside the game's files** (decision 3).
   - Today `edat_resolve` writes `<name>.dec` next to each `.sdat` it opens and reuses it while it is newer (ps3recomp `libs/filesystem/edat.c:635-652`, `edat.h:41-48`, read). With `PS3_VFS_ROOT` on RPCS3's library folder, that writes into the player's RPCS3 files.
   - The fix (inferred shape): open the `.sdat.unedat` copy from `<data>/inputs/` at the same relative path, and never write a cache. The folder comes from the launch hook, from a new `inputs=` line in `ready.txt`, as an environment variable, for example `PS3_SDATA_DIR`.
   - A developer build that still decrypts writes its cache in a folder of the port's own. Release builds drop the runtime's SDATA decryption altogether, as docs/51 decision 5 proposes.
   - This is a ps3recomp PR first, then the port's pin.
   - **Done (10-23)**, in that shape:
     - ps3recomp's `PS3_EDAT_PLAIN=<folder>` opens `<folder>/<path below PS3_VFS_ROOT>.unedat` in place of the NPD file, as long as its size is the plaintext size the NPD header gives. Nothing is decrypted or written then.
     - `PS3_EDAT_CACHE=<folder>` puts the runtime's own decryption, for a file with no such copy, in that folder as `<name>.<hash of the source's path>.dec`.
     - The launch hook sets them to `<data>/inputs` (where setup stages the player's 26 `.unedat` files) and `<data>/edat`, unless they are set.
     - With neither knob, the runtime does as before. Dropping the decryption from release builds is still to do.
2. **No other write into the dump.**
   - `ppu_fs.cpp` maps `/dev_hdd0/` and `/dev_hdd1/` under the VFS root (ppu_fs.cpp:126-153, read). `cellFs.c` maps `/dev_hdd0/` to `gamedata/dev_hdd0/` (cellFs.c:72, read).
   - Whether the game's own writes go through ppu_fs's mapping is not checked (inferred risk). That includes its `/dev_hdd1` cache.
   - Test T11 compares the dump before and after a session. Any write found there must be moved before shipping.
   - **Found (10-23): the game's `/dev_hdd1` cache was in the dump.** This PC's dump has a `cache/` folder of 955 files (336 MB), written from 10-14 to 10-20 by the runs that used it as the VFS root.
     - The launch hook now sets `PS3_HDD1_ROOT=<data>/hdd1`, unless it is set, and makes the folder. ppu_fs and sys_fs already read that knob; the Linux AppImage already sets it.
     - The game fills its cache again there on its first start; a PS3 game copes with an empty `/dev_hdd1`.
     - The old `cache/` folder stays where it is: nothing deletes a player's files. Setup could tell the player it can go (not done).
3. **The save folder must be one that setup and the game agree on.**
   - The game reads `./gamedata/dev_hdd0/home/00000001/savedata/`, relative to its working folder (cellSaveData.c:53, read), and the trophies relative to it too.
   - A player's start could have any working folder: a shortcut's "Start in" is the player's (inferred).
   - The launch hook (docs/88 section 3) should set the working folder, or a save-root knob, from the install. The host passes the same folder to the engine as `--gamedata`.
   - docs/45 4.4's move of the saves into the data folder is not needed for this route. The welcome's "Your settings and saves stay in %LOCALAPPDATA%\DemonsSoulsRecomp" is not true until it happens.
   - **Done (10-23), the exe's folder:** a player's start (the launch hook's Ready branch) works in the exe's folder, so the saves are `<exe folder>/gamedata/dev_hdd0`, where a double-click has always put them (and where the Linux AppImage's launcher says Windows keeps them).
     - A shortcut's "Start in" or a terminal in another folder no longer moves them.
     - Saves in the folder the game was started in are named in the log, never moved or deleted.
     - `DES_KEEP_CWD=1` leaves the working folder as it is, for test runs that keep their own copy of a save in their own folder.
     - A start with an ELF on the command line (`tools/des`, the AppImage) never reaches the Ready branch, so it is as before.
     - The other choice was the data folder (docs/45 4.4). That is still open for the owner; it would move every player's saves.
4. **The launch hook itself** (docs/88 section 10, item 1). `game=` in `ready.txt` names RPCS3's library folder, and becomes `PS3_VFS_ROOT`.

## 5. What is verified and what is inferred

**Measured** (10-22, one run: tests T1 and T3 together). RPCS3 v0.0.43-20290-eda7621f on Windows 11, closed beforehand, with no `portable` folder. The inputs were copies of EBOOT.BIN and `m06.talkesdbnd.dcx.sdat`, in one process with two `--decrypt` pairs. `RPCS3_CONFIG_DIR` was `%LOCALAPPDATA%\DemonsSoulsRecomp\setup\unlock-test\cfg\`, a fresh folder, removed afterwards.
- Exit 0 after about 1 s, for both files.
- `EBOOT.elf`'s SHA-256 is the table's. The `.sdat.unedat` is 7584 bytes with the table's sum.
- In setup's folder RPCS3 wrote `RPCS3.buf`, `config/config.yml`, `config/uuid`, `dev_hdd0/home/00000001/localusername`, the two text files under `games/`, an empty `log/TTY.log`, and empty folders (captures, dev_bdvd, dev_flash 1-3, dev_hdd1, dev_usb000, patches, ppu_progs, savestates, shaderlog, sounds, spu_progs).
- In the player's RPCS3 folder: `log/RPCS3.log` kept its SHA-256, `dev_hdd0/tmp` kept its items, and no file or folder in the top three levels was written or created during the run.
- **No `RPCS3.log`** in setup's folder. I9 is wrong, and sections 2.3 and 2.7 now say so.

So, on Windows: I1 holds, I3 holds for the EBOOT and one script file, I4's location check passes for a folder under `%LOCALAPPDATA%`, I10 holds for two files, I13 holds for one RPCS3 build, and I14 for one of the 26 script files.

**Read**:
- The `--decrypt` syntax, one value per option; the output names and silent overwrite; exit 0 after failures. (rpcs3.cpp:833-834, 1082-1141; decrypt_binaries.cpp:80-190.)
- The config folder order: `portable/` first, then `RPCS3_CONFIG_DIR` with `\` turned into `/` and a cut after the last `/`, then the exe folder. The Linux XDG rules. The cache, log, lock and emulator folders derived from these. (File.cpp `get_config_dir`, `get_cache_dir`, `get_log_dir`; rpcs3.cpp:610-640; system_utils.cpp:148-152.)
- The location check on the emulator folder, run before the decrypt branch. (rpcs3.cpp:996-1040.)
- The KLIC prompt's `std::cin >> input` and its end condition. (rpcs3.cpp:1111-1136.)
- The SDAT key is fixed, and the file name check is skipped for SDAT. (unedat.cpp:595-650, 811-816.)
- The side effects of a start. (System.cpp:502-523, 632-806.)
- PPU Debug's dump path. (System.cpp:390-400.)
- The runtime's `.sdat.dec` cache (edat.c:635-652), ppu_fs's mounts (ppu_fs.cpp:126-153) and the save root (cellSaveData.c:53).
- game_table.json's 27 decrypted sums and sizes, and its 28 originals.
- The engine's protocol, journal, check and stage.
- The host starts the engine with `CREATE_NO_WINDOW` in a job object (docs/88 section 5).

**Inferred, to be checked by a real run.** Section 7 says which test checks each one.

| # | Claim | Test |
|---|---|---|
| I1 | With `RPCS3_CONFIG_DIR` (Windows) or the XDG pair (Linux) on a fresh folder, RPCS3 writes its log, lock, config, uuid and dev_hdd0 tree there and nothing in its own folder | T1, T10 |
| I2 | Then an RPCS3 the player has open gives no "Another instance" dialog, and both run correctly | T2 |
| I3 | `--decrypt` needs nothing from the config folder for a disc SELF and the SDATs: no firmware, no RAP, no config, so an empty fresh folder works | T1, T3 |
| I4 | A folder under `%LOCALAPPDATA%` passes the location check (not a temp or OneDrive location), and Controlled Folder Access does not block RPCS3 writing there | T1 |
| I5 | Started by the engine, RPCS3 attaches to the engine's hidden console and opens no window. `SW_HIDE` hides a console if the attach fails. Windows Terminal as the default terminal does not change this | T7 |
| I6 | RPCS3's console output never reaches the engine's protocol pipe | T7, T8 |
| I7 | With inputs that match the table, the KLIC prompt is never reached. On Windows a prompt would wait forever; on Linux, stdin from the null device ends it | T5 |
| I8 | Ending the child by its own handle or process group leaves no RPCS3, console host or terminal process behind. The host's job object covers RPCS3 on Cancel | T5, T7 |
| I9 | ~~RPCS3's log in setup's folder is written out by exit, and holds the `DECRYPT:`, `SELF:` and `EDAT:` lines~~ **Wrong (measured): `--decrypt` writes no RPCS3.log** | T1 |
| I10 | One process unlocks all 27 files in seconds, and RPCS3 handles the inputs in the order given | T3 |
| I11 | An RPCS3 without `--decrypt` exits 1 with "Unknown option" on stderr when `QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1` is set | T6 |
| I12 | An RPCS3 build that ignores `RPCS3_CONFIG_DIR` shows by leaving no `RPCS3.buf` in setup's folder | T4 |
| I13 | RPCS3's `--decrypt` writes the same `EBOOT.elf` bytes in other RPCS3 versions, and PPU Debug's `exec.elf` equals it. The table holds one sum | T1 (two builds), T12 |
| I14 | RPCS3's 26 `.sdat.unedat` outputs equal the table's sums. The table does not record where its `.sdat` sums came from | T3 |
| I15 | A running RPCS3 can be found by its image path without opening its files | T4 |
| I16 | games.yml's BLUS30443 line can be read with a one-line parser, and an ISO entry is a file path | T9 |
| I17 | `persistent_settings.dat` is an INI file whose `[Users] active_user` names the user RPCS3 uses | T9 |
| I18 | The character count can be read from the save: SUB_TITLE or DETAIL, or the numbered data files | T12 |
| I19 | A converted RPCS3 save loads in the port (docs/44 R3 and R4) | T12 |
| I20 | A changed newest-file time catches RPCS3 saving during the copy | T9 |
| I21 | The game writes nothing under `PS3_VFS_ROOT` once the `.sdat.dec` fix is in (prerequisite 2) | T11 |
| I22 | Flatpak's `--env` overrides its XDG values, `--filesystem` maps setup's folder at the same path, and ending `flatpak run` ends the sandbox. The AppImage passes the arguments and environment through, and `APPIMAGE_EXTRACT_AND_RUN=1` works without FUSE | T13 |
| I23 | A player's start can have a working folder other than the install's, so the save folder must be pinned (prerequisite 3) | T12 |

## 6. Open questions for the user

1. **Portable RPCS3 installs** (`portable/` beside rpcs3.exe, where the redirect cannot work). There are three ways:
   - (a) Ask the player to close RPCS3, and say that RPCS3 replaces its last log. Like any start, it also empties its `dev_hdd0/tmp`.
   - (b) Copy RPCS3's program files, everything but `portable/`, into setup's folder and run the copy with the redirect. That is AC4-Port's way (docs/31). It touches nothing, but copies on the order of 100 MB (inferred size).
   - (c) Refuse, and point to the manual route.

   Recommendation: (a) first, since it has the fewest unknowns, then (b) if players ask. Under (a), the welcome line changes as in 1.1.
2. **Other RPCS3 settings to bring across**, beyond the four decided: anisotropic filtering and V-sync (plain copies), the aspect ratio patch (21:9 and 32:9 to "Match the display"), Master Volume 0 (mute), and trophy pop-ups. Recommendation: anisotropic filtering and V-sync yes, the rest not yet.
3. **The options page's first focus after a settings import**: Custom, or Enhanced as today (docs/91 1.1). Recommendation: Custom. The player ticked "After the build, the Custom options start from these."
4. **The save row when the port already has a newer save**: ticked or not? docs/44 8.3 proposed off. Recommendation: off when the port's save is newer, on otherwise. There is a backup either way.
5. **Demon's Souls not in RPCS3's library**: offer "Choose the game folder" on that row, so the player's RPCS3 can still unlock a dump it does not list? The engine takes `run GAME --rpcs3 PATH` anyway. Recommendation: yes.

## 7. Test plan

Each test is file-only or a single RPCS3 call, and nothing boots the game unless the test says so.

- **Unlock tests with a real RPCS3** (T1-T6): the work folder is under `%LOCALAPPDATA%\DemonsSoulsRecomp\setup\`, never under %TEMP% (2.4). It holds a decrypted copy of the player's file afterwards, so the player deletes it when done.
- **Save tests** (T12 and any test that boots the game): the pinned regression save goes in first, and the player's save is restored afterwards.

**T1. One file by hand, RPCS3 closed** (I1, I3, I4, I9, I13). Single-line PowerShell 5.1. Replace `<RPCS3 folder>` with the folder that holds rpcs3.exe, and `<game folder>` with the one that holds `PS3_GAME`:

```powershell
$r='<RPCS3 folder>'; $g='<game folder>'; $w="$env:LOCALAPPDATA\DemonsSoulsRecomp\setup\unlock-test"; New-Item -ItemType Directory -Force "$w\in","$w\cfg" | Out-Null; Copy-Item "$g\PS3_GAME\USRDIR\EBOOT.BIN" "$w\in\EBOOT.BIN" -Force; $log="$r\log\RPCS3.log"; $t0=if (Test-Path $log) {(Get-Item $log).LastWriteTimeUtc} else {$null}; $env:RPCS3_CONFIG_DIR="$w\cfg\"; $env:QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES='1'; $p=Start-Process -FilePath "$r\rpcs3.exe" -ArgumentList "--decrypt `"$w\in\EBOOT.BIN`"" -Wait -PassThru; Remove-Item Env:RPCS3_CONFIG_DIR, Env:QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES; "exit code: $($p.ExitCode)"; "EBOOT.elf matches: $((Get-FileHash "$w\in\EBOOT.elf" -ErrorAction SilentlyContinue).Hash -eq '90EAA5E9E48B4874292BADC311CCEA253E6A622FF188BFBDBE76E6F6821269D8')"; "RPCS3 wrote here: $((Get-ChildItem "$w\cfg" -Name) -join ', ')"; "your RPCS3 log changed: $(if (Test-Path $log) {(Get-Item $log).LastWriteTimeUtc -ne $t0} else {'no log there'})"; "portable folder: $(Test-Path "$r\portable")"
```

What to check:
- `exit code: 0`.
- `EBOOT.elf matches: True`.
- "RPCS3 wrote here" lists `log`, `RPCS3.buf`, `config` and `dev_hdd0`, among others. That is I1: RPCS3 used setup's folder.
- `your RPCS3 log changed: False`.
- `portable folder: False`. If it says True, the log is in `<RPCS3 folder>\portable\log\` and the variable was ignored, as 2.5 expects.
- RPCS3's own lines (`Decrypting binaries...`, `Decrypted ... -> ...`) may show in the same window. That is RPCS3 attaching to this console, and it is expected.
- If it asks for a KLIC, press Ctrl+C, and note it: it means I3 or I7 is wrong.

For I13, run it again with a second RPCS3 build, if one is at hand.

**T2. The same, with the player's RPCS3 open** (I2): no "Another instance" dialog, the same results, and the open RPCS3 keeps working.

**T3. Script files** (I3, I10, I14).
- One `.sdat` by hand, single-line PowerShell 5.1, with the same two folders as T1:

```powershell
$r='<RPCS3 folder>'; $g='<game folder>'; $w="$env:LOCALAPPDATA\DemonsSoulsRecomp\setup\unlock-test"; New-Item -ItemType Directory -Force "$w\in","$w\cfg" | Out-Null; Copy-Item "$g\PS3_GAME\USRDIR\script\talk\m06.talkesdbnd.dcx.sdat" "$w\in\m06.talkesdbnd.dcx.sdat" -Force; $env:RPCS3_CONFIG_DIR="$w\cfg\"; $env:QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES='1'; $p=Start-Process -FilePath "$r\rpcs3.exe" -ArgumentList "--decrypt `"$w\in\m06.talkesdbnd.dcx.sdat`"" -Wait -PassThru; Remove-Item Env:RPCS3_CONFIG_DIR, Env:QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES; "exit code: $($p.ExitCode)"; "size: $((Get-Item "$w\in\m06.talkesdbnd.dcx.sdat.unedat" -ErrorAction SilentlyContinue).Length) (want 7584)"; "matches: $((Get-FileHash "$w\in\m06.talkesdbnd.dcx.sdat.unedat" -ErrorAction SilentlyContinue).Hash -eq '898F1D78C752617524F3FC5AD5417628C48BA049AB41E145B2C18ED42773E452')"
```

  What to check: `size: 7584 (want 7584)` and `matches: True`.
- Then all 27 files through the engine's unlock alone (`run --unlock-only` with `DES_SETUP_DATA` in a scratch folder): every sum matches, and note the time taken.

**T4. Portable and old-build behaviour, on a copy** (I12, I15).
- Copy RPCS3's program files into a scratch folder outside %TEMP%. Never add a `portable` folder to the player's own install.
- With an empty `portable\` in the copy, check that the logs go there, and that the engine reports `@rpcs3 shared` and finds the copy when it runs.

**T5. The hang and the watchdog** (I7, I8, I9).
- Run the engine's unlock with a fake input that is not a SELF, only in a scratch test build with the hash pre-check turned off.
- Expected: no window, the watchdog ends RPCS3 after 30 s without progress, and no rpcs3, conhost or OpenConsole process of this run is left. The player's processes are not touched.

**T6. An RPCS3 without `--decrypt`, if a build that old is at hand** (I11): exit 1, "Unknown option" in the step log, and the "too old" sentence.

**T7. Started from the host** (I5, I6, I8).
- The setup screens' unlock page with the real engine, once with conhost and once with Windows Terminal as the default terminal.
- No window appears, and `run.log` holds only protocol and engine lines.

**T8. Engine unit tests with a stand-in RPCS3** (`tests/fake_rpcs3.py`, like fake_engine.py). It writes outputs from fixture bytes, writes short files, hangs, exits 1 with "Unknown option", or ignores the redirect. Tests:
- the plan and `@file`/`@progress unlock` lines;
- the one-by-one retry naming the bad file;
- the watchdog;
- the cleanup of `setup/unlock/`;
- that a fake RPCS3 folder is byte-for-byte the same before and after;
- that a second run skips the unlock;
- the protocol version and `HostAgreesTest` for the new keywords and codes.

**T9. Reader and import unit tests**:
- a fixture RPCS3 tree with games.yml (folder, ISO, none), vfs.yml, `persistent_settings.dat` with two users, a binary PSF save made with `sfo.build`, a TROPUSR.DAT and the three YAML files;
- the `@user`, `@library`, `@found` and `@setting` lines;
- the save conversion, the backup, the swap;
- the busy and changed-during-copy stops;
- the one-time rule: a later run imports nothing.

**T10. Read-only proof on a real install** (I1). List `<R>` (path, size and modified time of every file), run a whole setup on the RPCS3 route, and list it again. The lists are equal.

**T11. Nothing written into the dump** (I21, prerequisite 1 and 2). After the `.sdat.dec` fix:
- Boot to the Nexus with `PS3_VFS_ROOT` on RPCS3's library folder, using the usual muted form: `tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1`.
- Compare a listing of the dump before and after: no new or changed file.
- The game's `[edat]` lines show no decryption.
- **Passed on the folder route (10-23).** This was a player's start through the launch hook, not `tools/des`. The host was built from `feat/game-folder-read-only`, with ps3recomp's `feat/edat-player-copies`, and used the DLL and staged inputs of a real setup:
  - The dump's listing was the same before and after 200 s of play in the Tutorial: 13,765 files and folders, each with its path, size and modified time.
  - No `[edat]` line, so nothing was decrypted, and `<data>/edat` was never made. The game opened five `.sdat` files (the title and common menus, c0000, enemycommon and the Tutorial's m08 script), and each opened as plaintext (`magic DCX`). With `PS3_EDAT_CACHE` set, the runtime never reuses the old `.dec` beside a source, so the plaintext came from setup's staged `.unedat` copies.
  - The game's HDD cache went to `<data>/hdd1`: 203 files, 64 MB.
  - The RPCS3 route's form of this test (its library folder as the VFS root) is still to run.

**T12. An imported save loads** (I13, I18, I19, I23; docs/44 R3).
- Import an RPCS3 save from a scratch copy of an RPCS3 user.
- Boot and continue: the character, place and items are as in RPCS3.
- Check the character count and the PPU Debug `exec.elf` hash, if the player has one.

**T13. Linux** (I22), once the Linux setup window exists: Flatpak and AppImage runs of T1's steps.

## 8. What is built (10-22)

The engine's unlock phase, branch `feat/setup-rpcs3-unlock`. No host screen draws it yet.

**Files.**
- `tools/setup/des_setup/rpcs3.py` (new): finding the program, the shared test, the player's folders (read only), PPU Debug's dumps, the image-path lookup, the redirect, the child process and the unlock itself.
- `pipeline.py`: the `unlock` phase (weight 1) between check and fetch; `run --unlock-only` plans `check unlock`; the check's RPCS3-route rules of 3.3; the cleanup after the stage.
- `gamefiles.py`: `unlocks` (the "your RPCS3 unlocks it next" sentences) and `also` (PPU Debug's `exec.elf`). A decrypted file found in several places now counts when any copy is good; before, only the first one found was checked.
- `protocol.py`: version 2, `@file`, `@rpcs3`, the `rpcs3` code.
- The host: `setup_protocol.cpp` and `.hpp` read the two lines into `Status::file`, `rpcs3State` and `rpcs3Path`; `setup_window.cpp` knows the `rpcs3` code (Try again) and the `unlock` task's words. Nothing more.
- `__main__.py`: `run --rpcs3 PATH [--unlock-only]` and `check --rpcs3 PATH`.
- Tests: `tests/fake_rpcs3.py`, `tests/test_unlock.py` (17 tests); `@setup 2` in `test_pipeline.py` and `fake_engine.py`.

**The command and its environment.** One process for all the files: `<rpcs3> --decrypt <data>\setup\unlock\in\PS3_GAME\USRDIR\EBOOT.BIN --decrypt <data>\setup\unlock\in\PS3_GAME\USRDIR\chr\c0000\c0000.esd.dcx.sdat ...`, absolute paths in table order, started in `<data>\setup\unlock\`. The environment is the engine's, plus:
- Windows: `RPCS3_CONFIG_DIR=<data>\setup\unlock\rpcs3\`, with the trailing backslash. Linux: `XDG_CONFIG_HOME=<data>/setup/unlock/rpcs3/config` and `XDG_CACHE_HOME=<data>/setup/unlock/rpcs3/cache`. Neither for a shared install.
- `QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1`.
- stdin is the null device. stdout and stderr go to `setup/unlock/rpcs3-output.txt`, which is copied into `setup/logs/unlock.log`. Nothing reaches the protocol's stdout.
- Windows: `CREATE_NO_WINDOW` (ignored for a GUI program), `STARTF_USESHOWWINDOW` with `SW_HIDE`, never `DETACHED_PROCESS`. RPCS3 goes into a job object of its own; the watchdog ends the job, so anything RPCS3 started ends too.
- Linux: a new session, so its own process group, which the watchdog ends with `killpg`. `PR_SET_PDEATHSIG` ends RPCS3 when the engine ends, since the host's Cancel ends only the engine's group.

**Decisions for the owner to confirm.**
- **Where the outputs wait.** They stay in `setup/unlock/in/` until the stage (3.4 step 10 said: into `inputs/` at once). The check after the unlock finds them there, the stage copies them into `inputs/`, then `setup/unlock/` goes. So after a `--unlock-only` run the decrypted outputs, and nothing else, wait in `setup/unlock/in/` for the build.
- **The journal.** Step `unlock`; its fingerprint is the sorted (path, SHA-256) list of the originals it unlocks; its outputs are those files in `setup/unlock/in/`. A build after the unlock page skips it. An output that changed in between fails the check after it, and the unlock runs again, once.
- **Portable installs: question 1 (a).** The check says `@rpcs3 shared <folder>`. The unlock refuses while a process runs from that program ("RPCS3 is open. Close it so setup can start it."), found by its image path: `QueryFullProcessImageNameW` on Windows, `/proc/<pid>/exe` on Linux. Otherwise it runs RPCS3 without the redirect. The page's "Close RPCS3 first" callout and its Unlock button (1.5) are the host's, not built.
- **An ignored redirect.** No `RPCS3.buf` in setup's folder after the first run: the step log says so, and the program goes into `setup/rpcs3_shared.txt`, so it is `shared` from then on. With no output either, the sentence is "RPCS3 is open. Close it so setup can start it."
- **The order of the sentences** after the first run: too old ("Unknown option" and a non-zero exit), could not start (no output, and exit status 3, 0xC0000409 or SIGABRT), RPCS3 is open (the redirect ignored, no output), stopped answering (the watchdog), could not write ("Failed to create", no output). Otherwise each file that is not right runs once more alone. Still missing: "RPCS3 could not unlock script/m03.luabnd.dcx.sdat." Still wrong: "script/m03.luabnd.dcx.sdat.unedat came out different from the known good version." There is no retry after the watchdog, since a hang would hang again.
- **A copy that is not what RPCS3 needs** (its sum, `SCE\0` or `NPD\0`, the SDAT flag, the plain size) stops before RPCS3 runs: `@error step unlock Copying the game files for RPCS3 failed.`
- **The check on the RPCS3 route** also looks in `<data>/inputs/` and at PPU Debug's `exec.elf`. 3.3's "the stage skips an item whose `found` path is its destination" is not needed: the stage copies to `.tmp` first.

**What the unit tests cover.** They use the stand-in, so they test the engine's side only.
- T8 in full: the plan; the `@rpcs3`, `@file` and `@progress unlock` lines in order (one test paces the stand-in); the redirect variables, the absolute paths in table order and the null stdin; the step log; a file that fails in the batch and works alone; a file that still fails, missing or wrong, named; the watchdog, with a 1 s stall, ending the stand-in and the helper it started; too old, could not start, could not write; the cleanup after the stage and after a failure; the RPCS3 folder and the dump byte for byte the same through check, unlock, stage and lift; the second run's skip; a changed output unlocked again; the portable case; the remembered ignored redirect; PPU Debug's `exec.elf`; RPCS3 gone; `@setup 2` and HostAgreesTest.
- Section 5's inferred items, the engine's half only: I6 (RPCS3's output never reaches the protocol), I7 on Linux (stdin is the null device, so a prompt ends), I8 (the job or the group ends the whole tree), I12 (a missing lock is noticed), I15 (a process is found by its image path, tested on a Python child).
- They have run on Windows only (99 tests pass). The Linux branches (the XDG redirect, `/proc`, `killpg`, `PR_SET_PDEATHSIG`) have not run.

**On a real RPCS3 (measured 10-22).** `run --rpcs3 <folder> --unlock-only` on the US 1.00 dump, with RPCS3 v0.0.43-20290 (an install of its own, RPCS3 closed), on a fresh data folder:
- The check said `@rpcs3 own <folder>` and took the EBOOT from PPU Debug's `exec.elf` (`@check ok PS3_GAME/USRDIR/EBOOT.elf exec.elf matches.`), so only the 26 SDAT files went to RPCS3.
- One process unlocked all 26 in about a second, exit status 0. Every output matched the table's size and SHA-256 (the step log names each one).
- RPCS3 printed `Log file open failed: <data>/setup/unlock/rpcs3/log/RPCS3.log (error 0)` twice: it writes no log inside setup's folder. The unlock is not affected; the step log keeps RPCS3's output.
- The player's RPCS3 folder had no new or changed files afterwards.
- A second run skipped the step (`@step unlock skip`, from the journal).
- Not run yet: RPCS3 open (T2), a portable install, the watchdog on a real hang, a whole setup through the stage and the build on this route, Linux.

**What remained on 10-22** (section 9 has what is left now).
- The host's screens: the RPCS3 card and pages (1.2 to 1.7), finding RPCS3 (3.6), `rpcs3_dir` in settings.ini.
- The library and import half: `@user`, `@library`, `@found`, `@setting`; games.yml, so that `--rpcs3` alone finds the game; the import phase (3.5) with `--import`, `--no-import` and `--gamedata`.
- `flatpak:` and the AppImage's FUSE retry (3.7).
- Before this ships: the ps3recomp fix that stops `.sdat.dec` files beside the game's files (section 4, item 1), then items 2 to 4.
- The rest of the real-RPCS3 tests: T2 to T7 through the engine, then T10 to T13 (T1 and T3 ran as the unlock above).

## 9. What is built (10-23)

Branch `feat/setup-rpcs3-route`: the library and import half of the engine, the firmware from RPCS3, and the host's pages. With it the route works from the card to Ready to play. Section 8's unlock was already on master.

### 9.1 The engine: RPCS3's library, the import, the firmware

**Files.**
- `rpcs3_library.py` (new) reads the player's RPCS3, and only reads (3.1):
  - its folders, from `vfs.yml` or RPCS3's defaults;
  - the game in `games.yml`: a folder, an ISO file, or the Store version (`NPUB30910`, not supported);
  - the users, and the one RPCS3 uses (`persistent_settings.dat`);
  - the save, the trophies (`TROPUSR.DAT`) and the settings and patches;
  - `dev_flash/sys/external/libsre.sprx`.
- `rpcs3_import.py` (new) is the import phase (3.5). It writes `<data>/setup/imported.txt` for the done page and `rpcs3_settings.txt` for the options page.
- `yaml_subset.py` (new) reads the YAML subset RPCS3 writes. A line it cannot read is skipped, never guessed at.
- `firmware.py` (new): see 9.2.
- `pipeline.py`:
  - The plan is now `check unlock fetch stage lift compile import shaders`.
  - `run --rpcs3` without GAME takes the game from RPCS3's library.
  - The unlock also makes `libsre.prx` from RPCS3's system software.
  - `--import save,trophies,settings` stays pending in `request.json` until it is done; `--no-import` drops it.
  - The save goes into `--gamedata`, else the `gamedata` folder beside `--host`. The host passes `<exe folder>/gamedata`.
- `protocol.py`:
  - New lines: `@user <id> <users> <name|->`, `@library <folder|iso|store|none> <path|->`, `@found save|current-save|trophies|settings ...` and `@setting KEY value`.
  - The `import` code, with the step (`import-save`, `import-trophies`, `import-settings`) in place of the phase.
  - They come after `@rpcs3` and before the `@check` lines.
- `gamefiles.py`: the firmware's item can say "your RPCS3 unlocks it next", like the 27 decrypted files.
- Tests: `test_rpcs3_route.py` and `test_firmware.py` (new). All 171 engine tests pass on Windows.

**Settings brought across.** Frame rate, resolution scale, motion blur off, intro skip and anisotropic filtering (question 2: the four decided, plus anisotropic filtering). Only values that differ from the port's own defaults count.

**V-sync is not brought across**, although question 2 recommended it. RPCS3's default is off and few players change it, so for nearly everyone it would have turned the port's V-sync off.

### 9.2 libsre.prx from another PS3 system software version (measured 10-23)

The first real unlock failed on `libsre.prx`: "came out different from the known good version". This PC's RPCS3 has system software 4.89. The table's sum is for 4.86's copy, the one the port is built and tested with.

**What differs.** A byte compare of the two decrypted files:
- libsre writes the system software's version into its code as a constant built by `lis rX,0x0048` then `ori rX,rX,0x6000` (4.86) or `0x9000` (4.89).
- There are 86 such places.
- Apart from them, the two files are the same, byte for byte.

**The fix** is `firmware.py`, used by the check, the unlock's output check and the stage:
- It finds the version's places: each `ori` whose `lis` comes shortly before it, with a value from 4.00 to 4.99. The most common value, seen at least 8 times, is the file's version.
- It sets each place back to the table's version (game_table.json's new `"version": "0x00486000"`).
- Then the whole file must match the table's SHA-256. A copy that differs anywhere else still fails, so this cannot let a wrong file through.
- The check says: "libsre.prx matches. It is from PS3 system software 4.89; setup sets its version to 4.86, the one the port is tested with."
- The stage writes the table's bytes into `inputs/`. The player's file and RPCS3's are not changed.
- It applies only to the firmware's item. A decrypted game file is never changed this way.

Only 4.86 and 4.89 have been measured. Inferred: other 4.x versions differ the same way, and if one does not, it fails the sum as before.

### 9.3 The host's pages


**Files.**
- `setup_rpcs3.cpp` and `.hpp` (new): finding RPCS3 before any Python exists (3.6), and reading the BLUS30443 line of its game list.
  - Windows looks, in this order, at: `RPCS3_DIR`, settings.ini's `rpcs3_dir`, the targets of shortcuts (Desktop, the Start menu's Programs, the taskbar's pinned ones), a running rpcs3.exe, Windows' record of programs started (the shell's MuiCache and the Program Compatibility Assistant's store, read only), then two folders deep into Desktop, Documents, Downloads, Scoop's RPCS3 and each fixed drive.
  - Linux looks at `rpcs3` on `PATH` and at AppImages in Applications, Downloads, Desktop and `.local/bin`. Flatpak is not offered, because the engine has no `flatpak:` route yet.
  - A folder found by a search counts only when RPCS3 has run there (`config/`, `portable/` or `dev_hdd0/`). The first RPCS3 with the game in its list wins, else the first one found.
  - **Measured on this PC:** the RPCS3 here has no shortcut and is outside the folders searched (Documents is on OneDrive). Only Windows' record found it.
- `setup_rpcs3_window.cpp` and `.hpp` (new): "Your game in RPCS3" (1.3), "RPCS3 was not found" (1.4) and "Unlocking the game files" (1.5).
  - The RPCS3 page runs `run --check-only --rpcs3` and draws these rows from its lines: RPCS3 with Change, the user (two or more users), and the game. The game row covers library, ISO, store, not in the library (with "Choose the game folder", question 5), another copy, or a file that does not match.
  - "Bring across from RPCS3" has three tick rows, each set from its `@found` state. The save starts unticked when the port's own save is newer (question 4).
  - Continue opens the unlock page while any file is still to unlock, else Ready to build. The RPCS3 is remembered in settings.ini's `rpcs3_dir`.
  - The unlock page runs `run --unlock-only` and shows "File n of m", the file and the bar, then the done row.
  - For a portable RPCS3 the unlock page first shows "Close RPCS3 first" with an Unlock button. On a failure it shows Try again, Show me how, Details and Back.
- `setup_screens.cpp` changes:
  - "Where is your game?" gets the RPCS3 card first. The search runs off the UI's thread; the card shows "Looking..." until it answers, and a press before that opens the page when it does.
  - The help page gets "With RPCS3 (easiest)" and an "Import from RPCS3" button.
  - Ready to build takes a line for the game ("From RPCS3's library. ...") and a "From RPCS3" row.
  - The welcome's first row now says RPCS3 is enough.
- `setup_window.cpp` changes:
  - The `import` task and the `import` code, with "Go on without it" (`run --no-import`). The `rpcs3` code gets "Show me how".
  - The done page shows the save and trophy rows from `<data>/setup/imported.txt` (1.9).
- `setup_options_window.cpp` changes:
  - When `<data>/setup/rpcs3_settings.txt` exists and no preset is stored yet, Custom is picked first (question 3), with "Starts from your RPCS3 settings." and the imported values.
  - "A few choices" starts from them and says "Set from your RPCS3 settings."
  - Finish with Custom also stores motion blur and the intro skip, then deletes the file. Original and Enhanced ignore it.
- `setup_engine.cpp`: `Request` gains `rpcs3`, `imports` and `noImport`. The host passes `--rpcs3`, `--gamedata <exe folder>/gamedata`, `--import`, `--no-import` and `--unlock-only`.
- `setup_protocol.cpp`: `@user`, `@library`, `@found` and `@setting` go into `Status`.
- `setup.rcss`: the tick rows (`button.setup-check`, `icon.setup-box`).

**Seen.** The pages were shot with a stand-in engine that prints these lines, and the card with the real search. The shots are not committed. They show:
- the card, finding this PC's RPCS3 with the game in its library;
- the RPCS3 page with two users and the three tick rows (one unticked by the pad);
- the unlock page at "File 9 of 28", and done;
- Ready to build with the "From RPCS3" row.
- the ISO case ("Not yet", with Check again), the unlock failure (Try again, Show me how, Details, Back) and the import failure ("The import from RPCS3 stopped", with Go on without it).

### 9.4 On the real RPCS3 and the real game (measured 10-23)

These ran on this PC's RPCS3 data with the engine, not through the screens.
- **The card** found this PC's RPCS3 through Windows' record only, and said the game is in its library.
- **The check** (`run --check-only --rpcs3`) said:
  - `@rpcs3 own`, `@user 00000001 1`, and `@library folder` with the dump;
  - `@found save ok`, `@found current-save none`, `@found trophies ok 18/38`;
  - `@found settings ok 4`: frame rate 60, motion blur off, intro skip on, anisotropic filtering 16.
- **The unlock** used a copy of the player's RPCS3: its program, with `games.yml`, `libsre.sprx`, the save, the trophies and the settings copied in. On a fresh data folder:
  - one RPCS3 process unlocked 28 files: EBOOT.BIN, the 26 SDAT files and libsre.prx;
  - the first run failed on libsre.prx (9.2); after the fix all 28 were checked, exit status 0.
- **The import** brought across, into a scratch `gamedata` folder:
  - the save: 14 files, with the port's text PARAM.SFO and PARAM.PFD;
  - the trophies: 18 of 38;
  - the 4 settings.
- **The game loaded that save.** A test package (master with the decrypted-folder fix) started with the imported save in its `gamedata`, scripted with `DES_AUTO_CONTINUE`:
  - the save loaded (`LOAD complete`);
  - the RPCS3 character was in Boletarian Palace with its souls, spells and items;
  - the game autosaved into it.
- **Nothing of the player's changed.** Every file and folder in the dump (13,765) and in the player's RPCS3 folder (16,204) was listed before the unlock and after the boot, by path, size and time. There were no differences.
- **A small fix from the run:** the import left an empty `gamedata/import/` folder behind. It now goes once the save is in, and after a failure.

Not run yet:
- the whole route through the screens with the real engine and a full build (the owner's test of the next package);
- RPCS3 open during the unlock (T2), a portable install, two RPCS3 users, an ISO in the library, Linux.

### 9.5 The open questions (section 6), as built

The recommendations, except where noted:
1. Portable installs: (a), from section 8.
2. Settings: the four, plus anisotropic filtering. Not V-sync (9.1).
3. The options page starts on Custom after a settings import.
4. The save row starts unticked when the port's own save is newer. There is a backup either way.
5. Not in the library: the row offers "Choose the game folder".

### 9.6 What remains

- The owner's test of the whole route on a fresh package.
- An ISO in RPCS3's library: the row says "Not yet".
- `flatpak:` and the AppImage's FUSE retry (3.7). The Linux setup window comes first (docs/88 section 8).
- Section 4: dropping the runtime's SDATA decryption from release builds.
- Telling the player the old `cache/` folder in their dump can go (section 4, item 2).
- Tests T2, T5 to T7 and T13 on real installs.
