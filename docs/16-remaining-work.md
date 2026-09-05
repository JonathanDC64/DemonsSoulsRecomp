# 16 — Remaining Work: Full Outline

> **⚠ SUPERSEDED IN PART (2026-09-05) — read `docs/21-current-state-and-remaining-work.md` first.**
> Phase 1 ("Faithful SPURS SPU-kernel execution") is no longer speculative engineering we must
> build: upstream ps3recomp v0.10.0 ships an SPU interpreter + policy-module runner, and its
> `docs/FIRMWARE_LLE.md` reports the full libsre LLE path **proven on Yakuza: Dead Souls**.
> This doc also assumes static SPU lifting precludes kernel-driven dispatch — the interpreter
> removes that constraint.


*Written 2026-07-04, after the render-gate investigation converged on the SPU
completion root. This is the honest, end-to-end map of what's left to reach the
title/auto-save dialog and, beyond it, playable gameplay.*

---

## 0. Where we are today

- **Boot reaches ~13/14 milestones**: all 18 threads created, engine reaches an
  idle steady state (MAIN parked, SLSession waiting, HighGraphics idle), initial
  assets *opened* (~11 shader/texture/sound files), **one** frame rendered (1
  FIFO batch, 1 D3D12 draw), then the frame loop stops.
- **100% CPU fixed** (commit `7df837e`): the SPU taskset POLL busy-spin now
  EXITs by default.
- **The single root gate is identified and proven**: **SPU execution &
  completion propagation.** Every blocked thread bottoms out here.

The rest of this document is organized by dependency: each phase unblocks the
next. Phase 1 is the crux — most of the others are comparatively mechanical once
it exists.

---

## 1. SPU execution & completion substrate  ← THE CRUX

**Problem.** We lift SPU images statically as native blobs. Only some run
(image=7 Havok runs; the SPURS kernel image=2 does not drive dispatch).
Completions are *shimmed inconsistently*: `cellSpursEventFlagWait` force-satisfies
after ~200ms (producing wrong/undecompressed data), while raw lv2 conds (e.g.
MAIN's cond-29) are never signaled at all (0 `sys_cond_signal` calls all run).
Forcing any one wait just relocates the crash to the next unproduced-data access
(proven: `FORCE_COND_DONE=29` → MAIN advances then 264 crashes).

**Two viable strategies (pick one, or hybrid):**

### 1A. Faithful SPURS SPU-kernel execution (LLE-leaning)
- Run the SPU SPURS kernel (image=2) so it dispatches leaf tasks and posts their
  completion events with the exact protocol the PPU waiters expect.
- Blocker recorded in memory: RPCS3 runs SPU *bytecode* dynamically; we lift SPU
  *statically* per-image, so the kernel can't dynamically branch into our lifted
  leaves. Needs a bridge (kernel dispatch → our lifted leaf by image addr).
- Highest fidelity, highest effort.

### 1B. Per-workload host reimplementation + correct completion signaling
- For each SPU workload the boot needs, either run our lifted leaf (works for
  Havok) **or** host-implement it, then **on completion drive the exact PPU
  waiter with real data**.
- This is the tractable, incremental path. Do it one workload at a time.

**Deliverable of Phase 1:** a *consistent completion layer* — every SPU workload
completion maps to the specific PPU wait it should satisfy, with real data:

| PPU waiter (this build) | Wait primitive | Producer (SPU workload) |
|---|---|---|
| Core.Res.Decompressor | `cellSpursEventFlagWait` (evflag) | SPU decompression task |
| MAIN | raw lv2 mutex `0x461` + cond `29` (resource ring-buffer obj `0x01ADD640`) | resource-queue producer |
| Havok PPU Thread | cond `0x13` | Havok SPU leaves (image=7, already run) |
| hkSpuUtil Helper | evq `0xC` | hkSpuUtil SPU work |
| SPURuntimeService | SPURS USER event (q=1) | SPU kernel event port |

*Recommended first workload:* ~~Core.Res decompression~~ — **CORRECTED, see below.**

### Build finding (2026-07-04) — the true Phase 1 entry point is the SPU-event substrate, NOT decompression
Attempting to start with decompression revealed it is **unreachable**: the whole
Core.Res pipeline is *systemically dormant*. Live thread states:

- **11 threads parked with `lastHLE=none`** (never started work): SPURuntimeService,
  Core.Res.{TaskManager,FileLoader,CacheDeviceHandler}, SLSession, MOMixerThread,
  NexusRevolution Event, HG_Watchdog, MOMainThread, spu_printf_handler.
- **`.dcx` files are opened but never *read*** (0 reads); the 8 failed opens are
  first-run **cache misses** (`/09/<hash>`… = dev_hdd1 cache lookups; expected,
  RPCS3 falls back to bdvd too).
- **SPURuntimeService receives SPU USER events (woken 47×) but dispatches nothing**
  (`lastHLE=none`) — the events arrive (12 `post_user_event`) but the handler that
  would wake the pipeline never advances. This is the **docs/14–15** problem:
  synthetic events panic into recovery; needs the **real lv2-SPU-thread substrate**.

**Corrected Phase 1 entry point:** the **SPURS SPU→PPU event substrate**
(SPURuntimeService dispatch), per the docs/15 design — get real SPU-thread events
delivered so SPURuntimeService dispatches its handler → the handler enqueues/wakes
the dormant workers → the pipeline (FileLoader read → Decompressor) actually runs →
MAIN's cond-29 resource completes. Decompression is **downstream** of this, not the
starting point. So Phase 1's first brick is the event substrate (docs/15), and
decompression/rendering follow once the workers are no longer dormant.

---

## 2. Resource pipeline (real decompression)

Depends on Phase 1 for the decompression workload.

- **Decompress `.dcx` assets for real.** DeS `.dcx` = DCX-wrapped deflate/EDGE-zlib.
  Today the SPU decompress is force-satisfied → garbage output. Need real
  decompression producing correct bytes.
  - Option: host-side decompress (add zlib/inflate — **not currently linked**),
    intercepting the request's src/dst/size. Requires RE'ing the decompress
    request ABI (it's behind the generic DL-thread functor + SPURS task layer;
    the Decompressor thread entry `0x0196FAF8` is a generic wrapper, real work in
    its `arg` functor).
  - Option: run the lifted SPU decompression leaf if it exists as an image.
- **Signal Core.Res's event flag** with the real result → Decompressor proceeds
  with valid data (not the current shim).
- **Feed the resource ring-buffer + signal MAIN's cond-29** so MAIN's resource
  wait clears with a real resource (obj `0x01ADD640`, producers
  `func_00AE0BE8`/`func_00ADF198`/`func_00AE47A0`).

**Deliverable:** resources actually load; MAIN's cond-29 clears legitimately (no
force, no crash).

