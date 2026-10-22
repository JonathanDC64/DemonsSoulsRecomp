# 81. Setup core: the game check, pinned downloads and the step journal

The first part of the player setup (docs/52, and the setup plan page): the parts that do not depend on the two open setup questions, Q2 (a prebuilt host exe, or everything built on the player's PC with llvm-mingw) and Q7 (the defaults: build parts after setup, the Enhanced preset's frame rate, button prompts).

Code: `tools/setup/` on the port branch `feat/setup-core` (local, not pushed). Standard-library Python only, so it runs on the embeddable Python that setup will bring.

```
cd tools/setup
python -m des_setup check <dump folder> [--decrypted DIR]... [--firmware libsre.prx]
python -m des_setup stage <dump folder> [...] [--data DIR]
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

## 5. Next, in order

1. **The lift steps** as journal steps: the PPU loader and lifter, the NID table, the boot patches, the SPU images and `build_spu.py`, libsre.
   - These need small tooling changes first: path options for `build_spu.py`, which writes into the ps3recomp checkout today, and a Python form of `build_libsre.sh`, since a player's PC has no bash.
   - The flags would come from one shared file instead of the README.
2. **Compile and link.** This depends on Q2 (which compiler, and whether the host is built here).
3. **The screens.** This also depends on Q2: under A they run in the prebuilt RmlUi host, while under B no host exists until the build ends, so the first screens need another form.
4. **The shaders step, and the end-of-setup cleanup.** The cleanup default is Q7 a.
