# 51 — Legal decryption: keys, firmware and the player's own files (research, 2026-10-18)

Research only. Nothing was built, lifted or run, and no game was started. **This is research, not legal advice.** Section 8 lists the questions where a lawyer's opinion matters.

Three labels are used below:
- **(read)**: seen in our code, our docs, another project's code or docs, or a web page (linked in Sources).
- **(measured)**: computed here from files that already existed (hashes, sizes, file headers, git history), or read from a server's reply.
- **(inferred)**: my conclusion; not checked.

No key appears in this doc. Keys are named only by the names that our code or other projects give them. The GitHub owner of our repositories is written `<owner>`.

## Summary

- **What needs decrypting** (measured, read). Three inputs:
  - `EBOOT.BIN`, a retail SELF, needed for the lift and again at run time.
  - The 26 `.sdat` files (the game's scripts). Our runtime decrypts them today.
  - The firmware's `libsre.sprx`, which the port lifts. Only eight SPURS queue functions run from it.

  No other firmware file is used.
- **Already exposed** (measured, read):
  - The fork's runtime compiles in the published SDATA/EDAT/NPDRM keys, and its tools hold two PKG keys.
  - Its public history serves six decrypted firmware PRXs. Upstream committed them in 2026-06 and untracked them in 2026-10 without rewriting history.
  - The runtime writes decrypted `.sdat.dec` files into the player's dump.
  - The port repository itself holds no keys and no game files.
- **What others do** (read):
  - RPCS3 carries the PS3 keys and unpacks `PS3UPDAT.PUP` with them. A PUP alone is useless without such keys. No Sony action against RPCS3 was found.
  - The closest PS3 recompilations, AC4-Port and ACFA-Port, went keyless in 2026-10. They run the player's own RPCS3 once at setup.
- **The law** (read):
  - Decrypting counts as "circumvention" under US law, and trafficking in circumvention tools is banned in the US, the EU, the UK and Canada.
  - Nintendo's yuzu consent judgment (2024) and the *Keighin* default judgment (2025) treated software that works only with unauthorized keys, and links to it, as trafficking, even though users supplied the keys. Neither was a contested ruling.
  - No exemption covers decrypting your own game to play it on a PC.
  - Sony owns both the platform and Demon's Souls, and it sent takedown notices over FromSoftware fan projects in 2025.
- **Recommendation: Plan L** (inferred; section 6):
  - (d): replace the eight libsre functions with our own HLE, about 9 to 15 working days. Then no firmware and no PUP are needed.
  - (c): ship no keys and no decryption code. Setup takes the player's already-decrypted `EBOOT.elf` and 26 `.sdat.unedat` files and checks them by SHA-256.
  - Also compile the inherited keys out of release builds, never write into the dump, and keep the port free.
- **The cost.** The player must decrypt once with a tool they already have (today, RPCS3). That breaks the "no RPCS3" part of the goal. Removing it means shipping keys (Plan P), which only a lawyer should approve.
- **Where to go next.** Decisions for the user are in section 7, questions for a lawyer in section 8, and three checks that need no game run in section 10.

## 1. What the port decrypts today, and what our code already contains

### 1.1 The inputs

| Input | Protection | How the port gets it today | Needed for |
|---|---|---|---|
| `PS3_GAME/USRDIR/EBOOT.BIN`, 28,224,840 bytes | Retail SELF: header type 1, key revision 0x0001, program type APP, auth ID 0x1010000001000003 (measured) | Decrypted once with RPCS3 (Utilities > Decrypt PS3 Binaries) to `work/game/EBOOT.ELF`, 28,222,408 bytes, ELF64 big-endian PowerPC (read, README; measured) | The lift, and run time: `des_boot` takes the ELF's path as its argument and copies its segments into guest memory at boot (read, `tools/des/desrun.sh`; `ppu_load_elf` in `runtime/ppu/ppu_loader.cpp`). |
| 26 `*.sdat` files in `USRDIR/chr`, `menu`, `script` and `script/talk`, 1,411,472 bytes in all | NPD containers, version 2, flags 0x0100000C (the SDATA form), 16 KB blocks, empty content ID (measured). SDATA is AES-encrypted under fixed platform keys, with no per-title licence (read, `sdata_decrypt.h`, `edat.h`). | Decrypted by our runtime the first time the game opens each file. `edat_resolve` writes `<name>.sdat.dec` beside the original, inside the player's disc folder (read, `libs/filesystem/edat.c`). This PC's dump holds 13 such files (measured). | Run time. They hold each level's event scripts (ESD), Lua scripts and talk scripts, and the menu ESD. |
| `libsre.sprx`, from the firmware. This PC's RPCS3 has firmware 4.89 installed: 114,254 bytes (measured). | Retail SELF: key revision 0x001C, program type APP, auth ID 0x1070000040000001, version 4.89 (measured) | `ps3recomp/fw_spu/libsre.prx`, decrypted, 239,344 bytes. Its SHA-256 equals the copy upstream ps3recomp committed on 2026-06-25 (measured). So this checkout's copy arrived through git, not from a decryption of this PC's firmware. Whether the two are the same build is not checked. | The build: lifted into `des_boot` (756 functions, 51.9 MB of C; docs/22, docs/45). Run time: the glue loads the libsre image (section 1.2). |
| The Blu-ray disc encryption | Already removed: the dump is a folder ("JB" format) with `PS3_DISC.SFB` and `PS3_GAME`, and no `PS3_UPDATE` (measured) | Nothing to do | Nothing |

Not needed today (read):
- Any other `dev_flash` file. The runtime serves `/dev_flash` only from `PS3_DEV_FLASH` or `./dev_flash` (`runtime/ppu/ppu_fs.cpp`). Demon's Souls asks for `flashATRAC.pic` (MultiStream), and with that file present the boot hangs, so the default leaves it out (docs/28). It is a plain SPU ELF, not encrypted (measured), but it comes from the firmware.
- The other decrypted PRXs in `ps3recomp/fw_spu/`: libadec, libat3dec, libatrac3multi, libatrac3plus, libatxdec, and the SPU code lifted from them. Nothing in `port/` or `scripts/` uses them. cellAtrac decodes with a host decoder (PPSSPP's extract of FFmpeg's ATRAC3plus decoder, `third_party/at3_standalone`).

### 1.2 How libsre is used (read)

- **Build.** `scripts/build_libsre.sh` runs `lift_prx.py` (base 0x30000000), then `ppu_lifter.py --raw` with TOC 0x30039AB0, a value that belongs to this libsre build, then `gen_libsre_exports.py`, which writes `port/gen/libsre_glue.c`. `port/CMakeLists.txt` compiles the lifted libsre only `if(EXISTS ...)`.
- **Run.** `tools/des/deslib.sh` sets `LIBSRE_LLE=1` and `LIBSRE_LLE_ONLY` to eight NIDs; the exe itself does not (docs/45). The glue loads `work/libsre/libsre_image.bin`, a path relative to the working folder.
- **Only eight functions run as Sony code.** Every other SPURS call stays on ps3recomp's HLE (docs/23, docs/25):

| NID | Function |
|---|---|
| 011EE38B | `_cellSpursLFQueueInitialize` |
| 1656D49F | `cellSpursLFQueueAttachLv2EventQueue` |
| 8A85674D | `_cellSpursLFQueuePushBody` |
| 73E06F91 | `cellSpursLFQueueDetachLv2EventQueue` |
| 082BFB09 | `_cellSpursQueueInitialize` |
| E5443BE7 | `cellSpursQueueAttachLv2EventQueue` |
| 039D70B7 | `cellSpursQueueDetachLv2EventQueue` |
| 91066667 | `cellSpursQueuePopBody` |

- **Why the port switched to libsre for them** (docs/23, docs/25):
  - ps3recomp's HLE for the queue family guesses the layout; its own comments say so ("Whatever holds direction/init, it is not these offsets", `libs/spurs/cellSpurs.c`).
  - `_cellSpursLFQueuePushBody` and `cellSpursQueuePopBody` had no HLE at all. Every push faked success, so the decompression tasks waited forever, and HighGraphics spun in a drain loop.
  - The other side of each queue is the game's own SPU task code, which edits the queue's 128-byte line with GETLLAR/PUTLLC. A PPU side that does not match it exactly hangs or corrupts data. Sony's code matches it by construction.
  - With the eight functions on libsre, all boot assets streamed for the first time, and the boot reached the game's frame loop.
- **They are on the hot path.** HighGraphics spends 73-75% of its time waiting in libsre's queue pop, and every DCX block of a level load goes through the queue push (docs/28).
- **ps3recomp's cellSync LFQueue cannot stand in.** It is a host-side struct (host atomics and a host pointer), not the guest's 128-byte big-endian line (read, `libs/sync/cellSync.h`).
- **RPCS3 also runs libsre as firmware code.** Its library table marks `libsre.sprx` as not HLE, and its HLE versions of these functions are stubs: `_cellSpursLFQueuePushBody` logs a todo, and `_cellSpursQueueInitialize` and `cellSpursQueuePopBody` are unimplemented (read, `rpcs3/Emu/Cell/lv2/sys_prx.cpp`, `rpcs3/Emu/Cell/Modules/cellSpurs.cpp`). Its HLE of the plain cellSync LFQueue is complete (`cellSync.cpp`).
- **A developer-only use.** At configure time `scripts/gen_taskset_policy_blob.py` cuts the taskset policy module (7,744 bytes at libsre address 0x23680) for the `LBP_REAL_POLICY=1` A/B oracle. It stays inert unless both of its files exist.

### 1.3 Keys already in our code (measured)

- **ps3recomp runtime, compiled into `des_boot`:**
  - `libs/filesystem/edat.c`: `EDAT_KEY_0`, `EDAT_KEY_1`, `NP_OMAC_KEY_2`, `SDAT_KEY`, and four klicensee candidates (`NP_PSX_KEY`, `NP_KLIC_FREE`, `NP_PSP_KEY_1`, `NP_PSP_KEY_2`). Added upstream in 2026-09.
  - `runtime/ppu/sdata_decrypt.h`: `SDAT_KEY` and `EDAT_KEY_0`. Added upstream on 2026-07-22.
  - `edat.h` calls them "the published NPDRM constants (the same ones RPCS3, scetool and every EDAT tool carry)". `edat.c` leaves out RAP conversion on purpose, "so nothing in this tree has to carry license-conversion keys" (read).
  - Demon's Souls needs this code for its 26 `.sdat` files. With flags 0x0100000C, the code path uses `SDAT_KEY` and `EDAT_KEY_0` (read, the algorithm comment in `sdata_decrypt.h`; inferred for these files).
- **ps3recomp tools:** `tools/pkg_extract.py` holds `PS3_GPKG_KEY` and `PSP_PKG_KEY`, for PSN package decryption. It comes from upstream, and the port does not use it.
- **Upstream's statements:** its README says the project "does not contain any proprietary Sony code, firmware, encryption keys, or copyrighted material", and CONTRIBUTING.md says never commit "encryption keys" (read). The files above do not match those statements.
- **The port repository:** no keys, and no game or firmware file in the history of any local branch (measured: `git log --all` for binary and game file types, and a search of the source).

### 1.4 Firmware in ps3recomp's git history (measured)

- Upstream commit `02b599d` (2026-06-25) added six decrypted firmware PRXs under `fw_spu/` (libsre, libadec, libat3dec, libatrac3multi, libatrac3plus, libatxdec) and C lifted from them.
- Commit `0952e1c` (2026-10-07, upstream PR #210) untracked them. Its message says "History is not rewritten here."
- `fork/master` contains `02b599d`. So the fork's public history still serves those files.
- GitHub says commits pushed to any repository in a fork network "can be accessible from other repositories in that network", "even after a fork is deleted" (read). Removing them takes a history rewrite plus a request to GitHub Support for cached views, and other people's forks and clones keep their copies (read, GitHub's page on removing sensitive data). While upstream keeps the commits, cleaning only the fork does not remove them from the network (inferred).

## 2. How other projects handle the player's files

Sources for this section are listed at the end. Where a page could not be fetched directly, this says so.

### 2.1 Overview

| Project | The player supplies | Keys in the project | Firmware or BIOS | Game code in the download |
|---|---|---|---|---|
| RPCS3 (PS3 emulator) | A dump: folder, ISO (decrypted, or encrypted with a `.dkey` file), or `.pkg` plus `.rap`. `PS3UPDAT.PUP` from Sony's site. | Yes: SELF keys of every type, NPDRM, EDAT/SDAT, PKG, the firmware package keys and the PUP's HMAC key (read) | The player's PUP. RPCS3 unpacks it with built-in keys (read). | No (an emulator) |
| Upstream ps3recomp | A decrypted ELF (from RPCS3, or a debug fSELF). Their own decrypted firmware for LLE. | Yes, in source: the SDATA/EDAT/NPDRM constants and two PKG keys, although its README says none (section 1.3) | The user's own decrypted PRX (`docs/FIRMWARE_LLE.md`). It tracked decrypted PRXs until 2026-10 (section 1.4). | No; the lift is local |
| AC4-Port, ACFA-Port (PS3 recompilations) | The disc folder, their own RPCS3 "used once during setup", and `PS3UPDAT.PUP` 4.93 | None: "contains no game data, no game code and no keys, and it decrypts nothing" (read) | The player's 4.93, through the player's RPCS3. 10 modules run as LLE. | No. Built on the player's PC; the toolchain is downloaded at setup. |
| FFCCEOT-Recomp (a DS recompilation by the same owner; local checkout) | A ROM, checked by SHA-1. For the optional Wii cutscenes, a Wii disc dump and the Wii common key, typed in by the player. | None. The typed key is kept in memory only (read, `docs/LEGAL.md`, `docs/WII_ASSETS.md`). | None: the DS BIOS calls are HLE (read, `docs/ARCHITECTURE.md`) | No. Built on the player's PC with the bundled compiler. |
| Unleashed Recompiled (Xbox 360) | The game (disc image, store package or folder), its title update, and optional DLC. XXH3 hash checks. | Yes: the XEX retail key in `XenonUtils/xex.h`, used at install time. It ships in the release exe (inferred). | None | Yes: the recompiled code and the translated shaders are in the exe |
| Zelda64Recomp (N64) | The US ROM, in any byte order, checked by XXH3 | None needed: N64 ROMs are not encrypted | None | Yes: the recompiled code is in the exe |
| Dolphin | Discs and ROMs | Yes: the Wii common keys, the SD key and a default console key (`IOSC.cpp`) | None needed | No |
| Cemu | Games. `keys.txt` for `.wud`/`.wux` images. | No: "Since these keys are copyrighted we can't provide them". The `.wua` format needs none. | Not needed | No |
| yuzu, Ryujinx | `prod.keys` and `title.keys`, dumped from their own Switch with Lockpick_RCM, plus firmware | No | Dumped by the player | No |
| Vita3K | Firmware packages "from the official PlayStation website", and zRIF or `work.bin` licences | Yes: SELF and NPDRM keys to unpack the firmware, and PKG keys | The official firmware | No |
| shadPS4 | Decrypted dumps from a jailbroken PS4, and modules "dumped from your legally owned PlayStation 4 console" | No | Dumped by the player | No |
| Xenia | XEX files | Yes: retail and devkit XEX keys | Not needed | No |
| PCSX2, DuckStation | A BIOS dumped from their own console | No | The player's BIOS | No |

All rows are read from the projects' code or docs, except where marked inferred.

### 2.2 RPCS3

- **Keys** (read, `Crypto/key_vault.h` and `.cpp`):
  - SELF keys come in types: LV0, LV1, LV2, APP, ISO, LDR, UNK7 and NPDRM.
  - APP and NPDRM keys are chosen by key revision, LV1/LV2 by firmware version range, and ISO by both.
  - So APP keys belong to a revision, not to one game.
  - The vault also names PKG keys, NP and RAP constants, the EDAT/SDAT keys, the firmware package keys (`SCEPKG_ERK`, `SCEPKG_RIV`) and `PUP_KEY`.
- **Firmware install** (read, `Loader/PUP.cpp`, `Loader/TAR.cpp`, `main_window.cpp`):
  1. `PUP_KEY` is only used for an HMAC check of each PUP entry.
  2. RPCS3 keeps the `dev_flash_` entries of `update_files.tar`.
  3. It decrypts each one with the built-in firmware package keys and extracts it into `/dev_flash`.
  - The SPRX files stay encrypted on disk. Each one is decrypted in memory when loaded (`decrypt_self` in `sys_prx.cpp` and `PPUModule.cpp`). If that fails, RPCS3 falls back to its HLE version.
  - So a PUP alone gives nothing usable without RPCS3's keys (inferred).
- **Firmware is required** (read):
  - The Quickstart says "RPCS3 requires the PlayStation 3 system software, contained in PS3UPDAT.PUP" and links Sony's PS3 system software page.
  - Booting without it fails with "Firmware is missing." whenever a library runs as LLE or has no HLE.
  - On 2026-07-16 RPCS3 announced that cellSysmodule is now fully HLE: "You will be able to play PS3 games WITHOUT installing the PS3 firmware in the future!" (read, as reported by MuyComputer).
  - RPCS3 itself still runs libsre as firmware code (section 1.2).
- **Disc games** (read):
  - The Quickstart offers two ways to dump: a PS3 with custom firmware and file-manager homebrew, or a PC with a compatible Blu-ray drive and the PS3 Disc Dumper.
  - RPCS3 has booted ISO files since 2026-01. Encrypted ISOs have worked since 2026-04, with a `.dkey` or `.key` file beside the image or in RPCS3's `data/redump/` folder.
  - It can play from a Blu-ray drive since 2026-04 (Windows) and 2026-08 (other systems).
- **Store games** (read):
  - `.pkg` files install with the built-in PKG keys.
  - Licensed content needs `exdata/<CONTENT_ID>.rap` at boot. Free titles use the free klicensee.
  - SDATA uses `SDAT_KEY` and needs no licence.
  - No `act.dat` or `.rif` handling was found.
- **Decrypt PS3 Binaries and `--decrypt`** (read, `decrypt_binaries.cpp`, `rpcs3.cpp`):
  - It takes BIN, SELF, SPRX, SDAT and EDAT files. Output names are in section 4 (c).
  - `--decrypt` runs without a window and asks on the console for any KLIC it needs. An empty line ends the prompt.
  - Qt's parser collects repeated options, and AC4-Port passes one `--decrypt <file>` per file.
  - `--installfw` and `--installpkg` are refused with `--no-gui`.
- **Legal history** (read):
  - In 2017 Atlus asked RPCS3 to remove its Persona 5 mentions and tried to have its Patreon page taken down. Patreon kept the page, and RPCS3 removed the mentions.
  - No Sony action against RPCS3, and no removal of its keys, was found.

### 2.3 Upstream ps3recomp

- **Legal statements** (read):
  - The README: "This project does not contain any proprietary Sony code, firmware, encryption keys, or copyrighted material", and "You must provide your own legally obtained PS3 game files".
  - CONTRIBUTING: never commit "encryption keys".
  - Section 1.3 lists the keys that are in its source anyway.
- **Decryption** (read): `docs/GAME_PORTING_GUIDE.md` sends users to RPCS3 for a decrypted ELF, or to `tools/unfself.py` for debug fSELFs, which are not encrypted. The test harness can call an external `ps3sce` tool if one is installed.
- **Firmware** (read, `docs/FIRMWARE_LLE.md`):
  - The user supplies a decrypted PRX. "Decryption is deliberately outside this pipeline."
  - It also warns: "A standalone executable with lifted firmware baked in is closer to redistributing the firmware than a general emulator is", and "Keep the lifted output local".
  - The CHANGELOG records the 2026-10 untracking of `fw_spu/` as a fix of a breach of "our own firmware policy" (#210, #212).

### 2.4 AC4-Port and ACFA-Port

The closest model to ours: PS3 static recompilations with a launcher. Read from their repositories and docs/31.
- **Setup with the player's RPCS3:**
  1. The launcher copies RPCS3's program files (not its config, games or `dev_*` folders) into a temporary work folder, so the player's install is never changed.
  2. It checks `--help` for `--installfw` and `--decrypt`.
  3. It runs `--installfw PS3UPDAT.PUP`, unless the 4.93 modules are already there.
  4. It runs `--decrypt` once for each of the 10 SPRX modules, `flashATRAC.pic` and `EBOOT.BIN`.
  5. It keeps only the decrypted outputs and deletes the work folder.
  - A "Decrypted files" option skips this for players who decrypted the files themselves.
- **Hash checks:** every input is checked by SHA-256 and size (`known_inputs.cpp`). Firmware 4.93 is the baseline. 4.89 hashes of three modules are kept only so the launcher can say "wrong firmware". There is no warn-and-continue path.
- **Keyless:** ACFA's commits of 2026-10-15 are titled "Keyless inputs: the player's RPCS3-decrypted files, verified by SHA-256" and "the export refuses decryption or key code". A code comment suggests the project had its own decryption before (inferred).
- **Licence:** AC4's LICENSE adds: "This license covers the source code in this repository only. It grants nothing for Armored Core 4 or any other game data, keys or system software, which users must supply themselves."
- **Toolchain:** downloaded at setup (llvm-mingw, CMake and Ninja, about 250 MB, each archive checked by SHA-256). Our goal ships it instead (docs/45).

### 2.5 FFCCEOT-Recomp (local, read)

- The player picks their ROM. Setup checks its SHA-1 against a table in the README (USA and Europe), keeps a copy, and builds the game with the bundled compiler. Nothing is fetched.
- `docs/LEGAL.md` allows facts about the game in the repository (addresses, names, sizes, hashes). It bans the ROM, generated code, assets, "The Wii common key, or any other console key".
- The optional Wii cutscene feature decrypts the player's Wii disc with a key the player types in, held in memory only. Its tests use a disc built with made-up keys.
- A copyright guard script refuses commits that contain game data.

### 2.6 Unleashed Recompiled and XenonRecomp

- The release exe contains the recompiled game code and the translated shader cache, but no assets (inferred from the build files, read).
- The installer checks every file by XXH3-64 and accepts "original and unmodified files" only. It decrypts and patches the XEX with the title update on the player's PC, using the retail key in `XenonUtils/xex.h` (read).
- The README: "This project does not include any game assets", and players must use "your own legally acquired copy of the game" (read).
- No Sega statement, takedown or DMCA was found (inferred).

### 2.7 Zelda64Recomp and N64Recomp

- The release exe contains the recompiled code. The player's ROM supplies the assets at run time (inferred from the build steps, read).
- The ROM is checked by XXH3-64 after byte-order normalisation. N64 ROMs are not encrypted, so no decryption is involved (read; inferred).
- The README: "This repository and its releases do not contain game assets", and "The original game is required to build or run this project" (read).
- No Nintendo DMCA against either project was found (inferred).

### 2.8 What the survey says

- **Projects that ship keys** (RPCS3, Dolphin, Xenia, Vita3K, Unleashed Recompiled) are common, and RPCS3 has had no known action over its keys (read; not found). But a key in the source is exactly what Nintendo's letter about Dolphin named (section 3). Being common is no defence (inferred).
- **The PS3 recompilation closest to ours** (AC4/ACFA) moved to a keyless model in 2026-10. It uses the player's own RPCS3 once at setup.
- **Two shipped recompilations put game code in the release** (Unleashed Recompiled, Zelda64Recomp). Our plan does not: setup builds on the player's PC (docs/45). That keeps translated game code and lifted firmware out of our download, as upstream advises (inferred).

## 3. Legal background

Facts here are read from the statutes, court documents, GitHub's DMCA repository and press reports listed in Sources, unless marked. They are summaries, not legal analysis. Ars Technica, The Verge, Eurogamer, Wired, the BBC, Justia, BAILII and Dolphin's blog could not be fetched, so other sources stand in for them.

### 3.1 United States: 17 U.S.C. 1201

- **The bans** (read, the statute):
  - (a)(1)(A): "No person shall circumvent a technological measure that effectively controls access to a work protected under this title."
  - (a)(3)(A): to "circumvent" includes "to decrypt an encrypted work" without the owner's authority.
  - (a)(2) bans trafficking in technology that (A) "is primarily designed or produced for the purpose of circumventing", (B) "has only limited commercially significant purpose or use other than to circumvent", or (C) is marketed for circumvention.
  - (b)(1) bans the same for measures that protect the owner's rights, such as copy controls.
- **The reverse-engineering exemption, (f)** (read):
  - Someone who "has lawfully obtained the right to use a copy of a computer program" may circumvent "for the sole purpose of identifying and analyzing" what an independently created program needs to interoperate. The elements must not have "previously been readily available", and the acts must not infringe.
  - Tools may be built if "necessary to achieve such interoperability". The information or tools may be passed on "solely for the purpose of enabling interoperability".
- **The Copyright Office's exemptions** (read, 37 CFR 201.40 as amended in 2024):
  - Personal play is covered only for complete games whose authentication servers have shut down.
  - Game preservation, including circumventing console software, is for eligible libraries, archives and museums, on their premises. In 2024 the Register declined to drop the premises limit.
  - A console jailbreak exemption was denied in 2012.
  - No exemption covers decrypting your own game or console firmware to play on a PC (inferred from the text).
- **Case law on circumvention** (read):
  - *Universal v. Reimerdes* (S.D.N.Y. 2000) and *Universal v. Corley* (2d Cir. 2001): DeCSS came from reverse engineering a licensed player, which revealed the CSS "algorithm and keys". The (f) defence failed, because DeCSS was not posted "solely" for interoperability, and (f)(3) "does not apply to public dissemination of means of circumvention". The injunction against posting DeCSS and knowingly linking to it was affirmed.
  - *Davidson v. Jung* (8th Cir. 2005, the bnetd case): the EULA's ban on reverse engineering was enforceable. bnetd was trafficking under (a)(2), and (f) did not apply because the circumvention led to infringement.
  - The circuits split on whether 1201 needs a link to infringement. *Chamberlain v. Skylink* (Fed. Cir. 2004) requires access that bears "a reasonable relationship to the protections that the Copyright Act otherwise affords". *MDY v. Blizzard* (9th Cir. 2010) declined to require one.
  - The AACS "09 F9" key (2007): AACS LA sent DMCA notices treating a published key as circumvention technology, to Google, Digg and others. No lawsuit was reported.
- **Emulation and copyright** (read): *Sega v. Accolade* (9th Cir. 1992), *Sony v. Connectix* (9th Cir. 2000: "intermediate copying and use of Sony's copyrighted BIOS was a fair use") and *Sony v. Bleem* (9th Cir. 2000). All three are fair-use cases about copying; none decides 1201 (inferred).

### 3.2 Console and emulator cases

- **SCEA v. Hotz** (N.D. Cal. No. 3:11-cv-00167, 2011) (read):
  - Filed 2011-01-11 against George Hotz, two named fail0verflow members and 100 unnamed defendants. The claims were DMCA 1201, the CFAA, contributory infringement and state claims.
  - The temporary restraining order (2011-01-27) barred Hotz from distributing or linking to the jailbreak, helping others jailbreak, or distributing the information he had learned.
  - Subpoenas sought the identities of people who downloaded from his site and of people who could see a private YouTube video, and later his PayPal records.
  - It settled on 2011-04-11 with a consent judgment and a permanent injunction. Hotz denied wrongdoing, and the others were dismissed without prejudice.
  - Sony's GitHub notices of January 2011 targeted repositories named ps3keys, ps3tools, ps3utils and psgroove as "Custom Firmware" trafficking under 1201(a)(2) and (b)(1). The February notice took down several hundred fail0verflow forks under 1201 and for facilitating infringement.
  - After the LV0 keys leaked in 2012, no Sony GitHub notice and no lawsuit were found. SCE Japan only warned users.
- **Nintendo v. Tropic Haze, the yuzu case** (D.R.I. No. 1:24-cv-00082, 2024) (read):
  - The counts: 1201(a)(2) and (b)(1) trafficking, (a)(1) circumvention by the developers, direct infringement, and contributory and inducement liability.
  - yuzu shipped no keys. The complaint said it decrypts games "using an illegally-obtained copy of prod.keys", that users get the keys from websites or "by unlawfully hacking a Nintendo Switch console", and that hacking your own console and dumping your own games are "themselves unlawful".
  - The consent judgment of 2024-03-06 set damages at US$2.4 million. It found that software "that in its ordinary course functions only when cryptographic keys are integrated without authorization" violates (a)(2)(A).
  - The worldwide injunction covers "other software or devices that circumvent", including by using unauthorized copies of Nintendo's keys. At Nintendo's election, Lockpick_RCM and all copies of `prod.keys` are destroyed.
  - It is a consent judgment, not a contested ruling (inferred).
- **What followed** (read):
  - Ryujinx stopped on 2024-10-01 after Nintendo offered its developer an agreement.
  - GitHub disabled networks of 8,535 yuzu-fork repositories (2024-04-29) and 4,238 Ryujinx forks (2025-02-26), and more in 2026-02 and 2026-08.
  - The Lockpick_RCM key-dumping tool went in 2023 and 2024.
  - Nintendo's notices against repositories holding `prod.keys` say: "Distribution of keys without the copyright owner's authorization is a violation of Section 1201."
- **Nintendo v. Keighin** (D. Colo., default judgment adopted 2025-10-29, US$17,500) (read):
  - Linking to yuzu, Ryujinx and keys violated 1201(a)(2)(A) and (B).
  - Unauthorized copies of the keys are "primarily, if not solely, designed for the purpose of circumventing".
  - The defendant did not defend (inferred from the default).
- **Dolphin and Steam** (2023) (read):
  - Valve passed on a Nintendo letter. It said Dolphin "operates by incorporating these cryptographic keys without Nintendo's authorisation" and circumvents an access control.
  - No formal DMCA notice was filed. Dolphin abandoned the Steam release (2023-07-20), citing interoperability and legal advice.
  - Its source still holds the common keys (`IOSC.cpp`). No lawsuit was found.
- **Recompilation and decompilation ports** (read):
  - *Super Mario 64 PC port* (2020): Nintendo's notices targeted the compiled executable, posts and videos, as "an unauthorized derivative work". No GitHub notice against the decompilation source was found.
  - *Take-Two v. the re3/reVC developers* (decompiled GTA III and Vice City): DMCA notices in 2021, counter-notices, then a suit for copyright infringement that alleged the repositories held Take-Two's text, dialogue and some assets. It settled and was dismissed with prejudice on 2023-04-03.
  - Nicalis sent DMCA notices against a Cave Story decompilation (2020), claiming its own code was in it.
  - No action was found against Ship of Harkinian, Zelda64Recomp, Unleashed Recompiled or the AC ports, and no recompilation takedown at all in 2025-2026.
- **Sony, FromSoftware and PS3** (read):
  - A DMCA notice for SIE, reported 2025-01-31, asked Lance McDonald to remove links to his Bloodborne 60 fps patch. He says Sony objected to the use of the "Bloodborne" trademark. A week later a claim hit a video of a Bloodborne PSX demake.
  - In 2017 Atlus told RPCS3: "The PS3 emulator itself is not infringing on our copyrights and trademarks", but "To make Persona 5 work on the emulator, the user has to circumvent our DRM protections." Atlus objected to Patreon posts that promoted Persona 5.
  - No Sony action against RPCS3 was found. RPCS3 carries about 90 SELF key entries.
  - PSJailbreak (Australia, 2010): injunctions against distributors of the USB dongle, made permanent with compensation.
- **Canada, Nintendo v. King** (2017 FC 246) (read: a law firm's summary; the judgment itself was not read):
  - Liability for trafficking in, and installing, circumvention devices.
  - The interoperability defence (s. 41.12) failed: no user was shown to use the devices for homebrew, and their primary purpose was circumvention.
  - Statutory damages were the C$20,000 maximum for each of 585 games, plus C$1 million in punitive damages: C$12.76 million in all.

### 3.3 EU and UK (read)

- **InfoSoc Directive 2001/29, article 6:**
  - 6(1) protects "effective technological measures" against knowing circumvention.
  - 6(2) bans devices promoted for circumvention, or with only "a limited commercially significant purpose" other than circumvention, or "primarily designed" for it.
  - 6(3) counts encryption as effective.
  - Recital 50 leaves measures on computer programs to the Software Directive.
- **Software Directive 2009/24:**
  - 5(1): a lawful acquirer may do what is needed to use the program, "including for error correction".
  - 5(3): a lawful user may observe, study and test it.
  - Article 6 allows decompiling when "indispensable" for interoperability, limited to the parts needed. The information may not be used for other purposes.
  - 7(1)(c) gives remedies against means whose "sole intended purpose" is circumvention.
  - Article 8 voids contract terms contrary to article 6 or 5(2)-(3).
- **CJEU cases:**
  - *Nintendo v PC Box* (C-355/12, 2014): games are "complex matter", protected under 2001/29, not just as programs. Console-and-cartridge measures can qualify. Protection must be proportionate, weighing how the devices are actually used.
  - *Top System* (C-13/20, 2021): a lawful acquirer may decompile to correct errors, "to the extent necessary".
  - *Sony v Datel* (C-159/23, 2024): variable data a program puts in RAM is not protected if it does not let the program be reproduced.
- **UK, CDPA:**
  - s. 296: programs; the "sole intended purpose" test, with knowledge of infringing use.
  - ss. 296ZA-296ZF: other works. Criminal offences for devices and services (s. 296ZB), and civil remedies (s. 296ZD).
  - *Nintendo v Playables* [2010] EWHC 1932 (Ch): homebrew use "will still circumvent the ETM", and a non-infringing use "is not a defence".
  - *R v Higgs* [2008] EWCA Crim 1324 quashed modchip convictions because copying was not proved.
- **No EU or UK judgment on emulators or published console keys** was found.

### 3.4 Contract: Sony's PS3 system software licence

- **Version 1.5** says (read):
  - "solely for personal, non-commercial use".
  - "You do not have any ownership rights or interests in the System Software".
  - "you may not reverse engineer, decompile, or disassemble any portion of the System Software".
  - No one may "exploit the System Software in any manner other than to use it in your PS3™ system", or use "tools to bypass, disable, or circumvent any encryption, security, or authentication mechanism".
  - It does not mention emulators or PCs.
- **What follows (inferred):**
  - Lifting libsre and running it on a PC falls outside the licence's grant. That is a contract question, separate from copyright and 1201.
  - In the US, *Davidson v. Jung* enforced a EULA's ban on reverse engineering. In the EU, article 8 of the Software Directive voids such terms as far as article 6 and 5(2)-(3) go.
  - Whether the licence binds someone who only downloaded the PUP is for a lawyer (section 8).

### 3.5 What this means for the options (inferred)

1. **Shipping keys or a decryptor (options a and b)** is the conduct the trafficking bans, Sony's 2011 notices and Nintendo's key notices target. The yuzu judgment and Keighin treat software that works only with unauthorized keys as trafficking, even when users bring the keys. So (b) is not much safer than (a).
2. **Decrypting is "circumvention" by definition**, and no US exemption covers decrypting your own disc for PC play. Under option (c) the player does that act, and we ship nothing that decrypts.
3. **Driving the player's RPCS3 (c+), or naming RPCS3 in a guide, sits in between.** Corley enjoined knowing links to DeCSS, Keighin found links to emulators and keys to be trafficking (by default), and RPCS3 carries keys. The risk looks lower than shipping keys, but not zero. Section 8.
4. **HLE written from observed behaviour (option d)** is how emulators have long replaced firmware (*Connectix*). It decrypts nothing and ships no Sony code. The licence question in section 3.4 is about running Sony's firmware, which (d) removes.
5. **Who would act.** Sony owns both the platform and Demon's Souls, and it acted over FromSoftware fan projects as recently as 2025, including over a game's trademark. The likely first step is a takedown notice to GitHub or a host, not a lawsuit: that is how Nintendo, Sony and Take-Two began (read). A suit followed where code or assets of the rights holder were alleged (re3) or where a key-dependent emulator was the product (yuzu).
6. **Money and promotion raise the stakes.** Atlus's 2017 complaint was about Patreon posts promoting its game on RPCS3. Keeping the port free, with no paid builds or early access, and keeping the game's art out of it, keeps us away from that.

## 4. Options

Two kinds of input need decrypting:
- **Game files**: `EBOOT.BIN` (APP SELF, key revision 0x0001) and the 26 `.sdat` files.
- **Firmware**: `libsre.sprx` (APP SELF, key revision 0x001C in firmware 4.89; measured). Only needed while the eight queue functions run as lifted libsre.

The decrypted `EBOOT.ELF` and the libsre image are needed at run time as well as at build time (section 1.1). So whatever setup produces stays on the player's PC for as long as the game is installed.

Options (a) to (c) are ways to get decrypted files. Option (d) removes the firmware from the list.

| Option | Player supplies | Our download contains | Legal exposure (inferred; section 3) | Plug and play | Effort (inferred) |
|---|---|---|---|---|---|
| (a) Keys in our code | The dump. The PUP too, while libsre stays LLE. | A decryptor plus keys: SELF keys for two key revisions and the SDATA keys. Unpacking a PUP needs more keys. | Highest. Shipping keys and a decryptor is what the trafficking bans target. | Full | S to M. SELF decryption would be new code, written clean. SDATA decryption exists. |
| (b) Keys from the player | The dump, plus a key file or a PS3 running custom firmware or HEN | A decryptor, no keys | Medium to high. We still ship the decryptor. Players would get keys from key sites. | Poor | M |
| (c) Already-decrypted files only | `EBOOT.ELF` and the 26 decrypted `.sdat` files (plus `libsre.prx` without (d)), made with RPCS3 or another tool | No keys, no decryption code | Lowest | Needs a decryptor on the player's PC (today, RPCS3). This breaks the "no RPCS3" goal. | S: hash table, path mapping |
| (c+) (c), and setup runs the player's own RPCS3 | The dump, with RPCS3 already installed | No keys, no decryption code. It runs RPCS3's `--decrypt`. | Low, but above (c): our tool drives the decryption | Good if RPCS3 is there | S |
| (d) Our own HLE for libsre | No firmware at all | No firmware and no lifted firmware | Lowest, for the firmware part | Removes the PUP and firmware step | M: about 9 to 15 working days |

### (a) Keys in our code

- **What it would decrypt:**
  - `EBOOT.BIN`, with the APP key set for revision 0x0001.
  - The 26 SDATA files, with the SDATA keys the runtime already holds.
  - `libsre.sprx`, with the APP key set for revision 0x001C, if it stays LLE. From a PUP, the update packages must be decrypted first (inferred from how RPCS3 installs firmware; section 2).
- **SELF keys belong to a key type and revision, not to one title** (section 2, RPCS3's key vault). Keys that decrypt our `EBOOT.BIN` therefore decrypt every retail game of that revision (inferred). Our tool would be a general-purpose decryptor with one front end.
- **A hash-gated variant (Plan P in section 6):** decrypt only files whose SHA-256 is in our table. That makes the tool single-purpose. But the keys are still in the binary, and anyone can take them out (inferred).
- **Precedents (section 3):** Sony v. Hotz (keys and tools), Nintendo's letter about the Wii common key in Dolphin, and the AACS key takedowns.

### (b) Keys from the player's console or firmware

- **A PUP alone is not enough.**
  - The firmware packages inside `PS3UPDAT.PUP` are encrypted. RPCS3 installs them with keys built into RPCS3 (section 2).
  - The SPRX files inside are SELFs that need the SELF keys for their revision.
  - The PUP holds no ready-to-use key for game executables: the APP keys sit inside the encrypted loaders (inferred).
  - So a tool that gets keys "from the PUP" is option (a) with more keys in it (inferred).
- **Keys from the player's own PS3:**
  - The PS3 must run custom firmware or HEN, which is itself a circumvention by the player (inferred).
  - Homebrew on the console then decrypts files or dumps keys. An earlier search found no route that works on the console alone (read, web search; Sources).
  - Not plug and play, and many players no longer have a working PS3 (inferred).
- **Models elsewhere** (section 2): yuzu and Ryujinx (the player dumps `prod.keys` from their own Switch), Cemu (`keys.txt`), Vita3K (firmware plus licence files), and FFCCEOT-Recomp's Wii cutscenes (the player types the Wii common key; it is kept in memory only).
- **Exposure:** we would still ship the decryption code. The yuzu complaint argued that an emulator circumvents even when users supply the keys (section 3).

### (c) Accept only already-decrypted files

- **Inputs, each checked by SHA-256:**
  - `EBOOT.ELF`: 90eaa5e9…, appendix A (measured).
  - The 26 decrypted `.sdat` files: their hashes are not measured yet (section 10, check 2).
  - `libsre.prx`: 115f9df9…, and only without (d).
- **How the player makes them** (read, RPCS3's `decrypt_binaries.cpp`):
  - RPCS3's Utilities > Decrypt PS3 Binaries, or `rpcs3 --decrypt <file>`, once per file (section 2.2). It picks the method by the file's first four bytes, not its name.
  - The output goes beside the input. A `.sprx` becomes `.prx`. Any other SELF becomes `.elf`, so `EBOOT.BIN` gives `EBOOT.elf`. An NPD file gets `.unedat` appended: `m01.luabnd.dcx.sdat.unedat`.
  - It tries an all-zero KLIC first and asks for one only if that fails. SDATA uses fixed keys, so the 26 files should need no KLIC (inferred).
  - This answers docs/45's open question #7 (the output names).
- **Firmware for `--decrypt`:** RPCS3 probably needs no firmware for this step, because its keys are built in (inferred). Section 10, check 1 settles it.
- **The runtime must take pre-decrypted SDATA:**
  - Look for a plain copy (for example `<name>.sdat.unedat`, or a copy in the data folder) before `edat_resolve`.
  - Release builds leave the NPDRM keys out.
- **Plug and play:** the player needs a decryptor, and today that is RPCS3. Players who dump their own disc already use PC tools (section 2: PS3 Disc Dumper, or a hacked PS3 plus RPCS3's guide). Many will have RPCS3 already (inferred).
- **Exposure:** we ship no circumvention technology. Decrypting counts as "circumvention" by definition (1201(a)(3)(A)), and here the player does it. US law has no personal-use exemption for that act (section 3.1).
  - Naming the tool is not risk-free. Corley enjoined knowing links to DeCSS. Keighin, a default judgment, found that links to emulators and keys were trafficking. RPCS3 carries keys (section 3.2).
  - So the guide could name RPCS3 without a link, or name no tool at all (inferred). Section 8, question 2.

### (c+) Setup runs the player's RPCS3

This is AC4-Port's model (docs/31; section 2).
- Setup copies `EBOOT.BIN` and the 26 `.sdat` files into a temporary folder.
- It runs `rpcs3.exe --decrypt` with standard input closed, collects the outputs by name, checks their hashes and deletes the temporary copies.
- With (d), no PUP is needed.
- **Exposure (inferred):** our tool drives a third-party decryptor. That is closer to the trafficking question than (c), and still far from shipping keys. Section 8, question 1.

### (d) Our own HLE for the eight libsre functions

- **Scope.** The eight functions in section 1.2. The queue in guest memory is a big-endian `CellSyncLFQueue` line that the game's own SPU code edits with GETLLAR/PUTLLC (read, docs/25; comments in `libs/spurs/cellSpurs.c`).
- **Already in the fork's HLE** (read, `libs/spurs/cellSpurs.c`):
  - `_cellSpursLFQueueInitialize` and `_cellSpursQueueInitialize` write a big-endian line with the size, depth and buffer. The push/pop slot state (`bs[]`, `v1`) is "not guessed at".
  - `cellSpursQueuePushBody` exists.
  - The attach functions only log.
- **Missing:**
  - `_cellSpursLFQueuePushBody` and `cellSpursQueuePopBody`.
  - The slot state machine.
  - Real attach and detach: libsre connects an lv2 event queue port and releases it again (docs/25, section 5).
  - Blocking and wake-up between an SPU push and a PPU pop.
- **What we may build from:**
  - Public descriptions of `CellSyncLFQueue`.
  - RPCS3's cellSync HLE, read for behaviour only, since it is GPL. Its cellSync LFQueue is complete, while its SPURS queue functions are stubs (read).
  - Our own traces of the 128-byte line before and after each call while libsre runs.
  - The game's SPU side.
- **Don't translate libsre's code into C by hand.** That would make the HLE a derivative of Sony's code (inferred). Upstream says the same about lifted firmware in shipped binaries (read, `docs/FIRMWARE_LLE.md`). Write a spec from traces and the SPU side, then implement from the spec.
- **Effort (inferred): about 9 to 15 working days.**
  - Spec from traces: 2 to 3 days.
  - Implementation in the fork: 3 to 5 days.
  - A/B against the LLE as an oracle (as with the taskset policy), plus soak runs (boot to the Palace, level loads, the Nexus): 3 to 5 days.
  - Docs: 1 to 2 days.
  - The risk is freezes like the earlier lwarx and PUTLLC ones. The HLE's stores must go through the same lock-line path as SPU PUTLLC.
- **Payoff beyond the legal side:**
  - No firmware version dependence. The lift uses `LIBSRE_TOC` 0x30039AB0, which belongs to one libsre build. Players' firmware varies: Sony's current version is 4.93 (read, Sony's page), AC4-Port requires 4.93, and this PC's RPCS3 has 4.89 (measured). Another build's libsre may need another TOC and other checks (inferred).
  - No firmware licence question. Sony's system software licence allows use only "in your PS3™ system" and forbids decompiling it (section 3.4).
  - No PUP step, no lifted firmware in the build, and a smaller lift.
  - Upstream gains a SPURS queue HLE. Saints Row 2 and LittleBigPlanet use these queues too (read, `cellSpurs.c` comments).
- **The LLE path can stay as a developer option** for people who build from source with their own `libsre.prx`. It would be off in releases.

## 5. Dump formats and versions

### 5.1 Versions

| Title ID | What | Version and updates | For the port |
|---|---|---|---|
| BLUS30443 | US disc (Atlus, 2009-10-06) | 01.00. `PS3_SYSTEM_VER` 02.76 (measured, this PC's `PARAM.SFO`). Sony's update server lists no update (measured: HTTP 200, empty reply). | Built from this one. Accept. |
| BLUS30443CE | US Deluxe Edition (a product code) | Not checked | Probably the same disc (inferred). Accept by hash if identical. |
| BLES00932 | EU disc (2010) | 01.00. No update on Sony's server (measured). | Not supported: every guest address in `port/` is for BLUS30443 (docs/31). |
| BCJS30022 | JP disc (SCE, 2009-02-05) | Update 01.04: 48,414,800 bytes, needs system 2.50 (measured, Sony's update metadata) | Not supported |
| BCAS20071 | Asian disc | Update 01.04: 48,414,800 bytes, needs system 2.50 (measured) | Not supported |
| NPUB30910 | US PlayStation Store version (2013) | No update on Sony's server (measured) | Refuse: NPDRM (section 5.2) |
| NPJA00102 | JP PlayStation Store version | Sony's server returns 404 for this ID (measured). An RPCS3 issue runs it with the 1.04 update (docs/29, M9). | Refuse |
| BLUD80018 | US trade demo | 01.01 (RPCS3 `patch.yml`) | Refuse |

Sources: this PC's `PARAM.SFO` (measured); Sony's update metadata (measured); RPCS3's `patch.yml`, a local copy, which lists BLUS30443 01.00, BLES00932 01.00, BCAS20071 01.04, BCJS30022 01.04 and BLUD80018 01.01 (read); docs/29 (read); search results quoting Giant Bomb and the Video Game History Foundation (read; Sources). No EU store ID was found.

### 5.2 Formats

| Format | What it is | What needs decrypting | Plan L (section 6) |
|---|---|---|---|
| Folder ("JB" format): `PS3_GAME/` with or without `PS3_DISC.SFB` | The disc's files. The dumper has already removed the disc encryption. | `EBOOT.BIN` and the 26 `.sdat` | Accept. This PC's dump is one (measured). |
| Decrypted ISO | The same files in an ISO 9660 image | Same as the folder | Accept later. It needs only a read-only ISO reader. RPCS3 now boots ISO files too (read, `Emu/System.cpp`). |
| Encrypted ISO (a raw disc image) with a disc key or IRD file | The disc as pressed, with its disc encryption. RPCS3 reads these with a `.dkey` file since 2026-04 (section 2.2). | The disc layer as well | Refuse. Decrypting the disc layer is one more circumvention step (inferred). |
| Store package (`.pkg`) with a RAP or RIF licence (and `act.dat` plus the console ID) | The store version, licensed per account | The package key, the NPDRM keys and the player's licence | Refuse |
| The store version installed in RPCS3 (`dev_hdd0/game/NPUB30910`) | An installed store copy | Still an NPDRM executable | Refuse, for now |

The PS3 Disc Dumper makes decrypted copies of discs, given a compatible Blu-ray drive and the disc's key from the redump database or an IRD file (read, its README). Disc dumps are then folders; the `EBOOT.BIN` inside is still an encrypted SELF (measured on this PC's dump).

### 5.3 What setup checks (read-only)

1. `PARAM.SFO`: `TITLE_ID` BLUS30443, `APP_VER` 01.00 and `VERSION` 01.00, `CATEGORY` DG (the values in this PC's dump; measured). Any other ID or version gets a clear message. The port targets one version (docs/29, M9).
2. The SHA-256 of the encrypted `EBOOT.BIN`, 77375fbf… (appendix A). It tells the player "right game, right version" before anything is decrypted.
3. The SHA-256 of each decrypted input: `EBOOT.ELF` 90eaa5e9…, the 26 decrypted `.sdat` files (to be measured: section 10), and `libsre.prx` 115f9df9… only if the LLE path stays.
4. Nothing is ever written into the player's dump. Today the runtime writes `.sdat.dec` files there (section 1.1).

Hashes and title IDs are facts, not keys or game data. Other projects publish them (section 2): FFCCEOT-Recomp's ROM table, and AC4-Port's `known_inputs.cpp`.

## 6. Recommendation

All of this is inferred from sections 1 to 5. It is not legal advice.

### Plan L (recommended): (d) plus (c), and nothing secret in our download

1. **Do (d).** Write HLE for the eight libsre functions, then drop the firmware from the player's list: no PUP, no `libsre.sprx`, no `fw_spu`. Keep the LLE path as a developer option for A/B work, off in releases.
2. **Do (c) for the game files.** The release holds no keys and no decryption code. Setup accepts the decrypted `EBOOT.ELF` and the 26 decrypted `.sdat` files and checks them against the BLUS30443 01.00 hash table. The player makes them once with a tool of their choice. The guide names RPCS3 (Utilities > Decrypt PS3 Binaries) because that is what most players already have (inferred).
   - **(c+) is a user decision** (section 7): if RPCS3 is already installed, setup could run its `--decrypt` on temporary copies, as AC4-Port does. It is easier for players and a small step up in exposure (section 8, question 1).
3. **Keep keys out of what we ship.**
   - Add a build option that compiles the NPDRM/SDATA decryption out of the runtime (`libs/filesystem/edat.c`, `runtime/ppu/sdata_decrypt.h`). Release builds use it. The runtime then reads pre-decrypted SDATA instead.
   - Ship only the Python tools the build needs (an allow-list). `tools/pkg_extract.py` must never be in a release.
   - Add a release check that fails if a shipped file contains any key constant from our own source tree. The check compares bytes and never prints them.
   - Guard commits as well. FFCCEOT-Recomp's copyright guard refuses commits that contain game data (section 2.5). Ours would refuse SELF, NPD and PS3 ELF headers, game file signatures and known key bytes.
4. **Fix what is already exposed** (section 1):
   - The runtime writes `.sdat.dec` files into the player's dump. They should go to the data folder, or nowhere once SDATA arrives pre-decrypted.
   - The game's `ICON0.PNG` is built into `des_boot.exe` today (`port/CMakeLists.txt`). That is fine for a local build. A released exe must use our own icon.
   - The decrypted firmware in the fork's public history (section 1.4) needs a decision (section 7).
   - The port's README says "your own decrypted `libsre.prx`", and docs/05 says the runtime has no keys. Both change with this plan.
5. **The plug-and-play cost.** The player still needs a decryptor once, at setup. Everything else in the goal holds: no compiler, Python, CMake, Git or SDK to install, and no firmware.

What the player does under Plan L (inferred):
1. Dump the disc: the PS3 Disc Dumper with a compatible Blu-ray drive, or a PS3 with custom firmware (RPCS3's Quickstart).
2. Decrypt once.
   - Under (c): RPCS3's Decrypt PS3 Binaries on `EBOOT.BIN` and the 26 `.sdat` files. They sit in five folders, so that is a few rounds of the file dialog.
   - Under (c+): setup does this with the player's RPCS3.
3. Start our setup and pick the game folder. Setup finds `EBOOT.elf` and the `*.sdat.unedat` files beside the originals (or in a folder the player picks), checks every hash, copies them to its data folder and builds.

### Plan P: (d) plus a minimal (a), only with a lawyer's sign-off

- Setup decrypts `EBOOT.BIN` and the 26 `.sdat` files itself, only when their SHA-256 matches our table. It carries one SELF key set (APP, revision 0x0001) and the SDATA keys, and no PUP or NPDRM licence keys.
- This meets plug and play in full. It is also the trafficking case that sections 3 and 8 are about: the keys work for every game of that revision.

### Plan K: (d) plus (b), not recommended

- The player supplies a key file.
- We would still ship a decryptor, so most of Plan P's exposure remains (the yuzu complaint; section 3). Players would also have to fetch keys from key sites, which is worse for them and for us.

### Aside: licences in a binary release

- The runtime links `third_party/at3_standalone`, an LGPL-2.1-or-later ATRAC decoder (read, its README and `ps3recomp/CMakeLists.txt`).
- A binary release must let players replace it. A separate DLL, or object files for relinking, plus a pointer to its source would do (inferred).
- AC4-Port adds a paragraph to its MIT licence: the licence covers the source only and grants nothing for the game, its keys or system software (read, docs/31). Our LICENSE could add the same.

## 7. Decisions for the user

1. **The plug-and-play exception.** Accept that the player decrypts once with a tool they already have (Plan L), or carry the exposure of shipping decryption (Plan P only with a lawyer; Plan K is not recommended). Recommendation: Plan L.
2. **(c) or (c+).** Under Plan L, setup either only takes files the player decrypted, or also runs the player's RPCS3 `--decrypt` for them (AC4-Port's model). (c+) is easier for players and a little more exposed.
3. **Do (d)?** HLE for the eight libsre functions, about 9 to 15 working days, which removes the firmware from the player's list. Recommendation: yes, before the first public release.
4. **The firmware in the fork's public history** (`02b599d`; section 1.4). Options:
   - Leave it.
   - Ask upstream to purge it (a history rewrite there, plus GitHub Support for cached views).
   - Recreate our fork without that history.

   Any rewrite of pushed commits needs your explicit permission (repo rules). While upstream keeps the commits, the fork network still serves them.
5. **The inherited keys in the fork's source** (`edat.c`, `sdata_decrypt.h`, `tools/pkg_extract.py`). Options:
   - Compile them out of release builds and leave the source as upstream has it (recommended).
   - Also remove them from the fork's source, which drifts from upstream.
6. **Formats and versions.** Recommendation:
   - Accept: the folder dump of BLUS30443 01.00 now, decrypted ISOs later.
   - Refuse: encrypted ISOs, store packages, the demo and other regions.
7. **Wording.** Update docs/05-legal.md, the README's Requirements and Legal sections, and LICENSE (the added paragraph) to match the plan you choose.
8. **The LGPL decoder** in a binary release: comply (a separate DLL, or relinkable objects) or replace it.
9. **No money tied to the port.** Keep it free, with no paid builds, early access or donation tiers for it (section 3.5, point 6). Recommendation: yes.

## 8. Where a lawyer's opinion matters

1. Whether setup running the player's own RPCS3 `--decrypt` (c+) makes our tool a circumvention device, in the US and the EU or UK.
2. Whether a guide that names the tool that decrypts their files carries risk, with or without a link. Corley enjoined knowing links to DeCSS. Keighin (a default) found links to emulators and keys to be trafficking. RPCS3 is a widely used emulator that carries keys.
3. Whether a hash-gated decryptor (Plan P) changes the analysis under 17 U.S.C. 1201(a)(2) and (b)(1), and under InfoSoc article 6(2) (a "limited commercially significant purpose" other than circumvention).
4. Whether the decrypted firmware in the fork's public history (section 1.4) exposes the fork, and what removing it should involve.
5. Whether the interoperability exceptions reach a static recompilation done on the player's PC: 17 U.S.C. 1201(f), the EU Software Directive's article 6, and Canada's s. 41.12. They are narrow (section 3).
6. Whether the published SDATA and NPDRM constants in a shipped binary count as trafficking, even though the same constants are in RPCS3 and many tools (section 2).
7. The project's name and README use the game's trademark. docs/05 already lists trademark as a residual risk. Sony's 2025 notice over the Bloodborne patch was reportedly about the trademark (section 3.2).
8. Sony's system software licence (section 3.4). Does it bind a developer who only downloaded the PUP? Does studying libsre's behaviour to write the HLE (option d) conflict with it, and does the EU's article 8 or US fair use (*Connectix*) settle that?
9. Which law applies. That depends on where the developers, the host and the players are. The US, the EU, the UK and Canada all ban trafficking in circumvention tools (section 3).

## 9. Open questions

1. **Decrypted SDATA hashes.** Are RPCS3's `.sdat.unedat` outputs byte-identical to the runtime's `.sdat.dec` files? Section 10, check 2.
2. **Firmware for `--decrypt`.** Does RPCS3's `--decrypt` need installed firmware? Section 10, check 1 (inferred: no).
3. **Which libsre build.** Is the git copy of `libsre.prx` the decryption of firmware 4.89's `libsre.sprx`? If not, `LIBSRE_TOC` 0x30039AB0 may not fit other firmware versions, which is one more reason for (d). Section 10, check 3.
4. **Other pressings.** Does the Deluxe Edition or a reprint (Greatest Hits) carry the same `EBOOT.BIN`? Only owners' hashes can tell.
5. **The store version.** Is NPUB30910's code the same as the disc's 01.00, only NPDRM-wrapped? Under Plan L, players who own only the store version are left out either way (inferred).
6. **The EU store ID** was not found.
7. **Setup's decrypted inputs.** If (c+) is chosen, does RPCS3 run `--decrypt` without its first-start dialogs? AC4-Port copies RPCS3 into a work folder with its own settings to avoid them (docs/31).

## 10. Checks that need no game run

These read the player's own files, write only into a temporary folder, and start no game. `<rpcs3>` is the folder that holds `rpcs3.exe`, and `<game>` is the folder that holds `PS3_GAME`.

1. **EBOOT.** Copy `<game>\PS3_GAME\USRDIR\EBOOT.BIN` into an empty temporary folder and run `rpcs3.exe --decrypt` on the copy. Expect `EBOOT.elf` beside it, with SHA-256 `90eaa5e9…` (appendix A). This also shows whether `--decrypt` works without firmware.
2. **SDATA.** Copy the 26 `.sdat` files into the temporary folder and decrypt them the same way. Expect 26 `*.sdat.unedat` files. Compare each with the runtime's `*.sdat.dec` beside the original, where one exists (13 do on this PC). If they match, RPCS3's output can stand in for the runtime's decryption, and the 26 plaintext hashes go into setup's table.
3. **libsre.** Copy `<rpcs3>\dev_flash\sys\external\libsre.sprx` and decrypt it. Compare `libsre.prx` with `115f9df9…` (appendix A).
4. **Cleanup.** Delete the temporary folder afterwards. It holds decrypted copies of the player's files.

## Appendix A: hashes (measured)

Measured on 2026-10-18 from files already on this PC. Nothing was copied.

### Game files, BLUS30443 01.00

| File | Bytes | SHA-256 | SHA-1 | MD5 |
|---|---|---|---|---|
| `PS3_GAME/USRDIR/EBOOT.BIN` (encrypted SELF) | 28,224,840 | `77375fbf5c654b5a92b870831862a9a09bdc9e6052956d5b84503bda5704f3ac` | `1b668469dbca893d65713f4bfdfe5258489c3627` | `9a882a31807ab0a28ff11450bd742477` |
| `EBOOT.ELF` (decrypted with RPCS3; the lift's input) | 28,222,408 | `90eaa5e9e48b4874292badc311ccea253e6a622ff188bfbdbe76e6f6821269d8` | `ceb1d7d09868c093cb54606314ad2d3117b996fc` | `db95c32f21cf274fa6f41026e199970c` |
| `PS3_GAME/PARAM.SFO` | 1,008 | `13884a41803d9ad9955db48cf2f2c36b9c511f0770e511f91eccbda13b73c78b` | `faa89e9631fb9bffa54d7c01dd5c905a565b861d` | `95d07fa3c26d15ce7fea0c84214ce239` |
| `PS3_DISC.SFB` | 1,536 | `f7e6cad710fe219811dab941e3c46bfc9dc447b928dfbe7d1802f97d357cc75a` | `e1442210e6de83618b73a160f63315d8125d3e1e` | `185909f0370c2baa26b9535a959d1f13` |

SELF header of `EBOOT.BIN`: version 2, key revision 0x0001, header type 1, program type 4 (APP), auth ID 0x1010000001000003, vendor 0x01000002, app version 0x0001000000000000.

### The 26 SDATA files (encrypted, as on the disc)

Path under `PS3_GAME/` | bytes | SHA-256. All are NPD version 2, licence 0, type 0, flags 0x0100000C, 16 KB blocks, with an empty content ID.

| File | Bytes | SHA-256 |
|---|---|---|
| `USRDIR/chr/c0000/c0000.esd.dcx.sdat` | 33,568 | `b99189e0a18a5f4a0c63a853969f578d8395f12f0c217ed3d69c864c7146c843` |
| `USRDIR/chr/enemycommon.esd.dcx.sdat` | 8,912 | `09da58e5028881f7e1491cd8e39411075761d95243999cdcd81622773f6637c8` |
| `USRDIR/chr/m01.chresdbnd.dcx.sdat` | 3,264 | `efb19f6bfef6e85f8f5f605687bfab2b19945001ec7950aa5dda8adf566b9bbe` |
| `USRDIR/chr/m02.chresdbnd.dcx.sdat` | 30,784 | `c11f25a7e183e01c827515c3b3aa15e821ec40dbc4c7a3a5975396f1b752cb49` |
| `USRDIR/chr/m03.chresdbnd.dcx.sdat` | 27,168 | `fda311345115b7101144665051f742a5a1cf3cd0a9f8a334a8ce2218cca9bf87` |
| `USRDIR/chr/m04.chresdbnd.dcx.sdat` | 13,680 | `20bab94e9bb4033a9654176a4187cce6b280bcb80a979c3ce3e12941f1e9d4d4` |
| `USRDIR/chr/m05.chresdbnd.dcx.sdat` | 13,552 | `e67fc9a66f6aa4c6330e1a0fc5659ca4db89bf1946faaae375143b69cc9cc567` |
| `USRDIR/chr/m06.chresdbnd.dcx.sdat` | 22,320 | `6e49baf88aa33e3e635d946feb1cd9ea4edfe3932ff28460b723602507915afd` |
| `USRDIR/chr/m08.chresdbnd.dcx.sdat` | 7,984 | `95e651468c7dbc2fdcb8862839e22e1ffa0d82437f3612740dbb4f7b78457c24` |
| `USRDIR/chr/m99.chresdbnd.dcx.sdat` | 105,184 | `8ca5e413332071dc8ecb7594761e2a2923344fc9b3c1f2fc7258ac836ded1434` |
| `USRDIR/menu/common.menuesdbnd.dcx.sdat` | 9,616 | `ed01e42e1fe9af9806a20af3e0ec8803498347b9df1e50315b35719c7faacbff` |
| `USRDIR/menu/ingame.menuesdbnd.dcx.sdat` | 27,152 | `8800c6dcd7c65556bf3bbe97666b16b82956d3e752db88c444df73908b93da99` |
| `USRDIR/menu/title.menuesdbnd.dcx.sdat` | 4,304 | `b141224c80b32abddb389114f47484949afceeafbbfec6100caaf451e623483f` |
| `USRDIR/script/m01.luabnd.dcx.sdat` | 123,504 | `753b3e80024c71336f01e1cae7f28cc1f527668cb9b9d1186eac35e9fe59c78c` |
| `USRDIR/script/m02.luabnd.dcx.sdat` | 214,352 | `cae0b75b6f317adceb84713a0b9ec665ac197c6967fea32c3ca5485e8f9c3ae7` |
| `USRDIR/script/m03.luabnd.dcx.sdat` | 151,568 | `9a262026c4c62c349bcb34e828866467740b65e71f74d5d181c7c62bd5363f0e` |
| `USRDIR/script/m04.luabnd.dcx.sdat` | 156,608 | `8a8d37ae77c70ccdf58744869d941957f4ea996e738f171685caa221adee5abf` |
| `USRDIR/script/m05.luabnd.dcx.sdat` | 138,208 | `7878d9aead49e683a57247d133a63c6b5ec001547c9050cc8fbdcf6b24d049be` |
| `USRDIR/script/m06.luabnd.dcx.sdat` | 154,848 | `3238a4f554c48137e3e8953dd685ac7fc93fed6e86e79278b246c61cf66e32ca` |
| `USRDIR/script/m08.luabnd.dcx.sdat` | 99,616 | `3266881bf0fbe5a990ce0e1adf13f8dd22714d087aa5fa59f2e0830549f022ac` |
| `USRDIR/script/talk/m01.talkesdbnd.dcx.sdat` | 26,384 | `b39a4eccb7d5e33b4e9d77ec14421546bb9bd518bca1d0e97ef5c49c96a40660` |
| `USRDIR/script/talk/m02.talkesdbnd.dcx.sdat` | 12,432 | `6aadfdec449b7095ba318b3bd7ab8cebb91e2c43e54a58c9e6213e6e397bc9a5` |
| `USRDIR/script/talk/m03.talkesdbnd.dcx.sdat` | 7,440 | `a6a78319d9db924796e961eecc4d3e6a82771ba64ab443c2830ef47baab2f0d2` |
| `USRDIR/script/talk/m04.talkesdbnd.dcx.sdat` | 5,232 | `7b58617a223e909caaaf9a6add5fd6fce2fdb626d2552da12577abe2baef5f27` |
| `USRDIR/script/talk/m05.talkesdbnd.dcx.sdat` | 5,920 | `b65627a772f59c06bff4d627dc13a429eb1b59f8d5afe6bd3dd9a9f4dc4b3849` |
| `USRDIR/script/talk/m06.talkesdbnd.dcx.sdat` | 7,872 | `3dae370562eacd5a4f93029d0db058c8000c171d8c41b05d4b1c2234fe468efe` |

The 26-line manifest (`sha256 size path`, sorted by path, LF line ends) has SHA-256 `333e8ca383631fed7a28f9feecefec10e561eb9d0e968b50ef1fbc088d02c734`.

### Firmware files

| File | Bytes | SHA-256 | SHA-1 | MD5 |
|---|---|---|---|---|
| `ps3recomp/fw_spu/libsre.prx` (decrypted; the copy upstream committed in `02b599d`, identical in `0952e1c~1` and in this checkout) | 239,344 | `115f9df9596dd0b358e5a8c4eaaf77ee44c1f3ec16f9056b3abc517b739140c9` | `1189f3fa598dd6c7b13a24e261e8228d70112c08` | `8f83d53d3ef0d57d58c45a991d77bb63` |
| `dev_flash/sys/external/libsre.sprx`, firmware 4.89, in this PC's RPCS3 (encrypted) | 114,254 | `fd0f6e06a623c4c43f978cb75610243d620e04108f8075566fa8fefc34918e84` | `6984130594fd5eb40a54fbe68a21c57d4dfef9bf` | `3e696d3e452f6615bc633f0abda7bdd7` |
| `dev_flash/sys/external/flashATRAC.pic`, firmware 4.89 (a plain ELF32 SPU program; not used) | 35,131 | `1edf54e1323dd736b71958ca56307d536dedc7c05ccb0dd3b2a3be1af5b50e30` | `ed2e46e9947c191e27b3b25a6be67ba1901a8810` | `d15732f3891b5d6beaf9cd37cd79a7eb` |

SELF header of `libsre.sprx` 4.89: version 2, key revision 0x001C, header type 1, program type 4, auth ID 0x1070000040000001, app version 0x0004008900000000.

Whether the git copy of `libsre.prx` is the decryption of this 4.89 `libsre.sprx` is not known (section 10, check 3).

## Sources

### Our files and local checkouts (read)

- Port repository: `README.md`; docs/03, 05, 22, 23, 25, 28, 29, 31 and 45; `tools/des/deslib.sh`, `tools/des/desrun.sh`; `scripts/build_libsre.sh`, `scripts/gen_libsre_exports.py`, `scripts/gen_taskset_policy_blob.py`; `port/CMakeLists.txt`.
- The fork (ps3recomp): `libs/filesystem/edat.c` and `edat.h`, `runtime/ppu/sdata_decrypt.h`, `runtime/ppu/ppu_fs.cpp`, `runtime/ppu/ppu_loader.cpp`, `libs/spurs/cellSpurs.c`, `libs/sync/cellSync.h`, `tools/pkg_extract.py`, `tools/unfself.py`, `tools/harness/ps3_recomp_harness.py`, `third_party/at3_standalone/README.txt`, `CMakeLists.txt`. From upstream's master: `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `docs/FIRMWARE_LLE.md`, `docs/GAME_PORTING_GUIDE.md`. Git history of `fw_spu/` (`02b599d`, `0952e1c`).
- RPCS3 source, a local checkout, GPL: read for behaviour only, nothing copied. `rpcs3/rpcs3.cpp`, `Emu/System.cpp`, `Emu/Cell/lv2/sys_prx.cpp`, `Emu/Cell/PPUModule.cpp`, `Emu/Cell/Modules/cellSpurs.cpp`, `Emu/Cell/Modules/cellSync.cpp`. Also a local copy of RPCS3's `patch.yml`.
- FFCCEOT-Recomp, a local checkout: `README.md`, `docs/LEGAL.md`, `docs/WII_ASSETS.md`, `docs/ARCHITECTURE.md`.

### Measured

- Sony's public update metadata, `https://a0.ww.np.dl.playstation.net/tpl/np/<TITLE_ID>/<TITLE_ID>-ver.xml`, for BLUS30443, BLES00932, BCJS30022, BCAS20071, NPUB30910 and NPJA00102. It was read with curl because the server's certificate chain is self-signed.
- Hashes, sizes and headers of files on this PC (appendix A).

### Web pages (read)

RPCS3:
- Source files under https://github.com/RPCS3/rpcs3/tree/master/rpcs3: `Crypto/decrypt_binaries.cpp`, `Crypto/key_vault.cpp`, `Crypto/key_vault.h`, `Crypto/unself.cpp`, `Crypto/unedat.cpp`, `Crypto/unpkg.cpp`, `Loader/PUP.cpp`, `Loader/TAR.cpp`, `Loader/ISO.cpp`, `rpcs3qt/main_window.cpp`, `Emu/System.cpp`, `Emu/system_utils.cpp`, `Emu/Cell/lv2/sys_prx.cpp`, `Emu/Cell/PPUModule.cpp`, `rpcs3.cpp`.
- ISO history: https://github.com/RPCS3/rpcs3/commits/master/rpcs3/Loader/ISO.cpp, https://github.com/RPCS3/rpcs3/pull/18648, https://github.com/RPCS3/rpcs3/pull/19421, https://github.com/RPCS3/rpcs3/pull/18511
- The website's source, for the Quickstart, FAQ and disclaimer: https://github.com/DAGINATSUKO/www-rpcs3. rpcs3.net and its wiki returned HTTP 403 to the fetcher.
- https://wccftech.com/rpcs3-enables-encrypted-playstation-3-disc-iso-loading/amp/
- https://www.muycomputer.com/2026/07/17/rpcs3-esta-a-un-paso-de-convertirse-en-una-ps3-real-eliminara-la-necesidad-de-cargar-firmware-de-una-consola/
- https://vice.com/en/article/atlus-tried-to-take-down-a-playstation-3-emulators-crowdfunding-page

Dumping and decryption tools:
- https://github.com/13xforever/ps3-disc-dumper (README)
- A web search for on-console EBOOT decryption returned guides that all use a PC: https://shrinefox.com/blog/?p=659, https://consolemods.org/wiki/PS3:Decrypting_PS3_ISOs, https://forums.rpcs3.net/showthread.php?tid=181625 (search results only).

GitHub:
- https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository
- https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/about-permissions-and-visibility-of-forks

Recompilation projects:
- https://github.com/SulphurS16/AC4-Port: `README.md`, `LICENSE`, `data/toolchain.txt`, `src/common/known_inputs.cpp`, `src/common/crypto.cpp`, `src/launcher/rpcs3.cpp`, `download.cpp`, `setup.cpp`, `inputs.cpp`, and the commit list.
- https://github.com/SulphurS16/ACFA-Port: `README.md`, `LICENSE`, `src/common/known_inputs.cpp`, `src/launcher/rpcs3.cpp`, and the commit list.
- https://github.com/hedge-dev/UnleashedRecomp: `README.md`, `docs/BUILDING.md`, `docs/DUMPING-en.md`, `UnleashedRecomp/install/installer.cpp`, `xcontent_file_system.cpp`, `main.cpp`, `CMakeLists.txt`, `user/paths.h`, `UnleashedRecompLib/CMakeLists.txt`, and release v1.0.3.
- https://github.com/hedge-dev/XenonRecomp: `README.md`, `XenonUtils/xex.h`, `xex.cpp`, `xex_patcher.cpp`.
- https://github.com/Zelda64Recomp/Zelda64Recomp: `README.md`, `BUILDING.md`, `src/main/main.cpp`; https://github.com/N64Recomp/N64ModernRuntime (`librecomp/src/recomp.cpp`); https://github.com/N64Recomp/N64Recomp (`README.md`).

Other emulators:
- Dolphin: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/IOSC.cpp
- Cemu: https://cemu.info/faq.html
- Ryujinx keys page, on a mirror because the original wiki page returns 404: https://zff.dev/smj2k/Ryujinx/wiki/Keys
- yuzu settlement: https://www.engadget.com/makers-of-switch-emulator-yuzu-quickly-settle-with-nintendo-for-24-million-203204698.html
- Vita3K: https://vita3k.org/quickstart.html, and `vita3k/packages/src/` (`sce_utils.cpp`, `license.cpp`, `pkg.cpp`, `pup.cpp`) in https://github.com/Vita3K/Vita3K
- shadPS4: https://shadps4.net/quickstart/ and https://github.com/shadps4-emu/shadPS4 (`README.md`)
- Xenia: https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/xex_module.cc
- PCSX2: https://pcsx2.net/docs/setup/bios/
- DuckStation: https://github.com/stenzek/duckstation (`README.md`)

Sony:
- https://www.playstation.com/en-us/support/hardware/ps3/system-software/
- https://doc.dl.playstation.net/doc/ps3-eula/ps3_eula_en.html (System Software License Agreement, version 1.5)
- https://mp1st.com/news/ps3-update-4-93-required-to-keep-blu-ray-drive-working

The game's versions:
- https://www.demonssouls.com/How_to_play_the_Original_Demon%27s_Souls
- Search results quoting https://www.giantbomb.com/app.php/demons-souls/3030-24057/releases/ and https://library.gamehistory.org/repositories/2/archival_objects/17693 (both pages returned 403 or could not be reached), and https://gamebanshee.com/k4h4 for the 2013 store release.

### Law, court records and press reports (read, 2026-10-18)

US statute and rules:
- 17 U.S.C. 1201: https://www.law.cornell.edu/uscode/text/17/1201
- 37 CFR 201.40: https://www.law.cornell.edu/cfr/text/37/201.40
- The 2021 rule: https://www.govinfo.gov/content/pkg/FR-2021-10-28/pdf/2021-23311.pdf
- The Register's 2024 recommendation: https://www.copyright.gov/1201/2024/2024_Section_1201_Registers_Recommendation.pdf
- The 2012 rulemaking: https://www.eff.org/deeplinks/2012/11/2012-dmca-rulemaking-what-we-got-what-we-didnt-and-how-to-improve

US cases:
- *Universal v. Reimerdes*: https://cyber.harvard.edu/openlaw/DVD/NY/trial/op.html
- *Universal v. Corley*: https://law.resource.org/pub/us/case/reporter/F3/273/273.F3d.429.00-9185.html
- *Davidson v. Jung*: https://law.resource.org/pub/us/case/reporter/F3/422/422.F3d.630.04-3654.html
- *Chamberlain v. Skylink*: https://law.resource.org/pub/us/case/reporter/F3/381/381.F3d.1178.04-1118.html
- *MDY v. Blizzard*: https://cdn.ca9.uscourts.gov/datastore/opinions/2011/02/17/09-15932.pdf
- *Sega v. Accolade* (the Copyright Office's summary): https://www.copyright.gov/fair-use/summaries/segaenters-accolade-9thcir1992.pdf
- *Sony v. Connectix*: https://law.resource.org/pub/us/case/reporter/F3/203/203.F3d.596.99-15852.html
- *Sony v. Bleem* (the Copyright Office's summary): https://www.copyright.gov/fair-use/summaries/sonycomputer-bleem-9thcir2000.pdf
- The AACS key: https://blog.citp.princeton.edu/2007/05/01/aacs-plays-whack-mole-extracted-key and https://en.wikipedia.org/wiki/AACS_encryption_key_controversy

*SCEA v. Hotz* and Sony's 2011 notices:
- The docket: https://dn710603.ca.archive.org/0/items/gov.uscourts.cand.235965/gov.uscourts.cand.235965.docket.html
- Document 90, the subpoenas: https://storage.courtlistener.com/recap/gov.uscourts.cand.235965.90.0.pdf
- https://www.engadget.com/2011/01/12/sony-follows-up-officially-sues-geohot-and-fail0verflow-over-ps/
- https://www.engadget.com/2011/01/27/court-grants-sonys-temporary-restraining-order-against-geohot
- https://www.theregister.com/2011/03/05/geohot_visitors_unmasked/
- https://www.theregister.com/2011/03/16/geohot_paypal_records_subpoena/
- https://www.theregister.com/2011/04/11/sony_geohot_settle_playstation_suit/
- https://blog.playstation.com/2011/04/11/settlement-in-george-hotz-case/
- https://en.wikipedia.org/wiki/Sony_Computer_Entertainment_America,_Inc._v._Hotz
- GitHub's DMCA repository: the listings https://api.github.com/repos/github/dmca/contents/2011 and https://api.github.com/repos/github/dmca/contents/2012; https://raw.githubusercontent.com/github/dmca/master/2011/2011-01-27-sony.markdown and the Sony files of 2011-01-31, 2011-02-03 (with its counter-notice), 2011-06-07 and 2011-06-21 beside it.
- SCE Japan, 2012: https://gigazine.net/gsc_news/en/20121113-ps3-illegal-software

*Nintendo v. Tropic Haze* (yuzu) and what followed:
- The complaint: https://storage.courtlistener.com/recap/gov.uscourts.rid.56980/gov.uscourts.rid.56980.1.0.pdf
- The judgment: https://storage.courtlistener.com/recap/gov.uscourts.rid.56980/gov.uscourts.rid.56980.11.0.pdf
- https://www.gamingonlinux.com/2024/03/yuzu-agrees-to-pay-nintendo-2-4-million-and-will-entirely-shut-down
- https://www.gamingonlinux.com/2024/10/nintendo-switch-emulator-ryujinx-gets-shut-down/
- https://gigazine.net/gsc_news/en/20241002-switch-emulator-ryujinx-shuts-down-nintendo
- Nintendo's GitHub notices:
  - https://raw.githubusercontent.com/github/dmca/master/2023/05/2023-05-04-nintendo.md, and the -2, -3 and -4 files beside it
  - https://raw.githubusercontent.com/github/dmca/master/2024/04/2024-04-29-nintendo.md
  - https://raw.githubusercontent.com/github/dmca/master/2024/05/2024-05-02-nintendo.md, and the -2 file
  - https://raw.githubusercontent.com/github/dmca/master/2025/02/2025-02-26-nintendo.md
  - https://raw.githubusercontent.com/github/dmca/master/2026/02/2026-02-12-nintendo.md
  - https://raw.githubusercontent.com/github/dmca/master/2026/08/2026-08-17-nintendo.md
- *Nintendo v. Keighin* (D. Colo. 1:24-cv-03101), documents 36 to 38: https://storage.courtlistener.com/recap/gov.uscourts.cod.238647/gov.uscourts.cod.238647.36.0.pdf, and the same path with `.37.0.pdf` and `.38.0.pdf`
- https://torrentfreak.com/nintendo-easily-kills-4238-switch-emu-repos-on-yuzu-lawsuit-anniversary-250305/
- https://torrentfreak.com/nintendos-dmca-operation-continues-with-lockpick-kezplez-nx-takedowns-240508/
- https://www.nintendolife.com/news/2026/08/nintendo-wipes-out-hundreds-of-switch-emulator-repositories
- https://hitmarker.net/news/nintendo-lawyer-explains-why-it-has-been-tackling-illegal-emulators-following-yuzu-and-ryujinx-shutdowns-1275442

Dolphin and Steam:
- https://www.videogameschronicle.com/news/nintendo-has-blocked-the-steam-version-of-gamecube-and-wii-emulator-dolphin/
- https://www.videogameschronicle.com/news/gamecube-and-wii-emulator-dolphin-is-no-longer-coming-to-steam/
- https://www.readonlymemo.com/nintendo-vs-dolphin-on-steam-and
- https://gigazine.net/gsc_news/en/20230721-what-happened-to-dolphin-on-steam/
- https://store.steampowered.com/app/1941680/ (now lands on Steam's home page)
- Dolphin's `IOSC.cpp`: listed under Other emulators above.

Recompilation and decompilation ports:
- https://torrentfreak.com/nintendo-lawyers-file-copyright-complaints-against-super-mario-64-pc-port-200508/
- https://www.nintendolife.com/news/2020/05/nintendo_cracks_down_on_the_super_mario_64_pc_port
- https://github.com/HarbourMasters/Shipwright (README)
- https://torrentfreak.com/take-two-sues-enthusiasts-behind-gta-fan-projects-re3-revc-210903/
- https://torrentfreak.com/take-two-dismisses-claims-against-lead-defendants-in-gta-mods-lawsuit-230405/
- https://www.nintendolife.com/news/2020/11/nicalis_is_issuing_dmca_takedown_notices_to_free_versions_of_cave_story
- Zelda64Recomp and Unleashed Recompiled: the repositories listed above.

Sony, FromSoftware and PS3:
- https://www.pushsquare.com/news/2025/01/sony-issues-dmca-takedown-against-bloodborne-60fps-mod-four-years-after-release
- https://pushsquare.com/news/2025/02/copium-as-second-bloodborne-fan-project-hit-by-sony-dmca-takedown
- RPCS3's key table, for the count of entries: https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/key_vault.cpp
- PSJailbreak in Australia: https://www.theregister.com/2010/08/27/sony_oz_mod_chip_ban/, https://pulse.mallesons.com/ip-whiteboard/court-puts-a-temporary-stop-on-ps3-jailbreak/ and https://mcvuk.com/business-news/psjailbreak-banned-in-australia/
- Atlus and RPCS3 (Vice), and Sony's system software page and licence: listed above.

EU and UK:
- InfoSoc Directive 2001/29: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32001L0029
- Software Directive 2009/24: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- *Nintendo v PC Box*, C-355/12: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62012CJ0355
- *Top System*, C-13/20: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62020CJ0013
- *Sony v Datel*, C-159/23: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62023CJ0159
- CDPA s. 296: https://www.legislation.gov.uk/ukpga/1988/48/section/296, and the pages for ss. 296ZA to 296ZF beside it
- *Nintendo v Playables*: https://caselaw.nationalarchives.gov.uk/ewhc/ch/2010/1932
- *R v Higgs*: https://caselaw.nationalarchives.gov.uk/ewca/crim/2008/1324

Canada:
- Smart & Biggar's summary of *Nintendo v. King*: https://smartbiggar.ca/insights/publication/nintendo-awarded-12-7-million-for-circumvention-of-technological-protection-measures-in-precedent-setting-case
- Seen only as search results: https://mbm.com/lit_success_story/nintendo-of-america-inc-v-king/, https://www.dww.com/articles/game-over-federal-court-of-canada-puts-an-end-to-video-game-hacking-novel-case-0 and https://www.lawtimesnews.com/case-law/federal-court/circumventing-technological-protection-measures-constituting-copyright-infringement/265491
