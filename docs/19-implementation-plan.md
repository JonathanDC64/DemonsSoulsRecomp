# 19 — Full implementation plan (synthesized from the docs/18 requirements crunch)

*2026-07-05. Synthesizes the four crunched requirement specs (A task-completion
contract, B HighGraphics cond-2, C SLSession→dialog, D nondeterminism/verification)
+ docs/16 (roadmap) + docs/17 (SPURS model). This is the ordered, dependency-aware,
execution-ready plan to reach the boot dialog and build the reusable SPURS runtime.*

## THE CONVERGENCE (all four domains agree)

There is **one root**: **SPU→PPU SPURS task-completion propagation**. Everything
else is downstream and cascades once it's fixed:

```
[ROOT] SPU leaf/task completion is not propagated to the PPU waiter
  → Core.Res SPU-decompression completion never signals  (A: task-body cellSpursEventFlagSet; we force-satisfy it with garbage)
    → resource never ready → MAIN stalls on cond-29        (B/C: MAIN's barrier)
      → MAIN never runs the title/menu + DsSaveLoadMan submit code
        → SLSession producer func_00EC0F58 never runs        (C: needs item+sema together)
        → HighGraphics frame-message producer starves         (B: cond-2 DLEvent stops being Set)
          → cond-2 wait never re-signals → frames stop at ~20 boot-logo draws
          → cellSaveDataAutoLoad2 never called (14a)
```

Key corrections the crunch produced:
- **A:** RPCS3 has NO cellSpurs join/wait-task API (all UNIMPLEMENTED). The creator's
  wait is cleared by a signal the **task body emits** — a `CellSpursEventFlag`
  (`sys_event_port_send`→creator's `sys_event_queue_receive`). `wklCurrentContention`
  is only the kernel's SPU-occupancy counter, **NOT** the completion signal (docs/17 §A overstated it).
- **B:** cond-2 is a Dantelion **DLEvent** (N=1 binary event, `func_009E0370` wait /
  `func_009E0148` set), a **downstream symptom**. Force-signaling it renders degenerate
  frames. HighGraphics's created tasks = **SPU image=2** (ELF 0x0177C200).
- **C:** SLSession is a producer/consumer queue **downstream of MAIN cond-29**, not
  independently unblockable; needs the queue item AND the sema post together (why
  POKE_SEMA=3 failed). Milestone 14a=call reached (needs producer), 14b=overlay (Phase-4 render).
- **D:** the boot is nondeterministic (SPU host-thread races); use the repro harness
  to judge fixes by distribution, not one run.

## ALREADY DONE (this session — don't redo)
- Event-helper host thread = persistent consumer (commit 5df9b25) — the data0={0,2,3}
  substrate. NOTE: the RESOURCE completion is a **cellSpursEventFlag** (A-R4.1),
  a SEPARATE mechanism from the event-helper's data0 path.
- r4-r7 event-receive ABI fix (507ad9c). CPU fix (7df837e).
- Verification harness repro.sh/repro_report.py + monotonic milestones (076072d).
- docs/17 SPURS model; tools/des/spurs_trace.py analyzer.

---

## ORDERED PHASES

### Phase 0 — Verification foundation ✅ DONE (domain D)
`tools/des/repro.sh N` + `repro_report.py` (+ `--baseline/--candidate`). **Every
task below is verified by a distribution shift, never one run.** Optional D-R5
`SPU_SERIAL=1` leaf-serialization knob if variance blocks a specific measurement.

### Phase 1 — Resolve the request-vs-completion ambiguity (DIAGNOSTIC, ~0.5d)
Two framings must be reconciled before Phase 2: the Core.Res pipeline is "starved"
(no request enqueued) vs "stuck" (request enqueued, SPU-decompression completion
missing). Determine which:
- Instrument the cond-29 object (resource ring-buffer 0x01ADD640) enqueue + the
  Core.Res.TaskManager dequeue: does MAIN push a request BEFORE blocking on cond-29,
  and does TaskManager see it? (Watch 0x01ADD640 writes + TaskManager's queue count.)
- Trace what MAIN's cond-29 actually waits for (A-R8: match MAIN's blocked queue id to
  a `CellSpursEventFlag->eventQueueId` created near the taskset, vs the resource ring).
**Output:** confirms whether cond-29 = a SPURS event-flag wait (→ Phase 2 directly) or a
resource-pipeline barrier gated on the decompression leaf (→ Phase 2 + Phase 3).
Files: instrument sys_cond.c (cond-29 probe exists), sys_event.c; analyze with spurs_trace.py.

### Phase 1 RESULT (2026-07-05): STARVED, not stuck — refocus P2 on MAIN cond-29
Ran 6× with SPURS_LV2THREADS=1 + the ef-wait probe: `cellSpursEventFlagWait` is
**never reached** (0 calls) and Core.Res.Decompressor is dormant `lastHLE=-(#0)`
on cond-5 in all 6 runs. So the Decompressor's event-flag is NOT the active
gate — it's never dispatched a first request (pipeline STARVED, confirming the
Core.Res-dormancy study). **The active root is MAIN's cond-29 producer**, upstream
of the title/menu code that feeds Core.Res. So P2 targets **what signals MAIN's
cond-29** (object 0x01ADD640) directly, NOT the Decompressor eventflag (that
becomes relevant only after MAIN advances + the pipeline starts). The force-satisfy
shim in cellSpursEventFlagWait is currently moot (unreached under SPURS_LV2THREADS).

