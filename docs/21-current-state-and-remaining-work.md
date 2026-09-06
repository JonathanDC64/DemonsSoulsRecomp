# 21 — True current state & remaining work (post upstream v0.10.0 merge)

**2026-09-05.** Written after a full sweep of upstream `sp00nznet/ps3recomp` (all branches,
all tags, all 24 of its docs) following the v0.10.0 merge. **This supersedes docs/16 and
docs/19**, whose framing is now obsolete in important ways (see §6).

---

## 1. Where the boot actually is

Measured on the merged build (`ps3recomp` branch `upstream-merge`, commit `f057c52`):

| observable | pre-merge | post-merge |
|---|---|---|
| milestones | 13/14 | 13/14 |
| crash markers | 36 | **0** |
| threads created | 17 | **18** |
| determinism (`repro.sh 6 25`) | chronic non-determinism | **every observable stdev 0.0** |

The headline is **determinism**. Six consecutive boots produced identical numbers on every
observable. The non-determinism this project fought for months is gone.

`image=2` — the old pinned CRUX ("never completes") — **now runs and returns**. All SPU images
dispatch and return: image=7 ×16 rc=0, image=2 ×3, image=12 ×3.

New threads that never came up before: `(Dantelion2)PPURuntimeService`,
`(Dantelion2)SPURuntimeService`, `spu_printf_handler`, `NexusRevolution Event`.

### The gate

A two-sided SPURS deadlock, named by the runtime itself:

- **PPU** blocks in `cellSpursEventFlagWait` on `flagEA=0x01CE7800`
  (pattern `0x0001`, mode=AND, direction=1 = SPU→PPU), taskset `0x01CE8300`.
- **SPU task 1 of that same taskset** is parked in `WAIT_SIGNAL`, awaiting a signal
  that never arrives.

Each waits for the other. It bites ~2 s in; the rest of the run is just the two threads
waiting. Tried and ineffective: `SPURS_EF_FORCE=1`, `SPURS_EF_OBJ_DELIVER=1`,
`SPURS_EF_WAKE_PARKED=1`, `SPURS_EF_SPU_REPLY=1`.

Note this deadlock exists *because* upstream implemented **real** `WAIT_SIGNAL` semantics.
Our old code returned success immediately — a fake that let the task proceed on wrong state.
Blocking is the faithful behaviour; it is what made the missing link visible.

---

## 2. What upstream now gives us (this is where I was wrong before)

Upstream did **195 commits / 13,549 insertions across 81 files** in `libs/spurs` +
`runtime/spu` + `libs/sync` since our merge base. Subsystems our pre-merge tree never had:

| file | what it is |
|---|---|
| `runtime/spu/spu_interp.c` (+ `spu_interp_tables.inc`) | a real **SPU interpreter** — runs un-lifted SPU images (`spu_run_interp_job`) |
| `runtime/spu/spurs_policy.c` | runs a workload's **policy module** at LS 0xA00 under an HLE'd kernel handoff, per RPCS3's `SpursKernelContext` layout |
| `runtime/spu/spurs_job.c` | SPURS jobchain job runner |
| `runtime/spu/spu_lockstep.c` | lockstep interpreter-vs-lifted comparison |

Plus: real `WAIT_SIGNAL` semantics, a working `cellSpursSendSignal` (previously a no-op that
silently dropped signals — deadlocking any task parked waiting for one), the taskset-PM
`moduleId "TK"` @LS 0x1E4 and full-coverage `lsPattern` fixes (without which every task-API
blocking wait fails its context check with `0x80410909` / `0x8041090F`), 20 new cellSpurs
APIs, and a documented index of ~470 runtime diagnostics (`docs/DIAGNOSTICS.md`).

**This overturns the project's "two worlds" conclusion** (docs/09–15, and the
`spurs-two-worlds-rootcause` memory), which held that full LLE was impossible here *because*
we lift SPU statically, so the SPURS kernel could not dynamically dispatch into lifted leaves.
An interpreter removes exactly that blocker.

### Upstream's live SPURS architecture

One host poll thread per SPURS instance (a *virtual SPU*) polls the real big-endian
`wklReadyCount1[wid]` / `wklSignal1` fields the game writes — mostly via **inlined atomics**,
so there is no API call to hook — consumes one ready unit like the real kernel's dispatch, and
runs that workload's policy module to completion via `spu_run_policy_module`. Multiple worker
threads run concurrently; running them sequentially deadlocks at the cross-SPU barrier.

### Two different "policy" worlds — do not conflate them (I did)

- **Workload policy modules** (`AddWorkload` path): `spu_run_policy_module` is **live**, called
  from `cellSpurs.c`. The module bytes come from the game's own workload registration.
- **The libsre *taskset* policy** (`CreateTaskset`/`CreateTask` path — **this is what DeS
  uses**): served by a **C reimplementation**, `spurs_pm_build_context()` in
  `libs/spurs/spurs_pm.c`. The real lifted Sony taskset policy exists only as an **A/B
  oracle**, `spurs_run_taskset_policy_probe()`, and is **not wired up**.

---

## 3. The unwired extension point (corrected)

`spurs_policy.c` declares:

