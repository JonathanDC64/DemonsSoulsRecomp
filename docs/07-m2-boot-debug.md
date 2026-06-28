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

> This is the expected iterative phase: a large title's first allocation failure in
> stripped, lifted code. Progress is measured blocker-by-blocker.
