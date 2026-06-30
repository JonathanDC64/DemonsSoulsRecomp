# 09 — SPU / SPURS (the remaining gate)

Status (2026-06-29): the PPU side boots to its main loop (see `08-ppu-boot-complete.md`). The
boot is now gated on **SPU execution**: the `SPURuntimeService` PPU thread blocks forever on
`sys_event_queue_receive(q=1)` — the SPU-completion queue that only running SPU code can feed —
while the `PPURuntimeService` waits on a flag the SPU side would clear.

Good news: **ps3recomp already has a mature, tested static SPU recompiler + runtime**, and after
this session's additions it has **zero instruction gaps on all of Demon's Souls's SPU code**. SPU
is an *integration* job, not a from-scratch one.

---

## 1. What we have (ps3recomp SPU framework) — VALIDATED

- **SPU lifter**: `ps3recomp/tools/spu_lifter.py` (mirrors `ppu_lifter.py`). Input: SPU ELF (or raw
  LS image) via `--auto-functions`. Output: `spu_recomp.{h,c}` of `void spu_func_XXXX(spu_context*)`
  + a function table + `spu_recomp_register()`. Supporting tools: `extract_spu_images.py` (pull SPU
  ELFs from the EBOOT), `find_spu_functions.py` (boundary detection), `spu_disasm.py` (decoder,
  rebuilt against RPCS3's table). Validated originally on Uncharted; now on DeS too.
- **Runtime** `ps3recomp/runtime/spu/`:
  - `spu_context.h`: 128×128-bit `gpr` (u128 union: `_u32[4]/_u16[8]/_u8[16]/_f32[4]/…`), 256KB
    `ls`, pc, status, channels, MFC staging regs, atomic-reservation state.
  - `spu_helpers.h`: ~150 `static inline u128 spu_*()` instruction semantics (single source of truth,
    unit-tested). **Endianness**: registers hold values such that `_u32[i]` = word i; LS is BE and
    byte-swapped per 4-byte lane on access; within a word, `_u16[2i]`=low/odd halfword, `_u16[2i+1]`=
    high/even halfword on the LE host (this matters for mpy*/sumb/sign-extend).
  - `spu_channels.c`: `spu_wrch/rdch/rchcnt` — mailboxes (out/in/intr), signal-notify (SNR1/2),
    events (RdEventStat/WrEventMask/WrEventAck), decrementer, SRR0; MFC channels; atomic line
    reservations (GETLLAR/PUTLLC) with a global spinlock; indirect-branch dispatch registry
    (per-image via `image_id`); `spu_halt` longjmp landing pad for `br .` idioms.
  - `spu_dma.h`: synchronous MFC engine (GET/PUT, list/scatter-gather, tag completion, atomic cmds).
  - `spu_workload.{h,c}`: ELF→LS loader (`spu_elf_load_to_ls`), FNV-1a image fingerprint, a registry
    mapping fingerprint→lifted entry, and `spu_workload_dispatch(_async)`.
  - `spu_lifted_job.h`: `spu_run_lifted_job[_img|_abi]` — sets r1=0x3FFF0, r3 per ABI (simple-job:
    r3=args_ea; SPURS task: r3=0x00400000 marker + r3.word1=context EA), runs via `spu_run_with_halt`.
- **Syscalls** `runtime/syscalls/`: `sys_spu_initialize`, `sys_spu_thread_group_create/start/join/
  destroy/terminate`, `sys_spu_thread_initialize` (parses the SPU image ELF), `*_connect_event`,
  `*_write_ls/read_ls`, `sys_spu_thread_set_argument`, `sys_spu_thread_get_exit_status`.
  `spu_fallback.c`: a PPU-fallback registry (entry_point→host fn) that `sys_spu_thread_group_start`
  uses — if a fallback is registered it spawns a host thread running it, else completes instantly.
- **SPURS** `libs/spurs/cellSpurs.c`: `cellSpursAddWorkload`/`cellSpursCreateTask` funnel through
  `spu_workload_dispatch(_async)` (fingerprint→lifted entry; load ELF to fresh LS; run).
- **Tests** `runtime/spu/tests/`: out_sum, out_shufb, out_brsl_return, out_dma, out_taskset,
  out_workload, out_adapter, + `test_spu_helpers.c` (63 cases) — all passing.
- **Docs**: `ps3recomp/docs/SPU_LIFTER.md`, `SPU_FALLBACK.md`.

### Known stubs/limits in the SPU runtime (watch for these in DeS)
double-precision FP not lifted; SPU SPRs (`mfspr`) return 0; FPSCR no-op; decrementer stored but no
interrupt; list-DMA stall-and-notify parsed but no-op; halt-on-condition (`hgt*`/`heq*`) emitted as
no-ops. Validate via `--trace` diff against RPCS3 when a job misbehaves.

---

## 2. Demon's Souls SPU images (extracted)

`python ps3recomp/tools/extract_spu_images.py work/game/EBOOT.ELF --out work/spu_images` →
**12 SPU ELF images, ~858KB** (offsets in the EBOOT data section):

```
spu_0000 @01760280  45,448   spu_0004 @0178CC00  65,724   spu_0008 @017E9A00 142,292
spu_0001 @0176C200  68,796   spu_0005 @0179CD00  70,892   spu_0009 @0180C600  47,628
spu_0002 @0177CF00  32,404   spu_0006 @017AE200  95,996   spu_0010 @01818080  42,476
spu_0003 @01784E00  32,148   spu_0007 @017C5900 147,628   spu_0011 @01822680  67,140
```
They are stripped (no symbol/string identification). Expected mix: the SPURS kernel, the taskset
policy module, the system-service workload, and engine task binaries (zlib inflate, render command
processing, animation/physics, audio). Identifying which is which (by fingerprint, entry layout, or
SPURS GUIDs — `SPURS_GUID_SYS_WKL=0x1BB841BF38F89D33`, `SPURS_GUID_TASKSET_PM=0x836E915B2E654143`)
is an early task.

### ISA coverage: COMPLETE
Lifting all 12 with `--auto-functions` shows **0 real instruction gaps** (remaining "unsupported"
are `.word` data embedded in `.text`). This session added the only 3 DeS used that the lifter lacked:
`eqv` (XNOR), `sumb` (sum-bytes→halfwords), `mpyhhu` (mul high-halfword unsigned) — ps3recomp 456debd.
(`sumb` RA/RB→odd/even halfword mapping is best-effort; trace-verify vs RPCS3 when exercised.)

---

## 3. The SPU↔PPU contract (from RPCS3 — implement clean, don't copy)

- **lv2 event IDs**: queue/port `id_base = 0x8d000000`; `SYS_PPU_QUEUE=1`, `SYS_SPU_QUEUE=2`.
  `lv2_event = (source, data1, data2, data3)`.
- **SPU→PPU completion** (the thing the SPURuntimeService waits for): an SPU executes `STOP`
  (`0x0102` thread-exit / `0x0101` group-exit) or `sys_spu_thread_send_event`, and the kernel sends
  an event to the connected `lv2_event_queue` (via `sys_spu_thread_connect_event` port, or the group
  run/exception ports `ep_run`/`ep_exception`). Event source keys: `SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY
  =0xFFFFFFFF53505500`, `...EXCEPTION=…03`, `SYS_SPU_THREAD_EVENT_USER_KEY=…01`, `...DMA=…02`.
- **STOP-and-signal codes**: `0x0101` group exit, `0x0102` thread exit, `0x0110` receive event
  (blocking; SPU writes queue# to out-mbox first), `0x0111` try-receive, `0x0100` yield.
- **Channels**: out-mbox (28), in-mbox (29, 4-deep), intr-mbox (30), SNR1/2 (3/4); SPU event bits
  (`SPU_EVENT_TG=0x1` tag-group done, `SPU_EVENT_MB=0x10` inbox, `SPU_EVENT_S1/2`, …).
- **Image**: `sys_spu_image{ type, entry_point, segs, nsegs }`; `sys_spu_segment{ type COPY/FILL/INFO,
  ls, size, addr }`. Deploy = memcpy (COPY) / memset 0 (FILL) into LS at `seg.ls`.
- **SPURS**: instance 4096B (v1) / 8192B (v2); kernel LS entry `0x818`(k1)/`0x848`(k2); taskset PM
  entry `0xA00`, syscall `0xA70`; tasks live LS `0x3000..0x40000`. RPCS3 runs the SPURS kernel as
  REAL SPU code on its SPU threads (it does not fully HLE SPURS) and HLE-stubs the cellSpurs PPU API.
  RPCS3 SPU core files (`SPUThread.cpp`, recompiler) are NOT in our sparse checkout — add them when
  needed: `git -C C:\dev\rpcs3-reference sparse-checkout add rpcs3/Emu/Cell` (then SPUThread.cpp,
  SPUInterpreter, SPURecompiler).

---

## 4. The gate, precisely

- `[SPU] initialize(nspu=6, nrawspu=0)` runs (our `sys_spu_initialize`).
- **SPURuntimeService** thread (OPD `0x01970970` → `func_00C5DAB0`): first thing it does is syscall
  `0x82` = `sys_event_queue_receive(q=1, timeout=0)` → BLOCKS (queue 1 = SPU completion; nothing
  feeds it because no SPU executes).
- **PPURuntimeService** (OPD `0x0196FAF8` → `func_00C3A878` → `func_00C3A358`): sets a flag
  `*(0x01B91E20)`, then pumps `usleep(16666)`+`cellSysutilCheckCallback` waiting for it to clear —
  cleared after the SPU side completes a task. So PPU waits on SPU; SPU service blocks on the empty
  queue → stall.

For DeS the gate is **fundamental**: SPURS tasks do real work (zlib inflate of game data, render
command-list processing). A bare "completion event" shim won't help — the game needs the SPU's
actual OUTPUT in memory.

---

## 5. Integration plan (proposed order)

1. **Identify the 12 images.** Fingerprint each (FNV-1a, matching `spu_workload`); find the SPURS
   kernel (entry/size, GUID), the taskset PM, the system-service workload, and the task binaries.
   Trace which images the game actually loads first (instrument `sys_spu_thread_initialize` /
   `cellSpursAddWorkload` / `cellSpursCreateTask` to log image EA + fingerprint).
2. **Lift the needed images** (`spu_lifter.py --auto-functions`), build them in, and register each
   with `spu_workload_register_img(fingerprint, entry, image_id, name)` at startup.
3. **Wire the SPURS path end-to-end.** Decide HLE-vs-recompile for the SPURS kernel itself:
   - Option A (RPCS3-style): run the recompiled SPURS kernel on a host "SPU thread" via the
     sys_spu_thread_group path; it schedules workloads/tasks within LS.
   - Option B (more HLE): HLE the cellSpurs PPU API + a minimal scheduler that dispatches each
     workload/task image directly through `spu_workload_dispatch`, bypassing the on-SPU kernel.
   Start with whichever gets the first real task (likely a system/init task) to run and post its
   completion event so the SPURuntimeService unblocks.
4. **Make the SPU→PPU completion real.** When a dispatched SPU job finishes (STOP 0x0102 / send_event),
   post the matching `lv2_event` to event queue 1 so `sys_event_queue_receive` returns and the
   PPURuntimeService flag clears. Honor `sys_spu_thread_connect_event` / group event ports.
5. **Verify with `--trace`.** For any job whose output looks wrong, diff our per-instruction SPU
   trace against an RPCS3 SPU trace of the same image to pinpoint a bad helper (the known-risky ones:
   `sumb` even/odd, double-precision, FPSCR-dependent rounding).
6. **Iterate** task-by-task toward the title screen / first frame.

### Watch-outs
- Multi-image LS overlap (SPURS kernel + policy + task occupy overlapping LS ranges) — use
  `image_id` so `spu_indirect_branch` resolves the right function table.
- DMA EA correctness: SPU jobs DMA from/to main memory at guest EAs — `vm_base` must be the same VM
  the PPU uses; results must land where the PPU reads them (big-endian).
- The SPURS kernel uses atomic lock-line ops (GETLLAR/PUTLLC) heavily for the workload queue — the
  runtime's spinlock-backed reservations must be correct across host threads.

### First concrete experiment
Instrument the SPURS/`sys_spu_thread_*` path to log every SPU image the game loads + its fingerprint
+ entry, identify the first one the SPURuntimeService is waiting on, lift+register it, and get its
completion event delivered to queue 1. That single loop closing is the SPU-side equivalent of the
PPU "reaches main loop" milestone.

Reference clone: `C:\dev\rpcs3-reference`. Memory: `demons-souls-frontier-rsx-gcm`,
`demons-souls-rpcs3-differential`, `demons-souls-rpcs3-crossref`.

---

## 7. Entry point PINPOINTED (2026-06-29) — where the SPU integration begins

Tracing the boot SPURS sequence (YDKJ_HLETRACE), the game does, in order:
`sys_spu_initialize(6)` -> `_cellSpursAttributeInitialize` -> `cellSpursAttributeSetNamePrefix`
-> **`cellSpursInitializeWithAttribute`** -> `_cellSpursEventFlagInitialize`
-> **`cellSpursEventFlagAttachLv2EventQueue`** (attaches the SPURS event flag to lv2 **queue 1**).
Then the **SPURuntimeService** PPU thread blocks on `sys_event_queue_receive(q=1)`.

Current state of our HLE:
- `cellSpursInitializeWithAttribute` (libs/spurs/cellSpurs.c) is a PURE STUB: it records attrs and
  returns CELL_OK. It does NOT create/start the SPURS kernel SPU thread group. (i.e. we're in the
  plan's "Option B": no on-SPU SPURS kernel; workload images are meant to be dispatched directly.)
- The only SPU dispatch in cellSpurs.c is in cellSpursAddWorkload/CreateTask -> `spu_workload_dispatch_async`.
- MISSING: the **completion wiring** — when an SPU workload/task finishes (or signals the SPURS event
  flag), nothing posts an event to the attached lv2 queue (queue 1), so SPURuntimeService never wakes.

So the unblock chain to build is: game adds a workload/task -> we dispatch the lifted SPU image ->
on completion (or on the SPU signaling the SPURS event flag) -> POST an lv2_event to the queue that
`cellSpursEventFlagAttachLv2EventQueue` bound (queue 1) -> `sys_event_queue_receive` returns ->
SPURuntimeService runs -> clears the PPURuntimeService flag (*(0x01B91E20)). Closing that single
loop is the SPU-side "reaches main loop" milestone.

NEXT CONCRETE STEPS:
- a. In `cellSpursEventFlagAttachLv2EventQueue`, record the (event-flag -> lv2 queue id) binding.
- b. Implement `cellSpursEventFlagSet` / the SPU-side event-flag signal so it posts an lv2_event to
     the bound queue (queue 1). (Check what the SPURS event-flag wait/set semantics are vs RPCS3.)
- c. Trace whether/when the game calls cellSpursAddWorkload/cellSpursCreateTaskset/CreateTask after
     init (it may be driven from another thread or gated on the service running). Identify the first
     workload/task image (fingerprint) the service depends on.
- d. Dispatch that first image via spu_workload_dispatch, have it (or a temporary shim) signal the
     event flag -> queue 1, and confirm SPURuntimeService unblocks.
- e. Decide whether to keep Option B (direct dispatch + HLE the SPURS kernel's scheduling) or run the
     recompiled SPURS kernel on a real SPU thread group (Option A, RPCS3-style). Option B is likely
     faster to first-pixels; Option A is more faithful for complex taskset scheduling.

---

## 8. Architecture DECISION forced (2026-06-29): HLE the SPURS kernel (Option B)

Evidence: of the 12 EBOOT SPU images, **11 have entry at LS 0x3070-0x3090** (= `CELL_SPURS_TASK_TOP
0x3000`) → they are **SPURS task binaries**; `spu_0010` (entry 0x100) is a policy/workload module.
**None** is the SPURS kernel (no `SPURS_GUID_*`, no `0x818/0x848` kernel entry). The SPURS kernel
(the on-SPU scheduler) ships in **cellSpurs.prx (firmware)** — which we do NOT have and (clean-room)
do NOT want. => **We cannot run the real SPURS kernel; we must HLE the SPURS scheduler/runtime** and
execute only the game's task/policy images (which lift with 0 gaps). RPCS3 takes Option A (it runs
the firmware kernel as real SPU code); we are forced to Option B.

## 9. The SPURS bring-up handshake (why boot stalls before any task)

Boot SPURS calls observed (HLETRACE), then it STOPS — no CreateTaskset/CreateTask/AddWorkload:
`_cellSpursAttributeInitialize` -> `cellSpursAttributeSetNamePrefix` -> `cellSpursInitializeWithAttribute`
-> `_cellSpursEventFlagInitialize` -> `cellSpursEventFlagAttachLv2EventQueue` (binds to lv2 queue 1).
Three game threads then deadlock waiting for SPURS to come "up":
- **SPURuntimeService** (func_00C5DAB0): loops `sys_event_queue_receive(q=1, infinite)`; dispatches
  events whose **source == 0xFFFFFFFF53505501** (`SYS_SPU_THREAD_EVENT_USER_KEY`) to handlers
  func_00C5BE78 / func_00C5D6D8; exits on a `0xFEE1DEAD` game sentinel. It needs SPU USER events.
- **PPURuntimeService** (func_00C3A878 -> func_00C3A358): sets flag `*(0x01B91E20)=1`, pumps
  usleep+cellSysutilCheckCallback until it clears (cleared after SPU-side work completes).
- A thread on **`cond_wait(cond=3, infinite)`** — the init thread waiting for the services/SPURS to
  reach a ready state.
Our `cellSpursInitializeWithAttribute` is a pure stub (records attrs, returns OK) — it never brings
the SPURS system "up", so nothing ever drives those three waits.

## 10. Revised plan (Option B = HLE SPURS)

The job is to HLE the SPURS runtime well enough that the game's task images run and the PPU-side
handshake completes. Build order:
1. **Make `cellSpursInitializeWithAttribute` actually bring SPURS "up"** in HLE terms: record the
   instance, set up the workload table, and start a HOST "SPURS service" — a thread (or callback)
   that plays the role of the firmware kernel + PPU handler: it watches for added workloads/created
   tasks and, when one runs/completes, posts the right lv2 event.
2. **Wire `cellSpursEventFlagAttachLv2EventQueue`** to record (event-flag -> lv2 queue id=1) so our
   service knows where to post USER events (source 0xFFFFFFFF53505501) the SPURuntimeService consumes.
3. **Reverse the USER-event payload format** the SPURuntimeService expects (decode func_00C5BE78 /
   func_00C5D6D8: which fields in data1/2/3 select the workload/handler). This is required to post a
   *correct* event, not a blind one.
4. **Identify the init handshake**: who waits on cond=3 and what signal releases it (likely the
   SPURS handler/service reaching ready, or a first system-workload event). Make our HLE service
   produce exactly that so the init thread proceeds to actually create tasks.
5. **Task execution**: when the game creates tasks (cellSpursCreateTaskset/CreateTask), dispatch the
   matching image (one of the 12) via `spu_run_lifted_job_abi(..., spurs_task_abi=1)`; run its DMAs;
   post the completion USER event. Validate output via `--trace` vs RPCS3 where needed.

RISK/NOTE: HLE-ing SPURS is real work — the game (Dantelion2) wraps SPURS in its own runtime-service
layer with its own event protocol (the 0x53505501 USER events + 0xFEE1DEAD sentinel + cond=3
handshake). Step 3 (decode the event payload) and step 4 (the cond=3 release) are the two reverse-
engineering gates before tasks can run. The SPU *execution* itself is the solved part (lifter +
runtime ready; 0 ISA gaps).

### 9a. cond=3 never signaled (the concrete init block)
SYNCTRACE confirms: cond id 3 is `COND_WAIT`-ed exactly once and **never signaled** for the whole
run. So the SPURS init thread blocks on cond=3 forever. On real PS3 the cellSpurs PPU
handler/event-helper thread (created inside cellSpursInitializeWithAttribute) reaches "ready" and
signals it. Our stub creates no such thread → permanent block. This makes the **first concrete HLE
target** clear: `cellSpursInitializeWithAttribute` must stand up a minimal HLE "SPURS service" that
(a) releases whatever the init thread waits on (cond=3) and (b) later feeds USER events to queue 1.
Open RE question: confirm cond=3's waiter (which guest fn) and the exact ready-condition it re-checks
after wake, so the HLE signal is honored rather than spuriously looping. (Probe sys_cond_wait's
g_active_ctx->cia to get the waiter; correlate with the Dantelion2 SPURS-init function.)

---

## 11. Firmware question RESOLVED — HLE the SPURS kernel (no firmware needed) [2026-06-29]

Q: "firmware" = PS3 system software (cellSpurs/libsre.sprx live in PS3UPDAT.PUP, freely downloadable
from Sony). Is the real SPURS kernel obtainable? A: yes, but **RPCS3 does NOT use it.**

RPCS3 deep-dive (Emu/Cell/Modules/cellSpurs.cpp + cellSpursSpu.cpp): SPURS is **pure HLE**.
`cellSpursInitializeWithAttribute` builds a SYNTHETIC sys_spu_image (segs=null, nsegs=0, entry
0x818/0x848) — NO firmware load, no sys_spu_image_open. The SPURS kernel/sys-service/taskset-PM are
reimplemented in C++ (`spursKernelEntry`, `spursSysServiceEntry`, `spursTasksetEntry`,
`spursTasksetSelectTask`). Game task ELFs are parsed (`spursTasksetLoadElf`) and run as real SPU
code; workload completion -> PPU via `sys_event_port_send(eventPort, 0,0, (1u<<31)>>wid)` ->
event-helper thread -> registered handlers. **SPURS games run on RPCS3 with no firmware installed.**

Decision for our port: **HLE the SPURS kernel (Option B confirmed).** Rationale:
- It's what the gold-standard emulator does; no firmware required.
- Aligns with this project's goals: clean-room (no Sony code in the tool), no decryption keys,
  plug-and-play. (Option A — lift the firmware kernel — would need user firmware + keys at build and
  would put recompiled Sony code in the tool. Not chosen.)
- Our SPU recompiler already runs the GAME's 12 task images (0 ISA gaps); only the *scheduler* is
  HLE'd. We reimplement it CLEAN (reference RPCS3's behavior/contract, write our own C — RPCS3 is GPL).
- Precedent: `sp00nznet/flow` (flOw) is a shipped ps3recomp port of a SPURS game.

Reference contract (RPCS3 cellSpursSpu.cpp): kernel entry 0x818(k1)/0x848(k2); sys-service workload
embedded at LS 0x100; taskset PM entry 0xA00 / syscall 0xA70; tasks LS 0x3000+; PPU notify =
`sys_event_port_send` with bit `(1u<<31)>>wid` per workload id. To add RPCS3 SPU core / cellSpursSpu
to the reference clone: `git -C C:\dev\rpcs3-reference sparse-checkout add rpcs3/Emu/Cell` (already
has Modules; cellSpursSpu.cpp is under Modules).

---

## 12. HLE SPURS implementation spec (the build plan) [2026-06-29]

The bring-up is a circular dependency, confirmed by tracing: **`cond=3` (init thread, never
signaled) ← the Dantelion2 runtime services reaching "ready" ← SPURS producing events ← the (HLE)
SPURS kernel + the game's tasks running.** So there is NO shim shortcut; we must implement enough of
the SPURS runtime that events flow. Current state of our HLE (`ps3recomp/libs/spurs/cellSpurs.c`,
991 lines): attribute/init/eventflag funcs are bare stubs; `cellSpursInitializeWithAttribute` records
attrs only; `cellSpursEventFlagAttachLv2EventQueue` is a no-op; workload table + CreateTaskset exist;
`cellSpursAddWorkload`/`CreateTask` funnel to `spu_workload_dispatch_async`. Reference to reimplement
clean: RPCS3 `Emu/Cell/Modules/cellSpursSpu.cpp` (+ cellSpurs.cpp).

Dantelion2 service contract (from RE this session):
- SPURuntimeService (func_00C5DAB0): `sys_event_queue_receive(q=1)`; dispatches events with
  **source == 0xFFFFFFFF53505501** (SYS_SPU_THREAD_EVENT_USER_KEY) to func_00C5BE78/func_00C5D6D8;
  byte `(data2>>24)&0xFF` selects handler vs the value 0x20; exits on source **0xFEE1DEAD**.
- PPURuntimeService (func_00C3A878 -> func_00C3A358): sets flag *(0x01B91E20)=1, pumps
  usleep+cellSysutilCheckCallback until it clears.
- init thread: `sys_cond_wait(cond=3)` via DLConditionVariable::wait (func_00B5D3E8), waiting for
  services/SPURS ready.

### Phased build
**Phase A — SPURS "comes up" (release cond=3).** Make `cellSpursInitializeWithAttribute` stand up an
HLE SPURS instance: create the lv2 event queue/port infra, and start a host "SPURS service" thread
that emulates the kernel+PPU-handler. Goal: drive whatever the init thread's cond=3 waits on. First
confirm cond=3's exact ready-predicate (re-RE func_009E0370 / the waiter — its host backtrace was
trampoline-scrambled; use cia-stamping at the cond_wait + read the predicate loop, or single-step
the waiter). If cond=3 is "wait for SPURuntimeService ready," have the service signal it (sys_cond_signal)
once stood up.
**Phase B — event delivery.** Implement `cellSpursEventFlagAttachLv2EventQueue` to record the
(event-flag -> lv2 queue) binding, and a `spurs_post_user_event(queue, payload)` that posts an event
with source 0xFFFFFFFF53505501 to the bound queue (RPCS3 uses `sys_event_port_send(port,0,0,
(1u<<31)>>wid)` for workload-ready). Decode func_00C5BE78/func_00C5D6D8 to learn the exact payload
(which fields select the workload/handler) so posted events are correct, not blind.
**Phase C — task execution.** On cellSpursCreateTaskset/CreateTask, dispatch the matching game image
(one of the 12) via `spu_run_lifted_job_abi(..., spurs_task_abi=1)`; run its DMAs; on STOP/exit post
the completion USER event so the service dispatches the PPU callback and clears the PPURuntimeService
flag. Validate task output via `spu_lifter --trace` diff vs an RPCS3 SPU trace when a result looks wrong.
**Phase D — iterate** task-by-task toward the title screen / first rendered frame.

### Pre-req RE tasks (do these first; they unblock the build)
1. Pin cond=3's waiter + ready-predicate (what value/flag, who must set it).
2. Decode the SPURuntimeService USER-event payload (func_00C5BE78 / func_00C5D6D8).
3. Confirm where lv2 queue 1 is created (game vs cellSpursEventFlagAttachLv2EventQueue) and the
   event-flag<->queue relationship.
These three are pure analysis (no firmware, no new infra) and define exactly what Phase A/B must emit.

ESTIMATE: this is a multi-session build (the SPURS runtime + Dantelion2 wrapper handshake is real
work). The SPU *execution* substrate is done (lifter 0-gap, runtime tested); the remaining effort is
the scheduler/event HLE + the per-task bring-up. No quick unblock exists (circular dependency).

---

## 13. Contract details RE'd for the build [2026-06-29] (Phase A/B/C inputs)

Hard data gathered this session (so the build doesn't re-derive it):
- **lv2 queues at boot** (our `[evt] queue_create`): id=1 **key=0x1** size=127 (the SPU->PPU USER-event
  queue the **SPURuntimeService** drains), id=2 key=0x10000 size=127. **The GAME creates queue 1
  itself** (not cellSpursEventFlagAttachLv2EventQueue) — so events must be posted INTO queue 1.
- **SPURuntimeService event protocol** (func_00C5DAB0): `sys_event_queue_receive(q=1)` returns
  (r3=res, r4=source, r5=data1, r6=data2, r7=data3). Logic:
  - r3 != 0 (recv error) -> exit/cleanup (loc_00C5DB68).
  - source(r4) == 0xFEE1DEAD -> exit (game shutdown sentinel).
  - source(r4) != 0xFFFFFFFF53505501 -> ignore, loop.
  - else msg-type byte = `(data2>>24)&0xFF`: ==1 -> func_00C5D6D8(r31, data1); ==0x20 -> func_00C5BE78(
    r31, data1); else loop. (r31 = service context object; data1=r5, data3=r7 passed through.)
  So a valid SPU-user event = source 0xFFFFFFFF53505501 + data2 high byte in {1,0x20}. Posting one
  drives the service ONCE; what it then does (func_00C5D6D8 / func_00C5BE78) decodes the workload.
- **conds**: 1/2/3 all created by func_009DFDA8 (generic DLConditionVariable ctor), unnamed; cond 3
  paired with mutex 1093, never signaled -> the init-thread block. (The signal comes from a thread
  that's itself blocked on the SPU side -> circular.)
- **sys_event send infra** exists: `g_sys_event_queues[qid-1].buffer[tail]` + the port-send path
  (sys_event.c ~L370-413) — reusable to post events into queue 1 from the HLE SPURS service.

VERIFIED: no shortcut. Spurious-waking cond=3, or posting a blind event, won't pass the waiters'
predicates — they need REAL SPU task results flowing via queue-1 events. So Phase A cannot be faked;
it requires the HLE SPURS service to run a task (or the system-service workload) and post a genuine
completion/notify event. The SPU execution substrate (lifter 0-gap, runtime) is ready; the build is
the SPURS scheduler + event delivery + per-task bring-up. This is a multi-file, coordinated
implementation (RPCS3 does it in ~5k lines of cellSpursSpu.cpp) — the realistic next phase of the project.

---

## 14. Thread map + spurious-wake experiment [2026-06-29]

Spurious-wakeup experiment (YDKJ_CVWAKE: convert infinite cond-waits to 1.5s spurious wakeups —
semantically legal): RULED OUT as a shortcut. cond=3's waiter wakes, calls a sys_net function
(NID 0xFDB8F926 = func_016B9A64), then re-waits — its predicate is never satisfied. No PPU-ordering
shortcut exists; the boot needs the real SPURS/SPU work. (Reverted the experiment.)

Definitive thread map (SYNCTRACE tid correlation):
- **tid 22992 = MAIN thread** -> blocks on `sys_cond_wait(cond=3)` (mutex 1093). When woken it does
  sys_net (NP/online) init then re-waits. This is THE critical boot block: the main thread waiting
  for the runtime services to reach "ready".
- **tid 30932 = SPURuntimeService** (func_00C5DAB0) -> blocks on `sys_event_queue_receive(q=1)`
  waiting for SPU USER events.
- **PPURuntimeService** (func_00C3A878) -> usleep+cellSysutilCheckCallback pump, waiting on flag
  *(0x01B91E20).
- HighGraphics (func_00C3A878) -> ran, returned.

So: main(cond=3) waits for services ready <- services (SPURuntimeService q1, PPURuntimeService flag)
wait for SPU events/completion <- needs the SPU thread group executing SPURS tasks. Circular on real
SPU work, confirmed three ways now. The HLE SPURS build (docs/10 blueprint) is the only path; there
is no shim. cond=3 is critical (main thread), not a discardable background service.

---

## 15. SPU integration: all 12 images lift + dispatch HITs (DeS SPU code runs natively) [2026-06-29]

The boot now reaches an SPU workload dispatch on the graphics path (func_00A31158, after the GCM
control-register fix). Integrated the SPU side:

- **All 12 DeS SPU images lift cleanly** — `scripts/build_spu.py` runs `spu_lifter.py --symbol-prefix
  <img>_` on each `work/spu_images/*.elf` into `ps3recomp/work_spu/<img>/spu_recomp.c` (~8,000 SPU
  functions total). The only "unsupported" per image is `.word` = data embedded in .text (0 real ISA
  gaps — confirms docs/09 across the whole set).
- **Registration** — `build_spu.py` also generates `ps3recomp/work_spu/_gen/spu_workloads.cpp`, which
  registers each image with the workload-dispatch registry by FNV-1a-64 fingerprint
  (`spu_workload_register_img`) via a startup constructor. `port/CMakeLists.txt` globs the lifted
  `spu_recomp.c` + that file into des_boot. (work_spu/ is game-derived → gitignored in the fork.)
- **Result: the dispatch HITs and DeS SPU code executes natively.** Log:
  `[spu_workload] dispatch HIT (async) fp=0x4FBCAB40231BF72B args=0x00000000 image=2 -> spawning thread`
  (fp matches **spu_0001**, the 68KB image the game dispatched). This is the first time recompiled DeS
  SPU code runs.

**New SPU frontier (two issues):**
1. **Null task context** — `args=0x00000000`: the SPU job is dispatched with no context EA, because
   func_00A31158's cellSpurs setup is incomplete (the 3 unresolved cellSpurs NIDs 0x8F122EF8 /
   0x34552FA6 / 0xA121A224 return 0). Need to implement those + the SPURS task/job ABI so the job gets
   its real context (and can later signal completion to release func_00A31158's wait).
2. **SPU indirect-branch targets unresolved** — `[SPU] indirect branch to unknown LS address 0x12C28
   / 0x04488 (image 2)`: the SPU lifter doesn't register some computed-branch targets (SPU jump
   tables / function pointers) — the SPU analog of the PIC jump-table gap fixed on the PPU side
   (ps3recomp 430ced0). Likely needs the same treatment in `spu_lifter.py` (discover SPU jump tables /
   register indirect-branch targets), and/or the null context is producing garbage targets.

NEXT: pin whether the unknown branches are a lifter gap or a consequence of the null context; implement
the cellSpurs NIDs + task/job context so the job runs against real data and signals completion.
Reproduce: `python scripts/build_spu.py` then rebuild port.

### 15a. Null-context root diagnosis [2026-06-29]

Traced the `args=0` from the SPU dispatch back through the cellSpurs task setup (HLETRACE +
the cellSpurs printfs). func_00A31158's sequence: cellSpursCreateTasksetWithAttribute (0xC10931CB) ->
`_cellSpursTaskAttributeInitialize` (0xB8474EFF) -> cellSpursCreateTaskWithAttribute (0x1D46FEDF) ->
dispatch. The attribute-init captured:
  `_TaskAttributeInitialize(eaElf=0x0177C200 ctx=0x00000000 szctx=0xD0030000)`
eaElf is correct (so the arg mapping is aligned), but **eaContext=0** and **sizeContext=0xD0030000**
(a garbage stack address). So the game itself passed a null context + uninitialized size — its context
computation/allocation is broken upstream.

The break is the **3 unresolved cellSpurs NIDs** func_00A31158 calls (via wrappers func_016B9E64 ->
0x8F122EF8, func_016B9CE4 -> 0x34552FA6, func_016B9EE4 -> 0xA121A224), which return 0 (unimplemented).
One of them almost certainly computes/writes the task context-save-area size (cf RPCS3's
`cellSpursTaskGetContextSaveAreaSize`); returning 0 without writing the out-param leaves the size as
stack garbage (0xD0030000) and the context allocation null. These NIDs are unnamed (not in RPCS3's
table, no name in EBOOT.imports.json — only library=cellSpurs), so they need RE from func_00A31158 +
the cellSpurs API. Implementing them (so the task gets a real context) is the prerequisite for the SPU
job to run on real data and signal completion (releasing func_00A31158 -> the main thread's cond=3).
The "unknown LS branch" targets are most likely downstream of the garbage context, not a lifter gap —
re-check after the context is real.

### 15b. The 3 NIDs identified; real blocker is the task context, not them [2026-06-29]

Recovered the 3 "undocumented" cellSpurs NIDs by reverse-hashing candidate SDK names against the PS3
NID scheme (NID = first 4 LE bytes of SHA1(name + 16-byte suffix); same as RPCS3's ppu_generate_id).
Tool: `scripts/nid_name.py`. Results:
- 0x8F122EF8 = **cellSpursTasksetAttributeSetTasksetSize**
- 0x34552FA6 = **cellSpursTaskExitCodeInitialize**
- 0xA121A224 = **cellSpursTaskAttributeSetExitCodeContainer**

These are attribute setters / exit-code-container init — NOT context-size functions, so they do NOT
cause eaContext=0 (correcting §15a's guess). Returning them as named no-op stubs is ~equivalent to the
current unresolved->0, so they're not the boot blocker (implement for hygiene only).

**Real blocker:** the game calls cellSpursCreateTask with **eaContext=0**, which in SPURS means "let
SPURS allocate the task's context save area from the taskset pool." Our HLE passes that 0 straight
through, so the SPU task ABI hands the job r3.word1 (eaContext) = 0; the task entry (which checks the
0x40 marker then DMAs 64 bytes from eaContext — the ABI we use is correct) DMAs from address 0 ->
garbage descriptor -> garbage computed branches (the "[SPU] unknown LS address" targets are downstream
of this, NOT a lifter gap).

**FIX (next):** in cellSpursCreateTask (libs/spurs/cellSpurs.c), when eaContext==0, allocate a guest
context save area and synthesize a valid initial task context (per docs/11 §3: TaskInfo/context layout)
before dispatch, so the SPU job DMAs a real descriptor. This is the SPURS task-context HLE — the core
of docs/11 §3. cellSpursTaskGetContextSaveAreaSize gives the size from the ELF's LS pattern.

## How RPCS3 handles "undocumented" cellSpurs functions (for reference)
RPCS3 maps NID->function by NAME: REG_FUNC(cellSpurs, name) computes the NID via ppu_generate_id(name),
so it only "knows" a function whose name is in its source. For NIDs whose name it lacks it logs a
missing/unknown import and the call returns to a stub. Many cellSpurs PPU functions in RPCS3 are
UNIMPLEMENTED_FUNC stubs (or commented out) -- RPCS3 gets away with that because it runs the REAL SPURS
SPU kernel as actual SPU code, so the heavy lifting is on the SPU side, not the PPU API. We can't stub
as freely: we HLE the SPURS scheduler, so PPU-side cellSpurs functions that set up real SPU state
(context, taskset) must actually work. "Undocumented" is a misnomer -- they're documented SDK
functions; the names just weren't in RPCS3/our tables, recoverable via scripts/nid_name.py.

### 15c. SPU job blocker is a SPU-lifter jump-table gap (not the context) [2026-06-29]

Auto-allocating the task context (eaContext==0 -> zeroed guest buffer) was implemented
(cellSpursCreateTask) and is correct SPURS behavior, but the SPU job hits the SAME branches
(0x12C28, 0x04488) with the zeroed context as with the null one — so they are computed from the SPU
CODE (a jump table), not the context. Confirmed: neither 0x12C28 nor 0x04488 is a lifted function in
spu_0001 (they're mid-function: 0x04488 is inside func_00004464); the SPU lifter has NO jump-table /
computed-`bi` target discovery (unlike ppu_lifter's discover_jump_tables). So spu_indirect_branch can't
resolve them -> halt.
**FIX (next):** port jump-table discovery to spu_lifter.py — find the SPU switch pattern (load table
base via ila/ai + index, lqx/lqd the target, `bi`), read the table, and seed those targets as function
entries (so spu_recomp_register registers them and spu_indirect_branch resolves). Analog of PPU
430ced0. Then re-check the SPU job runs to completion + signals (releasing func_00A31158).

### 15d. SPU job now executes (fn-ptr discovery); next = completion handshake [2026-06-29]

Two spu_lifter fixes (fork): (1) conditional indirect branch to $r0 = conditional return; (2)
function-pointer target discovery — scan non-text PT_LOAD segments for in-text 4-aligned BE words
(SPU vtables, called via `bisl rX` with rX from LS data) and seed them as function starts. Verified:
the vtable at vaddr 0x13180 (file 0x10280) holds 0x04488/0x12C28; these were in data GAPS the boundary
detector skipped, so seeding adds them as new functions. Result: the dispatched SPU job (spu_0001) no
longer halts on "unknown LS address" — **it runs.**
But the boot doesn't advance yet: func_00A31158 keeps polling (usleep(100) loop). The SPU job either
runs without completing or completes without signaling. **NEXT = SPU->PPU completion handshake:** the
SPU job, on finishing, must signal via the mechanism func_00A31158 polls (a queue/flag/event — cf the
SPURS event protocol docs/09 §13, docs/11 §4). Determine func_00A31158's exact wait predicate (what it
polls) and make the SPU job's completion satisfy it. (Also verify the async SPU thread actually reaches
STOP vs. spinning on a mailbox/DMA it expects from the PPU.)

### 15e. SPU job runs but doesn't complete — needs the SPURS kernel framework [2026-06-29]

After fn-ptr discovery the SPU job (spu_0001) executes but never RETURNs (no completion log) within a
run, and the spin detectors show it is NOT busy-polling a channel (rdch/rchcnt). So it's a memory/state
loop, not a channel wait. Root: we dispatch the task ELF DIRECTLY (spu_workload), bypassing the SPURS
taskset policy module + kernel. RPCS3 instead runs the real taskset PM as SPU code, which: sets up the
task's real context, drives its state machine, feeds work, handles CELL_SPURS_TASK_SYSCALL_EXIT (writes
task_exit_code[taskId] to PPU memory + runs the on-exit hook). With a synthesized zeroed context and no
kernel driving it, our task loops in its own dispatch/wait logic and never reaches a valid exit.
**So completing this is the full HLE SPURS task framework (docs/11 §3-4):** stand up the taskset
PM/kernel behavior (taskset context @LS 0x2700, task select/dispatch, the task-start ABI r3=args
r4={tasksetEA,args}, syscall handling incl. EXIT) so the task runs to a real exit, writes its exit code
where the PPU polls it (the exit-code container, cellSpursTaskExitCodeTryGet), releasing func_00A31158.
This is the substantial M3 work; the SPU recompiler + dispatch + fn-ptr discovery (this session) are the
substrate it builds on. RPCS3 sidesteps all of it by running the firmware kernel as real SPU code; our
clean-room path must HLE the kernel.

### 15f. Task-START ABI + real argument [2026-06-29]

The SPU task entry (spu_0001 @0x3070) is finite: save r3(arg)/r4 -> init(func_00003010) -> restore ->
main body func_000053C8 -> STOP. The body extracts fields out of r3 (the task argument quadword:
`r3=arg; r4=rotqbyi(arg,4); call func_00004498(r3,r4)`), so a null r3 makes it loop. We were
dispatching with the kernel-marker ABI (r3={0x40,...}); fixed to the task-START ABI:
spu_run_lifted_job_abi mode 2 (r3 = 16-byte arg, r4 = tasksetEA), new spu_workload_dispatch_task, +
capture the argument EA in _cellSpursTaskAttributeInitialize (new eaArgument field) and read it in
cellSpursCreateTask. Verified the task now gets its real arg: {0x00A662C8, 0x019B5040, 0x00A663A0,
0x019B5040} = two PPU OPD {func,toc} pairs (0x019B5040 = main TOC).
STILL doesn't complete: the task processes the arg (DMAs/iterates the PPU OPDs, job-chain-like) and
loops -- it needs more of the SPURS framework than just the right r3 (DMA of the pointed-to data,
job-chain walk, completion/signal). NEXT: trace what the body does with the OPD arg (DMA target? a
job-descriptor list it walks to a terminator?) and supply what's missing so it reaches STOP, then
signal completion -> func_00A31158. This is the SPURS job-chain/task execution core (docs/11 §3).

### 15g. SPU task host-stack overflow + func_00A31158 frontier crossed [2026-06-29]

Wiring the task-START ABI (§15f) handed the SPURS leaf task its real argument, so it
finally runs its real work instead of looping on a null arg. That exposed a new
failure: right after `cellSpursCreateTask` dispatched the task, the **whole process
terminated** -- exit code 0, no `[CRASH]`, no `[ABORT]`, no `atexit`, no unwind log.
A long hunt (atexit probe, garbage-`sc` backtrace, SEH guard around the SPU run,
SPU_TASK_OFF toggle) pinned it: a lifted SPU job's guest `brsl` calls are real nested
host C calls, and the task's call chain overflows the **default 1 MB host thread
stack**. A stack-overflow SE can't run the unhandled-exception filter (the stack is
exhausted), so Windows tears the process down with no log and an exit code that reads
0. **Fix: reserve a 256 MB stack** for the SPU task threads (committed on demand).
Process now stays alive 30s+ with the task running concurrently.

Controlled experiment, decisive: `SPU_TASK_OFF=1` suppresses the SPU thread spawn.
- task OFF -> PPU spins forever on `0xD002A910` at `cia=0x00A31500` (= func_00A31158's
  wait) -- exactly the documented prior frontier.
- task ON  -> PPU advances past it.
So the SPU task IS the producer the `func_00A31158` wait blocks on, and the
task-start ABI + stack fix move the boot **past the func_00A31158 SPU-handshake
frontier**.

OPEN (new frontier): the task runs 30s+ without completing (no RETURNED) and the PPU
parks waiting for completion. Unknown whether that's genuine long work or a
lifter-induced deep/runaway recursion (256 MB defers, doesn't disprove, an infinite
chain). NEXT: pin where the task's call chain sits (the existing hang_watchdog
deadlocks vs the busy SPU thread via SuspendThread+CRT-lock; need a lock-free RIP
sampler or a depth counter in the lifted prologue), then verify it reaches its real
exit and writes 0xD002A910 to release func_00A31158. Diagnostics landed: SEH guard +
ENTER/RETURNED logs (spu_workload), garbage-`sc` backtrace (ppu_loader), atexit probe
(boot_main), SPU_TASK_OFF toggle.

### 15h. Root cause of task non-completion: unbounded host recursion from tail-call loops [2026-06-29]

§15g left the SPU task running 30s+ without completing. Settled it with a stack-size
sweep (env SPU_STACK_MB knob): time-to-overflow scales LINEARLY with the host thread
stack -- 4MB->2.3s, 16MB->5.7s, 64MB->20.3s. That is **unbounded recursion**, not
bounded long work; 256 MB only deferred the crash. (Process exit was the silent
stack-overflow path from §15g: no SE filter can run, exit code reads 0.)

ROOT: a lifted guest LOOP whose back-edge branches to ANOTHER lifted function's entry
was emitted as a plain tail call `func_X(ctx); return;`. The lifter intended a tail
call (no stack growth), but clang-cl did NOT reliably sibling-call-eliminate it (even
at /O2), so every iteration nested one more host C frame. The spu_0001 static call
graph shows the tell: micro-cycles between functions 4-12 bytes apart (e.g.
0x3030<->0x3034) -- one guest loop split across two functions whose back-edge became
a nested call. (The recursion is DIRECT func->func, not via spu_indirect_branch -- a
depth guard there never fired.)

FIX (lifter): emit cross-function tail branches through
  SPU_TAILCALL(call) = `__attribute__((musttail)) return call`   (clang)
which GUARANTEES the tail call at any opt level (a jump, O(1) host stack); call+return
fallback elsewhere. All lifted SPU funcs are `void f(spu_context*)`, so it's always
valid. Three sites: fall-through chain, conditional branch, unconditional branch.
Re-lift (build_spu.py); fingerprints unchanged. SPU TUs therefore do NOT need /O2 for
correctness (musttail is honored at /Od) -- reverted the /O2-on-SPU experiment.
Added spu_channels recursion guard (>6000 indirect nests -> pin + halt) as a net.

RESULT: spu_0001 survives a 4 MB stack 30s+ (was ~1s); the task runs bounded and the
PPU stays cleanly parked on func_00A31158's 0xD002A910 wait. Process stable.

NEW FRONTIER: the task halts at unresolved indirect branches -- `[SPU] indirect
branch to unknown LS address 0x04DC0 (image 2)` then `0x00000` -- before completing
the work that writes 0xD002A910. 0x04DC0 = a real LS target not registered as a
function (a jump-table/fn-ptr target the discovery missed); 0x00000 = a null/garbage
callback pointer. NEXT: seed/resolve 0x04DC0 (extend SPU jump-table / fn-ptr
discovery) and trace why a branch to LS 0 occurs (uninitialized callback in the task
arg's OPD list?), so the task runs to its exit + writes 0xD002A910 -> releases
func_00A31158 -> cond=3.

### 15i. SPU task is the SPURS command dispatcher; inline jump table seeded [2026-06-29]

With musttail (§15h) the task ran but halted at `[SPU] indirect branch to unknown LS
0x04DC0 / 0x00000`. Pinned the task (run inline via SPU_INLINE=1; hang_watchdog
sampled the PPU thread's RIP; symbolized vs des_boot.map) to **func_00004694** --
decoded as the SPURS task's COMMAND-QUEUE DISPATCHER:
  - queue empty (LS[0x13480]==LS[0x13400]) -> func_00005120 (RdEventStat/WrEventAck
    event-wait) -> loop;
  - else: getllar + DMA the next command, advance head, then
    `r2 = LS[r84 + (opcode-1)*4]; ctx->pc = r2; bi r2` -- a jump-table dispatch.
The table base r84 = LS 0x475C is a clean **37-entry handler table** (opcodes 1..37
-> 0x4850..0x5094) embedded IN .text between func_00004694 and func_00004804 (read
straight out of the ELF). 0x04DC0 = opcode 16's handler, never lifted because it's
reachable only via this computed dispatch; the fn-ptr discovery scanned only NON-text
segments. 0x00000 was a downstream effect (missing handler -> garbage reg -> bi 0).

FIX (spu_lifter): also detect jump tables embedded in .text -- a RUN of >=6
consecutive 4-aligned words all pointing into .text is reliably a jump table -- and
seed each entry. Re-lift: all 37 handlers lift; zero unresolved branches.

RESULT: the task dispatches its command queue cleanly and cycles in func_00004694;
process stable; PPU still parked on func_00A31158's 0xD002A910 wait.

FRONTIER (#7): the task is a persistent command-queue WORKER. To complete it needs
the SPURS runtime environment we don't yet provide: the queue ring (head/tail @LS
0x13400/0x13480) and command stream properly set up by the taskset kernel, and the
completion/exit protocol (the handler/command that writes 0xD002A910 the PPU polls).
NEXT: trace what populates LS 0x13400/0x13480 + what the dispatched handlers expect
(are they processing real commands or garbage from an un-set-up queue?), then provide
the missing SPURS scaffolding -> task reaches its exit + 0xD002A910 store -> releases
func_00A31158 -> cond=3. (docs/11 HLE SPURS task framework.)

### 15j. Scout past the SPURS gate; downstream map; completion contract decoded [2026-06-29]

Decoded the func_00A31158 PPU wait precisely (ppu_recomp_005 loc_00A314FC):
  loop { sys_timer_usleep(30); r0 = [r26+0x80]; r9 = [r26+0]; if (r0 != r9) repeat; }
i.e. it spins until [r26+0] == [r26+0x80]. Scouted by forcing them equal after 40
spins: observed **r26=0x01B51000, [r26+0]=0 (done/result slot), [r26+0x80]=0x45A4B280**.
0x45A4B280 is NOT a count -- it's a POINTER in the SPURS-managed region (taskset EA
was 0x45B4B300). So the PPU submits work and waits until the SPU task **writes the
result pointer 0x45A4B280 into [r26+0]=0x01B51000**. The SPU can't: its command queue
(loaded from the zeroed auto-context) is empty, so it processes nothing and never
writes the result.

Forcing the wait satisfied released func_00A31158 and the boot ADVANCED (stdout 34->49):
  -> [Dantelion2 Panic] DLStdAllocator.inl(268) "Transaction failed" (core dump, but
     execution continued)
  -> input init: cellKb Init, cellPad Init + SetPortSetting x6, cellMouse Init
  -> then a CRASH: write fault 0x01B3B33C, guest cia=0x00B7C000, last HLE =
     cellPadGetInfo2 (0xA703A51D).
The panic + crash are SCOUT ARTIFACTS: we faked SPU completion, so the PPU consumed
garbage where the SPU should have produced results. DECISIVE LESSON: SPURS completion
cannot be faked -- downstream (allocator, pad) genuinely needs the real SPU output.
But the scout proved there's a substantial boot road past SPURS (input + more), so
this gate is worth building through.

BUILD PLAN (Track 2, docs/11), now anchored to the real contract:
  1. The SPU task is a SPURS taskset command-queue worker. Its queue/state is DMA'd
     from its context; the PPU submits work via the taskset (EA 0x45B4B300) and
     expects the result pointer at [0x01B51000].
  2. Wire PPU-submitted work -> the SPU task's queue (shared EA) so func_00004694
     dequeues real commands and dispatches the 37 handlers.
  3. Run the handlers to produce the real result; write the result pointer to the
     PPU-polled slot (the SPURS task EXIT / completion protocol).
  Reference RPCS3's cellSpurs taskset/queue LLE + the SDK headers for the struct
  layouts (clean-room: contracts only, reimplement).

### 15k. func_00A31158 fully decoded = SPURS task EXIT-CODE completion; PM emulation scoped [2026-06-29]

Traced func_00A31158's call chain (NIDs reverse-hashed via nid_name.py); it is the
standard SPURS taskset+task create/wait:
  _cellSpursTasksetAttributeInitialize -> cellSpursTasksetAttributeSetName
  -> cellSpursTasksetAttributeSetTasksetSize [STUB] -> cellSpursCreateTasksetWithAttribute
  -> cellSpursTaskExitCodeInitialize [STUB] -> _cellSpursTaskAttributeInitialize
  -> cellSpursTaskAttributeSetExitCodeContainer [STUB] -> cellSpursCreateTaskWithAttribute
  -> WAIT (poll exit-code container until [r26+0]==[r26+0x80], r26=0x01B51000,
     target=0x45A4B280).
So the PPU wait IS the task EXIT-CODE poll, and 3 of its setup NIDs are stubbed.

RPCS3 cellSpursSpu.cpp gives the contract (the SPU-side taskset POLICY MODULE = kernel
at LS 0x2700, which we do NOT run -- we dispatch the leaf task ELF directly):
  - spursTasksetDispatch -> select task -> spursTasksetStartTask (sets task-start ABI,
    loads the task context per its ls_pattern, branches to the task).
  - On the task's `stop` = CELL_SPURS_TASK_SYSCALL_EXIT(0): spursTasksetProcessSyscall ->
    spursTasksetOnTaskExit(addr, taskId, exitCode, args): memcpy an EXIT-HANDLER routine
    from main memory (addr from taskset->x78 or taskContext x2FC0) into LS 0x10000 and
    fast_call it with r3=taskset, r4=taskId, r5=exitCode, r6=args. That handler writes
    the completion the PPU polls.
  - The leaf task signals the kernel via `stop <code>`; our lifter currently DISCARDS the
    stop code (emits bare SPU_STATUS_STOPPED_BY_STOP) -- must preserve it.

CONSEQUENCE: our leaf task (a) loops on an empty queue because the PM never delivers
its work/events, and (b) even if it exited, nothing emulates onTaskExit to write the
container. The build = emulate the taskset PM for the leaf task.

ORDERED BUILD PLAN (TDD; RPCS3 = contract reference only, reimplement clean):
  1. Lifter: preserve the SPU `stop` 14-bit code (so the runtime can dispatch task
     syscalls). Unit test: a tiny task that `stop`s with a code -> code visible.
  2. Runtime SPURS-task syscall layer: on a task `stop` with the SPURS syscall code,
     dispatch EXIT/YIELD/WAIT_SIGNAL/POLL. EXIT -> run onTaskExit (load+run the exit
     handler, or write the exit code to the container EA from
     cellSpursTaskAttributeSetExitCodeContainer). Unit test: task `stop EXIT` ->
     exit code lands in the container.
  3. Implement the 3 stubbed PPU NIDs (TasksetSize, TaskExitCodeInitialize,
     TaskAttributeSetExitCodeContainer) so the container EA is wired into the task.
  4. The hard part: get the leaf task to RUN TO its EXIT (it currently waits on
     PM-delivered events/work). Determine its work source (task arg = two PPU OPDs at
     0x00A662C8/0x00A663A0) and either deliver it or short-circuit to a clean exit.
  5. Integrate -> task EXIT -> container written -> func_00A31158 releases -> boot
     advances past SPURS (scout showed: input init + more).

### 15l. The leaf task's arg = two PPU callbacks; DLSpursManager is a job coordinator [2026-06-29]

Step 1 done: the lifter now preserves the SPU stop code (ctx->stop_code). Re-lifting
spu_0001 shows the entry func_00003070 ends with `stop 0` = CELL_SPURS_TASK_SYSCALL_EXIT
(the task's normal exit), reached only after its body func_000053C8 returns -- which it
doesn't, because it loops in the func_00004694 command loop (empty queue -> func_00005120
event-wait, never returns).

Dumped guest memory at the task-arg pointers (the 16-byte CellSpursTaskArgument):
  arg = { 0x00A662C8, 0x019B5040(TOC), 0x00A663A0, 0x019B5040(TOC) }
  @0x00A662C8: 38 63 00 08 7C 08 02 A6 F8 21 FF 71 ... (a PPU function prologue)
  @0x00A663A0: 38 63 00 08 7C 08 02 A6 F8 21 FF 81 ... (another PPU function prologue)
So the arg is TWO PPU FUNCTION POINTERS (+ TOC). func_00A662C8 / func_00A663A0 call
cellSpurs internals (func_016BB264 / func_016BB4C4, libsre) + game logic (func_00A9DD58).
=> spu_0001 (DLSpursManager) is NOT a self-contained SPU compute task; it is an async-JOB
COORDINATOR that orchestrates running PPU callbacks (which drive cellSpurs queues). The SPU
can't run PPU code, so the real flow signals the PPU (mailbox/event) to run the callbacks;
the empty "command queue" is the coordinator waiting for that round-trip.

TWO PATHS to release func_00A31158 (the task exit-code wait):
  A. FULL: emulate the SPURS taskset PM (LS 0x2700 ctx, task select/dispatch/start, event
     + signal delivery, the `stop`-syscall layer incl. EXIT -> onTaskExit writes the exit
     code) so the SPU coordinator runs its real round-trip to `stop 0`. Correct, large,
     reusable for all SPURS titles; RPCS3 cellSpursSpu.cpp is the contract reference.
  B. HLE SHORTCUT: recognize this task at cellSpursCreateTask, pull the two PPU callbacks
     from the arg, run them on the PPU via ppu_guest_call (TOC=0x019B5040), then write the
     completion (exit code) to the container func_00A31158 polls. Bypasses the SPU
     coordinator entirely -- smaller, could unblock the boot now, but DeS-specific and
     skips the real SPU work (may mis-sequence vs the game's expectations).
RECOMMEND: try B as a probe to unblock + reveal the next gate (the scout already showed
input init + more downstream), then build A properly. Either way step-1 stop-code +
the EXIT=stop0 + onTaskExit contract are the foundation.

### 15m. Reconciliation: the func_00A31158 blocker is a RESULT-SLOT handoff, not the exit code [2026-06-29]

Foundation A (fork 9404ead) implemented the 3 stubbed task exit-code setup NIDs
(cellSpursTaskAttributeSetExitCodeContainer/TaskExitCodeInitialize/TasksetAttribute-
SetTasksetSize); verified bound+firing, the exit-code container EA is captured
(0x45B4DC80). BUT reconciling the addresses IN ONE RUN shows the boot blocker is a
DIFFERENT thing:
  exit-code container        = 0x45B4DC80   (Foundation A)
  func_00A31158 wait: r26=0x01B51000 (= *(r30-0x7FEC), a global), [r26+0]=0,
                      [r26+0x80]=0x45A4B280
The wait spins until [r26+0] == [r26+0x80]=0x45A4B280, i.e. until a producer writes the
result pointer 0x45A4B280 into the global slot at 0x01B51000. That is a RESULT/PROMISE
handoff, NOT the task exit-code container. (The scout confirmed: forcing [r26+0] alone
released the wait.)

CONCLUSION/COURSE-CORRECTION: Foundation A is correct + part of the SPURS task lifecycle
(cellSpursTaskExitCodeTryGet/Join read 0x45B4DC80), but it does NOT release func_00A31158.
The blocker needs the WORK to be PRODUCED -- the DLSpursManager task (a coordinator whose
arg is two PPU callbacks func_00A662C8/func_00A663A0) must run the callbacks so they
publish 0x45A4B280 to [0x01B51000]. So the direct unblock is PATH B (run the two PPU
callbacks via ppu_guest_call(TOC=0x019B5040) at cellSpursCreateTask), and the exit-code
mechanism (Foundation A/B) is the separate task-lifecycle path. NEXT: probe Path B --
run the callbacks, check whether [0x01B51000] gets 0x45A4B280 (released) and what runs.

### 15n. Callback ABI: cellSpurs queue workers needing a handle [2026-06-29]

func_00A662C8 (callback 1) does r3 += 8, then calls func_016BB264(r3) (a cellSpurs
internal) and branches on its result code (0x80010005/0x80010009 = EAGAIN/EBUSY). So
the callbacks are cellSpurs QUEUE WORKERS that poll a queue/handle at r3+8; they need a
valid queue handle in r3 from the coordinator (running them blind derefs garbage).
func_00A31158's setup builds a struct {result-slot 0x01B51000, r24(=func_00A941F8 alloc
~ the job/result 0x45A4B280), r29, r29}: alloc the job, record it as the expected result,
create the coordinator task, wait until the SPU/worker publishes the job ptr to
[0x01B51000]. REMAINING = the full SPURS coordination (coordinator processes its queue /
PPU callbacks run with proper handles -> publish result). Everything is mapped; this is
the well-scoped next major build (Path A full taskset-PM emulation, or deep Path B that
reconstructs the queue handle + runs the callbacks). Scout's force-the-result stays the
bring-up shortcut to probe further downstream.

### 15o. Leaf first-run ABI corrected: r4 = {taskset->args, spurs EA} from LS 0x2700 [2026-06-30]

Cross-referenced RPCS3 `spursTasksetStartTask` + `spursTasksetDispatch` (cellSpursSpu.cpp)
for the EXACT register ABI the taskset PM hands a fresh leaf on first run:
```
gpr[3] = the 16-byte CellSpursTaskArgument (verbatim, BE lanes)
gpr[4]._u64[1] = taskset->args   (bytes 0-7)     // d0
gpr[4]._u64[0] = taskset->spurs  (bytes 8-15)    // d1 = SPURS object EA
pc             = savedContextLr  (= ELF entry on first run, after memset+LoadElf)
```
Crucially RPCS3 reads `taskset` from **LS 0x2700** (the taskset HEADER is copied there by
the kernel), so `spurs`/`args` come from LS 0x2700+0x60/0x68. Our task-start ABI was
setting only `gpr[4]._u32[1] = tasksetEA` -- WRONG (bare tasksetEA where the SPURS EA
belongs); the leaf uses r4's SPURS base to locate what to DMA, so it DMA'd from a garbage
base.

FIX (two parts, RPCS3-faithful, unit-tested):
1. `spurs_pm_build_context` now copies the taskset HEADER [0x00,0x80) (6 bitsets,
   spurs@0x60, args@0x68, wid@0x74) from the main-memory taskset into LS 0x2700 before
   writing the STC fields (which sit at >=0x80, no overlap). [spurs_pm.c]
2. The task-START ABI builds `r4 = {args (d0), spurs EA (d1)}` from LS 0x2700+0x68/0x60
   (BE u64), not from the bare tasksetEA. Our gpr lanes are BE order
   (_u32[0]=bytes0-3 .. _u32[3]=bytes12-15). [spu_lifted_job.h]
Unit test extended: asserts spurs EA @LS 0x2760, args @LS 0x2768, enabled-bitset copied
verbatim. PASS.

OBSERVED (the fix demonstrably reaches the leaf): the leaf's queue-ring state CHANGED.
- Before: `GETLLAR line=0x48037A00`, LS[0x13480] = `7B E3 00 20 ... 48 03 7A 05` (PPC code).
- After:  the leaf does a SECOND getllar at `line=0x00000000` and **LS[0x13480] is now all
  zeros** (was PPC `bl` code); LS[0x13400] head still code-looking (`78 63 00 30`).
So correct r4 made the leaf clear/re-init the tail of its command ring (queue empty path),
confirming r4 now drives its setup.

STILL Frontier #7 (same gate, not a new one): `0xD0000000-0xDFFFFFFF` is the STACK area
(vm.h VM_STACK_BASE), so the `0xD002A910` the PPU polls at cia=0x00A31500 is a STACK local
in func_00A31158's frame -- the completion flag the leaf must write on exit. Shadow stack:
`0x00B5D8D8 (this=0x1E) <- 0x00A31158 <- 0x009F7518 <- 0x00D1EAD0 <- ...`. The leaf still
reads code at LS 0x13400 because the command queue in MAIN MEMORY (that it DMAs via the
now-correct SPURS base) is itself not populated by our HLE cellSpurs -- the queue ring
head/tail and command stream are still un-set-up.

NEXT: trace how func_00004694 computes the command-queue EA from r4/spurs (which SPURS
field holds the queue base), and what our HLE cellSpurs must initialize so the leaf DMAs a
valid (empty-but-well-formed) queue + can reach the completion command that writes the
stack flag. The B1/B2/B3 + r4 scaffolding stays and feeds this.

### 15p. SMOKING GUN: leaf DMAs the arg callbacks as job descriptors (wrong code path) [2026-06-30]

Added a queue-DMA tracer (SPU_QLOG, gated on image 2 + LS window [0x13000,0x13600);
spu_dma.h mfc_do_transfer). With the corrected r4 (15o) the leaf's very first DMAs are:
```
GET lsa=0x13400 ea=0x00A662C8 size=256   # arg[0] -- a PPU CODE pointer (callback 1)
GET lsa=0x13340 ea=0x019B5040 size=64    # arg[1] -- the TOC (0x019B5040)
PUT lsa=0x13400 ea=0xFBC10080 size=16    # "ptr" read at desc+0xC = the code word FB C1 00 80
GET lsa=0x13580 ea=0x78630000 size=128   # "queue head" at desc+0 = code word 0x78630020 -> 0x78630000
... loops: GET 0x78630000/0x78630080/0x78630100 (+0x80), PUT 0xFBC10080 forever
```
So the leaf treats `arg[0]`/`arg[1]` as POINTERS TO JOB-CONTROL DESCRIPTORS, DMAs them,
and walks a "queue" whose head/next pointers it reads out of the DMA'd block. But the
block at 0x00A662C8 is PPC CODE (prologue `78 63 00 20 | 7C 08 02 A6 mflr | F8 21 FF 71
stdu | FB C1 00 80 std r30`), so every "pointer" it extracts (0x78630000 head, 0xFBC10080
put-target) is a misread opcode word -> it loops on garbage EAs and never completes.
(Confirms the LS 0x13400 PPC-code mystery of the prior turns: it's a DMA of arg[0]'s code.)

This nails the HLE/LLE mismatch precisely and aligns with 15l/15n: arg[0]/arg[1] are the
DLSpursManager COORDINATOR CALLBACKS (+ TOC), NOT descriptor pointers. So the leaf is on
the WRONG CODE PATH -- it entered a "process job descriptors pointed to by the argument"
branch when (given a callback-style arg) it should be on a "register the callbacks / set
up the command queue / wait for PPU-fed work" branch. What selects the branch is upstream
of the DMA: either (a) the SPURS/taskset state the leaf inspects (r4->spurs fields, the
command-queue ring base the PM kernel must seed), or (b) an argument-shape/word-order the
leaf keys on. Note ALL of r3's words are code/TOC (arg[0]=0x00A662C8, arg[1]=TOC,
arg[2]=0x00A663A0, arg[3]=TOC) -- no r3 slot holds a data pointer -- so a mere lane-swap
wouldn't fix it; the descriptor the leaf wants lives elsewhere (a queue the SPURS object/
taskset points to), un-set-up by our HLE.

