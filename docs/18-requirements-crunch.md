# 18 — Requirements crunch → full implementation plan

*2026-07-05. A one-session, parallel requirements-gathering pass that front-loads
ALL the specs needed to implement the coherent SPURS runtime + reach the boot
dialog, so implementation proceeds without repeatedly stopping to investigate.
Method: fan out N focused subagents (each on a non-overlapping requirement
domain, producing a numbered R1/R2/... spec), then synthesize into docs/19.*

## Why this exists

The problem is converged to ONE root (SPU→PPU SPURS task-completion signaling;
docs/17) but implementing it correctly needs several exact contracts nailed
first. Investigating them serially, one chat turn at a time, is slow and loses
context. So: crunch all domains in parallel, produce specs, then plan.

## The crunch: requirement domains (each a subagent → a numbered spec)

- **A. SPURS task-completion contract** (RPCS3 source). How a
  `cellSpursCreateTask` task signals completion; the CellSpursTaskset/CellSpurs
  state a host executor must maintain on task start/finish (contention, running
  bitset, exit code); the minimum writes+signal that clears a "wait for my tasks"
  barrier. → R-A*.
- **B. HighGraphics cond-2 barrier** (our lifted code). The #1 gate. The barrier
  object + counter after `cellSpursCreateTaskWithAttribute`; what signals cond-2;
  the sema-4 RSX-fence path; the task→leaf mapping. → R-B*.
- **C. SLSession sema-3 → dialog** (our code + RPCS3 log). The sema-3 poster
  (MAIN, after NP teardown?); whether SLSession also needs a work item enqueued;
  the `cellSaveDataAutoLoad2` call chain; independent of the SPURS gate or not. → R-C*.
- **D. Nondeterminism + verification** (our SPU runtime). Race sources; a
  stable-repro mechanism (serialized SPU mode / repro harness) so "did this fix
  help?" is answerable. Infrastructure that gates confidence in everything else. → R-D*.

Already-crunched inputs to fold in: docs/16 (remaining-work roadmap), docs/17
(SPURS runtime model + min-viable A/B/C/D), the built substrate (event-helper,
commit 5df9b25), tools/des/spurs_trace.py.

## Synthesis → docs/19 (full implementation plan)

Once A–D return, docs/19 is generated as an ordered, dependency-aware plan:

1. **Merge** the R-specs + dedupe against what's already built (event-helper,
   r4-r7 ABI, P2 substrate).
2. **Order by dependency** on the critical path to the dialog:
   verification-harness (D) first (so every step is measurable) →
   task-completion side-effects (A) → HighGraphics cond-2 signal (B) →
   [cascade: Core.Res is starved-not-broken, unblocks for free] →
   SLSession sema-3 (C) → cellSaveDataAutoLoad2 (14a) → render overlay (14b, docs/16 Phase 4).
3. Each task gets: exact files/functions to touch, the R-spec requirements it
   satisfies, the verification (spurs_trace.py observable), and an effort tag.
4. Flag parallelizable vs serial tasks + the risk/mitigation per task.

## Reproducing this crunch later

Re-run the four Agent prompts (task-completion / HighGraphics-cond2 /
SLSession-sema3 / nondeterminism) against RPCS3 source + our lifted code; each is
self-contained and idempotent. The synthesis step is docs/19.
