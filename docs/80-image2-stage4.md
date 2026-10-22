# 80. Image 2 Stage 4: commands 31-34 (built, measured; step 3 a win, step 4 not)

docs/48 planned Stage 4 as running HighGraphics' ring commands 31-34 natively in image 2, the SPU program that turns HighGraphics' ring into RSX commands. Last night's profile (docs/72) put commands 31-34 at about a quarter of image 2's time, with image 2 at 92% busy.

Three steps were built and measured on the night of 10-21. None of them moves the frame rate, and the third shows why: at today's balance image 2 does not hold the frame.

All runs: the deep Palace save (a copy of `work/tmp/lc_deep/save`), `RSX_VSYNC=0 DES_VSYNC=0 DES_FPS=unlocked VBLANK_MHZ=960000`, `RSX_PRESENT=0` (session locked), knob A/B (`tools/des/knobab.sh`), 3 boots of 3 ABBA pairs each. Your save was identical after every run.

## 1. SPU registers in C locals (lifter `--promote-regs`): slower

The lifted code keeps every SPU register in `ctx->gpr` and loads and stores it around each instruction. The idea was to keep the registers of the 11 region bodies that commands 31-34 run in C locals instead, so the compiler can hold them in host registers.

| Form | Plain | Promoted | Change |
|---|---|---|---|
| Stored back to `ctx->gpr` only at the exits, all 11 regions | 148.9 fps | 121.4 fps | −18.5% |
| Every write also stored at once, all 11 regions | 149.6 fps | 130.6 fps | −12.7% |
| Every write also stored at once, region 0x8CB8 only | 150.3 fps | 147.6 fps | −1.8% |

Every pair went the same way.

- A region body uses up to 84 SPU registers. x86-64 has 16 vector registers, so the compiler spills, and the spill code costs more than the plain body's one load and one store per operand.
- Region 0x8CB8 grew from 110 KB to 175 KB of code.
- The same thing happened with whole-region promotion in October (docs/28, the dead-end list). This rules out the two forms that keep the lift exact.

The code is on the local ps3recomp branch `perf/spu-reg-promote` (off by default, `SPU_REG_PROMOTE` unset). It is a record, not for a PR.

## 2. A profile inside the region bodies (lifter `--block-counts`)

`--block-counts` counts every label of every region body as it runs, weighted by the block's statements. Deep Palace, 30 seconds, share of image 2's lifted statements:

| Block | Region | Share | What it is |
|---|---|---|---|
| 0x6508 | 0x59C0 | 15.2% (overstated, see below) | a state compare-and-emit loop |
| 0x8E18 | 0x8CB8 | 13.3% | 32 passes over two table pairs: compare, copy, set a bit per changed quadword |
| 0x4694 | 0x4498 | 9.1% | the ring fetch's wait spin |
| 0xCCC8, 0xCAE0, 0xCC04 | 0xCA40 | about 12% | |
| 0x7348, 0x7414, 0x74B8 | 0x7140 | about 10% | a 16-entry scan that leaves for a handler when an entry changed |

- The first version weighted a block by all its statements even when it can leave early. Block 0x6508 has 14 statements before its early exit and 103 after it, so its share is overstated.
- The lifter now also counts after each one-line early exit, so each part is weighted by the times it ran. That version is built but has not been run.
- The wait spin alone (0x4694, 9.1%) shows that image 2 spends real time waiting for HighGraphics.

## 3. The 0x8E18 loop in native C: correct, no faster frame

`spu_hook_state_diff` (runtime `spu_diff_hook.c`, lifter `--native-block 0x8E18-0x8F60=spu_hook_state_diff`) runs every pass but the last with plain 16-byte compares and copies, then runs the last pass and the four instructions after the loop as lifted. So every register leaves exactly as the lifted loop leaves it.

- **Check mode** (`SPU_NATIVE_DIFF=2`: every 64th time the loop also runs in the SPU interpreter on a copy, and every register and the whole local store are compared): 32,768 checks at load and 131,072 in the level, 0 mismatches.
- **Speed:** native 148.3 fps, lifted 149.1 fps (+0.56% for the lifted side, p 0.33, 9 pairs). That is inside the noise.

So taking about 13% of image 2's lifted statements away does not change the frame rate. Image 2 has slack at this balance, and running more of commands 31-34 natively would not pay either.

- **Per-thread CPU** in the same A/B's profile windows (9 a side):

  | Thread | Native | Lifted |
  |---|---|---|
  | HighGraphics | 98.1% | 97.9% |
  | FIFO thread | 98.5% | 98.4% |
  | Image 2 | 90.1% | 90.1% |

  Image 2's share did not move, so the time the native loop saved went into polling the ring for the next command.
- **The causal slopes** (`TMP_SLOW_*`, the same night, docs/83) still give image 2 a slope above 1 per ring command. Delay added to every command lands in the frame, because HighGraphics waits on image 2's reply at the frame's end. Work taken out of a loop that image 2 runs while it has slack does not.

The code is on the local ps3recomp branch `perf/spu-native-diff` (two commits: `--block-counts`, then the hook; the native path is on by default there, `SPU_NATIVE_DIFF=0` turns it off). It is parked, not pushed.

## What would make Stage 4 pay later

- Image 2 has to hold the frame again: for example after HighGraphics' production gets faster (the lockline on its ring pointer store, docs/72 and docs/78).
- Check that first with one of the cheap causal knobs: `SPU_NATIVE_DIFF=0` against the default in the same knob A/B. When image 2 holds the frame, that 13% of its statements should show.
- Then the loops in the table above, starting with 0x7348 (a scan where a native block can skip the unchanged entries and hand the lifted loop the state of the first changed one), after a re-run of the corrected profile.

