# 25 — Taskset lifetimes, SPU thread queues, and cellSpursQueue on real libsre: the boot loads every asset

**2026-10-13.** After [doc 24](24-highgraphics-init-and-rsx-fifo.md), the boot got about 55 files
into RPCS3's 149-entry open sequence and then stopped. This doc covers the links between there
and the end of the boot asset list. Now **2 runs in 3** open the same 143 `/dev_bdvd` files as
RPCS3, in the same order, and end on `facegen/facegen.fgbnd`. That is RPCS3's last open before it
sits on the auto-save notice. The other 5 RPCS3 entries are 4 `.sdat` files that we open through
`cellFsSdataOpen`, which is logged differently, plus `sound/ds_bgm_other.mowb`.

| # | Symptom | Root cause | Fix |
|---|---------|------------|-----|
| 1 | `_cellSyncLFQueueCompletePushPointer` assertion; tasks of a dead taskset still running | `cellSpursShutdownTaskset` / `JoinTaskset` were no-ops, and a re-created taskset at the same EA inherited live tasks | real shutdown + join with a per-taskset generation |
| 2 | image 1 `stop 0` loops, decompressor `Join` timeouts | the round-notify heuristic set the SPU→PPU flag as soon as any task parked in WAIT_SIGNAL | run with `SPURS_NO_ROUND_NOTIFY=1` |
| 3 | MO synth SPU (`_synth2`) re-issues a syscall in a tight loop (10M log lines) | `sys_spu_thread_bind_queue` unregistered; stop codes 0x110/0x111 not serviced; the lifter ended functions at those stops | bind/unbind handlers, `receive_event` service, resumable stops in the lifter |
| 4 | HighGraphics spins forever after `default.rumblebnd`; boot stops at 54 opens | `cellSpursQueuePopBody` (0x91066667) had no HLE at all, and the rest of the `cellSpursQueue*` HLE is a guessed layout | route the family to real libsre |
| 5 | SPURS ports climb 16, 17, … 36 and are never released | syscall 252 (`disconnect_event_all_threads`) unregistered | handler, including group 0 (HLE SPURS) |

---

## 1. Taskset shutdown and join are real

Core.Res.Decompressor creates one taskset of five image-1 tasks per DCX file. It shuts the
previous taskset down and joins it before creating the next one, often **at the same EA**. Both
calls were no-ops, so:

- the old tasks kept running, parked in WAIT_SIGNAL
- they woke up on the new taskset's signals and popped its LFQueue

`runtime/spu/spu_workload.c` now keeps a lifetime table per taskset EA: a generation counter, a
live-job count, the task-id mask and host thread ids. Each async job snapshots the generation when
it starts.

- `spu_taskset_shutdown(ea)` bumps the generation and wakes every parked task.
- A task whose generation is stale leaves WAIT_SIGNAL with -1. The channel handler stops it
  (`SPU_STATUS_STOPPED_BY_STOP`) without reporting a completion.
- `spu_taskset_join(ea, ms)` waits for live == 0. It logs any job still live after 100 ms.
- `cellSpursCreateTaskset` shuts down any stale tasks at the EA before it clears the struct.

## 2. The round-notify heuristic corrupts decompression

`spurs_ef_notify_taskset_round` (cellSpurs.c) sets a taskset's SPU→PPU event flag whenever one of
its tasks parks in WAIT_SIGNAL while a PPU thread waits on the flag. It was added for the MO mixer
flag. For the decompressor it fires mid-file: the PPU believes the blocks are done, recycles the
buffers, and the tasks then inflate garbage.

- That garbage is the image-1 `stop 0` from the inflate error path: func_000044C8/00004658 →
  func_00004690.
- With the heuristic **off**: 0 stop-0 loops and 0 join timeouts.
- Every cached DCX validates with `work/tmp/dcx_check.py`.

Run with `SPURS_NO_ROUND_NOTIFY=1`. The MO streaming flag (0x01CE7800) still relies on it in some
runs; see §6.

## 3. SPU thread event queues (the MO synth)

The MO synth runs as a raw lv2 SPU thread (image 11, `_synth2`). It reads commands with
`sys_spu_thread_receive_event`:

1. `stop 0x110`, with the spuq number in OutMbox
2. reply in InMbox: `{CELL_OK, data1, data2, data3}`

Three pieces were missing:

- **Handlers.** `sys_spu_thread_bind_queue` / `unbind_queue` (193/194) were stubs. They now record
  `spuq → queue` per SPU thread (`lv2_register.c`).
- **Servicing the stop.** `spu_stop` now calls `spu_service_thread_syscall` first, which handles:
  - 0x100 `group_yield`
  - 0x110 `receive_event` (blocks on the bound queue through `sys_event_queue_receive_host`)
  - 0x111 `tryreceive`
- **The lifter.** It treated those stop codes as function terminators, so the code after them was
  never lifted. `RESUMABLE_STOP_CODES = (0x100, 0x110, 0x111)` in
  `tools/find_spu_functions.py` and `tools/spu_lifter.py`. The lifter now emits
  `spu_stop(ctx); if (ctx->status != SPU_STATUS_RUNNING) return;` and continues. Re-lifting
  changed only images 3–6 and 11.

The synth's log dropped from 10M lines to 72K.

## 4. cellSpursQueuePopBody was never implemented

