# The DLL split, milestone 1: des_host.exe and des_game.dll

Date: 2026-10-19; rebased onto the masters of 2026-10-22 and checked again (section 7). Status: in review (a ps3recomp and a port pull request). The split build boots into Boletaria. In a 6-round A/B its frame rate is not significantly different from master's: -1.2% and -2.3%, with 95% intervals that include zero (4.3).

This is step 1 of docs/45 (section 6.1) and its milestone 1: `des_host.exe` loads `des_game.dll`, built from today's lift with today's toolchain (clang-cl, both sides `/MD`), reaches Boletaria, and runs at the same frame rate as the single exe.

Labels, as in the other notes:
- **Measured**: from a build, a run, or `llvm-nm` on built objects.
- **Read**: seen in our source.
- **Inferred**: a conclusion that still needs a check.

## Short answer

- **Opt-in.** The port's `DES_SPLIT=ON` adds two targets, `des_game` (a DLL) and `des_host` (an exe), beside `des_boot`. ps3recomp's `PS3RECOMP_GAME_SPLIT=ON` builds the runtime library for the host, in its own build directory. Both options are off by default, and the default build does not change (section 5).
- **No lifter change.** The DLL is built from the lift directory as it is. Its lifted sources compile with `des_boot`'s exact command lines (measured, section 5), so their objects are the single exe's objects.
- **The boundary is small.** The lifted objects use 85 symbols from outside the lift, and the port uses 33 from inside it (measured, section 2). The DLL defines a jump thunk for each runtime and port function, owns the globals the inline memory accessors read and the two trampoline thread-locals, and exports one function, `des_game_init`.
- **Result.** `des_host.exe` loads `des_game.dll` (94,017 functions, in 78 ms). From the pinned save it boots through the title into the first level. At the deep Palace spot (about 113 fps), a 6-round A/B finds no significant difference from master. The split's means are -1.2% (profile windows) and -2.3% (between windows), with 95% intervals of -3.9% to +1.6% and -6.0% to +1.5% (section 4.3). The DLL's 30 lifted objects are byte-identical to master's `des_boot` objects (measured, section 5).

## 1. Building and running it

Runtime library for the host (ps3recomp checkout, its own build directory):

```
cmake -S ps3recomp -B ps3recomp/build_split -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_GAME_SPLIT=ON
cmake --build ps3recomp/build_split --target ps3recomp_runtime
```

Port, configured as usual plus `-DDES_SPLIT=ON`, then:

```
cmake --build port/build --target des_host des_game
```

`PS3RECOMP_SPLIT_LIB` names the split runtime library; it defaults to `ps3recomp/build_split/ps3recomp_runtime.lib`. The default `ps3recomp/build` library still links `des_boot` in the same build directory.

To run, keep `des_game.dll` beside `des_host.exe`, or set `PS3_GAME_MODULE` to its path. The test tools already work with it, with no change (measured: every run in section 4 used them). `DES_EXE=<build>/des_host.exe` stages the exe as `des_boot.exe` together with every DLL beside it, so the run checks and `des_kill_own` work as before. They do not stage `des_game.map`. The log says `[game-host] loaded <path>: <n> functions, <t> ms` once the DLL is in. Any refusal is a `[game-host]` or `[des_game]` line, and the boot stops there. For example, with no DLL beside it, `des_host.exe` stops at once with `[game-host] cannot load ...\des_game.dll (error 126)` and `game module load failed`, exit code 1. With `PS3_GAME_MODULE` naming the DLL in another folder, it loads that one (both measured).

## 2. The boundary (measured)

`llvm-nm` on master's objects (port d408341, ps3recomp d5dec60, the master lift with 39 hooks): the 14 PPU chunks, the 12 SPU images and 2 SPU jobs, the lifted libsre and its glue on the game side; everything else in `des_boot`, the runtime library and the UI library on the host side.

### 2.1 Game to outside: 85 symbols

