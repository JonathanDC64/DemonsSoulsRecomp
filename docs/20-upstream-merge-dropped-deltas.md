# Upstream merge (sp00nznet v0.10.0) — dropped deltas & new frontier

2026-09-05. Merged upstream `sp00nznet/master` (813 commits since our 2026-05-31
merge base) into the fork. 128 of our 261 commits were already absorbed upstream
by patch-id; 130 were uniquely ours. 107 files conflicted (81 real sources,
~500 hunks).

## Result

| | pre-merge | post-merge |
|---|---|---|
| milestones | 13/14 | 13/14 |
| crash markers | 36 | **0** |
| threads created | 17 | **18** |
| determinism (6 runs) | chronic non-determinism | **every observable stdev 0.0** |

New threads upstream brings up that we never did: `(Dantelion2)PPURuntimeService`,
`(Dantelion2)SPURuntimeService`, `spu_printf_handler`, `NexusRevolution Event`.

## New frontier (replaces "image=2 loops on a garbage command queue")

A two-sided SPURS deadlock, now precisely named by the runtime's own logging:

- PPU blocks in `cellSpursEventFlagWait` on `flagEA=0x01CE7800`
  (pattern 0x0001, mode=AND, direction=1 = SPU→PPU), taskset `0x01CE8300`.
- SPU **task 1 of that same taskset** is parked in `WAIT_SIGNAL`, waiting for a
  signal that never arrives.

So each side waits for the other. Upstream implements REAL `WAIT_SIGNAL`
semantics (RPCS3 `spursTasksetProcessSyscall`) and a working
`cellSpursSendSignal`, so the missing link is whoever should issue the
signal / set the flag. SPU images now all dispatch and return
(image=7 x16 rc=0, image=2 x3, image=12 x3) — image=2 completing is itself
new; it previously never finished.

Knobs tried, neither reaches the dialog: `SPURS_EF_FORCE=1`,
`SPURS_EF_OBJ_DELIVER=1`.

## Our deltas dropped in favour of upstream (re-apply only if boot regresses)
- 92c3f5f gcm: log GetLabelAddress index (diagnostic)
- f406890 gcm: vblank ticker advances GCM labels (probe)
- 7a977e6 gcm/vblank: post libgcm frame semaphore each vblank (probe)
  -> upstream now has a real flip tick + FIFO recycle; these may be obsolete.
- 784e231 rsx NV4097 semaphore-release label write -> SUPERSEDED by upstream
  (upstream also handles the back-end byte-swap quirk + NV406E).
- cellSpurs.h: our `argValue[4]` copy-at-init dropped for upstream's `argument_ea`.
  RISK: our comment said the init's argument ptr is a CALLER STACK buffer that may not
  survive to CreateTask. Upstream stores the EA instead of copying. If DeS task args come
  back garbage, re-introduce the copy. (ours: b5d2c2b, 8470d6e)

## Higher-risk drops (upstream design taken; re-add if the boot regresses)
- Deferred SPURS scheduling: `spu_pending_flush` (14d17d7, docs/13) + `spu_workload_dispatch_task`
  (task-START ABI, r3=16-byte arg). Upstream instead dispatches async on host threads.
  Declarations survive in spu_workload.h but have NO definition now -- nothing calls them.
- `SPURS_DONE_EVQ` completion-event posting (2ca55a6 / ce3b9f0, Havok evq-5 bridge).
- Multi-task taskset registry (6b5ea49). Upstream has its own g_ydkj_real_taskset_ea bridge.
- SPU_EVENT_LR delivery (47cf59d), SPU_QLOG / SPU_RESV_LOG / SPU_DISPATCH_LOG probes.
- POLL-spin breaker: RE-ADDED but DEFAULT OFF (SPU_POLL_BREAK_AFTER=500 to enable).
- ppu_loader.cpp diagnostics (HOTMAP, WWATCH/RWATCH/HEXWATCH, shadow-stack backing store,
  YDKJ_ICTRACE/THREADLOG, cia stamping, g_active_ctx dangling fix 0a6fe66).
  Upstream has its own richer set (LBP_WV, PS3_HLE_ARGS, PS3_WAIT_OBJ, bounded RVA mapper).
  NOTE: --shadow-stack survives in the LIFTER but its runtime backing store is gone,
  so a --shadow-stack lift would not link until ds_dump_shadow/g_ds_sp are restored.
