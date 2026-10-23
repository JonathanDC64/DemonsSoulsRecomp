# 81. Setup core: the game check, pinned downloads, the step journal and the lift

The first part of the player setup (docs/52, and the setup plan page): the game check, pinned downloads, the step journal and the lift.

**Decided (2026-10-22).**
- Q2 = A, a prebuilt host: CI builds the host exe, and the player's PC lifts and compiles only the game part (docs/52's "CI host, game DLL on the player's PC" route). The compiler and every other tool the build needs are downloaded by the host during setup, each pinned by version and SHA-256; none is bundled.
- Q7: a. the build files are kept after setup, because an update needs them to rebuild; b. the Enhanced preset is a fixed preset, with no benchmark; c. button prompts other than the PS3's are left for later.
- The game table and `scripts/spu_table.json` (sums, sizes and offsets only) are committed.
- Later on 10-22: **the compiler download** is a smaller archive that CI makes from LLVM's own 22.1.8 archive (clang, lld-link and clang's headers) and attaches to the release, pinned by SHA-256 like the other downloads.
- **The host exe builds in the Visual C++ runtime and the C++ library** and uses the UCRT that Windows 10 and 11 include, so players need no Visual C++ redistributable. Measured on 10-22 by linking the 254 runtime functions today's exe imports into a test program: +0.14 MB this way, +0.47 MB with the UCRT built in too (plain `/MT`), on a host of 7.4 MB (docs/57). Today's exe links them as DLLs (`/MD`): `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`.

