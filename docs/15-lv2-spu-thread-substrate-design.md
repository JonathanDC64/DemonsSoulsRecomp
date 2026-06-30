# docs/15 — lv2 SPU-thread substrate: design (Frontier #11 final build)

Status: DESIGN (2026-06-30). The boot reaches the game's main loop; the last blocker is the
DLSpursManager coordinator parked on cond 2, fed by the SPURS service (SPURuntimeService),
which never receives the SPU→PPU USER events because our HLE has no real lv2 SPU threads /
event ports and the SPURS kernel (which would send them) is HLE'd, not run. Synthetic events
were proven impossible (docs/14 §"DEFINITIVE"): handler B operates on real, stateful model
state (looks up the SPU thread by lv2 id in the service's registered list). So we must build
the real substrate. Clean-room: RPCS3 = contract reference only (no GPL copied).

## 0. The two-model bridge (the core architectural problem)

- Current SPU execution: `spu_workload` runs each SPURS *task* on a detached HOST thread
  (fingerprint → lifted entry → 256 KB LS). No lv2 SPU thread ids; no event ports. This is
  what got us to the main loop and MUST keep working.
- What SPURS expects: ONE lv2 SPU thread GROUP of N SPU threads (running the SPURS kernel),
  with lv2 ids, connected to an event queue via a port; the kernel sends USER events
  (sys_spu_thread_send_event) to the PPU service on workload/task state changes.
- Bridge: keep `spu_workload` as the executor, but (a) MINT real lv2 SPU thread ids at
  cellSpursInitialize and store them in the real CellSpurs layout so the PPU service
  registers them, (b) ASSOCIATE each running task with one of those lv2 thread ids, and
  (c) when a task signals (our existing 0xA70 taskset-syscall intercept, docs/09 §15y), POST
  the USER event as the kernel would, with that thread's lv2 id.

## 1. Real CellSpurs layout (RPCS3 cellSpurs.h offsets)

Our current `CellSpurs` is a simplified HLE struct (initialized/nSpus/flags/prefix) — WRONG
layout, so the game reads garbage for the SPU/event fields. Replace with a real-offset
accessor header (like libs/spurs/spurs_taskset.h), BE accessors only. Offsets (RPCS3):
```
0x00   wklReadyCount1[16]  atomic u8   ready count per workload 0-15
0x10   wklIdleSpuCountOrReadyCount2[16]
0x70   wklSignal1          be u16
0x80   wklState1[16]       u8          workload state 0-15
0xC9   spuPort             u8          SPU event port number (selector source!)
0xD0   wklState2[16]
0xD20  ppu0                u64         handler thread id
0xD28  ppu1                u64         event-helper thread id
0xD30  spuTG               u32         SPU thread-group id
0xD34  spus[0..7]          u32[8]      SPU thread lv2 ids   <-- service registers these
0xD5C  eventQueue          u32         lv2 event queue id
0xD60  eventPort           u32         lv2 event port id
0xD64  handlerDirty        u8 (atomic)
0xDB0  mutex (lwmutex)     ; 0xDC8 cond (lwcond)
```
NOTE: the game ALLOCATED the CellSpurs buffer (large); we must write these at the REAL
offsets, not our small struct. (Same fix pattern as the taskset, docs/09 §15q.)

## 2. cellSpursInitialize sequence (RPCS3 _spurs::initialize)

cellSpursInitializeWithAttribute must (using our existing lv2 handlers, which already mint
real ids — runtime/syscalls/lv2_register.c spu_alloc_group/thread):
1. sys_spu_thread_group_create(nSpus, prio)            -> spuTG  @0xD30
2. sys_spu_thread_initialize per SPU (0..nSpus-1)      -> spus[] @0xD34
3. sys_event_queue_create(SYS_PPU_QUEUE, size 0x2A)    -> eventQueue @0xD5C
4. sys_spu_thread_group_connect_event_all_threads(grp, q, portMask) -> spuPort @0xC9
5. sys_event_port_create(LOCAL); sys_event_port_connect_local(port,q) -> eventPort @0xD60
6. (handler thread @0xD20, lwmutex/lwcond @0xDB0/0xDC8 — minimal/stub initially)
nSpus for DeS = 2 (from AttributeInitialize log). We can call our lv2 handlers directly
in-process (they're C functions) rather than via the syscall ABI.

## 3. Event semantics (CONFIRMED, RPCS3 SPUThread.cpp / lv2/sys_event.h)

SPU `sys_spu_thread_send_event(spup, data0, data1)`:
- SPU writes SPU_WrOutMbox = data1, then SPU_WrOutIntrMbox = (spup<<24)|(data0 & 0xFFFFFF).
- lv2 posts to the queue connected to spup:
  { source=0xFFFFFFFF53505501 (SYS_SPU_THREAD_EVENT_USER_KEY),
    data1 = the SENDING SPU thread's lv2 id,
    data2 = (spup<<32) | (data0 & 0xFFFFFF),
    data3 = data1 (from OutMbox) }
- SPURuntimeService dispatch (func_00C5DAB0): selector = (data2>>32)&0xFF = spup; handler B
  (func_00C5D6D8, the WORK path that creates the next task) is selected by spup; it looks up
  the SPU thread by data1 (lv2 id) and uses data3 (payload).

Helper already built: `sys_spu_thread_post_user_event(qid, spu_lv2_id, spup, data0, data)`
(runtime/syscalls/sys_event.c) — encodes exactly this.

## 4. The trigger (where we post)

Two candidate trigger points; both may apply:
- (A) SPU task SPURS-syscall: our spu_indirect_branch already intercepts the taskset PM
  syscall at LS 0xA70 (docs/09 §15y, spu_spurs_taskset_syscall). On the relevant syscall the
  REAL kernel would send the USER event -> post it here.
- (B) SPU writes SPU_WrOutIntrMbox directly (raw sys_spu_thread_send_event from SPU code):
  handle it in spu_wrch (spu_channels.c) -> decode spup/data0 + the buffered OutMbox data1 ->
  post the USER event to the queue bound to spup.
Whichever the DeS SPU code actually uses drives it. The posting needs: (i) the SENDING task's
lv2 id (from the task↔thread association, §0c), (ii) the queue bound to spup (from the
connect in §2.4), (iii) data0/data3 the handler expects (OPEN — see §6).

## 5. Tests / correctness (the user asked)

- UNIT (libs/spurs/tests, like test_spurs_taskset_main.c, builds with clang standalone):
  - CellSpurs layout: writing spuTG/spus[i]/spuPort/eventQueue/eventPort via the BE accessors
    lands at the exact offsets 0xD30/0xD34+4i/0xC9/0xD5C/0xD60 (byte-exact, BE).
  - Event format: sys_spu_thread_post_user_event(q, lv2, spup, data0, data) produces
    source=0xFFFFFFFF53505501, data1=lv2, data2=(spup<<32)|(data0&0xFFFFFF), data3=data.
- INTEGRATION (boot run, env-gated SPURS_LV2THREADS=1):
  - cellSpursInitialize logs: group id, N thread ids, queue id, spuPort, written to the
    CellSpurs offsets; verify the service then registers the threads (its list non-empty).
  - On task signal, the posted USER event makes handler B FIND the thread (no list-miss) and
    NOT crash in the lookup; observe cond 2 getting signaled and the boot advancing.
  - A/B: SPURS_LV2THREADS unset = current behavior (boots to main loop) — REGRESSION GUARD.

## 6. Open questions (resolve during impl, cross-ref RPCS3 + the handlers)
- Q1 spup/selector: the kernel sends on spurs->spuPort (one port) -> selector = spuPort&0xFF.
  Handler B is selector 1. Does DeS's spuPort resolve to 1, or does the game use a distinct
  port for task-completion? Confirm by logging spuPort + which selector handler B needs.
- Q2 data0/data3 for the WORK event: what values make handler B create the CORRECT next task
  (vs the recovery panic). Read func_00C5D6D8 + cellSpursSpu.cpp send sites for the data.
- Q3 task↔lv2-thread association: assign each spu_workload dispatch one of spurs->spus[]
  (round-robin / by taskset). The lv2 id is what the event's data1 carries.

## 7. Risks + phasing
- RISK: a real SPU thread group could make the game USE the threads in ways our host-thread
  executor doesn't model -> regression. MITIGATION: flag-gate (SPURS_LV2THREADS), keep
  spu_workload as the executor (the lv2 threads are "registration shells" + event sources,
  not separate executors), A/B every step against the working boot.
- PHASES (each independently testable + committable):
  P1. Real CellSpurs layout header + unit test (no behavior change).  <-- START HERE
  P2. cellSpursInitialize creates group/threads/queue/port + writes the layout (flag-gated);
      verify the service registers the threads.
  P3. task↔lv2 association + post the USER event on the task syscall/IntrMbox trigger.
  P4. tune data0/data3 (Q2) until handler B creates the right task -> cond 2 -> first frame.

## 8. Refs
RPCS3: cellSpurs.cpp (_spurs::initialize 1068+, create_lv2_eq 513, create_event_helper 1309),
cellSpurs.h (offsets), cellSpursSpu.cpp (send_event 209, sites 1163/1237), lv2/sys_event.h
(SYS_SPU_THREAD_EVENT_USER_KEY), SPUThread.cpp (send_event 6037). PSL1GHT sys/spu.h.
Our side: libs/spurs/cellSpurs.{c,h}, spurs_taskset.h (layout pattern), runtime/syscalls/
lv2_register.c (spu group/thread handlers), sys_event.c (post helper), spu_channels.c
(SPU_WrOutIntrMbox, the 0xA70 syscall intercept).