### Phase 2 — SPU task-completion → MAIN cond-29 signal (THE ROOT, ~2–4d) [refocused by P1]
Make each SPU leaf's completion reproduce the **task-body completion signal** the PPU
waiter expects (A-R4.1/R7.2), + the scheduler side-effects (A-R6).
- **2a. Map each waiter → its event-flag** (A-R8): for Core.Res.Decompressor
  (cellSpursEventFlagWait, cond-5) and any HighGraphics/image=2 waiter, find the
  `CellSpursEventFlag` (eventPortId/eventQueueId, created in `_cellSpursEventFlagInitialize`,
  cellSpurs.cpp:3258) it blocks on. Files: libs/spurs/cellSpurs.c (EventFlag impl),
  runtime/spu/spu_workload.c (leaf→taskset/wid mapping).
- **2b. Replace the force-satisfy shim with a REAL set** (libs/spurs/cellSpurs.c
  cellSpursEventFlagWait/Set): on the SPU decompression leaf completion, perform
  `cellSpursEventFlagSet(flag, bits)` → its `sys_event_port_send` (cellSpurs.cpp:3415)
  wakes the waiter with real bits. Remove the ~200ms timeout force-satisfy.
- **2c. Reproduce the taskset side-effects** (A-R6) on leaf start/finish in
  spu_workload.c: taskset bitsets running/enabled (offsets 0x00/0x30), readyCount,
  wklCurrentContention[wid] (spurs+0x20+wid) — for scheduler coherence (NOT the unblock).
- **2d. Keep the event-helper (5df9b25) for data0=0 workload-shutdown + data0=3
  callbacks** — those remain the mechanism for shutdown barriers, not the resource path.
**Verify (D):** `repro.sh` shows Core.Res.Decompressor's cond-5 wait clears with real
data (lastHLE advances past cellSpursEventFlagWait), FileLoader reads (>0 fs reads),
crash distribution unchanged-or-better.
**Risk:** needs real decompressed bytes → Phase 3. If the waiter is a lwcond/cellSync
not an event-flag, use B-R5's `func_009E0148`-style set on that object instead.

### Phase 3 — Real .dcx decompression (~1–2d, only if 2 needs real bytes)
If the resource consumer faults on garbage (it will, per the panic history), do
host-side decompression: parse DCX (deflate/EDGE-zlib), inflate the .dcx payload the
FileLoader read, write the decompressed bytes where the pipeline expects. Add zlib
(not currently linked). Files: new libs/codec/dcx.c; wire into the FileLoader-read /
Decompressor path. Verify: decompressed shader/texture bytes are valid; no
"no runnable target" panics from the display work-queue.