## 4. Measured again without the profiler (2026-10-22, later the same night)

Every A/B above was a `knobab.sh` A/B, whose windows are profile windows. The profile session times every texture, SRV and pipeline lookup on the FIFO thread, which makes the FIFO thread the longest chain while it runs. Without the profiler, HighGraphics and image 2 hold the frame (docs/83 2.1). So an image 2 cut could not show in the numbers above.

The step 3 knob again in plain windows (no profile session; the flip count at each end of a window and `FRAME_LOG`), 3 boots, 9 pairs: native 157.6 fps, lifted 155.4, native +1.4% (p 0.055). That does not count yet, and the null knob in the same boots read +1.8% (p 0.057), so the batch was noisy. The 5-boot rerun: native 161.2 fps, lifted 158.4, **+1.8%**, faster in all 5 boots (15 pairs, p < 0.001); the null knob +0.16%. Regress passes at 30 and 240 fps. So step 3 is a win after all, opened as ps3recomp #100 (the hook) and DemonsSoulsRecomp #119 (`build_spu.py`'s block and the pin). Step 1's verdict (slower) stands: slower is slower in either kind of window.

So Stage 4 is back on: image 2's per-command time lands in the frame in plain play (docs/83 2.1), and the next loops in section 2's table (0x6508, 15.2%; 0xCA40, about 12%; 0x7140, about 10%) are the next steps.

## 5. The corrected profile, with step 3 native (2026-10-22 morning)

`--block-counts` with the early-exit split (each part weighted by the times it ran), on the build of #100 and #119 (0x8E18 native), deep Palace, 30 seconds. Share of image 2's lifted statements:

| Block | Region | Share | What it is |
|---|---|---|---|
| 0x7348, 0x7414 | 0x7140 | 12.5% | the 16-entry scan: the entry compare, then the counters |
| 0x74B8 | 0x7140 | 3.4% | the scan's handler for its fourth compare |
| 0xB6F0 | 0xAB30 | 8.6% | one straight block of 229 statements, 7.0 million runs |
| 0x5120, 0x4694 | 0x4498 | 7.7% | the ring fetch's wait spin |
| 0x6508 | 0x59C0 | 5.0% | 3.5% before its early exit, 1.5% after it |
| 0xCA40 region | | 13.9% | spread over a dozen blocks, the largest 2.1% |

- 0x6508 was overstated before (15.2%): most runs leave at its early exit.
- The scan runs all 16 entries every time it runs (43.1 million entries in 2.69 million loops). About 1.9 entries a loop go to one of its five handlers, 1.65 of them to 0x74B8. The loop body stores nothing.
- So the scan is step 4: a native block that works out the entries ahead and stops at the first one that goes to a handler.

## 6. Step 4: the 16-entry scan in native C: correct, not faster (parked)

`spu_hook_entry_scan` (runtime `spu_scan_hook.c`, lifter `--native-block 0x7348-0x7440=spu_hook_entry_scan`) works out the entries ahead at the loop head on host locals and stops at the first one that goes to a handler. It leaves every register as the lifted loop leaves it at that entry's head (and returns 0, so that entry runs lifted) or at the loop's exit (and returns 1). `SPU_NATIVE_SCAN=0` runs the lifted loop; `=2` checks it against the interpreter, entry by entry, then every register and the whole local store.

- **Check mode:** 131,072 checks (load and level), 0 mismatches, none skipped. Regress passes at 30 and 240 fps.
- **Speed, plain windows, 5 boots, 15 pairs:** native 154.4 fps, lifted 153.9 (+0.35% native, p 0.76). The null knob in the same boots read +0.25%. The windows spread from 143 to 162 fps (the PC was busy: 23% background CPU), so this batch could not have shown 1%.
- **What decides it: image 2's own time** (`profimg 2`, 10 s each, the knob live in one boot, native / lifted / native / lifted, exclusive samples):

  | | Native | Lifted | Native | Lifted |
  |---|---|---|---|---|
  | Region 0x7140 | 1.6% | 4.5% | 1.8% | 4.1% |
  | `scan_run` (the block) | 3.1% | | 3.0% | |
  | Together | 4.7% | 4.5% | 4.8% | 4.1% |

  The native block costs as much as the lifted loop. It works on `u128` locals through the same helpers as the lifted code, so it does the same work per entry; skipping the stores to `ctx->gpr` saved nothing measurable. (I have not read its machine code.) A version on `__m128i` with SSE compares could be about three times faster. But the loop is only about 3.4% of image 2's time, so even that would save about 2% of image 2, roughly +1% fps. That is under the bar, so it is not built.

- **Where image 2's time goes** (the same samples, by category):

  | | Share |
  |---|---|
  | Lifted image 2 code (all regions) | 26-29% |
  | Native blocks and hooks (ring fetch and post, put wait, 0x8E18, the scan) | 8-12% |
  | Runtime: the native ring loop (13%), DMA (`mfc_fast_xfer`, 10%), channel writes (8%), GETLLAR, atomics | 45-48% |
  | Windows and the C runtime, mostly the lock-line wait | 12-16% |

  The block-count profile counts lifted statements only, so it overstated every loop's share of the time: the scan is 12.5% of the statements but about 3.4% of the time. Step 3 paid because its loop was bigger in time and its native form is far cheaper (plain 16-byte compares and copies, no shufbs).

So Stage 4 has a low ceiling: all of image 2's lifted code is about a quarter of its time. The bigger part is the runtime around it: the native ring loop, the DMA path and the channel writes, about 45%. Those have been cut many times already (docs/28, docs/56). The next lever on image 2 is there, or on HighGraphics' side of the ring (docs/72, docs/78). The step 4 code stays on the local ps3recomp branch `perf/spu-native-scan` (one commit on fork master), not pushed.
