# First-start stutter: shader files and pipelines (2026-10-23)

The player tried G-Sync / FreeSync (docs/94) at 144 fps, from the Tutorial, with the shader cache cleared before each run (a first start). G-Sync was on: the log said the display followed the frames at 138 Hz. It still stuttered, while Even frames at 72 fps was smooth.

This doc covers:
- what the stutter was;
- the two fixes in ps3recomp (`PS3_SHADER_CACHE_WARM`, `PSO_THREADS`);
- what was measured;
- what is left.

Tags: **(measured)** run on this PC; **(inferred)** not run.

## 1. What it was (measured)

The runs were first starts like the player's: the cache cleared, setup's shader pass, then the game. They used the player's settings (render scale 200%) and the regression save, scripted and muted. Each was the arrival cutscene, then a 34 s walk.

Slow frames are frames more than 1.5 times the frame period after the one before.

| Setting | Slow frames | Over 2× | Worst | Time lost |
|---|---|---|---|---|
| G-Sync / FreeSync, 144 fps (runs at 138) | 23 | 15 | 38 ms | 207 ms |
| G-Sync / FreeSync, 120 fps | 13 | 9 | 38 ms | 161 ms |
| G-Sync / FreeSync, 100 fps | 10 | 2 | 37 ms | 104 ms |
| Even frames, 60 fps (runs at 72) | 9 | 0 | 27 ms | 88 ms |
| G-Sync / FreeSync, 144 fps, second start | 6 | 4 | 16 ms | 42 ms |

- **The slow frames came at the same places in the walk at every rate.** So they are work that takes a fixed time, not a frame budget that is too small. A 72 fps frame has 13.9 ms, so most of that work fits; a 138 fps frame has 7.25 ms, so it shows.
- **A second start had a quarter as many**, so the work was first-time work.

`RF_SLOW_MS` and `PERF=1` showed where the time went:
- the draw pre-pass, in `vp_get_fp_pso`, for draws whose pipeline did not exist yet;
- one frame needed 32 new pipelines and took 73 ms, 55 ms of it creating them one after another.

## 2. The shader files (measured)

A timer around each part of a pipeline miss showed that the big part was not the pipeline, nor the translation of the program:

| Part of a miss (189 in a walk) | Median | 90th percentile | Worst | In all |
|---|---|---|---|---|
| Translating the program (`fp_build_hlsl`) | 0.25 ms | 0.62 ms | 4.3 ms | 66 ms |
| Finding it in the shader cache (`shader_compile_cached`) | 0.16 ms | 6.3 ms | 54 ms | 520 ms |

No program was compiled: every lookup found its file.

- **The cause is the first read of a new file.** Windows' antivirus scans a file the first time it is opened after it was written.
  - The first read of 300 of these files took 3.2 ms each, and 734 ms in all.
  - A second read took 0.07 ms each, also from a new process: the scan is remembered.
  - Read on 4 threads, the 300 took 147 ms.
- **Setup's shader pass writes some 3,600 programs just before a first start.** So a first walk opened each of them for the first time when a draw needed it:
  - on the render thread, for a pixel program;
  - on the FIFO thread, for a vertex program. A late vertex program delays the flip, so those frames looked as if the game itself were late.

## 3. The fixes (ps3recomp)

- **`PS3_SHADER_CACHE_WARM=<n>`** (default 4; 0 = off):
  - At startup, n threads read every `.dxbc` in the shader cache once, in the background, below normal priority.
  - Here, 3,654 files took 2.9 s, done before the level started loading.
  - On a later start they are scanned already, so the read is short (inferred).
  - Off Windows there is no such scan, and nothing is read ahead.
- **`PSO_THREADS=<n>`** (live; default half the processors less one, at most 7; 0 = the old way):
  - In the draw pre-pass, a miss puts its key in the cache as pending and its description in the frame's batch.
  - At the end of the pre-pass, the workers and the render thread build the batch together. Then the pipelines go into their cache entries and into the draws that asked for them.
  - The frames are the same: every pipeline exists before anything is recorded, as before.
  - The translation and the shader cache lookup stay on the render thread, because they read guest memory and use static buffers.
  - 32 pipelines took 6.5 ms in place of 55.
- **The manifest's files are written on a thread of their own.** These are `pso_blobs/` and `pso_manifest.bin`, written from copies. So no file is touched where a pipeline is built. The manifest has its own lock, separate from the library's. Records still queued when the game closes are written at shutdown.

## 4. Measured with the fixes

The same first-start walk:

| Setting | Slow frames | Over 2× | Worst | Time lost |
|---|---|---|---|---|
| G-Sync / FreeSync, 144 fps, before | 23 | 15 | 38 ms | 207 ms |
| G-Sync / FreeSync, 144 fps, `PSO_THREADS` alone | 25 | 15 | 28 ms | 212 ms |
| G-Sync / FreeSync, 144 fps, both fixes | 8 | 3 | 16 ms | 48 ms |
| Even frames, 60 fps, before | 9 | 0 | 27 ms | 88 ms |
| Even frames, 60 fps, both fixes | 1 | 1 | 32 ms | 18 ms |

- The shader cache lookups for the walk's misses took 22 ms in all, in place of 520.
- With both fixes, a first start at 138 fps has about as many slow frames as a second start had before (6).
- Even frames still held every frame 2 refreshes.

The test window never has the focus, so G-Sync did not engage in these runs. The frame times are the game's; on a G-Sync display each frame shows as it comes (inferred).

## 5. What is left

- **About 8 slow frames in a 34 s first walk at 138 fps**, as in a second start: 10-16 ms frames from the game's own work and from the GPU. These are separate from first-time work.
- **A PC whose graphics driver has never seen the shaders.**
  - There, a pipeline takes 7-20 ms to create (docs/93), not 2. That is a new player's first start.
  - `PSO_THREADS` spreads that over the workers. Building the pipelines before play, from the game's files (docs/49), would remove it.
- **New variants written in play.** `fp_variants.txt` is still appended on the render thread when a program meets a new form (rare after setup's pass).

## 6. How to measure

- `FRAME_LOG=<file>` (ps3recomp): one row per present. Scratch scripts counted the slow frames after the cutscene, with the render-thread columns of each.
- `RF_SLOW_MS=<ms>`: the phases of each render_frame over that long. With `PERF=1`, also the pre-pass split (textures, pipelines, misses). PERF's own timers make each miss slower, so compare like with like.
