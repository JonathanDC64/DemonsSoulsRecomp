# 84. The freestanding game DLL (milestone 2)

Date: 2026-10-22. Status: built, booted and measured on this PC (section 8). The ps3recomp and port branches are `feat/game-freestanding`, on top of milestone 1 (docs/57), so their PRs wait for milestone 1's.

Milestone 1 (docs/57) split the game into `des_host.exe` and `des_game.dll`. Both are built with clang-cl against the Visual C++ headers, the Windows SDK and the C runtime (`/MD`). Milestone 2 (docs/57 6.2, docs/52 7.8, docs/45 step 3) makes the DLL **freestanding**: it compiles with clang, lld-link, clang's own headers and headers that ps3recomp ships, references no C runtime symbol, and links with `lld-link /dll /noentry /nodefaultlib`. The exe does not change. That is what setup needs to build the DLL on a player's PC with only the `llvm-win64` download (docs/81 sections 6 and 7).

Labels, as in the other notes:
- **Measured**: from a build, a run, or `llvm-nm` on built objects.
- **Read**: seen in our source, or in the compiler's or the C runtime's headers.
- **Inferred**: a conclusion that still needs a check.

## Short answer

- **C runtime symbols.** Today's DLL objects reference 20 C and compiler runtime symbols (measured, section 2). 14 C library functions become jump thunks into the exe's own C runtime, through a new list in the ABI (`PS3_GAME_HOST_CRT_FUNCS`, ABI 2). The same mechanism already carries the runtime functions (`PS3_GAME_HOST_FUNCS`). So `fma`, `sqrt`, `ldexp` and `exp2` run the same code as in the single exe and give the same bits. `getenv` becomes the exe's cached `getenv`. That closes docs/57 3.6's `GETENV_STATS` gap. `signbit` becomes the compiler's builtin (the same answer, with no call). `/GS-` removes the two stack-cookie symbols. A new support file, `game_libc.c`, defines `_fltused`, `_tls_index`, the TLS directory, `memcpy` and its friends, and runs any static constructors.
- **Thread-locals.** The guards (`__tls_guard`, `__dyn_tls_on_demand_init`) appear because clang-cl treats `__declspec(thread)` as a thread-local that may need dynamic initialisation, in C as well as C++ (read; measured in the SPU C objects). Recommended: **(a)** spell the thread-local keyword `__thread` in the DLL. The forced include does it, and both lifted headers already allow it: no lifter change. The DLL also gets its own TLS directory. The hot path loses the guard's load, compare and branch, and keeps the rest. **(b)**, moving the trampolines into `ppu_context` and `spu_context`, is cheaper still per access. But it touches some 70 sites in the lifters, the runtime and the tests, and needs a full re-lift (section 3.3). It stays a separate, later perf change.
- **Headers.** A shim folder, `ps3recomp/include/ps3emu/game_libc/`, holds six declaration-only headers (`stdio.h`, `stdlib.h`, `string.h`, `math.h`, `malloc.h`, `setjmp.h`). A forced include, `ps3emu/game_freestanding.h`, supplies `PPU_FENCE` without `<atomic>`, the thread-local keyword and the limits. With `PS3_GAME_FREESTANDING` set, `ps3types.h` leaves out its C++ part (`be_t`). No C++ library is used. The compile stays hosted (not `-ffreestanding`) and uses `/X`. The fixed part of `ppu_recomp.h` is now a ps3recomp header of its own, `ps3emu/ppu_lift.h`, which the host's sources include instead of the lift's header (section 4.5). No re-lift.
- **One import.** Windows sets up a DLL's thread-locals only when the image has an import table (measured, 3.2). So the DLL imports kernel32's `GetCurrentThreadId`, which it never calls, through an import library that `lld-link /lib` makes from a two-line `.def` file: still no Windows SDK.
- **Measured** (section 8): it builds, imports only that function, is 29.5 MB smaller (259.5 MB), boots to the Palace, and runs at the same frame rate as milestone 1 (deep Palace: +0.8%, not significant).
- **Opt-in.** The port's `DES_SPLIT_FREESTANDING=ON` (with `DES_SPLIT=ON`) changes `des_game` in place, in its own build directory. The default build and milestone 1's split build compile the same lifted objects as before (section 6). Setup's `game_build_desc.py` and `compile.py` need no change: the description is written again from the freestanding build (docs/81 7).

## 1. What changes, and what does not

- **The DLL** gets six shim headers, a forced include, one more source (`game_libc.c`), five more compile flags (`/X /Zl /GS- /GR- /FI...`), three more link flags (`/NODEFAULTLIB /NOENTRY /MANIFEST:NO`) and one library: kernel32's import library, made from `game_libc_kernel32.def` (3.2).
- **The exe** is milestone 1's. It now also fills a table of 14 C library functions for the DLL (`game_host.c`).
- **The ABI** goes from 1 to 2: `ps3_host_api` gains `crt[PS3_GC_COUNT]` at its end. Host and DLL come from the same ps3recomp, as in milestone 1. A mismatch is refused at load with `[game-host] ... refused: init 2`, as today.
- **The lift** does not change, and nothing is re-lifted. The lifter's fixed header text moves into `ps3emu/ppu_lift.h` (4.5); a new lift's header includes it, an old lift's keeps the same text inline.

## 2. The C and compiler runtime symbols

### 2.1 The 20 symbols

Measured: `llvm-nm -u` on the split build's `des_game` objects (10-22). docs/57 6.2 counted 22 on 10-19's objects, `memset` among them; today's objects have 20. Sites are read in our sources. Line numbers are today's ps3recomp (`feat/dll-split` plus this patch) and today's lift.

