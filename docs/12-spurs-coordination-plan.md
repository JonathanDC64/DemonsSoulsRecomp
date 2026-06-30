# SPURS coordination — build plan (release `func_00A31158`)

Goal: produce the work the DLSpursManager job submits so the result pointer
`0x45A4B280` gets published to the global result slot `0x01B51000`, releasing the
`func_00A31158` boot wait — then advance past the SPURS gate (the scout showed input
init + more downstream). This is the one large remaining piece of the SPU phase.

Cross-references: RPCS3 `/c/dev/rpcs3-reference/.../Modules/cellSpurs.cpp`,
`cellSpursSpu.cpp` (SPU kernel; **contracts only — reimplement clean, no GPL copy**),
the public cellSpurs/cellSync SDK names, and our `docs/09 §15a–§15n`.

---

## 1. What we already know (established this session)

- **Blocker (docs/09 §15m):** `func_00A31158` spins until `[0x01B51000+0] == [+0x80]`,
  where `[+0x80] = 0x45A4B280` is an expected **result pointer** an external producer
  must publish to `[0x01B51000+0]`. (`r26 = *(r30-0x7FEC)`, a global "promise" slot.)
- **Producer = the DLSpursManager job** (a SPURS taskset + the coordinator task
  `spu_0001`, arg = **two PPU callbacks** `func_00A662C8`/`func_00A663A0` + TOC
  `0x019B5040`). `func_00A941F8` allocates the job (≈ the `0x45A4B280` it expects back).
- **Callbacks are cellSpurs queue workers:** `func_00A662C8` does `r3 += 8`, then calls
  libsre `func_016BB264(r3)` → libsre-internal NID `0x1BC200F4`, branching on
  `EAGAIN/EBUSY` (`0x80010005/9`). They need a valid queue handle in `r3` (`r3+8`).
- **Queue type:** `CellSpursLFQueue == CellSyncLFQueue` — and we already implement
  `cellSyncLFQueuePush/Pop` (libs/sync/cellSync.c). RPCS3 stubs the PPU
  `_cellSpursLFQueuePush/PopBody` (relies on its SPU kernel).
- **Coordinator loops** in `func_00A694` (empty queue → `func_00005120` event-wait); it
  reaches its `stop 0` (EXIT) only if its body returns.
- **Already built + tested this session (clean-room, reusable):**
  - stop-code preservation (`ctx->stop_code`; EXIT = `stop 0`).
  - Foundation A — the 3 task exit-code setup NIDs (container EA captured = `0x45B4DC80`).
  - Foundation B/C — `spu_spurs_task_write_exit_code` + passing unit test.
  - End-to-end wiring: container EA threads `cellSpursCreateTask` → dispatch →
    `spu_run_lifted_job_abi` → writes on `stop 0`.
  - musttail recursion fix, 256 MB stack, inline jump-table discovery, `shufb` test fix.
- **Note:** the exit-code path (above) is the task *lifecycle*; it is **separate** from
  the result-slot blocker. The blocker needs the *work produced*.

## 2. The architectural question (decides scope)

Who actually produces the result? Two candidate models — Phase 0 resolves which:

- **Model PPU-only:** the PPU pushes job(s) to an LFQueue and the two PPU callbacks
  (run by a DLSpursManager PPU worker/handler thread) pop + process them and publish
  `0x45A4B280` → `0x01B51000`. The SPU coordinator task is a trigger/keepalive. **If
  true, we may not need to emulate the SPU kernel at all** — implement the PPU side
  (LFQueue wrappers + run the callbacks with the right handle) = far smaller.
- **Model SPU-coordinated:** the SPU coordinator must run (consume/produce the queue,
  signal the PPU) for the round-trip. Needs taskset-PM emulation (event/signal delivery)
  so the coordinator progresses to publish/exit = the large path.

## 3. Phased plan

### Phase 0 — RE the data flow (no code; decides Model PPU-only vs SPU-coordinated)
1. Map the job struct `func_00A941F8` allocates and the stack struct built in
   `func_00A31158` (`{0x01B51000, alloc, …}`): confirm `0x45A4B280` == that alloc, and
   find where the result pointer is meant to be written (which agent, which field).
2. Trace the **callback `r3`**: who invokes `func_00A662C8`/`func_00A663A0` on the PPU
   (an indirect-call site / a DLSpursManager handler thread?), and what `r3` (job ctx
   with the LFQueue handle at +8) it passes. Grep PPU recomp for indirect dispatch of
   these OPDs; check the `PPURuntimeService`/handler threads.