With 1–3 in place the boot stopped after `menu/mapshot.tpf.dcx` and `other/default.rumblebnd`.
The Core.Res threads were idle, and so was the decompressor: it had pushed and retired all 49
blocks of mapshot. The LFQueue at 0x48BBC100 read `push = pop = 0x11`, which is 49 mod 32, with
no waiter. MAIN was parked in a generic event wait (func_009E0148).

The watchdog's guest-thread lines now include `cia` and `r11`. They showed **HighGraphics (tid 4)
was not in a syscall at all**: it was running func_014C3958 → func_000347C0. Decompiled,
func_014C3958 drains a SPURS queue:

```c
while (counter < target)
    while (PopBody(q, &ev, 0, /*blocking*/1) == 0) { ...; if (target <= counter) break; }
do { r = PopBody(q, buf, 0, /*blocking*/0); } while (r == 0);   // drain until empty
```

`func_016B9E84` is the import stub for **NID 0x91066667 = `cellSpursQueuePopBody`**. Nothing
implemented it, so the unresolved default returned 0 forever and the drain loop never ended. The
queue is 0x47FC5080: SPU→PPU, 250 × 16 bytes, filled by the HighGraphics SPU tasks.

RPCS3 only stubs this function because it runs libsre itself. Our `_cellSpursQueueInitialize`
HLE, by its own comments, guesses the layout. Under libsre the queue is a `CellSyncLFQueue` that
the SPU side edits with GETLLAR/PUTLLC. So the fix routes the whole family to the loaded libsre,
like the LFQueue family before it:

```
LIBSRE_LLE=1 LIBSRE_LLE_ONLY=011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667
```

| NID | Function |
|-----|----------|
| 011EE38B | `_cellSpursLFQueueInitialize` |
| 1656D49F | `cellSpursLFQueueAttachLv2EventQueue` |
| 8A85674D | `_cellSpursLFQueuePushBody` |
| 73E06F91 | `cellSpursLFQueueDetachLv2EventQueue` (new) |
| 082BFB09 | `_cellSpursQueueInitialize` (new) |
| E5443BE7 | `cellSpursQueueAttachLv2EventQueue` (new) |
| 039D70B7 | `cellSpursQueueDetachLv2EventQueue` (new) |
| 91066667 | `cellSpursQueuePopBody` (new) |

Result: 143 opens and a live frame loop after facegen. HighGraphics keeps creating its image 3/4
tasks and Havok its image-7 tasks (task ids past 3550). DsSaveLoadMan's
`cellGameDataCheckCreate2` runs and its callback returns OK_CANCEL, as in RPCS3.

**Correction to earlier notes:** the RPCS3 log at `rpcs3/log/RPCS3.log` **never calls
`cellSaveDataAutoLoad2`**. After facegen (0:01:35) it runs frames until the window is closed
(0:01:53). The auto-save notice is drawn by the game (menu ESD), not by a sysutil dialog. We are
now at the same point in the boot.

## 5. Ports are released (syscall 252)

Real libsre's `cellSpursLFQueueDetachLv2EventQueue` calls
`sys_spu_thread_group_disconnect_event_all_threads` (252). It was not registered. Now it is, and
for group 0 it clears the HLE SPURS port table (`spurs_hle_disconnect_port`). RPCS3's own syscall
statistics for this boot show 234 connects against 216 disconnects. We previously did none.

## 6. Where it stands

- **Rendering is the new frontier.** Over 7000 draws reach the D3D12 backend, mostly the 20-vertex
  quads, but every dumped frame is pure black:
  - `CELLMARK_DUMP=40 CELLMARK_DUMP_EVERY=60 CELLMARK_DUMP_MINDRAWS=1 CELLMARK_DUMP_DIR=...`
  - After facegen the FIFO walker hits `JUMP to unmapped IO 0x1EC70000 (word 0x3EC70000)` and
    `resync (stuck-get)`. 0x3EC70000 looks like float data, so the walker is probably
    desynchronised from the SPU-built pushbuffer rather than seeing a real jump. The IO region
    is only `ioSize = 0x700000` at 0x49E00000, and the game never calls `cellGcmMapMainMemory`.
- **One run in 3 stalls early** (10–30 opens). In one of them MOStreamingThread waited on the MO
  flag 0x01CE7800 (pattern 0x1) after 17 wakes, with round-notify off, while MAIN sat on its
  Havok cond barrier. That needs a real completion path for the MO flag rather than the heuristic.
- `ds_bgm_other.mowb` (streamed BGM) is never opened; it is MOStreamingThread's file.

## 7. New diagnostics

| What | How |
|------|-----|
| Live memory dumps | `PS3_DEBUG=<file>`: write `mem <hex> [len]`, `threads`, `find32`, `poke32`, … to the file, read `<file>.out` |
| Guest syscall state per thread | watchdog `guest-tid` lines now print `cia`, `r11` (syscall number), `r4`, `r5` |
| SPU user events | `SPU_EVT_TRACE=1` logs every SPU `send_event`: image, port, queue, data, result |
| Taskset joins | `[spu_workload] join taskset …: N live (ids=…, parked, host tid)` after 100 ms |
| NULL indirect calls | the bctr-to-NULL report now prints `cia` |
| Ghidra on undiscovered functions | `tools/ghidra/dump_one.java` now disassembles and creates the function if analysis never found it (vtable/OPD-only targets) |