```c
extern const unsigned char g_taskset_policy_bytes[];   /* libsre vaddr 0x23680 */
extern const unsigned      g_taskset_policy_size;
extern void tsp_spu_func_00000A00(spu_context*);
```

defaulted to a **zero-size weak stub** (`runtime/spu/spurs_policy_blob_weak.c`,
`spu_tsp_weak.c`). At `spurs_policy.c:484` the probe **returns −1 when the size is 0**, so the
path is dead in our build.

**Important correction to what I said earlier:** wiring this does **not** by itself make the
real Sony scheduler run our tasks. `spurs_run_taskset_policy_probe` is a *diagnostic A/B
oracle* — it runs the real policy far enough to build the authentic `SpursTasksetContext` at
LS 0x2700, then **diffs it against our C reimplementation**. Nothing in upstream calls
`spu_run_taskset_policy` as a production scheduler.

It is still the highest-value next step, because behind the `LBP_REAL_POLICY` knob it prints
exactly what our deadlock needs:

- `[taskset-bits] RUNNING / READY / ENABLED / SIGNALLED / WAITING` — the task bitsets the
  policy reads (task N = bit 127−N), i.e. *why the policy exits without dispatching*;
- `[real-pm] LS 0x2700 diff: build=… real=…` — every field where our
  `spurs_pm_build_context` disagrees with authentic Sony code.

If task 1 parks in `WAIT_SIGNAL` and is never signalled because our taskset context or bitsets
are wrong, this diff names the wrong field directly. **We already have everything needed:**
`ps3recomp/fw_spu/libsre.prx` (236 KB, decrypted) and
`fw_spu/spu_libsre/taskset_policy_lifted/spu_recomp.c` (119 KB) defining
`tsp_spu_func_00000A00` + `tsp_spu_recomp_register()` (130 funcs under image id 100).

---

## 4. Remaining work, ordered

**A. Wire the taskset-policy A/B oracle** *(next; assets in hand)*
1. Extract the raw policy bytes from `fw_spu/libsre.prx` at vaddr `0x23680`.
2. Supply them as `g_taskset_policy_bytes` / `g_taskset_policy_size` from the port, and build
   `fw_spu/spu_libsre/taskset_policy_lifted/` into `des_boot` (`fw_spu/` is **not** currently
   in `port/CMakeLists.txt`).
3. Run with `LBP_REAL_POLICY=1`; read the taskset bitsets + the LS 0x2700 diff.
4. Fix whatever `spurs_pm_build_context` gets wrong. That is the candidate root of the
   `WAIT_SIGNAL` deadlock.

**B. Verify there is no asset-pipeline regression** *(open risk, see §5)*