| Group | Count | Symbols | In the split |
|---|---|---|---|
| Runtime functions | 31 | `vm_read8/16/32/64`, `vm_write8/16/32/64`, `ppu_stwcx32`, `ppu_stdcx64`, `lv2_syscall`, `ppu_timebase_now`, `ps3_indirect_call`, `ps3_hle_call`, `ppu_register_function`, `prx_load_module`, `prx_image_load_file`, and `spu_rdch`, `spu_wrch`, `spu_rchcnt`, `spu_stop`, `spu_halt`, `spu_indirect_branch`, `spu_drain_call`, `spu_begin_image`, `spu_register_function`, `spu_workload_register_img`, `spu_ls_watch_slow`, `spu_hook_ring_fetch`, `spu_hook_put_wait`, `spu_hook_inflate_ls` | a jump thunk each in the DLL (3.2) |
| Port functions | 16 | `ds_malloc`, `ds_free`, and the hooks and overrides `des_move_post`, `des_menu_tick`, `des_cam_keep`, `des_cam_rate`, `des_cut_cam`, `des_dof`, `des_hud_canvas`, `des_hud_onscreen`, `des_hud_w2s`, `des_motion_blur`, `des_mouse_look`, `des_noise_rate`, `des_slide_rate`, `des_viewer_rate` | a jump thunk each, filled by name from the exe (3.2) |
| Globals | 14 | `vm_base`, `g_vm_inl_rd_lim`, `g_vm_inl_wr_lim`, `g_vm_inl_rd_hole`, `g_spu_coh_bitmap`, `g_spu_coh_armed_line`, `g_spu_goto_slow`, `g_yz_lockstep_on`, `g_pm_flow_ctx`, `g_spu_ls_watch_n`, `g_spu_ch_diag`, `g_spu_ch_diag_gen`, `g_extra_symtab`, `g_extra_symtab_count` | defined in the DLL; the exe reaches them through pointers (3.3) |
| Thread-locals | 2 | `g_trampoline_fn`, `g_spu_trampoline_fn` | defined in the DLL; the exe caches a per-thread pointer (3.4) |
| C and compiler runtime | 22 | `sqrt`, `sqrtf`, `fma`, `_fdsign`, `ldexp`, `exp2`, `memset`, `getenv`, `atoi`, `strtoul`, `fputc`, `fwrite`, `fflush`, `__stdio_common_vfprintf`, `__acrt_iob_func`, `__security_cookie`, `__security_check_cookie`, `_tls_index`, `__tls_guard`, `__dyn_tls_on_demand_init`, `_fltused` (and `__imp_` forms) | the DLL's own CRT (`/MD`): the same `ucrtbase` and `vcruntime` as the exe |

docs/45 counted 72 (76 with the hooks of the time). The difference is the newer hooks: 16 port functions now instead of 4 to 8.

### 2.2 Outside to game: 33 symbols

| Group | Count | In the split |
|---|---|---|
| Lifted functions the port calls by name (`func_00023750` ... `func_00B5D548`, in `des_options.cpp`, `des_aspect.cpp`, `des_menu_sfx.cpp`, `des_chrins.cpp`) | 27 | forwarders in the exe, resolved by guest address at the load (3.5) |
| `function_table`, `function_table_count` | 2 | handed over by `des_game_init`; the runtime reads them through pointers (3.5) |
| `ps3_env_gen` | 1 | defined `selectany` in both modules; the console's setenv bumps both (3.6) |
| The taskset-policy blob and its lifted module (`g_taskset_policy_bytes`, `g_taskset_policy_size`, `tsp_spu_func_00000A00`) | 3 | stay in the exe: only the runtime's policy oracle uses them |

### 2.3 Defined on both sides

Constants and inline functions (COMDAT, one copy per module, harmless), `ps3_env_gen` (3.6), `ps3_load_prx_modules` (the libsre glue's strong definition; the exe's weak one), and five debug-probe latches of the SPU images (`g_spu_ls_dbg`, `g_spu_ls_probe`, `g_spu_smc_watch`, `g_wws_code_probe`, `g_wws_read_probe`). In the split each module has its own copy of those latches. They are read from the environment once, on first use, so the two copies agree; the two WWS probes' 48-line log caps count per module (read, `spu_context.h`).

No game object has a static constructor apart from `spu_workloads.cpp` (measured: `.CRT$XCU` sections), and none defines a thread-local.

## 3. Design

### 3.1 The DLL's entry: `des_game_init`

The exe loads the DLL as soon as the guest address space is reserved, before anything reads the function table (`runtime/ppu/game_host.c`, called from `boot_main.cpp`). It calls the DLL's one export:

```
int des_game_init(const ps3_host_api* host, ps3_game_api* game);
```

