# 23 — Upstream v0.14.0 merge, and the boot assets stream

**2026-10-13.** Merged upstream ps3recomp **v0.10.0 → v0.14.0** (+680 commits). That moved the
boot to a new gate, `Core.Res.Decompressor`. Getting past it took real Sony libsre code for one
API family. With that in place, **every boot asset streams and decompresses on SPU** for the first
time.

Fork branch: `ps3recomp` **`upstream-v0140`** (merge `724fec1`, fixes `1882e91`).

---

## 1. The merge

- **Conflicts:** 11 files, 14 hunks. The v0.10.0 merge had 107 files. Upstream keeps absorbing
  our fork's work. It had **independently fixed our SPURS deadlock**: `cellSpursCreateTask` now
  writes the task id back big-endian, and as the *slot index* rather than a global counter. It
  had also adopted our keyboard pad fallback word for word.
- **Policy, as before:** where upstream re-solved the same problem, upstream wins. Where only
  one side has a feature, keep both. Kept on our side:
  - the SPURS task exit-code container write (upstream still has none)
  - the `SPU_DONE_WORD` and task-resume diagnostics
  - the `SPU_QLOG` globals
- **The compiler changed.** Upstream's CMake now *refuses* `cl.exe` ("use clang-cl"), and its
  headers use GNU attributes. The runtime build dir was reconfigured with clang-cl. The old dir
  is kept as `ps3recomp/build-msvc-old`.
- **`fw_spu/` is no longer tracked upstream** (`*.prx` is ignored, per its firmware policy). The
  libsre LLE needs `fw_spu/libsre.prx`, so it is restored locally as an ignored directory from
  `work/fw_spu_backup/`. **Redo this after every merge.**
- **Two post-merge build fixes:**
  - a duplicated `cellSpursTasksetAttributeSetTasksetSize`
  - our task-resume path calling `spu_run_lifted_job_abi` with the old 6-argument signature

**Result with the existing lifted code:** still 13/14 and 0 crashes. Underneath, the old
`0x01CE7800` event-flag deadlock (docs/21 §12) is gone; that flag now cycles thousands of times.

## 2. Regenerate the NID table after every merge

`work/recomp/hle_nid_table.cpp` is generated and gitignored. Ours dated from v0.10.0, so
**198 handlers that upstream implements were never registered.** Each of them silently faked
`CELL_OK`, including both LFQueue setup calls, `cellGcmGetReport`, `_sys_heap_*` and the NP
init calls.

```
python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
```

The generator then failed to link. It scanned `sceNpCommerce.c`, which CMake excludes as a
duplicate, and emitted references to symbols nothing defines. It now skips that file (fork fix).

With the full table the boot creates **22 threads (was 18)**. The four new ones are
`_cellsurMixerMain`, `_sys_MixerChStripMain`, `_sys_mixerSurBusReq` and
`NexusRevolution Surveillance`. RPCS3 creates all four, and docs/21 had listed them as missing
from our boot. The result is deterministic: `repro.sh 4 40` gives stdev 0, with 0 crashes.

## 3. The new gate: `Core.Res.Decompressor` and the SPURS LFQueue

The game now **reads real asset files**: `/dev_bdvd/.../shader/*.shaderbnd.dcx`, `systex_tex.tpf.dcx`,
the sound banks and `movtaeds.movtae`. It hands the compressed chunks to SPU decompression tasks
(image 1, five tasks per taskset), and then blocks on an event flag waiting for them to finish.

Those tasks are fed through a **SPURS lock-free queue**:

| NID | function | status after the merge |
|---|---|---|
| `0x011EE38B` | `_cellSpursLFQueueInitialize` | upstream HLE (partly guessed layout) |
| `0x1656D49F` | `cellSpursLFQueueAttachLv2EventQueue` | upstream HLE (no-op) |
| `0x8A85674D` | `_cellSpursLFQueuePushBody` | **no implementation anywhere** |

Every push faked success and went nowhere, so the SPU tasks waited forever and so did the
decompressor.

### Using Sony's code for this one family

We already lift the real `libsre.prx` (docs/22), and it exports all three functions. Guessing the
push protocol (slot state machine, `push1/push2/push3`, the signal to the consumer task) would
have to match the SPU-side pop exactly. Sony's code matches it by construction.

**`LIBSRE_LLE_ONLY=<nid,...>`** (new, `ppu_hle.cpp`) is the inverse of `YDKJ_FORCE_HLE`: only
the listed NIDs go to the loaded PRX, and everything else stays on the working HLE path.

