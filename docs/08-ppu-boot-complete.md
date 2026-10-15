# 08 — PPU Boot: Complete Reference (how Demon's Souls boots to its main loop)

Status (2026-06-29): the PPU static recompilation boots Demon's Souls (BLUS30443) from the
user's own disc EBOOT all the way into its **60fps main loop**, with three Dantelion2 runtime
threads spawned. The only remaining gate is **SPU/SPURS execution** (see `09-spu-spurs.md`).

This is the durable reference for everything learned getting the PPU side to boot. `07-m2-boot-debug.md`
is the chronological lab notebook; THIS file is the organized "what's true and why" reference.

---

## 1. The boot pipeline (what runs, in order)

```
ppu_run(entry_opd) [runtime sets r1=stack, r2=TOC from entry OPD]
  -> CRT / _start (func_000124C0 -> func_000125F0 ...) self-loads its TOC (r2 = *(own_opd+4))
  -> __do_global_ctors_aux (func_016B84B0) walks .init_array, runs C++ global ctors in order
       - registry/type-system init (func_00A35780 & friends): the big static-init machinery
  -> sysmodule load (cellSysmoduleLoadModule / cellSysmoduleInitialize)
  -> RSX backend init -> "[rsx] backend init OK -- window open"
  -> Dantelion2 runtime-service threads created (sys_ppu_thread_create / _sys_ppu_thread_create):
       - (Dantelion2)PPURuntimeService  entry OPD 0x0196FAF8 -> func_00C3A878  (prio 1001)
       - (Dantelion2)SPURuntimeService  entry OPD 0x01970970 -> func_00C5DAB0  (prio 200)
       - HighGraphics                   entry OPD 0x0196FAF8 -> func_00C3A878  (runs, returns)
  -> sys cache mount (cellSysCacheMount), system params (cellSysutilGetSystemParamInt/String)
  -> MAIN LOOP: per frame  sys_timer_usleep(16666us) + cellSysutilCheckCallback
       - PPURuntimeService (func_00C3A878 -> func_00C3A358) pumps this loop, waiting on a flag
       - SPURuntimeService (func_00C5DAB0) blocks on sys_event_queue_receive(q=1) [SPU gate]
```

Key fixed addresses (main module): **TOC base r2 = 0x019B5040**. Heap manager singleton X at
`0x01B92CF8`. DL system heap descriptor `0x01A1F850`. ds_heap region `0x60000000-0xC0000000`.

---

## 2. Root-cause fixes (the ones that mattered), each cleared a whole symptom class

