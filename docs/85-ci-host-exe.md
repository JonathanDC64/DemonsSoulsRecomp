# 85. The host exe built in CI

Date: 2026-10-22. Status: built and checked on this PC. The workflow's steps ran by hand (a local script),
with ps3recomp at the split commit (bae620c, ps3recomp #101) plus the static-runtime change (ps3recomp
#102). The workflow itself has not run on GitHub: it needs a pin with both (open question 1). Not done yet:
a boot of the hybrid exe, and its frame-rate A/B (section 5).

Files: `.github/workflows/host.yml` (the workflow), `tools/ci/check_host_imports.py` (the import check),
`.github/host-files-allow.txt` (the artifact's game-file exceptions), and two options in
`port/CMakeLists.txt`. ps3recomp #102 has its side.

Labels, as in the other notes:
- **Measured**: from a build, a run, or a tool on built files.
- **Read**: seen in our source, or in a fetched third-party source.
- **Inferred**: a conclusion that still needs a check.

Decided (docs/81, 2026-10-22): CI builds the host exe, `des_host.exe`. The player's PC compiles only the
game DLL, with a downloaded clang. The host exe links the Visual C++ runtime and the C++ library into
itself, and uses the UCRT that Windows 10 and 11 include. No game-derived file may be in the repository, a
CI log or a release: not the lift, the EBOOT, firmware, or an icon made from the game's `ICON0.PNG`.

## Short answer

- **CI can build `des_host.exe` from no game or firmware data.** Everything it needs is our source,
  ps3recomp's source, a table made from ps3recomp alone, third-party code fetched by commit or tag, and the
  MSVC and Windows SDK libraries (section 1).
- **Three things stop a CI build today, all on our side** (1.3):
  1. The port's CMake cannot configure `des_host` without a lift. `des_boot` lists the SPU workload table,
     a lift file (`port/CMakeLists.txt:98`), the split block reads it (line 399), and `des_host` depends on
     `des_game` (line 514). A new option, `DES_HOST_ONLY`, configures `des_host` alone.
  2. The host's sources include the lift's `ppu_recomp.h`. They use only its fixed part (1.2). Until the
     header split, CI asks the pinned lifter for the header it writes for a lift with no functions: the same
     fixed text, nothing of the game (section 4).
  3. For the hybrid CRT, `boot_main.cpp`'s getenv cache defines the import slot `__imp_getenv`
     (`boot_main.cpp:201`). Under `/MT` nothing reads that slot, and the link stops on a duplicate symbol
     (measured). The fix defines `getenv` itself under `/MT` (2.3, ps3recomp #102).
- **The runtime libraries.** `/MT` at compile time for every target in the build directory (the UI,
  FreeType, RmlUi and the ps3recomp runtime library too), then `/NODEFAULTLIB:libucrt.lib
  /DEFAULTLIB:ucrt.lib` at the link. New options, off by default: `DES_HOST_STATIC_CRT` (port),
  `PS3RECOMP_STATIC_CRT` (ps3recomp). Measured on `des_host.exe`: +0.11 MB over `/MD` (2.5).
- **Measured:** the hybrid exe imports the UCRT's API sets and no Visual C++ runtime DLL, the import check
  fails today's `/MD` exe on its three DLLs, and with both options off the build is the same as before
  (section 5).
- **The exe and the game DLL keep one UCRT.** Both use Windows' `ucrtbase.dll`, so they share the heap, the
  environment and stdio, as today. A plain `/MT` exe would not, and the console's setenv would stop reaching
  the DLL (2.4).
- **Left out in CI:** the taskset-policy blob (firmware-derived; the weak stub links), the icon (none), the
  game dump's hash in the version ("unknown").

## 1. Inputs

### 1.1 The inventory

Read: `port/CMakeLists.txt` of the dll-split branch (2120bad), with `des_host`'s sources taken from
`des_boot`'s (lines 445-450, 487-503), and ps3recomp at bae620c. Line numbers are `port/CMakeLists.txt`'s
unless a file is named.

| Input | Where | Kind | In CI |
|---|---|---|---|
| Port host sources: `missing_stubs.cpp`, `des_heap.cpp`, `des_options.cpp`, `des_aspect.cpp`, `des_menu_sfx.cpp`, `des_chrins.cpp`, `des_prework.cpp` | 105-111 | plain source | the checkout |
| The split's port glue: `des_game_host.cpp`, `des_game_title.c`, with `des_game_calls.h` and `des_game_title.h` | 501-502 | plain source | the checkout |
| ps3recomp's boot scaffold: `runtime/ppu/ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `tests/boot_main.cpp`; and `game_host.c` | 100-104, 500 | plain source (ps3recomp) | the pinned checkout |
| The split runtime library, `ps3recomp_runtime.lib` built with `PS3RECOMP_GAME_SPLIT=ON` | 376-379; ps3recomp `CMakeLists.txt:408-419` | built from ps3recomp source | CI builds it, `/MT` |
| `include/ps3emu/game_split.h`, force-included | 507-508 | plain source (ps3recomp) | the pinned checkout |
| Include folders: ps3recomp's `include/`, `runtime/ppu`, `libs/video`; the port's `compat/` (a `dirent.h` for `ppu_fs.cpp`) | 114-123 | plain source | the checkouts |
| `split_gen/des_game_title_funcs.h`: the port functions the lift calls | 409-442, from `scripts/lift_ppu.json` and `port/insn_hooks.txt` | generated from committed files (names, addresses) | configure writes it |
| `hle_nid_table.cpp`, in `RECOMP_DIR` | 99 | generated from ps3recomp alone: `tools/gen_hle_nids.py --all` takes the function names in `libs/**/*.c` (`discover_all`, line 55; the writer, line 144) | a CI step makes it |
| `ppu_recomp.h`, in `RECOMP_DIR` (an include folder, line 119) | 119 | **game-derived** as a whole: one declaration per lifted function, plus a fixed part | stopgap until the header split: the pinned lifter's header of an empty lift (section 4) |
| The SPU workload table, ps3recomp's `work_spu/_gen/spu_workloads.cpp` | 98; read again at 396-407 | **game-derived**: made from the game's SPU images (`check_game_files.py` lists it) | not compiled into `des_host` (line 488 removes it), but configure needs it; `DES_HOST_ONLY` drops it |
| `des_game.dll`, which `des_host` depends on | 467-473, 514 | **game-derived** | `DES_HOST_ONLY`: not defined, no dependency |
| The lifted PPU chunks and SPU images | 29, 39 | **game-derived** | none; the globs come back empty |
| The real taskset policy: its raw bytes (`port/gen/taskset_policy_blob.c`) and its lift (`fw_spu/.../taskset_policy_lifted`) | 56-68 | **firmware-derived** (from `libsre.prx`); in the exe (docs/57 2.2) | left out. The generator does nothing without `libsre.prx` (58-61). The weak stubs link (`runtime/spu/spurs_policy_blob_weak.c:15`, `spu_tsp_weak.c:15`), so `LBP_REAL_POLICY=1` does nothing in a CI host |
| The libsre LLE: the lifted libsre, its glue, and `runtime/prx/prx_loader.c` | 78-90 | **firmware-derived** (the loader is plain source) | left out; the lifted libsre is the DLL's in a split build anyway. The exe hands the DLL `prx_load_module` and `prx_image_load_file` (`game_module.h:40`, `game_host.c:112-113`); without the LLE sources they come from the runtime library (`prx_loader.c:85`, `:146`; the library's glob, ps3recomp `CMakeLists.txt:90-106`) |
| The exe's icon, from `ICON0.PNG` | 319-334 | **game-derived** | none: the CI exe has no icon, and the window none either (resource 1) |
| The version: `des_version.h` and `des_version.rc` | `port/cmake/DesVersion.cmake:211-237` | git, `VERSION` and the pin; plus the SHA-256 of the decrypted EBOOT (lines 219-225) | git with full history; the dump hash is empty, shown as "unknown" (lines 157-161) |
| The wrapper UI, `des_ui`, with FreeType `VER-2-14-3`, RmlUi `6.3`, stb at a commit, and the fonts in `port/ui/res` | 346-348; `port/ui/CMakeLists.txt:18-54`, `69-74` | plain source and third-party | fetched at configure |
| Windows SDK libraries: `ws2_32 xinput ole32 bcrypt d3d12 dxgi d3dcompiler uuid dxguid` (270-273), the UI's `d3d11 shell32 user32 dbghelp imm32` (`port/ui/CMakeLists.txt:46`, `117-118`), and those named by `#pragma comment(lib)` in the sources (`winmm wtsapi32 setupapi hid avrt mfplat mfuuid`) | as listed | system | `ilammy/msvc-dev-cmd` |
| The static Visual C++ runtime: `libcmt.lib`, `libcpmt.lib`, `libvcruntime.lib` (MSVC), and `ucrt.lib` (Windows SDK) | new (2.6) | system | `ilammy/msvc-dev-cmd` |
| `des_dev.cpp` (dev cheats) | 146-147 | plain source, developer builds only | never: `DES_DEV=OFF`, checked by `tools/ci/check_release_no_dev.py` |
| Python 3, at configure (52-61) and in the CI steps | | tool | `actions/setup-python` |

### 1.2 What the host uses of the lift's header

The generated `ppu_recomp.h` is the lifter's fixed text, then one line per function (ps3recomp
`tools/ppu_lifter.py`, `emit_header`, line 3436):
- `HEADER_PREAMBLE` (line 72): the types, prototypes and macros below;
- a fixed shadow-stack block that defines `DS_FRAME`;
- per game: `void func_XXXXXXXX(ppu_context*)` for every lifted function and every outside call target, and
  an `extern "C"` declaration for each function override.

Twelve host sources include it (read): `ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`,
`boot_main.cpp`, `missing_stubs.cpp`, `des_heap.cpp`, `des_options.cpp`, `des_aspect.cpp`,
`des_menu_sfx.cpp`, `des_chrins.cpp` and `des_game_host.cpp` (and `des_dev.cpp` in a developer build). The UI, `des_prework.cpp`, `game_host.c`,
`des_game_title.c` and the NID table do not.

They use (read, by name in the sources):

| Declaration | From | Used in |
|---|---|---|
| `ppu_context` (with `ppu_vr` in it) | preamble | all twelve |
| `ppu_cr_get`, `ppu_cr_set` | preamble | `des_options.cpp:875` |
| `ppu_cr_pack`, `ppu_cr_unpack` | preamble | `des_menu_sfx.cpp:150`, `:178` |
| `vm_read8` ... `vm_read64`, `vm_write8` ... `vm_write64`, as `extern "C"` prototypes | preamble | `ppu_loader.cpp` defines them with no `extern "C"` of its own (for example lines 2024 and 2235), so their C names come from the header; called in `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `des_options.cpp`, `des_aspect.cpp`, `des_menu_sfx.cpp`, `des_chrins.cpp` |
| `ppu_stwcx32`, `ppu_stdcx64` | preamble | defined in `ppu_loader.cpp:1003`, `:1095` |
| `lv2_syscall` | preamble | defined in `ppu_loader.cpp:3503`; called in `ppu_hle.cpp`, `boot_main.cpp` |
| `PPU_THREAD_LOCAL` | preamble (also `runtime/ppu/ppu_tls.h`) | `ppu_loader.cpp`, `boot_main.cpp`, `des_options.cpp:1512`, `des_menu_sfx.cpp`, `des_chrins.cpp` |
| `func_entry`, `function_table`, `function_table_count` | preamble | `ppu_loader.cpp:56-58`, `:4429`; `boot_main.cpp:887-888`. In a split build `game_split.h` turns the two names into the host's pointers (`game_split.h:65-66`) |
| `DS_FRAME` | the shadow-stack block, not the preamble | `des_chrins.cpp:164` |

Not used:
- **Any per-game declaration.** In a split build the port's calls into the lift go through forwarders that
  `des_game_calls.h` declares (lines 12-46): 26 in the release sources, 29 with `des_dev.cpp`
  (`des_game_title.h` lists 29; docs/57 counted 27). The port defines its hooks and overrides with their own
  `extern "C"` (for example `des_heap.cpp:91-153`, `des_options.cpp:846`). `missing_stubs.cpp:21` defines
  `func_FFFFFFE8` itself. The runtime sources name `func_` addresses only in comments and log strings.
- **The inline memory accessors** (the preamble's `PPU_VM_INLINE` block). Only lifted sources define
  `PPU_VM_INLINE`. The host declares `vm_base` itself where it needs it (`des_heap.cpp:29`,
  `game_host.c:49`).
- **`PPU_FENCE`.** No host source uses it.

So the host needs exactly the preamble and the `DS_FRAME` block (section 4).

### 1.3 What stops a CI build today

| # | Problem | Where | Fix |
|---|---|---|---|
| 1 | configure fails: the SPU workload table is a listed source and is read | `port/CMakeLists.txt:98`, `:399` | `DES_HOST_ONLY` (2.6) |
| 2 | building `des_host` builds `des_game`, from the lift | `port/CMakeLists.txt:514` | `DES_HOST_ONLY` |
| 3 | no `ppu_recomp.h` without a lift | include folder, line 119 | the empty-lift header (stopgap); then the header split |
| 4 | no `hle_nid_table.cpp` without a lift | line 99 | `gen_hle_nids.py --all` in a step (or from CMake, optional) |
| 5 | the hybrid CRT: the `__imp_getenv` definition | ps3recomp `boot_main.cpp:201` | the `/MT` form (2.3), ps3recomp #102 |
| 6 | the hybrid CRT: the runtime library is `/MD` | ps3recomp `CMakeLists.txt` | `PS3RECOMP_STATIC_CRT`, ps3recomp #102 |
| 7 | CI checks out ps3recomp by the pinned commit | `ps3recomp.pin` | a commit on the fork's master with #101 and #102 (open question 1) |

## 2. The runtime libraries

### 2.1 The hybrid CRT

| Build | Visual C++ runtime, C++ library | C library (UCRT) | A player needs |
|---|---|---|---|
| `/MD` (today) | `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `MSVCP140.dll` | Windows' `ucrtbase.dll` | the Visual C++ redistributable |
| `/MT` | linked in | linked in (`libucrt.lib`) | nothing |
| hybrid: `/MT`, then `/NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib` | linked in | Windows' `ucrtbase.dll`, through the `api-ms-win-crt-*.dll` API sets | nothing (Windows 10 and 11 include the UCRT) |

Microsoft's Windows App SDK documents the same two link options for its "Hybrid CRT" (read,
`docs/Coding-Guidelines/HybridCRT.md`, 2026-10-22), and says "msvcrt(d).lib must never be used". The draft adds
`/NODEFAULTLIB` for `msvcrt.lib`, `msvcprt.lib` and `vcruntime.lib`, so a stray `/MD` object stops the link
instead of importing a DLL (inferred).