Code: `tools/setup/` (port PR #121). Standard-library Python only, so it runs on the embeddable Python that setup will bring.

```
cd tools/setup
python -m des_setup check <dump folder> [--decrypted DIR]... [--firmware libsre.prx]
python -m des_setup stage <dump folder> [...] [--data DIR]
python -m des_setup lift <dump folder> [...] [--data DIR] [-j N]
python -m des_setup compile [--data DIR] [-j N] [--cc clang-cl --ld lld-link]
python -m des_setup status | tools | fetch NAME
python -m unittest discover -s tests
```

## 1. What it checks

Setup supports one game: **BLUS30443, version 1.00**, checked by hash. It accepts only files the player has already decrypted (docs/51, plan (c)). It never decrypts anything.

1. **The folder.** The folder that holds `PS3_GAME`, or `PS3_GAME` itself.
2. **The title and version**, from `PARAM.SFO`: `TITLE_ID` BLUS30443, `APP_VER` and `VERSION` 01.00.
   - Another ID gets a plain message that names it, for example "the European disc (BLES00932)". The IDs come from docs/51 §5.1.
3. **The disc's own files**, by size and SHA-256: `PARAM.SFO`, `EBOOT.BIN` and the 26 `.sdat` files. This proves the dump is this version and intact.
4. **The decrypted files the build reads**:
   - `EBOOT.elf`, by SHA-256;
   - the 26 `.sdat.unedat` files.

   Setup looks for each one beside its original, where RPCS3's Decrypt PS3 Binaries writes it, and then by name in any folder the player picks. Names match whatever the case (`EBOOT.ELF` or `EBOOT.elf`).
5. **The firmware file**, when given: the decrypted `libsre.prx`, by SHA-256.

A file that is still encrypted gets its own message: it starts with `SCE\0` (a SELF) or `NPD\0` (SDATA). Every problem is one line in plain words that says what to do.

**The 26 decrypted `.sdat` sums** (docs/51 §10, check 2) were measured on 2026-10-22 from RPCS3's own output: a copy of RPCS3 0.0.43-20290 run as `rpcs3.exe --decrypt <file>...` (headless; SDATA needs no key) on copies of the 26 files. It writes `<name>.sdat.unedat` beside each one. All 26 start with `DCX\0`, and the 13 that the runtime also decrypts are byte for byte the same as the runtime's output. A table entry without a sum would still be checked by its first bytes and reported as "ok?"; none is left.

In a folder the player picks, a decrypted file is found by its name at the top, or at its own path under the folder (`chr/...`, `USRDIR/chr/...` or `PS3_GAME/USRDIR/chr/...`), because RPCS3 keeps the game's subfolders when it decrypts a copied folder tree.

The table (`des_setup/game_table.json`) holds sizes and SHA-256 sums only, no bytes of the game. It is generated from docs/51 appendix A.

## 2. Tested on a copy

The copy, outside the repository: `PARAM.SFO`, `EBOOT.BIN`, the 26 `.sdat` files, the lift's `EBOOT.ELF`, the runtime's 13 `.sdat.dec` files renamed to `.unedat`, and `libsre.prx`. The data folder was fresh.

| Case | Result |
|---|---|
| The disc's files | all 28 match docs/51's sums |
| Decrypted files | first run: `EBOOT.elf` and `libsre.prx` match; 13 script files found and start with `DCX\0`; 13 reported missing, which was right for that copy. With RPCS3's 26 files (2026-10-22): all 26 match by sum, "Ready." |
| No `--decrypted` folder | all 27 decrypted files reported missing, one line |
| `fetch llvm-mingw` | refused: the entry had no SHA-256, and nothing was written (the entry is gone since Q2 = A) |
| `fetch python-embed`, pinned | downloaded, size and sum checked, unpacked; a second run reused it with no download |
| `lift -j 8` | the stage step and all 19 lift steps done in 82 s (loader, PPU lift, patches, NID table, the cut, 14 SPU lifts, libsre); it lists the 13 missing `.sdat` files and carries on |
| `lift` again | every step skipped, 0.3 s |

The unit tests (24, on made-up files) cover:

- the title, version and folder cases;
- missing, still-encrypted and mismatched files;
- a damaged original;
- the firmware file;
- the copy step and its resume;
- the journal: skip, rerun and recorded failures;
- the downloads: fetch and reuse, resume, a server that ignores the range, a bad sum (deleted), unpinned and `http://` entries refused, a zip that tries to write outside its folder.

## 3. Pinned downloads (`fetch.py`, `toolchain.json`)

- Every tool is named at one exact version, with its URL, size and SHA-256.
- An entry without a SHA-256 is refused, so nothing unchecked is ever used.
- Downloads are `https://` only. They resume with HTTP Range, go to a `.part` file, and are renamed only once size and sum match. A mismatch is deleted.
- Zips unpack into a folder of their own. Absolute paths, drive letters, `..` and symlinks are refused.
- Everything stays in setup's data folder (`%LOCALAPPDATA%\DemonsSoulsRecomp`). Nothing is installed.

Entries today:

| Tool | Version | Size | State |
|---|---|---|---|
| Python, embeddable | 3.13.9 | 10,926,675 bytes | pinned 2026-10-22: the size and python.org's MD5 checked on the first download |

- llvm-mingw was only for Q2 = B and is out of the table.
- The compiler for the game part is not in the table yet. LLVM 22.1.8, the version the builds use today, is published as `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` (862,053,924 bytes) and as the `LLVM-22.1.8-win64.exe` installer (455,545,840 bytes). An installer would install, so it is out. Setup will download the smaller archive CI makes from the first one (Decided); it is not in the table until CI makes it.

## 4. The journal (`journal.py`)

- Each step records a fingerprint of its inputs: file sums, tool versions and arguments.
- A step that finished with the same fingerprint, and whose outputs are still there, is skipped. So setup resumes where it stopped, and a changed input redoes only the steps that depend on it.
- The journal is one JSON file, written to a temporary file and renamed, so a crash never leaves half of it.

`stage` is the first step built on it. It copies the decrypted inputs into the data folder, so a later rebuild or update needs nothing from the player. It checks each copy's sum.

## 5. The lift (`lift.py`)

`lift` checks and stages, then converts the game's code, each part a journal step. Everything it writes stays in the data folder (`work/`), and each tool's output goes to `setup/logs/<step>.log`.

| Step | Tool | Output |
|---|---|---|
| `loader` | `ppu_loader.py` | `work/loader` |
| `ppu` | `ppu_lifter.py`, with the flags from `scripts/lift_ppu.json` | `work/recomp_raw` |
| `patches` | `apply_boot_patches.py`, on a copy of the raw lift, so a rerun never patches twice | `work/recomp` |
| `nids` | `gen_hle_nids.py --all` | `work/recomp/hle_nid_table.cpp` |
| `spu-cut` | `build_spu.py --eboot`: the 12 SPU images and the 2 SPURS job blobs, each checked by `scripts/spu_table.json` | `work/spu_images`, `work/spu_jobs` |
| `spu:<name>` | `build_spu.py --only <name>`, one step per image or job | `work/spu/<name>` |
| `libsre` | `build_libsre.py`, only when the firmware file was given | `work/libsre`, `work/gen/libsre_glue.c` |

- **Fingerprints.** A step's fingerprint covers its input sums, one sum over every script that does the lifting (ps3recomp's `tools/`, this tree's `scripts/` and `port/insn_hooks.txt`) and its flags. So a new release redoes the lift, and a lift stopped anywhere resumes at the step it was in. The lifter's worker count is not part of it.
- **UTF-8.** The tools run as `python -X utf8`, because the lifter opens its outputs without an encoding (docs/52).
- **Progress.** The lifter's `lifted n/m functions` lines and the SPU step count come out as `@progress <step> n/m` lines, for the screens to read.
- **Tooling it needed** (commit `57f426d`, its own commit):
  - `build_spu.py --eboot` cuts the SPU programs itself; the job blobs had to be cut by hand before. It also gained path options, `--only` and `-j`. Without options it does what it did.
  - `build_libsre.py`, the Python form of `build_libsre.sh` (which now runs it), takes the TOC from `lift_prx.py`'s output instead of a constant.
  - `scripts/lift_ppu.json` holds the PPU lifter's flags. A test checks the README's command against it.
- **Tested** on made-up trees (9 tests), then on the copy (section 2). Against a developer's `work/` from 10-19: the loader output, the 12 SPU images, the 2 job blobs and libsre's tables and image are byte for byte the same, and so are 30 of the 31 files of the SPU lifts the developer builds use. Every other difference comes from tonight's ps3recomp being newer than that lift (the condition-register fields, ASCII comments, 80 more NIDs), plus one generated header whose dash is UTF-8 under `-X utf8` and cp1252 without it.
- **Against the README's commands with the same ps3recomp** (step 3 as written: no `-X utf8`, the lifter at `-j 16` against setup's `-j 8`, on the same `EBOOT.ELF`): the whole PPU lift (16 files, 436 MB), the loader output, the 12 SPU images, the 2 job blobs, all 6 libsre files and 30 of the 31 SPU lift files are byte for byte the same. The two that differ: the SPU registration's header comment, whose dash `-X utf8` writes as UTF-8 and a Windows console default as cp1252; and the libsre glue's image path (the next point). So the worker count does not change the lift, and setup makes what a developer makes.
- **The libsre glue names the data folder.** `--runtime-image` is an absolute path into it, built into the game, so moving the data folder after setup breaks the game's libsre load. Either the runtime looks for the image beside the exe or in the data folder it is told at start, or setup reruns the glue step after a move. To decide with the compile step.
- **The lifted SPU code includes `../../runtime/spu/...`.** Outside `ps3recomp/work_spu` it still resolves, through the `ps3recomp/runtime/ppu` include folder the commands already have (section 7, measured).

