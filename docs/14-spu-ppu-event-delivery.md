# docs/14 — SPU→PPU event delivery (SPURS USER events) — plan + decoded contract

Status: PLAN/SPEC (2026-06-30). Frontier #11 (docs/09 §15dd-§15ee). The boot reaches the
game's main loop after full engine init, then parks: a DLSpursManager coordinator waits on
cond 2 (a job-queue "enqueued" condition) whose producer never runs. The producers are the
SPURS service threads (SPURuntimeService on q=1, plus q=5/q=12), which are parked on
sys_event_queue_receive — because our HLE NEVER posts SPU→PPU events (0 port_send, 0
group-events all run). The SPURS kernel (which we HLE, not run) would normally post them.

Clean-room: RPCS3 = contract reference only.

## Decoded contract (the crux — from func_00C5DAB0 SPURuntimeService)

The service loops:
```
recv(queue = *(obj+0x48));   // returns r3=rc, r4=source, r5=data1, r6=data2, r7=data3
USER_KEY = 0xFFFFFFFF53505501   // SPURS user-event source
EXIT     = 0xFEE1DEAD            // stop sentinel
if (rc != 0)            -> stop (cleanup func_016BB504)
if (source == EXIT)     -> stop
if (source != USER_KEY) -> ignore, re-receive
b = (data2 >> 32) & 0xFF
if (b == 1) func_00C5D6D8(data1)   // handler B
else        func_00C5BE78(data1)   // handler A
```
So to wake the service + run a handler, post an lv2 event with:
- source = 0xFFFFFFFF53505501
- data2 high-dword low-byte (bits 32..39) = handler selector (0 -> A, 1 -> B)
- data1 = the handler's argument (meaning TBD — read func_00C5BE78/func_00C5D6D8)

This is the cellSpursEventFlag mechanism: an SPU workload sets a SPURS event flag ->
firmware posts a USER event to the attached lv2 queue -> the PPU event-helper dispatches.
Our cellSpursEventFlagSet only cond-broadcasts to cellSpursEventFlagWait waiters; it does
NOT post the USER event to the attached lv2 queue. And the SPU-side flag-set (the trigger)
isn't modeled.

## Infrastructure we already have
- sys_event_queue_push_by_id(qid, evt) [sys_event.c] — post an event to a queue.
- sys_event_port_send / connect_local [sys_event.c].
- cellSpursAttachLv2EventQueue / cellSpursEventFlagAttachLv2EventQueue [cellSpursSys.c] —
  STUBS; they don't record the flag/spurs -> queue binding (must fix to know which queue).
- cellSpursEventFlagSet — cond-broadcast only (must also post the USER event).

## Remaining unknowns to pin (small, vs the contract above)
1. data1/data2 meaning: read func_00C5BE78 (handler A) + func_00C5D6D8 (handler B) — what
   they expect in data1 (workload id? flag bits? a queue/handle?) and whether they
   enqueue a DLSpursManager job + signal cond 2 (the target cascade).
2. Queue binding: which lv2 queue the SPURuntimeService waits on (*(obj+0x48)) — likely q=1;
   and which queue each cellSpursAttachLv2EventQueue(q=3..12) call bound (record it).
3. Trigger: when an SPU task/workload signals (image-2/image-7 completion, or a workload
   event-flag set), map it to a USER-event post on the right queue.

## Build steps
1. Record bindings: in cellSpursAttachLv2EventQueue + cellSpursEventFlagAttachLv2EventQueue,
   store {spurs/eventFlag -> queue_id, port}.
2. Add a helper spurs_post_user_event(queue_id, selector, data1) that pushes
   {source=0xFFFFFFFF53505501, data1, data2=(selector<<32), data3=0} via
   sys_event_queue_push_by_id.
3. Trigger: on the SPU-side signal that maps to a workload event flag set (and/or on task
   completion), call spurs_post_user_event on the bound queue with the selector/data1 the
   handler expects (from step "unknowns 1").
4. Verify: the SPURuntimeService wakes, dispatches handler A/B, which enqueues a job +
   signals cond 2 -> the coordinator advances -> first frame / rendering.
5. Clean shutdown: post source=0xFEE1DEAD to stop services on teardown (nice-to-have).

## Verification
- Probe: post a USER event (source 0xFFFFFFFF53505501, selector 0, data1=<candidate>) to
  q=1 once and watch: does func_00C5BE78 run? does a cond_signal(2) appear? does the
  coordinator dequeue + the boot advance? (The service safely ignores non-matching sources,
  so a wrong source is harmless; a wrong data1 may misbehave in the handler — read it first.)
- A/B with the post disabled to confirm causation.

## Next concrete action
Read func_00C5BE78 / func_00C5D6D8 to pin data1 + confirm they reach cond 2; record the
attach bindings; then wire the trigger + post. The source key (0xFFFFFFFF53505501) and
dispatch are now known, which was the hard part.
