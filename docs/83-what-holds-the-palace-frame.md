# 83. What holds the deep Palace frame (2026-10-22), and what to cut next

A re-profile of fork master (654aacb, with docs/72's image 2 work and the usleep fix) at the deep Palace save, after Stage 4 (docs/80) and the reply-wait bound (overnight notes) both came out with no gain. It answers which thread sets the frame now, measures where that thread's time goes, and designs the next cuts.

Labels: **measured** (a run says so), **read** (seen in the code), **inferred** (reasoned, not run).

## Short answer

- **In plain play HighGraphics holds the frame** (measured, slopes without the profiler, section 2.1): a wait at its flip request lands in full, while the FIFO thread has about 0.1-0.25 ms of slack. **In profile windows the FIFO thread holds it** (section 2): the profiler's lookup timing loads that thread, and there it has about 0.06 ms of slack and HighGraphics 0.3-0.5 ms. Image 2 has spare CPU, but its per-command delay lands in the frame, because HighGraphics and the FIFO thread both wait on what it produces.
- **Three items are about 40% of the FIFO thread** (measured, profile): `d3d12_draw_indexed` 13.8%, `rt_writeback_store` 12.6% (the render targets copied back into guest memory each frame), and memcpy 12.4% at its top address alone.
- **The vertex traffic is large and mostly repeated** (measured, `VP_FETCH_STATS`): the GPU fetch path copies 29.1 MB of vertex spans a frame into the upload ring, at least 80.4% of them byte for byte what the same span held at its last copy. The CPU path writes another 16.8 MB. Triangle lists and quads are under 1% of the indices.
- **Upper bounds** (section 5): in plain play skipping the span copies gives +2.7%, the write-back +2.3%, both +3.6%; then HighGraphics sets the frame. (In profile windows: +6.7%, +6.5%, +7.6%.)
- **Order of work** (inferred, section 6): image 2 first. In plain play HighGraphics holds the frame, but it waits on image 2: docs/78's coherent store (-0.5%) and the reply wait (-0.2%) give nothing there, while image 2's per-command delay lands at 1.7x (section 2.1). Stage 4's native loop read +1.4% in plain windows (docs/80 4, rerun pending). The FIFO-thread cuts are worth at most about 3.6% together until then; of them, the write-back off the FIFO thread first, then the span copies onto the vertex worker, then a resident span cache. docs/64's lists and quads are not worth doing alone.

## 1. Conditions

- Deep Palace save (`work/tmp/lc_deep/save`, Palace 1-1, `m02`), uncapped (`DES_FPS=unlocked DES_VSYNC=0 RSX_VSYNC=0`, vblank 960 kHz), not presenting (`RSX_PRESENT=0`: the session was locked).
- **A Node.js dev server ran beside every run tonight**, not part of this project: about 10.5 s of CPU in each 6 s quiet-check window, so about 1.7 cores (measured). Tonight's baselines read 144-150 fps where docs/72 read 163.1 on 10-21. The A and B sides of every in-boot A/B share that load, so the comparisons hold; the absolute rates are lower (inferred: nothing else on this PC changed between the two nights that I know of).

## 2. The slopes (measured, profile windows)

A scratch build of fork master with busy-wait knobs at six places (`TMP_SLOW_*`, local branch `scratch/causal-1022`, never pushed). 2 boots, windows ABBAAB of 6 s each, `TMP_SLOW_RATE=1` for the per-frame counts. A slope is the share of the added time that shows up in the frame time.

| Busy wait | Where | Count a frame | fps A -> B | Frame time added | Wait added | Slope |
|---|---|---|---|---|---|---|
| 500 µs per flip (`TMP_SLOW_FLIP_US`) | HighGraphics, at its flip request | 1 | 146.6 -> 142.7 | +0.19 ms | 0.50 ms | 0.37 |
| 500 µs per present (`TMP_SLOW_PRESENT_US`) | FIFO thread, before Present | 1 | 145.1 -> 136.4 | +0.44 ms | 0.50 ms | 0.87 |
| 200 ns per draw (`TMP_SLOW_DRAW_NS`) | FIFO thread, each draw call | 6,418 | 146.8 -> 124.5 | +1.22 ms | 1.28 ms | 0.95 |
| 25 ns per ring command (`TMP_SLOW_RING_NS`) | image 2, each command its native fetch takes | 38,484 | 147.7 -> 122.8 | +1.37 ms | 0.96 ms | 1.43 |
| 50 ns per coherent store (`TMP_SLOW_COH_NS`) | every PPU store into a line an SPU has reserved | 39,290 | 147.9 -> 121.3 | +1.49 ms | 1.96 ms | 0.76 |
| 300 µs per `sys_cond_signal` of guest thread 1 (`TMP_SLOW_SC=1,108,*,300`) | main | 0.4 | 148.1 -> 148.0 | +0.00 ms | 0.12 ms | 0.02 |

6 pairs each. The first five have p of 0.007 or less; the control (the last row) has p 0.94.

**How to read them.** Each wait adds 0.5-2 ms to a frame of about 6.8 ms, more than any thread's slack. So a thread with slack s in a frame gives a slope of about 1 - s / added (inferred; a simple model in which the wait first uses up the slack, then lands in the frame).

| Thread | From | Slack a frame (inferred) |
|---|---|---|
| FIFO thread | present 0.87, draw 0.95 | about 0.06 ms (1% of the frame) |
| HighGraphics | flip 0.37, coherent store 0.76 | about 0.3-0.5 ms (5-7%) |
| Image 2 | ring 1.43 | none measurable |
| Main | the control | all of it, at that call |

- **The per-call slopes read high.** `tmp_spin_ns` waits on `rdtsc` with a `pause` in the loop, and one `pause` takes tens of nanoseconds on recent CPUs, so a 25 ns wait lasts longer than 25 ns (inferred). That is why the ring slope is above 1; the draw and coherent-store slopes are upper bounds too. The 500 µs waits have no such error.
- **Image 2 shows no slack, yet it has spare CPU.** Stage 4's native loop took about 13% of image 2's statements away and the frame did not move (docs/80), and image 2 polls the ring 20% of its time (`SPU_RATE`, section 3). Both fit a per-command coupling: a delay on each command delays the reply HighGraphics waits for and the RSX commands the FIFO thread reads, while image 2's own throughput has room (inferred).
- **So a FIFO-thread cut shows almost 1:1, up to HighGraphics' slack**: about 0.3-0.5 ms, +5-8% (inferred). Past that, HighGraphics and the image 2 hand-off set the frame.

### 2.1 The same slopes without the profiler (measured)

The table above was taken in profile windows, and the profile session times every texture, SRV and pipeline lookup on the FIFO thread (section 7). The same exe and knobs again in plain windows: each window is two console `stat` calls, the flip count at either end, and the frame rate comes from `FRAME_LOG` (a scratch copy of knobab, `knobab_plain.sh`). 2 boots, ABBAAB, 6 pairs each; a null line (an unused knob name) for the noise.

| Busy wait | fps A -> B | Frame time added | Wait added | Slope, plain | Slope, profile |
|---|---|---|---|---|---|
| 500 µs per flip, HighGraphics | 156.2 -> 144.9 | +0.50 ms | 0.50 ms | **1.00** | 0.37 |
| 500 µs per present, FIFO thread | 157.7 -> 148.6 | +0.39 ms | 0.50 ms | 0.77 | 0.87 |
| 200 ns per draw, FIFO thread | 154.5 -> 133.1 | +1.04 ms | 1.28 ms | 0.82 | 0.95 |
| 25 ns per ring command, image 2 | 157.1 -> 124.7 | +1.66 ms | 0.96 ms | 1.72 | 1.43 |
| 50 ns per coherent store | 157.1 -> 124.4 | +1.67 ms | 1.96 ms | 0.85 | 0.76 |
| null knob | 156.9 -> 156.6 | +0.01 ms | 0 | | |

The first five have p < 0.001; the null line has p 0.65 (-0.17%). The per-frame counts are the profile run's (`TMP_SLOW_RATE=1`); the content is the same, so they should carry over (inferred).

- **In plain play HighGraphics holds the frame, not the FIFO thread.** A wait at HighGraphics' flip request lands in full (slope 1.00), and image 2's ring delay more than in full (1.72, with the `pause` bias of section 2). The FIFO thread has slack: 0.12 ms by the present wait, 0.23 ms by the draw wait, about 2-4% of the frame (inferred, the same model).
- **The profiler moves the bottleneck.** Its lookup timing adds enough to the FIFO thread to make it the longest chain; that is why a FIFO-thread cut showed +6-8% in profile windows and much less between them (the render thread, section 6.4; docs/68 7.3).
- **What it changes:** the FIFO-thread cuts in section 6 are worth at most the FIFO thread's slack in plain play, about 2-4%, until HighGraphics and image 2 get faster. Work on HighGraphics (its coherent stores, docs/78) and on image 2's per-command delay comes first. Stage 4 and the reply-wait bound were measured in profile windows too; both are being re-measured in plain windows.

## 3. Where the time goes (measured, `perfprof.sh prof1022`)

One boot of the plain fork master exe. Profile windows: 144.2, 147.8 and 147.6 fps.

**Threads** (busy share from `QueryThreadCycleTime`, 8 s):

| Thread | Busy | Note |
|---|---|---|
| FIFO thread (`vblank_ticker`) | 98.8% | holds the frame |
| HighGraphics | 98.1% | 10.9% of it in its usleep wait |
| Image 2 (`spu_async_thread`) | 91.4% | polling the ring 20% of its time, napping 1% (`SPU_RATE=2`) |
| main | 33.9% | |
| Vertex worker (`vpj_worker`) | 29.5% | 72% of its samples in a wait: idle most of the time |
| SPURS exec, ChrInsMan | 25.6%, 24.9% | |

**The frame chain** (`trace 2`, 298 frames, medians): frame 6.53 ms; HighGraphics' work 5.78 ms and its reply polls 0.63 ms; image 2 asleep 0.50 ms; the FIFO thread reaches each flip request 2.06 ms after HighGraphics makes it; `render_frame` to present takes 1.76 ms of the FIFO thread.

**The FIFO thread's functions** (sampled, exclusive):

| Function | Share | What |
|---|---|---|
| `d3d12_draw_indexed` | 13.8% | each indexed draw: state, records, the fetch decision (about 150 ns a draw call at 6,418 a frame) |
| `rt_writeback_store` | 12.6% | each frame's render targets converted into guest memory (section 4.2) |
| memcpy (`VCRUNTIME140.dll+0x1C440`) | 12.4% | the span copies above all (section 4.1); more C runtime copy addresses follow below 2% each |
| `gcm_rsx_process_fifo_unlocked`, `rsx_process_method(_run)` | 7.7%, 6.1% | the FIFO parse |
| `upload_strip_vp_indexed`, `rsx_fetch_vertex_arrays` | 7.0%, 6.8% | the strips that fall back to the CPU path, and their vertex convert |
| `vp_get_vs_mode` | 5.8% | the vertex program's mode, per draw |
| `render_frame` | 5.0% | the frame's recording and submit |
| `vp_bind_sampler_table`, `vpj_sync`, `rsx_fetch_plan_init`, `cellGcmResolveLocated`, `vp_record_cb` | 2.1, 1.8, 1.6, 1.6, 1.4% | |

The profiler lists some samples under two neighbouring addresses (the waits show as equal pairs), so sums over neighbouring addresses are not exact.

**HighGraphics**: the coherent-store path is 19.4% of the thread (`spu_lockline_unlock_ea` 6.2%, `spu_lockline_lock_ea` 5.2%, `spu_ll_acquire` 4.6%, `spu_coh_notify_write` 3.4%), the same as docs/72 and docs/78 found. **Image 2**: `ring_loop` 15.6%, `spu_wrch_word` 12.3%, `mfc_fast_xfer` 11.7%, region 0x8CB8 10.5%.

## 4. The two big copies

### 4.1 Vertex spans (measured, `VP_FETCH_STATS=600`, docs/64 step 0)

Per frame, the last 6 windows of 600 frames:

| Primitive | What happened | Ranges | Indices |
|---|---|---|---|
| Strips | GPU fetch, appended to the last descriptor | 2,583 | 636,850 |
| | GPU fetch, new descriptor | 1,632 | 293,865 |
| | GPU fetch, the last descriptor reused | 1,249 | 287,187 |
| | CPU path, sparse (the only reason seen) | 663 | 118,218 |
| Triangle lists | CPU path; all would pass the fetch tests | 162 | 9,676 |
| Quads | CPU path; all would pass the fetch tests | 69 | 276 |

- **The fetch path copies 29.1 MB of spans a frame** (29,750 KB), about 4.4 GB/s at 150 fps, all on the FIFO thread, into `vf_buf`, an upload heap the GPU then reads over the bus (read). At least 80.4% of those bytes are the same as the last copy of the same span (address, size and a hash of the bytes; a table of 8,192 spans, so this is a lower bound).
- **The CPU path writes 16.8 MB a frame** more: 68,800 vertices of 256 bytes (`VP_VSTAT`), 54,466 of them for the sparse strips.
- The same numbers at 200% render scale.

### 4.2 The render-target write-back (read)

`rt_writeback_record` copies every offscreen target the frame used into a readback buffer at the end of the frame (`RSX_WRITE_COLOR`, default 1). At the start of the next frame, after the GPU wait it makes anyway (`RSX_WRITE_COLOR_DEFER`, default 1), `rt_writeback_store` converts each one into guest memory a word at a time (RGBA8 rotated, FP16 byte-swapped, swizzled targets in Morton order), then checksums each stored range for `off_rt_drop_rewritten`. All of it runs on the FIFO thread, at the start of the frame. The byte count a frame is not logged; its comment put it at about 25 MB when it was written.

## 5. Upper bounds (measured)

A scratch build (`TMP_VF_NOCOPY=1` skips the span copies; never pushed) and the existing `RSX_WRITE_COLOR=0`, as in-boot knob A/Bs (`knobab.sh`, 3 boots, ABBAAB, 9 pairs each). Both leave wrong pixels, so they only say how much frame time each copy holds.

| Knob | A fps | B fps | Change | Frame time |
|---|---|---|---|---|
| `TMP_VF_NOCOPY=1` | 146.1 | 155.9 | +6.7% | -0.43 ms |
| `RSX_WRITE_COLOR=0` | 147.4 | 156.9 | +6.5% | -0.41 ms |
| both | 147.6 | 158.9 | +7.6% | -0.48 ms |

B was faster in every pair, p < 0.001.

- **Either copy alone gives about +6.5%, both together only +7.6%.** The frame hits a ceiling about 0.45 ms down, which fits HighGraphics' slack from section 2 (0.3-0.5 ms). So one of the two cuts gets most of what the FIFO thread can give; the second adds about 1% (inferred).
- **These are profile windows**, where the FIFO thread carries the profiler's lookup timing (section 7). The same A/Bs in plain windows (section 2.1's method, 3 boots, 9 pairs each):

