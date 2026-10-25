# Launcher updates and a no-install patcher (research, 2026-10-18)

Research only. Nothing was built, lifted or run. The numbers come from build trees and logs that already existed (file sizes, ninja logs, `llvm-nm` and `llvm-readobj` on objects and on `des_boot.exe`), from the docs, from the sibling FFCCEOT-Recomp checkout (the source of the wrapper UI in docs/32), and from the web.

Three labels are used below:
- **(read)**: I read it in the source, a doc, a log or a web page.
- **(measured)**: counted or timed from files, logs or tools run on outputs that already existed.
- **(inferred)**: my conclusion, not checked. Section 9 lists runs that check the important ones.

The repository is written as `<owner>/DemonsSoulsRecomp`.

## Summary

- **The constraint (read).** `des_boot.exe` holds the compiled game: 298.7 MB, built from 94,017 lifted functions. It can never be shipped or patched by download (docs/05, README). Only our own code and third-party tools can ship. So the game part must be built on the player's PC, from the player's own disc. The Zelda64Recomp and UnleashedRecomp model (a prebuilt exe) is out. AC4-Port (docs/31) and FFCCEOT-Recomp build on the player's PC; they are the models to follow.
- **Patcher, recommended path.**
  1. Split the exe in two. `des_host.exe` holds the runtime, the port code and the UI; it is built in CI, signed and shipped. `des_game.dll` holds the lifted PPU and SPU code and libsre; it is built on the player's PC. The boundary is small (measured): the game-derived objects use 72 outside symbols, 76 with the current hooks. FFCCEOT-Recomp already ships this split (read).
  2. Ship a trimmed LLVM (clang, lld and clang's own headers: 68.8 MB zipped, measured) and the Python embeddable package (10.9 MB) inside the release zip. That is about 90 to 100 MB in all (inferred). The player installs nothing: no MSVC, Windows SDK, CMake, Ninja, Git or Python. Setup needs no network.
  3. Add a setup screen to `des_host.exe`. The UI already runs without the game: the crash dialog does (docs/32). Setup takes the disc folder and the player's RPCS3. It runs `rpcs3 --decrypt` on `EBOOT.BIN` and `libsre.sprx`, checks SHA-256 against known dumps, lifts with the bundled Python, compiles in parallel within the free RAM, links with lld, and compares the DLL's hash with the published one.
  4. Rebuild only when a "lift key" changes: lifter version, options, hooks, toolchain, flags or inputs.
- **What it costs the player (inferred from measured numbers).** About 11 minutes on this 8-core desktop (measured: 10.7 min wall, 41 min of CPU time). About 20 minutes on a 6-core desktop with 16 GB. 35 minutes to 2 hours on a 4-core laptop, the long end with 8 GB. About 4 GB of free disk during setup, about 0.3 GB after. `/O1`, a hot/cold split and smaller chunks may cut this; they need runs.
- **Updates, recommended in two stages.** *(Built 10-24: docs/105, the check and the update applied in the launcher, with one version kept to go back to. Not built from Stage B: signing, components, channels.)*
  - Stage A, first release: an update check with a banner and release notes (FFCCEOT-Recomp's 152-line WinHTTP check, ported) and a manual zip. Settings, saves, caches and the game build all live outside the install folder, so a manual update keeps them. The game is rebuilt only if the lift key changed.
  - Stage B, later, if wanted: apply updates in the app. A signed manifest (minisign format, checked with Monocypher), SHA-256 per component (Windows CNG), WinHTTP downloads with resume, side-by-side version folders under a small launcher stub, rollback to the kept version, and stable and beta channels. Download changed components instead of binary deltas. A typical update is then the 10 to 15 MB app part, because the toolchain rarely changes (inferred).
- **Signing.** Sign `des_host.exe` and the stub. Options: Azure Artifact Signing (about $9.99 a month, where eligible), SignPath Foundation (free for open-source projects) or an OV certificate ($150 to $300 a year) (read). The DLL built on the player's PC cannot be signed. It is never downloaded, so SmartScreen does not judge it, but Defender scans it (inferred).
- **Found while reading (read and measured).**
  - The exe needs the Visual C++ runtime DLLs, which are not part of Windows.
  - It imports Media Foundation directly, which Windows N editions lack.
  - The run defaults (LIBSRE_LLE and others) live in `tools/des/deslib.sh`, not in the exe. *(Since 10-22 the exe sets the ones a start needs itself; see 1.3.)*
  - The game's /dev_hdd1 cache is written into the disc folder.
  - Several paths are relative to the working folder.
  - Host code includes the generated `ppu_recomp.h`.
  - Two thread-local trampoline pointers are shared by the runtime and the lifted code. Windows cannot share a thread-local variable across a DLL boundary.
- **Effort.** Stage A: about 25 to 40 days (5 to 8 weeks). Stage B: about 7 to 10 days. First milestone, about 6 to 10 days: `des_game.dll` from today's lift, built by clang and lld alone, loaded by `des_host.exe`, reaching Boletaria at today's frame rate.
- **Not viable.**
  - Shipping game binaries, a cloud build, or deltas against game code (legal).
  - Translation at load: a new back end, and the same compile cost at first boot.
  - TinyCC: no optimiser, and `/Od` already made the main thread take about 70 ms a frame.

## 1. Today's pipeline, from the disc to a running exe

### 1.1 Steps (read)

From README "Building", docs/27 "Re-lift recipe", `scripts/` and `port/CMakeLists.txt`.

| # | Step | Tool and command | Output (measured sizes) |
|---|---|---|---|
| 1 | Decrypt the executable | RPCS3, Utilities > Decrypt PS3 Binaries | `work/game/EBOOT.ELF`, 28,222,408 bytes |
| 2 | Analyse it | `ppu_loader.py work/game/EBOOT.ELF -o work/loader` | `EBOOT.functions.json` (7.4 MB), `EBOOT.imports.json` |
| 3 | Lift the PPU code | `ppu_lifter.py` with `--override-funcs`, `--insn-hooks @port/insn_hooks.txt`, `--shadow-stack`, `-j 16` | `work/recomp`: 14 chunks, 437 MB of C++, 94,017 functions; "about 40 s" (docs/27) |
| 4 | HLE table | `gen_hle_nids.py --all` | `hle_nid_table.cpp`, made from ps3recomp's own sources; not game-derived |
| 5 | Boot shim | `scripts/apply_boot_patches.py work/recomp` | one patch in one chunk |
| 6 | SPU images and jobs | `extract_spu_images.py work/game/EBOOT.ELF --out work/spu_images`, then `scripts/build_spu.py` | 12 images (868 KB) and 2 job blobs, lifted to 15 MB of C, plus `_gen/spu_workloads.cpp` |
| 7 | libsre | `scripts/build_libsre.sh <libsre.prx>`: `lift_prx.py` (base 0x30000000), `ppu_lifter.py --raw` (TOC 0x30039AB0), `gen_libsre_exports.py` | `libsre_image.bin` (205,632 bytes), 51.9 MB of C, glue code |
| 8 | Build | CMake, Ninja and clang-cl under vcvars: `ps3recomp_runtime`, `rsx_shader_precompile`, `des_ui` (FetchContent of FreeType 2.14.3 and RmlUi 6.3), `des_boot` | `des_boot.exe` 298.7 MB, `des_boot.map` 26 MB; runtime lib 5.1 MB, UI lib 16.9 MB |
| 9 | Run | `tools/des/play.sh`: environment defaults, a staged copy, `des_boot.exe work/game/EBOOT.ELF` | saves in `gamedata/dev_hdd0/...`; shader cache in `gamedata/shader_cache` (52 MB, 3,907 files) |

Notes:
- The two job blobs are plain EBOOT bytes: 384 bytes at 0x0177B480 and 2,064 bytes at 0x0177B600 (measured: they match the ELF at those addresses). The pipeline can cut them from the EBOOT; no run is needed to dump them.
- The icon comes from `work/game/ICON0.PNG` through `scripts/make_icon.py`, so it is game data. CMake also runs `scripts/gen_taskset_policy_blob.py` on libsre at configure time; that blob is an optional A/B oracle.
- Machine needs today (read, README): Visual Studio 2022 or its Build Tools (MSVC, Windows SDK), LLVM clang-cl, CMake, Ninja, Python 3 with `requirements.txt`, Git Bash. `scripts/vsenv.bat` and `tools/des/build.sh` hard-code tool paths.
- The lift chain imports only the Python standard library and its own modules. This covers `ppu_loader`, `ppu_lifter`, `spu_lifter`, `lift_prx`, `extract_spu_images`, `gen_hle_nids` and `wrap_spu_elf` (read). The packages in `requirements.txt` (pycryptodome, capstone, construct, tabulate, tqdm) are not needed for it.

### 1.2 Build cost (measured)

This PC: Ryzen 7 9800X3D, 8 cores and 16 threads, 31.1 GB of RAM. Times from the ninja logs.

| Tree | Flags | Jobs | Per PPU chunk | Wall | CPU time |
|---|---|---|---|---|---|
| `port/build_cam` (2026-10-18) | `/O2` | 4 | 156 to 267 s | 10.7 min (PPU done at 642.9 s) | PPU 41 min (2,468 s); SPU about 83 s; libsre 27 s |
| `port/build` | `/O2` | 14 at once | 255 to 431 s | 7.2 min | 69 min |
| `port/build_hyb` (older lift) | `/Od`, chunk 005 at `/O2` | 4 | about 20 s; chunk 005 78 s | 1.7 min (103 s) | not computed |

- Chunks 012 and 013 are small: about 6 to 8 s and 2 to 3 s.
- Linking `des_boot.exe` with lld-link takes 1 to 4 s (ninja log entries).
- Objects: 1,357 MB in all with `/Zi`; each big PPU chunk object is 106 to 124 MB.
- RAM (read, comment in `port/CMakeLists.txt`): "One chunk at /O2 takes ~2 minutes and ~3 GB with clang-cl". The same file notes that `/Od` made main and ChrInsManThread take about 70 ms a frame.

### 1.3 What the exe expects at run time (read)

- **Defaults from the scripts.** `des_env_defaults` in `tools/des/deslib.sh` sets these:
  - `PS3_VFS_ROOT`;
  - `LIBSRE_LLE=1`;
  - `LIBSRE_LLE_ONLY` (8 function families);
  - `SPURS_NO_ROUND_NOTIFY=1`;
  - `VP_VB_VERTS=1572864`;
  - `PS3_SHADER_CACHE`.

  Nothing in `port/` sets LIBSRE_LLE. An exe started from Explorer runs without these defaults.

  *Update 10-22:* without them a player's start from setup hung on the loading screen (docs/88 section 11). The host now sets `LIBSRE_LLE=1`, `LIBSRE_LLE_ONLY`, `SPURS_NO_ROUND_NOTIFY=1`, `VP_VB_VERTS` and `PS3_VERBOSE=0` itself at the start of `main`, each only when the environment lacks it (`run_defaults` in `port/des_options.cpp`). `LIBSRE_LLE` is read by the generated libsre glue (`scripts/gen_libsre_exports.py`), not by `port/` or the runtime, so a search of those finds nothing; without it libsre is never loaded. `PS3_VFS_ROOT` comes from setup's `ready.txt` (docs/88 section 7), and the shader cache keeps its default beside the exe.
- **Paths relative to the working folder.**
  - The libsre image is `work/libsre/libsre_image.bin` (in the glue from `gen_libsre_exports.py`).
  - `/dev_hdd0/` maps to `gamedata/dev_hdd0/` (`libs/filesystem/cellFs.c`).
  - The shader cache is `shader_cache` beside the exe unless `PS3_SHADER_CACHE` is set (`rsx_d3d12_backend.c`).
- **/dev_hdd1 goes into the disc folder.** `host_path` in `runtime/ppu/ppu_fs.cpp` maps `/dev_hdd1/` under the VFS root, so the game's cache lands in `<disc folder>/cache`. `PS3_HDD0_ROOT` and `PS3_DEV_FLASH` exist as overrides; /dev_hdd1 has none.
- **DLL imports of `des_boot.exe` (measured with llvm-readobj).**
  - `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`: the Visual C++ runtime, which is not part of Windows. No CMake file sets `CMAKE_MSVC_RUNTIME_LIBRARY`, so the build uses the default `/MD`.
  - `MFPlat.DLL`: Media Foundation, used by `libs/codec/sail_movie.cpp`. Windows N editions lack it unless the Media Feature Pack is installed, and a missing direct import stops the exe from starting (inferred).
  - The rest are system DLLs: d3d12, dxgi, D3DCOMPILER_47, XINPUT1_4, HID and others.
- **Host code depends on the lift.**
  - Host files include the generated `ppu_recomp.h`. In the port: `des_heap.cpp`, `des_menu_sfx.cpp`, `des_options.cpp`, `missing_stubs.cpp`. In the runtime: `ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `boot_main.cpp`.
  - `ppu_recomp_register` (`ppu_loader.cpp`) and `boot_main.cpp` read the lifter's `function_table` and `function_table_count`.
  - The port calls lifted functions by name. The `port/build` objects have 6 such calls: 4 in `des_options`, 2 in `des_menu_sfx` (measured). The working copy of `des_options.cpp` names about 169 lifted functions, mostly tables for the UI timing fix (read).
  - So today the host cannot be built without a lift.

## 2. The legal frame, and how others ship

### 2.1 What may ship, and what never ships

May ship (read docs/05 and the README; inferred for the new items):
- our code: the host exe, the port, the UI, our headers and scripts;
- ps3recomp's runtime and Python tools (MIT);
- tables of known input hashes, and guest addresses (the repository already ships addresses in `port/insn_hooks.txt`);
- third-party tools with their licence texts:
  - LLVM (Apache-2.0 with the LLVM exception);
  - the Python embeddable package (PSF licence, plus the licences of the libraries it bundles);
  - Monocypher (CC0 or BSD-2);
  - FreeType and RmlUi (already in the host).

Never ships (read docs/05; inferred for the new items):
- the EBOOT, encrypted or not;
- `libsre.prx` or `.sprx`, or any firmware file; keys;
- the lifted C and C++, its objects, `des_game.dll` and its map;
- the function list JSON, the SPU images and job blobs;
- the shader cache, `ICON0.PNG`, saves.

Never upload `des_game.dll` anywhere: not to VirusTotal, not with a crash report, not to a Microsoft false-positive form. Today the crash report zip holds `crash.txt`, `system.txt`, `settings.ini` and `des.log`, not the minidump (read, docs/32). Keep it that way.

Decryption stays inside the player's own RPCS3. We only start it as a separate program. No keys and no RPCS3 code go in our download. The residual risks in docs/05 (DMCA section 1201, trademarks) stay as they are.

### 2.2 What other projects ship (read)

| Project | The player downloads | Game code in the download? | The player supplies | Updates |
|---|---|---|---|---|
| Zelda64Recomp (N64Recomp) | prebuilt zip; v1.2.2 for Windows is 28,940,501 bytes; "Building is not required to play" | yes: the build needs the ROM and N64Recomp "to generate the C code to be compiled" | the ROM, picked in the menu | manual; saves kept apart; `portable.txt` |
| UnleashedRecomp (XenonRecomp) | prebuilt zip; v1.0.3 for Windows is 53,386,467 bytes | yes: the build needs `default.xex`, `default.xexp` and `shader.ar` | game files, through an in-app installer with hash checks | extract over the old copy; saves and config kept |
| AC4-Port (docs/31) | a launcher that fetches pinned llvm-mingw, CMake and Ninja (SHA-256 pinned, "about 250 MB", about 4 GB of disk) | no: built on the player's PC "in a few minutes" | the disc folder, RPCS3, `PS3UPDAT.PUP` 4.93 | manual launcher downloads; "rebuilds only what changed" |
| FFCCEOT-Recomp (sibling port) | zip with the host and clang plus lld; 0.2.0-alpha is 93,086,230 bytes, the Linux AppImage 210,737,656 (measured, local `dist/`) | no: the game module is built on the player's PC at `/O1` | the ROM (SHA-1 checked, copied) | check plus banner plus release page; no self-replacement |

Two of these build on the player's PC. AC4-Port matches our inputs: a PS3 disc, RPCS3 for decryption, firmware. FFCCEOT-Recomp matches the build we want: a freestanding game module, a bundled clang and lld, a setup screen and an update check. Its `docs/PACKAGING.md` notes that "Nobody compiles game code on the player's PC" among the projects it surveyed. AC4-Port was not in that survey.

## 3. Patcher options

### 3.1 Option 1: bundle Python and a portable clang plus lld

Sizes:

| Item | Size | Source |
|---|---|---|
| LLVM 22.1.8: `clang.exe` (`clang-cl.exe` is an identical copy) | 104.7 MB | measured, local install |
| `lld.exe` / `lld-link.exe` | 71.5 MB | measured |
| clang's own headers | 16 MB | measured |
| clang + lld + headers (303 files) | 191.8 MB raw, 68.8 MB zipped (deflate level 9), 48.7 MB with xz | measured |
| whole LLVM install, for comparison | 2.9 GB; installer 455,545,840 bytes; tar.xz 862,053,924 bytes | measured; read (release assets) |
| Python 3.13.9 embeddable, amd64 | 10,926,675 bytes zipped | read (python.org) |
| FFCCEOT-Recomp's Windows folder: `clang.exe` 104,745,472, `lld-link.exe` 71,456,768, `ffcc.exe` 15,088,128, `ffcc.pdb` 77,819,904; 320 files | zip 93 MB | measured, local `dist/` |

- Use zip, not 7z or xz. Windows 10 can only open zip files by itself (inferred).
- **Python.** The embeddable package's `._pth` file replaces the default `sys.path` and turns on isolated mode. It then ignores `PYTHONPATH`, `PYTHONHOME` and the registry (read). That protects us from other Pythons on the player's PC. List the lifter folder in the `._pth`; the script's own folder is probably not added (inferred, to check). The lifters run their `-j` workers as processes, and `build_spu.py` starts `sys.executable`. Both should work with the embedded `python.exe` (inferred).
- **Licences.** LLVM is Apache-2.0 with the LLVM exception: ship `LICENSE.TXT`. Python is under the PSF licence; ship its licence file, which covers the bundled libraries. Both allow redistribution (read).
- **Compile time on player PCs (inferred).** Jobs = (free RAM - 2 GB) / 3 GB, capped at the logical CPU count. Per-core speed is scaled from this PC.

| PC | Jobs | PPU compile at `/O2` |
|---|---|---|
| this PC: 8 cores, 16 threads, 31 GB | 4 (14 tried: 7.2 min) | 10.7 min (measured) |
| 6-core desktop, 16 GB | 4 | about 20 min |
| 4-core laptop, 16 GB | 4, limited by heat | about 35 to 50 min |
| 4-core laptop, 8 GB | 1 to 2 | about 1 to 2 hours |

SPU and libsre add about 2 minutes of CPU time (measured here). The lift itself adds 1 to 3 minutes (docs/27: 40 s on 16 workers).

### 3.2 Option 2: ship a prebuilt host, compile only the game part

**Why a DLL, not a whole exe linked on the player's PC (inferred).** A whole exe needs the MSVC static CRT libraries and the Windows SDK import libraries at link time. Their licences do not let us ship them. A freestanding DLL links with nothing but our own files.

FFCCEOT-Recomp did exactly this (read, its `docs/PACKAGING.md` and `game/src/setup.cpp`):
- compile with `clang --target=x86_64-pc-windows-msvc -c -O1 -w -ffreestanding -fno-stack-protector -nostdlibinc`;
- link with `lld-link /dll /noentry /nodefaultlib /noimplib`;
- 65 files compiled in 11 s on 16 threads;
- the game ran pixel-identical to the CMake build over 8,000 frames.

Its module exports one function, `ntr_game_load`. That function receives a table of host services, and the generated code calls them through macros. FFCCEOT notes this "costs the same as a call through an import table".

**The boundary today (measured).** `llvm-nm` on the 14 PPU chunk objects, the 14 SPU objects, the lifted libsre and its glue in `port/build` finds 219,142 defined symbols and 72 symbols that come from outside:

| Group | Symbols | Change (inferred) |
|---|---|---|
| Runtime functions (31) | `vm_read8/16/32/64`, `vm_write8/16/32/64`, `ps3_indirect_call`, `ps3_hle_call`, `lv2_syscall`, `ppu_stwcx32`, `ppu_stdcx64`, `ppu_timebase_now`, `ppu_register_function`, `prx_load_module`, `prx_image_load_file`, and 14 `spu_*` functions (`rdch`, `wrch`, `rchcnt`, `stop`, `halt`, `indirect_branch`, `drain_call`, `begin_image`, `register_function`, `workload_register_img`, `ls_watch_slow`, `hook_ring_fetch`, `hook_put_wait`, `hook_inflate_ls`) | calls through a host table filled at load, as in FFCCEOT |
| Port functions (4) | `ds_malloc`, `ds_free`, `des_move_post`, `des_menu_tick`; the newer `build_cam` lift adds `des_cam_rate`, `des_slide_rate`, `des_dof`, `des_motion_blur` | the same table, built from the override list and `insn_hooks.txt` |
| Globals (16) | `vm_base`, `g_vm_inl_rd_lim`, `g_vm_inl_wr_lim`, `g_vm_inl_rd_hole`, `g_spu_coh_bitmap`, `g_spu_coh_armed_line`, `g_spu_goto_slow`, `g_spu_ls_watch_n`, `g_spu_ch_diag`, `g_spu_ch_diag_gen`, `g_pm_flow_ctx`, `g_yz_lockstep_on`, `g_extra_symtab`, `g_extra_symtab_count`; thread-local: `g_trampoline_fn`, `g_spu_trampoline_fn` | the ones the inline memory accessors read move into the DLL, and the host writes them through pointers; the two thread-locals move into the PPU and SPU context structs (or into the DLL, with per-thread pointers for the host); the rest through the table |
| C library (14) | `sqrt`, `sqrtf`, `fma`, `_fdsign` in the PPU chunks and libsre; `ldexp`, `exp2`, `getenv`, `atoi`, `strtoul`, `fprintf` (`__stdio_common_vfprintf`, `__acrt_iob_func`), `fputc`, `fwrite`, `fflush` in the SPU objects and the libsre glue | `sqrt` inline with `-fno-math-errno`; `signbit` as a builtin; `fma`, `ldexp` and `exp2` from a small support file or the host; the SPU and glue log lines and knobs call host functions instead of stdio and `getenv` |
| Compiler runtime (6) | `__security_cookie`, `__security_check_cookie` (stack protector), `_tls_index`, `__tls_guard`, `__dyn_tls_on_demand_init` (C++ thread-locals), `_fltused` | `-fno-stack-protector`; no C++ thread-locals in the module; `_fltused` defined in the support file |

The thread-locals are a real blocker, not a detail. `g_trampoline_fn` is defined in `runtime/ppu/ppu_loader.cpp` (line 2266). `g_spu_trampoline_fn` is declared in `runtime/spu/spu_context.h` (line 842). The runtime and the lifted code both use them on hot paths: `ps3_indirect_call_impl`, `icall_run`, `ppu_run`, `spu_indirect_branch`, `spu_drain_call`, and the SPU macros in `spu_context.h` (read). Windows cannot import a thread-local variable from another module (inferred, standard Windows behaviour).

The host side changes too:
- `function_table` and `function_table_count`, the SPU workload registration and the libsre glue all reach the host through one exported `des_game_init(host_api, game_api)`. Today the SPU workloads register through `__attribute__((constructor))` in `_gen/spu_workloads.cpp`. A DLL linked with `/noentry` runs no static constructors, so that registration becomes an explicit call.
- Named calls from the port (`func_008EF430(ctx)` and the tables in `des_options.cpp`) become pointers resolved by guest address at init. Template arguments such as `gauge_len_rate<func_...>` need a run-time form.
- Split `ppu_recomp.h` in two. A fixed runtime header (`ppu_context`, the accessors, the macros) ships with the host and carries an ABI number. The generated part adds the per-game declarations. Then the host builds in CI with no lift. A module built for another ABI number is refused at load, as FFCCEOT does.
- `gen_hle_nids.py --all` output moves into the host build, because it is not game-derived. The icon is set at run time from the player's `ICON0.PNG` (window icon and shortcut), not embedded. The taskset-policy blob stays a developer option.
- Keep unwind tables. `sys_ppu_thread_exit` uses `longjmp(s_exit_jmp, 1)` through lifted frames (read), and Windows x64 needs `.pdata` in the DLL for that. clang emits it by default (inferred).
- Link the host with the static CRT (`/MT`) for runtime, UI, FreeType and RmlUi. The host is then the only CRT user, and players need no Visual C++ runtime. Delay-load `mfplat.dll` and skip the movies when it is missing. Add the UTF-8 code page to the exe manifest, so the runtime's narrow `fopen` paths work for non-ASCII user names (inferred).
- The DLL will be about 280 to 290 MB: the exe minus the host (inferred).

**Performance (inferred).** Calls into the host become indirect calls, like import-table calls. `ps3_indirect_call` alone has 9,509 call sites in chunks 000 and 005 (measured). The memory accessors stay inline and read DLL-owned globals as they read exe globals today. I expect no visible change, but it needs the A/B in run request R3.

### 3.3 Option 3: lift at install, or translate at load

- **Lift at install.** The proposal above.
- **Translate at load.** This is RPCS3's way: LLVM compiles the PPU code at first boot and caches it. For us it would mean putting LLVM inside the exe and replacing the lifter's Python-to-C++ back end with an in-process translator. The compile work is the same code, so the first boot would take as long as setup. The only gain is that the separate setup step disappears. The cost is weeks to months of new back-end work. Verdict: no.
- **Lift lazily per function while the game runs.** That is a dynamic recompiler, which is an emulator. Out of scope.

Both keep the legal position of option 1: no game code is downloaded.

### 3.4 Option 4: other ideas

| Idea | Verdict | Why |
|---|---|---|
| Full llvm-mingw, link a whole exe on the player's PC (AC4-Port) | no | 190,836,485-byte zip (read). The runtime would need a mingw port: SEH and VEH, MSVC intrinsics, D3D12 headers (docs/31). |
| Zig as the compiler (100,268,137-byte zip) | no | Bigger than our trimmed clang plus lld, and it adds nothing for a freestanding DLL. |
| TinyCC or libtcc | no | No optimiser, and C only. `/Od` already made main and ChrInsManThread take about 70 ms a frame. |
| Port the lifters to C++ inside the host (FFCCEOT's recompiler is in its exe) | not now | Saves 11 MB of Python. Costs weeks, and forks the lifters from ps3recomp's Python tools. |
| PyInstaller or Nuitka builds of the lifters | no | Frozen-Python exes are a common antivirus false positive. The embeddable package is simpler. |
| A cloud build service | never | We would make and send copies of the game code. |
| Deltas against the lift or the DLL | never | A delta that rebuilds game code from our files carries game code. |
| A prebuilt shader cache | never | It is compiled from the game's shaders. The first-run prework builds it: 3,535 shaders in 28 s on 15 threads (docs/32). |
| CPU-specific flags (x86-64-v3 when the CPU has AVX2 and FMA) | later | Free speed, since we compile on the player's PC. Needs one expected hash per flag set. |
| A smaller custom LLVM (x86 target only, static) | later | Maybe 20 to 30 MB less to download (inferred). |

### 3.5 Choice

Options 1 and 2 together. Ship a prebuilt, signed host and a trimmed clang, lld and Python. Compile only the lifted code, as a freestanding DLL, on the player's PC.

## 4. The proposed setup flow

### 4.1 Steps

1. **Start.** `des_host.exe` looks for `build\<lift key>\des_game.dll` with a matching manifest. Found: the game boots behind the start screen, as today. Not found: the setup screen opens. It is the same RmlUi host on its own D3D12 device, as the crash dialog uses (docs/32).
2. **Disc.** The player picks the folder that holds `PS3_GAME`. Check `PARAM.SFO` for BLUS30443 version 01.00 and check that `EBOOT.BIN` is there. Another region or version gets a clear message; the port targets one version (docs/29, M9). The disc folder is never written to.
3. **RPCS3.**
   - The player picks `rpcs3.exe`.
   - Probe `rpcs3.exe --help` for `--decrypt`, as AC4-Port does.
   - Look for `dev_flash/sys/external/libsre.sprx` in RPCS3's folder. If it is missing, ask the player to install `PS3UPDAT.PUP` in RPCS3. Its `--installfw` needs the GUI: "Cannot perform installation in no-gui mode!" (read, `rpcs3/rpcs3.cpp`).
4. **Decrypt.**
   - Copy `EBOOT.BIN` and `libsre.sprx` into a temporary work folder.
   - Run `rpcs3.exe --decrypt <files>` with standard input closed. RPCS3 asks for a KLIC only for NPDRM files, and empty input ends that prompt (read, `rpcs3.cpp`).
   - Collect the outputs. Their names are not confirmed (section 8).
   - Also accept an already decrypted `EBOOT.ELF` and `libsre.prx`, with the same checks.
5. **Check.** Compare the SHA-256 of `EBOOT.ELF` and `libsre.prx` with a table in the exe, as AC4-Port's `known_inputs.cpp` and FFCCEOT's `verify_rom` do. An unknown file stops setup and shows the expected and actual hashes side by side.
6. **Lift.** The bundled Python runs one pipeline script, a thin wrapper over today's scripts:
   - `ppu_loader`;
   - `ppu_lifter`, with options from the app's own files (the override list and `insn_hooks.txt`);
   - `apply_boot_patches`;
   - `extract_spu_images`, plus the job blobs cut from the EBOOT;
   - `build_spu`;
   - `lift_prx`, the libsre lift and the glue.

   The output goes to `%LOCALAPPDATA%\DemonsSoulsRecomp\build\<lift key>\src`.
7. **Compile.**
   - Start the largest files first.
   - Run as many jobs as RAM and cores allow (4.3).
   - Show progress per file, and offer Cancel.
   - On a failure, show the last 1,500 characters of the compiler's output, as FFCCEOT does.
8. **Link.** Run `lld-link /dll /noentry /nodefaultlib` to make `des_game.dll`, plus a local map for crash symbols.
9. **Verify.** Compare the DLL's SHA-256 with the release's expected value for this flag set (4.5). On a mismatch, warn, keep the log and offer Rebuild and Play anyway.
10. **Clean up.** Delete the sources and objects, or keep the objects if the incremental cache is on. Write the manifest. Delete older builds once the new one has booted.
11. **First boot.** The shader prework runs (docs/32), then the start screen opens.

### 4.2 The lift key

A SHA-256 over:
- the lifter and script versions (the app ships them);
- the lifter options, the override list and the `insn_hooks.txt` contents;
- the boot-patch version;
- the SPU build options (regions, native hooks and blocks);
- the host-module ABI number;
- the hash of the clang and lld binaries, and the compile flags;
- the hashes of `EBOOT.ELF` and `libsre.prx`.

An app update that changes none of these reuses the old build. FFCCEOT keys its build the same way: ABI version, generator id and ROM hash.

### 4.3 The compile driver

- **Jobs.** min(logical CPUs, (free RAM - 2 GB) / RAM per job). Use 3 GB per job for the 600,000-line chunks (read, CMake comment) until run request R2 measures it.
- **Priority.** Below normal, so the PC stays usable. The test scripts already do this (`PS3_PRIORITY=below` in deslib).
- **Sleep.** Hold a power request while building, so the PC does not sleep halfway (inferred).
- **Resume.** Write a hash file next to each object. After a cancel, a crash or a reboot, skip the objects whose hash still matches.
- **Disk.** Check for 4 GB free before starting. The peak with `/Zi` objects is about 2.2 GB: 0.5 GB of sources, 1.36 GB of objects, 0.3 GB of DLL (measured sizes). Player builds need no `/Zi`, so they should use less (unmeasured).
- **Log.** Write `setup.log` in the data folder. A report zip includes it after redaction (the docs/32 report path).

### 4.4 Where files go

| What | Where | Why |
|---|---|---|
| host, scripts, toolchain | the folder the player extracted the zip to, or `app\<version>\` and `tools\` under it in stage B | read-only after install; no admin |
| `settings.ini`, logs, crashes, reports | `%APPDATA%\DemonsSoulsRecomp\DemonsSouls\` (as now, docs/32) | small and per-user |
| saves (`dev_hdd0`) | the same data folder | survive reinstalls; small |
| game build, objects, `/dev_hdd1` cache, shader cache, downloads | `%LOCALAPPDATA%\DemonsSoulsRecomp\` | large; not roamed or synced; Controlled Folder Access does not cover AppData by default (inferred) |
| everything, in portable mode | `<exe folder>\data\` when `portable.txt` is there (docs/32) | as today |

Today's cwd-relative paths (`gamedata\`, `work\libsre\libsre_image.bin`, `shader_cache`) and the run defaults from `deslib.sh` move into the exe. The new /dev_hdd1 override points at the local folder. Warn when the install or portable folder is under OneDrive.

### 4.5 The same DLL on every PC

- Use `/Brepro` in lld-link, prefix maps in clang (`-ffile-prefix-map`), no `/Zi` (or `/Zi` with prefix maps), sorted inputs and a pinned clang. Every PC should then produce a byte-identical DLL (inferred; run requests R5 and R6).
- Before each release, the maintainer builds the DLL from their own copy and publishes only its SHA-256, one per flag set. A hash is not game code.
- Crash reports can name game frames without any map: the runtime's function table maps an address in the DLL to `func_<guest address>+offset` (inferred). With identical builds, the maintainer can also rebuild the same DLL and map from their own copy.

### 4.6 First run, end to end (inferred)

| Step | Time |
|---|---|
| Pick the folders, decrypt, check the hashes | 1 to 2 min, mostly the player |
| Loader, lift, SPU and libsre lifts | 1 to 3 min |
| Compile | 11 min to 2 hours (3.1) |
| Link | seconds (1 to 4 s here, measured) |
| First-run shader prework | under a minute (docs/32: 28 s on 15 threads) |

### 4.7 Shorter builds, to measure

- **`/O1` everywhere.** FFCCEOT measured `/O1` as fast as `/O2` in its game, but its compile times were equal too: 17 s for both (read). For Demon's Souls both effects are unknown (R3).
- **Hot and cold.** `/O2` for functions that run every frame, from a list of addresses shipped as data, and `/O1` for the rest. This needs the lifter to put hot and cold functions in separate chunks. The risk is a function that is cold in the Palace but hot in a later level.
- **Smaller chunks.** `--chunk-lines` defaults to 600,000 (read, `ppu_lifter.py`). Smaller chunks balance the load better and need less RAM per job. That matters most on 8 GB PCs (R4).
- **No `/Zi` for players.** Smaller objects and less disk; the time saved is unmeasured.

## 5. Updates

### 5.1 The feed

- Use GitHub Releases on `<owner>/DemonsSoulsRecomp`. The repository is public and has no releases yet (read from the API, 2026-10-18).
- **Stable channel.** `https://github.com/<owner>/DemonsSoulsRecomp/releases/latest/download/update.json` redirects to that file in the newest release. "Latest" means the most recent release that is neither a prerelease nor a draft, sorted by `created_at`, which is the date of the commit used for the release (read). So tag releases in commit order.
- **Beta channel.** `GET /repos/<owner>/DemonsSoulsRecomp/releases?per_page=10` lists prereleases too. Take the newest one that has a manifest. FFCCEOT uses this list because "latest" skips prereleases (read).
- **Limits (read).**
  - Unauthenticated REST calls: 60 an hour per IP.
  - Release files: up to 2 GiB each, 1,000 assets per release, no bandwidth limit.
  - The `latest/download` redirect is a web download, not a REST call (inferred).
- **Version.** Today `DES_VERSION` is the git short hash (`port/ui/src/version.hpp`). Updates need a version from the tag. FFCCEOT's `is_newer` compares major.minor.patch, then -alpha, -beta and -rc.

### 5.2 Integrity

- HTTPS through WinHTTP, with the system's certificate store, is the base.
- GitHub gives each release asset a sha256 `digest` (since 2025-06-03). Immutable releases (generally available 2025-10-28) lock the assets and the tag once a release is published, and add attestations (read). Turn immutability on.
- **Our own signature on top**, so that a stolen GitHub account cannot push an update by itself.
  - Each release carries `update.json` and `update.json.minisig`. minisign signs a BLAKE2b-512 prehash of the file with Ed25519; the algorithm id is "ED" (read).
  - Monocypher verifies it: BLAKE2b plus its optional Ed25519 (SHA-512) check. It is a few C files under CC0 or BSD-2 (inferred from its API; check the version).
  - Windows CNG has no Ed25519; Curve25519 there is for key agreement only (read). The zero-dependency alternative is ECDSA P-256 with `BCryptVerifySignature`.
- **Keys.** The private key stays offline with the maintainer. Releases are published by hand as drafts, as FFCCEOT does. Embed two public keys in the exe, the current one and the next one, so the key can be rotated.
- **Downgrades.** Refuse a manifest whose version is lower than the running one. A rollback uses the kept local copy, never a download.
- **Files.** Check the SHA-256 of each component, from the manifest, with CNG `BCryptHash` before extracting.

Manifest example:

```json
{
  "version": "0.4.0",
  "channel": "stable",
  "notes_url": "https://github.com/<owner>/DemonsSoulsRecomp/releases/tag/v0.4.0",
  "min_stub": 1,
  "lift_key_inputs": "a1b2...",
  "components": [
    { "name": "app",    "file": "des-app-0.4.0.zip",         "size": 12000000, "sha256": "..." },
    { "name": "llvm",   "file": "llvm-22.1.8-trimmed.zip",   "size": 68800000, "sha256": "..." },
    { "name": "python", "file": "python-3.13.9-embed.zip",   "size": 10926675, "sha256": "..." }
  ],
  "expected_dll": { "baseline": "sha256...", "x86-64-v3": "sha256..." }
}
```

### 5.3 Code signing (read, Microsoft's code signing options page dated 2026-08-29, and SignPath)

| Option | Cost | Notes |
|---|---|---|
| Azure Artifact Signing | about $9.99 a month | Individuals only in the USA and Canada. Organisations in the USA, Canada, the EU and the UK. Identity check takes a few business days. Reputation builds over time. |
| SignPath Foundation | free for OSI-licensed projects | The signer shown is SignPath Foundation. Needs automated builds and their review. The repository is MIT. Whether they accept a game port is unknown. |
| OV certificate from a CA | $150 to $300 a year | The private key must be on hardware (HSM) since June 2023. Reputation builds over time. |
| EV certificate | $400 or more a year | No longer skips SmartScreen's reputation check (since 2024). |
| none | 0 | Every download shows "Windows protected your PC" until the player clicks through. |

Sign `des_host.exe` and the stub. The bundled `python.exe` is signed by its publisher (inferred). Whether LLVM's `clang.exe` and `lld-link.exe` are signed is unknown (section 8).

### 5.4 Downloads

- Use WinHTTP with `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` (Windows 8.1 and later; read). It follows GitHub's redirect to its download host.
- Resume with Range requests into `downloads\<sha256>.part`. Cap the size at the manifest's value.
- Timeouts: 5 s to connect, 8 s to receive. These are FFCCEOT's values.
- The User-Agent carries only the app name and version. FFCCEOT's `http_get` (WinHTTP, about 60 lines) covers most of this.

### 5.5 Install layout and swapping a running exe (stage B)

```
<install>\                          any folder the player can write to; no admin
  DemonsSoulsRecomp.exe             stub: about 100-200 KB, static CRT, signed
  app\current.txt                   "0.4.0"
  app\0.4.0\des_host.exe ...        host, UI files, lifter scripts
  app\0.3.2\...                     previous version, kept for rollback
  tools\llvm-22.1.8-<hash>\         clang, lld, headers; shared by versions
  tools\python-3.13.9-<hash>\       embeddable Python
```

- **Swap.**
  1. Download into `downloads\` and verify.
  2. Extract to `app\<new>.tmp`, then rename it to `app\<new>`.
  3. Write `current.txt.tmp` and replace `current.txt` with `MoveFileEx` (`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`).
  4. Start the stub and exit. The stub reads `current.txt` and starts that version's host.
  5. If the lift key changed, the setup screen builds into a new `build\<key>\` while the old build stays.
- **Never update while the game runs.** Every host holds a named mutex. The apply step waits until it is the only holder.
- **Rename and replace for the stub itself.** A running exe can be renamed but not overwritten or deleted. Rename `DemonsSoulsRecomp.exe` to `.old`, move the new one into place, and delete `.old` on the next start. Deleting at reboot (`MOVEFILE_DELAY_UNTIL_REBOOT`) needs admin, so it is not used (read, MoveFileEx). The same applies to `des_game.dll`, which is why builds go into a new folder.
- **Stage A has no stub.** The zip holds `des_host.exe`, `tools\` and the lifter scripts at the top. Stage B adds the stub later without moving the data.

### 5.6 Full downloads or deltas

- Download per component. The app part is about 10 to 15 MB zipped (inferred). LLVM (68.8 MB) and Python (10.9 MB) download only when we change their version. Most updates are then the app part only.
- Binary deltas (zstd `--patch-from`, Velopack's deltas) would save a few MB more per update. Not worth the code now.
- The game DLL is never downloaded. When the lift key changes, the player's PC rebuilds it. That rebuild, 10 minutes to 2 hours, is the real cost of an update, so release notes should say whether an update needs it.
- **Incremental rebuilds (inferred).** Keep a hash per chunk (source text, flags, toolchain) and reuse unchanged objects. With 14 large chunks that share one header, most lifter changes touch every chunk. Smaller chunks with stable boundaries, and per-game declarations kept out of the shared header, would let a single new hook rebuild one chunk.

### 5.7 Settings and saves

- **`settings.ini`** stays in the data folder (docs/32). Rules (inferred; check the current writer): keep unknown keys when writing, add new keys with defaults, and never drop a key on a downgrade.
- **Saves** stay in the data folder and updates never touch them. Before any change of layout, copy them to a dated backup and keep the backup.
- **Caches** (shader cache, /dev_hdd1 cache, builds) are safe to delete; they are rebuilt on demand. The shader cache is 52 MB here (measured).

### 5.8 Rollback

- Keep one previous app version and its build.
- Offer "Go back to version X" in Settings > About.
- Offer it on its own after two crashes in a row before the title screen. The crash handler counts crashes in the data folder, and reaching the start screen resets the count (inferred).
- A rollback never downloads anything.

### 5.9 Channels

- **Stable:** the newest normal release.
- **Beta:** prereleases as well.
- **Development builds:** numbered prereleases. A rolling "nightly" tag would need its assets replaced, which immutable releases forbid.
- The channel is a setting, and stable is the default.

### 5.10 Offline

- Check at most once a day, on a background thread.
- A failure is silent apart from one log line.
- A "Check for updates" setting, on by default, as FFCCEOT does. Test runs never check.
- Setup never needs the network, because the toolchain is in the zip. A player can copy the zip and the disc to an offline PC and set up there.

### 5.11 Libraries considered

| Library | Verdict | Why |
|---|---|---|
| own code: WinHTTP + CNG + Monocypher | chosen | Small, no new runtime dependency, and FFCCEOT's check is already written. Stage B is about 600 to 900 lines (inferred). |
| Velopack (MIT; C API, zstd deltas, channels) | not now | Its prebuilt updater and its install layout would replace ours, and packaging uses its own tool. Worth another look if host deltas start to matter (inferred). |
| WinSparkle (appcast, EdDSA signatures) | no | It runs a downloaded installer, and we ship zips. |

## 6. Plan and effort

### 6.1 Stage A: first release with setup and an update check

| Step | Work | Where | Days |
|---|---|---|---|
| 1 | Split with today's toolchain (both sides `/MD`): `des_game_init` (function table, SPU workloads, libsre glue, no static constructors); runtime header split; named port calls resolved at init; the two trampoline pointers; DLL-owned hot globals | ps3recomp `tools/ppu_lifter.py` (header template), `tools/spu_lifter.py`, `runtime/ppu/ppu_loader.cpp`, `runtime/spu/`; port `CMakeLists.txt`, `des_options.cpp`, `des_menu_sfx.cpp`, `scripts/build_spu.py`, `scripts/gen_libsre_exports.py` | 3 to 5 |
| 2 | Frame-rate A/B of the split, and fixes | `tools/des/exeab.sh` | 1 to 2 |
| 3 | Freestanding module: flags, small libc headers, a support file (`_fltused`, `memcpy`, `memset`, `memmove`, the math), SPU and glue logs and knobs through the host; build with clang and lld only, on a clean VM | lifter templates; a new `port/game_module/` | 2 to 3 |
| 4 | Host for players: static CRT everywhere (FreeType and RmlUi too); delay-load `mfplat.dll`; UTF-8 code page; a host build with no game inputs (icon at run time, NID table in the host, no TSP blob) | `ps3recomp/CMakeLists.txt`, `port/CMakeLists.txt`, `port/ui/CMakeLists.txt` | 1.5 to 2.5 |
| 5 | Player paths and defaults: the deslib defaults in the exe; data and local folders; a /dev_hdd1 override; no cwd-relative paths | `port/des_options.cpp`, `runtime/ppu/ppu_fs.cpp`, `libs/filesystem/cellFs.c`, `rsx_d3d12_backend.c`, `gen_libsre_exports.py` | 2 to 3 |
| 6 | Pipeline script: one entry point from the inputs to the sources, with progress lines | new `scripts/des_setup.py` over the existing scripts | 1.5 to 2 |
| 7 | RPCS3 step: find it, probe `--help`, `--decrypt` into a temporary copy, table of known hashes, messages | port setup code | 1 to 1.5 |
| 8 | Compile driver: jobs by RAM, progress, cancel, resume, disk check, lld link, expected hash, clean-up | port setup code (from FFCCEOT's `build_module`) | 2 to 3 |
| 9 | Setup screens: setup mode without the game, folder pickers, progress, errors, a rebuild prompt, Settings > Game files | `port/ui/src`, `port/ui/res/rml` (from FFCCEOT's `prelaunch` and `rom_setup`) | 3 to 5 |
| 10 | Release package: pinned LLVM archive trimmed to clang, lld and headers; embeddable Python with `._pth`; lifter scripts; licences; a scan for game data; zip | new `tools/package.py` (from FFCCEOT's) | 1 to 1.5 |
| 11 | Update check and banner; version from tags | `port/ui/src` (from FFCCEOT's `update_check.cpp` and `release_info.cpp`, with its tests) | 1 to 1.5 |
| 12 | CI release workflow: on a tag, build the host, package it, write `SHA256SUMS`, make a draft release | new `.github/workflows/release.yml`; the port has no workflows today | 1.5 to 2.5 |
| 13 | Identical DLL from two folders and two PCs; expected hash per flag set | build flags | 1 to 2 |
| 14 | Clean-PC tests: Windows 10 and 11, an N edition, no Visual C++ runtime, 8 and 16 GB, a non-ASCII user name, OneDrive, Defender on | VMs | 2 to 3 |
| 15 | Player docs: README section, FAQ, legal wording | `README.md`, docs | 1 |
| 16 | Code signing: account or SignPath application, CI step | CI | 0.5 to 1, plus the wait for identity checks |

Total: about 25 to 40 days (5 to 8 weeks).

### 6.2 Stage B: apply updates in the app

| Step | Work | Days |
|---|---|---|
| B1 | Signed manifest (minisign format, Monocypher, two keys); SHA-256 per component with CNG | 1 to 1.5 |
| B2 | Downloads: WinHTTP with resume, progress, size caps | 1 |
| B3 | Stub and side-by-side layout: `current.txt`, atomic switch, rename and replace for the stub, named mutex, no admin | 2 to 3 |
| B4 | Rollback: kept version and build, crash-loop offer, Settings > About button | 1 to 1.5 |
| B5 | Channels, and a publish script that signs the manifest | 0.5 to 1 |
| B6 | Tests: interrupted download, full disk, offline, proxy, antivirus | 1 to 2 |

Total: about 7 to 10 days. Optional build-time work (hot/cold split, smaller chunks, `/O1`): 2 to 4 days after R2 to R4.

### 6.3 Order and milestones

- **Milestone 1, about 4 to 7 days (steps 1 and 2).** `des_host.exe` loads `des_game.dll` built from today's lift, reaches Boletaria, and runs at the same frame rate.
- **Milestone 2, about 2 to 3 more days (step 3).** The same DLL built by the trimmed clang and lld alone, on a PC with no Visual Studio.
- **PR order.** The ps3recomp parts (lifter templates, header split, trampolines, /dev_hdd1 override, static CRT) go in a ps3recomp PR first. The port PR depends on it. Keep tooling (package script, workflow) apart from fixes.

## 7. Risks

- **Legal.** `des_game.dll` is the game. Never upload it, attach it or ask a player to send it. Crash reports keep excluding the minidump and every game file. The download holds no keys and no RPCS3 code.
- **Antivirus and SmartScreen.**
  - An unsigned exe shows the SmartScreen warning on every new download.
  - Behaviour engines may flag a program that starts a compiler many times, writes a 300 MB DLL and loads it. In stage B, one that also replaces its own files looks like a dropper.
  - Mitigations: sign the host and the stub; bundle the toolchain, so setup downloads no executables; no packers; keep self-update for stage B; send false positives to Microsoft for our exe only, never the DLL; explain it in a FAQ.
  - How long Defender takes to scan a fresh 300 MB DLL is unknown.
- **Download size.** About 90 to 100 MB for the first install. 4 GB free during setup.
- **Build time and RAM.** 11 minutes to 2 hours. 8 GB PCs need 1 to 2 jobs or smaller chunks. Laptops slow down when hot. Players may close a long setup, so resume matters.
- **Toolchain drift.** A newer clang can change the code or break the build. Pin one LLVM version per release and publish the expected DLL hash.
- **Lift determinism is unknown.** A lifter whose output depends on worker timing breaks the expected hash and incremental rebuilds (R5).
- **DLL performance.** Probably neutral, but unmeasured (R3).
- **Freestanding gaps.** Only symbol tables were read; no freestanding compile was tried (R1).
- **Inputs.**
  - A game update installed in RPCS3 replaces the EBOOT under `dev_hdd0`, so take the disc's `EBOOT.BIN`.
  - Other regions are out.
  - libsre differs by firmware version; `build_libsre.sh` assumes TOC 0x30039AB0.
  - RPCS3's command-line options may change.
- **The player's PC.**
  - Windows N editions lack Media Foundation.
  - No Visual C++ runtime (fixed by the static CRT).
  - Non-ASCII user names meet narrow `fopen` paths.
  - OneDrive folders.
  - Controlled Folder Access under Documents.
  - Read-only disc folders (fixed by the /dev_hdd1 override).
  - An antivirus that quarantines `clang.exe` halfway through a build.
- **Update security.** A stolen signing key, downgrade attacks and a stolen GitHub account. Mitigations: the offline minisign key, two embedded keys, the downgrade check and immutable releases.
- **Rate limit.** 60 API calls an hour per IP. Players behind shared IPs may see the check fail silently. The stable channel avoids the API.
- **Support load.** Builds fail on PCs we never saw. The setup log must reach reports, redacted.

## 8. What I could not determine

1. Compile time and peak RAM on typical player PCs. Only this PC was measured, and the 3 GB per chunk figure is a code comment.
2. Whether `/O1` or a hot/cold split keeps the frame rate. FFCCEOT found `/O1` equal to `/O2` for its game; `/Od` here took about 70 ms a frame.
3. Whether the lifter's output is byte-identical across runs (`-j 16`) and across PCs.
4. The real cost of the DLL boundary.
5. Whether small freestanding headers cover every chunk. Only symbol tables were read.
6. libsre across firmware versions: which hashes to accept, and whether the TOC changes.
7. The names and folder of RPCS3's `--decrypt` outputs. The decryption source (`rpcs3/Crypto/`) is not in the local reference checkout, and the web gave only an anecdote ("sprx decrypts to prx").
8. How long `ppu_loader`, the SPU lifts and the libsre lift take on their own.
9. Whether SignPath Foundation would accept a game port.
10. Whether LLVM's Windows binaries carry an Authenticode signature.
11. How Defender treats a fresh 300 MB DLL: scan time, and any machine-learning flags.
12. How the embeddable Python builds `sys.path` with a `._pth`: whether the script's folder is added.
13. Whether the lifters' worker processes run unchanged under the embeddable Python.
14. Object sizes without `/Zi`, for disk planning.
15. Whether `settings.ini` keeps unknown keys today.

## 9. Run requests

R1, R2 and R4 to R6 are builds or lifts into a scratch folder. They do not start the game. R3 runs the game. Use Git Bash from the repository root, and `<scratch>` for a scratch folder.

**R1. Freestanding compile of one chunk (no game run).**
- Do: compile the small chunk `work/recomp/ppu_recomp_012.cpp` with clang (not clang-cl) using these flags: `--target=x86_64-pc-windows-msvc -c -O2 -fwrapv -msse4.1 -ffreestanding -fno-stack-protector -fno-math-errno -DDS_NO_SHADOW_STACK`. Add the include folders `des_boot` uses (`port/CMakeLists.txt`). Then run `llvm-nm -u` on the object. Repeat with `-nostdlibinc` added.
- Log: compile time, errors, and the undefined symbols.
- Expect: without `-nostdlibinc`, no `__security_*` and no plain `sqrt` calls. The TLS-guard symbols may remain. With `-nostdlibinc`, errors for `<stdio.h>`, `<string.h>` and `<math.h>`. Those errors list the small headers we must provide.

**R2. Peak RAM and time per chunk.**
- Do: compile chunk 005 alone with the `des_boot` flags, at `/O2` and again at `/O1`. Sample the compiler's peak working set (Task Manager, or a PowerShell loop over `Get-Process clang*`).
- Log: wall time and peak memory for each.
- Expect: about 3 GB and 2 to 4 minutes at `/O2` (CMake comment). `/O1` unknown.

**R3. `/O1` build and frame-rate A/B (runs the game).**
- Do: configure a third build tree with `/O1` for the lifted chunks only, as `build_hyb` did for `/Od`, and build it. Then alternate the `/O2` and `/O1` exes in Boletarian Palace with `tools/des/exeab.sh`, at `DES_FPS=60 VBLANK_MHZ=240000`, with the usual knobs `DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1`.
- Log: total build time (ninja log) and the frame rate of each.
- Expect (inferred): a faster build; the frame rate within a few percent, or a clear loss on the HighGraphics or main thread.
- The same A/B, run later against a DLL build, answers the boundary-cost question.

**R4. Smaller chunks.**
- Do: lift with `--chunk-lines 150000` into `<scratch>/recomp150` (the lift alone is about 40 s). Count the chunks, then compile two of them as in R2.
- Log: chunk count, sizes, and peak memory per job.
- Expect: about four times as many chunks, and much less memory per job.

**R5. Lift determinism.**
- Do: run the docs/27 lift twice into `<scratch>/liftA` and `<scratch>/liftB`, with the same inputs and `-j 16`, then once more with `-j 4`. Compare the SHA-256 of every output file.
- Expect: identical files. If not, find where the order comes from.

**R6. Identical objects.**
- Do: compile chunk 012 twice, in two different folders, with `/Brepro`, `-ffile-prefix-map=<folder>=.` and no `/Zi`. Compare the SHA-256 of the two objects.
- Expect: identical.

## Sources

- GitHub, latest release: https://docs.github.com/en/rest/releases/releases#get-the-latest-release
- GitHub, about releases and their limits: https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
- GitHub, REST rate limits: https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api
- GitHub, asset digests (2025-06-03): https://github.blog/changelog/2025-06-03-releases-now-expose-digests-for-release-assets/
- GitHub, immutable releases (2025-10-28): https://github.blog/changelog/2025-10-28-immutable-releases-are-now-generally-available/
- GitHub, verifying release integrity: https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity
- minisign: https://jedisct1.github.io/minisign/
- Monocypher: https://monocypher.org/
- Windows CNG named curves: https://learn.microsoft.com/en-us/windows/win32/seccng/cng-named-elliptic-curves
- Ed25519 in Windows (Microsoft Q&A): https://learn.microsoft.com/en-gb/answers/questions/1696656/when-will-microsoft-support-ed25519-x25519-and-sha
- WinHttpOpen: https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpopen
- MoveFileEx: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw
- Code signing options for Windows apps: https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options
- Azure Artifact Signing pricing: https://azure.microsoft.com/pricing/details/artifact-signing/
- SignPath Foundation: https://signpath.org/ and https://signpath.org/terms
- Python, `sys.path` and `._pth` files: https://docs.python.org/3.12/library/sys_path_init.html
- Python 3.13.9 files: https://www.python.org/ftp/python/3.13.9/
- LLVM 22.1.8: https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8
- llvm-mingw: https://github.com/mstorsjo/llvm-mingw/releases
- Zig: https://ziglang.org/download/
- Velopack deltas and C API: https://docs.velopack.io/packaging/deltas and https://docs.velopack.io/reference/cpp/c-api
- WinSparkle: https://winsparkle.org/
- Zelda64Recomp: https://github.com/Zelda64Recomp/Zelda64Recomp (BUILDING.md, releases); N64Recomp: https://github.com/N64Recomp/N64Recomp
- UnleashedRecomp: https://github.com/hedge-dev/UnleashedRecomp (docs/BUILDING.md, releases); XenonRecomp: https://github.com/hedge-dev/XenonRecomp
- AC4-Port: https://github.com/SulphurS16/AC4-Port (`data/toolchain.txt`, `src/common/known_inputs.cpp`); see also docs/31
- RPCS3 source, `rpcs3/rpcs3.cpp` (`--decrypt`, `--installfw`), in the local reference checkout
- FFCCEOT-Recomp, local checkout: `docs/PACKAGING.md`, `docs/ARCHITECTURE.md`, `game/src/setup.cpp`, `game/src/update_check.cpp`, `game/src/release_info.hpp`
