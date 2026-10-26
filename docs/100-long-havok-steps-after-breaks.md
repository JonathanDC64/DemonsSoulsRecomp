# 100. What holds the long Havok steps after a break (2026-10-24)

Follows docs/77 and docs/98. docs/77 found that an ordinary Havok step is held by the Havok PPU Thread's own work,
with image 8's tasks mostly waiting for it. docs/98 made that work faster with the lifter's SSE4.1 helpers and stack
stores (+9.1% and then +5.8% fps after the breaks on docs/73's break replay). The spikes are still there: in the
first seconds after a break some steps take 12-25 ms. This note profiles those long steps on the new build and
measures three changes against them.

Labels: **measured** (a run says so), **read** (seen in the code), **inferred**.

## Short answer

- **In a long step the Havok PPU Thread waits for image 8** about a third of the time, and image 8 runs on about
  2.3 SPU threads, against 0.5 in an ordinary step (measured). An ordinary step is the reverse: the PPU's own work
  holds it (docs/77). That is why the step mean hardly moves when image 8 gets faster, while the spikes are long
  steps.
- **Three changes moved nothing** (measured; every run with the session locked, `RSX_PRESENT=0`):
  - image 8 lifted with `--regions` (its job code faster; docs/77 measured -24% job time): -0.9% fps after the
    breaks, overlapping;
  - a 100 us spin before the Havok thread's event-queue wait sleeps (`EQ_SPIN_US`, scratch): -0.1%, overlapping;
  - the lift compiled with `-mfma -mmovbe` (a minimum CPU with FMA3 and MOVBE): +0.9% on the replay and +2.3% at the
    deep Palace, both overlapping, the second from one low run.
- **Inferred:** most of image 8's time in a long step is synchronisation. That means the semaphore its tasks take
  work from and the job-queue lock, both in the MFC atomic path. Neither change touches that. The next step is a
  timeline of one long step (section 4).

## 1. How the long steps were caught

A profile started after each of the replay's rolls begins once the roll's long frames are over, so they never show.
Three scratch tools catch them instead:

- **`proft <tid> <seconds> <image>`**, a console verb in the host's `boot_main.cpp`. From a thread of its own, about
  every millisecond, it samples one PPU thread and every SPU thread running the given image. Each sample has a
  `steady_clock` timestamp and the raw stack (host frames, game-DLL frames, DLL leaves).
- **`DES_FRAMES=<file>`**, in the replay's probe host: each frame's end on the same clock, and whether the frame
  stepped the physics.
- **`longstep.py <frames> <samples> [long_ms]`**: puts each sample in the frame it falls in and splits the frames
  into long step frames (stepped, over 12 ms), ordinary step frames (stepped, under 8 ms) and the rest. Per class
  it reports the samples a frame by leaf, by nearest lifted function, and inclusive.

The samples cover 90 s from the first roll, while the frame log covers the whole run, so only the frames inside the
sampled window count below.

## 2. The profile (measured)

Two replay runs with the helpers and the stack stores (the probe DLL, `PS3_MAIN_STACK_LV2=1`), the frames inside the
sampled window:

| | long step frames | mean | ordinary step frames | mean | other frames | mean |
|---|---|---|---|---|---|---|
| run 1 | 27 | 14.92 ms | 2,612 | 4.33 ms | 22,682 | 3.43 ms |
| run 2 | 33 | 14.73 ms | 2,613 | 4.25 ms | 22,894 | 3.40 ms |

The sampler reads the Havok thread about every 1.6 ms in every class of frame, so shares of its samples compare
directly. Run 2 below; run 1 is within a few points.

**The Havok PPU Thread**:
- **`sys_event_queue_receive` under `func_010D48C8`: 32% of its samples in a long frame, 5% in an ordinary step
  frame.** The chain above it is `func_012991C0` -> `func_012931F8` -> `func_011BF690` -> `func_011DE908` ->
  `func_011E3DE8` -> `func_011DD538` -> `func_011DD1D8` -> `func_011DCC20` -> `func_010D0EB8`. It sits beside
  docs/77's job poster `func_010D4808`: the thread posts image 8's jobs, then waits for them.
- `sys_cond_wait` under `func_015C88C8`, the wait for the next step: 23% against 73%.
- The rest, 45% of a long frame, is the thread's own work, spread thin as in docs/77 (no lifted function above 3%).

**Image 8**, all its threads: 2.3 threads in the image per sample in a long frame, 0.45 in an ordinary step frame.
Shares of its samples in a long frame, inclusive:
- the tasks' main loop (`func_00006280` -> `func_000052B0`) 55%, and the job loop (`func_00011138`) 40%;
- the semaphore loop (`func_00011B5C`) 26%, and its wait for the lost-reservation event (`func_00011BF8`) 9%;
- the job-queue lock (`func_00011E0C`, a ticket lock polled with GETLLAR) 8%;
- **`spu_wrch`, the MFC commands, 37%**: `spu_atomic_lean` (PUTLLC) 25%, GETLLAR reads (`spu_getllar_read`) 15%,
  `mfc_for_slow` 7%;
- the per-hop calls (`spu_drain_call`, `spu_step_check`, `spu_depth_guard`, `spu_lookup`), exclusive: 16%;
- job workers (`psj_worker`) waiting in the kernel: 9%.

How each side waits (read):
- **Image 8's tasks spin.** `spu_llar_nap_check` sleeps only after 20,000 unchanged polls. docs/77 measured that
  sleeping sooner costs 6-9% fps after the breaks.
- **The Havok thread does not.** An untimed `sys_event_queue_receive` on an empty queue goes straight to
  `SleepConditionVariableCS`, so each completion it waits for ends in a kernel wake-up.

## 3. Three changes on top (measured)

Each on the break replay: fps in the 2 s windows after each of the ten breaks, ABBAABBAAB (5 runs a side), one host
and the same sources for both sides.

**Image 8 lifted with `--regions`** (`REGIONS` in `scripts/build_spu.py`; both DLLs compiled the same hour):

| | fps after the breaks | mean | worst frame ms | frames over 16 ms |
|---|---|---|---|---|
| plain | 279.1 / 283.3 / 282.4 / 282.9 / 280.8 | 281.70 | 20.7 / 16.7 / 22.3 / 18.1 / 15.9 | 2 / 1 / 4 / 3 / 0 |
| regions | 278.6 / 276.6 / 281.4 / 281.6 / 278.1 | 279.26 | 21.1 / 61.8 / 19.2 / 19.1 / 18.6 | 4 / 3 / 1 / 3 / 1 |

-0.9% after the breaks, -0.7% before them, overlapping. The one 61.8 ms frame is a single hitch; without it the
worst frames match. Not kept. docs/77 also saw +3% on the means with 2 rounds of 6 lost.

**A spin before the Havok thread sleeps** (scratch: `EQ_SPIN_US=<n>`, before an untimed receive sleeps on an empty
queue it polls the count for up to n us; the receiver is counted in `waiters` first, since SPURS notifies only
queues with a receiver; off against 100 us, one exe):

| | fps after the breaks | mean | worst frame ms | frames over 16 ms |
|---|---|---|---|---|
| off | 274.9 / 279.1 / 279.4 / 281.6 / 276.8 | 278.36 | 18.7 / 21.9 / 29.1 / 22.0 / 20.7 | 5 / 5 / 5 / 4 / 6 |
| 100 us | 281.2 / 274.7 / 275.3 / 278.9 / 280.8 | 278.18 | 18.4 / 20.6 / 20.2 / 16.7 / 22.3 | 1 / 6 / 4 / 1 / 3 |

-0.1% after the breaks; fewer long frames on the means (5 to 3), overlapping. The Havok thread's cycles after the
breaks rise 17%, since spinning counts as busy. Not kept. Inferred: either its wake-ups are not what makes a long
step long, or its waits there are mostly longer than 100 us.

**FMA3 and MOVBE** (the lifted PPU, SPU and libsre objects compiled with `-mfma -mmovbe`; ABBAAB, 3 runs a side):
- on the replay: 259.0 -> 261.3 fps after the breaks (+0.9%), the step 1.8% slower, overlapping;
- at the deep Palace: 161.97 -> 165.70 fps (+2.3%), overlapping, and only because of one plain run at 147.3. Without
  it the plain side reads 169.3, above the other.

Not kept. A minimum CPU with them would also have needed a check that no separate multiply and add in the lifted
float code get fused.

## 4. Next

- **A timeline of one long step.** Use docs/77's scratch marks in image 8's lift (semaphore take and give, lock
  take and release) and marks on the Havok thread's post (`func_010D4808`) and wait (`func_010D48C8`). Read them
  from the console trace after a break (`trace 2`, docs/77's `span8.py`). It shows what each side waits for
  between a post and its completion, and how many round trips a long step makes.
- **The MFC atomic path itself.** `spu_atomic_lean` and `spu_getllar_read` are 25% and 15% of image 8's samples in a
  long step, most of it in the semaphore loop and the lock. A cheaper GETLLAR and PUTLLC on a line no one else is
  writing would cut both (inferred).
- The profiling tools stay scratch. If long-step profiling comes back, `proft` and `longstep.py` are worth a
  tooling PR.
