# 24 — HighGraphics init, the RSX FIFO layout, and the boot reaching the engine

**2026-10-13.** After doc 23 the boot streamed its assets, but only some of the time. The main
thread was parked behind **HighGraphics init**, which never finished. This doc follows that gate
link by link. Each link was a real bug in the lift or the runtime, not in the game:

| # | Symptom | Root cause | Fix |
|---|---------|------------|-----|
| 1 | image 2 HALT-asserts on a size-0 DMA | `cntlzw(0)` lifted as `__builtin_clz(0)` (UB) | guard every clz in the old lift |
| 2 | image 2 runs, job chain never completes | SPURS **job binary** 0x0177B600 never lifted | lift + register job blobs in `build_spu.py` |
| 3 | HighGraphics waits forever on RSX label 0xFF | default command buffer at IO 0 instead of IO 0x1000 | real libgcm layout in `cellGcmSetupContext` |
| 4 | `unresolved indirect call -> 0x00ACAF2C` | the old lift dropped jump-table cases | `scripts/fix_jumptables.py` (424 cases restored) |
| 5 | image 7 DMAs image 1's data | taskset/task handoff through one process-wide global | per-thread dispatch handoff |
| 6 | resource-cache write-back fails (ENOENT) | `CELL_FS_O_*` flags transcribed as hex, not octal | octal values in `ppu_fs.cpp` / `sys_fs.h` |
| 7 | label wait again after ~19 draws | FIFO callback rewound to `begin` | fragment-ring `cellGcmCallback` semantics |

After 1–6 the boot creates the **whole engine thread set**: HG_Watchdog, Havok, all four
`Core.Res.*` threads, MO audio, NexusRevolution, DsSaveLoadMan, SLSession. Before, there were 4
threads. The file-open sequence matches RPCS3's entry for entry through the shader bundles and
their cache write-back.

---

## 1. cntlzw(0) — the lift's other zero-input UB

`func_00B13900` is a lazy singleton getter whose `(byte != 0)` result is computed branch-free with
`cntlzw`. The 2026-07-01 lift emits `__builtin_clz((uint32_t)x)`, which is **undefined for 0**.
PowerPC defines `cntlzw(0) = 32`. clang exploited the UB, so HighGraphics init took its failure
path and never initialised the command object. Its 0x9023 command payload stayed all-zero, and
image 2 asserted (`heqi` in `func_0000C4D8`) on the resulting size-0 `mfc_get`. Real hardware
would assert on that payload too.

This is the same class of bug as the rotate-by-zero UB in the
[PPU boot doc](08-ppu-boot-complete.md). Upstream fixed it in the lifter (a781188, emits
`x ? __builtin_clz(x) : 32`). The current lifter doesn't boot this title yet (§8), so the old lift
is patched in place. All `__builtin_clz((uint32_t)gpr)` and `__builtin_clzll(gpr)` sites get the
guarded form.

## 2. SPURS job binaries

Image 2's job chain runs a job whose binary is a raw PIC blob at EBOOT vaddr **0x0177B600**
(0x810 bytes, FNV-1a-64 `0xbd46853e5ade5098`). Jobs are loaded at LS 0 and fingerprinted over
their raw bytes. That blob was never lifted, so the job missed and the chain never completed.

`scripts/build_spu.py` now lifts every `work/spu_jobs/*.bin`:

- wrap the bytes as an ELF (`wrap(raw, base=0, entry=0)`)
- lift with `--auto-functions`
- register under the raw-bytes fingerprint (image id 13)

The job runs, sets event flag 0x01B93E00, and the chain completes.

## 3. The RSX command buffer starts at IO 0x1000, not 0

With 1–2 fixed, HighGraphics init sat in `func_00B5ED50` → loop at 0x00B5EFC8. That loop waits
for RSX **label 0xFF** (`cellGcmGetLabelAddress(0xFF)` = 0x20000FF0) to reach the fence value it
queued.

The queue is a PPU→SPU command ring at 0x45A4B280 (control block 0x01B51000: `+0` SPU get,
`+0x80` PPU put). Dumping every SPU fetch of it (`SPU_DUMP_GET`, now up to 16 dumps) showed:

```
{4, 019DC100, 019DC280}   init
{0x18, 0xFF, 0}           label 0xFF := 0
{0x19, 1} {0x1A, 0}
{0x18, 0xFF, 1}  {2}      label := 1, kick (completion -> 0x01B50FD0)
{0x18, 0xFF, 2}  {2}      label := 2, kick        <- never reached the RSX
```

Image 2 consumed every entry and DMA'd two FIFO batches: 80 bytes to 0x49E07000, then 32 bytes to
0x49E07040. It then wrote put = **0x8058** for EA 0x49E07058. Our walker read **zeros** at IO
0x8048. The game's offset math is `ea − begin + 0x1000`. That holds on real hardware, where
libgcm reserves the first 4KB of the IO region and the default buffer begins at IO 0x1000.
`cellGcmSetupContext` had put `begin` at IO 0.