Both structures are in ps3recomp's `include/ps3emu/game_module.h`, with an ABI number (`PS3_GAME_ABI`) and their sizes; each side refuses the other on a mismatch. The host passes `vm_base`, its `ps3_env_gen`, the 31 runtime functions and a lookup of the title's own functions by name. The DLL returns the function table and its count, the addresses of the globals it owns, two functions that return the calling thread's trampoline slots, its own addresses for the runtime functions (3.2), and the two pieces of startup work a single exe did on its own: the SPU workload registration (`des_spu_register_all`) and the libsre glue (`ps3_load_prx_modules`).

The DLL runs no static constructors of its own. The generated `spu_workloads.cpp` registered the SPU images from a constructor; CMake copies it into the build directory with the constructor turned into a plain function, and stops if one is left. The exe calls the registration right after the load.

### 3.2 Calls into the runtime and the port: jump thunks

Each of the 31 runtime functions and 16 port functions is a 6-byte thunk in the DLL, `jmp *slot(%rip)`, under the function's own name (`PS3_GAME_THUNK` in `game_module.h`). The lifted objects call `vm_read32` and `des_move_post` as before; the linker resolves the names to the thunks. `des_game_init` fills the runtime slots from the host's table and the port slots by name from the exe (`des_game_title.c`), and refuses to start (`[des_game] the exe has no ...`) if one is missing. A thunk does not care about the signature, so the 47 functions need no prototypes.

The thunks are module-level assembly in AT&T syntax, and the macro says so with `.att_syntax prefix`. clang-cl parses module-level assembly as Intel syntax: without that line, the first build stopped at `jmpq *ps3_gt_vm_read8(%rip)` with "unknown token in expression" (measured, clang-cl 22.1.8).

One consequence (read): when lifted code takes the address of a runtime function, it gets the thunk's address. The SPU code does that in one place: `SPU_RET` stores `spu_indirect_branch` in `g_spu_trampoline_fn`, and the runtime's `spu_drain_call` compares that pointer with `spu_indirect_branch`. In a split build the comparison accepts the DLL's thunk too (`host_fn_thunks` in `ps3_game_api`). Any new runtime code that compares a function pointer with a runtime function must do the same. No other comparison of that kind exists today (read).

### 3.3 Globals the DLL owns

The lifted code reads 14 globals on its fast paths: the inline memory accessors' `vm_base` and limits, the SPU coherency bitmap and its armed flag, the SPU dispatch flags, and the extra symbol table the libsre glue fills. The DLL defines them (`runtime/ppu/game_module.c`) with the runtime's initial values, so the lifted code reads them RIP-relative, exactly as it reads the exe's globals today.

The host reaches them through pointers. In a split build, `include/ps3emu/game_split.h` is force-included into every host source: the runtime library and the exe's sources. It turns each name into `(*ps3_split_p_<name>)`. Until the load the pointers point at host copies, defined where the single-exe build defines the globals (`PS3_SPLIT_DEFINE`, under `#else` of `#ifndef PS3_GAME_SPLIT`). At the load `game_host.c` copies the host's current values into the DLL's globals, then moves the pointers. No guest or SPU thread runs yet at that point. A declaration of one of these names elsewhere still compiles, as a declaration of its pointer.

`vm_base` itself stays a plain global on both sides: the DLL's copy is set once from the host. The guest space is reserved before the load and never moves. The extra symbol table goes the other way: the libsre glue fills the DLL's copy, and the exe copies it after running the glue.

### 3.4 The two trampoline thread-locals

Windows cannot import a thread-local from another module. The DLL defines `g_trampoline_fn` and `g_spu_trampoline_fn` itself (implicit TLS in a DLL loaded with `LoadLibrary` works since Vista), and the lifted code uses them unchanged. The host reaches the calling thread's slot through a function in the DLL, and caches the slot's address in a thread-local of its own on first use (`ps3_split_ppu_tramp()` in `game_split.h`). Before the load no lifted code has run; a host slot stands in then, and nothing is cached.

docs/45 also proposed moving the two slots into `ppu_context` and `spu_context`. That changes the lifter's templates and every lifted object, so it is left for later (Decisions).

### 3.5 Calls from the port into the lift

