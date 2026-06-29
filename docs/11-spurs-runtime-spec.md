# 11 — SPURS runtime reimplementation spec (clean-room, from RPCS3 behavior)

Distilled from RPCS3 `cellSpursSpu.cpp` / `cellSpurs.cpp` / `cellSpurs.h` (behavior/contract only —
we write our own C; RPCS3 is GPLv2). This is the **implementation spec** for the HLE SPURS scheduler
(pairs with docs/09 §13 event format and docs/10 bring-up blueprint). All offsets/constants cited
symbolic + numeric.

## LS memory map the firmware kernel assumes (game task ELFs / policy modules are linked to these)
| LS addr | Symbol | Contents |
|---|---|---|
| 0x0 | CELL_SPURS_INTERRUPT_VECTOR | SPU interrupt vector |
| 0x100 | (kernel ctxt base) | SpursKernelContext (size 0x190) |
| 0x808 | CELL_SPURS_KERNEL1_EXIT_ADDR | kernel-1 workload-exit trampoline |
| 0x818 | CELL_SPURS_KERNEL1_ENTRY_ADDR | kernel-1 entry |
| 0x838 / 0x848 | KERNEL2_EXIT / KERNEL2_ENTRY | kernel-2 variants |
| 0x290 | KERNEL{1,2}_SELECT_WORKLOAD_ADDR | select-workload trampoline |
| 0xA00 | CELL_SPURS_TASKSET_PM_ENTRY_ADDR | **where every policy-module / workload image is DMA'd and entered** |
| 0xA70 | CELL_SPURS_TASKSET_PM_SYSCALL_ADDR | taskset PM syscall entry |
| 0x2700 | (taskset ctxt base) | SpursTasksetContext (size 0x900) |
| 0x2C00 | — | trace buffer scratch (0x80) |
| 0x2D80 | — | scratch copy of wklState1.. (128 B) |
| 0x3000 | CELL_SPURS_TASK_TOP | task ELF load region start |
| 0x3FFE0 | — | scratch for selected WorkloadInfo (0x20) |
| 0x40000 | CELL_SPURS_TASK_BOTTOM | LS bottom |

Constants: MAX_SPU=8, MAX_WORKLOAD=16, MAX_WORKLOAD2=32, SYS_SERVICE_WORKLOAD_ID=32,
MAX_TASK=128, KERNEL_DMA_TAG_ID=31. Image-addr sentinels: SYS_SRV_WORKLOAD=0x100, TASKSET_PM=0x200.

## 1. SPU kernel loop + workload selection
`spursKernelEntry(spu)`: zero ctxt@0x100; spuNum=gpr3.w3, spurs_EA=gpr4.d1; isKernel2=(flags1 & SF1_32_WORKLOADS 0x40);
ctxt: dmaTagId=31, wklCurrentUniqueId=0x20, wklCurrentId=32(SYS_SERVICE), exit/select addrs set;
then dispatch sys-service: `spursKernelDispatchWorkload(spu, 32ull<<32)`. **SPU always starts in sys-service (wid32) which loops selecting real workloads.**

`spursKernel1SelectWorkload` (16-wkl) reads only CellSpurs first 128B. Per wkl i (0..15):
- contention[i] = wklCurrentContention[i](0x20) − ctxt.wklLocContention[i](0x180); poll mode adds pending.
- runnable = ctxt.wklRunnable1(0x1EC) & (0x8000>>i); wklSignal = wklSignal1(0x70) & (0x8000>>i);
  wklFlag = (wklFlag.flag(0x6C)==0 && wklFlagReceiver(0x77)==i); readyCount=min(wklReadyCount1[i](0x00),8);
  idleSpuCount=min(wklIdleSpuCountOrReadyCount2[i](0x10),8); requestCount=readyCount+idleSpuCount.
- **sysSrvMessage(0x72) & (1<<spuNum) overrides → select wid32** (clear bit if not poll).
- Eligible iff: runnable AND priority[i](0x1A0)!=0 AND wklMaxContention[i](0x50)>contention[i] AND (wklFlag OR wklSignal OR (readyCount!=0 && requestCount>contention)).
- weight(16b, higher wins, tie→lower id): bit0x8000 if(flag||signal||ready>cont); |(priority&0x7F)<<8; |0x80 if i==current; |0x40 if(cont>0 && wklMinContention[i](0x40)>cont); |((8-cont)&0xF)<<2; |0x02 if wklUniqueId[i](0x220)==current; |0x01.
- pollStatus: READYCOUNT=1 if ready>cont, |SIGNAL=2, |FLAG=4. spuIdling(0x1EB)=1 if none selected.
- Non-poll bookkeeping: inc contention[selected], writeback, set wklLocContention[selected]=1, ctxt.wklCurrentId=selected, clear selected wklSignal bit, consume wklFlag (set flag=-1) if selected==receiver.
- Result: gpr3.d1 = (wklSelectedId<<32)|pollStatus.

