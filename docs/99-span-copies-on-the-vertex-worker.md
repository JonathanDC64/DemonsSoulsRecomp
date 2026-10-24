# 99. The span copies on the vertex worker, and the night's other runtime cuts (2026-10-24)

Follows docs/83, which found three threads holding the deep Palace frame (HighGraphics, image 2's thread and the FIFO
thread) and designed the next cuts. This note measures six runtime changes made in one night, each on top of the last,
at the deep Palace and on the break replay (docs/73). One of them moves the Palace: docs/83 6.2, the GPU fetch path's
span copies moved from the FIFO thread to the vertex worker.

Labels: **measured** (a run says so), **read** (seen in the code), **inferred**.

## Short answer

- **The span copies on the vertex worker: +4.5% at the deep Palace** (170.8 -> 178.5 fps, every run with it above
  every run without; measured). On the break replay it loses nothing (+0.5% after the breaks, +0.1% before them,
  overlapping). ps3recomp #118.
- **Nothing else moved either benchmark alone** (measured, every pair overlapping): the register saves taken out of
  five hot functions' prologues, the stack cookies off image 2's GETLLAR path, `ps3_env_gen` on a cache line of its
  own, the depth guard's ring per thread with the step check's diagnostics out of line, and a four-way vertex-program
  memo. By the rule (a change goes out if it wins on one benchmark and loses on neither) they stay local.
- **Three of them add up on the replay**: `ps3_env_gen`'s line, the per-hop pair and the memo together read +4.1% fps
  before the breaks (266.0 -> 277.0, every run above), +2.2% after them and +1.3% at the Palace (both overlapping).
  Whether to send them as one PR on that sum is open.
- Why the span copies moved the Palace when the rest did not (inferred): they take about a sixth of the FIFO thread's
  time off it in one piece. The others each took well under 1% of one thread's time, which the other threads' waits
  absorb (docs/83 2.1).

## 1. The changes

| # | Change | Thread | Where |
|---|---|---|---|
| 1 | Cold code out of five hot functions: `mfc_fast_xfer` (456-byte frame and 10 XMM saves -> 88 bytes, none), `spu_lockline_lock_ea`/`_unlock_ea` (5 XMM saves -> none), `rsx_process_method` (5 XMM saves -> 1), `ps3_indirect_call` (664-byte frame and a stack cookie -> 32 bytes, none) | image 2, HighGraphics, the FIFO thread | local |
| 2 | The stack cookies off image 2's GETLLAR path and HighGraphics' lock-line acquire, `/GS` kept | image 2, HighGraphics | local |
| 3 | `ps3_env_gen`, read by every knob check, on a 64-byte line of its own (it shared one with `g_last_hle_name`, written by every HLE call) | every thread | local |
| 4 | `spu_depth_guard`'s ring of drain sites per thread (it was shared by every SPU thread, on its limit's line), and `spu_step_check`'s diagnostics out of line (it pushed six registers on every hop) | image 8 | local |
| 5 | The vertex-program memo keeps four programs per start (was two): with two, `vp_get_vs_mode`'s byte-at-a-time hash was about 3% of the FIFO thread | the FIFO thread | local |
| 6 | The GPU fetch path's span copies (29 MB a frame) as jobs for the vertex worker | the FIFO thread | ps3recomp #118 |

How 1 and 3 were found (scratch tools, never committed): each hot function's prologue read from the host's machine
code (the frame size, the XMM registers saved, the pushes, a stack cookie), and each Palace thread's most-sampled
loads with the global each one names, then the other symbols on that global's cache line.

## 2. The span copies on the vertex worker (6)

- `upload_strip_vp_fetch` copied each draw's vertex spans from guest memory into the fetch ring on the FIFO thread:
  29.1 MB a frame (docs/83 4.1), `memmove` and `memcpy_repmovs` at the top of that thread's profile.
- Each copy is now a job of no vertices in the vertex worker's queue, under the queue's rule (read: the comment above
  `VPJob`): nothing the title can observe of the RSX's progress gets ahead of `vpj_sync` (labels, semaphores, reports,
  the reference, the 2D engine's writes, `get`), and the frame's render syncs first. A draw that appends to its
  descriptor's span extends the last queued copy while that job is unpublished. The queue holds 8,192 jobs.
- `VF_COPY_WORKER=0` copies on the FIFO thread, as before.
- Regress on the state with it: every shot ok, no crash, no HALT (measured).
- It gave more than docs/83's +2.7% bound for it, which was measured on an earlier base.

## 3. Measured

All on this PC with the session locked (`RSX_PRESENT=0` in every run), uncapped, muted; one host per state, each
adding one change to the last.

**The deep Palace** (per boot the mean of 8 windows of 6 s after 20 s; ABCDEEDCBA twice; the clean DLL of docs/98's
first two commits):

| | state | fps (4 runs) | mean | against the row above |
|---|---|---|---|---|
| A | 1 + 2 | 170.82 / 167.52 / 165.44 / 170.60 | 168.60 | |
| B | + 3 | 168.63 / 170.53 / 169.03 / 172.55 | 170.19 | +0.9%, overlap |
| C | + 4 | 167.62 / 171.90 / 168.48 / 169.52 | 169.38 | -0.5%, overlap |
| D | + 5 | 171.33 / 170.68 / 171.63 / 169.69 | 170.83 | +0.9%, overlap |
| E | + 6 | 176.55 / 178.57 / 179.21 / 179.69 | 178.51 | **+4.5%, every run better** |

Earlier the same night, 1 against fork master's runtime: +0.2% at the Palace (167.92 -> 168.32) and -0.5% on the
replay, both overlapping.

**The break replay** (docs/73's ten recorded breaks; fps in the 2 s windows after each break and in the windows before
them; the probe DLL; ABCDEEDCBA twice):

| | fps after the breaks (4 runs) | mean | fps before them (4 runs) | mean |
|---|---|---|---|---|
| A | 263.2 / 262.1 / 266.9 / 261.8 | 263.50 | 271.1 / 266.9 / 267.9 / 258.1 | 266.00 |
| B | 261.7 / 265.4 / 268.7 / 260.1 | 263.98 | 271.4 / 265.8 / 278.2 / 267.9 | 270.83 |
| C | 258.1 / 270.9 / 273.3 / 270.9 | 268.30 | 267.4 / 272.1 / 274.6 / 277.4 | 272.88 |
| D | 271.5 / 270.1 / 266.4 / 269.4 | 269.35 | 279.4 / 275.4 / 276.2 / 276.8 | 276.95 |
| E | 269.1 / 272.1 / 268.9 / 272.5 | 270.65 | 278.6 / 278.2 / 278.1 / 274.2 | 277.28 |

Every adjacent pair overlaps. A to D before the breaks: every D run above every A run.

## 4. Next

- Which thread holds the Palace frame with 6 in (a profile of HighGraphics, image 2 and the FIFO thread on the new
  host), and docs/83 6.1's bound (the render-target write-back) measured on top of 6: run after this note, in the
  overnight log of 2026-10-24.
- 6.1 itself is a larger change than 6.2: the store lands just before `off_rt_drop_rewritten` checks the targets for
  foreign writes, and the next frame's GPU copy reuses the readback buffers. On a worker it needs double-buffered
  readbacks and that check moved after the stores (read).
- The pin: this port moves to a ps3recomp with #118 once #117 and #118 are both on the fork's master (one pin line, so
  a bump in this PR would collide with #170's).