### 2.2 One choice for the whole build directory

- **C++ objects.** The STL marks each object with its runtime (`RuntimeLibrary`); the linker refuses a
  `/MT` object beside a `/MD` one. So `des_ui`, RmlUi and the C++ files of the ps3recomp runtime library must
  be `/MT` as well.
- **C objects.** Each names its default C library (`libcmt.lib` or `msvcrt.lib`). A mixed link can pull
  both, or import `VCRUNTIME140.dll` without a word (inferred). FreeType is C.
- **CMake.** Both projects set CMP0091 to NEW through `cmake_minimum_required(VERSION 3.20)` (the port's
  line 1, ps3recomp's line 1). The policy is taken at the top `project()`, and the whole tree follows it.
  So `CMAKE_MSVC_RUNTIME_LIBRARY`, set before the first target, reaches every target, FetchContent's
  included. FreeType asks for `3.12...3.31.0` and RmlUi for `3.10...3.27`, and neither sets
  `MSVC_RUNTIME_LIBRARY` or edits `/MD` flags (read, in fetched copies). stb is headers only.
- **So** `DES_HOST_STATIC_CRT` holds for the whole build directory, `des_boot` and `des_game` included, and
  the ps3recomp libraries it links must be built `/MT`. A build directory of its own: `port/build_host`.

### 2.3 The getenv cache

Read: `boot_main.cpp` routes every getenv call through a cache (lines 97-200): the runtime has about 700
`if (getenv(...))` gates, many on hot paths. It installs the cache by defining the import slot (line 201):

```cpp
extern "C" char* (__cdecl* __imp_getenv)(const char*) = cached_getenv;
```

- **`/MD`.** The UCRT headers declare `getenv` with `__declspec(dllimport)`, because `_DLL` is defined. Every
  call reads `__imp_getenv`, and our definition wins over the import library's.
- **`/MT`.** `_DLL` is not defined, so there is no `dllimport`, and every call is a plain call to `getenv`. The
  linker takes it from `ucrt.lib`'s import member, which also defines `__imp_getenv`. Measured with a probe:
  lld-link stops on `duplicate symbol: __declspec(dllimport) getenv`. If it linked anyway, the cache would be
  bypassed.
- **The fix** (ps3recomp #102): under `#ifdef _DLL` keep the slot; otherwise define
  `extern "C" char* __cdecl getenv(const char* name) { return cached_getenv(name); }`. The object's `getenv`
  is defined before any library is searched, so `ucrt.lib`'s is never linked: the hybrid host links
  (measured). The `/MD` object is byte for byte the same as before, apart from the time stamp (measured,
  with `des_boot`'s own command). `cached_getenv`
  reads `GetEnvironmentVariableA`, not `getenv` (lines 120-127), so there is no loop. A `/MD` object that
  asks for `__imp_getenv` would stop the link on a duplicate `getenv`: one more tripwire.

No other source defines an import slot or tests `_DLL` or `_MT` (read).

### 2.4 The exe and the game DLL share one UCRT

- The hybrid exe and the DLL (`/MD` today, freestanding later: docs/52 7.8) use the same `ucrtbase.dll`. One
  heap, one environment, one set of stdio streams, as with two `/MD` modules today (inferred, from the
  UCRT's design).
- docs/57 3.6 relies on the shared environment: the console's setenv (`_putenv_s`, `boot_main.cpp:184`) must
  reach the getenv calls of the DLL's SPU images and glue.
- A plain `/MT` exe has its own UCRT: its own environment copy and its own `stderr`. A console setenv would
  then not reach the DLL (inferred). So the import check has `--require-ucrt`, and CI uses it.
- Each module has its own copy of the Visual C++ runtime: the exe's linked in, the DLL's
  `VCRUNTIME140.dll`. Only C crosses the boundary (`game_module.h`), and no C++ exception does, so that is
  fine (inferred). One path to test: a guest thread's exit, which unwinds with `longjmp` from the host
  through the DLL's frames (docs/52 7.8 point 6).

### 2.5 Sizes

- Today's `des_host.exe`: 7,743,488 bytes, `/MD` (measured, docs/57 4.1). It imports `MSVCP140.dll`,
  `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`.
- Measured on 2026-10-22 by linking the 254 runtime functions today's exe imports into a test program
  (docs/81): **+0.14 MB** hybrid, +0.47 MB plain `/MT`.
- Measured on `des_host.exe` itself (10-22): two host-only builds of the same tree, the same clang-cl and
  the same options apart from the runtime. Hybrid 8,107,008 bytes, `/MD` 7,993,856: **+113,152 bytes
  (+0.11 MB)**.

### 2.6 The CMake changes

The text is in `port/CMakeLists.txt` and ps3recomp #102. In short:

- **Port, `DES_HOST_STATIC_CRT`** (off): after `project()` and the language standards,
  `set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)`, which comes before `des_boot` (line 93) and `add_subdirectory(ui)` (line 347). The link
  options on `des_boot`, `des_game` and `des_host`. They have to be named on each: `des_host` copies
  `des_boot`'s sources, include folders, definitions, options and libraries, not its link options (line 445).
- **Port, `DES_HOST_ONLY`** (off; needs `DES_SPLIT=ON`): no SPU workload table, `des_boot` left out of the
  build (`EXCLUDE_FROM_ALL`: `des_host` still takes its source list from it), no `des_game`, and `des_host`
  depends on `des_version` only.
- **ps3recomp, `PS3RECOMP_STATIC_CRT`** (off): `set(CMAKE_MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>")` before `add_library` (line 111). The library links nothing, so it
  needs no link options.
- **ps3recomp code:** the `/MT` form of the getenv cache (2.3).
- **Port, `.github/host-files-allow.txt`:** the artifact's exceptions to the game-file check (3.3).

With all of them off the build is the same (measured: `ninja -t commands` for every target is the same
before and after the port's change, 336 commands with `DES_SPLIT` off and 386 with it on).

## 3. The workflow

### 3.1 Steps (`.github/workflows/host.yml`)

On pull requests and master pushes; `windows-latest`; clang-cl and Ninja set up as in `ci.yml`, with the same
action SHAs (`actions/checkout` v4.4.0, `ilammy/msvc-dev-cmd` v1.13.0, `actions/setup-python` v5.6.0), and
`actions/upload-artifact` v4.6.2 by SHA (`ea165f8d65b6e75b540449e92b4886f43607fa02`, read from GitHub's tag on
2026-10-22).

1. Check out the port, with full history (the version string counts commits).
2. Read `ps3recomp.pin`; check out that commit of the fork into `ps3recomp/`.
3. Show the toolchain. A clang-cl other than 22.1.8 is a warning for now (open question 2).
4. Configure and build the runtime library: `PS3RECOMP_GAME_SPLIT=ON`, `PS3RECOMP_STATIC_CRT=ON`, Release.
5. Make `hle_nid_table.cpp` with `gen_hle_nids.py --all` in the runner's temp folder.
6. **HEADER SPLIT STOPGAP:** write `ppu_recomp.h` there from `ppu_lifter.PPULifter().emit_header()`. It
   checks that the declarations of 1.2 are in it and no per-game ones.
7. Configure the port: `DES_SPLIT`, `DES_HOST_ONLY`, `DES_HOST_STATIC_CRT`, `DES_REQUIRE_PIN` on;
   `DES_DEV` off; `RECOMP_DIR` the temp folder; no build type, as in `ci.yml`.
8. Build `--target des_host`.
9. The import check's self-test, then the check with `--require-ucrt`.
10. `check_release_no_dev.py` on the exe.
11. Stage `des_host.exe` and `des_host.map`; print sizes and SHA-256; run the game-file check on the folder.
12. Upload them as `des_host-<commit>`, kept 14 days.

Nothing of the game is ever on the runner. The two lift-folder files are ours, and stay in the temp folder.
The logs show paths, counts and the NID table's size (inferred from the scripts' output lines).

### 3.2 The import check (`tools/ci/check_host_imports.py`)

- Reads the PE file's import table and delay-load table directly (`struct`; no `dumpbin` or `llvm-readobj`).
  PE32 and PE32+.
- Prints every DLL. **Fails** on `MSVCP*`, `VCRUNTIME*`, `CONCRT*`, `VCCORLIB*`, `VCOMP*`, `MSVCR<n>*` and
  `ucrtbased.dll`, imported or delay-loaded. Marks `api-ms-win-crt-*.dll` and `ucrtbase.dll` as the UCRT, and
  notes `msvcrt.dll` (Windows' own old C runtime).
- `--require-ucrt` also fails when no UCRT DLL is imported: a plain `/MT` build (2.4).
- `--functions` lists the functions of each DLL.
- `--self-test` builds 9 small PE images in memory (the hybrid exe, today's `/MD` exe, a runtime DLL only
  delay-loaded, plain `/MT` with and without `--require-ucrt`, a 32-bit image with the debug C++ library,
  Windows' own libraries, imports by ordinal, no imports), plus 3 files that are not PE images. All 12 pass.
- Exit status: 0 clean, 1 found, 2 usage or not a PE image. Standard library only.

### 3.3 The game-file check on the artifact

`check_game_files.py` flags every `.exe` and `.map`: its rule is that a built exe holds the compiled game
(read). For this folder that is not so. A list of its own, `.github/host-files-allow.txt`, names
`des_host.exe` and `des_host.map`, and the step passes it with `--allow` beside the default list. It stays out
of `.github/game-files-allow.txt`, which also covers the repository tree, where no exe may be committed. Any
other file in the folder still fails the check.

## 4. What waits on the header split

- **Today.** The host compiles against a `ppu_recomp.h`. CI makes one from the pinned lifter
  (`PPULifter().emit_header()` with no functions): the preamble and the shadow-stack block, which are
  exactly what the host uses (1.2). ps3recomp already does the same for its scaffold check
  (`tools/check_ppu_scaffold.py:80-98`, the preamble plus one prototype) and its smoke title
  (`tools/make_smoke_elf.py`, `build_header`).
- **Is it the same?** The real header only adds declarations the host never names. So the host's objects
  should be the same either way (inferred). Check: compile `des_host`'s sources against both headers and
  compare the objects, as docs/57 5 did, apart from the COFF time stamp.
- **The header split** (docs/57 6.2) then moves the fixed part into ps3recomp, for example
  `include/ps3emu/ppu_lift.h`:
  - it holds `HEADER_PREAMBLE` and the shadow-stack block (`DS_FRAME`, `DS_TLS`, `DsFrame`), with the `vm_*`
    prototypes still `extern "C"` (`ppu_loader.cpp` defines them without it);
  - the lifter's `ppu_recomp.h` includes it and adds only the per-game lines;
  - the host's twelve sources include the fixed header instead of `ppu_recomp.h`;
  - `runtime/ppu/ppu_context.h` keeps a second copy of `ppu_context` today, and `boot_main.cpp:34-41` claims
    its include guard to avoid a redefinition. The split can leave one definition.
- **Then** step 6 of the workflow goes, `DES_HOST_ONLY` no longer needs a `ppu_recomp.h`, and with the NID
  table made by CMake (optional, in the draft) `RECOMP_DIR` is not used at all.

## 5. Testing it locally

Not while an A/B or another build runs. Fresh build folders, so the usual `port/build` and
`ps3recomp/build_split` stay as they are. Run in Git Bash from the repository root.

```
# The runtime library, /MT
cmake -S ps3recomp -B ps3recomp/build_split_mt -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_GAME_SPLIT=ON -DPS3RECOMP_STATIC_CRT=ON
cmake --build ps3recomp/build_split_mt --target ps3recomp_runtime

# The lift folder of CI: the NID table and the empty-lift header (host.yml steps 5 and 6)
mkdir -p "$SCRATCH/host_lift"
python ps3recomp/tools/gen_hle_nids.py --all --out "$SCRATCH/host_lift/hle_nid_table.cpp"
python -c "import sys; sys.path.insert(0, 'ps3recomp/tools'); import ppu_lifter; open(sys.argv[1], 'w', newline='\n').write(ppu_lifter.PPULifter().emit_header())" "$SCRATCH/host_lift/ppu_recomp.h"

# The host alone, hybrid CRT
cmake -S port -B port/build_host -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE= -DRECOMP_DIR="$SCRATCH/host_lift" -DPS3RECOMP_SPLIT_LIB=ps3recomp/build_split_mt/ps3recomp_runtime.lib -DDES_SPLIT=ON -DDES_HOST_ONLY=ON -DDES_HOST_STATIC_CRT=ON
cmake --build port/build_host --target des_host
python tools/ci/check_host_imports.py --self-test --require-ucrt port/build_host/des_host.exe
```

What to check, and what it gave on 10-22:
1. **The link.** Before the getenv fix it stops on the duplicate `getenv` (measured with a probe). After it,
   it links (measured).
2. **The imports.** No `MSVCP140*` or `VCRUNTIME140*`; the `api-ms-win-crt-*` DLLs present. Measured: the
   hybrid exe imports 11 UCRT DLLs and no Visual C++ runtime DLL. On a `/MD` host of the same tree the check
   fails, naming the three DLLs. `check_release_no_dev.py` and the game-file check on the staged folder pass.
3. **The size.** +0.11 MB against a `/MD` host of the same tree (2.5); docs/81's test program gave +0.14 MB.
4. **The same objects.** Not done yet. Build `des_host` once more with `RECOMP_DIR=work/recomp` (the real lift) and
   compare its objects with the stub build's (1.2, section 4).
5. **A boot.** Not done yet. Put a `des_game.dll` from the usual split build (the same port and ps3recomp commits) beside
   the new exe, or name it with `PS3_GAME_MODULE`. Then boot through `play.sh` with
   `DES_EXE=port/build_host/des_host.exe` and the pinned save. Look for `[game-host] loaded`, the title, the
   level, and no crash lines. Try the console's setenv on a knob the DLL reads (2.4), and quit to the title
   and continue (guest threads exit: 2.4).
6. **The frame rate.** Not done yet. A plain-window A/B (docs/83) of the hybrid host against today's `/MD` host, both with
   the same DLL. Inferred: no change, or a little faster (direct calls to `memcpy` and the UCRT instead of
   calls through the import table).

## 6. Open questions

1. **The pinned commit on the fork.** CI checks ps3recomp out by the pin, and `DES_REQUIRE_PIN` wants exactly
   that commit. It needs both the split (ps3recomp #101) and the static runtime (ps3recomp #102). Once both are
   merged, this branch moves the pin to the fork's master, and the workflow can run.
2. **The compiler.** The runner's clang-cl is not pinned. docs/54 5.4 wants CI on 22.1.8, failing on any
   other. CI could use the same trimmed LLVM 22.1.8 archive it will make for players (docs/81): the host and
   the DLL then come from one compiler.
3. **The runner image.** `windows-latest` moves to new Visual Studio and SDK versions on its own, and the
   static runtime is linked into the exe, so the exe changes with them. Pin an image (`windows-2025`)?
4. **Required check.** Not until it passes. It builds FreeType, RmlUi, the runtime library and the host on
   every pull request (inferred: 10-20 minutes). A path filter would keep it from being required
   (docs/54 7.3).
5. **The dump hash.** A CI exe has none built in ("unknown"). Setup knows the dump; the host could read the
   hash from setup's data folder at run time, for the UI and the reports.
6. **The version resource** names `des_boot.exe` (`DesVersion.cmake:191-192`), and `des_host` takes it
   from `des_boot`'s sources. Name the host?
7. **The icon.** The CI exe has none, and the window none. docs/52 9 plans our own exe icon and the window
   icon from `ICON0.PNG` at run time.
8. **Matching the DLL.** `PS3_GAME_ABI` and the structure sizes cover ps3recomp's ABI. They do not cover the
   port's two lists (`DES_GAME_TITLE_FUNCS`, `DES_GAME_CALLS`) or `ppu_context`'s layout. A missing name stops
   the load with its name, but a key stamped into both (port and ps3recomp commits) would stop a mixed pair
   before anything runs.
9. **The PDB.** docs/52 9 keeps the host's PDB as a release artifact. CI could build with
   `DES_DEBUG_INFO=ON` (`/Zi` costs little for the host alone) and upload the PDB beside the map.
10. **FreeType and RmlUi by tag**, not by commit (docs/54 5.4 point 5). And the reproducible-build flags of
    docs/54 5.4 (`/Brepro`, prefix maps) are not in yet, so two CI runs make different exes.
11. **The NID table** from a workflow step (the draft) or from CMake in host-only mode (optional text in the
    CMake draft). CMake's way also serves local host-only builds.
12. **`DES_HOST_STATIC_CRT` for developers.** Off by default, so `des_boot.exe` stays `/MD`. Make the hybrid
    CRT the default everywhere later, after the A/B (5, point 6)?
