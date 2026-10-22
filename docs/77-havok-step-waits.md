# What the physics step waits on after a break (2026-10-21)

Follows docs/73. That note found that, for a few seconds after a breakable breaks, the Havok step's image 8 tasks take most of the extra frame time, and left "a faster SPU path for image 8" as the next item. This note profiles image 8 on its own, finds that its tasks mostly wait, and fixes the wait.

Labels as in docs/48: **measured** (from a run), **read** (seen in the code or the lift), **inferred**.

## Short answer

- **Image 8's tasks mostly wait for work** (measured, console trace). They spend 60-85% of their time in a semaphore loop, waiting for the next job. A faster image 8 alone did not shorten the step: lifting it with `--regions` cut its own job time by 24% and left the step frames where they were.
- **The Havok PPU Thread posts every job** (measured, `PPU_WWATCH`). It polls the job-queue lock with `cellSyncMutexTryLock` and `usleep(1)` between tries.
- **About 1 in 40 of those `usleep(1)` calls took 0.05-1.7 ms** (measured). The runtime spun on `SwitchToThread` to the deadline, and that hands the core to any ready thread for its whole quantum.
- **Fix: ps3recomp #97.** A `usleep` of at most 10 us waits in place on the pause instruction, as RPCS3's does. `USLEEP_SHORT_SWITCH=1` brings back the old way. Step frames after a break went from 7.34 to 6.96 ms, lower in each of 4 pairs; frames over 16 ms from 66 to 45 (measured).
- **What limits the step now** (measured, trace and `prof`): the Havok PPU Thread's own serial work, about 1.9 ms a step, spread over many Havok functions with none above 0.4% of the thread. Image 8's four tasks take a batch of four jobs together, finish it in about 30 us and wait about 70 us for the next.
- **Region lifts on top of the fix:** image 8's is 3% better on the means but lost 2 rounds of 6, so it is not in `build_spu.py`'s `REGIONS`. Image 7's gained nothing.
- **Spin-then-sleep for the semaphore wait:** it saves 0.3 of a core and costs 6-9% of the frame rate after breaks. Dropped.

## How it was measured

