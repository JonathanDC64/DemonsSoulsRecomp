# 17 — SPURS runtime design (authoritative model from RPCS3 source)

*2026-07-05. Synthesized from a clean-room study of RPCS3's SPURS implementation
(cellSpurs.cpp / cellSpursSpu.cpp / lv2/sys_event.cpp / SPUThread.cpp) — contract
reference only, no GPL copied. This replaces the guess-and-patch approach with a
coherent runtime. It is the reusable core for any SPU-based PS3 title.*

## The core misconception this corrects

I had been posting a "selector" event and expecting a single "handler B" to
create the next task. WRONG on three counts:
1. **There are two SPURS PPU threads**, and our port creates NEITHER:
   - `ppu0` / `<prefix>SpursHdlr0` = **handler thread** (workload dispatcher):
     lwcond-driven; scans workload state; starts the SPU thread group.
   - `ppu1` / `<prefix>SpursHdlr1` = **event helper**: a `while(true)`
     `sys_event_queue_receive` loop. (RPCS3: `DLSpursManagerSpursHdlr1` receives
     2112×; our port: absent.) These run in the **libsre PRX**, not the EBOOT — so
     we must reimplement them (HLE host threads), they aren't lifted.
2. **The event helper dispatches on `data0 = data2 & 0x00FFFFFF`, NOT on spup.**
   spup only selects the *queue* at the lv2 layer. data0 ∈ {0,1,2,3}.
3. **The PPU is never sent a "create next task" event.** Task selection is
   SPU-local. The PPU consumes the kernel's *side effects* (shared CellSpurs
   memory + semaphore posts + a callback-fanout event), not its decisions.

## Event encoding (SPUThread.cpp:5969 / sys_event.cpp:182,499)

SPU `sys_spu_thread_send_event(spup, data0, data1)`:
- writes `SPU_WrOutMbox = data1`, then `SPU_WrOutIntrMbox = (spup<<24)|(data0&0xFFFFFF)`.
- lv2 delivers to the queue bound to `spup` as the event tuple:
  `source=0xFFFFFFFF53505501, data1=<sending SPU lv2 id>, data2=(spup<<32)|(data0&0xFFFFFF), data3=<the OutMbox data1>`.
- **ABI: returned in registers r4=source, r5=data1, r6=data2, r7=data3, r3=rc.**
  (Our sys_event_queue_receive now sets these — commit 507ad9c. The pointer arg is
  a dummy, never written, for the blocking form.)

## Event-helper loop (cellSpurs.cpp:822) — reimplement as a host thread

```
while (true):
  sys_event_queue_receive(spurs->eventQueue @0xD5C, NULL, timeout=0)   // blocks forever
  src=r4; data1=r5; data2=r6; data3=r7
  if src == 0xFFFFFFFF53505503 (EXCEPTION): spurs->exception=1; drain; post wklF sems; continue
  data0 = data2 & 0x00FFFFFF
  switch data0:
    0: shutdownMask=data3; for each set wid -> wakeup_shutdown_completion_waiter(wid)  // posts wklF[wid].sem
    1: return            // ONLY exit -> sys_ppu_thread_exit (sent only at finalization)
    2: sys_semaphore_post(spurs->semPrv @0x980)                                        // trace ack
    3: invoke_event_handlers(&spurs->eventPortMux @0xF00)                              // <-- game completion callbacks
    else: panic
```
`invoke_event_handlers` (cellSpurs.cpp:758): if `eventPortMux.reqPending` (@0xF00) set,
walk `handlerList` (@0xF18, nodes {next,data,handler}) and call each
`handler(ppu, mux, node->data)`. The GAME registers these nodes (stateful
list-search on data1 — matches our earlier func_00C5D6D8 observation). data0=3 is
the path to the game's Havok/Dantelion completion functors.

## Handler thread (cellSpurs.cpp:620/698) — reimplement as a host thread

```
loop under spurs->mutex @0xDB0:
  for each wid: runnable(i) = wklState1[i]@0x80==RUNNABLE(2) && priority@wklInfo!=0
                            && (wklMaxContention[i]@0x50 & 0xF)
                            && (wklReadyCount1[i]@0x00 || (wklSignal1@0x70 & (0x8000>>i))
                                || (wklFlag@0x6C==0 && wklFlagReceiver@0x77==i))
  if any runnable: sys_spu_thread_group_start(spuTG @0xD30); join; loop
  else: handlerWaiting@0xD65=1; if handlerDirty@0xD64==0: sys_lwcond_wait(spurs->cond @0xDC8)
```
Woken by `cellSpursWakeUp` (handlerDirty=1 + lwcond_signal), `cellSpursReadyCountStore`,
`cellSpursSendWorkloadSignal`.