### Phase 4 — Cascade to the dialog (14a) (~0.5–1d, mostly free)
With Phase 2(+3), MAIN clears cond-29 → runs title/menu → DsSaveLoadMan submit
(func_00EA4210…) → SLSession producer func_00EC0F58 runs (item+sema, C-R1/R2) →
worker func_00EC1090 → func_00EB2D58 → **cellSaveDataAutoLoad2 (cia 0x00EB3AAC) = 14a**.
Core.Res pipeline cascades for free (it was starved, not broken). Verify: milestone
distribution reaches 14 (dialog CALL) in repro.sh; funcStat fires.
**Contingency (if MAIN can't be cleared yet):** C-R5 host-side synthetic
`func_00EC0F58` shim to reach 14a in isolation — fragile, for milestone proof only.

### Phase 5 — Render pipeline / visible overlay (14b) (~1w+, docs/16 Phase 4)
Frame loop iterates (cond-2 re-signals via func_009E0148 as MAIN submits frames);
RSX: write GCM label 255 (0x03000FF0) on flip (rsx_commands.c:270 path), real
geometry/textures, shader translation, the system-overlay draw for the savedata
dialog. This is the "pixels on screen" milestone; large, parallelizable after Phase 4.

### Phase 6 — Nondeterminism hardening (optional, D-R5)
If races block progress, add `SPU_SERIAL=1` leaf-serialization (spawn+join per leaf,
persistent tasks stay threaded) — env-gated, validated via repro.sh distribution.

---

## DEPENDENCY GRAPH + PARALLELISM
```
P0 (done) ─┬─ P1 (diag) ── P2 (root) ──┬── P4 (dialog 14a) ── P5 (overlay 14b)
           │                            └── P3 (decompress, if needed) ┘
           └─ P6 (nondeterminism, anytime, parallel)
```
- **Critical path:** P1 → P2 → P4. P3 parallel-ish with P2. P5 after P4. P6 anytime.
- **P2 is the crux** (~2–4d) and the reusable SPURS-runtime core; the rest is
  comparatively mechanical once the completion propagates.

## PER-TASK VERIFICATION (all via repro.sh N + repro_report.py)
| Phase | Pass signal (distribution) |
|---|---|
| P1 | ring-0x01ADD640 enqueue seen; TaskManager queue-count >0 (routes P2) |
| P2 | Decompressor cond-5 clears (lastHLE past cellSpursEventFlagWait); fs reads >0 |
| P3 | decompressed bytes valid; display work-queue panics → 0 |
| P4 | milestone mode = 14; cellSaveDataAutoLoad2 logged; funcStat fires |
| P5 | draw_arrays >20 with non-degenerate verts; overlay visible |

## RISK REGISTER
- **R1** SPU-race nondeterminism muddies every measurement → mitigated by P0 harness (+P6).
- **R2** The waiter may be lwcond/cellSync not event-flag → P1 resolves; B-R5 fallback.
- **R3** Real decompression (P3) may reveal further format specifics → bounded (DCX is documented).
- **R4** Clean-room: reference RPCS3 for contracts only; reimplement (docs/05 firewall).
- **R5** image=2 leaf may need real geometry output for HighGraphics → P5, deferrable (14a first).

## KEY ADDRESSES (from the crunch, for the implementer)
- Event-flag: `_cellSpursEventFlagInitialize` cellSpurs.cpp:3258; Set→send :3415; wait :3589.
- cond-2 DLEvent: wait `func_009E0370`, set `func_009E0148`; HG frame loop `func_00D1DEE8`; HG tasks=SPU image=2 (ELF 0x0177C200, taskset 0x45B4B300).
- SLSession: producer `func_00EC0F58` (kick `func_00EC1060`→post 0x00b0a718), worker `func_00EC1090`, dequeue `func_00EBF868`, AutoLoad2 call `func_00EB2D58`@0x00EB3AAC; submit methods `func_00EA4210`…`func_00EA4ED0`.
- MAIN barrier: cond-29 object 0x01ADD640 (mutex 0x461); resource ring writers func_00AE47A0/func_00AE0BE8.
- Our code: libs/spurs/cellSpurs.c (EventFlag, CreateTask), libs/spurs/spurs_runtime.c (event-helper), runtime/spu/spu_workload.c (leaf dispatch/completion), libs/system/cellSaveData.c:887 (AutoLoad2).