The 27 lifted functions the port calls by name live in the DLL. In the exe, `des_game_calls.h` maps each `func_<address>` to a forwarder `des_game_calls::f_<address>` that calls through a pointer. A forwarder is a real function, so calls, function pointers, template arguments (`gauge_fill_rate<func_...>`) and trampoline values all keep working. `des_game_host.cpp` resolves the pointers by guest address from the DLL's function table at the load, and the load fails with `[des_game] func_<address> is not in the game DLL's function table` if one is missing.

The runtime's own lookups (`ps3_indirect_call`, the syscall registration) read `function_table` through the same kind of pointer as the globals.

### 3.6 Environment knobs

`ps3_env_gen` is `selectany`, so each module has one. The console's `setenv` (`ps3_env_bump`) bumps the DLL's copy too, so the lifted code's knob checks and the SPU channel latch see a change.

The exe overrides `__imp_getenv` with the runtime's cached `getenv` (`boot_main.cpp`). That override covers only the exe. The DLL's SPU images and libsre glue call the CRT's `getenv` on their cold paths (knob latches). Both modules share the CRT's environment, which `setenv` updates through `_putenv_s`, so they read the same values (read). Only `GETENV_STATS` misses the DLL's calls.

### 3.7 Files

ps3recomp:
- `include/ps3emu/game_module.h`: the ABI (structures, the runtime function list, the thunk macro).
- `include/ps3emu/game_split.h`: the host's redirects, force-included in a split build.
- `runtime/ppu/game_module.c`: the DLL's half (slots, thunks, globals, thread-locals, `ps3_game_module_init`).
- `runtime/ppu/game_host.c`: the exe's half (load, hand-over, pointer moves, `ps3_game_load_prx_modules`).
- `CMakeLists.txt`: `PS3RECOMP_GAME_SPLIT`; the definitions of the moved globals and thread-locals in `ppu_loader.cpp`, `ps3_knob.c`, `spu_channels.c`, `spu_coherency.c`, `spu_drain.c`, `spu_lockstep.c` and `spu_context.h` are under `#ifndef PS3_GAME_SPLIT`, and so is `spu_dma.h`'s inline coherency test (7.1); `boot_main.cpp` calls the load.

Port:
- `port/CMakeLists.txt`: `DES_SPLIT`, the two targets.
- `port/des_game_module.cpp`: `des_game_init`, the port function slots.
- `port/des_game_title.h`: `DES_GAME_CALLS` (lifted functions the port calls). `DES_GAME_TITLE_FUNCS` (port functions the lift calls) is generated into the build directory since 10-22 (7.1).
- `port/des_game_title.c`, `port/des_game_host.cpp`, `port/des_game_calls.h`: the exe's side of the lists.
- `des_options.cpp`, `des_aspect.cpp`, `des_menu_sfx.cpp`, `des_chrins.cpp`, `des_dev.cpp`: include `des_game_calls.h`; their declarations of `g_trampoline_fn` are under `#ifndef PS3_GAME_SPLIT`.

## 4. Results

The setup: port d408341 and ps3recomp d5dec60 plus this branch, master's lift (`recomp_master`, 39 hooks), clang-cl 22.1.8 and lld-link, both modules `/MD`, no `/Zi`. All runs were in one slot hold, with no build or other game running beside them.

### 4.1 Builds (measured)

| Build | Time (12 jobs) | Output |
|---|---|---|
| Split runtime library (`PS3RECOMP_GAME_SPLIT=ON`, new build directory) | 13 s | `ps3recomp_runtime.lib` |
| Split port (`DES_SPLIT=ON`, new build directory, 343 steps) | about 290 s | `des_game.dll` 291,713,024 bytes, `des_host.exe` 7,743,488 bytes |
| After a change to one runtime source | 2 s for the library, 2 s to relink `des_host.exe` | the DLL is not relinked |
| Default port from this branch (`DES_SPLIT` off, new build directory, 337 steps) | 301 s | `des_boot.exe` 299,419,136 bytes, the same size as master's |

The two split binaries together are 37,376 bytes larger than master's `des_boot.exe`. The first split build stopped in `game_module.c` on the thunk syntax (3.2). With that fixed, the rest of the build took 5 s.

### 4.2 Boots (measured)

The Windows session was locked all night. So every run had `RSX_PRESENT=0` and no input after the title, apart from the pad-file presses that skip the opening cutscene. Knobs: `DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1` (Continue at the title).

