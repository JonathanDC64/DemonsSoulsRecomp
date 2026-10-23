# Player setup: design (2026-10-18)

Design only. Nothing was built, lifted or run for it. What was done: a few probe compiles of 2 to 6 lines each (no game code, through the build-slot lock), file sizes and pattern counts on existing trees, a read-only look at the existing lift (`work/recomp_l2`), the sibling FFCCEOT-Recomp checkout, and web pages. Nothing was downloaded or installed.

Three labels, as in docs/45:
- **(read)**: seen in code, a doc or a web page.
- **(measured)**: counted, sized or compiled on this PC.
- **(inferred)**: my conclusion, not checked. Section 13 lists the trial builds that would check the big ones.

This builds on docs/45 (launcher, updates and a no-install patcher). Two docs were written at the same time: docs/51 (decryption and its legal side) and docs/53 (build times and RAM per job). Decryption is a box here (section 3). It follows docs/51 as it stood at 19:11 that day, while its lane was still running. No build-time numbers are taken beyond docs/45's.

The player goal, from the fan-out rules: the player downloads our release, points it at their own game dump and, only if it can't be avoided, the PS3 firmware file. Everything the build needs ships in the download. The player installs nothing: no Visual Studio, Windows SDK, Python, CMake, Git or RPCS3. Nothing is fetched during setup. Each PC precompiles its own shader cache before the first launch.

One part of that goal does not hold under docs/51's recommendation (Plan L). The player decrypts `EBOOT.BIN` and the 26 `.sdat` files once, with a tool they already have (today RPCS3), because our download carries no keys. Only Plan P, which needs a lawyer's sign-off, removes that step. Everything else in the goal holds, and docs/51's libsre HLE removes the firmware.

## Answer first

**Recommended route: build the host in CI, build only the game on the player's PC (Route B, docs/45's options 1 and 2).**
- CI builds `des_host.exe` with today's toolchain: clang-cl with the MSVC STL, the MSVC CRT and the Windows SDK. It links the static CRT (`/MT`), so players need no Visual C++ runtime. Microsoft documents static linking as one of its three ways to deploy its runtime (read).
- The player's PC builds `des_game.dll` from the lift. It is freestanding C and C++: no CRT, no Windows SDK, no C++ library. The bundled compiler is the official LLVM release cut down to `clang.exe`, `lld-link.exe` and clang's own headers. That is 192 MB on disk and 68.8 MB zipped (measured here and in docs/45). Both exes import only Windows system DLLs, so they run on a clean PC (measured).
- The embeddable Python package (10.9 MB zipped, docs/45) runs the lifters. There is no CMake or Ninja on the player's PC. The host starts clang and lld-link itself, as FFCCEOT-Recomp's host does (read).
- The setup screen runs in the RmlUi host: the game folder and the decrypted files (docs/51), checks, lift, compile, link, verify, shaders, play. Each step shows progress, stops with a plain message when it fails, and resumes where it stopped.