NEXT: find the branch in func_000053C8/func_00004694 that decides "DMA arg as descriptor"
vs "wait on the SPURS command queue", and what SPURS/taskset field it tests -- i.e. what
the PM kernel normally seeds (the command-queue ring base + an initial empty/again state)
so the leaf parks on the queue instead of mis-parsing the argument. Diagnostic kept
(SPU_QLOG). Scaffolding (B1/B2/B3 + r4) stays.

### 15q. GATE CROSSED: task-argument ABI was wrong; real arg is r8 pointer, not r10 [2026-06-30]

15p showed the leaf DMAs its argument words as descriptors and arg[0] was PPU code. Traced
the PPU caller of _cellSpursTaskAttributeInitialize (NID 0xB8474EFF, stub func_016B9F64)
to func_00A31158 @0x00A314A8 and compared BOTH call sites. The real ABI is SIX args, not
eight:
```
r3=attr  r4=revision(1)  r5=sdkVersion(0x250001)  r6=eaElf  r7=eaContext(0)  r8=argument
```
r8 is a POINTER to the 16-byte CellSpursTaskArgument -- NOT sizeContext. r9/r10 are
clobbered/stale at the call (the preceding func_00A30980 trashes them). Our 8-arg HLE read
the argument from r10 (stale) -> garbage {func_00A662C8, TOC, func_00A663A0, TOC} (two real
PPU callbacks that happened to be left in r10's pointee). Decisive confirmations:
 - r8 = &{*(r30-0x7FEC), r24, r29, r29} on a CALLER STACK buffer (0xD002A980, right next to
   the 0xD002A910 wait flag); it varies per call site (a pointer, not a const size).
 - arg[1] = r24 = func_00A941F8(0x40,..) = a 64-BYTE alloc, and the leaf DMAs EXACTLY 64
   bytes from arg[1]. arg[0] = the 256-byte source. Sizes match a real descriptor pair.

FIX (cellSpurs.c / cellSpurs.h): _cellSpursTaskAttributeInitialize now treats its 6th GPR
(r8) as the argument pointer and COPIES the 16 bytes into the attribute immediately (the
src is a stack buffer that won't survive to CreateTask); CellSpursTaskAttribute gained
argValue[4]+hasArgValue; cellSpursCreateTask uses the copied value. sizeContext/lsPattern
aren't in this ABI -> 0 (context auto-allocates).

RESULT -- THE SPURS GATE IS CROSSED. The real argument is arg={0x01B51000, 0x45B4DC10, 0,0}:
arg[0]=0x01B51000 is the RESULT SLOT the PPU's func_00A31158 polls (per 15j); arg[1] a
64-byte buffer. The leaf now DMAs REAL data, and its GETLLAR shows it handling the actual
completion protocol: `45 A4 B2 80 .. 01 B5 10 80` (0x45A4B280 = the expected result ptr ->
written to result slot 0x01B51000). func_00A31158 releases and boot advances FAR past the
multi-session SPURS blocker into late init: HG_Watchdog thread spawn, _cellGcmInitBody, and
the INPUT subsystem -- cellKb/cellPad/cellMouse Init, cellPad SetPortSetting ports 0-5.

NEW FRONTIER (#8): crash shortly after, in cellPadGetInfo2 follow-on. Guest jumps to
cia=0x00B7C000 (NOT a lifted function) with lr=0 -- a vtable/fn-ptr indirect call to an
unlifted/invalid target -- and write-faults 0x1B3B33C (r3=0x01B3B338). Also a benign
[ppu] unresolved indirect call -> 0x01969238 earlier. NEXT: determine whether 0x00B7C000
is a real function the lifter missed (extend discovery) or an uninitialized C++ object/
vtable (this= pointers pervade the shadow stack), and resolve so input init completes.

### 15r. Post-gate: late-init HLE guest-pointer translation crashes [2026-06-30]

With the SPURS gate crossed (15q) the boot races through late init and now hits a CLASS of
bug: HLE library functions receive raw 32-bit GUEST effective addresses in pointer args but
deref them as HOST pointers -> segfault. (The generic HLE adapter passes GPRs verbatim;
modules must translate via vm_base, as cellSpurs/cellFs do. Modules written without it
were never exercised with a real pointer until now.)
 - cellPadGetInfo2 (NID 0xA703A51D): memset(info=r3=0x01B3B338,..) on the guest EA -> host
   write-fault 0x1B3B33C. FIXED: cellPad.c gained GUEST_PTR + translation in GetInfo2,
   GetData, GetCapabilityInfo, SetActDirect. Crash gone; boot advanced (threads 7/9 return,
   new cond=15 wait).
 - cellRtcSetTick (NID 0x99B13034): same pattern, read-fault 0xFEFFA70 (r3=0x0FEFFA78).
   Fixing cellRtc.c + cellKb.c + cellMouse.c (all input/util modules on the live path) with
   the same GUEST_PTR translation.
PATTERN/heuristic: an HLE crash whose fault address ~= a register holding a guest EA (and
"last HLE NID" names a pointer-taking function) = missing guest->host translation in that
module. Fix = add GUEST_PTR(p,T)=(vm_base+ea) and reassign each deref'd pointer param at
entry. This is mechanical HLE-completeness work, not RE -- expected to recur a few times as
late boot touches more modules. NEXT: rebuild, re-run, fix the next module the boot hits.

### 15s. Second task path: direct cellSpursCreateTask ABI; image=7 frontier [2026-06-30]

Past the input/audio/GCM init (15q/15r) the boot reaches a SECOND SPURS-completion spin:
PPU at cia=0x009F9604 polls stack flag 0xD002AD50, fed by a second task (image=7,
elf=0x017BE200, taskset 0x477ED800). It was dispatched with arg={0,0,0,0} and branched to
LS 0 (r3=0). Cause: this task is created via the DIRECT cellSpursCreateTask (NID
0xBEB600AC), whose real ABI is (taskset, taskId, elf, context, size, lsPattern, argument=
r9) -- our HLE had a WithAttribute-style signature (treated r8 as an attribute), so the
direct path never read the r9 argument pointer.

FIX (cellSpurs.c): split into spurs_create_task_core + two entries -- the direct
cellSpursCreateTask reads the 16-byte CellSpursTaskArgument from r9; CreateTaskWithAttribute
uses the attr's copied argValue (15q). image=7 now gets a real arg
{0x477FE220, 0x477FD800, 0x477FE600, 0x01BBD480} (data pointers in the 0x477Fxxxx region).

FRONTIER #9 (image=7 specifics): with the real arg, image=7 still doesn't complete -- it
takes indirect branches to unlifted LS 0x0004D (r3=0x80410911, an 0x8041xxxx cellSpurs
ERROR code; r80=0xFFFFFDA4) and LS 0x00000. So image=7 (a different SPU program than the
image=2 leaf) is hitting an error/assert path: either (a) its SPURS taskset/environment is
still incomplete for this task type, or (b) the lifter missed an in-.text jump table for
image 7 (cf 15i for image 2 -- re-run build_spu jump-table discovery on image 7). The
0xD002AD50 spin persists until image=7 completes its handoff. NEXT: trace image=7's path to
the 0x80410911 error (what it checks) + check image-7 jump-table coverage.

NET THIS SESSION: crossed the multi-session SPURS gate (15q) via the task-argument ABI fix,
then advanced through input (cellPad/Kb/Mouse) + audio (cellAvconfExt) + GCM flip-mode init
and a first completed SPU task, now parked at a second task (image=7). The blocker class
shifted from SPURS reverse-engineering to (mostly mechanical) HLE/ABI completeness + a
second per-image SPU bring-up.

### 15t. image=7 characterized: multi-task + bad argument-data pointers [2026-06-30]

Widened SPU_QLOG to image 7 (g_qlog7_budget cap). With its real arg
{0x477FE220, 0x477FD800, 0x477FE600, 0x01BBD480} image=7 (spu_0006_at_017AE200,
entry func_00003080) does:
 - GET 0x477FE600 (48B)->LS 0x1B600, GET 0x477FE220 (80B)->LS 0x19000,
   GET 0x477FD800 (128B)->LS 0x19080  -- DMAs its three argument structures.
 - read-modify-write 0x477FE900 (GET+PUT 128B).
 - then computes BAD source pointers from that data and DMAs them:
   GET ea=0xA73400E8 (128B) and GET ea=0x00000000 (NULL, 128B).
 - branch to garbage: func_00016CB8 does `r71 = rotqby(LS[r82+92], r82+92); pc=r71`
   -> r71=0x4D with r3=0x80410911 (an 0x8041xxxx cellSpurs ERROR code). Also two
   branches to LS 0 (r3=0,r80=0) near LS 0x17C00. -> halts; never completes; the
   PPU's 0xD002AD50 spin (cia=0x009F9604) never releases.
 - it also PUTs many 512B blocks to 0x381Dxxxx (+0x200 stride) -- looks like real
   output OR an error-spew loop.

So image=7's argument-data structures (at 0x477Fxxxx) contain dangling pointers
(0xA73400E8, null) -> its dispatch table (LS[r82+92]) resolves to garbage. TWO
contributing issues identified:
 1. MULTI-TASK: the game issues 3 direct CreateTask calls (argPtr 0x477EAA40/50/60,
    16B apart = a CellSpursTaskArgument array) but our spurs_create_task_core ALWAYS
    uses slot=0 and re-inits the taskset each call -> only 1 task runs (1 ENTER, 0
    RETURNED); the others are clobbered. A real taskset holds N tasks (slots 0..N-1).
 2. DATA DEPENDENCY: image=7's arg structures reference pointers (0xA73400E8 etc.)
    that are not validly set up -- either the game fills them incrementally / via a
    job chain, or they depend on prior-task output our HLE didn't produce correctly.

FRONTIER #9 (next session): (a) make spurs_create_task_core allocate distinct slots
per task (don't clobber slot 0; thread taskId through), (b) trace where image=7's bad
pointers (0xA73400E8 / null) originate in its DMA'd arg data -- dump guest mem at
0x477FE220/0x477FD800/0x477FE600 to see if the data is game-written-garbage (a setup
dependency) or mis-DMA'd, and (c) check image-7 jump-table coverage (the r71 dispatch
table). image=7 is its own multi-step SPU bring-up, like the image=2 leaf was.

### 15t-refine. image=7 immediate args are VALID; the bad ptr is deeper [2026-06-30]

Added an [argdump] of image=7's three arg-pointed structures at dispatch. They are
WELL-FORMED, not garbage:
  arg[0]=0x477FE220: 477FF580 0 0 0  477FD410 0 0 0      (two valid 0x477Fxxxx ptrs)
  arg[1]=0x477FD800: 0 0 FFFFFFFF FFFFFFFF  0 0 00000022 0  (a mask + count)
  arg[2]=0x477FE600: 477E7500 0 0 0  477E7500 0 0 0      (a valid 0x477Exxxx ptr)
None contains 0xA73400E8. So the direct-ABI arg CAPTURE is correct -- image=7 reaches the
bad pointer by walking the SECOND level (DMAing 0x477FF580 / 0x477FD410 / 0x477E7500 and
following a fn-ptr/table slot inside). The failing dispatch `r71=LS[r82+92]` and the
branch-to-LS-0 (r3=0 at link 0x17C00) are both image=7 reading an unpopulated
function-pointer table in a deeper structure. So the remaining work is image=7's internal
multi-level data graph / fn-ptr-table setup -- a focused per-image bring-up. NEXT: dump the
second-level structs (0x477FF580/0x477FD410/0x477E7500) to find the null/garbage fn-ptr
slot, and per-task slots in spurs_create_task_core (the game makes 3 image-7 tasks).

### 15u. image=7 bad pointer is COMPUTED, not in data; 2nd-level structs mapped [2026-06-30]

Recursed the [argdump] one level into image=7's arg structures. The full tree:
  arg[0]=0x477FE220: -> 0x477FF580:{0x477F3A80, 0...}  -> 0x477FD410:{0x42040000, 0x42040000, 0...}
  arg[1]=0x477FD800: {0,0, FFFFFFFF FFFFFFFF, 0,0, 0x22, 0}   (mask + count=0x22)
  arg[2]=0x477FE600: -> 0x477E7500:{ALL ZEROS}
0xA73400E8 (the bad GET source) appears NOWHERE in the data -> image=7 COMPUTES it on the
SPU from this data. Notable: 0x477E7500's struct is all-zeros (uninitialized) -> reading a
fn-ptr/size from it yields 0 (matches the branch-to-LS-0, r3=0). And 0x42040000 / 0x477F3A80
are the live inputs. So image=7's halt is a SPU-side computation producing a bad DMA EA
(0xA73400E8) and a null fn-ptr -- either a lifter miscompile in image=7 (cf the rotl-by-0
bug) or it consuming the zeroed/under-initialized 0x477E7500 struct.

The DMA EAL is computed through many functions/registers (MFC_EAL written from gpr[21]/
gpr[55]/gpr[54] at various sites), so isolating the 0xA73400E8 computation needs runtime
register-provenance tracing at the image=7 MFC_EAL write site, then back-tracing that
register. NEXT (focused per-image dive): (1) log image=7 MFC_EAL writes with the producing
LS address, (2) back-trace the register chain to the source LS field, (3) decide lifter-bug
vs zeroed-0x477E7500-dependency. Plus the per-task-slot fix (game makes 3 image-7 tasks).
This is image=7's own multi-step bring-up, analogous to the image=2 leaf (which took several
passes). The argdump/qlog diagnostics are in place to drive it.

### 15v. ROOT: image=7 raises CELL_SPURS_TASK_ERROR_NULL_POINTER -- runs before its data is filled [2026-06-30]

GPR provenance dump at the bad GET (image 7, ea=0xA73400E8) shows r80=r82=0x80410911,
r3=0x80410110, r13=0x80410949 -- these are cellSpurs ERROR codes built by image=7's
`ilhu 0x8041` instrs. 0x80410911 = **CELL_SPURS_TASK_ERROR_NULL_POINTER** (RPCS3
cellSpurs.h). So image=7 isn't randomly miscomputing: it DETECTED A NULL POINTER in its
input and entered an error path; func_00016CB8 then uses the error code as a struct base
(LS[r82+92] with r82=0x80410911) -> garbage branch (r71=0x4D) -> halt.

WHY the null: image=7's arg[2]-referenced struct (0x477E7500, and the per-task variants
0x477E5400 / 0x477E6400) are ALL ZEROS -- image=7 reads a null pointer from there.

LIKELY ROOT (architectural): our HLE runs each SPURS task SYNCHRONOUSLY the instant
cellSpursCreateTask is called (spu_workload_dispatch_task on a host thread immediately).
But the game creates the 3 image=7 tasks and THEN fills their per-task data / signals the
SPURS kernel to schedule them. The real SPURS kernel runs a task only when scheduled
(cellSpursWakeUp / ready-count), by which point the data is populated. Our run-on-create
races AHEAD of the game's data setup -> image=7 reads still-zero structs -> NULL_POINTER.
(image=2 worked because its arg[0] result-slot was ready at create time; image=7 needs
its data filled first.)

FRONTIER #9 -> direction: move from run-on-create to DEFERRED/SCHEDULED task execution --
register the task (Option B taskset, already built) but DON'T run the SPU until the game
drives the taskset (e.g. cellSpursWakeUp, a workload ready-count bump, or the PPU's next
poll of the result/completion). This also naturally handles the 3-task multi-slot case.
Cross-ref RPCS3 spursTasksetProcessRequest/scheduling. NEXT: find what the game calls
AFTER creating the image=7 tasks (cellSpursWakeUp? a flag?) that should gate their run,
and defer dispatch to that point so the per-task structs are populated first.

### 15w. Part A (deferred scheduling) DONE + Q2 result: image=7 data is cross-produced [2026-06-30]

Implemented docs/13 Part A: cellSpursCreateTask now REGISTERS the task pending (no thread
spawn); spu_pending_flush() runs pending tasks at the head of the PPU wait syscalls
(sys_event_queue_receive / sys_cond_wait / sys_semaphore_wait / sys_timer_usleep). env
SPU_TASK_EAGER=1 forces the old run-on-create for A/B. [spu_workload.c, the 4 syscalls]

RESULTS:
 - REGRESSION-FREE: image=2 still defers, flushes at the first PPU wait, and crosses its
   gate (cellGcm SetFlipMode + cellPad/Kb Init still reached). At flush its data is good
   (r3[0]=0x01B51000 holds 0x45A4B280) -- as expected (ready at create).
 - Q2 ANSWER (decisive): with deferral, image=7's arg structs at FLUSH time are
   BYTE-IDENTICAL to create time -- arg[2]=0x477FE600 still -> 0x477E7500 (still zeros).
   image=7 STILL hits CELL_SPURS_TASK_ERROR_NULL_POINTER (same 0xA73400E8 / LS 0x4D /
   0x80410911). So deferral did NOT populate image=7's data.

CONCLUSION: image=7's blocker is NOT a race with the creating thread's synchronous setup --
it is a DATA-FLOW dependency: the struct at 0x477E7500 (image=7's arg[2] target) is filled
by SOME OTHER producer that hasn't run by the time the PPU waits on q=5. Candidates: (a)
another task in the taskset / a coordinator SPU job, (b) image=7 is a job-CHAIN processor
and the job descriptors at 0x477E7500 are meant to be filled elsewhere, (c) image=7 reading
a ptr from 0x477E7500 is a wrong branch driven by arg[1]'s mask(0xFFFF..)/count(0x22) and
the zeroed struct is a valid "empty" state our lifted code mishandles.

Part A stays (correct RPCS3-faithful architecture, regression-free, needed for multi-task).
NEXT: find what writes 0x477E7500 -- add a guest-write watchpoint on 0x477E7400..0x477E7600
to identify the producer (PPU code or another SPU task), which decides (a)/(b)/(c).

### 15x. Producer hunt: 0x477E7500 is NEVER written -> image=7 calls a null fn-ptr [2026-06-30]

Added an all-width guest-write watchpoint (YDKJ_WWATCH, now covers write8/16/32/64 in
ppu_loader.cpp). Watching 0x477E7500 with tasks OFF and ON: ZERO hits. So image=7's arg[2]
struct (0x477E7500) is NEVER written by any PPU store for the whole run -- it is NOT an
under-populated producer dependency (15v/15w hypothesis (b) is OUT). Also note image=7 does
not even DMA 0x477E7500 directly (qlog).

Re-reading the failure order: the GPR-provenance dump (15v) was already in the error
cascade (r80=r82=0x80410911). The FIRST anomaly is earlier: the branch to LS 0 at
link/r0=0x17C00 with r3=0, r80=0 -- image=7 calls a NULL FUNCTION POINTER. Combined with
the func_00016CB8 dispatch (`r71 = LS[r82+92]; pc=r71`), image=7 dispatches through
FUNCTION-POINTER TABLES in LS that are unpopulated -> null/garbage targets -> the
NULL_POINTER cascade.

REFRAMED ROOT (hypothesis (c) confirmed direction): image=7's LS fn-ptr/dispatch tables are
not set up. Either (1) the SPU lifter missed seeding an in-.text jump/fn-ptr table for image
7 (cf 15i for image 2 -- image-7 may have tables the >=6-consecutive-word heuristic didn't
catch), or (2) image=7 builds those tables at init from data/template we don't provide, or
(3) a lifter miscompile in image=7's table setup. NEXT: trace image=7's fn-ptr table source
-- find the call site at ~LS 0x17BFC (returns to 0x17C00) and func_00016CB8's r82/table
base; check whether the lifter seeded image-7's jump tables (build_spu output) and whether
the table LS region is ever written/DMA'd. This is image=7's own SPU bring-up (its dispatch
tables), distinct from the SPURS scheduling work (Part A, done).

Part A (deferred scheduling) remains correct + committed; it is orthogonal to this.

### 15y. image=7 RUNS CLEANLY: the missing piece was syscallAddr (PM task-syscall entry) [2026-06-30]

Traced image=7's first failure (null fn-ptr call at link 0x17C00) to func_00017BD8:
  r7 = LS[0x2FB0]; r6 = rotqbyi(r7,8)  -> r6.w0 = LS[0x2FB8] = tasksetMgmtAddr = 0x2700
  r2 = r6 + 0xC4 = 0x27C4;  r4 = LS[0x27C4];  pc = r4   (call)
LS 0x27C4 = the SpursTasksetContext **syscallAddr** field (RPCS3 cellSpurs.h:
SpursTasksetContext.syscallAddr @0x27C4, kernelMgmtAddr @0x27C0). Our build_context left it
0 -> null call -> the whole 0xA73400E8 / 0x80410911 (CELL_SPURS_TASK_ERROR_NULL_POINTER)
cascade. And func_00017BD8 sets r3=0 first, so the syscall is num 0 = EXIT: image=7 was just
trying to EXIT via the PM, but syscallAddr=0 sent it to null.

RPCS3 contract: kernel sets ctxt->syscallAddr = CELL_SPURS_TASKSET_PM_SYSCALL_ADDR (0xA70);
a task branches there; the PM syscall entry runs spursTasksetProcessSyscall(num=r3.w3,
args=r4.w3) -> EXIT/YIELD/WAIT_SIGNAL/POLL/RECV_WKL_FLAG, then resumes/ends the task.

FIX:
 - spurs_pm.c build_context now writes syscallAddr=0xA70 and kernelMgmtAddr=0x100 into the
   SpursTasksetContext (STC_SYSCALL_ADDR/STC_KERNEL_MGMT_ADDR).
 - spu_channels.c spu_indirect_branch INTERCEPTS a branch to LS 0xA70 and HLEs the task
   syscall (spu_spurs_taskset_syscall): EXIT -> clean `stop 0` + halt (post-run exit-code
   write fires); YIELD/WAIT/POLL/RECV_WKL_FLAG -> return success + resume. (We don't have
   the real PM resident in LS, so we HLE its syscall entry -- matches RPCS3 registering an
   HLE function at 0xA70.)

RESULT: image=7 now reaches `SPURS taskset syscall num=0 (EXIT)` and RETURNS rc=0 -- ZERO
errors (no null branch, no 0xA73400E8, no 0x80410911). The crash cascade is fully gone.
No regression: image=2 still crosses its gate (input/GCM init reached); image=2 doesn't use
the syscall path.

REMAINING (Part C, completion notification): image=7 exits cleanly but the PPU still spins
(event_queue_receive(q=5) + HOTREAD64 0xD002AD50 @ cia=0x009F9604) -- nothing notifies it of
completion. NEXT: on task EXIT, notify the PPU -- post to lv2 event queue 5 and/or invoke
the taskset on-exit handler (taskset->x78, RPCS3 spursTasksetOnTaskExit: PPU handler called
with taskset,taskId,exitCode,args -> posts the event / writes the completion flag). Need to
find where the q=5 post / 0xD002AD50 write comes from (x78 handler vs the task's own post).

### 15z. Completion protocol mapped: PPU polls a task-count at 0xD002AD50 [2026-06-30]

With image=7 running clean (15y), the remaining block is the PPU's completion wait. Decoded
func_009F9600 (the cia=0x009F9604 spin):
  loop: func_00B5D8D8(0x1E); r0 = *(r27); if (r26 > r0) repeat;   // r27 -> 0xD002AD50
func_00B5D8D8(0x1E) is just a yield -- lv2 syscall 0x8D with r3=0x1E (usleep ~30us). So the
PPU busy-waits until the COUNTER at 0xD002AD50 (a stack local) reaches r26 (the expected
completed-task count; r26 = *(r25+0xB10), likely 3 = the 3 image=7 tasks the game created).

So COMPLETION = each task's EXIT must INCREMENT *(0xD002AD50). In SPURS that is the taskset
ON-EXIT handler's job: RPCS3 spursTasksetOnTaskExit calls the PPU handler (taskset->x78)
with (taskset, taskId, exitCode, args); that game handler bumps the completion counter /
posts the event. Two pieces remain (docs/13 Parts B+C):
 - PART B (multi-slot): run ALL 3 image=7 tasks, not just slot 0 (spurs_create_task_core
   hardcodes slot 0). The counter must reach 3.
 - PART C (completion): on task EXIT (our spu_spurs_taskset_syscall num=0), invoke the
   taskset on-exit handler so the counter advances. Need taskset->x78 (the on-exit handler
   EA) -- find where DeS registers it (cellSpursCreateTaskset / a taskset attribute), wire
   EXIT -> ppu_guest_call(x78_opd, taskset, taskId, exitCode, args). If x78 is unset, find
   what else increments 0xD002AD50 (the task's own DMA, or an event the PM posts).

STATUS: image=7 executes correctly end-to-end (runs -> EXIT -> returns); only the PPU
completion handshake (count to r26) remains. NEXT: find the taskset on-exit handler (x78)
+ run all N tasks, so the counter reaches r26 and func_009F9600 releases.

### 15aa. q=5 completion-flow trace: counter never incremented; we never post the event [2026-06-30]

Traced the completion path end-to-end:
 - The PPU batch manager func_009F94A8(this=r25, counterPtr=r27) creates the N image=7
   tasks, inits a completion COUNTER at 0xD002AD50 = 0, then busy-waits (func_00B5D8D8(0x1E)
   = lv2 syscall 0x8D usleep-yield) until *(0xD002AD50) >= r26 (expected count, derived from
   the manager object r25).
 - WATCHPOINT (YDKJ_WWATCH=0xD002AD50, all widths): the counter is written ONLY = 0 (init);
   it is NEVER incremented. So the PPU spins forever.
 - RPCS3 EXIT path (spursTasksetProcessSyscall EXIT) only calls the on-exit handler when
   ctxt->x2FD4==4 or ctxt->x2FC0!=0; image=7 (no exit-code container, elf&5==0) hits NEITHER
   -> no per-task on-exit callback. So the counter is NOT incremented by an on-task-exit
   handler. It is incremented by a PPU handler that runs on a SPU->PPU COMPLETION EVENT
   delivered to lv2 event queue 5 (one of the queues the game attached: AttachLv2EventQueue
   q=3..12; a thread waits on event_queue_receive(q=5)).
 - Our HLE NEVER posts that completion event: image=7 runs + EXITs, but nothing signals q=5.
   (cellSpursEventFlagSet only ORs bits + broadcasts to cellSpursEventFlagWait waiters; it
   does NOT push to the lv2 queue -- and that wait is separate from the counter poll anyway.)

MACHINERY INVENTORY:
 - HAVE: sys_event_queue_push_by_id(queue_id, evt) [sys_event.c] -- can post to q=5.
 - MISSING: (a) the taskset/SPURS -> lv2-queue binding (cellSpursAttachLv2EventQueue +
   cellSpursEventFlagAttachLv2EventQueue are STUBS -- they don't record which queue id),
   (b) the EXIT -> post-completion-event step, (c) the event PAYLOAD the q=5 handler expects.

PART C (concrete): on task EXIT (spu_spurs_taskset_syscall num=0), post a completion event
to the taskset's lv2 queue (q=5) via sys_event_queue_push_by_id so the PPU q=5 handler wakes
and increments 0xD002AD50. Needs: record the attach binding (which queue id), and the event
payload (SPURS source key 0xFFFFFFFF535055xx + workload/task id -- confirm from the q=5
receiver or RPCS3's SPURS event format). PART B: run all N tasks so the counter reaches r26.

This is the SPU->PPU completion-event delivery the project flagged as planned -- now the
precise, last thing blocking the second SPURS gate. NEXT: nail the q=5 event payload + the
attach binding, then implement EXIT->post + multi-slot.

### 15bb. CORRECTION: the 2nd blocker is GCM label-255 sync (RSX), NOT SPURS completion [2026-06-30]

Resolved the 0xD002AD50 confusion: func_009F94A8's poll loop reads *(r27) where
r27 = cellGcmGetLabelAddress(idx) (NID 0xF80196C1 = func_016B8564), and the loop also
calls func_00B5D8D8(0x1E) = a usleep yield. func_00B5D8D8 ENDS with `r0 = vm_read64(r1+0x90)`
to restore its saved LR -- and r1+0x90 == 0xD002AD50 for this frame. The HOTREAD64 detector
flagged that benign per-iteration LR-restore as the "spin address." So 0xD002AD50 is NOT a
counter; the 15z/15aa "completion counter / post event to q=5" framing was a WRONG LEAD.

THE REAL WAIT: func_009F94A8 polls **GCM label index 255 (0x03000FF0)** until it reaches r26
(the expected value, read from a struct). Confirmed: GetLabelAddress(index=255)->0x03000FF0
is requested repeatedly in the loop, and an all-width write-watchpoint on 0x03000FF0 shows
it is NEVER WRITTEN. So the PPU (PPURuntimeService -> func_00D1DEE8 -> func_009F94A8) waits
for the RSX/GPU to write label 255, which our HLE never does -> spin.

This is RSX/GCM label synchronization. image=7 (now running clean, 15y) GENERATES RSX
commands (its 512-byte PUTs to 0x381Dxxxx = a command-buffer/pushbuffer segment); the real
RSX would execute that buffer, hit a write-label-255 command, and update the label; the PPU
polls it. Our HLE has no RSX FIFO command execution that writes labels (only
cellGcmSetFlipCommandWithWaitLabel writes a label, line 539 -- not this path), so label 255
stays 0.

WHAT'S CORRECT FROM PRIOR WORK (keep): image=7 syscallAddr fix (15y) -- image=7 must run to
generate the commands; Part A deferred scheduling (15w); all the ABI fixes. WHAT'S A DEAD
LEAD (drop): the q=5 SPURS-completion-event plan (Part C as written in docs/13) -- the
counter it targeted doesn't exist; the blocker is a GPU label.

NEW FRONTIER (#10): RSX command-buffer / label processing. The PPU waits on GCM label 255
that the SPU-built RSX command buffer is supposed to make the GPU write. NEXT: find how
label 255 is meant to be written -- (a) trace the RSX FIFO / the command image=7 emits (a
write-label/SET_REFERENCE in the pushbuffer at 0x381Dxxxx), or (b) a cellGcm label-write call
the game makes -- then have our HLE GCM/RSX write label 255 to the expected value when the
corresponding submit/flush happens. This is the RSX/GCM frontier (see memory
frontier-rsx-gcm), now the precise thing gating boot.

### 15cc. FRONTIER #10 RESOLVED: RSX writes GCM label 255 -> boot reaches engine init [2026-06-30]

The RSX command stream writes label 255 via NV4097 semaphore methods (found via RSX_MLOG):
  method 0x1D6C (SET_SEMAPHORE_OFFSET) = 0xFF0  (= label 255 * 0x10, base 0x03000000)
  method 0x1D70 (BACK_END_WRITE_SEMAPHORE_RELEASE) = value (fence 0x10000,0x20000,0x30000..)
Our RSX parser (rsx_process_command_buffer/rsx_process_method) decoded these methods but had
NO handler for them -> label 255 never written -> func_009F94A8 polled it forever.

FIX (rsx_commands.c): handle 0x1D6C (store state->sema_offset) and 0x1D70/0x1D88 (write the
value to GCM label area 0x03000000 + offset via vm_write32). The methods are in the
processed buffer (not behind an unfollowed JUMP), so just adding the handler suffices.

RESULT -- HUGE: func_009F94A8's label-255 wait clears and boot CASCADES into full game-engine
init: Havok physics (hkSpuUtil Helper, Havok PPU Thread), audio (MOMainThread/Streaming/
Mixer), Demon's Souls save/load (DsSaveLoadMan::AsyncGetHddFreeSizeThread, SLSession),
NexusRevolution Event/Main, HighGraphics, cellGcmSetFlipMode(2). No regression (image=2/7
still fine). This is far past the SPURS+RSX gates -- into real subsystem bring-up.

NEW FRONTIER (#11): parked at cellSysutilCheckCallback + cond_wait(cond=2) (160x). Likely
the cellSysutil callback pump / the game waiting on a system callback or a worker sync.
(Refinement noted: 0x1D88 TEXTURE_READ_SEMAPHORE_RELEASE currently shares sema_offset with
0x1D70 and writes a constant 0x1000 to label 255 -- harmless so far, but it may need its own
offset/label; revisit if a downstream label wait misbehaves.) NEXT: characterize the cond=2
/ cellSysutil wait -- is it a missing system callback, or genuine steady-state?

### 15dd. FRONTIER #11 characterized: main loop pumps cellSysutil + waits on cond 2 [2026-06-30]

After engine init (15cc) the boot parks. Sync-trace: tid=15696 does COND_WAIT id=0x2 ~134x
(spurious wakeups -- sys_cond_wait uses a real Win32 CV w/ INFINITE timeout, so the thread
correctly loops `while(!pred) cond_wait()`), and there are ZERO cond_signals in the whole
run + ZERO flips (no rendering). So the game reached its main system loop but is stuck
pre-first-frame waiting on cond 2.

The waiter (one-shot stack probe -> cia=0x00B5D658) is a coordinator loop (func @0x00B5D6xx)
that: calls func_00A662C8 (DLSpursManager callback) -> func_016BB264 = cellSysutilCheckCallback
(NID 0x1BC200F4), then func_009E0370 (cond_wait on obj+0x48 = cond 2), then func_00A664E0,
repeat. I.e. it PUMPS cellSysutilCheckCallback and waits on cond 2 for work/an event.

So cond 2 is the "work ready" / "system event" condition; nothing signals it. Candidates for
the missing producer: (a) a system callback cellSysutilCheckCallback should deliver (it's a
no-op in our HLE -- e.g. an init-complete / drawing / save-data callback that signals cond 2),
(b) another subsystem thread that's itself parked (many threads wait on q=1/q=5/q=12, sem,
cond 3/9/E/F -- a steady-state SPU/RSX-event dependency), or (c) the SPU/RSX results need to
propagate to wake the coordinator.

NEXT: determine what signals cond 2 -- find the cond_signal(2) producer in the recomp (and
why it never runs), and/or which cellSysutil callback the loop expects delivered. This is a
new multi-thread / cellSysutil steady-state frontier, reached AFTER full engine init (Havok/
audio/save) -- the deepest boot point yet. Diagnostic: one-shot [cond2-bt] in sys_cond_wait.

### 15ee. Frontier #11 root: cond 2 is a job-queue; producer needs SPU->PPU events [2026-06-30]

Read the full coordinator (func @0x00B5D5xx-0x00B5D6xx): it's a doubly-linked-list WORK QUEUE
consumer -- dequeues jobs (list at obj+0x7C/+0x48/+0x20, classic head<->&head empty check)
and waits on cond 2 (obj+0x48) when empty, pumping cellSysutilCheckCallback between. So cond 2
= "job enqueued"; the PRODUCER never enqueues a job / never signals cond 2.

Ruled OUT cellSysutil as the dependency: cellSysutilCheckCallback only dispatches when a
SysutilEvent is enqueued (post_sysutil_event), and nothing posts one at boot -- but games
don't need a sysutil callback to START (those are runtime XMB/exit/menu events). The loop
pumps it as a courtesy; the real wait is for jobs.

So the missing producer is another thread. The system has many threads parked on SPURS event
queues (q=1 service, q=5 image-7 taskset, q=12), semaphores, and cond 3/9/E/F -- with ZERO
cond_signals and ZERO flips all run. In a SPURS engine the job producers ARE the service/
event-helper threads, which are driven by SPU->PPU EVENTS that our HLE never posts (we run
the SPU tasks but don't deliver their workload/completion events to the attached lv2 queues).
That gap (long on the roadmap: "make SPU->PPU completion real -- post lv2_event to the queue
on task/workload signal") is the well-grounded root of Frontier #11.

ASSESSMENT/RECOMMENDATION: the boot now reaches the game's MAIN LOOP after full engine init
(Havok/audio/save) -- the deepest point ever, via 5+ gates crossed this session (task-arg ABI,
deferred scheduling, image-7 syscallAddr, GCM label-255, RSX semaphore-writes). The next major
build is SPU->PPU EVENT DELIVERY: when an SPU task/workload signals (via the SPURS event flag /
a workload event), post the corresponding lv2 event to the queue the game attached (q=1/q=5/
etc.), so the parked service threads wake, produce jobs, and signal cond 2. This is substantial
(event format + the taskset/workload->queue binding + the SPURS event-helper contract) and
deserves a focused pass -- it's the clear next milestone, cross-referenced against RPCS3's
SPURS event posting + the cellSpursEventFlag/AttachLv2EventQueue path.

### 15ff. SPU->PPU event chain CONFIRMED end-to-end (probe); data1 = work descriptor [2026-06-30]

Probe (SPURS_EVTEST=1, one-shot in sys_cond_wait): posted a SPURS USER event
{source=0xFFFFFFFF53505501, data1=0, data2=0} to q=1. RESULT -- the chain is REAL:
 - The parked SPURuntimeService (q=1) WOKE: event_queue_receive(q=1) returned, the service
   advanced (then cond_wait(cond=3)).
 - Handler A (func_00C5BE78) was DISPATCHED and ran an allocator transaction, which FAILED
   with data1=0: "[Dantelion2 Panic] DLStdAllocator.inl(268) Transaction failed" -> DL_PANIC.

So source key + queue (q=1) + dispatch are all CORRECT -- the SPU->PPU event mechanism is
proven. The remaining requirement: data1 must be a VALID work-descriptor pointer (handler A
allocates from / dereferences it; data1=0 -> allocator panic). i.e. the USER event's data1
is the SPU-produced work item the handler enqueues.

REMAINING for the build (docs/14): (1) determine data1 = the SPU work descriptor (what the
SPU produces + where in main memory) and the selector ((data2>>32)&0xFF), (2) the trigger
(when an SPU task/workload produces a work item -> post the USER event with that data1),
(3) record the eventflag->queue binding (q=1). The probe validates posting; the open piece
is the correct payload/trigger, tied to the SPU tasks' output. NEXT: read handler A's use of
data1 (what struct/size it allocates) + what the SPU tasks write as their result descriptor,
to supply the right data1. The chain (event->service->handler->job->cond 2) is now verified
up to the handler.

### 15gg. Synthetic-event limit: handler needs REAL backing work; build = hook SPU production [2026-06-30]

Swept the probe data1 (SPURS_EVDATA): both data1=0 and data1=0x45A4B280 (the published job
ptr) reach handler A and PANIC in DLStdAllocator.inl(268) "Transaction failed". With
0x45A4B280 the service even ADVANCED (event_queue_receive(q=1) -> cond_wait(cond=3)) before
the panic -- so data1 matters, but neither value lets the alloc succeed. Handler A's path:
func_00C5BE78 -> func_000113B0 (a PLT/import trampoline -> func_01674898, a Dantelion2 method)
-> ... -> DLStdAllocator transaction -> panic.

KEY REFRAME: a SYNTHETIC event can't fully succeed. Handler A processes a "completed work
item" and allocates/enqueues based on it -- but our probe has NO real work behind the
descriptor, so the transaction fails. The probe PROVED the mechanism (source key + q=1 +
dispatch + the service advancing), but the handler needs the ACTUAL work the SPU produced.

So the build is NOT "guess the right synthetic data1" -- it's: hook the point where an SPU
task/workload ACTUALLY produces a work item, and post the USER event with data1 = that real
descriptor. The descriptor + its backing data are what the SPU writes; handler A then
allocates a job entry for it and signals cond 2.

CONFIRMED SO FAR: SPU->PPU event mechanism (q=1, source 0xFFFFFFFF53505501, dispatch). NEXT
(methodical, not blind): identify where the SPU tasks (image-2/image-7) write their result/
work descriptor (image-2 published 0x45A4B280 -> result slot 0x01B51000; what STRUCT does
handler A expect at data1, and does the SPU fully populate it?), then wire the event post to
that real production point. Read func_01674898 (the allocating method) for the descriptor
format. The probe (SPURS_EVTEST/SPURS_EVDATA) stays as the chain-validation harness.

### 15hh. Handler B (selector 1) = the WORK path: event drives cellSpursCreateTaskWithAttribute [2026-06-30]

Tested both dispatch selectors (SPURS_EVSEL):
 - sel=0 -> handler A (func_00C5BE78): a string/buffer build (func_01674898 -> func_01681BC0
   -> DLStdAllocator); panics "Transaction failed" with synthetic data1. Likely diagnostic/
   default path.
 - sel=1 -> handler B (func_00C5D6D8): the WORK path -- it reaches
   **cellSpursCreateTaskWithAttribute** (NID 0x1573DC3F) i.e. it CREATES THE NEXT TASK(S),
   then crashes reading 0x40A09B5C (a garbage ptr derived from synthetic data1=0x45A4B280).

THE PIPELINE (now clear): an SPU task completes -> posts a SPURS USER event (q=1, source
0xFFFFFFFF53505501, sel=1) -> handler B creates the next SPURS task(s) for the work -> ...
-> the DLSpursManager coordinator's job queue advances -> cond 2 signaled -> main loop
proceeds. So cond 2 is fed by the SPURS task PIPELINE continuing, driven by SPU-completion
events. Both handlers crash/panic on a SYNTHETIC data1 -- they need the REAL work descriptor
the SPU produced (handler B reads a struct/ptr field at data1; 0x45A4B280 -> 0x40A09B5C garbage).

BUILD (scoped, methodical -- the mechanism is proven, this is the real piece): 
 1. data1 contract: read func_00C5D6D8 (handler B) -- what struct it expects at data1 (it
    derefs a field -> 0x40A09B5C), and what cellSpursCreateTaskWithAttribute it builds.
 2. SPU production: find where the SPU task writes that work descriptor (image-2 published
    0x45A4B280; is THAT the descriptor, just not fully populated, or a different object?).
 3. Trigger+post: when the SPU produces a work item, post the USER event (sel=1, data1=real
    descriptor) to q=1 so handler B creates the next task.
This is the SPURS task-pipeline continuation -- a focused multi-layer build, now precisely
scoped. Probe (SPURS_EVTEST/EVDATA/EVSEL) is the validation harness (env-gated).

### 15ii. Handler B is a stateful routine; the fix is real SPU-side event production [2026-06-30]

Read func_00C5D6D8 (handler B, the work path). With r3=service_obj, r4=data1 it:
 1. acquires a lock via service_obj+0x18 (virtual call),
 2. SEARCHES a registered list (loc_00C5D770 loop) comparing each element to data1 (r31),
 3. builds a string from data1 (func_000113B0 -> func_01674898),
 4. (downstream) cellSpursCreateTaskWithAttribute -- creates the next task.
So data1 must be a WORK-ITEM OBJECT already REGISTERED in the service's list (the crash with
synthetic data1=0x45A4B280: not registered -> search miss -> fallback derefs garbage
0x40A09B5C). NB: the earlier crash cia was 0x00B5D658 (the cond-2 COORDINATOR) -- so handler
B did advance the pipeline (created a task / signaled, the coordinator woke) before crashing
on the garbage object.

CONCLUSION (honest): this is a STATEFUL subsystem. handler B keys on data1 matching the
service's accumulated registered-list state, which real event processing builds up
incrementally as SPU tasks produce work. A one-shot SYNTHETIC event fundamentally can't
replicate it -- data1 must be a real, registered work item, and the list must be populated
by the real flow. So the proper fix is SPU-SIDE EVENT PRODUCTION integrated into the normal
task flow: when an SPU task produces/registers a work item, IT posts the USER event (sel=1,
data1 = that registered object) -- so the service's list and the events stay consistent.

This is the full SPURS task-pipeline event substrate -- a substantial, methodical build, not
a synthetic injection. SCOPE for it: (1) the SPU-side mechanism that registers a work item +
signals (cellSpursEventFlag-from-SPU / the workload event), (2) map it to a USER-event post
with the real registered data1, (3) ensure the service's registered list is populated by the
same flow. The mechanism (q=1, source, sel=1->handler B->create task->cond 2) is PROVEN; this
is connecting it to real, stateful SPU output. Probe harness (SPURS_EVTEST/EVDATA/EVSEL) stays.