| Run | Build | Save | Result |
|---|---|---|---|
| `split1_b1` | split | the pinned regression save | PASS. `[game-host] loaded ...: 94017 functions, 78 ms`. The title data opened, then level m08 (the Tutorial: Boletaria, the first level) loaded. Two 8 s profile windows ran at 30.0 fps (the default rate), with 1% lows of 24.6 and 20.1 fps. No crash lines, and the user's save was unchanged. |
| `ab1`, `ab2` | split, 6 boots | the deep Palace save | All six loaded m02 (Boletarian Palace 1-1) and ran their profile windows (4.3). |
| `dflt_b1` | default single exe from this branch | the pinned regression save | PASS. The title data opened and level m08 loaded. Two profile windows ran at 30.0 fps, with 1% lows of 24.5 and 22.3 fps. No crash lines, and the user's save was unchanged. |

### 4.3 Frame rate (measured)

The method is the perf lane's A/B (`perfab.sh`, run from this lane's copy). A is master's `des_boot.exe`, built the same way from the same commits (no `/Zi`, the master lift). B is the split build. Every boot:
- loads the deep Palace save (m02, Boletarian Palace 1-1) with `DES_FPS=60 VBLANK_MHZ=240000 PS3_PRIORITY=normal PS3_AFFINITY=all`, and nothing is pressed after the title;
- waits 40 s after the level starts loading, then runs two 8 s profile windows, each followed by 10 s with nothing armed;
- gives two rates per window: the profile session's own (profile windows), and the rate between sessions from `FRAME_LOG` (between windows), with nothing armed.

Rounds alternate the order, and a quiet-PC check runs before each boot. Neither build was capped. `DES_FPS=60` is the game's 60 fps mode, which presents at the vblank rate, here 240 Hz, and this spot runs at about 113 fps.

There were two batches. In `ab1`, A was master. `ab2` swapped the labels, so each build went first in three of the six rounds. The PC was quiet for `ab1`. During `ab2` the quiet check had to wait before most boots, and master's own three boots spread by 8.5%.

Split minus master, in % of master, from each round's boot means:

| Batch, round (first) | Profile windows | Between windows |
|---|---|---|
| `ab1` 1 (master first) | +0.44% | -3.08% |
| `ab1` 2 (split first) | +0.13% | -0.42% |
| `ab1` 3 (master first) | -1.29% | +0.45% |
| `ab2` 1 (split first) | -6.34% | -8.58% |
| `ab2` 2 (master first) | +0.47% | -3.10% |
| `ab2` 3 (split first) | -0.55% | +1.06% |
| Mean (median) | -1.19% (-0.21%) | -2.28% (-1.75%) |
| Split faster in | 3 of 6 | 2 of 6 |
| Paired t on the six deltas | p = 0.32 | p = 0.18 |
| 95% interval of the mean | -3.9% to +1.6% | -6.0% to +1.5% |
| Boot means, master / split | 112.02 / 110.64 fps | 117.10 / 114.38 fps |