---

## 3. Engine frame loop advances

Once MAIN's cond-29 clears with real data:

- MAIN's frame loop (`func_00B498E8`) iterates: dispatch frame work
  (`func_00A662C8`) → wait completion (`func_009E0370`) → continue → next frame.
- MAIN re-primes HighGraphics's render queue each frame (the render service is
  already understood and works — it's self-fed and renders what it's told).
- Verify frame count climbs (RPCS3 does 69 flips during boot; we do 1).

**Deliverable:** the engine ticks multiple frames instead of one.

---

## 4. Rendering (make frames visible + correct)

Much of the plumbing exists; needs completion once frames tick.

- **libgcm display-event loop.** Scaffolding built this session
  (`gcm_display_event_post`, fee1dead-port detection). Still need a host
  `_gcm_intr` equivalent that delivers vblank/flip and invokes the game's
  internal display functor (the per-frame trigger). See the work-queue map in
  memory (`demons-souls-spurs-poll-breaker`).
- **RSX command processing.** FIFO drain (`cellGcm_rsx_process_fifo`) + the D3D12
  backend work for one frame. Need: full method coverage, real vertex/texture
  data (once assets decompress), shader translation for `ds_flver`/`ds_menu`/
  `ds_sfx` bundles, render targets, blending, etc.
- **Present + vblank pacing** to a window (window already opens).

**Deliverable:** the boot logos / loading screen actually render.

---

## 5. The auto-save dialog (the M-milestone)

Downstream of MAIN advancing:

- MAIN advancing → MAIN posts SLSession's semaphore (the one never posted today,
  poster = MAIN after NP teardown) → SLSession's worker loop proceeds →
  `func_00EC3548` → **`cellSaveDataAutoLoad2` (NID 0xFBD5C856)** = the dialog.
- `cellSaveDataAutoLoad2` is already implemented (`cellSaveData.c:887`);
  `cellGameDataCheckCreate2` already wired. The dialog is a system overlay — once
  SLSession calls it, it should fire.

**Deliverable:** milestone 14 — the auto-save dialog (the stated goal).

### De-risk result (2026-07-04) — the target is real but has no shortcut
A scoped experiment confirmed the dialog cannot be cheaply forced, and clarified
exactly what reaching it requires:

- **The code path is real** (static + RPCS3-confirmed): SLSession worker
  (`func_00EC1090`) → `func_00EBF868` (locked dequeue) → `func_00EC3548` →
  `cellSaveDataAutoLoad2`. RPCS3 shows this exact chain producing the dialog.
