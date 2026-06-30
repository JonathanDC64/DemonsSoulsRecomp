# docs/13 — SPURS task scheduling: deferred execution + completion notification (plan)

Status: PLAN (2026-06-30). Frontier #9 from docs/09 §15v. Goal: fix image=7 (and make the
SPURS task model correct) by replacing "run-on-create" with RPCS3-faithful **deferred,
scheduled** task execution, and wiring **completion → PPU** the way the SDK does.

Clean-room: RPCS3 used as CONTRACT REFERENCE only (researched, not copied). No GPL code, no
keys, no game assets committed.

---

## 1. The problem (recap)

We currently run each SPURS task **synchronously the instant `cellSpursCreateTask` is
called** (`spu_workload_dispatch_task` → spawns a host thread immediately). For image=2 this
happened to work (its arg[0] = the result slot `0x01B51000`, ready at create time). For
image=7 it FAILS: the task runs before the game has populated its per-task data, reads a
NULL pointer from its zeroed arg[2] struct (`0x477E7500`), raises
`CELL_SPURS_TASK_ERROR_NULL_POINTER` (0x80410911), and branches to garbage → halts. The PPU
then spins forever (`event_queue_receive(q=5)` / `0xD002AD50`).

## 2. The RPCS3 contract (researched — see docs/09 §15v + this session's RPCS3 study)

RPCS3 runs tasks **ASYNCHRONOUSLY, when the SPU kernel schedules them**, never at create:

1. **CreateTask** (cellSpurs.cpp:4210+ `_spurs::create_task` then `task_start`):
   - fills `taskset->task_info[id]` (elf, context, args, ls_pattern)
   - sets the `enabled[id]` bit
   - sets the `pending_ready[id]` bit (a STAGING set, not `ready`)
   - `cellSpursSendWorkloadSignal` + `cellSpursWakeUp` → notify the kernel/handler
   - returns to the game **without running the task**
2. **SPU kernel loop** (cellSpursSpu.cpp): consults the workload `readyCount`, schedules the
   workload's policy module; the taskset PM `spursTasksetProcessRequest(SELECT_TASK)` picks a
   task from `ready0 = ready|pending_ready|signalled & enabled & ~running`, marks it
   `running`, loads its ELF, and `spursTasksetStartTask` jumps to the entry. **This happens
   AFTER the game has populated per-task data.**
3. **Task EXIT** (cellSpursSpu.cpp:1885+): `REQUEST_DESTROY_TASK` (clear running/enabled),
   then `spursTasksetOnTaskExit(addr, taskId, exitCode, args)` — **DMAs the game's PPU exit
   handler to LS 0x10000 and `fast_call`s it with r3=taskset, r4=taskId, r5=exitCode,
   r6=args.** That PPU handler posts to the lv2 event queue / event flag the PPU is waiting
   on. `addr` = `taskset->x78` (the on-task-exit handler EA) or `ctxt->x2FC0`.

Key consequences for our HLE:
- We must DEFER task execution until "the game is done setting up" (its data is ready).
- Completion is a **PPU callback** (`taskset->x78`), not just the exit-code container write.
- `cellSpursWakeUp` is NOT separately called by DeS (it's folded inside CreateTask), so the
  deferral trigger is NOT WakeUp — it is the game reaching its **wait** for completion.

## 3. Our hooks (all already exist)

- `sys_event_queue_receive` is intercepted — `runtime/syscalls/sys_event.c`.
- PPU poll-loops sleep via `sys_timer_usleep` (func_00A31158 polls 0x01B51000 that way).
- `ppu_guest_call(opd_ea, r3, r4, r5, r6)` — `runtime/ppu/ppu_loader.cpp` — runs a guest PPU
  function from the runtime (used for GCM vblank/flip handlers). This is how we call the
  task exit handler.
- `CSTS_X78` (taskset+0x78 on-task-exit handler) + `spurs_taskset_set_exit_handler` already
  defined in `libs/spurs/spurs_taskset.{h,c}`.
- Option-B taskset builders (`spurs_taskset_init/add_task`, PM `select/build_context`) already
  build the real shared layout and are unit-tested.

## 4. Design

### Part A — Deferred (scheduled) task execution  [the core fix]

Replace run-on-create with register-then-run-on-schedule:

- **At `cellSpursCreateTask`**: build the Option-B taskset + register the task as PENDING
  (record image, arg, taskset_ea, taskId, exitcode_ea, slot in a global pending list). Set
  the `enabled`+`ready` bits (we collapse pending_ready→ready; we don't model the kernel's
  staging distinction). DO NOT spawn the SPU thread.
- **Trigger = "the creating PPU thread waits"**: add `spu_pending_flush()` calls at the head
  of the PPU blocking/poll points — `sys_event_queue_receive`, `sys_cond_wait`,
  `sys_semaphore_wait`, and `sys_timer_usleep` (the poll-loop sleep). Rationale: by the time
  the game blocks/polls for a result, it has finished the synchronous setup that fills
  per-task data. `spu_pending_flush()` spawns each pending task's host thread (today's
  `spu_async_thread` path) and clears the pending list.