## 6. Next, in order

1. **The DLL split** (docs/57, local branch `feat/dll-split`, milestone 1 built on 10-19): `des_host.exe` and `des_game.dll`. Its frame-rate check was in profile windows; it needs a plain-window A/B (docs/83) on today's master before a PR.
2. **The game part freestanding** (docs/52 section 7.8): no C runtime, Windows SDK or C++ library in `des_game.dll`, so the player's PC needs only clang, lld-link and clang's headers.
3. **Compile and link in setup**, with the compiler download (section 3). Done for milestone 1's commands on 10-22: section 7.
4. **The screens**, in the prebuilt RmlUi host.
5. **The shaders step.** The build files stay (Q7 a).

## 7. The compile (`compile.py`, 10-22)

`compile` builds `des_game.dll` from the lift, each part a journal step. Everything it writes stays in the data folder, and each tool's output goes to `setup/logs/<step>.log`.

| Step | What | Output |
|---|---|---|
| `gen` | `des_game_title_funcs.h` from `scripts/lift_ppu.json` and `port/insn_hooks.txt`, and the SPU workload table without its static constructor, made as `port/CMakeLists.txt` makes them for `DES_SPLIT` | `build/gen` |
| `cc:<name>` | one per object (33): the 14 PPU chunks, the 12 SPU images and 2 jobs, the lifted libsre and its glue, the workload table, `game_module.c` and `des_game_module.cpp` | `build/obj/<name>.obj` |
| `link` | the linker | `game/des_game.dll` |

- **CMake's commands, kept as data.**
  - `scripts/game_build_desc.py` reads `des_game`'s commands from a split build (ninja's compile database and its link command). It writes `tools/setup/des_setup/game_build.json`: one command per kind of object, with placeholders for the folders. `{src}` is the source tree, which the install mirrors; `{work}` is the lift; `{gen}` is `build/gen`. The compiler and the linker are passed in.
  - It checks three things: every command expands back to CMake's exactly, no machine path is left, and all sources of one kind share one command.
  - The compiler is `clang.exe --driver-mode=cl` from the `llvm-win64` download (docs/86), which has no `clang-cl.exe`; `--cc` and `--ld` name other ones.
  - `--check` compares the build with the file. `--check --commands-only` needs only a configured build, so CI can run it with placeholder lift files.
