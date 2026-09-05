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