- **No shortcut — it's gated behind MAIN advancing (Phase 1).** SLSession is
  parked at *startup* on sema-3 (`lastHLE=none`, 0 HLE calls) waiting to be
  released, AND its worker needs a load request *enqueued by DsSaveLoadMan*.
  Forcing sema-3 (`POKE_SEMA=3`, `FORCE_SEMA=3`) did **not** advance it — both
  the release (MAIN, after NP teardown) and the enqueue (DsSaveLoadMan, after
  engine progress) depend on MAIN advancing. So Phase 1 is **necessary and on the
  critical path** to the dialog; there is no cheaper route.
- **A *visible* dialog needs rendering too.** `cellSaveDataAutoLoad2`
  (`cellSaveData.c:887`) today fires `funcStat` + returns `CELL_OK` — no overlay
  is drawn. So distinguish two sub-milestones: **14a** = the
  `cellSaveDataAutoLoad2` *call* is reached (pure logic; Phases 1–2), and **14b**
  = a *visible* auto-save overlay (needs system-overlay rendering; Phase 4).

**Implication:** target **14a first** — it's the honest, logic-only proof that
the engine reached the dialog, and it validates Phases 1–2 without needing the
render pipeline. 14b (pixels) follows with Phase 4.

**Residual risk (untestable until Phase 1):** that once MAIN advances,
DsSaveLoadMan actually enqueues the auto-load request + posts SLSession's sema.
RPCS3's boot log shows this sequence, and our PPU HLE mirrors it, so the
expectation is well-founded — but it's the one link not yet observed live here.

---

## 6. Beyond the dialog → playable

Not required for the dialog, but the real finish line:

- **Input**: cellPad face-buttons + keyboard wired (done partially); DualSense/
  XInput mapping.
- **Save/load**: real `cellSaveData` read/write of `BLUS30443DEMONSS005`.
- **Audio**: MultiStream/MO mixer (`MOMainThread`/`MOMixerThread`) actually
  producing sound (currently threads exist, no output); `cellsurMixer`.
- **Full SPU workload coverage**: rendering geometry (EDGE), animation, physics,
  particle, post-processing — every SPU task the game uses per frame (Phase 1 at
  scale).
- **Networking / NP**: `sceNp*` — currently stubbed to success; the online
  features + trophy sync (`DsTrophyMan`).
- **Late threads** RPCS3 creates that we don't: `FaceGenMan::Thread`,
  `DsTrophyMan::AsyncUpdateThread`, `_fs_aio_thread`, `FXHGDrawEntityTracer`,
  `NexusRevolution Surveillance`, `_cellsurMixerMain`.
- **Stability**: eliminate the shims/force-satisfies once real completions exist;
  remove the diagnostic probes.

---

## 7. Supporting / infrastructure (ongoing)

- **Copyright firewall** (docs/05): never commit game-derived material.
- **Regression harness**: `tools/des/milestones.sh` (13/14 baseline) — extend as
  milestones advance.
- **RE tooling** (built this session, reusable): `ppu_guest_call_code`,
  `YDKJ_WWATCH` write-watch, cond/barrier/sema probes, `tools/des/sym.py`
  symbolizer, Ghidra headless (`tools/ghidra/dump.sh`).
- **RPCS3 oracle**: keep diffing against the known-good boot (Trace-level logs by
  channel) to pin protocols.

---

## Dependency summary (critical path to the dialog)

```
Phase 1 (SPU completion substrate)   ← the crux, ~all effort concentrates here
      │
      ├─► Phase 2 (real decompression → resources load)
      │        │
      │        └─► MAIN cond-29 clears
      │                 │
      │                 ├─► Phase 3 (frame loop iterates)
      │                 │        └─► Phase 4 (rendering) [visual, parallel]
      │                 │
      │                 └─► MAIN posts SLSession sema
      │                          └─► Phase 5 (cellSaveDataAutoLoad2 = DIALOG) ★ GOAL
      │
      └─► (at scale) Phase 6 (playable: audio, save, net, full SPU coverage)
```

**Honest scope.** Phase 1 is the fundamental unsolved problem the project has
circled since the SPU phase (docs/09–15). It is **major engineering** (real SPU
LLE execution, or a comprehensive consistent completion layer), not a shim and
not a few-line fix. Everything downstream (2–5) is comparatively mechanical and
mostly already scaffolded; **the dialog is close *after* Phase 1, and blocked
without it.** The right approach is to build Phase 1 **one workload at a time**,
starting with Core.Res decompression, verifying each PPU waiter clears with real
data — never by forcing/shimming, which only relocates the crash.