3. Determine the LFQueue producer/consumer direction and whether the SPU coordinator is
   on the critical path (instrument: does anything ever push to the queue the callbacks
   poll? does the coordinator ever signal the PPU via mailbox `SPU_WrOutMbox`?).
4. Check whether libsre internal `0x1BC200F4` (+ siblings) is HLE-resolved or a stub
   (grep `hle_nid_table.cpp`); a stubbed libsre internal would break the callback path.
   Output: a decision (Model A vs B) + the exact handle/struct layout.

### Phase 1 — cellSpurs LFQueue PPU API (needed by both models; bounded)
5. Implement the PPU `cellSpursLFQueue*` entry points the game/libsre use, as thin
   wrappers over our existing `cellSync LFQueue` (push/pop/attach). Add any missing
   `cellSync LFQueue` ops. **Unit-test** each (extend the spu/tests harness or a PPU
   HLE test) — TDD, RPCS3 contracts for arg order.
6. If `0x1BC200F4` et al. are stubbed libsre internals on the callback path, implement
   them (clean) so `func_016BB264` returns real EAGAIN/empty vs an item.

### Phase 2 — produce the work (the chosen model)
- **If Model PPU-only:** at the right point (likely a DLSpursManager handler the boot
  already runs, or `cellSpursCreateTask` recognizing the pattern), run the two callbacks
  with the reconstructed `r3` (job ctx + LFQueue handle); they pop/process and publish
  the result. Add a `(code,toc)` PPU-call helper (the arg is `{entry,toc}` inline, not an
  OPD `ppu_guest_call` wants).
- **If Model SPU-coordinated:** emulate the minimal taskset PM — deliver the coordinator
  its start event so it leaves the `func_00005120` wait, runs its body, and either
  pushes the result (→ PPU callback consumes) or signals completion; hook `SPU_WrOutMbox`
  to drive the PPU callback run. Reference `cellSpursSpu.cpp` `spursTasksetDispatch` /
  request codes for the contract.

### Phase 3 — completion + integration
7. Ensure the result pointer `0x45A4B280` is published to `[0x01B51000]` (by the
   callbacks/coordinator, not a forced poke). The exit-code path (already built) fires in
   parallel when the coordinator hits `stop 0`.