| Knob | A fps | B fps | Change | Frame time | p |
|---|---|---|---|---|---|
| `TMP_VF_NOCOPY=1` | 155.5 | 159.6 | +2.7% | -0.17 ms | 0.003 |
| `RSX_WRITE_COLOR=0` | 156.4 | 159.9 | +2.3% | -0.14 ms | < 0.001 |
| both | 157.2 | 162.9 | +3.6% | -0.22 ms | < 0.001 |
| null knob | 157.0 | 157.0 | +0.0% | 0.00 ms | 0.96 |

- **In plain play each copy is worth about 2.5%, both about 3.6%**: the FIFO thread's slack from section 2.1 (0.12-0.23 ms), then HighGraphics sets the frame. These are upper bounds: moving a copy to the vertex worker keeps some of its cost (the hand-off, `vpj_sync`).

## 6. Design

### 6.1 The write-back off the FIFO thread

- **What.** Hand the frame's stores (the readback mappings and the targets' guest ranges) to a worker at the frame's start, instead of converting them on the FIFO thread. The checksums go with them.
- **When the data must be there.** Today the guest memory gets the data at the start of the next frame. With a worker it gets it a little later in that frame. The rule that keeps that unobservable is the vertex worker's: anything the title can see of the RSX's progress waits for the stores first. That is labels, semaphores, reports, `get` and the 2D engine (`vpj_sync`, `g_rsx_sync_hook`, read), plus two more for this case: a texture upload or a draw that reads guest memory overlapping a pending target, and `off_rt_drop_rewritten`. With at most 16 targets, that overlap test is a short loop.
- **Which worker.** The vertex worker is idle most of the frame (72% of its samples wait). One queue for both keeps the order simple, and `vpj_sync` then covers the stores.
- **Expected.** At most +2.3% in plain play (section 5's bound with the write-back off): the FIFO thread saves up to `rt_writeback_store`'s 12.6% (about 0.85 ms), but only its 0.12-0.23 ms of slack shows until HighGraphics gets faster. In profile windows the same bound is +6.5%.
- **Cheaper still, later:** convert on the GPU (a compute pass writes guest byte order into the readback buffer), so the store is a plain copy; and skip the targets the title never reads back, if a way to know that turns up.

### 6.2 The span copies onto the vertex worker

- **What.** In `upload_strip_vp_fetch`, queue (ring offset, guest address, length) instead of copying; the worker copies. The descriptors only need the ring offsets, which the FIFO thread still assigns.
- **Ordering.** The same as the CPU path's queued vertices (read: the comment above `VPJob`): `vpj_sync` before anything observable and before the frame's submit.
- **Risk.** One worker then does about 29 MB of copies a frame on top of its conversions. If it falls behind, the FIFO thread waits in `vpj_sync` instead (1.8% today). Measure its busy share beside the frame rate (`tools/des/cpusample.sh` during an A/B window).
- **Expected.** At most +2.7% in plain play (section 5), for the same reason.

### 6.3 A resident span cache

- **What.** Spans stay in a GPU buffer across frames, keyed by guest address and length, so an unchanged span is not copied again, and the GPU reads it from its own memory instead of over the bus. At least 80% of the bytes would hit (section 4.1).
- **The hard part is knowing a span did not change.** Lifted PPU code writes guest memory directly, so there is no store hook. The options:
  - a hash of the span, on the worker: it still reads every byte, but writes only the misses;
  - the OS's write tracking (Windows: guest memory reserved with `MEM_WRITE_WATCH`, then `GetWriteWatch` per frame), which reports dirty pages with no cost in the lifted code. It needs a Linux counterpart and a change where guest memory is reserved.
- **After 6.2**, and only if 6.2's measurement leaves the copies on the frame's path.

### 6.4 Smaller items

- **docs/64 step 7**, the gather for sparse strips: 54,466 vertices a frame and most of `upload_strip_vp_indexed` + `rsx_fetch_vertex_arrays` (13.8%). Lists and quads (steps 2-4) are not worth it alone: under 1% of the indices.
- **`vp_get_vs_mode`** (5.8%): a per-draw lookup that a cache keyed by the vertex program could make cheaper (inferred; not read in detail).
- **The render thread** (docs/68), measured again on today's master: +6.8% in profile windows; in plain play +1.0% and +1.5% in two in-boot batches (the second p < 0.001) and +1.7% between exeab's profile windows. A real but small gain, the size of the FIFO thread's slack (section 2.1). Parked until HighGraphics and image 2 are faster.
- **docs/78's coherent store without the lock** (`SPU_COH_FAST=1`), on the same exe in plain windows: -0.5% (p 0.53). HighGraphics then waits on image 2 instead, as docs/78 saw in its thread shares.

## 7. How to measure each

In-boot knob A/Bs (`tools/des/knobab.sh`, 3 boots, ABBAAB) with each change behind a live knob, the FIFO thread's and the worker's busy shares beside the frame rate (`tools/des/cpusample.sh`), and the 1.5% bar (`tools/des/knobstat.py`).

**knobab's windows are all profile windows, and the profile session loads the FIFO thread** (it times every lookup and guest syscall). A change on the FIFO thread can therefore read bigger there than in plain play: the render thread read +5.8% in profile windows and +0.7% in the gaps between them (docs/68 7.3). So every FIFO-thread change, and the bounds in section 5, also get an exeab run with the knob on one side (`EXEAB_ENV_B="KNOB=1"`, the same exe on both sides; local tools branch `tools/exeab-side-env`), and the gap fps decides.

Each change also needs regress at 30 and 240 fps, and for 6.1 a check that the title still sees its render targets: the per-draw fingerprints of an F9 capture (`tools/des/capdiff.py`) before and after.