- This is the minimal faithful model: "kernel schedules the task after the game is done,"
  approximated by "run pending tasks the first time the game waits."

Why this is safe for image=2 (must not regress): image=2's result slot (arg[0]=0x01B51000)
is ready at create; delaying its thread start to the first PPU wait (func_00A31158's
`sys_timer_usleep` poll, or the q=1 receive) is harmless — it still runs and DMAs the slot,
the poll still sees it. We MUST include `sys_timer_usleep` in the trigger set so image=2's
poll-loop path flushes it.

Risk: a PPU wait that occurs BETWEEN creating a task and finishing its data would flush too
early. Mitigation: flush is idempotent/one-shot per task; if it proves too eager we can scope
the trigger to the specific taskset's attached event queue (see §6 open Q2).

### Part B — Multi-slot tasksets

The game creates N tasks per taskset (3 image=7 tasks). `spurs_create_task_core` currently
hardcodes `slot=0` and re-inits the taskset each call, clobbering. Change:
- Track a per-taskset slot counter (or derive the next free slot from the `enabled` bitset).
- `spurs_taskset_init` ONCE per taskset (first CreateTask for that taskset_ea); subsequent
  CreateTasks only `spurs_taskset_add_task(slot=next)`.
- Register each task as a distinct pending entry (slot 0..N-1).
- On flush, run each; the PM `build_context` already takes taskId/slot.

### Part C — Completion notification (task EXIT → PPU)

On task EXIT (stop code 0 / EXIT syscall — we already detect this in `spurs_task.c`):
1. Keep the existing exit-code-container write (Foundation A/B) — `eaExitCode`.
2. NEW: if the taskset has an on-exit handler (`taskset->x78` != 0), call it as a PPU guest
   function: `ppu_guest_call(x78_opd, taskset_ea, taskId, exitCode, args)`. That handler is
   what posts to the lv2 event queue (q=5 for image=7) and wakes the PPU's
   `event_queue_receive`. (Mirrors RPCS3 `spursTasksetOnTaskExit`.)
3. image=2 completes by its own DMA to the result slot (self-contained) — no handler needed;
   image=7 needs the handler/event post. Support both.

## 5. Implementation steps (file-by-file)

1. `runtime/spu/spu_workload.{h,c}`:
   - Add a pending-task registry: `spu_task_register_pending(image, size, arg[4],
     taskset_ea, exitcode_ea, taskId)` and `spu_pending_flush(void)` (spawns threads for all
     pending, clears list; thread-safe via a small lock).
   - `spu_workload_dispatch_task` becomes: register pending (NOT spawn). Keep a
     `SPU_TASK_EAGER=1` env to force old run-on-create for A/B comparison.
2. `runtime/syscalls/sys_event.c` (+ sys_cond/sys_semaphore/sys_timer): call
   `spu_pending_flush()` at the head of the receive/wait/usleep handlers.
3. `libs/spurs/cellSpurs.c` `spurs_create_task_core`: per-taskset slot allocation (init once,
   add_task per slot); register pending instead of dispatching.
4. Completion: in `spu_run_lifted_job_abi` / `spurs_task.c` exit path, after writing the
   exit code, if `taskset->x78` set, `ppu_guest_call` it. Capture `x78`: find where the game
   provides the taskset exit handler (taskset attribute or a field) — see open Q1.
5. Diagnostics: log pending register/flush + exit-handler calls.

## 6. Open questions to resolve during implementation

- **Q1 — exit-handler address**: does DeS set `taskset->x78`? If not, where does the
  image=7 completion handler / event-post come from? Check the taskset-attribute calls and
  the AttachLv2EventQueue(q=5) wiring; trace what posts to q=5 in RPCS3's path. (May be that
  the SPU task itself posts via an MFC/atomic to a queue doorbell, like image=2's slot DMA.)
- **Q2 — does deferral alone fix image=7?** Re-test after Part A: at flush time (PPU blocked
  on q=5 receive), dump 0x477E7500 again. If now populated → deferral was the fix. If STILL
  zero → there is a cross-task/coordinator data producer to find (image=7's data may be
  output of another task or the DLSpursManager coordinator). This is the decisive test.
- **Q3 — trigger scope**: global flush-on-any-wait vs per-SPURS/per-queue. Start global
  (simplest); narrow if it runs tasks too eagerly.

## 7. Verification

- Unit: extend the spurs taskset tests for multi-slot add (slots 0..N-1 bits set; taskinfo
  per slot).
- Integration: `SPU_QLOG=1` run — confirm (a) image=7 flush happens at the q=5 receive,
  (b) 0x477E7500 is populated by then (Q2), (c) image=7 no longer hits 0x80410911,
  (d) image=7 reaches EXIT, (e) the exit handler posts to q=5 and the PPU advances past
  `0xD002AD50`. A/B with `SPU_TASK_EAGER=1` to confirm the change is the cause.
- Regression: image=2 still crosses its gate (result slot written, boot reaches input init).

## 8. Sequencing

Part A (deferral) + Q2 test FIRST — it's the highest-information step and likely the image=7
fix. Then Part B (multi-slot) and Part C (completion handler) as needed for full image=7
completion. Each step is independently testable and committable.
