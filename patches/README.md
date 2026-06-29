# Patches to ps3recomp

We use [ps3recomp](https://github.com/sp00nznet/ps3recomp) as the recompilation
base. The full clone lives in `../ps3recomp/` (gitignored — third-party, large).

**Canonical home of our changes:** our ps3recomp fork —
[github.com/JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp)
(branch `master`, currently 27 commits ahead of upstream `sp00nznet/master`). Clone
that instead of upstream to get every fix already applied.

`01-portability-and-scaling-fixes.patch` is a **convenience export** of the full
delta vs upstream (`git diff sp00nznet/master..fork/master`), so the fixes survive a
re-clone of upstream and are ready to upstream as a PR. It may lag the fork; regenerate with:

```
git -C ps3recomp diff origin/master..master > patches/ps3recomp/01-portability-and-scaling-fixes.patch
```

Apply to a fresh upstream clone with: `cd ps3recomp && git apply ../patches/ps3recomp/01-portability-and-scaling-fixes.patch`

## What it contains (all general improvements, not Demon's Souls-specific)

Grouped by area — see the fork's commit log for per-change detail:

- **Lifter (`tools/ppu_lifter.py`)** — O(N²) mid-function-entry slicing fix (multi-minute hang → <1 min on a 106k-fn binary); **PIC switch jump-table reconstruction** (two-level-TOC base resolution + inline-table fallback, lifted as an in-function `ctr` switch — was emitting unresolved indirect calls); **rotate-by-zero UB** fix in `ppc_rotl64`/`rlwinm`/`rlwimi` (clang miscompiled the ubiquitous `rldicl …,0,32` under -O2); optional `--override-funcs`; **`--shadow-stack`** (per-function guest call-stack guard for reliable backtraces).
- **SPU lifter (`tools/spu_lifter.py`) / `runtime/spu`** — added `eqv`/`sumb`/`mpyhhu` (covers all of DeS's SPU images).
- **Runtime (`runtime/ppu`)** — function-registry hash 64K→512K slots + bounded-probe guard (106k-fn titles overflowed the old table → init hang); propagate HLE ctx-handler return to r3; shadow-call-stack backing store + `ds_dump_shadow`; indirect-call/trampoline diagnostics.
- **Syscalls** — corrected event-flag syscall numbers (were colliding with timer/usleep); raised `SYS_MUTEX_MAX`/`SYS_COND_MAX` 256→8192; real `sys_lwmutex` deadlock semantics; **implemented `sys_memory_get_page_attribute` (351)** with per-allocation page-size tracking.
- **GCM (`libs/video/cellGcmSys.c`)** — moved the FIFO control register into guest memory + `get→put` drain (the title polls it as a guest address; a host pointer made its FIFO-drain wait spin).
- **Build/portability** — exclude duplicate `sceNpCommerce.c` (lld-link dup symbols); Win32 `<dirent.h>` shim (in our `port/compat/`, on the include path).