`spursKernel2SelectWorkload` (32-wkl): contention/priority/maxContention are **packed nibbles** (low=0–15, high=16–31), weight 8b: priority<<4 | (current?0x04:0).

`cellSpursModulePollStatus(spu,*status)`: gpr3.w3=1(poll), call select, *status=(u32)result, **return (result>>32==wklCurrentId)?0:1** → 1 means "yield, another wkl wants this SPU". Cooperative-yield signal.

`spursKernelWorkloadExit` @exitToKernelAddr: gpr3=0 (real select), select, dispatch. Loop: module exits→reselect→dispatch next.

## 2. Workload→image→entry (`spursKernelDispatchWorkload`) — the dispatch ABI
Given widAndPollStatus (wid=hi32, pollStatus=lo32):
1. WorkloadInfo src: wid<16→wklInfo1[wid] (spurs+0xB00+wid*0x20); 16≤wid<32 & k2→wklInfo2[wid&0xf] (spurs+0x1000); else wklInfoSysSrv (spurs+0xD00).
2. DMA 32B WorkloadInfo→LS 0x3FFE0. Layout (size 0x20): +0x00 addr(EA u64), +0x08 arg(u64), +0x10 size(u32), +0x14 uniqueId(u8), +0x18 priority[8].
3. Load image if ctxt.wklCurrentAddr(0x1D0)!=addr: addr==0x100→built-in sys-service; ==0x200→built-in taskset PM; **else memcpy(LS[0xA00], addr, size)** (game's workload image — the lifter case). Save wklCurrentAddr/UniqueId.
4. **Set regs + jump (THE workload/task ABI):**
   - gpr0.w3 = ctxt.exitToKernelAddr (LR→kernel exit)
   - gpr1.w3 = 0x3FFB0 (SP)
   - gpr3.w3 = 0x100 (r3 = LS addr of kernel ctxt, NOT an EA)
   - gpr4.d1 = wklInfo.arg (workload arg u64)
   - gpr5.w3 = pollStatus
   - pc = 0xA00

For our `spu_run_lifted_job_abi`: image at LS 0xA00; r3=0x100, r4=arg, r5=pollStatus, LR=exit, SP=0x3FFB0. ctxt.spurs (LS 0x1C0) holds CellSpurs EA. Pre-entry valid memory: CellSpurs 128B header mirrored to LS 0x100, WorkloadInfo@0x3FFE0, image@0xA00.

## 3. Taskset & task execution
**Taskset = workload whose PM image is built-in (addr 0x200).** create_taskset (PPU): zero CellSpursTaskset, set spurs/args/enable_clear_ls/size; AddWorkloadWithAttribute(pm=0x200, pm_size=0x1E40, **data=tasksetEA**, priority, maxContention); set wkl_flag_wait_task=0x80, wid=*wid. **So workload arg(r4) for a taskset = EA of CellSpursTaskset.**

`spursTasksetEntry`: ctxt@LS 0x2700; arg=gpr4.d1=tasksetEA→ctxt.taskset; kernelCtxt=gpr3.w3(0x100); moduleId="SPURSTASK MODULE", syscallAddr=0xA70, taskId=0xFFFFFFFF; init then dispatch.

`spursTasksetDispatch`:
1. process_request(SELECT_TASK=5, &taskId, &isWaiting).
2. taskId>=128 → taskset_exit (back to kernel).
3. DMA task_info[taskId] (48B TaskInfo)→LS 0x2780: +0x00 args(16B), +0x10 elf(EA; low3=flags), +0x18 context_save_storage_and_alloc_ls_blocks(EA&-0x80=save; low7=LS blocks), +0x20 ls_pattern(16B 2KB-block bitmask).
4. isWaiting==0 (fresh): zero LS 0x3000..0x40000; loadElf(&entry,&lowest,elf,skipW=false) (PT_LOAD within 0x3000..0x40000, returns e_entry); savedContextLr=entry; startTask(args):
   - gpr2=0; gpr3=taskArgs(128b); gpr4.d1=taskset.args; gpr4.d0=tasksetEA; gpr5..127=0; pc=entry.
   - **task entry ABI: r3=16B arg, r4={tasksetEA(hi), taskset.args(lo)}, pc=e_entry.**
5. isWaiting==1 (resume): opt clear LS; if ls_pattern!=all-ones reload non-writeable segs; memcpy(LS[0x2C80], save, 0x380) (LR+SP+r80..127+fpscr); per ls_pattern bit i(6..127) reload 2KB from save+0x400+(i-6)*0x800 → LS[0x3000+(i-6)*0x800]; restore fpscr/tagmask/eventmask; gpr3=0; resume (pc←saved LR).

Task syscalls @0xA70 (`spursTasksetProcessSyscall`), saves task ctxt (LR=gpr0,SP=gpr1,gpr80..127) then syscallNum&0xF:
| Num | Symbol | Behavior |
|---|---|---|
| 0 | EXIT | destroy task(req0), task-exit hook, redispatch |
| 1 | YIELD | if poll says yield→save ctxt, YIELD_TASK(req1) |
| 2 | WAIT_SIGNAL | if not signalled→save ctxt, WAIT_SIGNAL(req2) |
| 3 | POLL | returns FOUND_WORKLOAD(2)/FOUND_TASK(1) |
| 4 | RECV_WKL_FLAG | wait/consume workload flag |
After incident: re-poll → exit or dispatch next.

Task bitsets (six 128-bit in CellSpursTaskset): running 0x00, ready 0x10, pending_ready 0x20, enabled 0x30, signalled 0x40, waiting 0x50. Bit for task t = 1<<(~t & 127) (MSB-first). last_scheduled_task 0x73, wkl_flag_wait_task 0x72.
SELECT_TASK: readyButNotRunning=(signalled|ready|pending_ready)&~running, minus flag-wait task; round-robin from last+1; mark running, clear waiting, set last; output isWaiting. Returns 128 if none. numNewlyReadyTasks fed into spurs.readyCount(wid) (clamp 0..255).

`_cellSpursSendSignal` (PPU): atomic over taskset first 128B; signal=(~signalled & waiting & mask); signalled|=mask; **if signal==1: cellSpursSendWorkloadSignal(spurs, wid) then cellSpursWakeUp(spurs)**.

## 4. SPU→PPU event/completion (ALL via one lv2 queue)
spurs.eventQueue(0xD5C) created in create_event_helper (size 0x2A, name "_spuPrv\0") + eventPort(0xD60) connected. PPU event-helper (spurs.ppu1) loops sys_event_queue_receive → tuple (src=gpr4, data1=gpr5, data2=gpr6, data3=gpr7).
SPU posts via sys_spu_thread_send_event(spu,spup,data0,data1): data1→OutMbox, (spup<<24)|(data0&0xFFFFFF)→OutIntrMbox. lv2 makes queue entry: **data2 & 0xFFFFFF = data0**, data3=payload.

Event-helper dispatch (`event_helper_entry`):
- src==SYS_SPU_THREAD_EVENT_EXCEPTION_KEY → spurs.exception=1, drain, post all wkl sems.
- else data0=(data2 & 0xFFFFFF): **==1**→helper exits (stop ping: port_send(eventPort,0,1,0)); **==0**→workload shutdown completion (data3=shutdownMask, per wid wakeup_shutdown_completion_waiter); **==2**→trace-update done, post semPrv(0x980); **==3**→run event-port-mux handlers (generic "SPU wants PPU callback", used by event flags/queues).

Other PPU port_send callers: cellSpursShutdownWorkload→port_send(eventPort,0,0,(1u<<31)>>wid) (data0=0 path); cellSpursEventFlagSet→port_send(eventFlag.eventPortId,0,0,0).

Payloads to post (sys_event_port_send(port, data1, data2, data3); helper reads data0=data2&0xFFFFFF):
| Meaning | data1 | data2/data0 | data3 |
|---|---|---|---|
| Workload(s) shutdown complete | 0 | 0 | 0x80000000>>wid |
| Helper exit | 1 | 0 | 0 |
| Trace update complete | 0 | 2 | 0 |
| Run muxed handlers | 0 | 3 | 0 |

**No per-task "completed" PPU event in normal path.** Task EXIT → on-SPU task-exit hook (spursTasksetOnTaskExit fast_calls callback @LS 0x10000, r3=tasksetEA,r4=taskId,r5=exitCode,r6=args). PPU learns of completion only via app event flags / cellSpursEventFlagWait (→data0==3 mux or event-flag port).

## 5. Handler thread + wakeup
Two PPU threads per SPURS: handler (spurs.ppu0, handler_entry, owns start/join SPU TG) + event-helper (ppu1, §4).
`handler_entry`: if(flags&SAF_UNKNOWN_FLAG_30 0x2)exit; loop: if(flags1&SF1_EXIT_IF_NO_WORK 0x80) handler_wait_ready(); sys_spu_thread_group_start(spuTG); rc=join; if rc==ESTAT exit; if !EXIT_IF_NO_WORK ensure(handlerExiting==1),exit.
`handler_wait_ready`: under mutex(0xDB0) set handlerDirty=0, scan wids runnable iff wklState1[i]==RUNNABLE(2) && wklInfo1[i].priority!=0 && (wklMaxContention[i]&0xF)!=0 && (wklReadyCount1[i] || wklSignal1&(0x8000>>i) || (wklFlag.flag==0 && wklFlagReceiver==i)); found→break(start group); else handlerWaiting=1, if handlerDirty==0 sys_lwcond_wait(cond 0xDC8), clear handlerWaiting.
`cellSpursWakeUp`: handlerDirty.exchange(1); if handlerWaiting signal_to_handler_thread (lock mutex, lwcond_signal, unlock). handlerDirty closes the lost-wakeup race.
AddWorkload does NOT signal handler directly: sets wklState=RUNNABLE, sysSrvMsgUpdateWorkload=0xff(0xBD), sysSrvMessage=0xff(0x72). Running sys-service SPU sees it, activates (sets wklRunnable1 bits, copies priorities). App's subsequent WakeUp (or task_start/SendSignal) wakes handler.
flags1(0x74): SF1_EXIT_IF_NO_WORK=0x80, SF1_32_WORKLOADS=0x40.

## 6. Minimal viable scheduler for our HLE (lifted images, no firmware PRX)
Collapse SPU kernel into a host-thread scheduler; keep only parts touching shared CellSpurs/Taskset memory + PPU event queue.

MUST be real: CellSpurs 128B header semantics (wklEnabled 0xB0, wklState1/2 0x80/0xD0, wklReadyCount1 0x00, wklSignal1/2 0x70/0x78, wklMaxContention 0x50, wklInfo1/2 0xB00/0x1000, wklFlag 0x60/receiver 0x77, sysSrvMessage 0x72, sysSrvMsgUpdateWorkload 0xBD); selection predicate (simplified weight OK: highest-priority eligible runnable, low id tie); dispatch ABI §2 exactly; cellSpursModulePollStatus contract (return 1 when another wkl wants SPU); tasksets (six bitsets + SELECT_TASK round-robin, ELF load honoring ls_pattern, task-start ABI, ctxt save/restore, readyCount feedback); event-helper data0∈{0,1,2,3}+exception; WakeUp handlerDirty/handlerWaiting/lwcond; SendWorkloadSignal + _cellSpursSendSignal.

Can stub: trace (cellSpursModulePutTrace/traceStartIndex no-op); GUID magic; wklMinContention/pending/spuIdling fine accounting (single-SPU-per-wkl: contention∈{0,1}, skip pending); sys-service idle group-exit dance (host thread just blocks when nothing eligible); data0==2 trace-notify & data0==0 shutdown unless game uses workload shutdown; jobchain PM unless game uses cellSpursCreateJobChain.

Plain-workload completion = lifted image returns to LR (kernel exit) → reselect; if none eligible & wkl had shutdown hook/waiter, post data0==0 (data3=0x80000000>>wid). Taskset task completion fires on-SPU exit hook + bitset update; PPU notified only if app set up event flag.

## Implementation checklist (ordered C functions) — see RPCS3 cellSpursSpu.cpp/cellSpurs.cpp/cellSpurs.h
1. spurs_dispatch_workload(spurs_EA, wid, pollStatus) — §2.
2. spurs_select_workload(spurs_EA, ctxt, isPoll) → (wid<<32)|pollStatus — §1.
3. spurs_module_poll_status(ctxt,*status)→int — §1.
4. spurs_kernel_worker(spuNum, spurs_EA) — host thread: init ctxt, loop select→dispatch→reselect, block when idle.
5. spurs_sys_service_activate(spurs_EA, ctxt) — on sysSrvMessage: scan wklState, set wklRunnable, copy priority(0x10-p)/uniqueId, shutdown mask.
6. spurs_post_shutdown_events(spurs_EA, mask) — port_send(eventPort,0,0,mask).
7. taskset_pm_entry(spurs_EA, taskset_EA, pollStatus) — init ctxt, taskset_dispatch.
8. taskset_process_request(taskset_EA, req, *taskId, *isWaiting) — six-bitset SM; readyCount feedback.
9. taskset_dispatch — SELECT_TASK; none→exit; else DMA TaskInfo, load/restore, jump.
10. taskset_load_elf(elf_EA, skipWriteable, *entry, *lowest) — SPU ELF PT_LOAD copy within 0x3000..0x40000.
11. taskset_save_context / resume_task / start_task — per ls_pattern; start ABI (r3=args, r4={tasksetEA,args}).
12. taskset_process_syscall(num,args) — EXIT/YIELD/WAIT_SIGNAL/POLL/RECV_WKL_FLAG; task-exit hook on EXIT.
13. ppu_cellSpursAddWorkload[WithAttribute] → set header, sysSrvMsgUpdateWorkload=0xff, sysSrvMessage=0xff, wklState=RUNNABLE, alloc uniqueId.
14. ppu_cellSpursWakeUp — handlerDirty=1; if handlerWaiting signal condvar.
15. ppu_cellSpursSendWorkloadSignal(spurs,wid) — set wklSignal bit (state==RUNNABLE).
16. spurs_handler_thread(spurs_EA) — wait-ready predicate; start/join worker(s); honor EXIT_IF_NO_WORK.
17. spurs_event_helper_thread(spurs_EA) — queue_receive loop; dispatch data0∈{0,1,2,3}+exception.
18. ppu_cellSpursCreateTaskset[WithAttribute] → zero taskset, AddWorkload(pm=0x200,size=0x1E40,data=tasksetEA), wkl_flag_wait_task=0x80, wid.
19. ppu_cellSpursCreateTask → alloc id from enabled bitset, fill task_info[id], task_start (pending_ready bit, SendWorkloadSignal, WakeUp).
20. ppu__cellSpursSendSignal(taskset, taskId) — signalled|=mask; if newly-signalled-waiter SendWorkloadSignal+WakeUp.

## DeS-specific note
Dantelion2 runs its OWN PPU services (PPURuntimeService func_00C3A878, SPURuntimeService func_00C5DAB0
on queue 1) instead of cellSpurs's internal handler/event-helper threads (docs/10). So for DeS the
critical path is: the SPU thread group must actually execute the game's task images and post queue-1
events in the game's format (docs/09 §13: source 0xFFFFFFFF53505501, data2 high byte {1,0x20}) — which
is the §3 taskset/task execution + a DeS-specific event bridge, layered on this contract.

---

## External prior-art survey [2026-06-29] — conclusion: clean-room from RPCS3 is the only path
No reusable SPURS scheduler exists outside RPCS3 (GPL — study-only). Confirmed avenues:
- **RPCS3** cellSpursSpu.cpp/cellSpurs.cpp = the only working scheduler reference (~5k lines, pure HLE; SPURS games boot with no firmware). Clean-room from behavior, as we're doing.
- **flOw port (sp00nznet/flow)**: its only "real SPURS" route LOADS decrypted libsre.prx (relocated to 0x30000000, lifted to C) — REJECTED here (violates no-keys/no-firmware/clean-room). Also flOw never actually exercises SPURS at runtime (cellSpurs stubbed, never triggers SPU thread create). Reusable: PRX-loader/NID-dispatch/SPU-PPU-fallback plumbing + expanded SPURS NID database (100+ NIDs).
- **PSL1GHT** (ps3dev/PSL1GHT, ppu/include/spurs/*.h): API surface only (Spurs = opaque 4096B / 8192B v2; SpursAttribute = 512B blob; transparent SpursInfo has deadlineMiss/MeetCounter). libspurs SPRX just imports firmware cellSpurs. SAFE clean-room interface facts: attribute init **revision=0x02, sdkVersion=0x330000**; struct sizes. No scheduler.
- **Sony SPURS patents** (US7647483B2, US8589943, US20070074207, US2005/0188373): design model only (LS-resident kernel, tasksets, contention/priority, deadline meet/miss counters) — no bit-level offsets/protocol. Background.
- **libspe2 / IBM Cell SDK sim**: NOT usable on x86 (hardware control lib / abandoned full-system sim). Our 0-gap SPU lifter is the superior in-house asset.

Cheap wins to harvest: flOw NID database + SPU-PPU fallback registry pattern; PSL1GHT rev=0x02/sdk=0x330000 constants + struct sizes.