8. **Integration test:** boot reaches past `func_00A31158` → input init (cellKb/Pad/Mouse)
   with NO Dantelion2 panic (the scout's panic was from faked data; real work = no panic).
   Then chase the next gate.

## 4. Risk / effort
- Phase 0 is RE-heavy but cheap and decisive — **do it first**; it can collapse the whole
  build to Model PPU-only (small) or confirm the large SPU-kernel path.
- Phase 1 is bounded + unit-testable (we already have the LFQueue core).
- Phase 2 is the variable: small (PPU-only) to large (SPU-kernel emulation).
- Throughout: clean-room (RPCS3 = contracts only); keep the scout's force-the-result as a
  gated bring-up fallback to probe further downstream while the real path is built.

## 5. Immediate next action
Phase 0, step 2 — **find who invokes the two PPU callbacks and with what `r3`.** That
single fact (PPU handler vs SPU-only) determines the entire scope below it.

---
## Phase 0 progress (started)

- **Callback ABI found:** `func_00A662C8(r3 = object)`; it does `r3 += 8` then the
  cellSpurs/cellSync LFQueue op on `object+8` — so `r3` is a manager/job object with its
  **LFQueue at +8**. At a real call site (cia 0x0005D3C4) `r3 = r28` (an object ptr).
- **Dual-use:** `func_00A662C8`/`func_00A663A0` are called DIRECTLY by game PPU code at
  many `0x0005xxxx` sites AND registered as the SPURS task's callbacks (the arg). So they
  are ordinary PPU queue-worker methods; the SPURS task just defers running them.
- **Implication for scope:** if a PPU-side DLSpursManager handler runs these callbacks
  (Model PPU-only), reconstructing `r3` (the manager object, LFQueue at +8) + our existing
  cellSync LFQueue may suffice — no SPU-kernel emulation. NEXT Phase-0 sub-steps: (a) find
  the object `r28` here (what manager owns the LFQueue) and whether the SPURS-arg path
  passes the same object; (b) confirm whether a PPU handler thread already runs these on
  schedule, or only the SPU coordinator would. That picks Model PPU-only vs SPU-coordinated.

---
## Phase 0 CONCLUSION — Model SPU-coordinated, via the LR atomic-wait pattern

Decisive evidence:
- `SPU_TASK_OFF` -> PPU spins forever => the SPU task IS on the critical path (NOT pure
  Model PPU-only).
- The SPU task makes **zero mailbox writes**; it talks only via MFC DMA + events
  (`SPU_WrEventMask`/`WrEventAck`/`RdEventStat`). So it DMA-publishes its result (it does
  not signal via mailbox) and is event-driven.
- It arms event mask **0x400 = SPU_EVENT_LR** (Lock-Line Reservation Lost, confirmed vs
  RPCS3 SPUThread.h) and waits on `RdEventStat`. Combined with its getllar/putllc atomic
  loop (func_00004694/func_00005120), this is the classic SPURS pattern: **getllar a
  control line, arm LR, sleep until another agent (the PPU) writes that line.**
- Our runtime has reservation state (resv_ea/resv_valid) but **never fires the LR event**
  when an external write hits a reserved line; `event_status` is only cleared by acks,
  never set. So the task's `RdEventStat` wait for 0x400 never completes.

So the producer chain is: PPU pushes work / updates the control line -> SPU LR event ->
SPU task wakes, drains its queue (the 37-handler dispatch), DMA-publishes the result
pointer to the global slot 0x01B51000 -> func_00A31158 releases.

### Revised build (concrete + bounded, supersedes the vague "large subsystem")
1. **SPU LR-event delivery (core, bounded):** when a guest write (PPU vm_write* or another
   SPU's putllc) hits a 128 B line an SPU holds a getllar reservation on, set that SPU's
   `event_status |= SPU_EVENT_LR (0x400)` (respecting its event mask) so its `RdEventStat`
   wait wakes. Define SPU_EVENT_* in spu_context.h; hook the PPU vm_write path + the SPU
   putllc path to check active reservations. Unit-test (extend spu/tests): getllar on a
   line, arm LR, external write -> RdEventStat returns 0x400.
2. **Ensure the work is pushed:** confirm who writes the watched control line / pushes the
   queue the SPU getllars (Phase 0 next: find the line EA from the task's getllar and who
   on the PPU writes it -- func_00A31158 setup, or a service thread). If the PPU never
   pushes (because it's blocked on the same wait), the work was queued earlier or via a
   path we stubbed -- trace + supply it.
3. **Run to publish:** with LR delivered + work present, the SPU drains + DMA-publishes
   0x45A4B280 -> 0x01B51000; func_00A31158 releases (and the task later hits stop 0 -> our
   exit-code path fires). Integration test: boot past func_00A31158, no Dantelion2 panic.

This is meaningfully smaller than full taskset-PM emulation: the linchpin is SPU LR-event
delivery tied to guest memory writes (a real, testable MFC primitive), plus confirming the
work-push. NEXT: find the getllar line EA + its PPU writer (Phase 0 step 2 above).

---
## Phase 0-next finding — the watched control line + the missing writer

With SPU_EVENT_LR delivery in place (Phase 1a) + SPU_RESV_LOG instrumentation, the
coordinator (image 2) getllar's:
- **line 0x48037A00** (a SPURS control structure in the 0x48xxxxxx region; distinct from
  taskset 0x45B4B300 / exit-code container 0x45B4DC80 / result 0x45A4B280), and
- **line 0x00000000** (a null/uninitialized pointer -> the empty-queue branch).
**LR fired 0 times** -> nothing writes 0x48037A00 while the coordinator waits, so it never
wakes. I.e. the PPU "push work / ready signal" that should write that control line is not
happening (stubbed cellSpurs op, or the producer is the blocked thread, or the
coordinator's control pointers are themselves uninitialized -- the 0x0 getllar hints the
DMA'd context/queue head-tail are not fully set up).

So the LR primitive is correct + ready, but the chain still needs the WRITE to 0x48037A00.
NEXT (the decisive sub-step): identify what 0x48037A00 is (the SPURS object's workload/
LFQueue control?) and which cellSpurs PPU op is supposed to write it (cellSpursReadyCount*
/ an LFQueue push / the workload signal). Two angles:
  (a) trace where the coordinator gets 0x48037A00 (its DMA'd context/taskset field) to map
      the structure;
  (b) grep the PPU cellSpurs path for the readyCount/workload-signal store and confirm it's
      stubbed in our HLE -> implement it to write the control line -> LR fires -> coordinator
      wakes, drains, DMA-publishes the result.
The 0x0 getllar also suggests verifying the coordinator's context (eaContext, currently
auto-zeroed) is initialized with the real queue head/tail -- a parallel root to check.

---
## Phase 0-next CONCLUSION — the watched control is UNINITIALIZED (root pinned)

Dumped the line at the coordinator's getllar: **0x48037A00 content = all zeros** (and
the second getllar 0x00000000 is the null next-pointer computed from that zeroed
control). No PPU write32 ever hits 0x48037A00 either. So the blocker chain bottoms out:

  result-handoff (0x01B51000) <- SPU coordinator publishes <- coordinator drains its
  queue <- coordinator wakes on LR (primitive BUILT) <- PPU writes the control line
  <- the control line 0x48037A00 is INITIALIZED with valid queue head/tail/data.

The last link is missing: **0x48037A00 (the SPURS queue/control the coordinator polls)
is never initialized** -- it stays zeroed, so there is no work and no valid pointers,
and nothing ever writes it to fire LR. This is the same recurring root (the SPURS data
structures aren't set up because our HLE create/init chain is incomplete), now pinned to
a specific structure.

### Next (the real fix, now precisely located)
Find what initializes 0x48037A00 and why it's skipped:
- Identify the structure: a cellSync LFQueue control? a cellSpurs workload/queue? Trace
  where the coordinator's context yields 0x48037A00 (its DMA'd taskset/context field) and
  what guest setup allocates+fills it.
- Grep the boot for the cellSync/cellSpurs queue-init the DLSpursManager runs
  (cellSyncLFQueueInitialize / cellSpursQueueInitialize / _cellSpursQueueInitialize) and
  confirm whether it's implemented + actually targets 0x48037A00, or stubbed/misrouted.
- Implement/fix that init so the control has real head/tail (+ the work pushed). Then the
  push writes 0x48037A00 -> LR fires (primitive ready) -> coordinator drains -> DMA-
  publishes 0x45A4B280 -> func_00A31158 releases.

Net: the mechanism (LR) and the exit-code path are built + tested; the remaining work is
the SPURS queue/control INITIALIZATION (a bounded, locatable target now, not a vague
subsystem). The cellSync LFQueue core already exists (we implement push/pop).

---
## ROOT CAUSE (architectural) — HLE/LLE shared-memory-layout mismatch [2026-06-30]

Why 0x48037A00 is zeroed: our cellSpurs HLE (cellSpurs.c) handles the PPU APIs by
memset-ing a SIMPLIFIED struct (CellSpurs/CellSpursTaskset/CellSpursEventFlag) and
tracking PPU-visible fields (eventFlag->bits, spurs->nSpus, ...). It does NOT build the
REAL SPURS internal memory structures (the control/queue/event-flag internals at the
SDK's exact offsets) -- because those are libsre/SPU-kernel private layout. But the SPU
coordinator (spu_0001) is REAL lifted SPU code that reads those real internal structures
via getllar/DMA (0x48037A00 = one of them). So the two sides disagree on the shared
memory layout, and the SPU reads zeros.

Confirmed: the game's libsre wrappers (func_016B9xxx) just forward the cellSpurs NID to
our HLE (boot logs "[cellSpurs] CreateTaskset()" = our stub); the boot uses the SPURS
EventFlag + lv2 event queues (event_queue_receive(q=1)), NOT a cellSync LFQueue.

This is the crux of HLE-ing the PPU side while running the real SPU side. Three strategic
resolutions:
  A. HLE builds the REAL layout: make our cellSpurs set up the exact SDK-layout internal
     structures the SPU reads. Needs the precise libsre/SDK struct offsets (hard without
     SDK headers; partially recoverable from the SPU code + RPCS3 SpursTasksetContext etc.).
  B. LLE the SPURS PM (RPCS3's approach, "correct"): DON'T HLE cellSpurs -- run the REAL
     libsre taskset POLICY MODULE as actual SPU code (lift fw_spu/libsre.prx's PM binary)
     under a sys_spu_thread_group, so libsre itself builds the structures + schedules the
     leaf task exactly as on hardware. Largest, but matches the design and avoids layout
     guesswork. (We already lift parts of libsre; this extends to the PM SPU binary +
     sys_spu_thread_group support.)
  C. Minimal targeted: RE the exact fields the DeS coordinator reads at 0x48037A00 (head/
     tail + one work entry the 37-handler dispatch expects) and have the HLE fill just
     those. Smallest, DeS-specific, brittle.

The LR primitive + exit-code path are built+tested and are needed by A and C (and used by
B's leaf-task path). RECOMMENDATION: evaluate B (LLE the PM) vs A/C -- B is the principled
fix and reuses our SPU recompiler; A/C are faster but fragile. This is a strategy decision
for the next push, not a quick patch.

---
# OPTION B chosen — reimplement the SPURS taskset Policy Module (clean-room, RPCS3-faithful)

RPCS3 HLEs the SPURS SPU kernel/PM in C++ (cellSpursSpu.cpp) and runs real leaf tasks.
We do the same: build a PM layer that sets up the real shared-memory structures + LS
context the leaf task expects, schedules our lifted leaf task, and handles its syscalls.
Reference contracts (reimplement clean, no GPL copy):

STRUCT LAYOUTS (from RPCS3 cellSpurs.h):
- CellSpursTaskset (main memory, alignas 128): atomic bitsets running/ready/pending_ready/
  enabled/signalled/waiting at 0x00/0x10/0x20/0x30/0x40/0x50; spurs ptr 0x60; args 0x68;
  enable_clear_ls 0x70; wkl_flag_wait_task 0x72; last_scheduled_task 0x73; wid 0x74;
  x78 (= on-task-exit handler EA) 0x78; TaskInfo task_info[128] @0x80 (48 B each: args 0x00,
  elf 0x10, context_save_storage|alloc_ls_blocks 0x18, ls_pattern 0x20); size 0x1890;
  event_flag_id1 0x1898; event_flag_id2 0x189C. (DeS: taskset @0x45B4B300, size=10496.)
- SpursTasksetContext (LS @0x2700, 0x900 B): taskset ptr 0x27B8; spuNum 0x27CC; dmaTagId
  0x27D0; taskId 0x27D4; savedContextLr 0x2C80; savedContextSp 0x2C90; savedR80..R127
  0x2CA0; savedFpscr 0x2FA0; savedEventMask 0x2FB4; x2FC0/x2FC8; taskExitCode 0x2FD0;
  x2FD4 0x2FD4.
DISPATCH (spursTasksetDispatch): SELECT_TASK from ready&enabled bitsets -> DMA task_info
[taskId] to LS 0x2780 -> load the task ELF (ls_pattern) -> savedContextLr=entry ->
branch into the task. Syscalls via `stop <code>` -> spursTasksetProcessSyscall:
EXIT(0)->onTaskExit (load handler @taskset.x78 / ctx.x2FC0 to LS 0x10000, run with
r5=exitCode), YIELD(1), WAIT_SIGNAL(2), POLL(3), RECV_WKL_FLAG(4).

## Option-B phases
- **B1 (structs):** define CellSpursTaskset / SpursTasksetContext / TaskInfo at the real
  offsets (shared header). Unit-testable struct-size asserts (CHECK_SIZE analog).
- **B2 (create path):** rewrite cellSpursCreateTaskset to build the real taskset (bitsets
  cleared, spurs/args/wid/x78/size/event_flag_ids set); cellSpursCreateTask to write
  task_info[taskId] (args/elf/context/ls_pattern) AND set ready|enabled bits for the task.
  This is what populates the structures the leaf task + PM read (fixes the 0x0/zeroed
  reads). Keep our exit-code wiring (eaExitCode -> ctx).
- **B3 (PM dispatch):** before running the leaf task, run the PM: build SpursTasksetContext
  @LS 0x2700, SELECT_TASK (find ready&enabled), DMA its TaskInfo, set up the task-start
  ABI + LS per ls_pattern, branch to the leaf entry. Replaces the raw
  spu_workload_dispatch_task. Reuses our SPU run + the stop-code path.
- **B4 (syscalls):** on the leaf task's `stop <code>` (ctx->stop_code, already preserved),
  dispatch EXIT->onTaskExit (write exit code + run the handler), YIELD/WAIT_SIGNAL via the
  taskset bitsets + the SPURS EventFlag (cellSpursEventFlagSet/Wait, which we have PPU-side)
  + LR delivery (Phase 1a) for the SPU wait.
- **B5 (completion):** the leaf task / onTaskExit publishes the result (DMA to 0x01B51000)
  + signals the EventFlag -> lv2 event queue 1 -> the PPU thread in event_queue_receive(q=1)
  wakes; func_00A31158 releases. Integration test: boot past the SPURS gate, no panic.

## Open question to resolve early in B2/B3
What is 0x48037A00 (the leaf task's getllar control, NOT inside the taskset @0x45B4B300)?
Candidates: the SPURS object's workload/queue control, or the SPURS EventFlag's main-mem
control. Identify it (trace the leaf task's source of that EA, or the event-flag/spurs
object layout) -- B2 must initialize THAT structure too, not just the taskset.

## First concrete step: B1 + locating 0x48037A00
Define the structs (B1) and resolve what allocates/owns 0x48037A00 so B2 initializes it.