No difference is significant. `ab1` alone, on the quiet PC, gives -0.57% and -1.04% (medians; p = 0.70 and 0.44). The combined means lean against the split by 1-2%, mostly because of one noisy round (`ab2` round 1, where master's boot was the fastest of its batch by 6%). Six rounds at this noise cannot resolve a 1-2% difference either way (6.1).

## 5. The default build does not change

- `des_boot`'s part of `port/CMakeLists.txt` is untouched. The split is one block after it, under `option(DES_SPLIT ... OFF)`.
- ps3recomp's `PS3RECOMP_GAME_SPLIT` is off by default. When on, it adds its definition and the forced include to the runtime library only. The two new runtime sources are in `runtime/ppu/`, which the library does not compile (the title's exe does). They compile to nothing without `PS3_GAME_SPLIT` or `PS3_GAME_MODULE`.
- Every changed source preprocesses to the same text as on master in the default configuration (measured: clang-cl `/EP` with `des_boot`'s and the runtime library's flags, this branch against master, with the tree paths and blank lines normalised). All 13 checked sources are identical: `des_aspect.cpp`, `des_chrins.cpp`, `des_menu_sfx.cpp`, `des_options.cpp`, `ps3_knob.c`, `ppu_loader.cpp`, `boot_main.cpp`, `spu_channels.c`, `spu_coherency.c`, `spu_drain.c` and `spu_lockstep.c`, plus `spu_workload.c` and `spurs_policy.c`, which include the changed `spu_context.h`.
- The lift directory is used as it is. In the split build directory, every lifted source (14 PPU chunks, 12 SPU images and 2 SPU jobs, the lifted libsre and its glue) compiles for `des_game` with exactly the command line it gets for `des_boot` (measured, `ninja -t commands`, apart from the object and dependency file names): 30 of 30 identical.
- The objects are identical too. They were compared byte for byte, apart from the time stamp in the COFF header, which clang-cl sets to the build time. The DLL's 14 PPU chunk objects match master's `des_boot` objects from a master build of the same commits. Its SPU images, SPU jobs, lifted libsre and glue match the same sources compiled with master's ps3recomp include and runtime trees. 30 of 30 match (measured). So the lifted code is byte-identical in the DLL. It is also byte-identical in the default build, which compiles the same sources with the same command lines and the same headers.
- The default build from this branch (`DES_SPLIT` off, no `/Zi`) builds and runs (measured). It built in 301 s, and its `des_boot.exe` is 299,419,136 bytes, the same size as master's. Its 326 compile commands for `des_boot` match those of a master build of the same commits (`ninja -t commands`, tree paths normalised), except the commit hash that three UI sources get as `DES_VERSION`. Its link flags and object list match too. Its 30 lifted objects are identical to the DLL's, and so to master's. It booted from the pinned save into the Tutorial just as the split build did (`dflt_b1`, 4.2).

## 6. What is left

### 6.1 For milestone 1

- **Review and merge.** ps3recomp's commit goes first, then the port's (Decision 6). Both are opt-in.
- **A live session.** Every run tonight was at the lock screen: nothing was presented to a window (`RSX_PRESENT=0`), and no menu, pad or keyboard input came after the title. Input, the UI and presenting are in the exe in both builds. Still, a play session on an unlocked desktop with `DES_EXE=<build>/des_host.exe` should confirm them.
- **A longer A/B on a quiet PC.** Tonight's six rounds cannot resolve a 1-2% difference (4.3). More rounds at the Palace spot would settle it. So would the pinned-save spot, which runs about 320 fps at `VBLANK_MHZ=960000` (the perf lane's A/A), so a per-call cost weighs more there. If a cost shows, the first suspects are the thunked runtime calls on hot paths (the SPU channel calls, `ppu_stwcx32`, `ps3_indirect_call`) and the host's reads of the coherency bitmap through a pointer (inferred: not profiled).
- **Scope.** The split is Windows only: the thunks are PE/COFF x64 assembly, and `game_host.c` uses `LoadLibrary`. It is also Demon's Souls only for now: ps3recomp's part (the ABI, the host and module halves) is generic, but `des_game_module.cpp` and the two lists are the port's, and another title needs its own.

### 6.2 For milestone 2 (docs/45 step 3: the DLL built by clang and lld alone)

- **No CRT in the DLL.** Replace the 22 C and compiler runtime symbols: `sqrt` inline (`-fno-math-errno`), `fma`, `ldexp`, `exp2`, `_fdsign`, `memset` and `_fltused` in a small support file, the SPU and glue log lines and knob reads through host functions, `-fno-stack-protector`.
- **Thread-locals without the CRT.** The lifted code reaches the trampolines as C++ thread-locals, with `__tls_guard` and `__dyn_tls_on_demand_init` guard calls, and the DLL's TLS directory comes from the CRT. A DLL linked `/nodefaultlib` must define `_tls_used` and `_tls_index` itself, or the trampolines move into the context structures (then the DLL has no thread-locals at all).
- **Header split.** The exe still compiles against the lift's generated `ppu_recomp.h` (for `ppu_context` and the accessors). A host built in CI without a lift needs the fixed runtime part of that header shipped with ps3recomp and the per-game part generated.
- **Link.** `lld-link /dll /noentry /nodefaultlib`, then an A/B against this build.

## 7. Rebased onto the masters of 2026-10-22

Setup's Q2 is decided as A (docs/81): CI builds the host exe, and the player's PC compiles only the game part. That makes this split setup's first step. So both branches were rebased onto ps3recomp fork/master 601fb36 and port master af06c0f, and checked again.

### 7.1 The rebase