| Symbol | Objects (measured) | Where it comes from (read) | Replacement | Why it is right |
|---|---|---|---|---|
| `__tls_guard`, `__dyn_tls_on_demand_init` | every PPU chunk, SPU image and job, lifted libsre | `PPU_THREAD_LOCAL` = `__declspec(thread)` (`ppu_recomp.h` 196-201, from `ppu_lifter.py` 270-275), on `g_trampoline_fn` (`ppu_lifter.py` 469, read by `DRAIN_TRAMPOLINE` 471-479); `SPU_THREAD_LOCAL` (`spu_context.h` 848-854) on `g_spu_trampoline_fn` (864) | `__thread` from the forced include (section 3) | `__thread` must be constant-initialised, so the compiler emits no guard. Both trampolines start at 0 (`game_module.c` 83-84). |
| `_tls_index` | the same, plus `game_module.c` | every implicit TLS access: `gs:[0x58]`, then `_tls_index`, then the section-relative offset | defined in `game_libc.c`, with the TLS directory | It is the loader's slot number for this module. The loader writes it only if the image has a TLS directory (section 3.2). |
| `_fltused` | PPU chunks 000-011, SPU images 0001-0011, libsre | the compiler, in any object that uses floating point | `int _fltused = 0x9875;` in `game_libc.c` | Only an anchor: nothing reads it. |
| `__security_cookie`, `__security_check_cookie` | PPU chunks 000-011, SPU images and jobs, libsre glue | `/GS` (on by default), in functions with a local array (inferred: the VMX temporaries `float d[4]`, the SPU helpers' lanes) | `/GS-` | The cookie only detects a stack overrun. It never changes a correct result. The checks also go from the hot paths (inferred: small). |
| `__imp__fdsign` | PPU chunks 000-011 | `signbit(a[i])` in `vmaxfp`/`vminfp` (`ppu_lifter.py` 2841, 2843). The UCRT's C++ `signbit` overload calls `_fdsign` (read, `corecrt_math.h`) | `signbit` macro over `__builtin_signbitf` / `__builtin_signbit` (`game_libc/math.h`) | Both return the sign bit, NaNs included. It becomes a bit test instead of a call. |
| `__imp_sqrtf` | PPU chunks 000-011 | `1.0f/sqrtf(b[i])` in `vrsqrtefp` (`ppu_lifter.py` 2873) | host thunk `sqrtf` | With math-errno (clang-cl's default; inferred from the references) the compiler emits `sqrtss`, then calls `sqrtf` only when the result is a NaN, to set errno. The call runs the exe's `sqrtf`: the same function as in the single exe. |
| `sqrt` | PPU chunks 000-011 | `fsqrt`, `fsqrts`, `frsqrte`, `frsqrtes` (`ppu_lifter.py` 2027-2039) | host thunk `sqrt` | As `sqrtf`. |
| `fma` | PPU chunks 000-011, libsre | the `fmadd` family's helper in the chunk preamble (`ppu_lifter.py` 362), which needs a fused result | host thunk `fma` | The same UCRT function, so the bits are exact. There is no FMA unit at `-msse4.1`, so it is a call on every `fmadd`. It was one already: the plain name (no `__imp_`) means the compiler made the call itself, and it went through the import library's jump stub. The thunk is the same one indirect jump (inferred). |
| `ldexp` | SPU images 0001, 0004, 0005, 0010, 0011 | the compiler's rewrite of `exp2((double)int)` in `spu_cflts`, `spu_cfltu`, `spu_csflt`, `spu_cuflt` (`spu_helpers.h` 333-339), where the scale is not a constant | host thunk `ldexp` | The same UCRT function as the single exe's call. The rewrite happens in both builds: the shim declares `exp2` with its standard prototype, so the compiler knows it as the library function. |
| `__imp_exp2` | SPU image 0010 only | the same four helpers, at a site the rewrite did not take (inferred: the argument is no longer a plain integer conversion after optimisation) | host thunk `exp2` | The same function. |
| `__imp_getenv` | SPU images and jobs, libsre glue | knob reads: `ps3_env_int` (`ps3_knob.h` 37), `spu_context.h` 677-680 and others; the glue's `LIBSRE_LLE` (`libsre_glue.c` 429) | host thunk `getenv` | `game_host.c` passes `&getenv`. In a `/MD` exe that reads the import slot, which `boot_main.cpp` 198 points at its cached `getenv`. In the CI host's static runtime, `getenv` is `boot_main`'s own function (ps3recomp #102, docs/85 2.3). Either way the DLL makes the same lookup as a single exe, and `GETENV_STATS` sees it (docs/57 3.6). |
| `__imp_atoi` | SPU images, job 0177B600 | `ps3_env_int` (`ps3_knob.h` 38), `spu_context.h` 678 | host thunk | The same function. |
| `__imp_strtoul` | SPU images, job 0177B600 | `spu_context.h` 679-680 | host thunk | The same function. |
| `__imp_fflush` | SPU images and jobs | `spu_context.h` 572, 959; `spu_helpers.h` 80 | host thunk | The exe's `stderr`, flushed by the exe's C runtime. |
| `__imp___acrt_iob_func` | SPU images and jobs, libsre glue | the UCRT's `stderr` macro, in every `fprintf(stderr, ...)` | host thunk; the shim's `stderr` is `__acrt_iob_func(2)` | The `FILE*` is the exe's own. |
| `__imp___stdio_common_vfprintf` | SPU images and jobs, libsre glue | the UCRT's `fprintf` is inline in `stdio.h` and calls it (read). Sites: `spu_context.h` (13 calls) and `spu_helpers.h` (1), the lifted `HALT-ASSERT` line (`spu_lifter.py` 901), `libsre_glue.c` 435 and 475, `game_module.c` | host thunk `fprintf` to `ps3_gh_fprintf` in `game_host.c`, which calls `vfprintf` | A jump thunk leaves the variable arguments as the caller passed them. The output is the same text to the same stream. |
| `fputc`, `fwrite` | SPU images, job 0177B600 | the compiler's rewrite of `fprintf` calls with a constant format (inferred: a lone character, or no conversion) | host thunks; `fputs` too, for the `"%s"` rewrite | The same functions the single exe calls. |

Also covered, though not referenced today (measured: absent):
- `memcpy`, `memmove`, `memset`, `memcmp` and `strlen` are defined in `game_libc.c`. The compiler may call them for a large or variable-size copy. Today it inlines every copy.
- `fmaf` is declared, for `spu_helpers.h`'s `__FMA__` branch. That branch is an instruction, never a call.
- `__chkstk` (frames over 4 KB), `__udivti3` (128-bit division), `_Init_thread_header` (C++ thread-safe local statics) and `__CxxFrameHandler3` (C++ unwinding with destructors) are not defined. If a future lift needs one, the link fails and names it (section 7).

### 2.2 Why thunks into the exe, not our own copies

- **The same bits.** `fma`, `sqrt`, `ldexp` and `exp2` are the UCRT's, as in the single exe and in milestone 1. A copy of our own would need its own correctness argument for every input: `fma` exactly, and `ldexp` and `exp2` at every subnormal and overflow edge. A thunk needs none.
- **The same cost.** The calls the compiler makes itself (`fma`, `sqrt`, `ldexp`, `fputc`, `fwrite`) already went through an import jump stub in both builds (inferred from the plain names). The dllimport calls (`__imp_getenv` and the like) gain one jump. They are all on cold paths: knob latches, log lines, the NaN path of a square root.
- **The same state.** `stderr`, the environment cache and errno are the exe's, so a log line from the DLL lands in the same stream as the exe's.
- **One mechanism.** `PS3_GAME_THUNK`, the slots and the null check at the load are milestone 1's, used for a second list.

### 2.3 What `game_libc.c` defines

The file is empty unless `PS3_GAME_FREESTANDING` and `_WIN64` are set.

- `memcpy`, `memmove`, `memset` (`rep movsb` / `rep stosb`), `memcmp` and `strlen`. They are marked `no_builtin`, so the compiler cannot turn their loops back into calls to themselves. `memmove` copies backwards, with the direction flag set and cleared again, only when the target starts inside the source.
- `_fltused`.
- **The static constructors.** The DLL has no C runtime, so nothing runs its `.CRT$XCU` list. `game_libc.c` places markers in `.CRT$XCA` and `.CRT$XCZ`, and `ps3_game_libc_run_ctors` calls every entry between them, as the CRT's `_initterm` does. `ps3_game_module_init` calls it once the C library slots are filled. The lift has no constructor today (inferred: milestone 1 removed the workload table's, and the test in section 8 checks the objects). A destructor would need `atexit`, which is not in the list, so it fails to link instead of being dropped.
- **The TLS directory** (section 3.2), and the one import that makes Windows set it up.

## 3. Thread-locals

### 3.1 Why the three symbols appear

- `__declspec(thread)` in clang-cl is a thread-local "that may need dynamic initialisation". MSVC 2015 and later allow a `thread_local` with a dynamic initialiser, and clang-cl follows that in C as well as C++ (read; measured: the SPU images are C, and they reference the guards).
- An access to such a variable **defined in another file** gets an on-demand guard. The code reads the thread's `__tls_guard` byte, and the first time on each thread it calls `__dyn_tls_on_demand_init`. That runs the module's TLS initialisers from the C runtime.
- `_tls_index` is in every implicit TLS access: `mov rax, gs:[0x58]` (the thread's slot table), the module's slot `_tls_index`, then the variable's offset in the `.tls` section.
- The lifted code's thread-locals are the two trampolines. `g_trampoline_fn` is read after every lifted call (`DRAIN_TRAMPOLINE`). `g_spu_trampoline_fn` is read after each SPU call bracket (`spu_call_done`, `spu_context.h` 909) and by `SPU_RET*`. The DLL defines both (`game_module.c` 83-84, docs/57 3.4). `g_spu_pch` (`spu_context.h` 870) is used only by `SPU_DRAIN`, which the runtime runs, not the lift (inferred from docs/57's boundary: the DLL owns two thread-locals).

### 3.2 Option (a): `__thread` and the DLL's own TLS directory (recommended)

- **The keyword.** `ps3emu/game_freestanding.h` defines `PPU_THREAD_LOCAL` and `SPU_THREAD_LOCAL` as `__thread`. Both headers define them only when they are not defined yet (`#ifndef`: `ppu_recomp.h` 196, and `spu_context.h` 848 after this patch), so the lift is unchanged. `__thread` must have a constant initialiser, so no guard is emitted (read). The fallback is `/Zc:tlsGuards-` (clang 18 and later), which turns the guards off for the whole compile (inferred: the flag's name and version are from memory).
- **The directory.** `game_libc.c` defines what the CRT's `tlssup` supplies:
  - `_tls_index`, which starts at `PS3_GAME_TLS_INDEX_UNSET` (0xFFFFFFFF);
  - `_tls_start` in `.tls` and `_tls_end` in `.tls$ZZZ`. The compiler puts the thread-locals in `.tls$`, and the linker sorts the three by the text after `$` and merges them into one `.tls`;
  - an empty callback list;
  - `_tls_used`, an `IMAGE_TLS_DIRECTORY64` in `.rdata$T`, kept by `/include:_tls_used`. The linker points the image's TLS data directory at the symbol with that name (inferred: link.exe's documented behaviour, which lld-link follows).
- **The check.** If the loader did not process the directory, `_tls_index` stays at the sentinel. Without the check, every thread would then use slot 0, which is the exe's. `ps3_game_module_init` refuses such a module (code 5) with a message, instead of letting the trampolines alias the exe's thread-locals.
- **The loader, and the one import.** Windows sets up the TLS of a DLL loaded with `LoadLibrary`, for every thread, those already running included, but only when the image has an import table (measured 10-22, Windows 11 26200). The first freestanding DLL had none: the loader never wrote `_tls_index`, and `ps3_game_module_init` refused the DLL with code 5, as designed, instead of running on the exe's slot. Probe DLLs with the same directory showed it: with no import, `_tls_index` stays as it starts (0, 7 or the sentinel), with or without an entry point; with one import, the loader writes slot 1. A normal CRT DLL always has imports, so milestone 1 never met it.
  - The fix: `game_libc.c` asks for kernel32's `GetCurrentThreadId` (`/include:__imp_GetCurrentThreadId`), which nothing calls. Its import library comes from ps3recomp's `runtime/ppu/game_libc_kernel32.def`, made at build time with `lld-link /lib /def:` (measured: works with no Visual Studio environment and no SDK). kernel32 is in every Windows, so the import costs nothing.
  - The build checks the DLL's imports: exactly that one function.
- **Hot-path cost.** Each trampoline read is the TLS base (one `gs:` load, one `_tls_index` load, one slot load) plus the pointer's load and test. The guard's byte load, compare and branch go away, and so does the cold guard call at every site. That is a little less work and a little less code than milestone 1 (inferred: to be measured, section 8).

### 3.3 Option (b): the trampolines in the contexts

The lifted code always has the context in a register. A trampoline at a fixed offset in `ppu_context` and `spu_context` costs one load from `[ctx + offset]`: no `gs:` read, no slot table. It also removes the DLL's last thread-locals, and the host's per-thread cached pointer (`game_split.h` 91-92, `game_host.c` `ps3_split_*_tramp_init`).

What it touches (read):
- **The PPU lifter:** the declaration (`ppu_lifter.py` 469); `DRAIN_TRAMPOLINE` (471-479); the setters (1681, 1693, 1760, 1773, 1803, 3678); the pattern checks that recognise them (525, 548, 573, 680, 3365); the header template (267-275).
- **The SPU lifter** setters (`spu_lifter.py` 625, 733, 1132, 1163, 1167, 1191, 1207, 1252, 1345), and `spu_context.h`: the declaration (864), `spu_call_done` (909), `SPU_DRAIN` (967-982), `SPU_TAILCALL_TO` (1013), `SPU_RET` (1025) and `SPU_RET_REG` (1035).
- **The runtime:** `ppu_loader.cpp` (the definition at 2291; drains at 2755, 3118, 4110, 4209, 4249, 4388); `spu_drain.c` (23, 480-538); `spu_channels.c` (142, 381-402, 2318, 2774-2817, 3091); `spu_diff_hook.c` 273; `spu_ring_hook.c` 539 and 741.
- **The split's glue:** `game_module.c` 83-86, `game_module.h`'s slot functions, `game_split.h` 91-95, and `game_host.c`'s per-thread cache.
- **The layouts:** the lifted `ppu_context` copy in `ppu_recomp.h` must match `runtime/ppu/ppu_context.h` field for field.
- **The tests:** `smoke_recomp.c`, `test_prx_loader.cpp`, `test_spu_lifted_start.c`, `lift_selftest.py`, `spu_lift_selftest.py`, `test_ppu_fallthrough.py` and `test_ppu_cs_epilogue_restore.py`; also `spbalance.py` and `callgraph.py`, which read the setters' text.
- **A full re-lift:** the PPU chunks, the SPU images and jobs, and libsre.
- **The meaning changes** from "per host thread" to "per context". These agree while each thread runs one context at a time. That is stated for SPU contexts (`spu_context.h` 860-861). For PPU contexts it needs an audit: a callback that runs another context on the same thread, or a context that moves between threads (inferred risk).

### 3.4 Decision

(a) for milestone 2: no lifter change, and nothing to re-lift. (b) is a separate perf change, measured on its own, if the A/B in section 8 or a profile ever shows the trampoline reads.

A side note: the single exe (`des_boot`) pays the same guard today. `/Zc:tlsGuards-`, or the same `__thread` define, would remove it there too. That is a separate change with its own A/B (inferred: small).

## 4. Headers

### 4.1 What the game sources include today (measured)

Clang's `/showIncludes` on one source of each kind (10-22). The counts are of headers from Visual C++ (the STL and vcruntime) and from the Windows SDK (the UCRT).

| Kind | Visual C++ | SDK | What pulls them in (read) |
|---|---|---|---|
| PPU chunk, lifted libsre | 29 | 20 | `<atomic>` for `PPU_FENCE` (`ppu_recomp.h` 21-29), which brings `type_traits`, `xatomic.h`, `xthreads.h`, `ctime` and `cstdlib`. The preamble's `stdio.h`, `string.h`, `math.h` (`ppu_lifter.py` 295-298) and `intrin.h` (414). `string.h` again for the inline accessors (`ppu_recomp.h` 97). |
| SPU image, SPU job | 8 | 16 | `spu_context.h` 13-18 (`stdint.h`, `string.h`, `stdio.h`, `stdlib.h`, and `stdlib.h` through `ps3_knob.h`), 491 (`stdlib.h` for `_byteswap_ulong`), 787 (`intrin.h`); `spu_helpers.h` 16-25 (`string.h`, `math.h`, `intrin.h`) |
| libsre glue | 6 | 8 | `stdio.h`, `stdlib.h`, `stdint.h` (`libsre_glue.c` 13-15) |
| workload table | 14 | 13 | `ps3types.h`'s C++ part (73-77: `endian.h`, `<type_traits>`, `<cstring>`), through `spu_workload.h` and `spu_context.h` |
| `game_module.c` | 6 | 8 | `stdint.h`, `stdio.h`, `ps3_knob.h` |
| `des_game_module.cpp` | 9 | 4 | `<cstdint>`, `<cstdio>` |

Clang's `intrin.h` includes `setjmp.h` in a hosted compile. Through `x86intrin.h` and `immintrin.h`, `mm_malloc.h` then includes `stdlib.h` and, on Windows, `malloc.h` (read in clang 22's headers; measured with `/X`).

