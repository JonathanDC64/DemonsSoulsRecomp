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

## Blocker #1 (CURRENT) — `operator new` throws `bad_alloc` during early boot

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

### Concrete next steps (resume here) — need a throw-site or ground truth
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