- Three conflicts:
  - `spu_drain.c`: master's new `g_spu_bi_switch` sits beside `g_spu_goto_slow`;
  - `port/CMakeLists.txt`: master's new Linux, `DES_UI` and `DES_DEV` parts;
  - `des_options.cpp`: the warp moved to `des_dev.cpp`, which now has the guarded `g_trampoline_fn` declaration and includes `des_game_calls.h`.
- The boundary, measured again (`llvm-nm` on the 30 lifted objects of a master build of the same commits and the same lift): 106 symbols from outside the lift, against 85 on 10-19.

| Group | 10-19 | 10-22 | New since 10-19 |
|---|---|---|---|
| Runtime functions | 31 | 35 | `spu_hook_ring_post` and `spu_hook_state_diff` (image 2's native loops), `spu_registering_image` and `spu_bi_switch_verify` (the SPU jump-table gotos): a thunk each, at the end of `PS3_GAME_HOST_FUNCS`. `PS3_GAME_ABI` stays 1, since no split build has shipped. |
| Port functions | 16 | 33 | 17 hooks merged since: `des_body_xform`, `des_cam_ctor`, `des_cam_own`, `des_cam_step`, `des_dev_hook`, `des_fade_rate`, `des_fx_state`, `des_ghost_rate`, `des_hud_ctx`, `des_net_grade_poll`, `des_net_show`, `des_net_sync`, `des_phys_kf`, `des_phys_rate`, `des_qwc_local`, `des_rag_pose_a`, `des_rag_pose_b` |
| Globals | 14 | 16 | `g_spu_bi_switch` and `g_spu_task_regs`, which `spu_bi_switch_on` reads inline in the lifted SPU images. The DLL owns them, like the other dispatch flags (3.3). |
| Thread-locals | 2 | 2 | none |
| C and compiler runtime | 22 | 20 | none; `memset` is no longer used |

- The port now calls 29 lifted functions by name, against 27: the two more are in `des_dev.cpp` (`func_004418B8`, `func_008FB4F0`).

Two changes keep the lists from falling behind again:
- **`DES_GAME_TITLE_FUNCS` is generated** (Decision 2, the first half). CMake reads the overrides in `scripts/lift_ppu.json` and the hooks in `insn_hooks.txt`, the lift's own inputs, into `split_gen/des_game_title_funcs.h` in the build directory (33 names). `DES_GAME_CALLS` stays by hand; a missing entry still stops the exe's link with its name.
- **`des_host` follows `des_boot`.** It takes `des_boot`'s sources, include folders, definitions, options and libraries from the target itself, minus the lifted sources, so the section stays last in the file. The 10-19 list was a copy, and by 10-22 it lacked `des_dev.cpp`, the version resource and `des_ui_stub.cpp`. Configure stops if a lifted source would go into `des_host`.

One host fix: the DMA fast path (ps3recomp, 10-21) added an inline coherency test to `spu_dma.h` that names the bitmap and the armed flag directly. The host's link stopped on both names. Under `PS3_GAME_SPLIT`, the test now reads them through the pointers, as `ppu_loader.cpp`'s test does (3.3). The default build keeps the old text.

### 7.2 Checks (measured)

| Check | Result |
|---|---|
| A default build preprocesses as master's. Checked: every source of ps3recomp's runtime library, and every source of `des_boot` except `_deps` and lift chunks 001-013 (chunk 000 stands for the 14, which share their headers). Method: clang-cl `/E` with the master build's own commands (`ninja -t commands`), once on master's trees and once on the branches', with the tree paths, line markers and blank lines normalised. | 170 of 170 and 319 of 319 the same |
| The DLL's lifted sources get `des_boot`'s command lines (`ninja -t commands`, one build directory, apart from the object and dependency file names) | 30 of 30 |
| Builds, 12 jobs | the split runtime library 15 s; the two port targets in a new build directory about 5 minutes |
| Sizes | `des_host.exe` 8,182,272 bytes and `des_game.dll` 289,020,416 bytes: together 38,912 bytes more than master's `des_boot.exe` (297,163,776) |
| Boot and screenshots: `regress.sh` from the pinned save, at 30 and 240 fps, the session unlocked (presenting) | `[game-host] loaded ...: 94017 functions, 79 ms`. All four shots ok at both rates, with the same differences as master's own runs at those rates. The user's save is unchanged. |

### 7.3 Frame rate, in plain windows (measured)

The method is the perf lane's A/B (`perfab.sh`), with the verdict from the plain windows (docs/83). A is master's `des_boot.exe` from the merged masters' build, and B is this split build, with the same lift, compiler and flags. Each side ran 8 boots, alternating A B, B A. Every boot:
- runs uncapped: `RSX_VSYNC=0 DES_VSYNC=0 DES_FPS=unlocked VBLANK_MHZ=960000`, normal priority on all CPUs;
- presents to a window (the session was unlocked), and nothing is pressed after the title;
- waits 40 s after the level starts loading, then runs two 8 s profile windows, each followed by 10 s with nothing armed (the plain window).

The PC was not quiet. The user's Vite dev server used about a core all morning, so the quiet check's limit for processes outside the batch was raised from 3% to 20%. Both sides ran beside that same load, in alternating rounds. All 32 boots were valid, and the user's save was unchanged after every boot.

| Spot | Window | A, master (boot mean) | B, split (boot mean) | B vs A | Rounds B faster | Paired t |
|---|---|---|---|---|---|---|
| The pinned save, Tutorial (m08) | plain | 425.71 fps | 425.98 fps | +0.06% (median +0.35%) | 4 of 8 | p = 0.97 |
| | profile | 403.68 fps | 403.59 fps | -0.02% (median -0.58%) | 3 of 8 | p = 0.99 |
| The deep Palace save (m02, Boletarian Palace 1-1) | plain | 147.03 fps | 150.05 fps | +2.05% (median +0.98%) | 5 of 8 | p = 0.12 |
| | profile | 138.09 fps | 139.31 fps | +0.88% (median +1.63%) | 6 of 8 | p = 0.41 |

At the Tutorial spot each round's delta swings by 4 to 9% either way, so the 95% interval of the mean is about -3.7% to +4.1%. That batch cannot rule out a 1-2% cost. The Palace batch can: its 95% interval of the mean (plain windows, the round deltas) is -0.7% to +4.9%, so a cost above 0.7% is ruled out there. Neither batch finds a difference. The split leans faster at the Palace, but not significantly; a faster split would need a cause, and none is known. The 10-19 result (4.3: -1.2% and -2.3%, not significant, in profile windows) is in line with this.

### 7.4 What is left

- **Review and merge**, ps3recomp first (Decision 6).
- **A play session** with `DES_EXE=<build>/des_host.exe`: the menus, the pad, the keyboard and the mouse. The screenshot route walks with the `PAD_STICK` knob, not with real input.
- **Setup's next step**: the freestanding game DLL (6.2, docs/52 section 7.8). The host then builds in the Visual C++ runtime and the C++ library and uses the UCRT that Windows includes (docs/81, decided 10-22).

## Decisions

These need the user's call. The milestone works without any of them.

1. **Trampolines.** Keep them as DLL thread-locals with a per-thread pointer in the exe (today; no lifter change), or move them into `ppu_context` and `spu_context` (a lifter and runtime change that touches every lifted object, but removes the DLL's only thread-locals before milestone 2).
2. **The two lists.** Half done on 10-22: `DES_GAME_TITLE_FUNCS` is generated from `insn_hooks.txt` and the override list (7.1). `DES_GAME_CALLS` (29 lifted functions) is still kept by hand in `port/des_game_title.h`: a new `func_` call in the port must be added there, and a missing entry stops the exe's link with its name. A scan of the port's sources could generate it too.
3. **Where the DLL lives.** Beside the exe, or `PS3_GAME_MODULE`. docs/45 wants `build\<lift key>\des_game.dll` with a manifest; that comes with the setup screen.
4. **Diagnostics.** A crash in the DLL prints an address relative to the DLL (`[CRASH] module=` gives its base); symbolize with `des_game.map`, not the exe's map. `GETENV_STATS` does not see the DLL's `getenv` calls. The SPU debug-probe latches exist once per module.
5. **The thunk rule.** Runtime code that compares a function pointer with a runtime function must also accept the DLL's thunk address (3.2). Keep this rule, or have the lifter call the runtime through its own table and never take addresses.
6. **PR order.** ps3recomp first (the two headers, the two runtime sources, the guarded definitions, the option), then the port. Both are opt-in, so either can merge alone.