### 4.2 The C++ library uses, and what replaces them

- **`std::atomic_thread_fence` and `std::memory_order_*`** (`PPU_FENCE`, `ppu_recomp.h` 24), in every PPU chunk and libsre. They are replaced by the forced include's `PPU_FENCE(o)`. It is the MSVC STL's own lowering on x64 (read, `<atomic>`): `relaxed` is nothing. `consume`, `acquire`, `release` and `acq_rel` are a compiler barrier (`_ReadWriteBarrier`). `seq_cst` is a locked increment of an uninitialised stack word between two barriers. So the fences compile to the same instructions (inferred: section 8 compares the disassembly).
- **`be_t`, `vm::ptr`** and their `std::is_arithmetic`, `std::is_enum`, `std::underlying_type_t` and `<cstring>` (`ps3types.h` 73-239). The only game-side C++ source that includes them is the workload table, which uses none of them. With `PS3_GAME_FREESTANDING`, `ps3types.h` leaves the C++ part out. A game-side source that did use `be_t` would fail to compile, not misbehave.
- **`std::fprintf`** (`des_game_module.cpp` 38) becomes `fprintf`, with `<stdio.h>` and `<stdint.h>` in place of `<cstdio>` and `<cstdint>`. The same code in every build.
- Nothing else: the lifters emit no `std::` (read: `ppu_lifter.py`, `spu_lifter.py`), and the include lists above have no other C++ header.

