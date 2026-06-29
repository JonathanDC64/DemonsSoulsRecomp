# 07 — M2 Boot Debugging Log

Iterative log of getting the recompiled `des_boot.exe` past each boot blocker.
Each blocker = the live "to-do list" the static-recomp boot produces.

Run recipe:
```
set PS3_VFS_ROOT=D:\Users\jonat\Documents\roms\ps3\Demon's Souls (USA)
port\build\des_boot.exe work\game\EBOOT.ELF
```
Diagnostic env toggles (in ps3recomp runtime): `YDKJ_SCTRACE=1` (all lv2 syscalls),
`YDKJ_HLETRACE=1` (all resolved NID/HLE calls), `YDKJ_WWATCH=0xADDR` / `YDKJ_RWATCH=0xADDR`
(watch guest memory writes/reads near an address).

---

## Blocker #1 — `bad_alloc` during early boot — ✅ FIXED (2026-06-28)

**Fix:** override the game's `malloc` (`func_0167AC88`) and `free` (`func_0167AC58`) with a
clean host-backed guest allocator (`port/des_heap.cpp`, region `0x40000000+`), via the new
lifter `--override-funcs`. The DL system heap can't be backed in our env (no `liblv2`
`prx_start`), so we bypass it entirely — and it's plug-and-play (our code, no firmware).
Re-lift: `--override-funcs 0x167AC88=ds_malloc,0x167AC58=ds_free`.

**Result:** static-init `bad_alloc` gone. Boot now passes C++ static init, runs the entry,
loads system modules (`cellSysmodule`: RTC, L10N, SYNC2, SPURS, RESC, NET), spawns worker
threads, and runs indefinitely without crashing. Huge cascade unblocked.

## Blocker #2 — module-load init — ✅ PASSED; now in main()

The mutex-create loop was finite (257 mutexes for a static object array) and completed.
**The game then reached `main()` (`func_016B84B0`)** and runs its own init. Big milestone.

## Blocker #3 — main() data-structure build loop — ✅ CLEARED by /O2

Building chunk 005 (the hot loop: `func_00AE1408`+`func_00ADCB28`) with `/O2` cleared it:
HOTMAP went from hundreds of dumps to **zero** — the mass-memory loop now completes fast.
Also dropped the Debug `/RTC1` runtime-checks (big overhead, /O2-incompatible) by configuring
with an empty `CMAKE_BUILD_TYPE` + explicit `/Od /Zi`. Per-chunk `/O2` capability added to
`port/CMakeLists.txt` (`DES_O2_CHUNKS`). So blocker #3 *was* partly unoptimized-build speed.

## Blocker #4 (CURRENT) — main thread busy-waits (thread-coordination spin)

Past the scan, the main thread pins at a **fixed PC** (host rva `0x8986CDB`, same at 8s & 15s)
with **no syscalls, no HLE calls, no heavy memory reads** (no HOTMAP/HOTREAD). It's a tight
busy-wait on a condition another thread should set — and the watchdog shows many worker
threads **parked in ntdll (OS waits)**. So: main spins waiting on a worker that is itself
blocked → effective deadlock. No SPU thread-group activity yet, but SPURS is loaded and the
SPU crux (we don't execute SPU code) is the prime suspect.

### Narrowed (YDKJ_SYNCTRACE)
Instrumented `sys_event_queue_receive`/`flag_wait`/`flag_set`, `sys_cond_wait`/`signal`/`signal_all`,
`sys_semaphore_wait`/`post` with tid-tagged logging → **ZERO events before the spin.** So the
deadlock is **NOT** via event-queue / event-flag / cond / semaphore. That leaves:
- **lwmutex / lwcond** (the lightweight primitives the game hammers — `sys_lwmutex_lock`/`unlock`
  = 0x1573DC3F/0x1BC200F4 dominate the trace), or
- a **guest-level busy-poll** in lifted code (the main thread spins at a fixed guest PC making
  no syscalls/HLE; if it polls via `vm_read16` there's no detector for that width).

### ROOT CAUSE LOCATED — free-list walk runs off the end of our allocator
Tooling: `ppu_context` has no guest-PC field, and `/O2` chunks don't symbolize via
llvm-symbolizer. Solution (reusable): added a **linker map** (`/MAP:des_boot.map` in
`port/CMakeLists.txt`) → resolve any host RVA to `func_<addr>` by finding the symbol just
below it (preferred base `0x140000000`). (lldb works too but `backtrace all` is too slow on
the 251 MB PDB; `image lookup -a` is the fast path if needed.)

Spin RVA `0x8986CDB` → **`func_00AC8078` +0x26B**. Reading it: a **free-block list walk** —
`next = *(node+0x4); node = next - 8`, looped, comparing block sizes (best/first-fit search in
a memory pool; reads region fields at +0x1C/+0x20/+0x24/+0x28). The chain **never terminates**:
the HOTMAP addresses climb from `0x600F4000` upward — i.e. just **past our allocator region
`0x40000000–0x60000000`** — into uninitialized demand-paged memory, following bogus ever-
increasing "next" pointers forever.

**So our naive bump allocator is the culprit.** The game runs its own memory-pool/free-list
management on top of malloc'd memory; our `ds_malloc` (no chunk headers, no freelist, no
coalescing, leaks on free) doesn't satisfy the layout/semantics the pool walker expects, so
its free-list runs off the end of our region and loops.

### Next techniques (resume here)
1. **Replace the bump allocator with a real one.** `ds_malloc`/`ds_free` need proper free-list
   management with the chunk metadata the game's allocator-walkers expect — OR, cleaner: stop
   overriding the game's allocator and instead **back its DL heap properly** (the original #1
   approach) so its own (correct) free-list management runs. Reconsider override vs. back-the-heap
   now that we know the game does sophisticated pool management on malloc'd memory.
2. Confirm by reading `func_00AE60A0`/`func_00451F68` (callers): what pool is being built/walked,
   and what layout it assumes for blocks returned by malloc.
