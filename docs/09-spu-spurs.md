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