Fix (`ps3recomp/libs/video/cellGcmSys.c`, matching RPCS3's `_cellGcmInitBody`):

- `begin = current = ioAddress + 0x1000`
- `end = ioAddress + 32KB − 4`
- control `put = get = 0x1000`, walker `get = 0x1000`

`GCM_LEGACY_CMDBUF=1` restores the old layout. `GCM_FIFO_SNAP` now only dumps passes that have
work (`get != put`).

## 4. Jump tables the old lift truncated

Next: `[ppu] unresolved indirect call -> 0x00ACAF2C` on HighGraphics. That address is case
0x88 of a GCC PIC switch in `func_00ACA890` (table 0x00ACAEA4). The old lift recovered **only the
first case**. It emitted the table as `/* TODO: .word */` lines, left the other case bodies as
unlabelled code, and routed everything else to `default: ps3_indirect_call`.

Survey of the whole lift: **705 switches, 41 truncated, 424 missing cases.**

`scripts/fix_jumptables.py work/recomp` patches them in place. The old lifter emits **exactly one
C line per PPC instruction**, so every line can be given an address. The count is checked against
the `loc_X:` labels and `ctx->cia = X;` lines. Across the lift only 3 anchors disagree, all in
hand-patched functions the fixer does not touch. For each truncated switch, the fixer:

- resolves every table-relative entry
- inserts `loc_X:` at the target line
- adds the `case`

It is idempotent. Re-run it after any re-generation of `work/recomp` (gitignored), together with
the clz guard (§1).

## 5. Two tasksets, one global

Once the Havok (image 7, taskset 0x477ED800) and Core.Res (image 1, tasksets 0x48BB….) task
families ran concurrently, logs showed `built SpursTasksetContext image=1 taskset=0x477ED800`.
Image 7 was then GETting misaligned EAs out of image 1's data. The dispatcher hands each new job
its taskset/task through `g_ydkj_real_taskset_ea/taskid`. Two writers race on that pair:

- `cellSpursCreateTask` (PPU threads)
- the "start next task after exit" path (SPU worker threads)

Those globals also serve as "a real taskset exists" flags elsewhere, so they stay. Both dispatch
sites now also set a **thread-local handoff** (`spu_workload_set_dispatch_task`). The job capture
in `spu_workload_dispatch_async` prefers that handoff and consumes it. After the change, every
pairing is consistent and there are 0 rejected DMAs.

## 6. CELL_FS_O_* are octal

The resource cache misses on its first lookup (as in RPCS3), decompresses, and then reopens
`/dev_hdd1/cache/xx/<hash>` with flags **0x41** to write the result back. That failed with
ENOENT. The SDK constants are octal: `CELL_FS_O_CREAT = 000100` = 0x40, `EXCL = 000200`,
`TRUNC = 001000`, `APPEND = 002000`. `ppu_fs.cpp` had CREAT=0x200 and `sys_fs.h` had
CREAT=0x100, so a create-open never created anything. `libs/filesystem/cellFs.h` was already
correct. Now the cache files are written (sizes match RPCS3's `ftruncate`), and a second boot
gets cache hits.

## 7. The default buffer is a ring of 32KB fragments

After ~19 draws HighGraphics stalled in `func_009F94A8` at 0x009F9604.
It was again waiting for label 0xFF (wanted 0x26, label at 5). The SPU had produced commands up to
0x49E10B24, but put = get = 0x8000 and the context's `current` was 0x49E08000.

libgcm's default buffer is `cmdSize / 32KB` fragments. `begin`/`end` bracket only the current
one. DeS lets an SPU append a pushbuffer in the following fragments, then sets `context->current`
to where the SPU stopped. The next reserve then overflows `end`, and the SDK callback must:

1. flush `put = current`
2. JUMP to the next fragment
3. move `begin`/`current`/`end` there
4. wait until `get` is outside it

Our recycle rewound to `begin` and set `put = begin`, so the SPU's commands never ran.
`gcm_fifo_next_fragment` implements the SDK/RPCS3 behaviour for the real layout. With it the
label wait clears and the boot moves on to the Core.Res decompression stalls in
[doc 25](25-spurs-queue-lle-boot-assets-complete.md).

## 8. Things tried and parked

- **Full re-lift with the current lifter** (`work/recomp_v0140lift`, 94016 funcs). It fixes clz
  and jump tables at the source but changes other semantics:
  - 64-bit `addi`
  - `ctx->lr` set at call sites
  - 1-arg `DS_FRAME`

  It breaks the boot early: the `func_00B0AF28` singleton reads ASCII garbage. Parked until
  that is bisected.
- `SPU_INTERP_IMG=<image>:<entry>` runs one image under the SPU interpreter. It confirmed the
  lifted image 2 behaves identically, so the lift was never the image-2 problem.

## 9. New diagnostics

| Env | What it does |
|-----|--------------|
| `USLEEP_SHADOW=<tid>[:n]` | after a thread's n-th usleep, dump its guest shadow stack and r24–r31 with the memory they point at (the poll loop's registers survive the sleep helper) |
| `PS3_POLLTOP=<s>` | now keys on `cia` when `lr` is 0, which names the real poll site in this lift |
| `SPU_HALT_LS=<ls>,<len>` / `SPU_HALT_EA=<ea>,<len>` | dump LS / guest memory on an SPU halt |
| `SPU_INTERP_IMG=<img>:<entry>` | run one SPU image under the interpreter |
| `SPU_DUMP_GET=<ea>` | dumps the first 16 fetches (rings are re-read) |
| `GCM_LEGACY_CMDBUF=1` | old IO-0 command-buffer layout |
| `[fs] open FAIL` | now prints flags and errno |