Sony's code then reported exactly what was wrong:

```
_cellSpursLFQueueInitialize          -> 0
cellSpursLFQueueAttachLv2EventQueue  -> 0x80010005 (ESRCH)
  [sc] 251(group=0x0, queue=0x11, req=0xFFFFFFFFFFFF0000, ...) -> ESRCH
_cellSpursLFQueuePushBody            -> 0x8041090F (TASK_ERROR_STAT, x every push)
```

Syscall 251 is `sys_spu_thread_group_connect_event_all_threads`. libsre attaches its queue one
level *below* `cellSpursAttachLv2EventQueue`, on `CellSpurs.spuTG`, and the HLE SPURS never
creates an lv2 SPU thread group, so that field is 0.

**Fix:** group 0 is never a real lv2 group. Syscall 251 on group 0 now binds the lowest free
requested port in the HLE SPURS port table (`spurs_hle_connect_port_mask`), the same table HLE
`cellSpursAttachLv2EventQueue` fills. SPU user events on that port therefore reach the queue.

### Result

Run with `LIBSRE_LLE=1 LIBSRE_LLE_ONLY=011EE38B,1656D49F,8A85674D`:

| | HLE only | LFQueue on libsre |
|---|---|---|
| LFQueue attach | faked | **6/6 OK** |
| `PushBody` | unresolved ×29 | **81/81 OK** |
| decompressor `BLOCKED` | 24 | **0** |
| assets opened / fully read | 10 / partial (393 KB) | **10 / all (826 KB)** |
| image-1 task run time per round | 0 ms (never fed) | **16–250 ms (real work)** |

**All ten boot assets now stream and decompress:**

- `magicorchestra2.mgs`, `rpcsetting.rpc`, `demonsmixer.momd`
- `systex_tex.tpf.dcx`, `movtaeds.movtae`
- `dbgfont`, `ds_filter`, `ds_flver`, `ds_menu` and `ds_sfx` `.shaderbnd.dcx`

docs/21 §19 had diagnosed the whole idle picture as *"nothing is loaded"*. That is no longer
true.

## 4. Corrections to earlier notes

- **The `sem=2` waiter is MOMixerThread, not SLSession.** SLSession waits on **sem 3** inside
  `func_00B0A5A8`. As docs/21 §14 found, sem 3 is a work-queue signal. The gate is the engine
  *enqueuing a save/load request*, not the post itself.
- **`PS3_SCTRACE` logs a syscall when it returns**, so a call that blocks forever never appears.
  SLSession looked as if it made "zero syscalls" for that reason. Use the watchdog's host stack
  (`func_00B0A5A8` ← `func_00B0A428` ← `func_00EC1090`) to see a blocked call.
- The 1 GB RPCS3 Trace log has been overwritten by a later Notice-level run, so a new one is
  needed for any further sync-object diff.

## 5. Where it stands, and next

The asset pipeline works. The engine has moved forward: MAIN is now parked on object `0x23`,
not on docs/21's `cond 30`. The dialog has not appeared yet.

1. **MAIN's new wait (`0x23`).** Use `YDKJ_SYNCTRACE`/`SEMTID` to find out whether it is a cond
   or a mutex, and who should signal it. This follows the same method as cond 13 (§20) and
   cond 30.
2. **Graphics.** With shaders now loaded, re-check whether HighGraphics submits a non-empty FIFO
   (`put != 0`) and whether label 255 is written. docs/21 §18–19 found an empty ring because
   nothing was loaded.
3. **Widen `LIBSRE_LLE_ONLY` one family at a time.** The same pattern (Sony code serving one API
   family next to the HLE) works now. Candidates are the families whose PM bookkeeping we keep
   hand-writing: event flags and task completion.
4. **References** from other ps3recomp ports (survey 2026-10-13):
   - Killzone 3's gist R1–R6, on Havok SPU tasksets
   - NoahBPeterson's inFamous full-LLE branch, whose commits `70c456e6` and `69031106` model the
     SPU kernel's `selectWorkload` and workload `0x20`. That is our docs/22 "70 instructions,
     workload 32" symptom.
   - Yakuza: Dead Souls' native SPURS rewrite

   No FromSoftware/Dantelion title has a ps3recomp port.

### Reproduce

```
python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
tools/des/build.sh all
tools/des/desrun.sh 40 run.txt LIBSRE_LLE=1 LIBSRE_LLE_ONLY=011EE38B,1656D49F,8A85674D
```