**C. Full PPU-side firmware LLE of libsre** *(the big one; PROVEN upstream)*
`docs/FIRMWARE_LLE.md` documents the pipeline (`lift_prx.py` → `ppu_lifter --raw --base` →
bind the game's SPURS NIDs to the lifted exports) and reports it **validated end to end on
Yakuza: Dead Souls**: the real lifted scheduler loop runs stably and dispatches a real game's
tasks, including SPU-side context switches. This replaces HLE `cellSpurs` with authentic Sony
code and is the durable answer to the whole class of SPURS divergence bugs. Upstream is
explicit that generalising beyond that one title is a *hypothesis*, and that SPU-lifter
correctness is a prerequisite (running the real kernel stresses rare opcodes).

**D. Then the chain to the dialog** (unchanged in shape from docs/16)
```
SPURS completion (A/C)
  └─► real .dcx decompression → resources load
        └─► frame loop iterates
              ├─► rendering            [parallel, visual]
              └─► cellSaveDataAutoLoad2 = DIALOG   ★ goal
                    └─► playable: audio, save, net, full SPU coverage
```

**E. Housekeeping**
- Re-evaluate the deltas dropped by the merge (`docs/20`), especially deferred SPURS
  scheduling (`spu_pending_flush`) and the `SPURS_DONE_EVQ` bridge — both plausibly touch
  this deadlock.
- `patches/ps3recomp/01-*.patch` is stale (its README still claims "27 commits ahead");
  regenerate or retire it.
- Decide whether to merge `upstream-merge` → `master`, and whether to push to our fork.

---

## 5. Open risks and caveats

- **Unverified possible regression.** The merged build does **zero file opens/reads**, and the
  deadlock bites at ~2 s. Memory records the pre-merge Core.Res pipeline *streaming real
  assets*. Log volume also dropped 4347 → 1414 lines and thread exits ~30 → 3. Milestone
  parity at 13/14 may therefore be **masking less actual work done**. Not yet settled — the
  pre-merge log was overwritten; the check is to rebuild from `master` (needs an SPU re-lift
  with the old lifter) and diff file activity.
- **Backtrace-derived conclusions in docs/07–19 are suspect.** Upstream fixed a bug where the
  host-RVA → guest-function mapper *invented* stack frames (unbounded "largest pointer ≤
  target"). Anything concluded from a symbolised backtrace before this merge was read through
  that.
- **Firmware is committed to git.** Upstream **tracks `fw_spu/libsre.prx` and 28
  firmware-derived files**, while its own `docs/FIRMWARE_LLE.md` states that firmware and
  anything derived from it "must stay gitignored". Convenient for us, but it is a
  redistribution concern that arrives in our tree via the merge — worth a deliberate decision
  before pushing anything public.
- **`docs/SPU_FALLBACK.md` upstream is stale** — it still claims "ps3recomp does not execute
  SPU code (no SPU ISA JIT, no SPU local-store model)", which predates both the SPU lifter and
  the interpreter. Do not plan against it.
- The merge resolved ~500 hunks by taking upstream wholesale wherever it had rewritten a
  subsystem. Validated by "builds + boots deterministically", **not** by a line-by-line audit.

---

## 6. What is now obsolete in docs/16 and docs/19

- **docs/19's pinned CRUX is resolved.** "image=2 never completes → HighGraphics cond-2 stuck"
  is no longer true; image=2 runs ×3 and returns.
- **docs/19's Phase 2** (MAIN cond-29 message content) was derived pre-merge and needs
  rechecking against the new deadlock before any of it is acted on.
- **docs/16 §1A "Faithful SPURS SPU-kernel execution (LLE-leaning)"** was scoped as major
  speculative engineering. It is now largely *available upstream* and **proven on another
  title** — the effort shifts from "build it" to "wire and validate it".
- Both docs treat static SPU lifting as precluding kernel-driven dispatch. The SPU interpreter
  removes that constraint.

---

## 7. New capabilities worth using immediately

- **Debug console** (`PS3_DEBUG=dbg.txt`): ask a *running* title `threads`, `hle`, `stat`,
  `mem <hex> [len]`, `poke32`, `knobs` — no rebuild. Far better than watchdog sampling for a
  hang that appears seconds in. Upstream's tip: a spin and a deadlock look identical in a log
  and are told apart by CPU time.
- **`docs/DIAGNOSTICS.md`** indexes ~470 runtime diagnostics that were previously grep-only.
  Most are read once and cached, so they are launch-time settings.
- **`spu_lockstep.c`** — lockstep interpreter-vs-lifted comparison, for catching lifter
  miscompiles in SPU code.

---

## 8. UPDATE — the oracle is wired, and it produced results

`scripts/gen_taskset_policy_blob.py` + `port/CMakeLists.txt` now supply
`g_taskset_policy_bytes` (7744 bytes, libsre vaddr 0x23680 = LS 0xA00..0x2840, base
verified against the lifted first instruction `ila $2,0x20DB8`) and build the lifted
policy under image id 100. The probe's image gate was hardcoded to LBP's image 3;
it is now `REAL_POLICY_IMG` (DeS's event-flag producer is **image 12**).

Run it with: `LBP_REAL_POLICY=1 REAL_POLICY_IMG=12`

### What the authentic Sony policy told us

**Finding 1 — a real divergence, found and fixed.** Our `spurs_pm_build_context`
was clobbering the task's lsPattern with all-ones:

```
LS 0x27A0:  build=FFFFFFFF   real=03FFFFFF
```

The TaskInfo DMA already placed the game's own pattern there and the next four
stores overwrote it. DeS's `cellSpursCreateTask` logs
`lsp=03FFFFFF FFFFFFFF FFFFFFFF FFFFFFFF`, so the "it reaches us mangled anyway"
justification for the override does not hold here. Fixed (`SPURS_LSPATTERN_FORCE=1`
restores the old behaviour); 0x27A0 no longer appears in the diff.

**It did NOT fix the deadlock.** No `0x8041090F` / `0x80410909` errors were being
raised either way. It is a faithfulness fix, not the cause.

**Finding 2 — task 1 is the intended producer.** `CreateTask(id=1,
entry=0x01832680, ctx=0x01CE9E00)` dispatches as **image 12**, and its task
argument contains **`0x01CE7800`** — the exact event flag the PPU blocks on. So the
task that parks in `WAIT_SIGNAL` is the one meant to set the flag the PPU waits for.

**Finding 3 — even authentic Sony code does not dispatch.** The real policy runs and
exits to kernel immediately: `END status=0x2 pc=0x009C0 polls=0 exited=1`, with

```
RUNNING=0000000001000000  READY=4000000000000000  ENABLED=4000000000000000
SIGNALLED=0000000000000000  WAITING=0000000000000000
```

`SIGNALLED` and `WAITING` are **empty**. So the real scheduler, given this taskset
state, also finds nothing to dispatch. That points the investigation **upstream of
the policy**: the taskset's signalled/ready state is not being driven by whoever
should drive it — it is not that our policy reimplementation picks wrongly.

### Next leads (precise, from the diff)

Three context fields our reimplementation zeroes and authentic Sony code sets:

| LS | build | real |
|---|---|---|
| `0x2700` | `00000000` | `40000000` |
| `0x2770` | `00000000` | `00000001` |
| `0x27D0` | `00000000` | `00000008` |

(The block at `0x2800`–`0x2838` is the policy module's own table region, present in
the real run because the blob is loaded there and absent in ours — treat as expected,
not as a divergence to chase.)

Given Finding 3, the higher-value thread is **who sets `SIGNALLED` / the event-flag
bits**, not further context-field matching: trace what on real hardware calls
`cellSpursSendSignal` / `cellSpursEventFlagSet` for taskset `0x01CE8300`, and why
nothing in our run does.

---

## 9. ROOT CAUSE FOUND AND FIXED — the dropped signal

Following §8's lead ("who drives SIGNALLED?") to its end produced a genuine root-cause bug.

**The chain, verified end to end:**

1. `cellSpursCreateTask` wrote the assigned task id through a translated host pointer:
   `if (taskId_h) *taskId_h = s_tasks[i].id;` — a **host-endian store into guest memory**.
2. The guest, being big-endian, read its taskId back as `bswap32(1)` = **`0x01000000`**.
3. It handed exactly that to `_cellSpursSendSignal`. Confirmed, not inferred:
   `PS3_HLE_ARGS=E0A6DBE4` showed the guest itself passing `r4=0x01000000` — it was echoing
   back what we had written.
4. `spu_taskset_signal_task` rejects anything `>= 128`, so the signal was **silently dropped**.
5. The taskset's `SIGNALLED` bitset stayed empty — which is exactly what the authentic Sony
   policy reported in §8 (`SIGNALLED=0 WAITING=0`).
6. SPU task 1 parked in `WAIT_SIGNAL` forever, never set event flag `0x01CE7800`, and the PPU
   blocked on it in `cellSpursEventFlagWait`. Each side waiting for the other.

**Fixed** (`vm_write32` — the same bug class as upstream's `libs/` guest-pointer sweep: a
scalar the guest reads back needs `vm_write*`, not a bare store). The log now reads
`_SendSignal(taskset=0x01CE8300 id=1)`, and the signal is delivered **and consumed** — task 1
reaches `WAIT_SIGNAL#2`, where it previously never got past `#1`.

**The boot does not yet advance.** No regression either: still 13/14, 0 crashes, 18 threads,
and `repro.sh 3 20` is fully deterministic. The gate moved rather than opened.

### The next link (new, sharper)

Task 1 now wakes on the signal and **immediately re-enters `WAIT_SIGNAL` with `ran=0ms`** —
it does no work in between, and still never sets `0x01CE7800`. So the task is *alive and
starving for content*, not dead on a dropped signal. This is the same shape as docs/19's
"gates are LIVE loops starving for CONTENT" finding, now at a precisely located link.

Next question: **what does task 1 read to find its work, and why is it empty?** Its inputs are
known — task argument words `0x01CE7A80` (word 1) and `0x01CE7800` (word 3, the event flag),
context `0x01CE9E00`, entry `0x01832680`, dispatched as image 12. Trace what it DMAs after
waking (`SPU_QLOG`, `SPU_ARGWATCH`) and who on the PPU side should have filled that buffer
before signalling.

---

## 10. What task 1 actually is — and the exact remaining question

**Correction to §9's "starving for content".** It is not starving. Tracing its transfers
(`LBP_MFC_TRACE=2`, filtered to `img=12`) shows a **complete round of real work** between the
two waits — 36 MFC transfers:

```
#1   GET   16 B  <- 0x01CE7A80    (its work descriptor, task-arg word 1)
#2   GET  144 B  <- 0x01CE6D80
#3   GET  128 B  <- 0x01D27300
#22-25 GET 3136 B each             (source data, 4 streams)
#26-29 PUT 16384 B each
#30-34 GET/PUT 16384 B
#35  PUT 8192 B  -> 0x60000000     <-- CELL_AUDIO_GUEST_BASE
#36  PUT 8192 B  -> 0x60002000
```

`0x60000000` is `CELL_AUDIO_GUEST_BASE` — upstream's audio ring (moved off `0x01000000` in
this merge). **Task 1 is Demon's Souls's SPU audio mixer, and it successfully mixes and writes
PCM output.** DeS's audio engine is MO / "Magic Orchestra" (`magicorchestra2.mgs`,
`demonsmixer.momd`), with `MOMainThread` / `MOMixerThread` / `MOStreamingThread` on the PPU.

So the working sequence is now:

```
PPU MOMixer  --_cellSpursSendSignal(taskset,1)-->  SPU task 1 wakes
SPU task 1   --mixes a full round, PUTs PCM to the cellAudio ring-->  done
SPU task 1   --WAIT_SIGNAL again-->  parked
PPU MOMixer  --cellSpursEventFlagWait(0x01CE7800)-->  blocked forever
```

### The exact remaining question

**Nothing ever sets event flag `0x01CE7800`.** Established, not assumed:

- `SPU_ATOM_EA=01CE7800` — **zero** atomic operations on the flag, ever.
- Image 12 issues **only** taskset syscall `num=2` (WAIT_SIGNAL), twice
  (`args=0x0000000A`, `link/r0=0x0F2F8`) — no flag-set syscall.
- No MFC transfer targets `0x01CE7800`.
- The PPU waiter reports `bits=0x0000` every second — the flag's event word never changes.

So the completion edge the PPU waits on is simply never produced. On real SPURS the SPU-side
`cellSpursEventFlagSet` is a task-library call; our lifted image 12 does not appear to reach
it. Candidate explanations, in order of cheapness to test:

1. The set is issued through a channel we swallow (mailbox / `WrOutIntrMbox`) rather than an
   atomic or an MFC transfer. `SPU_CHHIST=1` is meant to answer this but produced no output in
   a `timeout`-killed run — find its flush point or add one.
2. Our `WAIT_SIGNAL` returns `rc=0` unconditionally; if the real syscall returns state the task
   branches on, it may be taking a path that skips the notify.
3. The task-library flag-set lives in a part of image 12 our lift halts before reaching.

`SPURS_EF_SPU_REPLY=1` exists upstream as a deliberate *diagnostic bisect* (it fakes the SPU
reply); it did not advance the boot earlier, but re-testing it now that the signal actually
gets delivered is a cheap A/B that would confirm hypothesis-class (1)/(3) vs something later.

---

## 11. DIAGNOSIS CONFIRMED — the missing piece is the taskset PM's completion notify

§10 listed three hypotheses. Testing eliminated the first two and confirmed a fourth.

**Ruled out by measurement:**

- *Channel/mailbox*: `SPU_CHHIST` reported nothing because its print interval was a fixed
  2000 channel ops and the mixer does a few dozen per round — fixed (`SPU_CHHIST=<n>`). With
  it working: the only `WrOutMbox` / `WrOutIntrMbox` writes in the whole run happen **after**
  task 1 parks, from image 7 — the mixer writes no mailbox at all.
- *Atomics*: `SPU_ATOM_EA` shows **zero** atomics anywhere in the `0x01CE…` region — including
  the taskset itself, which is consistent with our HLE'ing the taskset PM in C rather than
  running its SPU code.
- *Syscall*: image 12 issues **only** `num=2` (WAIT_SIGNAL), twice.
- And our own HLE completion path contains **no** event-flag notify — grep finds only a comment.

**The actual cause.** On real SPURS the taskset **policy module** performs the completion
bookkeeping when a task finishes the round it was signalled for, including setting the event
flag a PPU waiter is blocked on. We HLE that PM in C (`spurs_pm_build_context`) and never run
its notify logic, so the edge the PPU waits on is never produced by anyone.

**Confirmed by bisect.** `SPURS_ROUND_NOTIFY=<flagEA>[:<bits>]` (new, off by default) sets the
named flag when a task enters WAIT_SIGNAL. With `SPURS_ROUND_NOTIFY=01CE7800:1`:

| observable | before | after |
|---|---|---|
| `EventFlagWait BLOCKED` | 24 | **0** |
| `EventFlagWait WAKE` | 0 | **40** |
| `_cellSpursSendSignal` | 1 | **9148** |
| WAIT_SIGNAL rounds | 2 | **218** |
| log lines | 1404 | **10813** |

The deadlock disappears and the MO audio pipeline cycles: PPU signals → SPU mixes → notify →
PPU wakes → repeat. **This is the first time that loop has ever run.**

**Honest limits.** The boot still does not reach the dialog, creates no new threads, and does
no file I/O — the audio gate opened, the boot did not advance past it. And this is a *bisect*,
not a fix: the real PM decides *which* flag and *which* bits from taskset state we do not
model, whereas the knob applies what the env names. The ~9148 signals in 50 s also suggests
the loop is now free-running rather than paced.

### Turning the bisect into a fix

1. Derive the flag EA from **taskset state** instead of the env. The task's own argument
   already carries it — DeS's task 1 gets `arg = 00000000 01CE7A80 00000000 01CE7800`, i.e.
   word 3 *is* `0x01CE7800`. Reading it from the TaskInfo/argument is a far better
   approximation of what the PM does than a hardcoded value.
2. Work out the correct **bits** (and pacing) the same way §8's oracle was used: run the real
   policy and observe what it writes, rather than assuming `0x0001`.
3. The durable answer remains the PPU-side firmware LLE of `libsre` (§4C), which replaces this
   whole reimplementation with authentic Sony code.

---

## 12. FIXED — the taskset PM's round-completion notify (no knobs required)

The §11 bisect is now a real fix (`spurs_ef_notify_taskset_round`, ps3recomp `975fefd`) that
derives everything from guest state instead of an env-supplied address:

| what | source |
|---|---|
| which flags belong to this taskset | `EF_ADDR` (0x70) |
| which of those are SPU→PPU | `EF_DIRECTION` (0x0E) |
| which bits the waiter actually wants | the blocked waiter's own pattern |

The last one comes from the **host-side** `EventFlagSync` table, not the guest's
`EF_PPU_WAIT_MASK`: that guest field is deliberately left clear so the SPU-side lifted Set code
stays on its "no PPU waiter" path, so it always reads 0. The pattern is recorded on entry to
`cellSpursEventFlagWait` and cleared on wake, so a flag with no blocked waiter has mask 0 and is
left alone. The notify fires only when a PPU thread is genuinely blocked on a flag owned by the
taskset whose task just completed a round — the PM's own condition.

With **no env knobs set**:

```
[round-notify] taskset 0x01CE8300 round done -> flag 0x01CE7800 bits=0x0001 (waiter mask)
```

| observable | before | after |
|---|---|---|
| `EventFlagWait BLOCKED` | 24 | **0** |
| `EventFlagWait WAKE` | 0 | **40** |
| `_cellSpursSendSignal` | 1 | **6743** |
| log lines | 1404 | **8406** |

Clean A/B: `SPURS_NO_ROUND_NOTIFY=1` restores exactly the 24 blocked waits. `repro.sh 4 20`
remains fully deterministic, 0 crash markers, 18 threads.

### The state has changed qualitatively

There are now **no hard blocks left**. What the threads are doing:

```
21x  timer_usleep(100 us)     cia=0x009F75E4
14x  timer_usleep(30 us)      cia=0x00A31500
11x  mutex_lock(mutex=1092)
 8x  timer_usleep(16666 us)   cia=0x00B49574     <-- 60 Hz frame pacing
 6x  timer_usleep(16666 us)   cia=0x00D1E188     <-- 60 Hz frame pacing
```

Two independent 16666 µs (60 Hz) pacing loops plus ordinary mutex traffic — the engine is
*running and pacing frames*, not parked. Previously the entire process sat on one event flag.

**Still 13/14.** No auto-save dialog, no file I/O, no new threads, 2 draws. So the engine loops
without advancing to content loading. The gate is no longer a deadlock; it is the pre-title →
title transition — which is where docs/19 was before the merge, but now reached with a live
audio pipeline, frame pacing, and a deterministic boot.

### Next

1. **Verify the asset pipeline** (still open from §5): zero `sys_fs_open` throughout. Find what
   the engine is waiting for before it starts streaming — with the boot now deterministic and
   the audio loop live, this is a much better environment to ask that in than before.
2. The `PS3_DEBUG` console (`threads` on demand) is the right tool for a boot that now *runs*
   rather than hangs — pair it with `tools/des/sym.py` to symbolise, since its own
   symbolisation is sparse.
3. `mutex=1092`/`1093` and the `cia` addresses above are concrete anchors for what the frame
   loops are gated on.

---

## 13. Post-fix survey: who is actually waiting, and on what

With the deadlock gone, the boot RUNS. Added the guest tid to the `timer_usleep` wait log
(cond_wait already had it), which immediately answers "which thread":

| tid | thread | site | sleep |
|---|---|---|---|
| **4** | **HighGraphics** | `func_009F7518+0xCC` | 100 µs ×37 |
| **4** | **HighGraphics** | `func_00A31158+0x3A8` | 30 µs ×20 |
| 2 | (Dantelion2)PPURuntimeService | `0x00B49574` | 16666 µs (60 Hz) |
| 5 | HG_Watchdog | `0x00D1E188` | 16666 µs (60 Hz) |

So the two 60 Hz loops are normal frame pacing, and **HighGraphics is the only real spinner**.

### The GCM FIFO is NOT the blocker

`func_009F7518` is the "wait until the RSX drained the FIFO (get==put)" loop this project has
suspected for months. Read the control register live via the new `PS3_DEBUG` console
(`mem 20002000 16`; base is `ppu_hle_inject_base = 0x20000000`, control at +0x2000):

```
0x20002000  00 00 01 00 | 00 00 01 00 | 00 00 00 00
             put=0x100     get=0x100     ref=0
```

**put == get**, and byte-identical 8 seconds later. The FIFO is fully drained and static — the
game has submitted ~256 bytes of commands in the entire run and nothing since. HighGraphics is
not blocked on the RSX; it has no work to submit. (2 draws total.)

### Other facts established

- **Zero `sys_semaphore_post` in the entire run.** `sem=2` and `sem=3` are waited on and never
  posted — `sem 3` is created (`init=0 max=1`) immediately before the `SLSession` thread, and
  SLSession is what calls `cellSaveDataAutoLoad2`, i.e. milestone 14. This is the same
  semaphore gate recorded in the project's older notes, still unresolved.
- **DeS never calls the exit-code setup NIDs** (`cellSpursTaskAttributeSetExitCodeContainer` /
  `TaskExitCodeInitialize`) — 0 calls. So although `func_00A31158` was previously read as "the
  task exit-code poll", that container is never registered by this title; whatever it polls
  comes from somewhere else. Worth re-deriving rather than inheriting the old label.
- Still zero `sys_fs_open` / `cellFsOpen` of any kind.

### The shape of the remaining problem

Everything is alive, paced and idle. Nothing is deadlocked; the engine is waiting for its own
state machine to advance, which needs content, which needs file I/O that never starts. The
question is unchanged in kind but much better posed than before: **what does the engine require
before it issues its first asset request?** Two concrete threads to pull:

1. `func_009F7518` and `func_00A31158` — what HighGraphics actually polls (both now have exact
   addresses and a reproducible, deterministic boot to observe them in).
2. The `sem=2` / `sem=3` posters — identify who on the PPU side is supposed to post them, since
   that is the direct route to SLSession → `cellSaveDataAutoLoad2` → the dialog.

---

## 14. The semaphore is NOT the dialog gate (negative result, and a correction)

§13 named the `sem=2` / `sem=3` gate as "the most direct remaining route to the dialog".
Tested with a new `SEMA_RELEASE=<id>[:<secs>]` bisect. It is not.

**Attribution first** (`SEMTID=1`):

| tid | thread | waits on |
|---|---|---|
| 19 | **SLSession** | `sem=3` |
| 13 | MOMixerThread | `sem=2` |

with `[sem] create id=3 init=0 max=1` immediately before the SLSession thread, and **zero
`sys_semaphore_post` in the entire run**.

**Releasing `sem=3` does not produce the dialog.** SLSession wakes and *immediately re-waits on
`sem=3`* — 15 times over the run — issuing no HLE call in between. So `sem=3` is SLSession's
**work-queue signal**, not the dialog gate: the thread has no work queued, and posting the
semaphore only spins its loop.

This **corrects** the older reading carried in the project notes ("SLSession blocks on a
semaphore whose poster is MAIN"), which implied that posting it would let the dialog through.
It would not. Whoever posts that semaphore must also *enqueue a save/load request*, and the
absence of the request — not the absence of the post — is the gate.

### Method note worth keeping

The first version of this probe put its elapsed test at function entry. `sys_semaphore_wait_impl`
is entered once per wait and then blocks inside a loop, so the check could never fire — and a
probe that never fires reads exactly like "the semaphore is not the gate", which is the wrong
conclusion reached for the right-looking reason. The test has to live inside the wait loop.
Generally: when a bisect produces a negative result, confirm the probe fired before believing it.

### Where that leaves the search

Every direct route has now been eliminated:

- not the SPURS deadlock (fixed, §12)
- not the GCM FIFO (`put == get`, static, §13)
- not the SLSession semaphore (this section)
- not the SPU task exit-code container (never registered by this title, §13)

What remains is the thing all of them point at: **the engine never advances its own state
machine to the point of requesting content**, so nothing is ever queued for the loader, the
saver, or the renderer. The next move is to work forward from what HighGraphics polls
(`func_009F7518+0xCC`, `func_00A31158+0x3A8`) — the only thread doing anything other than
pacing — rather than backwards from the consumers, which are all correctly idle.

---

## 15. CORRECTION to §13, and the real hot loop

**§13 was wrong, because the instrument was truncating.** The `timer_usleep` wait log was hard
capped at 60 lines, so its picture of "who sleeps where" was really "who slept during early
init". Every HighGraphics sample it showed sat in lines 107–177 of a 7802-line log. A thread
that stops appearing then looks *idle* when it may be spinning hardest.

Capped removed (`PS3_WAIT_LOG=<n>`, 0 = unlimited). The true distribution:

| count | thread | site | sleep |
|---|---|---|---|
| **1,065,802** | **HighGraphics (tid 4)** | `func_00ACAFA8+0xF4` (`0x00ACB09C`) | 30 µs |
| 5,344 | MOStreamingThread | `0x00D64EA0` | 5 ms |
| 4,494 | NexusRevolution Main | `0x00B49574` | 8.3 ms (120 Hz) |
| 2,286 / 2,284 / 2,278 | PPURuntimeService / HG_Watchdog / MOMainThread | — | 16.6 ms (60 Hz) |
| 50 | HighGraphics | `func_009F7518+0xCC` | 100 µs |
| 16 | HighGraphics | `func_00A31158+0x3A8` | 30 µs |

The two sites §13 named are the **rare** ones. Everything except HighGraphics is normal pacing.

### The real gate

`func_00ACAFA8+0xF4` is a **completion-flag spin**:

```c
vm_write32(r26 + 0, 0);                    /* clear the flag        */
... store 0x9023, call func_00011720 ...   /* submit the work       */
loc_00ACB098:
    usleep(30);
    if (vm_read32(r26 + 0) == 0) goto loc_00ACB098;   /* wait for non-zero */
```

Live registers (`PS3_WAIT_OBJ=00ACB09C`, which now matches CIA since our lifted syscall path
leaves `lr` at 0):

```
r26=0x01B50FD0  (the polled flag -- always 0)   r27=0x01B51000   r29=0x01B50FC4
```

**`r27 = 0x01B51000` is exactly the graphics SPURS task's argument** —
`cellSpursCreateTask(id=0, entry=0x0177C200, arg=01B51000 …)`, dispatched as **image 2**. So
HighGraphics kicks the SPU graphics task and blocks on its completion word.

**Image 2 runs and returns `rc=0`** — 7 MFC transfers (GET 256 from `0x01B51000`, PUT 16 back
×3, plus reads around `0x45A4B2xx`). But it only ever writes `0x01B51000`, and **never
`0x01B50FD0`**, 0x30 bytes below. It completes without producing the completion edge its PPU
waiter needs.

This is the **same shape as the audio task before §12's fix** — a task that finishes without
signalling — and it is the true successor to docs/19's old "image=2 is the CRUX" finding:
image 2 no longer hangs, but it still does not complete its contract.

### Next

1. Establish what writes `0x01B50FD0` on real hardware: the SPU task itself (a path it never
   reaches because its command queue is empty), or the taskset PM as part of completion — the
   latter would make it the graphics-side analogue of §12 and might be fixable the same way.
2. Only 7 transfers and 3×16-byte writes suggests image 2 finds an empty/unset command queue —
   docs/19's original reading — so trace what fills `0x01B51000`'s queue on the PPU side.
3. `0x9023` is stored into a command slot just before the wait; identifying that method would
   name the operation being awaited.

### Method note

Two instruments lied this session: a probe whose check could never fire (§14) and a log whose
cap hid the hottest loop in the program (this section). Both produced confident, wrong pictures.
Before trusting a null or a distribution, check that the instrument can actually see the thing.

---

## 16. The graphics gate is PERSISTENT-TASK RE-ENTRY

§15 located the spin. This section names its cause.

**The structure** (read live via the debug console):

```
desc = 0x01B50FC4   +0x00 = 0x45A4B320   end pointer
                    +0x0C = 0x00000000   completion word  <- HighGraphics polls this

queue = 0x01B51000  +0x00 = 45 A4 B3 20   +0x0C = 01 B5 10 00   <- self-pointer
                    +0x10 = 45 A4 B2 80   +0x1C = 01 B5 10 10   <- self-pointer
```

`0x45A4B320 - 0x45A4B280 = 0xA0` — the command region HighGraphics fills.

**Image 2 does see the work.** Its 7 MFC transfers: GET 256 B from `0x01B51000` (the
descriptor), GET 128 B from `0x45A4B280` and 144 B from `0x45A4B290` (the commands), and 3×
PUT 16 B back to `0x01B51000`. It reads the commands and updates the descriptor — it simply
never writes the completion word.

**The A/B that names the cause.** `SPU_DONE_WORD=<descEA>[:<img>]` publishes "consumed up to
the end pointer" on task completion:

| | without | with |
|---|---|---|
| HighGraphics spins (35 s) | 928,568 | 925,146 |
| `done-word` fired | — | **1** |
| **image 2 runs** | — | **1** |

The word is written **once**; HighGraphics then clears it and waits again, and **image 2 never
runs a second time**. Publishing the word is therefore not the fix — the missing thing is the
second run.

**This tree already names the gap** (`spu_workload.c:620`, `YDKJ_CRI_RESUME`):

> *a real SPURS task is PERSISTENT — on yield the kernel re-enters it when work is signalled.
> Our HLE runs it once, so it polls the (concurrently PPU-updated) eaContext, finds no work,
> yields and exits before the PPU marks work-ready.*

That is exactly the graphics case. Our HLE dispatches image 2 once at `cellSpursCreateTask`; it
finds the queue empty (the self-pointers above are an empty list), exits, and nothing ever
re-enters it. Upstream approximates re-entry for image 22 (cri) only.

### So the two SPU gates are the same shape

| path | task | what was missing |
|---|---|---|
| audio | image 12 | completion **notify** on round end — **fixed** (§12) |
| graphics | image 2 | persistent **re-entry** when work is submitted — open |

### Next

1. Generalise persistent re-entry beyond image 22: re-dispatch a taskset task when the PPU
   submits work to the queue it polls. `YDKJ_CRI_RESUME` is the existing shape to follow.
2. Identify the kick: HighGraphics stores `0x9023` and calls `func_00011720` just before
   waiting — whatever that does is the "work submitted" edge a re-entry would hang off.
3. The durable answer remains the PPU-side libsre firmware LLE (§4C): the real SPURS kernel
   does this re-entry natively, which is precisely why upstream reports it as the fix for this
   whole class.

---

## 17. Re-entry is not enough — the gap is submission→queue linkage

§16 concluded the graphics gate was persistent-task re-entry. Implemented it
(`SPU_TASK_RESUME=<img>[:<n>[:<ms>]]`, generalising `YDKJ_CRI_RESUME` beyond image 22) and
tested with 800 re-entries of image 2. **It does not unblock HighGraphics** — still ~1.2 M
spins, completion word never written. The negative result localises the gap precisely.

**Why**, from `YDKJ_AWATCH=01B50FC4`:

```
write 0x01B50FC4 = 0x45A4B320
  func_00ACAFA8+0x68B <- func_009F94A8+0xC9A <- func_00D1DE40+0x5B4
                      <- func_00D1DEE8+0x108B <- func_00C3A878+0x6C1   (HighGraphics)
```

Two facts fall out:

1. **HighGraphics submits exactly once.** The descriptor is written twice in the whole run: `0`
   at init, then the end pointer. It then spins forever waiting for a completion that never
   comes — it never retries or re-submits.
2. **Image 2 never reads that descriptor.** The submission goes to `0x01B50FC4`; image 2 only
   ever GETs `0x01B51000..0x01B51100`. Its own queue entries remain self-pointers (an empty
   list) no matter how many times it is re-entered — which is why re-running it changes nothing.

So the missing step is **not** "run the task again". It is whatever turns a PPU-side submission
at `0x01B50FC4` into task-visible work in the queue at `0x01B51000`. On hardware that is the
taskset policy module / SPURS kernel — the same root as the audio round-notify (§12), one level
further in: there we had to *publish a completion*, here we would have to *publish a
submission*.

### Corrected picture of the two SPU gates

| path | task | missing kernel step |
|---|---|---|
| audio | image 12 | completion notify on round end — **fixed** (§12) |
| graphics | image 2 | submission → queue linkage (task never sees the work) — open |

### Next

1. Determine the queue format at `0x01B51000` (two 16-byte entries, each ending in a
   self-pointer) and what a linked entry looks like — then have the HLE link a submission in,
   the mirror of §12's notify.
2. `func_009F94A8` is in the submit chain and is the function the older notes associate with the
   label-255 wait; worth re-reading now that the chain is known.
3. This is the second gate in a row whose root is "our C HLE of the taskset PM does not do what
   the real one does". That is a strong argument for prioritising the PPU-side libsre firmware
   LLE (§4C) over continuing to reimplement PM behaviours one at a time.
