# 10 — SPURS HLE reimplementation blueprint (from RPCS3, clean-room)

Reimplementation checklist for the SPURS runtime, distilled from RPCS3's `cellSpurs.cpp` /
`cellSpursSpu.cpp` (behavior/contract only — write our own C). Pairs with `09-spu-spurs.md`.

## Critical realization (changes the plan)
RPCS3's SPURS bring-up posts **NO explicit "SPURS ready" event**. `cellSpursInitializeWithAttribute`
creates objects + two PPU threads (handler `spurs->ppu0`, event-helper `spurs->ppu1`) that then
**block until the game calls `cellSpursWakeUp` / `cellSpursAddWorkload`**. So our `cond=3` init-block
is **NOT** released by a SPURS event — it's Dantelion2's own logic.

Also key: the threads we see (`(Dantelion2)PPURuntimeService` -> func_00C3A878, `SPURuntimeService`
-> func_00C5DAB0 receiving on queue 1) are the GAME's OWN threads — i.e. **Dantelion2 reimplements
the SPURS PPU-side handler/event-helper itself** using lv2 primitives directly, rather than using
cellSpurs's internal threads. So our HLE must make the lv2 + SPU-thread-group substrate behave such
that Dantelion2's own threads function — less "drive cellSpurs's threads," more "make the SPU thread
group + events real so Dantelion2's services get their events."
=> Pre-req #1 (pin cond=3's predicate) is still required and is Dantelion2-specific; the RPCS3
blueprint doesn't shortcut it. Use a reliable guest backtrace (per-frame LR offsets) or single-step.

## cellSpursInitializeWithAttribute — init order (RPCS3 _spurs::initialize)
1. memset(spurs, 0, 4096 / 8192 if SAF_SECOND_VERSION).
2. Static fields: revision, sdkVersion, ppu0=ppu1=0xffffffff, flags, prefix/prefixSize;
   wklInfoSysSrv = { addr=0x100 (SPURS_IMG_ADDR_SYS_SRV_WORKLOAD), size=0x2200, arg=0, uniqueId=0xff }.
3. 16 workload semaphores (name "_spuWkl"), +16 more if SAF_SECOND_VERSION; store in wklF1[i].sem.
4. 1 private semaphore semPrv (name "_spuPrv").
5. SPU thread group: spuImg.entry = 0x818 (k1) / 0x848 (k2); sys_spu_thread_group_create(nSpus,prio);
   per SPU: arg1=(num<<32), arg2=spurs_EA; sys_spu_thread_initialize(...).
6. sys_lwmutex_create(spurs->mutex) + sys_lwcond_create(spurs->cond).
7. flags1 = (SAF_EXIT_IF_NO_WORK?SF1_EXIT_IF_NO_WORK:0)|(isSecond?SF1_32_WORKLOADS:0);
   wklFlagReceiver=0xff; wklFlag.flag=0xffffffff; ppuPriority,nSpus set.
8. create_event_helper: lv2 event queue (SYS_PPU_QUEUE, name "_spuPrv", size 0x2A) + event port
   (connect_local) + connect SPU thread-group events to it; spawn the event-helper PPU thread.
9. create_handler: spawn the handler PPU thread (blocks until work).
10. cellSpursEnableExceptionEventHandler; init_event_port_mux(spuPort,eventPort,3).
11. if SAF_SYSTEM_WORKLOAD_ENABLED: add_default_syswkl; elif SAF_EXIT_IF_NO_WORK: cellSpursWakeUp.

## Event-helper thread protocol (cellSpurs event_helper_entry) — drives PPU on SPU events
loop: sys_event_queue_receive(eventQueue) -> (src=gpr4, d1=gpr5, d2=gpr6, d3=gpr7).
- src == SYS_SPU_THREAD_EVENT_EXCEPTION_KEY -> spurs->exception=1; post all wkl semaphores.
- else data0 = d2 & 0xFFFFFF: ==1 -> exit thread; ==0 -> workload-shutdown completion (d3 mask, bit
  `(0x80000000>>wid)`, +`(0x8000>>wid)` for 16..31) -> wake shutdown waiters; ==2 -> post semPrv;
  ==3 -> invoke_event_handlers(eventPortMux).
