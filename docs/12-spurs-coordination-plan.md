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