- **What the install needs.** The description lists the source-tree headers the compile reads, from ninja's recorded dependencies. Today that is 9 headers, in `ps3recomp/include/ps3emu` and `ps3recomp/runtime/{prx,spu}`. The install also needs `game_module.c`, `des_game_module.cpp`, `lift_ppu.json` and `insn_hooks.txt`. If a header is missing, setup stops and lists it.
- **The lift's relative includes work as they are.**
  - The SPU code includes `../../runtime/spu/...`, and the libsre glue includes `../../ps3recomp/runtime/prx/...`. From setup's `work/`, neither is found beside the source, so clang tries each include folder in turn.
  - The SPU include resolves under `{src}/ps3recomp/runtime/ppu`, and the glue's under `{src}/port/compat`. Both folders are already on the commands.
  - So the commands need no change. This is measured: the objects are identical (below). It corrects section 5's last point, which was inferred.
- **Fingerprints.**
  - An object's fingerprint covers its command, the compiler, its source's sum and the sums of every header it read last time. The compiler is identified by the archive it was unpacked from, or by its exe's sum. The headers come from clang's dependency file (`-clang:-MD -clang:-MF`). So a changed header rebuilds only the objects that read it.
  - The link's fingerprint covers its command, the linker and the objects' fingerprints.
- **Jobs** (as first built; section 8 replaced the count and the retry on 10-23).
  - The largest sources start first.
  - The job count follows docs/52 4.4: the smaller of the CPU count and (available memory - 2 GB) / 3 GB, and at least 1. `-j` overrides it.
  - A job that runs out of memory runs once more with one job fewer. This is detected from the message, or from exit status 0xC0000017 or 0xC000012D.
  - After a failure, no new object starts. The next run resumes.
- **The journal** can now run steps from several threads: it takes a lock, and each step's work runs outside it. It can also record a fingerprint that is known only after the step has run (the headers).
- **Tested.**
  - 17 unit tests, with a fake compiler and linker, cover these cases:
    - the plan, and a build followed by a run with nothing to do;
    - a changed header rebuilds only its readers, and a changed hook table rebuilds only the module;
    - a failure, then a resume;
    - the out-of-memory retry;
    - a missing install header, and a missing lift;
    - the generator's command splitting and folder mapping.
  - On the real lift (10-22), in a data folder laid out as setup's, with clang-cl 22.1.8: 20 objects are byte for byte CMake's objects, apart from the time stamp. They are the smallest PPU chunk, the 12 SPU images, the 2 jobs, libsre, the glue, the workload table and both module files. With 1 job they took 86 s (only 3.6 GB of memory was available). A second run compiled nothing.
  - The link took 2 s, with the other 13 chunks taken from the CMake build. The DLL's `.text`, `.rdata`, `.data`, `.pdata`, `.tls` and `.reloc` are identical to CMake's `des_game.dll`. CMake's has one more section, `.rsrc`: the 364-byte default manifest that `cmake -E vs_link_dll` embeds. Windows does not use it for a DLL.
  - `clang.exe --driver-mode=cl` gives the same objects as `clang-cl.exe` (measured on an SPU image, the glue and `des_game_module.cpp`).
  - Not run: all 14 chunks compiled by setup. That is about 35 minutes with 1 job, and the PC was short of memory.
- **Today's description is milestone 1's.**
  - Its commands use the Visual C++ headers and the Windows SDK, and the link takes the C runtime and Windows' import libraries. So it builds only on a PC with Visual Studio's build tools.
  - Milestone 2 (docs/57 6.2, docs/52 7.8) changes the commands. The description is then written again from that build, and `compile.py` stays as it is.
- **Open.**
  - **Where the host finds the DLL.** Today the host loads the file named by `PS3_GAME_MODULE`, or else `des_game.dll` beside the exe (`game_host.c`). Setup writes the DLL to the data folder's `game/`. The screens step decides whether the launcher sets `PS3_GAME_MODULE` or the host looks in the data folder.
  - **The glue's absolute libsre image path** (section 5). This goes with milestone 2, which moves the glue's file reads into the host anyway.
  - **A CI job** that runs `game_build_desc.py --check --commands-only` on a configured split build, so the description cannot drift from CMake. It goes with the CI host build.

## 8. Jobs by memory (`compile.py`, 10-23)

The compile no longer fixes its job count at the start (section 7). Each job starts only when the memory holds it, so a PC with little memory free compiles fewer at a time, and one with plenty runs more, as the memory changes during the compile.