- The 10 replayed breaks from docs/71 and docs/73 (`rep2.txt`) on a copy of that save, uncapped, `RSX_PRESENT=0`, `DES_GOD=1`, with docs/73's `DES_PHYST` probe. `brkstat.py` gives the step-frame mean, fps and frames over 16 ms in the two 2 s windows after each break.
- Interleaved A/B runs, one at a time, on a quiet PC. Two earlier A/Bs that day were thrown out: a stray `grep` of mine was using a core through both.
- New console command `profimg <image> [s] [rip] [raw]` (ps3recomp #98): it samples only the host threads that are running that SPURS task image at the moment, so the pool threads' other images stay out of the profile.
- Scratch marks in image 8's lift (semaphore take and give, lock take and release) and docs/73's history marks, in the console trace (`trace 2`) after each break.
- `PPU_WWATCH=477FD980 PPU_WWATCH_LEN=0x80` for the PPU writes to the semaphore's line.

## Image 8 on its own (measured, `profimg 8 3 raw` after each break)

| Where the time goes | before #97, plain lift | before #97, `--regions` |
|---|---|---|
| Samples in image 8 (all threads) | 2,908 | 2,410 |
| Semaphore loop (`0x11AF8`-`0x11BF8`) | 41.0% | 45.9% |
| Job work | 59.0% | 54.1% |
| ...of which per-hop overhead (`spu_step_check`, `spu_drain_call`, `spu_depth_guard`, `spu_lookup`, `spu_indirect_branch`) | 23.6% | 4.6% |
| ...of which lifted code | 42.2% | 42.7% |
| ...of which DLL leaves (memcpy, waits) | 15.0% | 29.7% |
| ...of which GETLLAR and reservations | 8.9% | 9.6% |

- The region lift did what it does: the per-hop share fell from 23.6% to 4.6% of the job work, and image 8's job samples fell by 24% (1,716 to 1,304).
- Step frames after breaks did not move: 7.81, 7.78, 8.18 ms without it and 7.92, 7.78, 8.17 ms with it (3 pairs, measured; this A/B also had the stray `grep` running, in both arms).
- About a fifth of the job-work samples have no lifted frame at all: the pool thread between tasks, or in a nap.

## The semaphore and the lock (read, then measured)

Image 8's code at `0x11AF8` takes work from a counting semaphore on the line `0x477FD980`. It does GETLLAR, reads a halfword count, and PUTLLC's it down by one. If the count is 0, it waits for the lock-line-lost event and tries again. `0x11C08` gives to the same semaphore. `0x11CC8` and `0x11E98` take and release a ticket lock on `0x477FD800`, the job-queue lock: a `CellSyncMutex`.

From the marks, in 2 s after a break (measured, three windows):

- Image 8 never gives to the semaphore. Every post comes from the PPU: `[ww] 0x477FD980 <- 0x04000001`, guest function `0x010D4808`, thread 11, the Havok PPU Thread.
- The job-queue lock is held 3-12% of the time image 8 runs, for about 1.3 us at a time. Waiting for it is 1-4% of the tasks' time. It is not the limit.
- The semaphore wait is 60-85% of the tasks' time: 160-590 us a wait on average, up to 3.3 ms.

## The Havok PPU Thread's poll (measured, console trace)

In 2 s after a break, the Havok PPU Thread made, besides its idle waits on cond 0x13:

| Call | Count | Total |
|---|---|---|
| `sys_timer_usleep(1)` | 2,506 | 25.7 ms |
| `cellSyncMutexTryLock` | 19,331 | 1.9 ms |
| `sys_lwmutex_lock` / `_unlock` | 20,321 each | 2.3 / 2.0 ms |
| `cellSpursCreateTask` | 240 | 1.3 ms |

Most `usleep(1)` calls took about 1 us. 61 took over 50 us (21.4 ms in all), 33 over 200 us, the longest 1.7 ms. Each of those holds back the next post, so all four tasks wait with it.

The cause was the spin in `sys_timer_usleep` (ps3recomp `runtime/syscalls/sys_timer.c`, read): for a sleep under 1 ms it called `SwitchToThread` until the deadline. `SwitchToThread` gives the core to any other thread that is ready on it, and the caller then waits until that thread's quantum ends. RPCS3 waits in place for short sleeps (`tpause` or `mwaitx` in `lv2_obj::wait_timeout`, read).

## The fix and its A/B

ps3recomp #97: a `usleep` of at most `USLEEP_YIELD_US` (10 us) spins on the pause instruction to the same deadline. Off Windows it did `nanosleep`, which adds the timer slack (50 us by default) to every call, so it spins there too. `USLEEP_SHORT_SWITCH=1` restores the old behaviour.

4 interleaved pairs (measured):

| Pair | Step frames after breaks, before | after | Frames over 16 ms, before | after |
|---|---|---|---|---|
| 1 | 7.34 ms | 6.93 ms | 15 | 7 |
| 2 | 7.56 ms | 6.83 ms | 23 | 10 |
| 3 | 7.27 ms | 7.15 ms | 16 | 16 |
| 4 | 7.19 ms | 6.94 ms | 12 | 12 |

- Mean 7.34 to 6.96 ms after breaks (-5%), and 6.62 to 6.12 ms for step frames away from breaks (-7.5%).
- Average fps after breaks 182.7 to 186.2.
- In a trace with the fix, no `usleep(1)` of the Havok PPU Thread took over 50 us, and the median Havok step (the span between its idle waits) went from 2.85 to 2.27 ms.

## What limits the step now (measured)

With the fix, the longest step of a trace looks like this (times from the Havok thread's wake-up):

- 0.0-1.0 ms: the four image 7 tasks, then image 8 starts.
- From about 3.1 ms on, every 100 us or so: the four image 8 tasks take a job each within 10 us of one another, finish it in about 30 us, and go back to waiting.
- In each gap the Havok PPU Thread reads an event (`sys_event_queue_receive` on queue 3, 3 us), then runs guest code for 40-60 us, with a few `cellSyncMutexTryLock` calls in it, before the next four posts.

A `prof` of the Havok PPU Thread over the 3 s after each break: idle 94.2% (`ntdll` waits), the rest spread over many Havok functions (`func_0122B770` 0.4%, `func_01286120` 0.3%, `func_01285458` 0.3%, and so on). That is about 1.9 ms of PPU work a step (inferred: 5.8% of 30 s over about 900 steps).

So the step is now a relay: PPU work, then a short SPU batch, then PPU work again. The PPU part is ordinary lifted code with no single hot spot. What would shorten it is the general PPU code speed work, not anything specific to Havok.

## Region lifts on top of the fix (measured)

Two interleaved A/Bs, 3 rounds each, every build with ps3recomp #97:

- **First A/B, both region lifts against the plain lift.**
  - Image 8's: step frames after breaks 6.89 ms against 7.13 ms on the means, but round 3 went the other way (A 6.80 ms).
  - Image 7's (`spu_0006_at_017AE200`): no gain.
- **Second A/B, image 8's against the plain lift**, in the same batch as the spin-then-sleep runs below:

| Round | Step frames after breaks, plain | regions | fps after breaks, plain | regions |
|---|---|---|---|---|
| 1 | 7.05 ms | 6.94 ms | 187.9 | 187.6 |
| 2 | 7.45 ms | 6.94 ms | 177.8 | 187.1 |
| 3 | 7.07 ms | 7.11 ms | 184.5 | 183.7 |
| Mean | 7.19 ms | 7.00 ms | 183.4 | 186.1 |

- Frames over 16 ms: 46 plain, 29 with regions. CPU use: 5.28 and 5.22 cores.
- Over the 6 rounds, the region lift had the shorter step in 4 and the longer in 2. A change has to win every round to go in, so it stays out of `REGIONS` for now. The decisions list carries it: more rounds could settle it.
- This fits the profile above. The tasks wait for the PPU, so a faster image 8 only shortens the roughly 30 us batches between posts.

## Spin-then-sleep for the semaphore wait (measured)

docs/73's `SPU_LR_WAIT` napped at once and made the steps slower. This one spins first: in `rdch SPU_RdEventStat` with only the lock-line-lost event enabled, it polls the reserved line for up to `SPU_LR_SPIN_US` microseconds, then naps until a write wakes it (`spu_llar_idle_wait`). Scratch patch, kept locally (`work/tmp/scratch_20261021/havok/sts_patch.py`).

3 interleaved rounds, the same build with `SPU_LR_SPIN_US` set or not (measured):

| | Spin off (today) | Spin 100 us | Spin 30 us |
|---|---|---|---|
| fps after breaks | 183.4 | 166.7 | 172.9 |
| Step frames after breaks | 7.19 ms | 9.30 ms | 8.75 ms |
| Frames over 16 ms | 46 | 144 | 143 |
| fps away from breaks | 186.7 | 172.1 | 177.9 |
| CPU use (cores, from 60 s on) | 5.28 | 4.96 | 5.00 |

- Every round was slower with the spin, at both lengths, and away from breaks too.
- The nap saves 0.3 of a core, but each wake from it comes late. A post that arrives during a nap holds all four tasks back until the waker gets to them, the same failure as `SPU_LR_WAIT` in docs/73.
- The rule was to keep it only if the frame rate does not drop, so it is dropped. The patch stays scratch.

## Watch out

- A Claude Code `TaskStop` on a foreground command that was moved to the background does not always end the process. A `grep` over a 1 GB session transcript ran for 45 minutes through two A/Bs and lowered every run's fps by about 15%. Check for your own leftover processes before an A/B.
- With `--regions`, image 8's semaphore loop is `spu_region_00011AF8`, not `spu_func_00011B5C`: a stack script that looks for the function names finds no semaphore at all.
- `profimg` samples threads by the image slot each thread publishes. A slot two thread ids share by chance (ids are hashed into 1024 slots) can put a sample in the wrong image; it was not seen in these runs.
- The marks cost: a trace with them has 1.3 million records in 2 s, and its step times are longer than in a run without. Use the trace for the order of events, the A/B runs for times.
- `cellSyncMutexTryLock` and the lwmutex calls are HLE and cheap (about 0.1 us each). The poll's cost was the sleeps between them.

## Tools used (scratch, never committed)

Copies are kept locally in `work/tmp/scratch_20261021/havok/`; `work/` is not in git.

- `profimg.sh <tag> <image> [rip|raw]`: replays the breaks and runs `profimg <image> 3` after each roll.
- `stk.py <con.stacks> <des_boot.map> [image prefix]`: splits profimg's raw stacks into the semaphore loop and job work, by leaf category, leaf function and the nearest lifted frame. `dllup.py`: for samples whose leaf is in a DLL, the exe function that called out.
- `mark8.py <region lift> <out>`: adds the six marks to image 8's region lift. `an8.py <trace>...`: lock holds, lock waits and semaphore waits per window. `span8.py <trace> [rank]`: the rank-th longest Havok step as a timeline.
- `ab8.sh` and `abn.sh`: interleaved A/B of the replayed breaks for two or more exes. `abn3.sh` adds a per-variant environment and the game's CPU use (a PowerShell sampler of the staged `des_boot`'s processor time every 5 s).
- `us_patch.py` (the fix as first written), `sts_patch.py` (spin-then-sleep).