3. (superseded) Get the main thread's guest PC: add a global `ppu_context* g_main_ctx`
   (set in `ppu_run`), dump `g_main_ctx->pc` from the watchdog (chunk 005 is `/O2` so the host
   RVA `0x8986CDB` won't symbolize; the guest PC maps trivially to `func_<addr>`). Read that
   function → find the flag it polls + who should set it.
2. **Instrument lwmutex/lwcond** (the ctx-handlers) the same way — see if a worker is parked in
   `sys_lwcond_wait` for a signal that never comes (missed-wakeup HLE bug) or a lwmutex it can't
   acquire (lock held by the spinning main thread = classic deadlock).
3. If it resolves to waiting on SPU work → SPU PPU-fallbacks (`docs/SPU_FALLBACK.md`).

## (history) Blocker #3 — main() init: slow/large data-structure build loop

After `main()`, a 100%-CPU loop with no syscalls. Located via `YDKJ_HOTMAP` + host backtrace:
```
main(func_016B84B0) → func_004553B8 → func_00451F68 → func_00AE60A0
   → func_00AE1408  (loop)  → func_00ADCB28 (16-byte block copy) + vm reads/writes
```
- `func_00ADCB28` = a 16-byte block copy; `func_00AE1408` is the driver: it allocs 0x38-byte
  nodes (`func_00A941F8`), does per-iteration vtable calls, and reads/writes an ever-growing
  region at `0x60000000+` (HOTMAP addresses climb 0x600F4000 → 0x9A7AE000+ and keep going,
  past the ~905 MB DL-pool size; rate slows 29→2 MB/s over time).
- Signature = **building a large data structure (O(n²)-ish), pathologically slow under the
  `/Od` debug build** (a function call + trampoline drain per 16-byte copy). Possibly also a
  data/allocator-layout mismatch making the structure larger/corrupt than on hardware.

### Next techniques
1. **Rule out `/Od` slowness:** build the hot chunk(s) (chunk 005 has these fns) with `/O2`
   (full `/O2` over 1.4 GB is impractical — do per-file/per-chunk optimization) and see if it
   completes quickly → then it was just unoptimized speed.
2. **Check the `0x60000000+` region:** what does our `sys_memory_allocate`/`sys_mmapper`
   HLE return for the game's big allocations? If the game expects data at addresses it
   `sys_memory_allocate`'d (RPCS3: 0x30000000+) and ours differ / aren't populated, the
   structure build reads zeros and may loop wrong. Verify region addresses match expectations.
3. Read `func_00AE60A0`/`func_00451F68` to learn what dataset is being built (asset table?
   object registry?) and its expected bound.

## (history) Blocker #2 — init settles into a mutex-create / lock-unlock loop

After loading the 6 modules, steady state is a tight repeating loop of `sys_mutex_create`
(syscall 100) + `sys_lwmutex_lock`/`unlock`. Main thread IP keeps moving (progressing, not
deadlocked). Either heavy object init (a lock per object) or — more likely — a **spin-wait on
a result that never arrives** (we don't execute SPU code; SPURS was just loaded). Remaining
minor items: unresolved NID `0x63FF6FF9`, unresolved indirect calls `0x019FFC50` / `0x00A8CF98`.
Next: determine if the loop is finite init or a stuck wait; if a wait, find what it polls
(SPU/RSX/thread) and satisfy it (likely an SPU PPU-fallback or a thread/RSX signal).

## (history) Blocker #1 — `operator new` throws `bad_alloc` during early boot

### Symptom
After CRT/TLS init the game prints (via its own `sys_tty_write`):
```
exception: bad allocation
abort() is called from 0x0... (x9 frames, all zero — game's backtrace walker
                                 doesn't resolve in our env)
Continue... (Lv-2 is still running.)
```
then calls `sys_process_exit(code=1)`.

### Exact boot sequence (from YDKJ_SCTRACE + YDKJ_HLETRACE)
```
[ppu] loaded 2 PT_LOAD segments, entry OPD 0x01916138
[ppu] run: code 0x000124C0, toc 0x019B5040, sp 0x0FF00000
[crt] sys_initialize_tls: block 0x0E000000, r13=0x0E007000 (seg 0x0+0, mem 0)
[sc] 352  sys_memory_get_user_memory_size  -> total 256MB, avail 192MB   (our values)
[sc] 120  sys_rwlock_create
[sc] 125  sys_rwlock_wlock
[sc] 127  sys_rwlock_wunlock
[sc] 100  sys_mutex_create   x5
<bad_alloc thrown here — NO further syscall or HLE call>
[hle] sys_process_exit  nid=0xE6F2C1E7
```

### What we've established / ruled out
- **Not a stub/coverage gap:** zero unlifted-stub hits, zero unresolved NIDs, zero
  stubbed-syscall fallthroughs during the whole boot. Execution is clean.
- **Not endianness:** `vm_write32` correctly byte-swaps to big-endian; the game reads
  256MB/192MB correctly from `get_user_memory_size`.
- **Not an HLE malloc returning 0:** the *only* HLE call in the whole boot is the final
  `sys_process_exit`. The failing allocation makes **no** syscall or HLE call.
- **No `sys_memory_allocate (348)` / `sys_mmapper (330)` / `sys_heap_create` is ever
  called** before the failure.

### Leading hypothesis
The game's global `operator new` is backed by a **custom allocator** (FromSoftware
engine) that is bailing *before* it requests memory from lv2 — i.e. its arena/heap
size is computed as 0 or invalid, so the first allocation (likely during C++ static
construction) fails immediately. Likely inputs to that computation: the PS3
**process param** (`sys_process_param` in the `proc_prx_param` segment: SDK version,
`malloc_pagesize`, primary stack size) which our loader/HLE may not surface, or a
global the allocator-init reads.

### Concrete next steps
1. **Locate the allocator-init / operator-new path.** The strings live at guest
   ~`0x1770150` ("bad allocation") and ~`0x176E...` ("Continue... (Lv-2"). Find the
   lifted function that loads those addresses (search the chunks for the `lis/ori`
   building `0x1770150`) → that's the C++ terminate/abort handler; walk back to the
   `operator new` caller and the allocator's heap-base/size global.
2. **Watch the allocator's size global.** Once the heap-base/size global is known, use
   `YDKJ_WWATCH=<addr>` to see what value gets written (expect 0) and `YDKJ_RWATCH`
   to catch the read that triggers the abort.
3. **Check process-param exposure.** Verify what `proc_prx_param`/`sys_process_param`
   the game reads for its heap size and ensure the loader/HLE provides sane values
   (SDK version, malloc page size, primary stack size).
4. If the allocator expects a pre-reserved heap region, reserve it in the loader and
   point the allocator's base at it.

### Progress this session (still open)
- **Fixed `sys_initialize_tls` main-thread clobber** (real bug, upstream fix #5): the
  handler was discarding the loader's PT_TLS image on the game's `(0,0,0)` call,
  zeroing all TLS vars. Now a no-op when no template is supplied. The D3D12 window
  now opens, but the `bad_alloc` persists → TLS was not the (sole) cause.
- **Ruled out the memory-budget hypothesis:** bumping reported available memory
  192MB→224MB did not change anything.
- **Built a fatal backtrace pipeline:** on `sys_process_exit`, dump the guest stack
  back-chain (LRs are 0 — the lifted ABI uses direct C calls + trampolines, not the
  PPC LR-on-stack convention) **and** the host RVA backtrace (lifted `bl` = real C
  calls). Symbolize with `llvm-symbolizer --obj=des_boot.exe --relative-address`
  against the 251 MB PDB.
- **Localized the abort chain** (entry → … → exit):
  ```
  ppu_run → func_016B84B0 → func_01516590 → func_01686CE8 → func_016A0500
          → func_0168ABD0 → func_0168BBA0 → func_0167AD50 → func_016BB5A4 → sys_process_exit
  ```
  - `func_016BB5A4` = exit wrapper; `func_0167AD50`/`func_0168BBA0` = error-message
    builder/printer (read the message strings via `r2+0x4CB8/0x4CBC/0x4CC0`).
  - **`func_0168ABD0` is a C++ static-initializer / init-list runner** — it walks a
    list of `{func,toc}` OPD pairs and calls each via `ps3_indirect_call`. So the
    `bad_alloc` happens inside a **global constructor during static init**, before
    the engine's main allocator is set up.

### Refined diagnosis (important)
The abort chain `func_0168BC18 → func_0168BBA0 → func_0167AD50 → func_016BB5A4` is
GCC's **`__verbose_terminate_handler`**: `func_0168BC18` calls the exception object's
virtual `what()` (vtable dispatch) and the others format/print `"exception: <what>"`
from TOC strings (`r2+0x4CB8..0x4CC4`) and exit. So this is `std::terminate` running
**after** a `std::bad_alloc` was thrown and **fully unwound** — the throw site
(`operator new` → `__cxa_throw`) is already gone from the stack. **Therefore the
sys_process_exit backtrace can never reach `operator new`** (why it's been elusive).

Established beyond doubt: the game makes **no allocation request at all** (no
`sys_memory_allocate`/`mmapper`/container syscall, no malloc-via-HLE) before the
throw. So the engine's `malloc`/`operator new` returns null *immediately* without
asking the OS — its heap arena is empty and `morecore`/`sbrk` is never reached. Most
likely the libc/SDK malloc heap was never initialized (heap config = 0), plausibly
from the unparsed **process param** (`sys_process_param` in `proc_prx_param`).

### ROOT CAUSE FOUND (RPCS3 ground-truth comparison)
Booted DeS in RPCS3 with trace logging (log at `rpcs3/log/RPCS3.log`). The real boot:
```
liblv2: 0x1f53ddc  sys_memory_allocate(size=0x10000, flags=0x200) -> 0x10000000
liblv2: 0x1f57788  sys_memory_allocate(size=0x80000, flags=0x200) -> 0x10010000
game 0x167aa30     sys_memory_get_user_memory_size -> Avail=0xb650000 Total=0xd500000
game 0x1679474     sys_memory_allocate(size=0x100000, flags=0x400) -> 0x30000000
game 0x00c378b4    sys_memory_allocate(size=0x5600000 /*86MB*/, flags=0x400) -> 0x30100000
...
```
Two facts:
1. **Real `get_user_memory_size` returns Total=0xD500000 (213 MB), Avail=0xB650000** — not
   256/192. Fixed our handler to match (didn't fix the hang, but is correct).
2. **The first two allocations come from `liblv2.sprx` (loaded at 0x1f5xxxx) — its module
   init (`prx_start`) setting up the libc heap.** We do **not** load/run `liblv2`; we HLE
   its exports. So that heap-init never runs.

**Mechanism (high confidence):** With all HLE paths traced (fixed `YDKJ_HLETRACE` to cover
ctx/prx paths — the calls were all ctx-handlers, previously invisible), the boot makes
**only** `sys_initialize_tls`, 11× `sys_lwmutex_create` (= `std::ios_base::Init` / iostream
global ctors), `sys_time_get_system_time`, `sys_ppu_thread_get_id`, lwmutex lock/unlock,
then the terminate handler + `sys_process_exit`. **No `operator new`/`malloc`/`_sys_heap_malloc`/
`sys_mmapper_allocate_memory` is ever called** (game imports `_sys_heap_malloc` +
`sys_mmapper_allocate_memory` but never reaches them). So a C++ static constructor allocates
against the never-initialized libc/DL heap → DL "nonexistent heap" → `std::bad_alloc` →
terminate. On real PS3 this works because `liblv2`'s `prx_start` set up the heap first.

### THE FIX (next) — provide the liblv2 heap init
Either:
1. **LLE-load `liblv2.sprx`** (firmware module) via the harness `ps3_load_prx_modules()` hook
   + `prx_loader.c`, run its `prx_start` so the libc heap is set up as on hardware. Cleanest
   match to real behavior; needs the decrypted `liblv2.sprx` + PRX relocation/init.
2. **Replicate the heap init in our runtime/HLE**: at boot, do the equivalent
   `sys_memory_allocate(0x10000)/(0x80000)` and initialize whatever libc-malloc / DL
   runtime-heap globals the game reads, so the static allocator has a backing arena.
3. **Investigate C++ init order**: confirm whether a heap-setup constructor should run before
   the iostream ctors (the CRT init-array runners `func_00010D60`/`func_00010D50` in
   `func_000125F0`); if our order differs, fix the init-array execution.

### #2 investigation (replicate heap init) — findings + remaining blocker
- **Plug-and-play alignment:** LLE-loading `liblv2.sprx` (#1) needs the user's PS3
  firmware (copyright + not plug-and-play). Replicating liblv2's heap in our own clean
  code (#2) needs no firmware → correct for the goal. **Decision: pursue #2.**
- Heap API the game imports from `sysPrxForUser`: `_sys_heap_create_heap` (0xB2FCF2C8),
  `_sys_heap_malloc` (0x35168520), `_sys_heap_memalign` (0x44265C08),
  `_sys_heap_free` (0x8A561D92), `_sys_heap_delete_heap`, plus `sys_mmapper_allocate_memory`
  (0xB257540B). FromSoft's DL allocator wraps these.
- **No variable/data imports** (`nvar=0` for all 25 libs) — so the heap handle is NOT a
  liblv2 data export the game reads; the allocator state is an **internal game global**.
- The early `operator new` throws **without calling** any heap import or memory syscall —
  it checks an internal "heap ready / malloc limit" global (candidate: the newlib
  `_malloc_limit`, cf. the `"WARNING : '_malloc_limit' is ignored"` string) that liblv2's
  `prx_start` would have set. Fixing `get_user_memory_size` to real values did NOT clear it.

### Malloc chain fully traced (this session)
- `malloc` = **`func_0167AC88`**: loads the default heap from `*(TOC+0x4A68)` = **`0x01A1F850`**
  and calls the DL allocator **`func_01678748(heap, size)`**.
- The heap struct at `0x01A1F850` is **statically configured** (vtable/strategy pointers at
  +0x00/+0x18/+0x30; pool sizes `0x19000000`/`0x0B000000`/`0x12000000` at +0x0C/+0x24/+0x3C)
  but its **memory/free-list pointers are null (+0x08/+0x10/+0x14/+0x1C/+0x20/+0x28… = 0)** —
  i.e. **the heap is configured but never *backed* with memory**.
- It's backed on real PS3 by **`func_00C36C80`** (contains the `sys_memory_allocate(0x5600000
  /*86MB*/)` at ret-addr `0x00c378b4` seen in the RPCS3 trace) = `DL_SET_SYSTEM_HEAP_SIZE`.
  **Our build never calls it before the throwing constructor** → `func_01678748` finds null
  pools → returns 0 → `operator new` throws `std::bad_alloc`.
- So the fix narrows to: **get the DL heap (`0x01A1F850`) backed before the first allocation** —
  either make `func_00C36C80` run first (init-order), or replicate its backing in our runtime.

### CONFIRMED: init-order / heap-init runs but doesn't back the heap
- We trace ALL syscalls + ALL HLE: **zero `sys_memory_allocate`/`sys_mmapper_allocate_memory`**
  before the throw, and **no `0x00Cxxxxx` (game-proper) code runs** (ICTRACE has no 0x00C*
  targets). So the failure is purely in C++ static init / CRT, before `main`.
- The libc/default heap `0x01A1F850` is initialized by **`func_0167A9C8`** (called via
  `func_0167ACE0(heap=*(TOC+0x4A68))` during CRT init in `func_000125F0`). `func_0167A9C8`
  **runs** in our build (we see its `get_user_memory_size`/352 call) but the heap ends up
  **unbacked** (free-list pointers stay null) → first `operator new` → `bad_alloc`.
- The 86 MB DL system heap (`func_00C36C80`, called indirectly as a ctor from
  `func_00C35520`/`func_00C37948`) is a SEPARATE, later heap for game data — not the one the
  early ctors use. Not the immediate blocker.
- **So the precise bug: `func_0167A9C8` does not back `0x01A1F850` in our build the way it
  does on real PS3.** Decoding *why* (which internal branch/precondition differs) needs to
  watch its control flow at runtime.

### Next step: the `--trace-funcs` lifter feature (clean instrumentation)
Hand-editing the 3M-line generated chunks is too fragile (broke a `_cs_15` scope; reverted).
Add `--trace-funcs 0x167A9C8,...` to `ppu_lifter.py` to emit a clean entry/branch log for
chosen functions, re-lift, and watch `func_0167A9C8` decide whether/how to back the heap.
Then either fix the precondition it checks or replicate the backing in our HLE.

### (historical) Open question for the fix: init-order vs failed-setup
Need to know whether `func_00C36C80` (heap backing) *runs but fails* in our build, or *never
runs* before the throw. Tried instrumenting `func_00C36C80`/`func_0167AC88` entries by editing
the generated chunks — **too fragile** (broke a `_cs_15` scope in the 3M-line output; reverted).
**Better method:** add an opt-in `--trace-funcs <addr,addr>` to `ppu_lifter.py` that emits a
logging line at chosen function entries (clean, regenerated), OR set a host breakpoint on the
chunk object's symbol via the debugger. That single data point picks the fix:
- *never runs* → init-order / a missing early init call → make it run (clean, general fix).
- *runs but heap stays null* → our `sys_memory_allocate`/`sys_mmapper_allocate_memory` HLE
  returns something the DL setup rejects → fix the HLE to return a valid guest region.

### Remaining blocker for #2 + next techniques
Need to find the exact internal global the static allocator checks (and set it), which
static RE of stripped lifted code hasn't pinned down (throw unwinds; trampolines blur host
backtraces). Concrete next techniques (in order of leverage):
1. **Find & instrument the game's `operator new` / newlib `malloc` directly.** Locate it
   (e.g. it's the static fn that, on failure, constructs `std::bad_alloc` / references its
   typeinfo, or the one reached just before the first terminate-path call), add a one-line
   log in the generated chunk at its entry → see the size + which global it reads as 0.
2. **Build a minimal guest single-step / watch harness:** set `YDKJ_WWATCH`/`RWATCH` on the
   `_malloc_limit` / DL heap-state globals once located, to catch the read that triggers the
   throw and what writes (or fails to write) them at init.
3. **Diff init vs RPCS3 at instruction granularity** for `func_0167A9C8` (heap config) and
   the CRT init-array runners, to find the step that sets the malloc limit / heap state on
   hardware but not for us.

### Earlier next steps (superseded by root-cause above)
1. **RPCS3 ground-truth comparison (recommended, authoritative).** Boot DeS in RPCS3
   with LV2/`sys_memory` logging (porting guide method); capture the first ~200 boot
   syscalls. If real DeS calls `sys_memory_allocate`/container/`sys_process` setup
   that ours doesn't, that's the missing piece (we likely returned a value that made
   DeS skip its normal heap setup).
2. **Hook the throw, not the exit.** Find guest `__cxa_throw` / `operator new`
   (`_Znwm`) addresses (e.g. via the bad_alloc typeinfo xref, or by instrumenting a
   generated chunk) and log at the throw with a LIVE stack → identifies the failing
   allocation + size.
3. **Process-param / SDK heap.** Verify what heap-size/config the libc malloc reads
   at init (from `proc_prx_param`/`sys_process_get_paramsfo`) and ensure the loader
   surfaces sane values so malloc sets up its arena.

> This is the expected iterative phase: a large title's first allocation failure in
> stripped, lifted code, during C++ static init. Progress is measured
> blocker-by-blocker, and there will be many before the title is playable.

### Diagnostics reference (all in ps3recomp runtime)
| Env var | Effect |
|---|---|
| `YDKJ_SCTRACE=1` | log every lv2 syscall (num + r3-r6) |
| `YDKJ_HLETRACE=1` | log every resolved NID/HLE call (name + args + ret) |
| `YDKJ_WWATCH=0xADDR` / `YDKJ_RWATCH=0xADDR` | log guest writes/reads near an address |
| (automatic) | guest+host backtrace on `sys_process_exit` |

Symbolize host RVAs:
`"C:\Program Files\LLVM\bin\llvm-symbolizer.exe" --obj=port\build\des_boot.exe --relative-address 0x... 0x...`

---

## Blocker #4 — ROOT CAUSE FOUND: use-before-construction of a C++ singleton (2026-06-28)

Earlier theory (free-list runs off the bump allocator) was **wrong**. After upgrading
`ds_heap` to a real free-list allocator + fixing a region collision with
`sys_memory_allocate` (0x40000000-0x50000000 → moved ds_heap to 0x60000000-0xC0000000;
both correct keepers), the spin was **unchanged** → it is allocator-independent.

### The spin mechanism (func_00AC8078)
`func_00AC8078` is the game's segregated free-list allocator's free-block search.
With a **NULL/0 heap descriptor**, its size-class bin loop never terminates:
```
loc_00AC80E8:  r9 = r9 << 1;   // r9 starts 0 (gran/shift from NULL desc) → stays 0
               r11 += 8;
               if (r7 > r9) goto loc_00AC80E8;   // needed_size > 0 always → INFINITE
```
So the spin is a symptom: it received a NULL heap.

### Why the heap is NULL (the real bug)
The heap should come from a **virtual method call** that silently no-op'd. Traced via
`ctx->cia` stamping + a call-site probe to the **exact** failing instruction:
`func_00A35780` @ guest `0x00A359E4` — a `vtable[3]` dispatch on the singleton object
`0x01B92CF8` (BSS). Probe at the call:
```
[CALL1] obj=0x01B92CF8 *obj(vt)=0x00000000 vt[3]=0 *(vt[3])=0 r10=0 ctr=0x018736B0
```
**`*(obj) == 0` — the object's vtable pointer is NULL: it is used BEFORE it is
constructed.** The lifted virtual call dereferences through 0: `r10=*(0+0xC)=0`, then
`ctr=*(0)=vm_read32(0)=0x018736B0` (incidental garbage in our flat VM's unmapped low
memory — guest[0] is not file-backed). The runtime can't resolve 0x018736B0 (it's the
*data* address of a vtable), logs "unresolved indirect call", and **returns without doing
anything** → the method that sets up the heap never runs → NULL heap → spin.

The 0x018736B0 / vtable-chasing was a **red herring**; the defect is the NULL vtable.

### Where construction goes wrong
The singleton accessor is `func_00B0AF28` (Meyers-style lazy static): it checks two guard
bytes and either returns the existing instance or constructs (ctor `func_00A668B0` stores
the vtable; `func_00A66790` runs post-init). It returned the object **unconstructed**
(vtable still 0). Construction is gated by C++ **thread-safe static init**:
`func_00B0AF28` → multi-TOC thunk `0x000121B0` → guard helper `0x01664CD8`
(`bl 0x0166F990` acquire / guard counter / `bl 0x0166F910` release — lock-protected).

WWATCH on `0x01B92CF8` confirms the vtable IS eventually written `0x018736B0`, but **after**
the failing use → a use-before-init **ordering / guard** failure.

### Ruled out
- Our allocator (spin present with both the zeroed bump allocator and the new free-list).
- Region collision (fixed; spin unchanged).
- Relocations (EBOOT is fixed-load: no `.rela`, no `PT_DYNAMIC`; the vtable global
  `@0x018FE1E8`=0x018736B0 is already correct in-file).

### Next step (continuing #1)
Investigate the thread-safe-static-init guard path: `0x01664CD8` and its lock funcs
`0x0166F990`/`0x0166F910` (are these lwmutex/sync primitives our runtime mishandles?), and
whether the init runner `func_00A35780`/`func_00A35DB0` re-enters the singleton during its
own construction. Likely fix: make the guard/lock behave correctly so the constructor
actually runs before the accessor returns. Secondary hardening: map guest page 0 as a
fault/guard so NULL-vtable derefs trap loudly instead of reading garbage.

Diagnostics added this round (kept): `ppu_loader.cpp` unresolved-call handler now prints
`ctx->cia`, the `this`/vtable chain, and a deep `CaptureStackBackTrace`. Probes in the
generated chunk 005 (AC8078 desc dump, CALL1 dump, cia stamps) are in gitignored files and
are removed on the next re-lift.

### Blocker #4 — deeper: the singleton is zeroed then used before its vtable is set

Instrumented the lazy accessor `func_00B0AF28` (guard-byte trace) and watched the
object's vtable slot `0x01B92CF8` (`YDKJ_WWATCH` + host backtraces). Findings:

- The accessor path uses C++ thread-safe-static guards; a **second entry** returns the
  object while `*(obj)==0` (vtable not set) — `[B0AF28] RETURN-EXISTING *obj=0 guard2=1`.
- The vtable slot `0x01B92CF8` is written by two routines (zero **before** set):
  - `func_00AC7578` (+0x38) — a field-clearing ctor that zeroes obj+0(vtable),+4,+0x10..+0x3C
    (a ~0x40-byte object). This is what leaves `*(obj)=0`.
  - `func_00B25CB8` (+0x1DD) — sets `*(obj)=0x018736B0` (the vtable) **later**.
- The failing virtual call (`func_00A35780`@`0x00A359E4`, `vtable[3]`) lands in the window
  **between the zero and the vtable-set**, so it dereferences a NULL vtable, reads garbage
  from unmapped guest[0] (`0x018736B0`), the call is unresolved → no-op → NULL heap → spin
  in `func_00AC8078`.
- Whole chain is top-level C++ static init: CRT global-ctors (`func_00010210`) →
  `func_007CF5D0` → `func_007CEE78` → `func_00AE7768` → `func_00A35DB0` →
  `func_00A35780` (init-list runner) → accessor/dispatch.

So the defect is a **construction-vs-use ordering / reentrancy** in static init: the heap-
manager singleton is used (virtual init call) after being cleared but before its vtable is
installed. On real PS3 this same code works, so our environment perturbs the order (extra
re-entry, or a divergent branch in the init list). Host backtraces can't prove nesting
(tail-call trampolines unwind the host stack), and the construction spans multiple routines
(`func_00AC7578` clear, `func_00B25CB8` vtable-set) — so the next step is to find
`func_00B25CB8`'s caller and why `func_00A35780` dispatches on the object before that runs.

Pragmatic fallback if the ordering proves intractable: map guest page 0 as a no-access guard
so NULL-vtable derefs trap in the VEH (loud, catchable) instead of silently reading garbage
and spinning.

Diagnostics added to `ppu_loader.cpp` (kept): `ds_dump_bt()` host-backtrace helper, and a
`YDKJ_WWATCH` backtrace on the exact watched word.

### Blocker #4 — confirmed SAME-THREAD reentrancy in C++ static init

Tagged the accessor probes with the OS thread id (`ds_tid()`). Both the constructing call
and the half-built-returning call are on the **same thread** (tid identical):
```
[B0AF28] enter guard1=0 callno=1 tid=T        (1st: begins construction)
[B0AF28] === CONSTRUCT-B first-time            (guard1=1, guard2=0, calls guard-helper)
[B0AF28] >>> CONSTRUCT-A running               (real ctor begins)
[B0AF28] enter guard1=1 callno=2 tid=T         (SAME thread re-enters mid-construction)
[B0AF28] RETURN-EXISTING *obj=0 guard2=1 tid=T (returns the half-built object)
→ CALL1 NULL vtable → spin
```
So `func_00A35780` (the static-init list runner) re-enters the *same* singleton's
initialization on the same thread, before its vtable is installed, and uses it. The object's
vtable IS written later (WWATCH showed the write *after* the spin point — from another thread
that also constructs it), but the spinning thread already grabbed the NULL-vtable version.

Same-thread recursive entry into a thread-safe static's init is C++ UB; the retail game must
not do this on real PS3. So our environment perturbs either the construction order (vtable
installed later than on PS3) or the init-list iteration (an HLE/resource return value steering
`func_00A35780`/`func_00A66790` down a branch that revisits the singleton). A wait-loop fix is
impossible (same-thread → would deadlock, since the ctor is paused up the stack).

Next candidate directions:
- Find what `func_00A66790`'s post-ctor virtual call does that re-enters `func_00A35780`
  (likely an HLE/resource query returning a value that differs from PS3, taking a branch that
  re-walks the init list).
- Re-examine whether our CRT entry runs global ctors / the init list in the same order PS3
  does (boot harness vs the game's own `func_00010210` CRT path).
- Pragmatic hardening: map guest page 0 no-access so the NULL-vtable deref faults in the VEH
  (loud + exact guest PC) instead of silently reading garbage and spinning.

### Blocker #4 CLEARED (experiment) — use-before-build confirmed; now blocker #5 (same class)

Re-lifted with `ctx->cia` stamping before every bl/bctrl/bcctrl/bctr (lifter change), so the
exact guest PC of any divergence is recorded with no /O2-inlining/trampoline ambiguity. With
caller-PC + object-identity probes, the picture is now exact:

- `func_00A35780` (static-init runner) does, in order:
  - `0x00A359E4`: `X = func_00B0AF28()` — accessor returns X=0x01B92CF8 with **`*X=0` (unbuilt)**
  - `0x00A35A14`: `X.vtable[3]()` — dispatch on a NULL vtable → no-op → downstream NULL heap → spin
  - `0x00A35AFC`: `func_00B25CB8(X)` — actually CONSTRUCTS X (too late)
- The accessor `func_00B0AF28` never builds X; X is built only by `func_00B25CB8`. So X is
  **used before it is constructed**.

EXPERIMENT: made `func_00B0AF28` lazily build X (call `func_00B25CB8` on a ctx copy when
`*X==0`, guarded against recursion). Result: `*X` becomes 0x018736B0, **the unresolved
indirect call disappears, and the spin moves on** — init advanced one subsystem further
(func_007CEE78+0x8A9 → +0x8CF). This CONFIRMS use-before-build is the root cause.

New state — **blocker #5, same class**: `func_00AC8078` spins again with `desc=0x00000000`
(NULL heap), called from `func_00B26C18+0x68`, reached at `func_00A35780+0x10A9`. So another
heap manager's POOL is NULL — the game's heap managers have uninitialized pools because
pool-creation is itself a virtual call on a not-yet-built object (a bootstrap cycle). The
force-build (specific to func_00B0AF28's singleton) doesn't cover this different object.

IMPLICATION: this is whack-a-mole at the symptom level. The robust fix is the ROOT — either
(a) fix the static-init ordering so objects/heaps are built before use, or (b) back the game's
DL heap subsystem so its pool-creation runs natively (the original "#2" approach). The lifter
`cia` stamping + crash-handler `cia` reporting are kept as permanent debugging infrastructure.

### Blockers #4 & #5 CLEARED — boot advancing through C++ static init (2026-06-28)

Two pragmatic fixes (persisted by `scripts/apply_boot_patches.py`, re-applied after each
re-lift) get the game past its memory-bootstrap and steadily forward through static init:

1. **func_00B0AF28 lazy-build (clears #4).** The accessor for heap-manager singleton
   X=0x01B92CF8 returned X unbuilt (`*X==0`), so the init runner `func_00A35780` dispatched a
   virtual method on a NULL vtable. Fix: when `*X==0`, construct X via its real ctor
   `func_00B25CB8` (on a context copy, so only guest memory is built). Result: the vtable is
   installed, the unresolved indirect call disappears, the dispatch resolves.

2. **func_00AC8078 NULL-heap -> ds_heap (clears #5).** The DL segregated allocator's bin
   search spun forever (size-class doubling from 0) when handed a NULL heap descriptor. The
   game's heap pools are transiently NULL during static init. Fix: when the descriptor is
   NULL, route the allocation to our backed `ds_heap` (real memory) instead of spinning.
   Returning "alloc failed" (0) is NOT enough — the game then takes a failure path that hits
   another unbuilt vtable; the allocation must SUCCEED.

Effect: the init runner `func_00A35780` advanced from +0x28F (blocker #4) through +0x10A9
(#5) to +0x124A, executing real game code through the indirect-call trampoline, with ~18
worker threads alive. The DL allocations now succeed via ds_heap (ptr=0x60000000+).

**New blocker #6**: a loop in `func_00A35780+0x123D` (init-runner), chain
`func_007CF5D0 -> func_007CEE78 -> func_00AE7768 -> func_00A35DB0 -> func_00A35780`, with
`func_00B26C18` (allocator wrapper) in the stack. Likely another list-walk / use-before-build
in the same static-init machinery. The lifter `cia` stamping + ds_heap routing are the tools
to keep grinding these.

These fixes are pragmatic (symptom-level) but VALIDATE the model: provide working memory +
lazily build singletons, and the game's own static init proceeds. The long-term-clean
alternative remains backing the DL heap subsystem so all pools initialize natively.

### Blocker #6 — data-structure init cascade begins (2026-06-28)

After #4/#5, the init runner `func_00A35780` advanced to a loop at host +0x123D, inside (an
inlined) `func_00A35210` (called at guest cia=0x00A35990, args r3=obj, r5/r6 from stack,
r7=0x1F — looks like a hash/sorted insert with a 31-mask). HOTMAP shows it polls a container
at ds_heap addr 0x71000000 (tagged ptr 0x71000003): begin (@+3) and end (@+7) read 0, while
it sweeps an inline array (@+0xB, stride 0xC) — i.e. it iterates a container whose storage is
zero/uninitialised, so the sweep climbs forever (the same "iterate past a NULL bound" shape
as the func_00AC8078 spin, but now in game container code).

This is no longer the memory-bootstrap (#4/#5, cleared). It's the FIRST of a likely CASCADE
of game data-structure init issues: structures get allocated (now succeeding, via ds_heap)
but their contents/bounds aren't what the game expects, so iterations over them diverge. Each
is increasingly game-specific RE.

STRATEGIC NOTE: the memory-bootstrap blockers were the hard, high-value unlock (done). The
remaining cascade is long and suggests a systemic cause worth finding before more whack-a-mole
— most likely the global-ctor / static-init ORDER or completeness differs from real PS3 (an
early init that should populate these containers runs differently here), OR a shared upstream
resource these inits depend on returns empty. Candidate next investigations:
 - Trace whether ALL global ctors (.init_array via the game CRT func_00010210) run to
   completion in the same order as RPCS3 (compare init-call sequences).
 - Identify what populates these containers on real PS3 and why it's empty here (likely an
   HLE/resource read returning 0/empty that steers an init branch).
 - Or pivot to backing the DL heap subsystem natively (the "#2" approach) so the game's own
   memory + init machinery runs end-to-end instead of being patched per-symptom.

### Systemic-root hunt — finding: pervasive use-during-construction in C++ static init (2026-06-28)

Investigated the systemic cause of the post-bootstrap cascade. Results:

- **Not a missing data file.** No cellFs/file opens happen before the spin — the game is still
  in pure in-memory C++ static init, before any I/O. So empty containers aren't from a failed
  resource read.
- **Not a single empty registry.** `func_00A35780` (the registry processor) is called once
  with an EMPTY registry list (`*head==0` at 0x01B9111C), correctly taking its init/create
  path. It then builds a nested container and spins iterating it while the container's
  begin/end pointers are still 0 (the container object is freshly allocated zeros; its ctor
  hasn't set begin=end=storage yet).
- **The pattern is pervasive.** Every layer is the same shape: a structure (vtable, container
  begin/end, free-list bound) is read/iterated before the code that initializes it has run.
  #4 (object vtable), #5 (heap pool), #6 (nested container begin/end) are all instances. Our
  pragmatic patches clear individual instances but the pattern recurs one construction layer
  deeper each time.

CONCLUSION: there is no single fixable "root value" — the divergence is in *construction
ordering / re-entrancy* across the game's C++ static init, and it is pervasive. Likely either
(a) our execution subtly diverges from real PS3 so inits run in a different order / re-enter
(possibly exacerbated by the force-build-on-context-copy itself, which builds guest memory but
discards register-flow side effects), or (b) the whole DL heap + container machinery needs to
run natively rather than be patched per-symptom.

REALISTIC PATHS (for a future session):
 1. **DL-heap pivot** — back the game's DL system heap so its own allocator + container ctors
    run end-to-end (most likely to dissolve the whole class; larger up-front work).
 2. **RPCS3 differential** — run the same boot under RPCS3 with PPU tracing and diff the
    init-call / construction sequence to pinpoint the FIRST divergence (where our order/values
    first differ from real hardware). Surgical but needs RPCS3 trace plumbing.
 3. **Reconsider scope** — a full boot-to-menu is a long effort; the memory bootstrap (done)
    was the first major milestone of many.

Net for this session: the hard memory-bootstrap blockers (#4/#5) are CLEARED and persisted;
the remaining work is a deep static-init construction-ordering problem, not a quick patch.

### RPCS3 differential — findings (2026-06-28)

Booted the same EBOOT.BIN under RPCS3 (v0.0.30; per-game config BLUS30443) headless and
captured its log (rpcs3/log/RPCS3.log). Notes on mechanics: RPCS3 writes its live log to
`rpcs3/log/RPCS3.log` (not the top-level one), buffers via RPCS3.buf (flushed on clean exit,
so SIGKILL loses tail), and the first boot needs minutes for PPU module load + SPU cache
build. Disabled its auto-updater (check_for_updates=false) to stop it hijacking startup.

Results:
1. **RPCS3 boots Demon's Souls FULLY** — reached the menu/intro and idled (log at 5:51
   runtime; syscall histogram shows ~380k sys_timer_usleep, 324k sys_event_queue_receive,
   226k sys_cond_wait, etc.). So the target is definitely reachable; RPCS3 sails past our
   static-init spin.
2. **Demon's Souls is a SPURS/SPU engine (FromSoftware Dantelion2).** It imports the entire
   `cellSpurs` API and RPCS3 found SPU executables named `SpursTaskZlibInflate_PS3_MASTER`,
   `SpursTaskCommandDispatcher_PS3_M`, and `n:/dantelion2_autobuild/dist/ps3...`. **Our
   recomp executes NO SPU code** (the known crux) — so even after clearing every static-init
   blocker, a working boot ultimately REQUIRES SPU support (run SPU code or PPU-side
   fallbacks for the SPURS tasks). This is the fundamental gate for the project.
3. **Our current spin is PRE-SPURS, single-threaded C++ static init.** Our HLE trace right
   before the spin is all `sysPrxForUser`: `sys_lwmutex_create` (0x2F85C0EF, many times — one
   guard per static object), then `sys_lwmutex_lock`/`unlock` (0x1573DC3F/0x1BC200F4). No
   cellSpurs, no sys_spu, no sys_ppu_thread_create yet. So the empty-container spin is a pure
   PPU static-init construction-ordering issue, not (directly) the missing SPU.

CONCLUSION: the differential confirms (a) the boot is reachable, (b) SPU is the eventual hard
requirement, and (c) the immediate blocker is early PPU static init. The one piece automation
can't give is RPCS3's exact memory/register state at our spin (func_00A35780 / the container
build) — that needs RPCS3's interactive GUI debugger (set a breakpoint at guest 0x00A35780,
read the container's begin/end). RPCS3 is confirmed working for this, so that differential
data point is available on request.

STRATEGIC: two real tracks remain, both large — (1) finish the PPU static-init so boot
progresses, then (2) implement SPU/SPURS (the crux) for an actually-working game. The memory
bootstrap (#4/#5) milestone stands; everything is committed + reproducible.

### Fresh look + research (2026-06-28) — lwmutex fix applied; container spin narrowed

Resources consulted: ps3recomp's own docs (GAME_PORTING_GUIDE Phase 11 confirms the SPURS
zlib task -> host-zlib fallback approach, and "compare with RPCS3"); RPCS3 source
(Emu/Cell/Modules/sys_lwmutex_.cpp); PS3 dev wiki (PRX, LV2). 

REAL FIX FOUND & APPLIED (ps3recomp 4950f4e): our `sys_lwmutex` was a no-op owner-stamp that
always returned CELL_OK and treated every re-lock as recursive. Per RPCS3, a same-thread
re-lock of a NON-recursive lwmutex must return **CELL_EDEADLK** — the C++ thread-safe-static
guard relies on that to detect recursive initialization. Reworked create/lock/unlock to real
semantics (combined attribute incl. SYS_SYNC_RECURSIVE at attr+4; owner==tid -> EDEADLK unless
recursive; real per-thread owner via ds_tid). Effect: advanced the static-init spin
(func_00A35780 +0x129D -> +0x133A) — recursion-detection was one genuine factor.

REMAINING (blocker #6): still spins iterating a packed container at 0x71000000 (begin@+3,
end@+7, both 0 / element ptr ~0x70FFFFFF) during X's construction on the registry's SINGLE
(empty-registry create) path: func_00A35780 -> func_00B0AF28/func_00B25CB8 -> ... ->
func_00A35210 (a std::lower_bound-style binary search that never converges on the
empty/misaligned container). All heavily /O2-inlined, which is why host->guest mapping is
fuzzy. Two ways to crack it: (a) RPCS3 GUI debugger — breakpoint at guest 0x00A35780, read the
container begin/end to get ground truth (is it empty here on real HW, i.e. our loop is a
lifter empty-check bug, or populated, i.e. our construction diverged); (b) build chunk 005
without /O2 to de-inline func_00A35210 and instrument it standalone (risks reviving the slow
blocker-#3 loop). 

NET: the research delivered a correct lwmutex fix + strategic clarity (RPCS3 boots fully; SPU
is the eventual gate). PPU static init is partially advanced; the container spin remains.

### ROOT FOUND via RPCS3 differential (2026-06-28) — registry registered then wiped

Used RPCS3's interpreter + debugger (breakpoint at guest 0x00A35780, main PPU thread) to read
the registry list head on real hardware, and WWATCH on our side. Decisive comparison:
  - r2 (TOC) = 0x019B5040 in both. Registry head chain: *(r2-0x6C50)=0x01905EAC=r30;
    *(r30-0x7FD4)=0x01B9111C = list-head sentinel (SAME static addr in both).
  - **RPCS3: *(0x01B9111C) = 0x01B91128** (head -> a real node; registry POPULATED).
  - **Ours: *(0x01B9111C) = 0** (registry EMPTY).

WWATCH on 0x01B9111C in our recomp shows WHY ours ends up empty:
  - func_00A35780 +0xA6E  writes *head = 0x01B91128   (registration runs correctly!)
  - func_00A35780 +0x137D/+0x139F/+0x13C0  then runs a 3-stores/iter copy-or-memset loop that
    zeroes 0x01B9111C, +0x20, +0x24, +0x28 ... (wipes the registry) and never terminates.

So our recomp registers the entry then a runaway loop WIPES it (and spins). The loop's end
bound derives from the empty/uninitialised source container at 0x71000000 (begin==end==0 read
at 0x71000003/0x71000007). On real PS3 the registry is already populated when func_00A35780
runs, so it takes the *walk* path and never executes this *create*-path copy loop.

Likely fix angles (next):
  1. The copy loop iterating an empty (begin==end) source without terminating smells like a
     LIFTER empty-check bug in the inlined loop (general win if confirmed) — read the exact
     loop's begin==end guard.
  2. Or a static-init ORDER issue: the registering ctors run before func_00A35780 on PS3 but
     our env reaches func_00A35780 with the registry still empty -> create path -> the buggy
     loop. (Earlier #4/#6 were symptoms of this same empty-registry/create-path divergence.)

This is the first time we have GROUND TRUTH (RPCS3) pinpointing the divergence: registration
itself works; the create-path copy loop is what corrupts/wipes it.

### Not a lifter empty-check bug — it's an uninitialized container (begin=end=0)

Read both copy loops in func_00A35780:
- Walk-path copy (loc_00A358E8) HAS a correct begin==end guard (skips empty).
- Create-path (loc_00A35ADC/loc_00A35B30) builds + registers the node correctly
  (`vm_write32(r31+0, r26)` = *head = node), and the runaway copy is in an inlined helper
  whose SOURCE container has begin==end==0.

The runaway loop is effectively `for (p=&inline_storage; p != container.end; p+=12)`. A
correctly-constructed *empty* container has `end == &inline_storage` (loop exits at once);
ours has `end == 0`, so `p` climbs from storage and never meets 0 -> runs forever, zeroing the
registry. So the container's CONSTRUCTOR never set begin/end (still 0). This is the
use-during-construction root (confirmed by the RPCS3 differential: real PS3 has these set),
NOT a single lifter codegen bug. No one-line fix.

Bottom line of the differential investigation: registration logic is correct; the failure is
that a C++ container reached the create-path copy with begin/end uninitialized — the same
construction-order/completeness problem, now proven against ground truth. The lwmutex fix
(EDEADLK) was a correct contributing fix but the construction-order issue remains the wall.

### .init_array / global-ctor investigation (2026-06-28) — iterator is fine; ctor-internal construction is the issue

func_016B84B0 is the C++ global-constructor iterator (__do_global_ctors_aux): list ptr =
*(r2+20660), walk backward (r31-=4), `mtctr; bctrl` each entry, stop at the -1 sentinel.
It runs correctly in our recomp, so global ctors DO execute in the same fixed order as real
PS3 (same binary, same list). The wall is therefore NOT missing/misordered .init_array entries.

The failure is INSIDE a global ctor (func_007CF5D0 -> ... -> func_00A35780): it uses a nested
C++ container at 0x71000000 whose begin/end (+3/+7) are 0. The container's inline storage is at
+0xB; a correct ctor sets begin=end=&inline_storage. Ours are 0 -> the object was allocated
(raw zeros, from X's NULL heap via our ds_heap fallback) but its constructor never ran ->
runaway create-path copy -> wipes the registry. So: allocated-but-not-constructed, the
use-during-construction cascade, now localized to *within* a running ctor.

SYNTHESIS / real fix direction: these nested objects are allocated from the game's DL system
heap, which in our build is NULL (we force-build X's vtable and fall back to ds_heap for the
raw bytes). A raw ds_heap block is not a substitute for a functioning DL heap + the game's own
construction path. The robust fix is to **functionally initialize the game's DL system heap**
(the banked "#2" DL-heap-backing) so the game's own allocator AND the object/container
constructors run natively, instead of patching symptoms. The force-build + NULL-heap->ds_heap
fallbacks carried us through the memory bootstrap but cannot fully replace a working heap.

## BREAKTHROUGH (2026-06-29) — root was a rotate-by-zero UB in the lifter, not a C++ cascade

After returning to `master` (override approach) and re-lifting, the boot still spun in the
C++ static-init machinery. Backtrace-resolved the spin to `func_00A35780` (a type/factory
registry), in a 0xC-stride container-copy loop reading `begin=0x70FFFFFF, end=0` from low
memory and running away. The copy pointer `r28` should have been `r27+0x70` (the registry
node's container header), but was 0/garbage.

Root cause (NOT the construction cascade we'd been chasing): the lifter's rotate helpers
invoked C undefined behavior for rotate-by-zero:
- `ppc_rotl64(v,n)`: `(v<<n)|(v>>(64-n))` → `v>>64` UB when n==0.
- `ppc_rlwinm`/`ppc_rlwimi`: `(rs<<sh)|(rs>>(32-sh))` → `rs>>32` UB when sh==0.

`rldicl rA,rS,0,32` (32-bit truncate) and `rlwinm …,0,…` (mask/move) are everywhere; clang
-O2 miscompiled them inside the huge `func_00A35780`. Probe proof:
`ppc_rldicl(0x01B91198,0,32)` returned **0x00000002** instead of 0x01B91198, so
`r28 = r27+0x70` came out wrong → runaway copy. Many earlier "NULL pointer / use-before-
construction" symptoms (blocker #4/#5/#6, the RPCS3 register-then-wipe) were downstream of
this single miscompile.

Fix (ps3recomp 58fd9d8): guard the shift on sh/n==0 in all three helpers. After the fix,
`r28` computes correctly (0x01B91198), the node container is empty (begin=end=0) so the copy
is skipped, and boot advances PAST the entire static-init cascade into
**`cellSysmoduleLoadModule`** (PRX module loading).

New frontier (next blockers): unresolved HLE NID `0x63FF6FF9`, and unresolved indirect calls
at guest `0x00A8CED4` and `0x00AE16F4`. Memory: [[demons-souls-rotl-ub-bug]].

Open cleanup: re-evaluate whether `scripts/apply_boot_patches.py` (force-build singleton X,
func_00AC8078 NULL-heap->ds_heap) is still required now that the miscompile is gone — some of
those NULL-pointer symptoms may have been the UB, not real construction-order bugs.