- **The rule.**
  - At most one job a logical processor.
  - A job starts while this is at least its expected peak: the memory free now, less 2 GB for the rest of the PC, less what the running jobs are still expected to take (each one's expected peak less what it holds now).
  - When nothing runs, the next job starts anyway. A PC short of memory still compiles, one job at a time.
  - The biggest start first: by expected peak, then by source size. A smaller job that fits starts beside them.
  - `-j N` runs N at a time, whatever the memory (for developers).
- **A job's expected peak:**
  - the peak measured the last time that object compiled, with 10% room (the journal keeps it in the object's step, `results.peak`);
  - else its kind's figure, from docs/53 section 2 with some room: a PPU chunk 2.6 GB, libsre 2 GB, an SPU program 0.5 GB, the rest 0.5 GB;
  - raised to the largest peak measured so far in this run among its kind, with 10% room, when that is more. A smaller one does not lower it: on the first real run, chunk 012 peaked at 0.86 GB in 5 s, and when the others were expected to take as little, a third 2.2 GB chunk started and the free memory fell to 1.9 GB.
- **Measuring a peak.**
  - Windows: the compiler's peak commit (`PeakPagefileUsage`, from `K32GetProcessMemoryInfo`), read once it has ended, while its handle is still open.
  - Linux: `VmHWM` from `/proc/<pid>/status`, sampled every half second while it runs, since it is gone once the process ends.
- **Out of memory.** A job that runs out of memory runs once more with nothing beside it: no new job starts while it waits, and it starts once the running ones have ended. If it fails again, the compile stops as in section 7.
- **Free memory** is the physical memory available, or on Windows the commit the system can still give when that is less (a small page file). When it cannot be read, half the processors run, as the first rule did.
- **The hint.** When memory has held jobs back for 5 s, the engine prints `@memory short`, and the note under the bar reads "Closing other programs makes this faster." `@memory ok` puts the usual note back. The protocol stays version 2, because an older host reads an `@` line it does not know as a plain line (`setup_protocol.hpp`).
- **The progress** counts source bytes, as docs/52 4.3 has it, not objects; the screen shows it in MB.
- **The time estimate** on the start screen (`setup_pc.cpp`) counts the PPU chunks that fit in the memory free now, with the compile's figures (2 GB kept, 2.6 GB a chunk). It counted the PC's whole memory before, and promised 9 jobs on a PC that ran 1.
- **Tested.**
  - 10 unit tests (`TestMemory` in `test_compile.py`), with a stand-in for the free memory and a fake compiler that takes the time it is told:
    - short memory runs one job at a time; small jobs start beside a big one; plenty of memory runs several side by side;
    - memory that cannot be read runs half the processors; `-j` ignores the memory;
    - the hint comes once and goes at the end;
    - the measured peak is kept and used next time (Windows); a kind's figure is raised but not lowered by what others of its kind took;
    - an out-of-memory job runs again alone;
    - the progress counts source bytes.
  - One more checks that the start screen's estimate uses the compile's figures, and one the `@memory` line.
  - A real setup from a fresh data folder (10-23, the screens and the engine from this branch, the local clang-cl 22.1.8, 16 threads and 31 GB with the usual programs open):
    - 8.6 GB was free at the start, and the setup screens' own process took 1.6 GB of it (mostly the RSX backend's draw buffers, which the screens never use), so the compile began with about 7 GB. The first rule would have run one job there: (7 - 2) / 3.
    - The whole run took 1,046 s and the compile 976 s. The 10-22 run, one job at about 5 GB free, took 1,602 s and 1,533 s. The chunks took the same time each in both runs (008: 158 s, 009: 143 and 149 s), so the gain is the jobs side by side: two chunks at once for most of the compile, the SPU programs and libsre beside them, at most 7 jobs and 1.5 on average.
    - Peaks: the PPU chunks 2.2 to 2.68 GB (008 the largest), libsre 1.79 GB, the SPU programs 0.09 to 0.26 GB, chunk 012 0.86 GB and 013 0.51 GB. The free memory never fell below 2.8 GB. The hint showed under the bar.
    - A second fresh run, with the shaders phase merged in locally and the progress by source bytes (8.7 GB free at the start): the compile 891 s, at most 4 jobs and 1.7 on average, the whole run 986 s.
    - Two faults came out of these runs. A first run, stopped, showed a small chunk lowering the others' expected peak (fixed before the run above, see above). The run above showed the progress counting objects, so the bar read 78% and "About a minute left" with nine minutes to go; it now counts source bytes (the timing does not change).