## CellSpurs offsets (cellSpurs.h) — the shared main-memory contract

first 128 bytes = the scheduling block (kernel mutates, PPU reads):
```
0x00 wklReadyCount1[16]  0x10 wklIdle/ReadyCount2[16]  0x20 wklCurrentContention[16]
0x30 wklPendingContention[16]  0x50 wklMaxContention[16]  0x60 wklFlag(flag@0x6C)
0x70 wklSignal1  0x77 wklFlagReceiver  0x78 wklSignal2  0x80 wklState1[16]
0xA0 wklEvent1[16]  0xB0 wklEnabled  0xC9 spuPort
0xD20 ppu0(handler tid)  0xD28 ppu1(event-helper tid)  0xD30 spuTG  0xD34 spus[8]
0xD5C eventQueue  0xD60 eventPort  0xD64 handlerDirty  0xD65 handlerWaiting
0x980 semPrv  0xB00 wklInfo1[16](0x20 each: addr,arg@8,size,uniqueId,priority[8]@0x18)
0xDB0 mutex  0xDC8 cond  0xF00 eventPortMux(reqPending@0,spuPort@4,eventPort@0x10,handlerList@0x18)
```
WorkloadState: NON_EXISTENT0 PREPARING1 RUNNABLE2 SHUTTING_DOWN3 REMOVABLE4.
Taskset (separate 0x2000 struct): 0x00 running,0x10 ready,0x20 pending_ready,
0x30 enabled,0x40 signalled,0x50 waiting (128-bit each by taskId), 0x80 task_info[128].

## MINIMUM VIABLE MODEL (we run leaves directly; give the PPU the kernel's side effects)

**A. Materialize CellSpurs first-128-bytes as real mutated memory:**
- wklState(wid)=RUNNABLE while active; ->SHUTTING_DOWN->REMOVABLE on teardown.
- **wklCurrentContention[wid]: ++ on leaf start, -- on leaf exit** (MOST IMPORTANT —
  barriers/handler infer "work outstanding/done" from this).
- decrement wklReadyCount as consumed; clear wklSignal bit when picked up.

**B. Maintain taskset bitsets** for run tasksets: task bit ready->running on start,
clear running+enabled on DESTROY_TASK, bump readyCount by numNewlyReadyTasks.

**C. Send the real completion events on spurs->spuPort with correct data0:**
- workload done: `send(spuPort, data0=0, data1=<bitmask of finished wids>)`
  -> event helper posts wklF[wid].sem (the barrier waiters block on these).
- game callbacks: set eventPortMux.reqPending=1, ensure handlerList populated
  (game does this), `send(spuPort, data0=3, data1=<mux key>)` -> invoke_event_handlers.

**D. Kick the handler thread** on any new runnable/readyCount: handlerDirty=1;
if handlerWaiting: sys_lwcond_signal(spurs->cond).

Omit safely: weight-based SelectWorkload, LS scratch contexts, trace, ELF load.

## IMPLEMENTATION PLAN for our port (libs/spurs + runtime)

P-A. cellSpursInitialize (deepen P2 in cellSpurs.c): create eventQueue + eventPort +
  connect + spuPort; write them to CellSpurs @0xD5C/0xD60/0xC9. Store the CellSpurs EA.
P-B. Spawn two HOST threads (our HLE substitute for the libsre PRX threads):
  - event-helper: the loop above (receive spurs->eventQueue; dispatch data0;
    for data0=3 call the guest handlers via g_ps3_guest_caller / invoke_event_handlers
    reimplemented on the guest eventPortMux).
  - handler: the lwcond-driven workload dispatcher (or a simpler poll that starts
    workloads when RUNNABLE).
P-C. Replace trigger B / SPURS_DONE_EVQ hacks: on spu_workload leaf completion,
  do the A/B/C/D side effects (contention--, send data0=0 with the wid bitmask,
  and data0=3 for callbacks) to the SPURS eventQueue.
P-D. Wire cellSpursWakeUp/ReadyCountStore/SendWorkloadSignal to kick the handler.

Each of P-A..P-D is independently testable via tools/des/spurs_trace.py (watch the
event-helper's recv count rise + wklF sem posts + the dormant workers waking).
Gate behind SPURS_LV2THREADS so default boot is unaffected until proven.