**Why not llvm-mingw (Route A, or A' with CI-built libraries).** It is possible. Every header and import library we use exists in mingw-w64 (read, section 7.6). But:
- C++ objects built for MinGW (Itanium C++ ABI, libc++) cannot link with objects built against MSVC's STL (inferred, standard). So the whole host (runtime, libs, port, UI, RmlUi, FreeType) has to move to MinGW, not just the game.
- On the MinGW target some MSVC spellings stop working (measured). Without `-fms-extensions`, `__declspec(thread)` and `__declspec(align)` are ignored with only a warning, `__declspec(allocate)` is ignored, `#pragma comment` is dropped, and `__try` is an error. The runtime and libs have 10 `__declspec(thread)` sites in `_WIN32` branches; one is the PPU thread-exit `jmp_buf`. Even with the flag, the crash hook's `.CRT$XCV` entry runs before C++ static constructors, not after (measured and read).
- The download grows by about 30 to 40 MB zipped and the install by about 200 MB (inferred from package sizes, section 10).
- Every host update needs a relink on the player's PC, so about 1 to 1.4 GB of objects must be kept, or the game must be rebuilt.

Keep A' as the fallback. docs/45's milestone 1 (the DLL split with today's toolchain, 4 to 7 days) is the go/no-go for Route B.

**Work estimate (inferred; days for one engineer).**

| Part | Route B (recommended) | Route A' (llvm-mingw) |
|---|---|---|
| Route-specific | 8.5 to 14.5: docs/45 6.1 steps 1 to 4 and 13 (the split and its frame-rate A/B, the freestanding module, the `/MT` host, identical DLLs) | 8.5 to 16: MinGW port of runtime, libs, port and UI; a MinGW CI build; a separate launcher exe; relinking on updates; identical exes (section 8.2). Then two toolchains to keep working. |
| Common to both | 19 to 29: setup screens, the pipeline script, the decrypted-files step, the compile driver with resume, the shaders step, packaging, release CI, clean-PC tests, docs, signing (docs/45 6.1 steps 5 to 12 and 14 to 16, plus sections 2 to 5 here) | 19 to 29 |
| Total | about 28 to 44 days (6 to 9 weeks) | about 28 to 45 days, plus the second toolchain from then on |

The two routes cost about the same to build. Route B wins on what comes after: one toolchain, a smaller download, host updates with no rebuild, and fewer unknowns.

Not included:
- docs/51's work: the libsre HLE (9 to 15 days), the runtime reading pre-decrypted SDATA, and keys compiled out of releases.
- The PSO prediction work (docs/49 steps 1 to 3, 6.5 to 8.5 days). Without it, the shaders step does today's prework only.

**Decisions for the user.**
1. Route B (recommended) or Route A' (section 8).
2. Which LLVM to pin and ship. This PC has 22.1.8 (measured), and FFCCEOT-Recomp ships 22.1.8 (read). llvm-mingw 20261006 and MSYS2 already use 23.1.3 (read). Recommendation: 22.1.8, packaged from the existing install, so no download is needed.
3. The embeddable Python version: 3.13.9 as docs/45 sized it, or a 3.14 release.
4. The Visual C++ runtime in the host: static `/MT` (recommended) or app-local DLLs.
5. After setup, delete the objects (default, about 0.6 to 0.7 GB left in use) or keep them as a rebuild cache (about 1 to 1.4 GB more).
6. Run the shader step in setup, before the first launch, on all threads (recommended, and what was asked), or leave it to the game's first start as today.
7. The decryption choices belong to docs/51 (its section 7): Plan L or Plan P; under Plan L, (c) files the player decrypted, or (c+) setup also runs the player's RPCS3; and the libsre HLE. The box in section 3 fits each choice, so the setup UI can start before they are made.
8. The LGPL ATRAC decoder in the host: the same question as docs/51's decision 8 (section 11 here).

**Trial builds that need a toolchain that is not installed, and downloads that need your OK first.** Details in section 13.
- None is needed for Route B. Its compiler is the installed LLVM 22.1.8. T2 (a freestanding compile of one lifted chunk) needs only a build slot.
- For Route A' only: the llvm-mingw 20261006 `ucrt-x86_64` zip (190,836,485 bytes, read) for trial T1, a MinGW build of runtime, libs, port, UI, RmlUi and FreeType. **Please OK this download before anyone runs T1.**
- Optional: the Python 3.13.9 embeddable zip (10,926,675 bytes, read) for trial T4, the lifters' worker processes under the embeddable Python. **Also needs your OK.**

## 1. What the player sees

### 1.1 FFCCEOT-Recomp's first-run flow (read)

Files: `game/src/setup.hpp`, `game/src/setup.cpp`, `game/src/ui/rom_setup.cpp`, `game/src/ui/prelaunch.cpp`, `docs/PACKAGING.md`.
- **States.** `setup::check(data_dir)` reads only the manifest and returns `Ready`, `Missing`, `Outdated` (built by another version) or `Damaged` (files the manifest names are gone).
- **Worker.** `setup::run(request, callbacks)` runs on a thread. `Callbacks` holds `progress(fraction, step)`, `log(line)` and a `cancel` flag.
- **Inputs.** `verify_rom` checks the file. `expected_roms` lists the dumps this version supports. `find_roms` looks for likely dumps in the usual folders by file header. `find_toolchain` takes the bundled clang, or one on PATH during development.
- **Steps shown.** "Checking your ROM", "Copying your ROM", "Translating the game", "Analysing the game", "Writing the game's code", the compile, "Linking the game", "Finishing", "Done".
- **Keeps a copy of the input.** A rebuild after an update needs no ROM from the player.
- **Build key.** `generator_id` covers "abi N rev M" and the configuration files. A changed key means `Outdated`.
- **On failure.** It deletes the work folder and keeps the ROM copy. A retry starts over. Its build takes 11 to 17 s (read, PACKAGING.md), so it needs no resume.
- **Start screen.** The main button says "Select ROM" until setup is done, then "Play". A status line says "ROM ready.", "Needs setting up again.", "Game files missing." or "No ROM selected.". An update banner says "Version X is available", with a Download button.
- **Modals.** "Checking your ROM". "Setting up the game" or "Setting up again", with a progress bar and Cancel ("Cancelling..."). "This is not the right ROM", with the expected and actual hashes and "Choose another". "Setup did not finish", with the error, the log, Report (opens a prefilled issue) and Retry.

### 1.2 Our flow

Same shape, with three differences. The inputs are a game folder and the decrypted files that docs/51 asks for. The build takes 11 minutes to 2 hours, not 15 seconds (docs/45), so it needs resume, an ETA and a stay-awake request. A shaders step comes last.

It runs in `des_host.exe` with the RmlUi UI on its own D3D12 device, as the crash dialog does (docs/32, docs/45). The existing pieces fit: `prelaunch`, `modal`, `prework_modal`, `report`, `settings_window` (read, file list on origin/master).

1. **Start screen.** A status line and one main button:
   - Missing: "Set up the game".
   - Interrupted: "Resume setup" (with "Start over" beside it).
   - Outdated: "Set up again". The kept inputs are used, so the player is asked for nothing.
   - Damaged: "Repair". Only the missing or changed files are rebuilt.
   - Ready: "Play".
2. **Inputs.**
   - Game folder. A picker, plus a scan of likely places (Downloads, Documents, Desktop, other drives' roots) for a `PS3_GAME\PARAM.SFO`, like FFCCEOT's `find_roms`. It shows what it found: "Demon's Souls, BLUS30443, version 01.00".
   - Decrypted files (docs/51 Plan L). Setup looks for `EBOOT.elf` and the 26 `*.sdat.unedat` files beside the originals, which is where RPCS3 writes them (docs/51). A second picker takes another folder. A checklist shows each file as found and checked, or missing. A "How do I make these?" link opens the player guide.
   - Under (c+), a "Use my RPCS3" button: pick `rpcs3.exe`, and setup decrypts temporary copies with it (docs/51; docs/45 4.1 steps 3 and 4).
   - The firmware's `libsre.prx`, only while libsre stays LLE. docs/51's HLE removes this picker.
3. **Before starting.** "Setup needs about N GB free on C: and takes about T on this PC." T comes from the core count and free RAM (docs/45 3.1, refined by docs/53). Laptops on battery get "Plug in first". Then Start.
4. **Progress.** A step list with ticks, the current step's own bar, an overall bar and an ETA. A line says the PC is kept awake. Cancel. "Details" shows the log tail.
5. **Shaders.** The same screen, as the last step (section 5).
6. **Done.** "Play".

Failure modal: one plain sentence on what failed, one on what to do, then Retry, Report and Close. An input mismatch shows the expected and actual hashes side by side, as FFCCEOT does.

### 1.3 States

| State | When | Start screen |
|---|---|---|
| Ready | the manifest matches the lift key, and the DLL's size and hash are right | Play |
| Missing | no build folder for this lift key, and no kept inputs | Set up the game |
| Outdated | an app update changed the lift key (docs/45 4.2) | Set up again (kept inputs; nothing to pick) |
| Damaged | the manifest names a file that is gone or has changed (for example, removed by antivirus) | Repair |
| Interrupted (new) | the journal says a step was running, and no setup is running now (section 4.2) | Resume setup / Start over |
| Game folder missing (play time) | the game folder moved. Setup copies only the decrypted files, so the game reads all other files from the dump at run time (inferred from docs/51 5.3). | asks for the folder; no rebuild |

### 1.4 Messages

| Failure | What the player reads | Action |
|---|---|---|
| Folder without `PS3_GAME\USRDIR\EBOOT.BIN` | "This folder has no PS3_GAME\USRDIR\EBOOT.BIN. Pick the folder that contains PS3_GAME." | pick again |
| Another region or version | "This is BLES00932 (Europe). This version supports BLUS30443 version 01.00 only." (docs/45: other regions are out) | pick again |
| Store version, demo or encrypted disc image (refused in docs/51 5.1 and 5.2) | "This is the PlayStation Store version (NPUB30910). Only the disc version, BLUS30443, is supported." Likewise for the demo, and for an encrypted image: "Use a folder dump of the disc." | pick again |
| Encrypted `EBOOT.BIN` with another hash | "This EBOOT.BIN is not one this version knows." and the expected and actual SHA-256 | Report, pick again |
| Decrypted files missing | "Found EBOOT.elf and 22 of the 26 decrypted .sdat files. Missing: ..." | open the guide, Check again |
| A decrypted file does not match | "m01.luabnd.dcx.sdat.unedat does not match. Decrypt it again from the original .sdat." and both hashes | Check again |
| Not enough disk | "Setup needs 4.0 GB free on C:. 1.2 GB is free." | free space, Resume |
| A tool is missing or blocked | "clang.exe is missing from the tools folder. Antivirus may have removed it. Extract the download again." | Retry |
| Out of memory in the compiler | "The compiler ran out of memory. Setup will use fewer jobs." | automatic retry, once |
| The compiler failed | "Building the game failed on ppu_recomp_007.cpp." plus the last 1,500 characters (as FFCCEOT) | Report, Retry |
| DLL hash differs from the published one | "This build differs from the reference build." (docs/45 4.1 step 9) | Rebuild, Play anyway |
| No D3D12 device in the shaders step | "Shaders could not be prepared. The game will compile them while it runs." | Skip |

## 2. Step map: README steps by hand, setup steps in the launcher

The README steps today (read): clone two repos, decrypt `EBOOT.BIN` with RPCS3, `ppu_loader`, `ppu_lifter` with the override list and `insn_hooks.txt`, `gen_hle_nids --all`, `apply_boot_patches`, `build_spu.py`, `build_libsre.sh`, CMake and Ninja with clang-cl, then the shader prework at the first start. The table maps each one.

| # | Today, by hand (read) | Setup step | Runs where | Progress unit | Resume point | What can go wrong |
|---|---|---|---|---|---|---|
| 0 | Clone this repo and the ps3recomp fork; install VS, LLVM, CMake, Ninja, Python, Git Bash | Unzip the release | CI builds it | none | none | antivirus quarantine: a hash check of the tools at each start |
| 1 | Decrypt `PS3_GAME/USRDIR/EBOOT.BIN` with RPCS3 to `work/game/EBOOT.ELF`. The runtime decrypts the 26 `.sdat` files itself at run time (docs/51 1.1). | **Game files**: the box in section 3. Under Plan L, find the player's decrypted files and copy them to `inputs\` | player | per file | per file, by output hash | missing or wrong files: checklist |
| 1b | "Your own decrypted libsre.prx from your PS3 firmware" | the same box, only while libsre stays LLE | player | per file | per file | as row 1 |
| 2 | (none) | **Check**: `PARAM.SFO` (BLUS30443, 01.00, disc), the encrypted `EBOOT.BIN`'s SHA-256 before anything else, then each decrypted file's SHA-256 against the release's table (docs/51 5.3) | player | one step | redone (seconds) | mismatch modal |
| 3 | `ppu_loader.py EBOOT.ELF -o work/loader` | **Lift 1, analyse** | player (bundled Python) | one step | redone (seconds) | internal error: Report |
| 4 | `ppu_lifter.py ... --override-funcs ... --insn-hooks @port/insn_hooks.txt -j 16` | **Lift 2, PPU code** | player | functions written, from the lifter's progress lines | whole step: about 40 s on 16 workers (docs/45) | out of memory: fewer workers |
| 5 | `gen_hle_nids.py --all` | none: built into the host | CI | none | none | none (not game-derived, docs/45) |
| 6 | `apply_boot_patches.py work/recomp` | **Lift 3, boot patches** | player | patches applied | whole step, from the unpatched copy | a patch that does not apply means the app and the lift disagree: Report |
| 7 | `extract_spu_images.py`, and SPU job blobs "cut from the EBOOT at the descriptor's eaBinary" by hand (read, `build_spu.py`) | **Lift 4, SPU programs**: cut the 12 images and 2 jobs with a table of offsets, sizes and fingerprints shipped in the release | player | per image | per image | a fingerprint mismatch: Report |
| 8 | `build_spu.py` (runs `spu_lifter.py` with the native hooks and blocks) | **Lift 5, SPU code** | player | per image | per image | as row 4 |
| 9 | `build_libsre.sh <libsre.prx>`: `lift_prx.py`, `ppu_lifter.py --raw --toc 0x30039AB0`, `gen_libsre_exports.py` | **Lift 6, libsre**, only while libsre stays LLE: the same three tools from the Python pipeline, with the TOC read from `lift_prx`'s output instead of the constant. docs/51's HLE removes this row. | player | sub-steps | whole step | another firmware's libsre (docs/45 risk): check its hash first |
| 10 | `cmake --build ps3recomp/build --target ps3recomp_runtime rsx_shader_precompile` | none: inside the host | CI | none | none | none |
| 11 | `cmake --build port/build --target des_boot --parallel 4` | **Compile**: 14 PPU chunks, 14 SPU objects, the workload table, and libsre with its glue while it stays LLE | player (bundled clang) | source bytes compiled | per object, by its hash file (docs/45 4.3) | out of memory, disk full, a compiler crash |
| 12 | (link is part of row 11) | **Link** `des_game.dll` with lld-link | player | one step | redone (1 to 4 s, docs/45) | disk full |
| 13 | (none) | **Verify** against the published hash for this flag set (docs/45 4.5) | player | one step | redone | mismatch: Rebuild or Play anyway |
| 14 | First start: the shader prework behind a progress bar (README, docs/32) | **Shaders**: prework, PSO prediction, pipeline library (section 5) | player | shaders, then pipelines per tier | per pass, and per tier | no D3D12 device: Skip |
| 15 | `tools/des/play.sh` with knobs from `deslib.sh` | **Play**: the run defaults live in the exe (docs/45 6.1 step 5) | player | none | none | none |

Notes:
- **One pipeline script.** Rows 3 to 9 become one entry point, `scripts/des_setup.py` (docs/45 6.1 step 6). It prints progress lines in a fixed form, for example `@progress lift-ppu 1200/94017`. The host parses them, and logs every line to `setup.log` (inferred design).
- **No bash.** `build_libsre.sh` is bash, and Git Bash is not on players' PCs. While libsre stays LLE, its three Python calls and its file rename move into the pipeline script (read: the script is 3 Python calls and a `mv`).
- **The 26 decrypted `.sdat` files are not build inputs.** Only the runtime reads them. Setup checks them and copies them into `inputs\`, and the runtime reads them from there (docs/51 6, point 3).
- **Boot patches on a copy.** The lift writes `src/raw/`, and the patches write `src/`. A rerun then never patches twice (inferred design).
- **The job blobs need a table.** Their offsets and sizes in the EBOOT, plus the FNV fingerprint `build_spu.py` already uses, ship in the release. Addresses and hashes are not game code; the repo already ships addresses in `port/insn_hooks.txt` (docs/45).
- **The CPU check.** The lift is compiled with `-msse4.1` today (README, `port/CMakeLists.txt`). Setup checks for SSE4.1 with CPUID first. Windows 11 needs SSE4.2 anyway (inferred).

## 3. The decryption box (details in docs/51)

docs/51 decides how the files get decrypted and what is legally safe. Setup needs only a fixed contract, so the UI can be built before those decisions.

**The contract.**
- **In:** the game folder (`PS3_GAME/PARAM.SFO`, `PS3_GAME/USRDIR/EBOOT.BIN` and the 26 `.sdat` files), plus whatever the chosen plan needs (below).
- **Out, into `inputs\` in the local data folder (docs/45 4.4):**
  - `EBOOT.ELF`.
  - The 26 decrypted `.sdat` files, under their `USRDIR` paths.
  - `libsre.prx`, only while libsre stays LLE.
- **Checks** (docs/51 5.3):
  - `PARAM.SFO` first.
  - Then the encrypted `EBOOT.BIN`'s SHA-256, so the player learns "right game, right version" before decrypting anything.
  - Then every output's SHA-256 against the release's table. docs/51 appendix A has the `EBOOT.ELF` and `libsre.prx` hashes. The 26 decrypted `.sdat` hashes are still to be measured (docs/51 section 10).
- **Never write into the player's dump** (docs/45, docs/51). Today the runtime writes `.sdat.dec` files there (docs/51 1.1). That changes with docs/51's runtime work.
- **Keep the outputs for as long as the game is installed.** The runtime needs `EBOOT.ELF` (and the libsre image while LLE) at run time too (docs/51 4). Kept outputs also mean that a rebuild after an app update needs nothing from the player, as FFCCEOT keeps its ROM copy (read). Size: `EBOOT.ELF` 28.2 MB (measured); the `.sdat` set about 1.4 MB (inferred from the 1,411,472 encrypted bytes, docs/51); `libsre.prx` 239,344 bytes (docs/51).

**How each docs/51 plan fills the box.**

| Plan (docs/51 6) | What the player does | What setup does | UI |
|---|---|---|---|
| L with (c), recommended in docs/51 | decrypts `EBOOT.BIN` and the 26 `.sdat` files once with a tool they have (today RPCS3: Utilities > Decrypt PS3 Binaries) | finds `EBOOT.elf` and `*.sdat.unedat` beside the originals or in a picked folder; checks; copies | a checklist of 27 files (28 while libsre stays LLE), a guide link, Check again |
| L with (c+), a docs/51 user decision | has RPCS3 installed | copies the files to a temporary folder, runs `rpcs3.exe --decrypt` with standard input closed, collects the outputs by name, checks, copies, deletes the temporary copies (docs/51, docs/45 4.1 step 4) | "Use my RPCS3", then a bar per file |
| P, only with a lawyer's sign-off | nothing | decrypts only files whose SHA-256 is in our table | one bar, "Reading the game files" |

Under every plan, setup also accepts files the player already decrypted.

**Firmware.** Under docs/51's recommendation the firmware goes away (its option (d), our own HLE for the eight libsre functions). Until then, setup takes a decrypted `libsre.prx` through the same box, and rows 1b and 9 of section 2 stay.

### 3.1 What the player sees under Plan L (c)

1. Pick the game folder. Setup checks `PARAM.SFO` and the encrypted `EBOOT.BIN`.
2. The checklist shows `EBOOT.elf` and the 26 `.sdat.unedat` files as found, missing or wrong.
3. Missing files: "Decrypt these with a tool you have, then press Check again". The guide shows how. Per docs/51 section 8, question 2, it may name RPCS3 without a link.
4. All found and checked: setup copies them and the build starts.

## 4. The build engine: progress, errors, resume

### 4.1 Where it runs

- A worker thread in the host runs the steps. Python and clang run as child processes.
- All children go into one Windows job object with kill-on-close. If the host crashes or is closed, no compiler keeps running (inferred, standard Windows API).
- Children run below normal priority, and the host holds a power request while building (docs/45 4.3).
- Paths are passed as wide strings. The game folder and the user folder may hold non-ASCII names (docs/45 risk).

### 4.2 The journal, and how resume works

`build\<lift key>\setup.json` records, per step: its state (pending, running, done, failed), the hashes of its inputs, its outputs (path, size, hash), the tool versions and the time taken. It is written to a temp file, flushed and renamed after every change, so a crash or power cut leaves the old or the new file, never a half one (inferred).

| Step | Unit of resume | After a restart |
|---|---|---|
| Game files | per file | skip a file whose output exists and matches the table |
| Lift 1 to 3 and 6 | whole step (seconds to a minute) | rerun the step if any output is missing or changed |
| Lift 4 and 5 | per SPU image | rerun only the missing images |
| Compile | per object, with a hash file beside it: source hash, flags, compiler hash (docs/45 4.3) | skip the objects whose hash file matches |
| Link, verify | whole step (seconds) | rerun |
| Shaders | the prework's stamp; the PSO library is saved once per finished tier (docs/49 D7) | rerun from the first unfinished tier |

- **Interrupted.** The journal says "running", but no setup process holds the build folder's lock file. The start screen then offers Resume.
- **An app update while interrupted.** A new lift key makes the half build stale. Setup starts over from the kept inputs, and asks the player for nothing. The old folder is deleted once the new build has booted (docs/45 4.1 step 10).
- **Lift determinism is still unknown** (docs/45 R5). Within one setup it does not matter, because the lift runs once and its outputs are hashed. Across setups it decides whether objects can be reused.

### 4.3 Progress and ETA

- The overall bar weights each step by its CPU time. Starting values come from docs/45: the lift takes 1 to 3 min; the compile takes 41 CPU minutes at `/O2`; SPU and libsre take about 2 CPU minutes. docs/53 replaces these with measured values.
- Compile progress counts source bytes done. The PPU chunks are 5.7 to 38.7 MB each (measured), and compile time grows with size (inferred).
- The ETA appears after the first chunk finishes, is smoothed, and reads "about 25 minutes left".
- The largest files start first (docs/45).

### 4.4 Jobs and RAM

docs/45's rule: jobs = min(logical CPUs, (free RAM - 2 GB) / 3 GB per job), with 3 GB until docs/53 measures it. If a job fails out of memory (the exit code, or "out of memory" in its output), setup retries that file once with one job fewer (inferred design).

### 4.5 Errors and reports

Every failure gives a one-line cause, the next action, the last 1,500 characters of the tool's output (as FFCCEOT does), the full `setup.log`, and Report. Report goes through the docs/32 report path with redaction, and never attaches game files or the DLL (docs/45 section 7).

### 4.6 Disk

Check before starting, again before the compile, and again before the link (section 10 has the sizes). When the disk fills, say how much to free and offer Resume.

## 5. Shaders: the last step before the first launch

### 5.1 What the game compiles while it runs today, and what covers it

| What | Today | What covers it in setup | State |
|---|---|---|---|
| RSX programs to DXBC (FXC) | the prework at the first start after an update: 3,535 shaders in 28 s on 15 threads (docs/32) | the same pass, run from setup | exists |
| The EBOOT's 8 Cg programs | compiled at first use | docs/49 step 2 | to build |
| Fetch-form vertex programs | precompiled since 13f5fb3 (docs/49) | the same pass | exists |
| D3D12 pipelines (PSOs) | built at first draw; kept in `pso_library.bin` (PR #34) and rebuilt from the manifest after a driver change | `pso_predict.py` plus the predicted build (docs/49 steps 1 to 3): 5,062 pipelines; 98.1% of the live manifest and 100% of 6 samples (measured in docs/49); about 88 MB (inferred in docs/49) | to build |
| What prediction misses | compiled at first use, on the render thread | docs/49 fallback B: compile a frame's misses in parallel before that frame | to build |
| The backend's own pipelines (blit, present) and the UI's shaders | at device creation | nothing needed (few and fast) | exists |

### 5.2 The step

1. Run the prework pass. It also writes `shader_keys.tsv` (docs/49 step 2).
2. Run `pso_predict.py` over the game folder's files with the bundled Python. It writes `pso_predict.tsv` into the shader cache folder. docs/49 D4 already assigns this to "the setup step".
3. Build the predicted pipelines tier by tier into `pso_library.bin`. Use all threads at normal priority: the player is waiting and nothing else runs. During play, docs/49 does this at the lowest priority on up to 4 threads instead.

Time: 28 s for the shaders (docs/32), plus 35 to 101 CPU-seconds of driver compiles (docs/49), so about a minute on this PC (inferred).

Step 1 is built (10-23, docs/88 section 13): the engine's `shaders` phase runs `des_host --shaders`, 3,603 shaders in about 20 s on 15 threads. It runs at the engine's priority, below normal, like the rest of setup. Steps 2 and 3 are not built.

The step does not need the game DLL. The prework, the precompiler and the PSO code are host code. They read the game folder's files and `EBOOT.ELF` (inferred from docs/49 2.2). So under Route B it could even overlap the compile. It stays last, as asked, and because the compile already uses every core.

### 5.3 Later changes

- A new driver or GPU makes `pso_library.bin` fail to load. The next start rebuilds it in the background: the manifest's pipelines first, then the predicted rest. That is about 9 to 25 s of work on 4 idle threads (docs/49 2.6). The host could show the shaders screen for it instead, as setup does (inferred design).
- An app update changes the stamp, which holds the exe's link time, so the next start precompiles again: about 4 s over a full cache (docs/49 2.6).
- Setup and a normal start share the same code. Setup just runs it in the foreground first.

## 6. The bundled toolchain

### 6.1 Compiler and linker (Route B)

From the official LLVM 22.1.8 install on this PC (measured):

| File | Size | Imports |
|---|---|---|
| `bin/clang.exe` | 104,745,472 bytes | ADVAPI32, KERNEL32, OLEAUT32, SHELL32, VERSION, ntdll, ole32 |
| `bin/lld-link.exe` | 71,456,768 bytes | ADVAPI32, KERNEL32, OLEAUT32, SHELL32, ntdll, ole32 |
| `lib/clang/22/include` | 16 MB, 249 entries | headers |

- **No Visual C++ runtime needed.** Both exes import only Windows system DLLs, so LLVM's release builds carry their CRT inside (measured).
- **One linker file is enough.** `lld-link.exe`, `ld.lld.exe` and `lld.exe` are the same file, as are `clang++.exe` and `clang.exe` (measured).
- **Size.** docs/45 measured the trimmed set at 191.8 MB raw, 68.8 MB zipped and 48.7 MB with xz. FFCCEOT ships the same three parts, taken from an LLVM 22.1.8 install (read, PACKAGING.md).
- **No compiler-rt.** `clang_rt.builtins-x86_64.lib` (190 KB, measured) is not needed. None of docs/45's outside symbols comes from compiler-rt. Its "compiler runtime" group is MSVC CRT symbols (the stack protector, C++ thread-local support, `_fltused`), which the flags and the support file remove (read, docs/45 3.2).
- **Pin one version per release.** Its hash is part of the lift key (docs/45 4.2).
- **Later.** An LLVM built for the X86 target only might save 20 to 30 MB (docs/45, inferred).

### 6.2 Python, or a compiled lifter

| Option | Verdict | Why |
|---|---|---|
| Embeddable Python (10.9 MB zipped) | **chosen** | The lift chain uses only the standard library (docs/45, read). Its `._pth` file isolates it from other Pythons on the PC (read in docs/45). |
| Scripts as a zip of `.pyc` built in CI | yes, with the above | fewer files and a faster start; Python imports from zips (inferred) |
| PyInstaller or Nuitka exes | no | frozen-Python exes are common antivirus false positives (docs/45) |
| Cython | no | still needs CPython; no gain for a one-off run |
| Lifters ported to C++ inside the host | not now | weeks of work, and it forks the tools from ps3recomp's Python lifters (docs/45) |

Open (docs/45 questions 12 and 13): does the `._pth` setup put the script's folder on `sys.path`, and do the lifters' `-j` worker processes start under the embeddable `python.exe`? Trial T4 answers both.

### 6.3 CMake and Ninja: not shipped

| Job | CMake and Ninja today | The host instead |
|---|---|---|
| Find the compiler | CMake's detection, under vcvars | the bundled path, hash-checked |
| List the sources | globs in `port/CMakeLists.txt` (read) | the pipeline script's output list |
| Run jobs in parallel | `--parallel 4` | its own pool, sized by RAM, largest first (FFCCEOT's `build_module`, read) |
| Skip what is done | timestamps | content hashes, which also survive a re-lift that rewrites files unchanged (inferred) |
| Progress, cancel, errors | none for the player | per-file progress, cancel through the job object, the output tail |

Sizes, for the record (read): CMake 4.4.4's Windows zip is 54,400,562 bytes, and Ninja 1.13.2's `ninja-win.zip` is 291,570 bytes. The build is about 31 compile jobs and one link, a graph the host handles easily. Developers keep CMake. CI uses CMake for the host.

### 6.4 The llvm-mingw toolchain (Route A' only)

- llvm-mingw 20261006 is built with LLVM 23.1.3. Its `ucrt-x86_64.zip` is 190,836,485 bytes (read, GitHub API) and targets i686, x86_64, armv7 and arm64 in one package (read, README). It defaults to UCRT, and uses native TLS (read).
- The x86_64-only parts, sized from MSYS2's clang64 packages (LLVM 23.1.3, mingw-w64 14.0.0 snapshot), compressed and installed (read):

| Package | Compressed | Installed |
|---|---|---|
| mingw-w64 headers | 6.57 MB | 91.36 MB |
| mingw-w64 CRT (import libraries, startup code) | 2.89 MB | 47.48 MB |
| libc++ | 1.87 MB | 19.38 MB |

- So a MinGW sysroot for x86_64 adds about 160 MB on disk and about 20 to 30 MB zipped to clang plus lld (inferred: MSYS2 uses zstd, and zip compresses worse).

## 7. MSVC dependencies: what breaks with a non-MSVC toolchain

### 7.1 What cannot ship, and where today's build uses it

- **Cannot ship:** the MSVC STL, the MSVC CRT libraries and the Windows SDK's headers and import libraries (task statement; docs/45 3.2). The build today is clang-cl with all three (README).
- **Import libraries in use (read).** From `ps3recomp/CMakeLists.txt`, `port/CMakeLists.txt`, `port/ui/CMakeLists.txt` and `#pragma comment(lib)`: `ws2_32`, `xinput`, `ole32`, `bcrypt`, `d3d12`, `dxgi`, `d3dcompiler`, `uuid`, `dxguid`, `d3d11`, `imm32`, `shell32`, `user32`, `dbghelp`, `windowscodecs`, `wtsapi32`, `avrt`, `mfplat`, `mfuuid`, `setupapi`, `hid`.
- **Shader compiler.** FXC only, through `d3dcompiler_47.dll`, which is part of Windows (inferred). No DXC: no `dxcapi.h` or `IDxc*` anywhere (measured).

### 7.2 Census method

A regex census per category over each tree. It counts matches and files. The script lives in this lane's scratch folder and is not committed. The trees:

| Tree | Source | Files | Lines |
|---|---|---|---|
| port | origin/master `port/` | 73 | 10,470 |
| runtime | ps3recomp fork/master f15a4f0 `runtime/` | 88 | 42,582 |
| libs | the same, `libs/` | 248 | 108,947 |
| include | the same, `include/` | 11 | 1,556 |
| third_party | the same, `third_party/` (at3_standalone) | 25 | 8,507 |
| RmlUi 6.3 | `port/build/_deps/rmlui-src` | 587 | 135,537 |
| FreeType 2.14.3 | `port/build/_deps/freetype-src` | 502 | 244,212 |

The lifter output was looked at read-only and is not in the counts (section 7.8).

### 7.3 Census (measured): matches (files)

| Category | port | runtime | libs | include | third_party | RmlUi | FreeType |
|---|---|---|---|---|---|---|---|
| `__try`/`__except` | 0 | 2 (1) | 0 | 0 | 0 | 0 | 0 |
| `__declspec(...)` | 3 (3) | 25 (13) | 11 (6) | 1 (1) | 2 (1) | 17 (8) | 11 (5) |
| `#pragma comment` | 1 (1) | 0 | 19 (11) | 0 | 0 | 1 (1) | 0 |
| `#pragma section` and other MSVC pragmas | 1 (1) | 0 | 0 | 0 | 3 (1) | 12 (6) | 18 (7) |
| `<intrin.h>` | 0 | 5 (5) | 1 (1) | 0 | 0 | 3 (3) | 2 (1) |
| MSVC intrinsics (`_BitScan*`, `_byteswap_*`, `__cpuid`, ...) | 0 | 36 (5) | 5 (3) | 3 (1) | 0 | 16 (5) | 6 (2) |
| `_Interlocked*` intrinsics | 1 (1) | 14 (5) | 1 (1) | 0 | 0 | 0 | 0 |
| Win32 `Interlocked*` functions | 2 (1) | 71 (10) | 104 (6) | 0 | 0 | 0 | 2 (1) |
| MSVC keywords (`__forceinline`, `__int64`, `__stdcall`, ...) | 5 (2) | 19 (5) | 13 (6) | 0 | 0 | 19 (5) | 20 (5) |
| `_aligned_malloc` | 0 | 2 (1) | 0 | 0 | 2 (1) | 8 (3) | 0 |
| `_s` CRT functions | 7 (2) | 14 (2) | 18 (5) | 0 | 0 | 3 (1) | 1 (1) |
| Other MS CRT names (`_stat64`, `_fseeki64`, `_beginthreadex`, ...) | 28 (6) | 55 (8) | 40 (15) | 1 (1) | 1 (1) | 4 (3) | 16 (5) |
| VC runtime only (`_set_abort_behavior`) | 1 (1) | 0 | 0 | 0 | 0 | 0 | 0 |
| `.CRT$XC*` sections | 5 (1) | 0 | 0 | 0 | 0 | 0 | 0 |
| C++ COM (`__uuidof`, `IID_PPV_ARGS`) | 14 (3) | 0 | 3 (2) | 0 | 0 | 11 (3) | 0 |
| C COM (`lpVtbl`) | 0 | 0 | 692 (3) | 0 | 0 | 0 | 0 |
| COM methods that return a struct | 1 (1) | 0 | 39 (2) | 0 | 0 | 16 (2) | 0 |
| `setjmp`/`longjmp` | 0 | 26 (4) | 0 | 0 | 0 | 0 | 13 (5) |
| Thread-local spellings (all kinds) | 2 (1) | 41 (17) | 10 (4) | 1 (1) | 0 | 0 | 0 |
| `_MSC_VER`, `__clang__`, `__GNUC__` checks | 1 (1) | 25 (10) | 35 (7) | 4 (2) | 5 (2) | 97 (15) | 91 (22) |
| `_WIN32`, `__linux__`, `__APPLE__` checks | 2 (1) | 327 (43) | 224 (40) | 0 | 1 (1) | 43 (5) | 43 (11) |
| `__attribute__((weak))`, `musttail` | 0 | 13 (9) | 0 | 1 (1) | 0 | 0 | 0 |
| ANSI `...A` Win32 calls | 6 (5) | 51 (14) | 37 (6) | 0 | 0 | 5 (3) | 7 (3) |

The first-party `__declspec` uses, without stb and tests, by kind and guard (measured):
- `thread`: 20 uses. 10 are in `_MSC_VER` branches, so MinGW takes the `__thread` branch. 10 are in `_WIN32` branches: `spurs_job.c:140`, `spu_dma.h:1016`, `sys_ppu_thread.c:14` (the thread-exit `jmp_buf`), `sys_timer.c:114-117`, `thread_diag.c:8`, and two in libs (`cellSpurs.c`, `cellSysutil.c`).
- `align`: 6 uses. 5 are in `_MSC_VER` branches; `rsx_d3d12_backend.c:10116` is in a `_WIN32` branch.
- `allocate`: 1 use, unguarded: the crash hook in `port/ui/src/crash_report.cpp:382`.
- `noinline` 3 and `selectany` 2: all in `_MSC_VER` branches.

### 7.4 Probes (measured)

Two-to-six-line C or C++ files, compiled with the installed clang 22.1.8 for both targets, with `-ffreestanding -nostdinc`, then inspected with `llvm-objdump`, `llvm-readobj` and `llvm-nm`.

| Spelling | MinGW target, plain | MinGW target, `-fms-extensions` | MSVC target |
|---|---|---|---|
| `__declspec(thread) int x;` | "unknown attribute 'thread' ignored": `x` goes into `.bss`, one copy for all threads | in `.tls$` | in `.tls$` |
| `__declspec(align(64))` | "unknown attribute 'align' ignored": 16-byte alignment | 64 | 64 |
| `__try` / `__except` | error: "use of undeclared identifier '__try'" | compiles | compiles |
| `#pragma comment(lib, ...)`, `#pragma comment(linker, "/include:...")` | dropped with no warning: no directives in the object | `/DEFAULTLIB:` and `/include:` directives | the same |
| `__declspec(allocate(".CRT$XCV"))` | "unknown attribute 'allocate' ignored": the pointer goes into `.data` and never runs | in `.CRT$XCV` | in `.CRT$XCV` |
| A C++ global with a constructor | in `.ctors` (not run plain; inferred from the next column) | in `.ctors` | in `.CRT$XCU` |

What the last row means (read, mingw-w64 `crt/crtexe.c`): MinGW's startup calls `_initterm(__xc_a, __xc_z)`, which runs the `.CRT$XC*` entries, and only then `__main()`, which runs `.ctors`. So under MinGW, `crash_report.cpp`'s `.CRT$XCV` hook would run before every C++ static constructor. Its own comment says it relies on the reverse order: ".CRT$XCV runs after every ordinary static initializer (.CRT$XCU) and before main()" (read).

Two more probes, for Route B's freestanding module (MSVC target, `-ffreestanding -nostdlibinc`):
- clang's own `<intrin.h>`, `<stdint.h>`, `<stddef.h>` and `<stdatomic.h>` work. `_BitScanForward64`, `__umulh` and `__popcnt64` compile. `_byteswap_uint64` does not: MSVC declares it in `stdlib.h`. The lift uses `__builtin_bswap*` instead and never calls `_byteswap_*` (measured: counts in `ppu_recomp.h` and chunks 000 and 013).
- C++ `<atomic>` is not found. It belongs to the C++ library, not to clang's headers.

### 7.5 What each category means on MinGW (Route A or A')

| Category | First-party count | On MinGW | Work |
|---|---|---|---|
| `__declspec(thread)` in `_WIN32` branches | 10 sites | **wrong without `-fms-extensions`** (one copy for all threads; only a warning); right with it (measured) | `-fms-extensions` everywhere, plus a portable `PS3_TLS` macro and a CI grep so new sites can't slip in |
| `__declspec(align)` in a `_WIN32` branch | 1 | wrong without the flag, right with it (measured) | the same |
| `.CRT$XCV` crash hook | 1 (5 lines) | **runs too early, even with the flag** (measured and read) | call the hook from `main`/`WinMain`, or use a portable constructor |
| `__try` | 1 site (`ppu_loader.cpp:628`, the sampling profiler) | error without the flag (measured). It is probably ineffective on both targets anyway (section 7.9). | rewrite without SEH |
| `#pragma comment(lib)` | 20 | dropped without the flag; with it, lld in MinGW mode has to map `foo.lib` to `libfoo.a` (inferred) | pass every library on the link line; the link list is ours anyway |
| MSVC intrinsics, `_Interlocked*`, Win32 `Interlocked*` | 44 + 16 + 177 | mingw-w64's `intrin.h` and `winnt.h` provide them (inferred: long-standing mingw-w64 headers); most sites are `_MSC_VER`-guarded and take the GCC branch that the Linux build uses | compile and fix leftovers |
| MSVC keywords, `_aligned_malloc`, `_s` and MS CRT names | 37 + 2 + 39 + 124 | mingw-w64's headers declare them, UCRT exports them, and the secure API is on by default (inferred) | compile and fix leftovers |
| C COM (692) and struct-returning methods (40) | 3 files | mingw-w64's vtable entries use the out-parameter form for struct returns; its C wrapper macros differ (read, via an SDL commit note; the 1.25 MB `d3d12.h` was too large to fetch). The backend calls through `lpVtbl` in the out-parameter form, for example `lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h)` and `lpVtbl->GetResourceAllocationInfo(device, &ai, 0, 1, td)` (read). | compile; probably nothing |
| C++ COM (`__uuidof`, `IID_PPV_ARGS`) | 17 + RmlUi 11 | mingw-w64 supplies `__uuidof` through `__CRT_UUID_DECL` (inferred) | compile and fix gaps |
| `setjmp`/`longjmp` | 26 | UCRT's `longjmp` unwinds through frames, as with MSVC (inferred). Thread exit longjmps through lifted frames. | test in T1 and in a boot |
| `weak`, `musttail` | 14 | COFF weak externals work in lld's MinGW mode, and `musttail` is a clang feature (inferred) | compile |
| Static-init order across files | unknown | `.ctors` versus `.CRT$XCU` order differs (measured); SPU workloads register through `__attribute__((constructor))` (docs/45) | audit while booting |
| C++ library | port: `<format>` 5, `<filesystem>` 4, `<regex>` 1, `<ranges>` 1, `<charconv>` 1 (integer `from_chars`) | libc++ 23 has all of these (inferred) | compile |
| Debug info and crash stacks | n/a | needs CodeView and a PDB from lld (`-gcodeview`, `--pdb`) for the docs/32 crash stacks (inferred) | flags, check |

Estimate for the port itself: 3 to 6 days, plus 2 to 4 days to boot and match frame rate (it needs run requests), plus CI and packaging (section 8.2). Most categories need only "compile it and see" (inferred), so T1 would turn most of this table into facts in one afternoon.

### 7.6 Headers and import libraries in mingw-w64

| API | Header | Import library | Status in mingw-w64 master | Licence |
|---|---|---|---|---|
| D3D12 | `d3d12.idl`: `ID3D12Device` to `ID3D12Device10`, `ID3D12PipelineLibrary`(1), the DRED interfaces, `ID3D12GraphicsCommandList7` | `d3d12.def` (`D3D12CreateDevice`, `D3D12GetDebugInterface`, `D3D12SerializeRootSignature`, ...) | present (read) | LGPL-2.1+ (Wine) (read) |
| D3D12 debug layer | `d3d12sdklayers.idl`: `ID3D12Debug`, `ID3D12InfoQueue` | (in `d3d12`) | present (read). The DRED interfaces live in `d3d12.idl`, as in the SDK. | LGPL-2.1+ (read) |
| DXGI | `dxgi1_4`, `dxgi1_6.idl`: `IDXGIFactory6`/`7`, `IDXGIAdapter4`, `IDXGIOutput6`, `DXGI_GPU_PREFERENCE` | `dxgi` | present (read headers; library inferred) | LGPL-2.1+ (read) |
| D3D11On12 (UI overlay) | `d3d11on12.idl`: `D3D11On12CreateDevice`, `ID3D11On12Device` to `Device2` | `d3d11` | present (read headers; library inferred) | LGPL-2.1+ (read) |
| FXC | `d3dcompiler.h` | `d3dcompiler_47.def` (`D3DCompile`, `D3DCreateBlob`) | present (read library; header inferred) | |
| IIDs | | `libdxguid.a`, built from `libsrc/dxguid.c`, which defines `INITGUID` and includes `d3d12.h`, `d3d12sdklayers.h`, `dxgi1_4.h`, `dxgi1_6.h` and `d3d11on12.h` | the D3D12 and DXGI IIDs are emitted (read the includes; emission inferred) | |
| Media Foundation (movies) | `mfapi.h`, `mfidl.h`, `mftransform.idl` (`IMFTransform`, `MFT_OUTPUT_DATA_BUFFER`) | `mfplat.def` (`MFStartup`, `MFCreateSample`, `MFCreateMemoryBuffer`, `MFCreateMediaType`, `MFTEnumEx`); `libmfuuid.a` from `libsrc/mfuuid.c` | present (read `mftransform.idl`, `mfplat.def`, `mfuuid.c`) | LGPL-2.1+ for `mftransform.idl` (read) |
| WASAPI and MMCSS | `mmdeviceapi.h`, `audioclient.h`, `avrt.h` | `avrt.def` (`AvSetMmThreadCharacteristicsW`, `AvRevertMmThreadCharacteristics`), `ole32` | present (read `avrt.def`; headers inferred) | |
| DualShock HID | `hidsdi.h`, which includes `hidpi.h`; `setupapi.h` | `hid.def` (`HidD_GetAttributes`, `HidD_GetHidGuid`, `HidD_GetPreparsedData`, `HidD_Get/SetFeature`, `HidP_GetCaps`); `setupapi.def` (`SetupDiGetClassDevsW/A`, ...) | present (read). `hidsdi.h` is public domain. | |
| XInput | `xinput.h` | `xinput1_4.def` (`XInputGetState`, `XInputSetState`, `XInputGetCapabilities`, `XInputEnable`, ...) | present (read). Link `-lxinput1_4` explicitly. | |
| Lock screen | `wtsapi32.h` | `wtsapi32.def` (`WTSRegisterSessionNotification`, `WTSQuerySessionInformationW`) | present (read) | |
| Others: `ws2_32`, `bcrypt`, `dbghelp`, `windowscodecs`, `shell32`, `user32`, `imm32`, `uuid` | | | not checked one by one; all are long-standing in mingw-w64 (inferred) | |

Route B does not depend on any of this. Its host is built with the Windows SDK in CI, and the module includes no Windows headers.

### 7.7 The C++ library and the C++ ABI

- **Route B.** Only C crosses the boundary. The host uses the MSVC STL inside itself. The module uses no C++ library: it is C++ only as a language, with `extern "C"` symbols at the edge (`ppu_recomp.h` declares the runtime calls `extern "C"`, read). So no C++ ABI meets another (inferred).
- **Route A'.** Everything uses libc++ and the Itanium C++ ABI. The CI-built libraries and the player-built objects must come from the same pinned toolchain (inferred).

### 7.8 What the freestanding module needs (Route B)

From the lift (a read-only look at include lines, plus identifier counts that were measured; no code quoted):
- **`ppu_recomp.h` includes** `<stdint.h>`, `<atomic>` (C++) or `<stdatomic.h>` (C), and `<string.h>`.
- **Each chunk includes** `<stdio.h>`, `<string.h>`, `<math.h>`, and `<intrin.h>` under `_MSC_VER`.
- **Library calls in the chunks.** `memcpy` (35,271 in chunk 000), `memset`, `sqrt`, `sqrtf`, `nearbyint`, `nearbyintf`, `fabs`, `trunc` and `fma`. `_umul128`, `_mul128` and `_BitScanReverse*` come from the lifter's own template.
- **The SPU lift** includes the runtime's `spu_context.h` and `spu_helpers.h`. Those pull in `<stdlib.h>`, `<stdio.h>`, `<string.h>`, `<math.h>`, `<intrin.h>` and `<smmintrin.h>` (read).

What that means:
1. **`<atomic>`.** It is missing in freestanding mode (measured). The lifter's template wraps it in `#ifndef PPU_FENCE` (read, `tools/ppu_lifter.py` around line 61). A forced-include header can define `PPU_FENCE` with `__atomic_thread_fence`, so the lifter needs no change (inferred).
2. **`<intrin.h>`, `<stdint.h>`, `<stdatomic.h>`, `<smmintrin.h>`** come from clang's own headers (measured for the first three).
3. **Small shims.** `string.h`, `math.h`, `stdio.h` and `stdlib.h`, plus a support file for docs/45's 14 C library symbols and `_fltused` (docs/45 6.1 step 3). The other compiler-runtime symbols go away with `-fno-stack-protector` and with no thread-locals in the module (docs/45 3.2).
4. **No thread-locals in the module.** Without the CRT there is no `_tls_index` and no `_tls_used`, so a module that uses a thread-local fails to link or gets no TLS directory (inferred, PE format). The support file could define both, as the CRT's TLS support does, but it is simpler to have no thread-locals. Today the module side has two kinds:
   - `g_trampoline_fn` and `g_spu_trampoline_fn` (docs/45). The runtime's loop reads and clears `g_trampoline_fn` after every call it makes (read: `icall_run` in `ppu_loader.cpp`).
   - The shadow stack's `g_ds_sp` and `g_ds_stk`. Default builds already compile it out: `DES_SHADOW_STACK` is off, which defines `DS_NO_SHADOW_STACK` (read, `port/CMakeLists.txt`).

   Proposal: move both trampoline pointers into the PPU and SPU context structs. Every lifted function already takes the context. A store to the context is no slower than a TLS store, which first loads `_tls_index` (inferred). The runtime and the lifter template share the context layout (read), so this is a coordinated ps3recomp change with an ABI number bump.
5. **No static constructors.** A `/noentry` DLL runs none. The SPU workload registration (`__attribute__((constructor))`) becomes an explicit init call (docs/45).
6. **Keep the unwind tables.** Thread exit uses `longjmp` through lifted frames (docs/45). On x64 Windows, clang emits `.pdata` and `.xdata` for every function that needs them, even with `-fno-exceptions` (inferred: the x64 ABI requires them).

docs/45's run request R1 (one chunk, freestanding) can now be done with these settings. It is trial T2.

### 7.9 Side finding: `__try` around a faulting load

`samp_stack_guest` in `ppu_loader.cpp` (the sampling profiler) reads other threads' stack slots inside `__try`/`__except`. In the probe, a `__try` around a plain load left no reference to `__C_specific_handler` on either target (measured). That suggests clang builds no handler for a fault directly inside the `__try` body (inferred: clang's SEH covers faults in calls unless asynchronous exceptions are enabled). Today's build passes `/EHsc`, not `/EHa` (read, `port/CMakeLists.txt`). ps3recomp's CMake sets no `/EH` flag of its own, so the runtime gets CMake's default, `/EHsc` (inferred). Either way clang's asynchronous SEH is off. So a bad stack slot may crash the profiler under today's clang-cl too. Fix it in any route: use `VirtualQuery`, or a `ReadProcessMemory` on the own process. To verify with a forced fault in a small test.

**Verified by trial T3 (2026-10-19, section 13).** With clang-cl 22.1.8 and the flags today's build gives `ppu_loader.cpp` (`/O2 /EHsc`, C++20), the guard does not work: a fault on the load inside `__try` kills the process with an access violation (measured). Moving the load into a non-inlined function does not help at `/O2` either (measured; inferred cause: clang sees that the callee cannot throw and drops the handler range). Only `/EHa` catches both forms; at `/Od` only the callee form is caught (measured). All three objects do reference `__C_specific_handler` (measured), so a reference to it does not show that a fault is caught. A scan bounded with `VirtualQuery` works in every build (measured).

What it means today: `samp_stack_guest` runs only with `PS3_SAMPLE` and `PS3_SAMPLE_MAIN=1`, for the boot thread (read). It can take the game down when that thread's `rsp` is less than 4 KB below the top of its stack and the memory above is not mapped. That is rare, and the knobs are developer-only. The fix is the `VirtualQuery` bound: one call per sample, and no `__try` left in the tree, which also removes the only SEH site Route A' would have to rewrite (section 7.5).

## 8. The two routes compared

### 8.1 Route B in detail

- **The boundary.** docs/45 3.2 counts 72 outside symbols. Its table lists 31 runtime functions, 4 port functions (8 with the newer hooks), 16 globals, 14 C library and 6 compiler-runtime symbols (measured there). Those groups add up to 71, so one symbol is unlisted; recount when building the table. They become a host table passed to `des_game_init`. The memory-accessor globals move into the DLL.
- **The trampolines.** Context fields (section 7.8 point 4), instead of docs/45's other option, per-thread pointers into DLL-owned TLS. A DLL without the CRT has no TLS support unless the support file supplies it.
- **The header split** with an ABI number (docs/45). A DLL built for another ABI is refused at load and rebuilt, as FFCCEOT does (read).
- **The VC++ runtime.** `/MT` everywhere in the host: set `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` before the FetchContent of RmlUi and FreeType, so they match (inferred; CMake policy CMP0091). Then:
  - The exe imports no `MSVCP140`, `VCRUNTIME140` or `VCRUNTIME140_1` (they are there today, docs/45).
  - The module has no CRT, so the process has exactly one CRT. No `FILE*` or `malloc` memory crosses the boundary (inferred).
  - Microsoft: "Static linking creates a single binary without a DLL dependency, so that you don't have to deploy the Microsoft C++ library files separately". It advises against it only because statically linked code "can't be updated in place" (read, "Deployment in Microsoft C++"). Our updates replace the exe anyway.
  - FFCCEOT-Recomp links its C and C++ runtime statically for the same reason: "the player installs nothing" (read, its PACKAGING.md).
- **CI.** It builds with clang-cl against the MSVC toolset and Windows SDK on the GitHub Windows runner. ps3recomp's CMake warns that `cl.exe` does not build it: the runtime and the lift use `__atomic_*`, `__int128` and `__builtin_bswap*` (read). So CI stays on clang-cl. Building there with Microsoft's licensed toolset, and redistributing the statically linked result, is the usual open-source practice (inferred; the user may want to confirm the licence terms).
- **Host-only updates** need nothing on the player's PC while the ABI number holds.
- **Risks.** The split's frame-rate cost (docs/45 R3: indirect host calls); the TLS change; ABI discipline (every change to the context layout or the inline accessors means a full rebuild for players).

### 8.2 Route A' in detail

- CI builds, with llvm-mingw: the runtime, libs, port non-game code, UI, RmlUi and FreeType as static `.a` libraries. It also builds a launcher exe that holds the setup UI and the shader step, because the game exe does not exist before setup.
- The player's PC compiles the lift with the bundled llvm-mingw and links one exe: the CI libraries, libc++, mingw-w64's CRT and import libraries, and the lifted objects.
- Licences: the exe links mingw-w64's runtime, whose licence asks for its notices with binaries (read). The exe is never distributed, though.
- **Host-only updates** need a relink on the player's PC: 1 to 4 s (docs/45), but only with about 1.4 GB of objects kept (docs/45 measured 1,357 MB with `/Zi`; less without, unmeasured). Otherwise it is a full rebuild.
- Work items (inferred):

| Item | Days |
|---|---|
| CMake toolchain file for llvm-mingw; `-fms-extensions`; explicit link list | 1 to 1.5 |
| TLS and align macros, and a CI grep | 0.5 to 1 |
| Crash hook start-up order; audit other static-init order | 0.5 to 1 |
| SEH site rewrite (useful in any route) | 0.25 |
| C++ COM, Media Foundation, WASAPI and HID header gaps | 0.75 to 1.5 |
| CRT and libc++ leftovers | 0.5 to 1 |
| Lift, SPU and libsre under MinGW; boot; frame-rate A/B (run requests) | 1 to 2 |
| PDB and crash stacks for a MinGW exe | 0.5 |
| Player-side link and delay-loaded `mfplat` (`llvm-dlltool -y` or `LoadLibrary`) | 0.5 to 1 |
| Separate launcher exe | 1 to 2 |
| Relink on update, and the object cache | 0.5 to 1 |
| No game inputs in the CI libraries (NID table, icon at run time, no TSP blob); the UTF-8 manifest as a prebuilt resource object | 0.5 to 1 |
| Identical exe on every PC, and an expected hash per flag set (as docs/45 6.1 step 13) | 1 to 2 |
| **Total** | **8.5 to 16**, plus a second toolchain to keep green |

### 8.3 Route A (everything on the player's PC)

The same as A', but the player's PC also compiles the runtime, UI, RmlUi and FreeType. That is a few more CPU minutes (inferred) and a build graph for third-party libraries without CMake. It gains nothing over A'. Not considered further.

### 8.4 Comparison

| | Route B | Route A' |
|---|---|---|
| Player toolchain | clang + lld + clang's headers: 68.8 MB zipped, 192 MB on disk (measured) | the same plus a MinGW sysroot and libc++: about 90 to 100 MB zipped, about 350 MB on disk (inferred) |
| What the player compiles | the lift only | the lift only; links against CI libraries |
| Boundary work | the split: 72 symbols, TLS, init (docs/45) | none |
| Porting work | none: the host keeps today's toolchain | MinGW port (section 7.5) |
| Host-only update | nothing to do | a relink with about 1.4 GB of kept objects, or a full rebuild |
| Toolchains to maintain | one host toolchain (as today) plus fixed flags for generated code | MSVC for development and MinGW for release, or a switch of everything |
| Things that can differ between players' PCs | clang, lld and our headers only | also the CRT start-up, libc++ and the sysroot |
| Identical DLL or exe on every PC (docs/45 4.5) | easy: few inputs | possible: more inputs |
| Crash symbols | the host PDB from CI; the module through the function table (docs/45) | a PDB from lld on the player's PC |
| Precedent | FFCCEOT-Recomp: freestanding module, pixel-identical over 8,000 frames (read in docs/45) | AC4-Port: whole exe with llvm-mingw (docs/31) |
| Unknowns | the split's frame-rate cost (R3) | MinGW behaviour of a large Win32 runtime |
| Work (inferred) | about 28 to 44 days | about 28 to 45 days, plus a second toolchain from then on |

### 8.5 Recommendation and order

Route B. Start with docs/45's milestone 1, the split built with today's toolchain on both sides (4 to 7 days). If the split costs frame rate that can't be won back (R3), switch to A'. The MinGW census (section 7) then turns into its work list.

## 9. What CI builds, and what the player's PC builds

| Item | Where | Why |
|---|---|---|
| `des_host.exe`: runtime, libs, port host code, UI, RmlUi, FreeType, at3, the HLE NID table, shader precompile, setup | CI (Route B) | not game-derived |
| The host's PDB | CI, kept as a release artifact, not in the zip | crash symbols |
| Module headers: the fixed runtime header part, freestanding shims, the forced-include file, the ABI number | CI (copied) | ours |
| `ds_support.obj`: `memcpy`, `memset`, `memmove`, the math, `_fltused` | CI, built with the pinned clang | ours, and identical for everyone |
| Lifter and pipeline scripts, `pso_predict.py`, as a `.pyc` zip | CI | ours (MIT) |
| Data tables: known input hashes, job blob offsets, sizes and fingerprints, the override list, `insn_hooks.txt`, published DLL hashes | CI | addresses and hashes, not code (docs/45) |
| Embeddable Python, trimmed LLVM | CI repackages, hash-pinned | third-party |
| UI resources, fonts, licences | CI | ours and third-party |
| The window and shortcut icon | the player's PC, at run time from `ICON0.PNG` (docs/45). The exe's own file icon is ours (docs/51 6, point 4). | game data |
| Decrypted `EBOOT.ELF`, the 26 decrypted `.sdat` files, and `libsre.prx` while LLE | the player's PC; under docs/51 Plan L the player makes them and setup checks and copies them | game and firmware data |
| Loader JSON, lifted PPU, SPU and libsre sources, glue, workload table | the player's PC | game-derived |
| Objects, `des_game.dll`, its map | the player's PC | game-derived |
| DXBC cache, `shader_keys.tsv`, `pso_predict.tsv`, `pso_library.bin` | the player's PC | game- and driver-derived |

## 10. Download size and disk use

### 10.1 Download

| Part | Route B | Route A' |
|---|---|---|
| Host or launcher (zipped) | 10 to 15 MB (docs/45, inferred) | 10 to 15 MB |
| clang, lld and headers | 68.8 MB (measured, docs/45) | 68.8 MB, or the llvm-mingw equivalent |
| MinGW sysroot and libc++ (x86_64) | none | 20 to 30 MB (inferred, section 6.4) |
| CI-built static libraries | none | 8 to 12 MB (inferred) |
| Embeddable Python | 10.9 MB (read) | 10.9 MB |
| Scripts, data, licences | about 1 MB (inferred) | about 1 MB |
| **Total** | **about 91 to 96 MB** | **about 120 to 140 MB** |

### 10.2 Disk, Route B (measured where marked; the rest inferred)

| | During setup (peak) | After setup |
|---|---|---|
| Install: LLVM 192 MB (measured), Python about 20 to 25 MB, host about 15 to 40 MB | about 0.25 GB | about 0.25 GB; the toolchain stays for rebuilds |
| Kept inputs: `EBOOT.ELF` 28.2 MB (measured), the decrypted `.sdat` set about 1.4 MB, and `libsre.prx` 0.24 MB while LLE (docs/51) | 0.03 GB | 0.03 GB |
| Sources: PPU lift 444 MB, SPU 15 MB, and libsre with its JSON 100 MB while LLE (measured today's trees) | 0.56 GB (0.46 GB without libsre) | deleted |
| Objects: 1,357 MB with `/Zi` (docs/45 measured); player builds use no `/Zi`, size unmeasured | up to 1.36 GB | deleted, or kept as a cache (decision 5) |
| `des_game.dll` | 0.28 to 0.29 GB (docs/45) | 0.29 GB |
| Shader cache: 52 MB in all, measured on a cache from real play plus the startup precompile. It covers only the areas played so far (details below). | 0.05 GB (measured); about 0.12 to 0.14 GB for the whole game (inferred) | the same |
| **Total** | **about 2.5 GB at peak, during the compile and link; ask for 4 GB free** (docs/45). The shader cache is written after the clean-up, so it does not add to the peak (inferred). | **about 0.6 GB with today's cache; about 0.7 GB with a full-game cache (inferred)** |

The shader cache, measured by the main session on the main tree's `gamedata/shader_cache` (du, 2026-10-18). The cache was built by real play in the Tutorial, the Nexus and the Boletarian Palace, plus the startup precompiles:

| Part | Size (measured) |
|---|---|
| DXBC: 3,637 files (3,499 `ps_5_0`, 138 `vs_5_0`) | 28.2 MB |
| `pso_library.bin` | 10.7 MB |
| `pso_manifest.bin` | 0.9 MB |
| `pso_blobs/`: 265 files | 3.8 MB |
| `fp_variants.txt` | 4 KB |
| **All, on disk** | **52 MB** |

The parts add up to about 44 MB. du counts whole disk clusters for each of the roughly 3,900 files, which probably explains the difference (inferred).

This cache covers only the areas played so far, so a full-game cache will be larger. How much larger (inferred):
- The DXBC part should grow little. The startup precompile alone writes 3,535 shaders (docs/32), and the cache holds 3,637.
- The pipeline library grows with play. Today's 10.7 MB matches docs/49's 10,715,422 bytes for 616 pipelines. docs/49's predicted set for the whole game is 5,062 pipelines and about 88 MB (inferred there).
- So a full-game cache with prediction would be about 120 to 140 MB.

Route A' adds about 0.2 GB to the install, and about 1.4 GB after setup if objects are kept for relinks.

## 11. Licences of everything bundled

| Component | Where it ships | Licence | What we must do |
|---|---|---|---|
| ps3recomp runtime, libs and tools | host, scripts | MIT (read) | keep the notice |
| at3_standalone (in ps3recomp `third_party/`) | host, statically linked | LGPL-2.1-or-later; "kept in this directory as a separable component; the rest of ps3recomp is MIT" (read, its README.txt) | ship the LGPL text and a link to its source, and let players use a changed decoder. A separate DLL is the clearest way. The public, buildable host source may be enough on its own (inferred). Same question as docs/51's decision 8. |
| Port code | host | MIT (read, LICENSE) | keep the notice |
| RmlUi 6.3 | host, static | MIT (read) | keep the notice |
| FreeType 2.14.3 | host, static | FreeType License, taken instead of GPLv2 (read, `docs/FTL.TXT`) | credit in the docs and About: "Portions of this software are copyright © <year> The FreeType Project (www.freetype.org). All rights reserved." (read) |
| Alegreya Sans, Cinzel | UI fonts | SIL OFL 1.1 (read, `port/ui/res/fonts`) | ship the OFL texts (already beside the fonts) |
| Material Symbols Rounded | UI font | Apache-2.0 (read) | ship the licence (already beside the font) |
| stb headers (in ps3recomp libs) | host | public domain or MIT (inferred, stb's usual dual licence) | none needed |
| MSVC static CRT and STL | inside the host (Route B) | Visual Studio licence; static linking is a documented deployment (read) | nothing to ship |
| LLVM: clang, lld, clang's headers | toolchain | Apache-2.0 with LLVM exception (read, docs/45) | ship `LICENSE.TXT` and the third-party notices for whatever the release build links in statically (inferred: check the release's licence files when packaging) |
| Embeddable Python 3.13.9 | tools | PSF licence, plus its bundled libraries (OpenSSL, libffi, zlib, bzip2, liblzma, SQLite, mpdecimal, expat) (inferred list) | ship its `LICENSE.txt`, which covers the bundled ones (docs/45) |
| Monocypher (docs/45 stage B update signatures, later) | host | CC0-1.0 or BSD-2-Clause (docs/45) | a notice under BSD |
| CMake, Ninja | not shipped | | |
| Route A' only: mingw-w64 CRT, headers, winpthreads | toolchain, and linked into the exe | permissive notices (the mingw-w64 notice, getopt, gdtoa, Sun math, Rich Felker's MIT code); Wine-derived headers and IDL under LGPL-2.1+; Cephes math marked "FIXME" for permission (read, `COPYING.MinGW-w64-runtime.txt`) | ship that file with the toolchain. Binaries that link the runtime must carry its notices (read). Ours are built on the player's PC and never distributed. |
| Route A' only: libc++, libc++abi, libunwind, compiler-rt | toolchain, and linked into the exe | Apache-2.0 with LLVM exception (inferred) | ship the licence with the toolchain |
| Route A' only: llvm-mingw's scripts | not shipped; we ship only the built toolchain | ISC; "the built toolchain is covered by the licenses of the individual external projects" (read, its LICENSE.txt) | |

The game module or exe built on the player's PC is never distributed by us, so no licence notice travels with it (inferred).

## 12. Risks and open questions

- **The decrypt step.** Under docs/51's Plan L, the player decrypts 27 files in five folders once, with a tool they already have. It is the one manual step left, and the likeliest place for players to give up. A clear checklist, a guide and (c+) are the mitigations (section 3).
- **Firmware input.** Today libsre LLE needs the firmware's `libsre.sprx` (README; LIBSRE_LLE is a run default, docs/45). docs/51 recommends our own HLE for its eight functions (9 to 15 days), which removes the firmware from setup. Until then, setup takes a decrypted `libsre.prx`.
- **libsre version, while LLE stays.** The TOC constant fits one firmware build (docs/45, docs/51). Read the TOC from `lift_prx`, and check libsre's hash against a table of known firmware versions.
- **ABI churn (Route B).** Runtime work often touches the inline accessors and the context layout (perf history in docs/45 and the memory notes). Batch such changes into releases, because each one makes every player rebuild.
- **Antivirus.** Covered in docs/45. In addition, check the toolchain's hashes at every start, to catch a quarantined `clang.exe` early.
- **Determinism** (docs/45 R5, R6) decides whether the published DLL hash works.
- **Freestanding gaps.** Mostly settled by section 7.8; T2 confirms them.
- **The `__try` side finding** (section 7.9): confirmed by T3. The guard catches nothing under today's flags; the fix is a `VirtualQuery` bound.
- **Open.** Does an embeddable-Python worker process start correctly (T4)? How big does a full-game shader cache get (52 MB measured after three areas; section 10.2)? Does the official LLVM release link third-party libraries statically, for the notices? What are the 26 decrypted `.sdat` hashes for the table (docs/51 section 10)?

## 13. Trial builds and run requests

T2, T3 and T4 were run on 2026-10-19 (results below the table). T1 was not run: it needs the llvm-mingw download, which is not OK'd.

| # | What | Needs | Expect |
|---|---|---|---|
| T1 | MinGW build of ps3recomp runtime and libs, port host code, UI, RmlUi and FreeType: llvm-mingw for x86_64-w64-mingw32, `-fms-extensions`, through the slot lock; count errors per file | **the llvm-mingw 20261006 ucrt-x86_64 zip, 190,836,485 bytes: needs your OK**; only if Route A' is chosen or wanted as a hedge | most categories in 7.5 compile; the list of real fixes |
| T2 | Freestanding compile of chunk 013 (the smallest, 5.7 MB): `clang --target=x86_64-pc-windows-msvc -c -O2 -fwrapv -msse4.1 -ffreestanding -nostdlibinc -fno-stack-protector -fno-exceptions -fno-math-errno -DDS_NO_SHADOW_STACK`, a forced-include file defining `PPU_FENCE`, and shim `string.h`/`math.h`/`stdio.h`; then `llvm-nm -u` | the installed LLVM 22.1.8 and one build slot; no download | outside symbols: docs/45's list minus the thread-locals |
| T3 | The SEH test (section 7.9): a fault inside `__try` in a 10-line program, built with clang-cl | installed LLVM; no download | shows whether today's profiler guard works |
| T4 | Run `ppu_loader` and `ppu_lifter -j 4` under the embeddable Python, from a folder with a non-ASCII name | **the Python 3.13.9 embeddable zip, 10,926,675 bytes: needs your OK** | the workers start; `sys.path` comes from `._pth` |

### 13.1 T3 results (2026-10-19)

A 60-line program (`seh_test.cpp`) maps one committed page followed by a reserved page and scans 512 slots from 64 bytes below the boundary, the way `samp_stack_guest` scans a stack. The 9th load faults. Modes: `direct` puts the load in the `__try` body, as `samp_stack_guest` does; `call` puts it in a non-inlined function; `vq` bounds the scan with `VirtualQuery` and has no `__try`. clang-cl 22.1.8, with the flags today's build gives `ppu_loader.cpp` except `/Zi` and the warning switches.

| Build | direct | call | vq |
|---|---|---|---|
| `/O2 /EHsc` (today's flags) | access violation, process dies | access violation, process dies | ok (8 slots scanned) |
| `/O2 /EHa` | caught | caught | ok |
| `/Od /EHsc` | access violation, process dies | caught | ok |

Every object references `__C_specific_handler`. What it means: section 7.9.

### 13.2 T2 results (2026-10-19)

Chunk 013 of master's lift, compiled with clang 22.1.8 and T2's flags (`-nostdlibinc`, the shims, `force.h`), one job, compared with the object today's clang-cl `/O2` build made from the same source (`llvm-nm -u`, `llvm-objdump -h`).

| Variant | Result | `.text` | Outside symbols |
|---|---|---|---|
| T2's flags as written (`-ffreestanding`) | 2 errors | | |
| The same plus `-include stddef.h` | builds, 1.9 s | 15,285 B (+15 %) | today's 11, plus `memcpy` |
| The same plus the shims mapping `memcpy`, `sqrt` and the rest to `__builtin_*` | builds, 2.0 s | 14,116 B (+6 %) | exactly today's 11 |
| Hosted (no `-ffreestanding`), no system headers | 1 fatal error | | |
| Today's build (reference) | | 13,275 B | 11 |

- **The two errors are both `NULL`**, in the function table's end entry `{ 0, NULL, NULL }`. Clang's own `stddef.h` defines it, but nothing in the chunk includes it; the MSVC build gets it through the CRT headers. Either the freestanding module force-includes `stddef.h`, or the lifter writes `nullptr` there (a one-line change). [measured]
- **Hosted mode does not work without system headers:** clang's `intrin.h` includes `mm_malloc.h`, which includes `stdlib.h`. `-ffreestanding` skips that include. [measured]
- **`-ffreestanding` turns builtins off**, so every `memcpy` in the lift becomes a real call (the one extra outside symbol here). The builtin shims bring the object back to today's symbol list. Route B needs them. [measured]
- **The outside symbols, all 11:** the thread-local machinery (`_tls_index`, `__tls_guard`, `__dyn_tls_on_demand_init`, from the thread-local `g_trampoline_fn`), `g_trampoline_fn`, `vm_base`, `vm_read64`, `vm_write64`, `g_vm_inl_rd_lim`, `g_vm_inl_wr_lim`, `g_spu_coh_bitmap` and `g_spu_coh_armed_line`, plus the 93,945 `func_*` entries of the function table. So the thread-locals are still referenced: the expected list (docs/45 minus the thread-locals) holds only once `g_trampoline_fn` stops being `thread_local` or moves behind an accessor. [measured]
- The size difference that remains (+6 %) was not looked into. Chunk 013 is mostly the function table (72 functions), so it says little about code size; a code-heavy chunk is the better size check. [inferred]

### 13.3 T4 results (2026-10-19)

The embeddable Python 3.13.9 zip (10,926,675 bytes; `python.exe`, `pythonw.exe` and `python313.dll` signed by the Python Software Foundation, `vcruntime140.dll` by Microsoft), extracted to a folder named `Pythön-テスト`. The tools ran from a copy in `wörk-テスト` (ps3recomp at c779320, the commit master's lift came from, and master's `insn_hooks.txt`), with every path argument absolute, as a launcher would pass them. Output went to a pipe, as a launcher would read it.

| Step | Plain `python.exe` | With `-X utf8` |
|---|---|---|
| Interpreter | isolated, environment ignored, no `site`; `sys.path` = `python313.zip` and Python's folder | |
| `ppu_loader` | its 4 outputs written, byte-identical to the system Python's and to `work/loader`; then exit 1 printing a path | |
| `ppu_lifter -j 4` | 15 files written, then exit 1 printing a path | exit 0 in 47.1 s; 39 hooks, 19,166 call targets |
| `gen_hle_nids --all` | its output written (identical to master's), then exit 1 printing a path | |
| `apply_boot_patches` | exit 0, 1 patch applied | |

- **The `-j 4` workers start** under the embeddable Python (docs/45 question 13). [measured]
- **The script's folder is not on `sys.path`** with the `._pth` file (docs/45 question 12). Each lifter inserts its own folder, so they work anyway. [measured]
- **Printing a path with characters outside the code page ends the script with an error** when its output goes to a pipe: standard output is cp1252 and `print` raises `UnicodeEncodeError`. The files are written by then, but the exit code is 1. `PYTHONUTF8` would not help, because the environment is ignored; `-X utf8` fixes it. [measured]
- **The lift is the same, but not byte for byte.** Against `work/recomp_master`, `ppu_recomp.h` and `hle_nid_table.cpp` are identical. The 14 chunks are the same text in another encoding: the lifter opens its outputs with no encoding, so they are written in the system code page (cp1252 here) unless Python runs in UTF-8 mode, and three comments it writes have an em dash (the header, `mtfsf` and `tw` traps: 1 to 23 a chunk). Under `-X utf8` the chunks are UTF-8. [measured]
- **So a plain `python` lift fails on Japanese and Korean Windows.** Python's cp932 and cp949 codecs cannot encode U+2014, so the first chunk write would raise `UnicodeEncodeError` there (Western, Cyrillic, Chinese and other ANSI code pages can encode it). The fix is small: ASCII in the lifter's comments and `encoding="utf-8"` on its writes. That changes every chunk's bytes once, so the next lift rebuilds all 14 chunks once. [measured for the codecs; inferred for a Japanese or Korean PC]
- **For the launcher:** start every script with `python.exe -X utf8`. It makes the prints safe and the outputs UTF-8 whatever the code page.

The run request for the shader cache size is done: the main session measured it without a game run, and the numbers are in section 10.2.

## 14. Sources

Repository (read): `README.md`; `port/CMakeLists.txt`, `port/ui/CMakeLists.txt`, `port/ui/src/crash_report.cpp`, `port/ui/src/number_button.cpp` (origin/master); `scripts/build_libsre.sh`, `scripts/build_spu.py`; ps3recomp fork/master f15a4f0: `CMakeLists.txt`, `tools/ppu_lifter.py`, `runtime/ppu/ppu_loader.cpp`, `runtime/spu/spu_context.h`, `spu_helpers.h`, `spu_dma.h`, `spu_coherency.c`, `spu_lockstep.c`, `libs/video/rsx_d3d12_backend.c`, `rsx_live_draw.c`, `libs/audio/cellAudio.c`, `libs/codec/sail_movie.cpp`, `libs/input/pad_hid_sony.c`, `third_party/at3_standalone/README.txt`; the lift `work/recomp_l2` (include lines and identifier counts only); docs/05, 27, 30, 31, 32, 45, 49, and docs/51 as it stood at 19:11 on 2026-10-18.

FFCCEOT-Recomp (read): `docs/PACKAGING.md`, `game/src/setup.hpp`, `game/src/setup.cpp`, `game/src/game_module.cpp`, `game/src/process.cpp`, `game/src/ui/rom_setup.cpp`, `game/src/ui/prelaunch.cpp`, `runtime/include/ntr/recomp.h`.

Web (read):
- llvm-mingw releases and README, LICENSE.txt: https://github.com/mstorsjo/llvm-mingw (release 20261006 via https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest)
- mingw-w64 sources: https://github.com/mingw-w64/mingw-w64. Files: `mingw-w64-headers/include/d3d12.idl`, `d3d12sdklayers.idl`, `dxgi1_6.idl`, `d3d11on12.idl`, `mftransform.idl`, `hidsdi.h`; `mingw-w64-crt/lib-common/d3d12.def`, `d3dcompiler_47.def`, `xinput1_4.def`, `mfplat.def`, `avrt.def`, `wtsapi32.def`, `hid.def`, `setupapi.def`; `mingw-w64-crt/libsrc/dxguid.c`, `mfuuid.c`; `mingw-w64-crt/crt/crtexe.c`; `COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt`
- MSYS2 package pages: https://packages.msys2.org/packages/mingw-w64-clang-x86_64-headers, `-crt`, `-libc++`, `-clang`, `-lld`, `-llvm-libs`
- Microsoft, Deployment in Microsoft C++: https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp
- Microsoft, Redistribute Visual C++ Files: https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files
- Ninja releases: https://github.com/ninja-build/ninja/releases (v1.13.2)
- CMake releases: https://github.com/Kitware/CMake/releases (v4.4.4)
