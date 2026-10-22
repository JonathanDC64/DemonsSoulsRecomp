# 81. Setup core: the game check, pinned downloads, the step journal and the lift

The first part of the player setup (docs/52, and the setup plan page): the parts that do not depend on the two open setup questions, Q2 (a prebuilt host exe, or everything built on the player's PC with llvm-mingw) and Q7 (the defaults: build parts after setup, the Enhanced preset's frame rate, button prompts).

Code: `tools/setup/` on the port branch `feat/setup-core` (local, not pushed). Standard-library Python only, so it runs on the embeddable Python that setup will bring.

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

**The 26 decrypted `.sdat` sums are not measured yet** (docs/51 §10, check 2). They need RPCS3's output, and this PC has only the runtime's own decryption of 13 of them. Until they are in the table, setup checks each file's first bytes: a decrypted file is a DCX container and starts with `DCX\0`. The runtime's 13 decryptions do start that way. The CLI reports these files as "ok?".

The table (`des_setup/game_table.json`) holds sizes and SHA-256 sums only, no bytes of the game. It is generated from docs/51 appendix A.

## 2. Tested on a copy

The copy, in the scratchpad: `PARAM.SFO`, `EBOOT.BIN`, the 26 `.sdat` files, the lift's `EBOOT.ELF`, the runtime's 13 `.sdat.dec` files renamed to `.unedat`, and `libsre.prx`. The data folder was fresh.

| Case | Result |
|---|---|
| The disc's files | all 28 match docs/51's sums |
| Decrypted files | `EBOOT.elf` matches; `libsre.prx` matches; 13 script files found and start with `DCX\0`; 13 reported missing, which is right for this copy |
| No `--decrypted` folder | all 27 decrypted files reported missing, one line |
| `fetch llvm-mingw` | refused: the entry has no SHA-256 yet, and nothing was written |
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

Entries today, both **not pinned yet**:

| Tool | Version | Size | Route |
|---|---|---|---|
| Python, embeddable | 3.13.9 | 10,926,675 bytes | both answers to Q2 |
| llvm-mingw, UCRT x86-64 | 20261006 | 190,836,485 bytes | Q2 = B |

Pinning each needs one download to measure its SHA-256. That needs your OK (decisions list).

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

1. **Compile and link.** This depends on Q2 (which compiler, and whether the host is built here).
2. **The screens.** This also depends on Q2: under A they run in the prebuilt RmlUi host, while under B no host exists until the build ends, so the first screens need another form.
3. **The shaders step, and the end-of-setup cleanup.** The cleanup default is Q7 a.