### 4.3 The shim folder and the forced include

`ps3recomp/include/ps3emu/game_libc/`, first on `des_game`'s include path, with `/X` so that nothing from Visual C++ or the SDK is found:

| Header | Declares | For |
|---|---|---|
| `stdio.h` | `FILE` (incomplete), `EOF`, `__acrt_iob_func`, `stdin`/`stdout`/`stderr`, `fprintf`, `fputc`, `fputs`, `fwrite`, `fflush` | the log lines |
| `stdlib.h` | `getenv`, `atoi`, `strtoul`, `_byteswap_ushort/ulong/uint64` (compiler builtins once declared), `malloc`, `free` | knobs, `SPU_BSWAP32`, `mm_malloc.h` |
| `string.h` | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen` | copies (inline for small sizes, as before) |
| `math.h` | `sqrt(f)`, `fabs(f)`, `trunc(f)`, `floor(f)`, `ceil(f)`, `nearbyint(f)`, `round(f)`, `copysign(f)`, `fma`, `fmaf`, `ldexp`, `exp2`; `signbit`, `isnan`, `isinf`, `isfinite`, `INFINITY`, `NAN`, `HUGE_VAL(F)` as builtins | the lifted float code |
| `malloc.h` | `_aligned_malloc`, `_aligned_free` | `mm_malloc.h` only (its inline `_mm_malloc` is never called) |
| `setjmp.h` | `jmp_buf`, as MSVC's x64 header defines it | the declaration of `_setjmp` in clang's `intrin.h` |

- Only what the game sources call is declared. A call to anything else fails to compile, which is earlier than failing to link.
- The rounding functions are SSE4.1 instructions at `-msse4.1`, which both `DES_PPU_OPT` and `DES_SPU_OPT` give. Without it they would be C runtime calls, so the CMake module requires both options.
- `ps3emu/game_freestanding.h` is force-included (`/FI`) before each source's own includes. It defines `PS3_GAME_FREESTANDING`, includes `stddef.h`, `stdint.h`, `limits.h` (`INT_MIN` and the rest, which the lifted code used to get through `<atomic>`) and clang's `intrin0.h` (for `_ReadWriteBarrier`), and defines the thread-local keyword and `PPU_FENCE`.

### 4.4 Why a hosted compile with `/X`, not `-ffreestanding` (measured)

- `-ffreestanding` turns the inline `memcpy` of 4 and 8 bytes into calls, and `sqrt` loses its `sqrtsd`. `-fbuiltin` does not undo either. The lifted code's guest loads and stores are those `memcpy`s, so that is not usable.
- Without `/X`, clang-cl finds the Visual C++ and SDK headers through the registry, even in a clean environment. With `/X`, the only system folder is clang's `lib/clang/22/include`.
- So: hosted (the compiler keeps knowing the library functions), `/X`, the shims and the support file.

### 4.5 The fixed and per-lift parts of `ppu_recomp.h`

The DLL is compiled from the lift directory, so milestone 2 itself does not need this split. The exe does, once CI builds it with no lift (docs/85 section 4, whose stopgap asks the lifter for an empty lift's header). It is done here, on a branch of its own merged into milestone 2's (no re-lift):
- **`ps3recomp/include/ps3emu/ppu_lift.h` is the one copy of the fixed text:** the old preamble (`PPU_FENCE`, `ppu_vr`, the `ppu_context` copy, the inline accessors, the `vm_*` prototypes in `extern "C"`, `lv2_syscall`, `PPU_THREAD_LOCAL`, the function table) and the shadow-stack block, inside an include guard.
- **The lifter reads that file** and cuts its parts out at marker lines, so `HEADER_PREAMBLE` and `FENCE_PREAMBLE` stay byte-identical to the old constants (measured, by a test).
- **A new lift's `ppu_recomp.h`** is the banner, `#pragma once`, `DS_SHADOW_STACK`'s define for a `--shadow-stack` lift, `#include "ps3emu/ppu_lift.h"`, then the per-game lines. A `--symbol-prefix` lift (libsre's) keeps the text inline, byte for byte as before.
- **Old lifts work as they are.** The lifted sources include only their own header and the host's sources only the fixed one; no source includes both. Today's lift's inline text is exactly the fixed header's (measured: its text up to the end of the shadow-stack block is the lifter's inline form, with the shadow stack on).
- **The host's 13 sources** include `ps3emu/ppu_lift.h`. In a single exe the port's sources also call 29 lifted functions by name, which the lift's header used to declare; `des_game_calls.h` now declares them, with the same declarations.
- **The shadow stack.** With `DES_SHADOW_STACK=ON` and a `--shadow-stack` lift (`scripts/lift_ppu.json`), CMake defines `DS_SHADOW_STACK=1` on the host's sources, which no longer see the lift's header. Not on the whole target: libsre is lifted without it. The default build defines `DS_NO_SHADOW_STACK`, as before.
- **`runtime/ppu/ppu_context.h`** keeps its second copy of `ppu_context` (its `vr` is a `u128` with other members), and `boot_main.cpp` still claims its guard.
- **Checked** (measured 10-22): the host's objects are byte-identical before and after, apart from the time stamp, in a single-exe build (11 objects) and in the split build (12). Two chunks (000 and 013) preprocess byte-identically against today's inline header and against the include form the new lifter writes (made from today's header by a helper script, no re-lift). The lifter's tests pass, a new one included.
- Then docs/85's stopgap step goes.

## 5. Build flags and link line

Compile (each `des_game` object: milestone 1's flags as measured in its `build.ninja`, plus this patch's additions; the order is inferred):

```
clang-cl /nologo -TP -DDS_NO_SHADOW_STACK -D_CRT_SECURE_NO_WARNINGS -DPS3_GAME_FREESTANDING
  -I<ps3recomp>/include/ps3emu/game_libc -I<port>/compat -I<lift> -I<ps3recomp>/include -I<ps3recomp>/runtime/ppu -I<ps3recomp>/libs/video
  /DWIN32 /D_WINDOWS /EHsc -std:c++20 -MD /bigobj /O2 /clang:-fwrapv /clang:-msse4.1 /Zi /EHsc /Zc:__cplusplus
  /wd4100 /wd4244 /wd4267 /wd4146 /wd4334 /X /Zl /GS- /GR- /FI<ps3recomp>/include/ps3emu/game_freestanding.h
  /Fo<obj> /Fd<pdb> -c -- <source>
```

- SPU images also get their source options `/O2 /Zi /clang:-msse4.1`. C sources get no `/GR-`.
- `-MD` stays: with `/Zl` it no longer names `msvcrt.lib`, and nothing in the shims reads the `_DLL` it defines.
- `/X`: no Visual C++ or SDK headers. `/Zl`: no default-library names. `/GS-`: no cookies. `/GR-`: no RTTI (`type_info` is the C++ library's).

Link (`cmake -E vs_link_dll ... -- lld-link`, the linker part, which is what setup runs):

```
lld-link /nologo <objects> /out:des_game.dll /implib:des_game.lib /pdb:des_game.pdb /dll /version:0.0
  /machine:x64 /MAP:des_game.map /NODEFAULTLIB /NOENTRY /MANIFEST:NO <gen>/game_libc_kernel32.lib
  [/DEBUG /OPT:REF /OPT:ICF]
lld-link /lib /nologo /machine:x64 /def:<ps3recomp>/runtime/ppu/game_libc_kernel32.def /out:<gen>/game_libc_kernel32.lib
```

- **One library, made at build time.** Milestone 1's line ends with CMake's ten default Windows libraries (`kernel32.lib` to `advapi32.lib`, measured in its `build.ninja`). These would need the SDK at setup, so the module clears `CMAKE_C_STANDARD_LIBRARIES` and `CMAKE_CXX_STANDARD_LIBRARIES` for the port's folder and gives the same ten to `des_boot` and `des_host` by name (measured: it works per folder; the DLL links with none of them). The one library is the kernel32 import library above, in the split's generated folder (setup's `{gen}`), so setup's compile has to make it too.
- **`/MANIFEST:NO`** keeps `vs_link_dll` from embedding its default manifest. docs/81 7 found that manifest to be the only difference between CMake's DLL and setup's (measured: the DLL has no `.rsrc` section).
- **`_tls_used`** is kept by the `/include:` in `game_libc.c`'s directives. `/Zl` drops only default-library names.
- **The export** is `des_game_init`, as before. There is no entry point.

## 6. Order of the patch

Two PRs, ps3recomp first. Both are opt-in.

1. **ps3recomp:** the shims, the forced include and `game_libc.c` (new files, unused elsewhere); the `ps3types.h` and `spu_context.h` guards (no effect unless `PS3_GAME_FREESTANDING` or `SPU_THREAD_LOCAL` is set); ABI 2 with the C library list (`game_module.h`, `game_module.c`, `game_host.c`).
   Then the one import (`game_libc_kernel32.def`), and the header split (`ps3emu/ppu_lift.h`, the lifter, the runtime's host sources), each its own commit.
2. **The port:** `des_game_module.cpp` on the C headers; the `DES_SPLIT_FREESTANDING` option and `cmake/DesGameFreestanding.cmake` with the import library; the host sources on `ps3emu/ppu_lift.h`; this note; then the pin bump.
3. **After the first freestanding build:** `game_build.json` written again from it (`scripts/game_build_desc.py`, on the setup branch). The install list gains `game_libc.c` and `game_libc_kernel32.def` (sources), the shim headers (from ninja's recorded dependencies on their own), and `ps3emu/ppu_lift.h`, which the lifter reads when it runs. `compile.py` gains the import-library command before the link.

What stays the same:
- **The default build:** no new option is on. `ps3types.h`'s guard and `spu_context.h`'s `#ifndef` give the same text without the new define. `game_module.h` is only in split builds. So `des_boot`'s objects are unchanged (inferred; checked in section 8).
- **Milestone 1's split:** the lifted objects are unchanged. `game_module.obj` and `game_host.obj` change (ABI 2, and a `crt` table the milestone 1 DLL ignores). `des_game_module.obj` should come out the same (the same calls with C headers; inferred).

## 7. Risks

- **The TLS directory.** A wrong `_tls_used`, or `.tls` not merged as expected, means either the loader rejects the DLL or `_tls_index` stays at the sentinel (refused, code 5). Neither is silent. It happened once, for another reason (no import table, 3.2), and the check caught it at the first boot.
- **`__thread` in clang-cl.** Accepted in MSVC mode as constant-initialised TLS: no object references `__tls_guard` or `__dyn_tls_on_demand_init`, and the DLL links with `/NODEFAULTLIB` (measured).
- **Static constructors.** Today: none expected. Now run by `ps3_game_libc_run_ctors` in any case. `.CRT$XI*` (C initialisers) and `.CRT$XD*`/`.CRT$XL*` (TLS initialisers and callbacks) are not run; the objects are checked for them.
- **New compiler calls.** `__chkstk`, 128-bit division helpers, `_Init_thread_header`, `__CxxFrameHandler3` and `__std_terminate` fail the link with their names, never silently. `__chkstk` can then join `game_libc.c` (compiler-rt's `___chkstk_ms` body: probe each page below `rsp`, keep every register but `r10`, `r11` and the flags).
- **`_MSC_VER` drift.** clang-cl takes it from the installed Visual Studio (19.44 on the build PC, measured: `--msvc-ver=1944`), or from a built-in default where there is none, as on a player's PC. With `/X` it still changes some of clang's own behaviour and the objects' bytes. Pinning `-fms-compatibility-version` in the commands removes the drift (open question 1).
- **Variable arguments through a jump thunk.** The thunk is one `jmp`: registers and stack are as the caller left them, floats in both XMM and integer registers included. `ps3_gh_fprintf` then uses `va_start`/`vfprintf` (read).
- **`longjmp` through the DLL's frames** (`spu_halt`, thread exit). The UCRT's `longjmp` unwinds with the DLL's `.pdata`, which lld emits without the C runtime, as in milestone 1 (inferred).
- **`/GS-`** drops the stack-overrun fast-fail in lifted code. It only matters for debugging.
- **`getenv`** now goes through the exe's cache (`cached_getenv`, behind an SRW lock). That is the single exe's behaviour, and different from milestone 1, where the DLL read the C runtime's environment directly.
- **ABI 2.** Host and DLL must come from the same ps3recomp. A mismatch is refused at load, with its code.
- **The CI host** (docs/85) links the Visual C++ runtime statically and uses Windows' UCRT. docs/85 2.4 needs the DLL and the exe to share one UCRT, for the environment and `stderr`. A freestanding DLL has no C runtime and uses the exe's, so that need goes away. Each of the 14 entries is the exe's own function, or its import-library stub to `ucrtbase.dll`: one more jump in the second case (inferred).
- **The exes' link order** in the freestanding build directory: the ten libraries move from the end of the line into `LINK_LIBRARIES`. The import table's order may differ from milestone 1's exe (inferred: harmless; that directory's exes are not the A/B's).
- **The A/B's resolution.** A 1-2% difference needs plain windows and enough rounds (docs/57 4.3, docs/83).

## 8. How it was tested (10-22)

Built from today's lift (the merged masters' lift, 10-22) in a new build directory, `DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON`, with 3 jobs (memory was short): 328 compiles in 11 minutes. All measured.

1. **The options off.** With `DES_SPLIT_FREESTANDING` off, the port configures to the same commands as before for every target: 343 without the split, 399 with it (`ninja -t commands`, the old and new `CMakeLists.txt` in the same tree).
2. **A compile trial** of one source of each kind (chunk 013, job 0177B480, SPU image 0010, libsre, the glue, the workload table, `game_module.c`, `des_game_module.cpp`, `game_libc.c`): all compiled at the first try.
3. **Undefined names** (`llvm-nm -u`): each object leaves only the boundary names, the 14 thunks' names, `_tls_index` and `_fltused`. None of section 2's other 18 is left: no TLS guard, no stack cookie, no `_fdsign`.
4. **Sections:** `game_libc.obj` has `.CRT$XCA` and `.CRT$XCZ`, `.tls`, `.tls$ZZZ` and `.rdata$T`. The DLL's `.CRT` is 16 bytes, the two markers: no static constructor.
5. **The DLL:**
   - imports kernel32's `GetCurrentThreadId` only (3.2; the first build had no import at all and was refused at the first boot);
   - exports `des_game_init` only;
   - entry point 0;
   - a TLS directory whose index field is `_tls_index`, which the loader sets (slot 1 in a test load);
   - 259,508,736 bytes, against milestone 1's 289,020,416: 29.5 MB (10%) less, the TLS guards and stack cookies gone from every site.
6. **A boot** from the deep Palace save: `[game-host] loaded ... des_game.dll: 94017 functions`, the Palace, no `[game]` or `[des_game]` line.
7. **The A/B** (plain windows, deep Palace, `RSX_VSYNC=0 DES_VSYNC=0 DES_FPS=unlocked`, 960 kHz vblank, 8 rounds each side): side A milestone 1 (`des_host.exe` + its `des_game.dll`), side B milestone 2's exe and DLL.
   Result: no significant difference. The plain-window rate (docs/83), which decides: A 143.4 fps, B 144.9 fps (means); median B vs A +0.78%, B faster in 4 of 7 complete rounds, paired t p = 0.29. The perturbed windows: median +0.04%, p = 0.79. All 16 boots reached the Palace. Both sides were lower than this morning's baselines (about 150), with only 3 to 5 GB of memory free.

Not done yet:
- the disassembly of chunk 013 and job 0177B480 against milestone 1's (the guards and cookies gone, the fences unchanged);
- `regress.sh` and `shotdiff.py` against milestone 1 (identical shots expected);
- `LIBSRE_LLE=1` and a live `setenv` from the console, for the thunks' `getenv`;
- (done 10-23, section 9) setup: `game_build.json` from this build, and `compile.py` with the import-library step and only the `llvm-win64` download.

## 9. Setup builds it (10-23)

Setup's compile (`tools/setup/des_setup/compile.py`, docs/81 section 7) now builds this DLL, so a player's PC needs no Visual Studio, Windows SDK or C runtime for it: only the pinned `llvm-win64` download (docs/86).

- **The description.** `game_build.json` is written again by `scripts/game_build_desc.py`, from a CMake build with `DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON` of a lift that setup made (14 chunks). Every object's command gains `-DPS3_GAME_FREESTANDING`, the shim folder first on the include path, `/X /Zl /GS-`, `/GR-` for C++, and the forced include. One more object: `game_libc.c`. The link loses the ten Windows libraries and gains `/NODEFAULTLIB /NOENTRY /MANIFEST:NO` and `{gen}/game_libc_kernel32.lib`.
- **The import library** is a journal step of its own, `lib:game_libc_kernel32`, before the link: `lld-link /lib /nologo /machine:x64 /def:{src}/ps3recomp/runtime/ppu/game_libc_kernel32.def /out:{gen}/game_libc_kernel32.lib`, CMake's custom command. The generator reads it from `ninja -t commands des_game` into the description's `libs` list, so the description still holds only CMake's commands. The step is done again when its command, the linker or the `.def` file changes, and the link's fingerprint covers it. A missing `.def` stops with its name.
- **The headers** the install must have: 16. The six shims, `game_freestanding.h` and `ppu_lift.h` are new. `endian.h` is no longer read. `ppu_lift.h` is read because a new lift's `ppu_recomp.h` includes it (4.5). The 10-22 build used an older lift, which keeps that text inline, so a description written from it lacked the header. That is why this one comes from a lift setup made.
- **The generator** now refuses a build that is not freestanding. It maps a link input named relative to the build directory (the import library, `split_gen\...`) to `{gen}`.
- **The `_MSC_VER` question** (open question 1) is not settled by this. On this PC clang still finds Visual Studio's version; on a PC without it, clang uses its own default, and the objects may differ in some bytes (section 7). The sources the DLL compiles test only whether `_MSC_VER` is defined, never its value (measured: no comparison in the shims, the runtime headers, the glue or the lift).

Tested (measured):
- **The description** matches its CMake build (`game_build_desc.py --check`). 104 setup unit tests, 4 of them new (the library made before the link; a changed `.def`; a missing `.def`; a failed `/lib`; the generator's parsing of ninja's command).
- **A real setup** from a fresh data folder, through the screens, with the scheduler and the shaders phase merged in. The compiler was the pinned `llvm-win64-22.1.8-1` zip, put in the data folder's `downloads/` beforehand (fetch reused it), and no local compiler was given. The plan read `check fetch stage lift compile shaders`. The run took 845 s and the compile 771 s: 34 objects, `lib:game_libc_kernel32` in 0.1 s, the link in 0.5 s.
- **Setup's DLL is CMake's.** 33 of the 34 objects are byte for byte the objects of the CMake build above, apart from the COFF time stamp. The libsre glue differs only through the absolute path of the libsre image that the lift writes into it: the two builds' data folders, in two strings, their hashed symbol names and two section checksums (docs/81 section 5, "the glue's absolute libsre image path"). The DLL: 259,510,272 bytes, as CMake's; imports kernel32's `GetCurrentThreadId` only; exports `des_game_init` only; entry point 0; a TLS directory.
- **The headers:** the 16 files under the source tree that the objects read, from the journal's dependency lists, are exactly the description's 16.
- **The player's start** after that setup: `[game-host] loaded ...\game\des_game.dll: 94017 functions`, "shaders: cache up to date", the pinned save played for 200 s; the Tutorial rendered with its HUD (10 frames dumped, the last one looked at).
- Compared with the hosted build (milestone 1's commands, same lift): each chunk took the same time (1,415 s in all, against 1,398 s), and peaked about 0.1 GB lower (chunk 008: 2.59 GB, against 2.68).
- Not tested: a PC without Visual Studio (a VM, setup plan item L).

## Open questions (decisions)

1. **Pin the compiler's MSVC version** in the freestanding commands (`-fms-compatibility-version=19.44`, maybe `--target=x86_64-pc-windows-msvc19.44`), so the build PC, CI and players' PCs compile the same thing? Recommended: yes, the build PC's value.
2. **`-fno-math-errno` later?** It would make `sqrt` and `sqrtf` pure instructions and drop two thunks. But it changes code generation in every chunk, and the NaN results then need their own argument (inferred: the same default NaN). Recommended: not in milestone 2.
3. **`/GS-`, or cookies of our own** (`__security_cookie` and `__security_check_cookie` in `game_libc.c`, the cookie set at init)? Recommended: `/GS-`.
4. **Option (b)** (the trampolines in the contexts): when, if ever? Recommended: only after a profile shows the reads.
5. (Done: the `ppu_recomp.h` split, 4.5.)
6. **`/Brepro`** for the objects and the DLL, so a player's DLL is byte for byte the build PC's? docs/81 found time stamps the only difference.
7. **`__chkstk`** now, unused, or when a lift first needs it? Recommended: when needed (the link names it).
8. **More A/B spots?** Deep Palace with 8 rounds each side is measured (section 8). The pinned-save spot, where per-call costs weigh more (docs/57 6.1), is not.