| # | Fix (commit) | Root cause | Symptom it explained |
|---|---|---|---|
| 1 | **Rotate-by-zero UB in lifter** (ps3recomp 58fd9d8) | `ppc_rotl64`/`rlwinm`/`rlwimi` computed `v>>64`/`rs>>32` for shift==0 — C UB. clang -O2 miscompiled `rldicl …,0,32` / `rlwinm …,0,…` (32-bit truncate & mask/move — ubiquitous) in optimizer-hostile functions. | THE root of the entire C++ static-init "construction cascade" (#4/#5/#6, NULL vtables, register-then-wipe). Proven: `ppc_rldicl(0x01B91198,0,32)` returned `0x00000002`. Fix: guard `sh/n==0`. |
| 2 | **SYS_MUTEX_MAX/SYS_COND_MAX 256→8192** (788c645) | Dantelion2 creates >256 mutexes during init; `sys_mutex_create` returned `CELL_EAGAIN`, the game's lifted error-switch was an unlifted jump table → corruption/hang. | Spin in `func_00AE60A0` after RSX init. |
| 3 | **event-flag syscall numbers** (5a41644) | The whole `sys_event_flag_*` block was numbered 139–146, colliding with `sys_timer_usleep/sleep` (141/142) & `sys_event_port_connect_ipc` (140). The per-frame `usleep(16666)` was dispatched to `event_flag_wait(16666,0x5B)` → `CELL_ESRCH` → 1.9M/s spin. Correct: create=82 destroy=83 wait=85 trywait=86 set=87 clear=118 cancel=132 get=139 (RPCS3 lv2.cpp). | The "cellSysutilCheckCallback wait loop" spin. |
| 4 | **Disable force-build-X workaround** (main bf55cc0) | The apply_boot_patches force-build of singleton X (a band-aid for the UB-induced NULL vtable) became unnecessary AND harmful after the UB fix: it built X on a context copy and corrupted a Meyers singleton. | The `*(0x78)` NULL-object poll. |
| 5 | **Propagate return in ctx-handler thread wrappers** (42bbd07) | The ctx-handler HLE path (`ppu_hle.cpp`) calls `fn(ctx)` without assigning `gpr[3]` from the return; `hle_ppu_thread_create` didn't set it, so `_sys_ppu_thread_create` (NID 0x24A1EA07) returned its 1st arg (a pointer) not CELL_OK. Game read that as an error → assert path (`func_00A9DD58`) clobbered the TOC (r2=0) → garbage singleton (`this=0xB8`) → the `*(0x78)` spin. | The deep `0xB8`/`0x78` TOC-corruption cascade; also blocked the 2nd+3rd worker threads. |
| 6 | **cellSysutil guest-pointer translation** (af71a5d) | Several `libs/system` HLE handlers with C signatures (`s32*`, `char*`) are dispatched with RAW guest addresses (REG_FUNC stores the bare fn ptr; HLE passes `gpr[N]` verbatim). They deref'd guest addresses as host pointers → segfault (VM is not identity-mapped; `vm_base != 0`). Fixed `cellSysutilGetSystemParamInt/String`, `cellSysCacheMount`, `cellDiscGameGetBootDiscInfo` to translate via `vm_base` + big-endian store. | Segfaults in system-service init → reaching the main loop. |

Pragmatic shims still in `scripts/apply_boot_patches.py` (re-applied after every re-lift):
- **Patch 1**: `func_00AC8078` (DL segregated allocator) with a NULL heap descriptor (r3==0) →
  route the allocation to `ds_heap` instead of spinning in the bin-doubling loop. Size from r4.
  (Patch 2, force-build X, is disabled — see fix #4.)

---

## 3. Diagnostic techniques that worked (reuse these)

- **`ctx->cia` stamping**: the lifter stamps `ctx->cia = <guest pc>` before every bl/bctrl/bctr.
  Any spin/crash reports the exact guest instruction. Indispensable.
- **Host→guest address resolution via the linker /MAP**: `port/build/des_boot.map` lists each
  `func_XXXXXXXX` with `Rva+Base` (preferred base `0x140000000`). Resolve a host rva to
  `func+0xNN` by `bisect` over the sorted symbol RVAs. Script pattern is in 07/this repo's history.
- **`ds_dump_bt(tag)`** (ppu_loader.cpp): `CaptureStackBackTrace` printed as host rvas → resolve
  via the MAP. Note: trampoline tail-calls (`g_trampoline_fn` + `DRAIN_TRAMPOLINE`) make the host
  stack UNRELIABLE for tail-called frames; cross-check with `ctx->cia`/`ctx->lr` and known call sites.
- **HOTREAD / HOTREAD8 / WWATCH** (ppu_loader.cpp): detect a thread spinning on the same address
  (hot poll) or watch writes to an address. HOTREAD8 now also prints the guest `cia` of the poller.
- **WATCHDOG** (boot_main.cpp): periodic per-thread sample; prints the boot thread's rip + a stack.
- **`[CRASH]` handler** (boot_main.cpp): on a host access violation prints code, fault address,
  guest `cia/ctr/lr/r3`, the last HLE NID+name, and a host backtrace.
- **YDKJ_* env gates**: `YDKJ_HLETRACE` (all HLE calls), `YDKJ_ICTRACE` (indirect-call targets),
  `YDKJ_SYNCTRACE` (EVF/EVQ/mutex sync), `YDKJ_WWATCH=<addr>`, `YDKJ_HOTREAD*`.
- **RPCS3 differential**: run the same EBOOT under RPCS3 (interpreter decoder for breakpoints) and
  diff state at the first divergence. RPCS3 source reference clone (github.com/RPCS3/rpcs3) at `<rpcs3-src>`
  (sparse: `Emu/Cell/Modules`, `Emu/Cell/lv2`) — cross-ref its lv2.cpp/Modules for syscall numbers,
  NIDs, struct layouts, error codes. **Always reimplement clean (RPCS3 is GPLv2; this port is clean-room).**
- **Probe discipline**: probes go into the gitignored `work/recomp/*.cpp` (wiped on re-lift). When
  inserting via python heredocs, the file is CRLF — write a literal backslash-n as `chr(92)+'n'`,
  not `"\\n"` (which became a real newline twice and broke the build). `extern "C"` can't appear
  inside a function body — declare it at file scope above the function.

---

## 4. Architecture facts & gotchas (PPC64 ELFv1 + this runtime)

- **TOC discipline (r2)** is the subtle one. r2 = the module's TOC (main module = `0x019B5040`),
  used for all `*(r2 ± off)` global access. Caller sets r2 from the callee's OPD `{code, toc}` for
  cross-module calls and restores its own r2 after (`ld r2, 0x28(r1)`). Intra-module calls keep r2.
  HLE calls: `ppu_hle.cpp` saves the caller's r2 to `sp+0x28` so the caller's restore works; the
  PRX-export path sets `gpr[2]=callee toc`. A function that makes a cross-module call but whose
  lifted body lacks the r2-restore (or whose HLE wrapper corrupts it — fix #5) breaks every
  `r2`-relative load downstream → garbage globals. If you ever see a global read as a tiny/garbage
  value, **check r2 first**.
- **OPD (Official Procedure Descriptor)**: function pointers are OPD addresses, not code addresses.
  OPD = `{u32 code, u32 toc, u32 env}`. `.init_array` entries, vtable slots, C++ function pointers
  are all OPDs. `functions.json` records each function's `opd`.
- **Flat 4GB VM**: `vm_base` (host) + guest addr; commit-on-demand via a VEH. **Not identity-mapped**
  (`vm_base != 0`) — HLE handlers must translate guest pointers via `vm_to_host(addr)` / `vm_base+addr`
  and read/write **big-endian** (PS3 is BE). C-signature HLE handlers receive RAW guest addresses in
  pointer params (see fix #6).
- **HLE dispatch tiers** (`ppu_hle.cpp` ps3_hle_call): (1) PRX export (`prx_resolve_export`), (2) ctx
  handlers `ps3_hle_register_ctx(nid, fn(ppu_context*))` — fn reads gpr & sets gpr[3] itself; (3)
  generic `ps3_hle_register(nid, fn)` and the module registry (`REG_FUNC`, NID = SHA-1-based hash of
  the name) — called `fn(gpr[3],…,gpr[10])`, return → gpr[3]. Unresolved NID → returns 0 (CELL_OK-ish).
- **Lifted jump tables (switch) are NOT reconstructed**: a computed `bctr` through a table emits an
  unresolved `ps3_indirect_call` + `TODO: .word` offsets. Seen in `func_00A8CE10` (mutex-error
  switch), `func_00C3A6E8`, `func_01672D98`, `func_00FFFCF0`. They no-op; usually the surrounding
  code is an error/rare path, but it's a real lifter gap worth a general fix eventually.
- **`apply_boot_patches.py`** must run after EVERY re-lift (chunks are regenerated & gitignored).
  `work/recomp/` is ~1.5GB of lifted C, reproducible from EBOOT; re-lift recipe is in §6 below.
- **Build quirk**: same-second edits can be missed by the build's mtime check — if a probe doesn't
  fire, force-touch the chunk or rebuild verbose to confirm it recompiled. Build via the PowerShell
  tool (`cmd /c "scripts\vsenv.bat cmake --build port\build -j 14"`); the Bash tool mangles the
  backslash paths in `cmd /c`.

---

## 5. Things that were RED HERRINGS / corrected beliefs (don't re-chase)

- The "C++ construction cascade / use-before-construction" (NULL vtables, the `0x018736B0` garbage,
  the register-then-wipe runaway copy) was **not** a real ordering/reentrancy problem — it was the
  rotate-by-zero UB (fix #1) miscompiling guard/index math. Once UB was fixed, the cascade vanished.
- The **DL-heap backing** hypothesis (back `0x01A1F850`) was disproven: the spinning allocator got
  `desc=0` (a NULL object heap), not the system heap. (See the `dl-heap` branch docs; that branch is
  a documented dead-end-for-now.)
- The `*(0x78)` spin was **not** the force-build-X patch's fault per se — it was the thread-create
  return-value bug (fix #5) corrupting the TOC. The usleep syscall bug (#3) had been masking it.

---

## 6. Reproduce the current (boots-to-main-loop) state

Both repos on `master`. From the project root:
```
# 1. lift (override approach; ~3-5 min)
python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
  --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
  --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free" --output work/recomp -j 16
# 2. re-inject the boot shim (patch 1 only)
python scripts/apply_boot_patches.py
# 3. build runtime + port  (PowerShell tool; ~5 min for the chunk recompile)
cmd /c "scripts\vsenv.bat cmake --build ps3recomp\build -j 14"
cmd /c "scripts\vsenv.bat cmake --build port\build -j 14"
# 4. run (PS3_VFS_ROOT points at the user's extracted disc)
PS3_VFS_ROOT="<disc root holding PS3_GAME>" ./port/build/des_boot.exe work/game/EBOOT.ELF
```
Expected: no crash, no spin; threads created; repeating `timer_usleep(16666)` + `cellSysutilCheckCallback`;
`[SPU] initialize(nspu=6)`; `event_queue_receive(q=1)` (the SPU gate).

Commits this milestone: ps3recomp 58fd9d8, 788c645, 5a41644, dcf7ed5, 42bbd07, af71a5d;
main bf55cc0, 58ffccb, c8a322a, a39a19e.
