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
- **The lifted SPU code includes `../../runtime/spu/...`.** Outside `ps3recomp/work_spu`, the compile step has to put `ps3recomp/runtime/spu` on the include path (its `../..` is ps3recomp).

## 6. Next, in order

1. **The DLL split** (docs/57, local branch `feat/dll-split`, milestone 1 built on 10-19): `des_host.exe` and `des_game.dll`. Its frame-rate check was in profile windows; it needs a plain-window A/B (docs/83) on today's master before a PR.
2. **The game part freestanding** (docs/52 section 7.8): no C runtime, Windows SDK or C++ library in `des_game.dll`, so the player's PC needs only clang, lld-link and clang's headers.
3. **Compile and link in setup**, with the compiler download (section 3).
4. **The screens**, in the prebuilt RmlUi host.
5. **The shaders step.** The build files stay (Q7 a).