NOTE: Dantelion2's SPURuntimeService (func_00C5DAB0) is the analog but uses a DIFFERENT protocol:
source 0xFFFFFFFF53505501, data2 high byte {1,0x20} (see docs/09 §13) — it's the game's own, not this.

## SPU kernel startup (cellSpursSpu spursKernelEntry -> spursSysServiceMain)
ctxt@LS 0x100: spuNum=gpr3, spurs_EA=gpr4; dmaTagId=31; wklCurrentId=32 (SYS_SERVICE);
exitToKernelAddr=0x808/0x838. Dispatches the system-service workload (no PPU "ready" post). The
system service polls workload state, idles via spursSysServiceIdleHandler until signaled.

## cellSpursEventFlag (SPU<->PPU flag over an lv2 queue)
- struct: ctrl{ppuPendingRecv, ppuWaitSlotAndMode, ppuWaitMask, spuTaskPendingRecv, events},
  direction (SPU2PPU/ANY2ANY), clearMode (AUTO/MANUAL), eventQueueId, eventPortId, spuPort, isIwl,
  addr (SPURS/taskset EA), 16 wait slots (mask/mode/wklId/taskId), pendingRecvTaskEvents[16].
- AttachLv2EventQueue: CREATES an lv2 queue (name "_spuEvF") + port, stores eventQueueId/spuPort;
  ANY2ANY also creates an event port (connect_local) for PPU->SPU.
- Set(bits): atomic; if a PPU/SPU-task waiter's mask satisfied (AND: all bits; OR: any), clear (if
  AUTO), set pendingRecv, and notify: PPU via sys_event_port_send(eventPortId,0,0,0); SPU task via
  _cellSpursSendSignal(taskset, taskId).
- Wait(mask,mode): if already satisfied return; else take a wait slot, sys_event_queue_receive(eventQueueId).

## Constants / offsets (CellSpurs @ EA)
KERNEL1/2 entry 0x818/0x848, exit 0x808/0x838, select_workload 0x290; SYS_SERVICE_WKL_ID=32;
SYS_SRV_WORKLOAD img 0x100; DMA tag 31. Offsets: wklF1[16]@0x100 (128B each, sem@+0x20),
wklInfo1@0xB00, ppu0@0xD20, ppu1@0xD28, spuTG@0xD30, spus[8]@0xD34, eventQueue@0xD5C, eventPort@0xD60,
handlerDirty/Waiting/Exiting@0xD64-66, spuImg@0xD70, flags@0xD80, prefix@0xD8C, mutex@0xDB0,
cond@0xDC8, semPrv@0x980. (Verify against our CellSpurs in cellSpurs.h before relying on offsets.)

## Minimal subset to get past bring-up + handler receiving
struct clear + fields; 16+1 semaphores; SPU thread group + threads; lwmutex/lwcond; event
queue+port; event-port-mux; handler thread (blocks); event-helper thread (blocks on eventQueue).
NOT needed initially: system workload, exception handler, trace, tasksets/tasks.
BUT for DeS specifically: Dantelion2 runs its OWN services, so the real requirement is that the SPU
thread group actually RUNS (our recompiled task images) and posts events to queue 1 — which still
needs the SPURS scheduler running tasks. So "bring-up" for DeS ≈ get the SPU thread group executing
SPURS work and delivering queue-1 events; the cellSpurs-internal-threads model above is the contract,
but Dantelion2 layers its own services on top.

## Build approach
Implement faithfully in `ps3recomp/libs/spurs/cellSpurs.c` against this checklist, using our sys_*
handlers to create the lv2 objects and host threads for the PPU-side service loops; dispatch the
game's task images via spu_run_lifted_job_abi; deliver SPU->PPU events to queue 1 in the game's
expected format (docs/09 §13). Validate incrementally with HLETRACE/SYNCTRACE. Pin cond=3 first.
