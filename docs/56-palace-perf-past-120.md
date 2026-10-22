# 56. Palace performance past ~120 fps (2026-10-19)

Status: stage 2 done (10-19, 00:11-07:10). Every run is in section 4; the open choices are in section 5. Numbers
without a run tag come from docs/28, docs/48 and the 10-17 sessions.

**Since then (2026-10-22).** The branches this doc kept locally have all merged: in ps3recomp the memo fix (#72), the
bi-switch (#73), the PUT trim (#74), the HLE and notify trims (#75, #76), the lifter gaps (#77), Stage 0 (#78) and
FIFO_PUT_WAKE (#79), then docs/48 Stages 2 and 3 (#87, #91); in the port the PGO tool (#81), the bi-switch lift (#83),
the cutscene blend (#84), the A/B tools (#85), the M+KB layout (#86), regress by frames (#72) and profsym's one table
(#69). The doc is the record of that night, so the states below are as they were then.

**Correction (10-19, 01:45).** Up to `aa1c` the runs used the pinned regression save, and stage 1 called it docs/28's
Palace spot. It is not: it loads the Tutorial (`m08`, 794 draws a frame, ~320 fps uncapped). docs/28's ~120 fps, its
A/Bs and its causal slopes come from the deep Palace save (`m02`, Palace 1-1, 2,240 draws). From `prof2` on, the
lane runs there (`$L/saves/deep`, the save behind `work/tmp/lc_deep`). `aa1c` and `logwrap` stay at `m08`, so the
logwrap A/B has its A/A at the same spot.

**Save note (10-19, 04:10).** Since the dev-cheats run at about 03:50, the save folder holds the pinned regression
save, not the live one: levelcap's restore trap does not run on SIGTERM. The live save was in a backup in the
lane's scratch folder and was put back by hand later (port #75's save-folder lock came out of this). The per-boot
save checks of later runs therefore compare that pinned save, so they show only that a run put back what it found. The
checks up to cand3 (02:50) were of the live save. None of this lane's scripts ends levelcap with a signal:
`perfprof.sh` and `r3probe_run.sh` end its `sleep`, and `perfab.sh` and `perfknob.sh` let it run to the end of its
command file.

**Notation.** This doc is the record of one night's measuring lane. `$S` is that session's scratch folder and `$L` =
`$S/lanes/perf`. The lane's scripts lived in `$L`, were tested against mock runs (`$L/test_mock/`), and are not in the
repo; the repo's A/B tools came out of them (docs/63).

## Short answer

**10-19 afternoon (section 6):**

- **The stack: +6.0% profile fps and +7.2% gap fps** over the base (stackab1, faster in 6 of 6 rounds, p<0.001), past
  both cross-build bars. It is CR fields, the FIFO cuts and the bi-switch. Neither CR fields nor the cuts move the
  frame alone; together they do: CR fields take HighGraphics off the limit, and then the FIFO thread holds the frame.
  With the batch in as well, every cut off costs 2.5% (stackk4, p 0.012; stackk1's -6.7% was hit by the memo bug of 77-79). Regress passed at 30 and 240 fps.
- **Merged at 18:47:** CR fields (ps3recomp #69, with the port's five CR sites in port #82; every title
  re-lifts), the FIFO cuts (ps3recomp #70) and the batch (ps3recomp #71). Before the batch the bi-switch
  added nothing measurable (-0.9% with it off, p 0.44).
- **The FIFO batch** (texture checksums on worker threads, each draw's pipeline in its record): +3.9% inside the stack (both parts off: -3.7%, p<0.001; stackk2), regress passed; ps3recomp #71, merged at 18:47.
- **PGO:** +6.3% profile fps and +7.9% gap fps on the base (pgo1). It stays a developer and measurement tool
  (Decision 2, settled): nothing for players. The tool is port draft PR #81 (`tools/pgo`, docs/58).
- **The black test-run window** was a bug in the FIFO cuts' memo, reached only by switching `RSX_VP_MEMO` to 1 and
  back live, as the in-boot A/Bs did. Fixed in ps3recomp #72 (e1119c8). It made stackk1's cut line unreliable;
  measured again with the fix, every cut off costs 2.5% in the stack with the batch (stackk4, p 0.012).
- **All four PRs together** against the base, cross-build: +4.8% profile fps (p 0.001) and +3.0% gap fps (p 0.014) with the bi-switch off, faster in 6 of 6 rounds (stackab2); the gap number is under its 4.5% bar. With the batch in, image 2 holds the frame (92% busy, the FIFO thread 82%).
- **The bi-switch pays now:** with the batch in, image 2 holds the frame, and turning the switch off costs 4.53%
  (stackk5, p 0.005). PRs: ps3recomp #73 and port #83 (Decision 3, settled).
- **FIFO_PUT_WAKE** (the FIFO thread's stale 1 ms sleep, docs/67): no gain while image 2 holds the frame (stackk6,
  -0.34% with it off, p 0.83); kept on a local branch.
- **The merged masters** re-lifted and built: `work/tmp/merged2` (release) and `work/tmp/merged2-dev`; regress passed at 30 and 240 fps. The arrival cutscene's hitch at 13.3 s is confirmed as an Euler flip, and docs/62's blend removes it in the log (jumps 4 to 0; local branch `fix/cut-euler-blend`).
- **The eight-way VS memo:** no gain (stackk3); it stays local.
- **Next:** image 2 holds the frame now, so image 2's native ring loops first (docs/48 Stage 2; docs/60 maps it to
  edits; Stage 0 is on `perf/spu-ring-hist`), and HighGraphics' wait on image 2's reply (docs/67); then the render
  thread (docs/68), once the FIFO thread holds the frame again. Section 7 says where each starts.

**10-19 night (sections 1-5):**

- **No change cleared the frame-rate bar that night, so there were no perf PRs.** Two changes save real CPU and are
  kept local for the maintainer's call (Decisions 3 and 6). The profsym fix (tooling) is pushed for review.
- **The spot and the noise:** docs/28's ~120 fps comes from the deep Palace save (`m02`, 2,254 draws a frame), not
  the pinned regression save (the Tutorial). There the base reads about 116-123 fps in profile windows and 122-129 in
  the gaps, depending on the hour and whether the screen is on. The same exe on both sides (aa2) puts the cross-build
  bar at 2.8% (profile fps) and 4.5% (gap fps); in-boot knob A/Bs (`perfknob.sh`) resolve about 1-1.5%.
- **What sets the frame:** HighGraphics (96% of a core), the FIFO thread (92%) and image 2 (89%) are all close to
  saturated (prof2). The causal slopes (slope2) are 0.69-0.85 a millisecond for HighGraphics and the FIFO thread and
  1.51 for image 2. A cut on one thread pays only until the next one saturates.
- **CR fields (#12, ps3recomp `perf/ppu-cr-fields`):** the condition register as eight bytes takes the packed word's
  load chain out of every lifted function (in 12 hot functions, 475 loads of it become 22). HighGraphics does about 19%
  less work a frame and now waits 30% of the time instead of 13%, and the game uses 0.12 cores less. The frame rate does
  not move (+0.5%, p 0.55). Checked by the tests, the conformance suite, two whole-EBOOT lifts, libsre and regress at
  30 and 240 fps. It needs every title to re-lift.
- **The bi-switch (docs/48 Stage 1):** takes ~3 points of a core off image 2. Alone it is inside the noise (-1.26%
  with it off, p 0.11). With CR fields under it, turning it off costs 2.08% in-boot (p 0.041), so the pair works. But
  the pair against the base across builds reads +0.3% / +1.3% (profile / gap), inside the bars. The FIFO thread
  (93%) is what holds the frame after both.
- **Dropped:** `tex_csum` without the divide and the MFC context slot (nowhere near the bar); the order file and
  docs/53 R3 (their expected gains sat below aa2's 2.8%; rerun commands are in `runs/*/SKIPPED.txt`). The log-counter
  fix (`fix/log-count-wrap`) costs nothing measurable and its logs match.
- **Image 10's R3 loop (the soak):** not reproduced in four short boots. Statically it is an assert loop at `stop 0`
  on an over-released Havok semaphore (section 4); a soak-only lead.
- **Tooling found on the way:** `profsym.py` counted every exe sample twice in `profrip` dumps (fixed on
  `tools/profsym-one-table`, pushed); regress.sh's `shot()` sorts frame names as text, so 240 fps runs fail past
  frame 999 (known, port PR #72); two CMake globs that miss new files or link stray ones; the PPU lifter gets slower,
  not faster, with a short `--functions` list.
- **Next:** the FIFO thread (render_frame, the VCRUNTIME copy and fill bodies, `d3d12_draw_indexed`,
  `upload_strip_vp_indexed`, `rt_writeback_store`) is now the busiest thread, and neither change touches it. Trimming it
  is what would let CR fields and the bi-switch show in frames. PGO (Decision 2) speeds up all three threads at once
  (+6-8%, section 6), as a developer tool only.
- **Tools:** `perfab.sh` (cross-build, ABBA rounds), `perfknob.sh` (live knobs, ABBAAB windows inside each boot),
  `perfprof.sh` (one boot: busy shares, waits, sync pairs, frame chain, per-thread profiles), `perfslope.sh` (causal
  slopes), `abthreads.py` (per-thread CPU from an A/B's profile sessions), `perfreg2.sh` (regress with the frame-sort
  fix). All run through the slot lock at `DES_FPS=60 VBLANK_MHZ=960000` from a frozen save.

## 1. A/B recipe

**The spot.** Boletarian Palace 1-1 (`m02`), from the deep Palace save: docs/28's A/B spot ("All numbers are from
`DES_FPS=60` at the deep Palace save"). The lane keeps frozen copies in `$L/saves/<name>/savedata`; `PERF_SAVE`
picks one (`deep`, `palace`, `live`, `regress`) and `PERF_MAP` names the map whose script archive starts the settle
clock. `deep` is the save of `work/tmp/lc_deep` (same md5), which loads `m02`. The pinned regression save
(`work/tmp/regress/save`) loads the Tutorial (`m08`); the first runs of the night used it by mistake (see the
correction at the top).
- levelcap loads the save through the title (`DES_AUTO_CONTINUE=1`). `LC_NOPRESS=1` means nothing is pressed after that,
  so the player stands still at the spawn point.
- If a cutscene plays, the pad file skips it (START, then CROSS). A boot whose windows end inside a cutscene is marked
  invalid.
- The windows start `PERF_SETTLE` = 40 s after the `level loading (.../m02.luabnd...)` line.

**Settings, both sides:** `DES_FPS=60 VBLANK_MHZ=960000` (`PERF_VBLANK`), `PS3_PRIORITY=normal PS3_AFFINITY=all`.
`RSX_PRESENT` is set once per batch from the lock state (0 while the session is locked).
- The vblank is the frame cap. At 120000 the gains near 117 fps were squashed (docs/28, "The A/Bs were capped at 120").
  The plan said 240000, but with `RSX_PRESENT=0` at `m08` every window sat at 240.0 fps (prof1, aa1b), so it is 960000
  now: aa1c at `m08` ran 302-340 fps with nothing capping it.

**Builds.** Both sides use the same route (`perfbuild.sh`): the overnight `build_rt_wt.bat` and `build_wt.bat` (no
`/Zi`), the master lift `work/recomp_master`, and the same worktree and build directory. Configure must report "libsre
LLE available" and "real taskset policy WIRED", or the step refuses. The runtime is the lane's own ps3recomp worktree at
the base commit, because every ps3recomp candidate needs its A side built the same way as its B side.

**Quiet PC.** Before the batch, and again before each boot, CPU use is sampled over 5 s (3 s before a boot) with no game
running (`quiet.ps1`): the total, and the share of processes outside the overnight batch. The batch's own processes are
the agents and what they run (claude, VS Code, which hosts them, the shells, python, git, the index servers); node, vite
and tsx (another project), the browser and the OS count as outside. Quiet means a total of at most
`PL_QUIET_MAX` = 30% and an outside share of at most `PL_QUIET_OUT` = 3%. Otherwise the script waits 30 s and tries
again, and stops after 15 min (exit 4). Each boot's reading goes into `runs.txt`.
- The first version wanted a total of at most 5%. On 10-19 aa1 waited its full 15 min under it (both slots held, exit 4):
  with a dozen agents running, the machine sat at 13-26% total, nearly all of it the batch itself, while the outside
  processes (node, firefox) stayed under 1%. A build or a game is far above 30%, and the slot lock keeps those away
  anyway.
- So the noise bar comes from an A/A under the same background. aa1b ran at the 240 Hz cap (every boot ~240 fps, "not
  enough valid boots"), so it is discarded; aa1c, below, is the A/A.

**Order and count.** Rounds alternate A B, then B A (ABBA...), so drift cancels. The default is 4 rounds (8 boots, about
25 min). Each boot has two 8 s profile windows, each followed by 10 s with nothing armed.

**Statistics** (`abstat.py`). The boot (the mean of its windows) is the unit, because windows in one boot are not
independent.
- Per side: windows and boot means, median, min-max, sd.
- Per round: the B-A delta.
- A paired t over the rounds, and a Welch t over the boot means.
- A window under 0.6 x its side's median (a cutscene lock, a stall) is dropped and listed.
- **A gain counts** when the median delta is at least `PERF_NOISE_PCT`, the paired p is under 0.05, and B is ahead in all
  rounds but at most one (3 rounds minimum).
- Boot-to-boot spread was ~3% in 10-17's exeab batch (boot means 117.6-121.7), so a cross-build A/B needs the full 4
  rounds.

**First batch: A/A.** The same exe on both sides, 3 rounds. Set `PERF_NOISE_PCT` to the larger of 0.8% and twice the sd
of the A/A round deltas. If time allows, also run base against the coordinator's rebuilt `master_nozi`: two builds of the
same source from different directories. That shows whether layout alone moves the frame rate.

**aa1c (10-19 01:26-01:37, `m08`, 960 Hz, `exe/base` on both sides, 3 rounds):**

| | boots (profile-window fps) | median | spread |
|---|---|---|---|
| A | 317.9, 324.6, 315.5 | 317.9 | 2.8% |
| B | 328.2, 338.9, 304.1 | 328.2 | 10.6% |

- Round deltas B-A: +3.22%, +4.41%, -3.61%; paired p 0.64. Gap fps: -1.26%, +4.21%, -0.65%.
- Bars: 8.7% for profile-window fps and 6.0% for gap fps (twice the sd of the round deltas), against 0.8% assumed.
- 10-17's batch had boot means 117.6-121.7 at the Palace. Tonight's 11% spread is at `m08` and 320 fps, with a dozen
  agents keeping 14-20% of the 8-core / 16-thread CPU busy. Windows in one boot agree within 1-2%, so the spread is
  set at boot, most likely by where the hot threads land (HighGraphics and image 2 on one core's SMT pair cost 8-10%
  in docs/28). That is a
  property of this PC, so nothing is tuned for it.
- The Palace bar is measured separately (aa2, below), because the balance there is different.

**aa2 (10-19 04:26-04:36, the deep Palace save, `m02`, 960 Hz, `exe/base` on both sides, 3 rounds, 9.5 min):**

| | boots (profile-window fps) | median | spread |
|---|---|---|---|
| A | 118.8, 118.5, 118.9 | 118.75 | 0.4% |
| B | 115.9, 118.9, 118.0 | 118.0 | 2.5% |

- Round deltas B-A: -2.40%, +0.38%, -0.76%; paired p 0.37. Gap fps (123.6-126.7): -0.86%, +3.64%, +1.40%.
- Bars: **2.8% for profile-window fps and 4.5% for gap fps** (twice the sd of the round deltas), about a third of
  `m08`'s. Background load was 8-12% (14-20% at `m08`). Most of the spread is one window: B1's second window read
  113.3 against 117.8-120.1 for the other eleven.
- 2,254 draws a frame, as in docs/28. Gap fps 123.7-125.4 boot medians: the ~120 fps of docs/28 holds.
- What it changed: ordb2 (27, expected 0 to +3%) and r3b (28, a guard) would most likely read inside a 2.8% bar on
  tonight's PC, so both were dropped at 04:40 (`runs/<tag>/SKIPPED.txt`; their exes stay for a quiet-PC run), and the
  CR A/B (39) went from 4 to 6 rounds with the time saved.
- A knob A/B carries its own reference: a knob line that sets a variable nothing reads (`PERF_NULL=1`).

**Live-knob A/B** (`perfknob.sh`). Use it for any change that has a live knob. Each knob gets ABBAAB windows (6 s) inside
every boot, cleared or set through the console's `setenv`. Both sides share the boot, so the ~3% boot spread drops out.
This is the development loop. The PR number is still a cross-build `perfab.sh` batch when the default build differs from
"knob off" in more than the knob.
- Its own noise reference: a knob line that sets a variable nothing reads (`PERF_NULL=1`). Its B-A is an in-boot
  A/A under the same windows and order. A knob's result counts when B-A is past twice the null line's |B-A| (and at
  least 0.8%), with the paired p under 0.05, on the side that favours the change.
- `knobcpu.py` adds what the frame rate cannot show: each window's CPU per thread (HighGraphics, image 2, the FIFO
  thread, main, total cores), A against B per knob line. A change can save CPU on a thread that does not set the
  frame (decision 3).

**Save safety (all scripts).** The live save is copied to `runs/<tag>/usersave_bak` with an md5 snapshot. levelcap swaps
the frozen save in and puts the live one back. The md5s are compared after every boot, and the batch stops if they differ.
No script deletes a backup, `rm`s the save path or `cd`s into it.

**Commands** (always through the slot lock, both slots):

```
bash $S/slots/grab2.sh perf:aa1 bash $L/perfab.sh aa1 $L/exe/base/des_boot.exe $L/exe/base/des_boot.exe 3
bash $S/slots/grab2.sh perf:<tag> bash $L/perfab.sh <tag> <exeA> <exeB> 4 [ENV...]
bash $S/slots/grab2.sh perf:<tag> bash $L/perfknob.sh <tag> <exe> 2 <knobs-file> [ENV...]
```

Outputs go to `$L/runs/<tag>/`: `abstat.txt` / `knobstat.txt` (the verdict), `batch.txt` (exes and md5s, knobs),
`runs.txt` (validity and background CPU per boot), and the `lc_<boot>/` run folders.

## 2. Profiling plan

**Per thread, one boot** (`perfprof.sh <tag> <exe>`, about 6 min, all in `$L/runs/<tag>/`, digest in `summary.txt`):

| Step | What it gives |
|---|---|
| `cpu 3` | every host thread: tid, guest name, cpu%, app/os/dll split, hot function |
| W1, 8 s profile | reference fps (prof and gap) |
| W2 + `tslack.exe <pid> 8 50` | busy share per 50 ms for every thread, from `QueryThreadCycleTime` alone (nothing suspended): slack = 100 - busy, with the spread over the 50 ms slices |
| G, 10 s idle | unperturbed gap fps |
| `waits 4 40` | time per guest thread in each syscall and HLE call |
| `synctop 24` | the busiest (cond/mutex, op, thread, caller) tuples: who waits on whom |
| `trace 2` + `chain.py` | per frame: HighGraphics' work, its usleep polls and cond waits, image 2's naps, FIFO lag, render_frame, the label wait |
| `profrip` HighGraphics, image 2, FIFO thread (6 s), main (4 s); `prof` vpj_worker, rt_writeback (4 s) | hottest functions per thread (`profsym.py`) and every sampled address |
| `SPU_RATE=2`, 7 s | image 2's steps, naps, polling (it turns the lean atomic path off, so it runs last) |
| W3 | drift check against W1 |
| `mkorder.py` | `order.txt`: every sampled exe function, hottest first, for the order-file candidate |

**Causal slopes** (`perfslope.sh <tag> <causal exe> 2`). These need a scratch runtime: local branch
`scratch/perf-causal` in `$L/wt-ps3r` (also `$L/causal.patch`; never pushed, never merged). Each knob below is set
(B) and cleared (A) in ABBAAB windows inside each boot. With `TMP_SLOW_RATE=1` the runtime counts how often each site
runs, and `knobstat.py` turns that into the time added per frame and the slope.

| Knob (`slope_knobs.txt`) | Thread, where |
|---|---|
| `TMP_SLOW_FLIP_US=500` | HighGraphics, once per flip request |
| `TMP_SLOW_PRESENT_US=500` | FIFO thread, once per presented frame |
| `TMP_SLOW_DRAW_NS=200` | FIFO thread, per draw (~2,240 a frame) |
| `TMP_SLOW_RING_NS=25` | image 2, per ring command (~42,000 a frame) |
| `TMP_SLOW_COH_NS=50` | coherent PPU stores (mostly HighGraphics' ring writes) |
| `TMP_SLOW_SC=1,108,*,300` | main thread, per `sys_cond_signal` (use 109 if `synctop` shows signal_all) |

How to read a slope (frame-time change / time added): 1 means every microsecond added there lands in the frame; 0 means
that thread has slack there. Adding time can only make a thread more critical, so the slope is an upper bound on what
removing time there gives. With two chains near balance, removal can give much less: docs/28's render_frame cuts gave
nothing at ~100 fps because the time came back in HighGraphics' window.

**Reference points.** 10-17 at ~120 fps: FIFO 0.87, HighGraphics 0.77 (1 ms a frame). At ~106 fps, image 2 per command
was 0.71 and HighGraphics per coherent store 0.52. Re-measure after every gain: three threads are within a few percent of
saturation.

## 3. Candidates, ranked by expected gain for the cost

Gains are estimates until measured. "Dead-end check" compares each one against docs/28's no-gain lists.

| # | Candidate | Thread | Expected | Cost | Dead-end check |
|---|---|---|---|---|---|
| 1 | **Link order file**: the sampled hot functions first in `.text` (lld-link `/order`, from `mkorder.py`). `.text` is ~290 MB and HighGraphics' time is spread over thousands of lifted functions, none above a few percent: many scattered code pages. | HighGraphics, main, all lifted code | 0 to +3% | small: relink twice (ordered and unordered, same objects; `perfbuild.sh order`), one perfab batch | dropped 10-19 after aa2: 0 to +3% expected against a 2.8% bar. The relinks (`exe/ord0b`, `exe/ord1b`, from prof2's 822 functions) are ready; the rerun command is in `runs/ordb2/SKIPPED.txt`. It is not docs/53's "hot /O2, cold /O1": only the layout changes. Static functions cannot be ordered (no global name). |
| 2 | **docs/48 Stage 1, `--bi-switch`**: image 2's jump-table `bi` as a C switch (indirect dispatch ~6.5% of image 2). Live knob `SPU_BI_SWITCH`. | image 2 | 0 to +2% now; more if image 2 sets the frame | small to medium: lifter, SPU re-lift (~17 s), runtime build; perfknob A/B | tried 10-19 (cand3, biswf, crbk2, crfb): ~3 points of a core off image 2. Alone, inside the noise (-1.26% with it off, p 0.11); inside the CR build, -2.08% with it off (p 0.041); the pair against the base inside the cross-build bars. With the batch in (10-19 evening), -4.53% with it off (stackk5, p 0.005): ps3recomp #73 and port #83. The image 2 dead ends are register promotion, `restrict` LS base and the `mfc_for` inline. |
| 3 | **PGO** (clang `-fprofile-instr-generate`, then `-fprofile-instr-use`) for the runtime and the lift. LLVM 22 here already has `clang_rt.profile` and `llvm-profdata`: no install. | all | +3 to +10% (typical for branchy C; unmeasured here) | high: an instrumented full build, a profile run, a PGO full build (both slots, ~20 min each), one perfab batch. Flags go in through a lane copy of the compiler launcher and `_LINK_`, with no CMake edit. The profile must survive levelcap killing the game: continuous mode (`%c`, `-mllvm -runtime-counter-relocation`) or a scratch dump hook. | not tried. Shipping is a decision (below). |
| 4 | **FIFO thread prepass**: when a draw's sampled units match the previous draw's and nothing was uploaded or rendered in between, take the previous draw's resolved views instead of resolving each unit again. render_frame is 2.6 ms; per frame the draw-prepass is 1.36 ms and the vp-pass 0.66 ms. The unit loop takes 1.16 ms a frame in a profile session, 0.73 ms of it in 5,762 texture lookups, and the session's own timing inflates both. The views themselves are already only re-created when they change (`RSX_SRV_MEMO`), so what is left is the lookups. | FIFO | 0 to +1.5% | medium, with a correctness risk: a render-to-texture between two draws must break the reuse | related cuts gave nothing at ~100 fps, while the FIFO thread had slack (`tex_csum` trims, checksums on helper threads). Its slope is 0.87 now, so a re-test is fair, at 240 Hz only. "Only the sampled units" and the view memo are already in. Gate: `PRESENT_US` / `DRAW_NS` slopes and the FIFO profile. |
| 5 | **Re-measure two "inside the spread" items** with the calibrated bar: the MFC engine slot in the SPU context (+0.6-1.2% at 240 Hz, one round the other way; no branch left, ~30 lines to rewrite) and `tex_csum` without the divide (+0.6% at ~100 fps). | image 2, FIFO | +0.5 to +1% each | small each, best as live knobs (perfknob) | tried 10-19 (cand3, in-boot knobs): turning each off moved the frame rate +0.01% (`tex_csum`'s divide back) and -0.37% (the MFC context slot off), nowhere near the bar; dropped, branches local. |
| 6 | **docs/48 Stages 2-3**: native end-of-command step (`SPU_NATIVE_POST`), then a native loop (`SPU_NATIVE_LOOP`). | image 2 | +1-2% at today's balance; +8-14% where image 2 sets the frame | medium to high, plus the record/replay harness | not tried. The no-gain "native block extended over the state PUT" was a different block, and the PUT hook itself paid +5%. Gate: the RING_NS slope. |
| 7 | **R3 (docs/53)**: 14 against 53 chunks. A guard, not a speed-up: it decides `--chunk-lines 150000`. | lifted code | within a few % (fewer callees visible to the inliner, a different layout) | one perfab batch, no build (`$S/lanes/f3-buildtime/exe_nozi14`, `exe_bt3`; same EBOOT and hooks, LLE, older master: compare the two with each other only) | dropped 10-19 after aa2: a guard that could only rule out a loss above 2.8%. The rerun command is in `runs/r3b/SKIPPED.txt`. |
| 8 | **CPU baseline above SSE4.1** for the lift (AVX2 and BMI2: `rorx`/`andn`/`shlx` for `rlwinm`-heavy code) | HighGraphics, main | 0 to +2% (SSE4.1 gave +1.2-1.9%) | small (one flag) | not tried. It drops pre-2013 CPUs: a decision. |
| 9 | **Render-target write-back**: skip targets the guest never reads (the FIFO thread waits 0.41 ms a frame in wait+store; the `rt_writeback_store` thread is at 27% of a core) | FIFO | 0 to +2% | medium to high, plus a correctness risk (guest reads of target memory) | not tried. Last on the list. |

**Built for stage 2 (local branches, not pushed until they win):** #2 is ps3recomp `perf/spu-bi-switch` (8a107ad
runtime, ed39c83 lifter) plus port `perf/spu-bi-switch` (022548a, `build_spu.py`'s `BI_SWITCH` table). #5 is
`perf/tex-csum-nodiv` (0d2bd3a: a running remainder in place of `(k*28) % step`; `TEX_CSUM_DIV=1` brings the divide
back) and `perf/spu-mfc-ctx` (7fcf08c: `mfc_for` reads the slot kept in the context before the thread-local;
`SPU_MFC_CTX=0` skips it). All three are measured in one exe, `exe/cand3`, built from the local merge
`scratch/perf-cand3` (26ba05e, never pushed) with image 2 lifted with `--bi-switch`. Each knob line turns one off
with the other two on, so each result is that candidate's own share.

**What prof2 changes in this ranking** (section 4 has the numbers):
- The frame's chain at the Palace is HighGraphics and image 2. HighGraphics works 7.7 ms of an 8.9 ms frame and waits
  1.1 ms for image 2's replies; it waits on the FIFO thread's label for 0.009 ms (median). The FIFO thread trails it by
  4.6 ms and keeps up (92% busy). So, at this balance, cuts on the FIFO thread (#4, #9, and the copies below) would
  save CPU without moving the frame. `slope2` (31) checked this with the `PRESENT_US` and `DRAW_NS` knobs: slowing
  the FIFO thread costs 0.75-0.85 ms a ms, so it is near saturation too, and the first ~1 ms of extra FIFO work
  already moves the frame. That does not make a FIFO cut pay: with HighGraphics at 95-96%, a faster FIFO thread still
  waits on it (section 4, slope2).
- Image 2 (#2, #5's MFC slot, #6): a faster command shortens HighGraphics' 1.1 ms wait, so +1-2% is the likely ceiling
  for #2 alone, as estimated.
- HighGraphics itself (#1, #3, #8, and below) is where a larger gain has to come from. Its time is spread: the top 40
  functions hold 71% of its samples, and the biggest lifted ones are the ring command builders (`func_00103878`,
  `func_000B90F0`, `func_00CFEC70`, `func_00CF73F8`, `func_00CFA090`, `func_000F88E0`, `func_000F1FF0`, 20% between
  them; all seven call `func_00B5D8D8`, a `sys_timer_usleep` wrapper, while the ring is full or a reply is due). The coherent store path is 10.7%, which
  docs/28 already worked through.

**New candidates from prof2 (10 and 11 not built tonight; 12 prototyped):**

| # | Candidate | Thread | Expected | Cost | Dead-end check |
|---|---|---|---|---|---|
| 10 | **The guest's `memcpy` as a host routine**: `func_01666594` (8-byte loop, byte tail, backward copy when the ranges overlap) is 1.9% of HighGraphics. A native hook would copy with `memmove`. | HighGraphics | up to +1.5% | small to medium: an insn hook or a lifted-function override, plus the coherence rule: the lifted stores notify SPU reservations line by line, so a bulk copy must do the same for every line it touches | not tried |
| 11 | **Vertex-constant banks: only the registers the program reads.** `vp_record_cb` copies all 468 registers (7.5 KB) into the write-combined upload heap for every draw whose constants changed (`VP_CB_REUSE` already skips the unchanged ones). The VP decompiler could record which registers a program reads; without relative addressing only those need copying. A reused bank must then cover the next draw's registers too. | FIFO | CPU only at this balance (the copy routines are 14.9% of the FIFO thread, most of it this copy) | medium, with a correctness risk (a register the analysis misses reads stale data) | not tried |
| 12 | **The condition register as eight field bytes** (lifter and runtime header). Every lifted compare rewrites the packed 32-bit `ctx->cr`: load, mask, or, store. So all of a function's compares form one dependency chain through memory, each link a store-to-load forward plus two ALU operations, whatever fields they touch. In prof2, 8.5% of HighGraphics' samples (21% of its exe samples) land on the instruction right after that load (`cr_after.py`: a `mov r32, [reg+0x400]`); `func_00103878`'s machine code in `exe/base` has 46 such loads and 45 stores (the CR test's 7 functions around 0xB90F0 and 0x103878: 214 and 213 in 26,863 instructions, `cr_codegen.py`; an earlier count of 64 and 63 ran past the function's end), while its branches read the field from a register (one `testb` on memory). RPCS3 keeps CR as 32 separate bytes (`cr_bits`). With `uint8_t crf[8]` (the same 8 bytes; `lr` and everything after keep their offsets) a compare is one byte store, a branch tests one byte, a CR logical op is one byte read-modify-write, and mfcr/mtcr pack and unpack. Prototype: ps3recomp `perf/ppu-cr-fields` (32e8bf4, local, in `$L/wt-cr`). | HighGraphics, every lifted PPU thread | +2 to +6% if the stall share is the chain (HighGraphics' slope 0.77) | high: a full re-lift (EBOOT and libsre), a full build (~25 min, both slots), a cross-build A/B and a regress run. It touches every title: host code that uses `ctx->cr` must move to `ppu_cr_get`/`ppu_cr_set`/`ppu_cr_pack`/`ppu_cr_unpack` (in both `ppu_context.h` and the generated `ppu_recomp.h`); renaming the field turns each such site into a compile error rather than a silent change. In this port: `des_motion_blur` and `warp_chr` (`des_options.cpp`), `des_menu_tick` (`des_menu_sfx.cpp`), and the dev-cheats branch's `set_cr7` and H11 (`des_dev.cpp`) | tried 10-19 (section 4, crab4 and crfb): HighGraphics ~19% less work and 0.12 cores less, frame rate +0.5% (inside the bar); with the bi-switch +0.3% / +1.3% (profile / gap, inside the bars). Not on docs/28's lists |

**Not worth retrying at this balance** (docs/28):
- HighGraphics' poll and wake-up: `USLEEP_SPIN` (three times), `pause` instead of `SwitchToThread`, a shorter poll (-1.3%),
  `FIFO_SPIN_US`. Wake latency is not on the path. An event-driven reply wake falls in the same class.
- Thread placement: SMT siblings were worse, and two pinned cores were worse than unpinned. It would also be tuning for
  one PC.
- Lifted-code shape: register promotion in SPU regions (`u128`, `__m128i`, segment caching), `__restrict` ctx (-1.7%),
  `restrict` LS base, `/GS-`, `/Zc:tlsGuards-`, the strip walk split further, corner-slot reuse.
- Coherence and atomics: one runtime call per coherent store, waiting on an empty ring natively, per-line PUT locking,
  the notify filter and `SPU_COH_TAGS`, lazy notify (dropped), PUTLLC fast-fail without the lock, a no-op
  PUTLLC without the lock (-0.45 fps).
- Froze the game: skipping the commit hooks on a no-op PUTLLC, and a lock-free `vm_write` fast path.
- Others: more than one `vpj_worker` (worse), `FLIP_AT_VBLANK`, EcoQoS and timer resolution, a WaitOnAddress lwmutex for
  the cache thread.

## 4. Stage 2 run order

The base build (`perfbuild.sh tslack`, `rt d5dec60`, `port base 12`) is the coordinator's queued job `main:nozi2`, and
`exe/base` is shared with R1, so it is never renamed, moved or rebuilt. `$L/q_launch.sh` queued the rest on the slot lock
on 10-19 at 00:11, one waiter per step so each keeps its place. Logs and exit codes are in
`$L/logs/q/<nn>_<label>.log` and `.rc`. A step whose exe or order file is missing exits at once. `port <name>` and
`order` refuse until `exe/base` exists, so no step can compile the lift chunks under one slot.

| # | Step | Slots | What |
|---|---|---|---|
| 01 | `perfab.sh aa1 base base 3` | 2 | A/A: the noise bar, about 18 min. Exit 4 under the old 5% quiet test; rerun as `aa1b` |
| 02 | `perfprof.sh prof1 base` | 2 | the per-thread profile, `order.txt`, then the R3 phase (image 10), about 8 min |
| 03, 04 | `perfbuild.sh order ord1 runs/prof1/order.txt`, `order ord0 empty_order.txt` | 1 | relinks from the base objects, before any runtime change |
| 05, 06 | `perfbuild.sh rt 3380439`, `port logwrap` | 1 | the coordinator's log-counter fix (`fix/log-count-wrap`); the PPU lift chunks do not include the changed headers |
| 07 | `perfab.sh logwrap base logwrap 3` | 2 | its A/B, and its test run (boots, plays, the first-N log lines: `logcmp.py`). Inside the noise, logs match (below) |
| 08, 09 | `perfbuild.sh rt scratch/perf-causal`, `port causal` | 1 | the causal build |
| 10 | `perfslope.sh slope1 causal 2` | 2 | causal slopes, about 18 min |
| 11 | `perfab.sh ord ord0 ord1 4` | 2 | the order file. Exits at once: prof1's order file had only 61 functions (`exe/ord1` is now `exe/ord1_61fn`); redone as 20-24 |
| 12 | `perfab.sh r3 exe_nozi14 exe_bt3 4` | 2 | docs/53 R3. Exits at once (`runs/r3/MOVED.txt`): moved to the end as 28 |
| 13 | `perfbuild.sh rt d5dec60` | 1 | the runtime back at base, for later builds that must match `exe/base` |
| 14 | `r3probe.sh r3probe1 base 2` | 2 | since 02:05 this place runs the candidate A/B (`cand3_ab2.sh`, `runs/cand3`, 2 boots: 3 would take ~13 min, more than the ~7 agreed); the probe is 32. Ran 02:40-02:50: no candidate clears the bar (below) |
| 15 | `bisw_check.sh` | 1 | Stage 1 checks: the lifter without `--bi-switch` gives the lift in use byte for byte, the switched lift, three refusals, syntax-only compiles (queued 00:58 behind `main:item4ctl`) |
| 16 | `perfab.sh aa1b base base 3` | 2 | the A/A again under the new quiet test (queued 01:15 right behind prof1). Ran at the 240 Hz cap: discarded |
| 17 | `perfab.sh aa1c base base 3` | 2 | the A/A at 960 Hz (`m08`): bar 8.7% (section 1) |
| 18 | `bisw_build.sh` -> `cand3_build.sh` | 1 | `exe/cand3`: the three runtime candidates in one exe, each with a knob that turns it off (section 3, #2 and #5). Failed at link time (duplicate symbols, below); redone as 29 |
| 19 | `bisw_ab.sh` | 2 | since 02:05: the regress run of `exe/cand3` at 30 and 240 fps, only if a candidate came out ahead in the A/B (14); otherwise it ends at once. Ended at once (02:56): none was ahead |
| 20 | `perfprof.sh prof2 base PROF_RIP_ROWS=4096` | 2 | the per-thread profile at the deep Palace save, uncapped, and an order file of every sampled function |
| 21-23 | `perfbuild.sh port base2`, `order ord1b runs/prof2/order.txt`, `order ord0b empty_order.txt` | 1 | the base objects back after the candidate builds, then the two relinks (each waits for its input's `.rc`) |
| 24 | `perfab.sh ordb ...` | 2 | exits at once (`runs/ordb/MOVED.txt`): moved to the end as 27 |
| 25 | `cand3_ab.sh` | 2 | exited at once (no `exe/cand3`: 18 had failed) |
| 26 | `perfab.sh aa2 base base 3` | 2 | the A/A at the deep Palace save: bar 2.8% (profile-window fps), 4.5% (gap fps); section 1 |
| 27 | `perfab.sh ordb2 ord0b ord1b 4` | 2 | the order file at the deep Palace save. Dropped at 04:40 after aa2 (`runs/ordb2/SKIPPED.txt`): 0 to +3% expected against a 2.8% bar |
| 28 | `perfab.sh r3b exe_nozi14 exe_bt3 4` | 2 | docs/53 R3 at the deep Palace save. Dropped at 04:40 after aa2 (`runs/r3b/SKIPPED.txt`): a guard that could only rule out a loss above 2.8% |
| 29 | `cand3_build2.sh` | 1 | `exe/cand3` again, with the original image 2 lift parked outside `work_spu` (queued 02:01 behind `t2:compile`) |
| 30 | `caus_build.sh` | 1 | the causal build again, from `scratch/perf-causal` b22e62d (end of the queue) |
| 31 | `perfslope.sh slope2 causal 2` | 2 | causal slopes at the deep Palace save. Ran 05:09-05:23: slopes 0.69-0.85 for HighGraphics and the FIFO thread, 1.51 for image 2 (below) |
| 32 | `r3probe_run.sh r3probe2 base 2` | 2 | image 10 after R3 (the probe of 14, moved to the very end at 02:06). Ran 05:24-05:30: no spin, no frame cost (the prof1 notes below) |
| 33 | `cr_test.sh` | 1 | the CR-fields test job (#12), queued 02:53 behind `dll-split:q1`. Exits at once (`runs/crtest/MOVED.txt`): moved beside 30 as 35, so it does not hold one slot while 26 waits on the other |
| 34 | `biswf_ab.sh` | 2 | the bi-switch alone: `SPU_BI_SWITCH=0` and the in-boot A/A, 3 boots of `exe/cand3` at the deep Palace save, pooled with 14's pairs (`knobpool.py`). Right behind 26, at the coordinator's OK (02:55). Ran 04:36-04:46: frame rate inside the noise, image 2 CPU -3 points (below) |
| 35 | `cr_test2.sh` | 1 | the CR test job beside 30 (one slot each): every `tools/test_*.py` (CI=1, so the conformance suite compiles and runs) and `check_ppu_scaffold.py` on the base and on the change; a lift of 7 hot functions with both lifters, `cr_norm2.py` on each file; their machine code (instructions, CR loads and stores); libsre lifted both ways; recomp_master's copies of the 7 functions against the base lifter's. Results in `runs/crtest2`. Ran 04:48-05:09: tests and conformance clean on both sides, libsre clean; the 7-function lift did not finish (below) |
| 36 | `cr_lift.sh` | 2 | end of the queue. Version 2 since 05:15 (v1 is `cr_lift_v1.sh`): only if `cr_gate2.sh` passes on 35, commits `perf/ppu-cr-fields` locally, lifts the whole EBOOT with both lifters (`lift_cr/base`, `lift_cr/recomp`), checks the base lift against recomp_master byte for byte (the A side), every CR chunk against the base lift with `cr_norm2.py`, and the 7 functions (`seven.diff`, `cr_norm.py`). Ran 05:31-05:33: the CR lift is clean against the base lift; the base lift differs from recomp_master only in where the hook declarations sit, so `lift_cr/OK` was written by hand (below) |
| 37 | `cr_build.sh` | 2 | only with `lift_cr/OK`: the runtime in `wt-cr`, the port from the scratch worktree `wt-portcr` (master with its three `ctx->cr` sites moved to the helpers; never committed) in `build_cr`: `exe/crf` |
| 38 | `cr_reg.sh` | 2 | `perfreg.sh crfreg exe/crf 30 240` |
| 39 | `cr_ab.sh` | 2 | only if 38 passed: `perfab.sh crf base crf 6` at the deep Palace save (6 rounds since 04:41, with the time ordb2 and r3b left) |
| 40 | `cr_xval.sh` | 1 | the change's 280 new whole-CR conformance checks (CR logical ops, `mcrf`, `mfcr`, `mtcr`/`mtcrf` against a bit-numbered model) run against the base lifter in a temporary worktree, so the model is checked by the lifter every title runs on (queued 05:05 behind 31). Ran 05:23: 1,813 checks, 0 failed |
| 41-43 | `cr_build.sh`, `cr_reg.sh`, `cr_ab2.sh` | 2 | 37-39 again (`q_crend2.sh`, 05:35): 37-39 had exited at once (exit 3) on the missing `lift_cr/OK`. `cr_ab2.sh` is `cr_ab.sh` reading 42's exit code. 41 compiled the lift, then failed on an include (crbuild, below); 42 and 43 exited at once |
| 44-46 | `cr_build.sh`, `cr_reg.sh`, `cr_abx.sh` | 2 | the same a third time (`q_crend3.sh`, 05:4x), with `$L/ps3recomp` as a junction to `wt-cr`. 44 built `exe/crf`; 45 passed at 30 fps and failed at 240 on regress.sh's frame-name sort (crreg, below); 46 exited at once on 45's exit code |
| 47-49 | `cr_reg2.sh`, `cr_abx.sh`, `base_reg240.sh` | 2 | `q_crend4.sh` (05:5x): `perfreg2.sh crfreg2 exe/crf 240` with the fixed regress copy, then the A/B (`perfab.sh crf base crf 6`, only if 47 passed), then `exe/base` through the same copy at 240 as the control. Ran 05:52-06:16: both regress runs pass; the A/B reads +0.5%, inside the noise, with HighGraphics ~19% less busy at its work (crab4, below) |
| 50 | `perfprof.sh prof3 exe/crf` | 2 | `PERF_R3=0 ... PROF_RIP_ROWS=4096`: the frame chain with CR fields, to see which thread now sets the frame (coordinator OK 06:2x) |
| 51-54 | `crb_build.sh`, `crb_reg.sh`, `crb_knob.sh`, `crb_ab.sh` | 2 | `q_crb.sh` (06:3x): `exe/crfb` = CR fields + the bi-switch (local merge `scratch/perf-crb` d6231a5, image 2's switched lift, new build dir `build_crb`), its regress run at 30 and 240, the in-boot `SPU_BI_SWITCH=0` A/B (3 boots, `runs/crbk`), and only if the switch clears the knob bar there (`crb_gate.py`), a cross-build A/B against `exe/base` (6 rounds). 51 and 52 passed. 53 lost `perfknob.sh` to a SIGTERM from outside the lane in its second boot (06:34:35); 54 then ended at once |
| 55-56 | `crb_knob2.sh`, `crb_ab2.sh` | 2 | `q_crb2.sh` (06:4x): 53 and 54 again, tag `crbk2`. The wrapper ends its own levelcap through the sleep children if `perfknob.sh` dies on a signal, so the slots are never freed with a game still running. Ran 06:40-07:10: the switch pays inside the CR build (-2.08% with it off, p 0.041), so 56 ran; the pair against `exe/base` reads +0.3% profile and +1.3% gap fps, inside the bars (crbk2 and crfb, below) |

At the coordinator's request (01:45), the candidate A/B (25) took ordb's place and ordb and r3 went to the end of the
queue, so the other lanes wait no longer than before. At `m08`'s 8.7% bar, ordb and r3 would most likely have read inside
the noise. At the Palace (docs/28: about 1 fps at ~100 fps between alternated runs) they may not, which is why they
were moved and not dropped. aa2 then put the Palace bar at 2.8%, not ~1%, so both were dropped at 04:40 after all
(the coordinator had asked for that if tonight's numbers left them without a place).

**Two builds failed at link time (01:58), both from CMake globs that are evaluated at a different time than expected:**
- `exe/cand3` (18): `cand3_build.sh` kept the original image 2 lift next to the switched one as
  `work_spu/<img>.base`. The port's `file(GLOB SPU_CHUNKS CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")` checks the
  glob again at every build, so it found both directories and linked both: "duplicate symbol:
  spu_0001_at_0176C200_spu_func_00003010" and on. `cand3_build2.sh` (29) parks the original in `$L/lift_base` instead
  and puts it back after the link. A sibling directory under `work_spu` with a `spu_recomp.c` in it is always linked.
- The causal build (08, 09): `scratch/perf-causal` had its new code in a new file, `runtime/diag/tmp_spin.c`. The
  runtime's `file(GLOB_RECURSE ... "runtime/diag/*.c")` has no `CONFIGURE_DEPENDS`, so a build directory configured
  before the file existed never compiles it: "undefined symbol: g_tmp_ctr" and the other `tmp_` names. The fix is a
  scratch commit (b22e62d) that moves the code into `ps3_knob.c`, which is already built; the build is requeued as 30,
  the slopes as 31.
- 25 then found no `exe/cand3` and exited at once. The A/B first went back to 19 (`bisw_ab.sh`). At 02:04 the
  coordinator agreed to run it in r3probe's place (14), right after 29, so it no longer waits behind four other
  lanes. 19 now only runs the regress check when a candidate comes out ahead, and the probe went to the very end
  (32), behind everything queued before 02:06. The coordinator's own front-of-queue runs (the mouse-fix game run and
  `morning:all`) stay ahead of all of it.

The lane's builds share one ps3recomp worktree and one build directory, and the slot lock runs two one-slot waiters
side by side (ord0 next to rtlog, rtlog next to portlog). So `perfbuild.sh rt`, `port` and `order` take a lane lock
(`perflock.sh`) in the order they asked for it, and a `port` build that starts beside a `perf:rt*` slot owner waits for
it to end first.

Then candidates 2, 4 and 5, in the order the slopes and profiles point to. PGO goes last, and only once agreed.

**cand3 (10-19 02:40-02:50, deep Palace save `m02`, 960 Hz, `perfknob.sh`, 2 boots, 6 s windows in ABBAAB order): no
candidate clears the bar.** `exe/cand3` (8ec0c73d95f4) carries all three runtime candidates. Each knob turns one of
them off with the other two on, so a negative B-A means that candidate helps.

| Knob (B side) | A fps | B fps | B-A | Paired p | Pairs |
|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` (#2 off) | 111.23 | 108.93 | -2.07% | 0.283 | 6 |
| `TEX_CSUM_DIV=1` (#5: the divide back) | 112.58 | 112.60 | +0.01% | 0.991 | 6 |
| `SPU_MFC_CTX=0` (#5: the MFC slot off) | 111.92 | 111.50 | -0.37% | 0.729 | 6 |
| `PERF_NULL=1` (read by nothing: the in-boot A/A) | 111.65 | 110.82 | -0.75% | 0.595 | 6 |

- The bar for a knob: B-A past twice the in-boot A/A (here 1.5%) and at least 0.8%, with paired p under 0.05.
  `tex_csum` without the divide and the MFC slot are nowhere near it. Both are dropped; their branches stay local, with
  no PR.
- The bi-switch has the right size but is not significant. Its six pairs read +4.1, +0.1, -7.7, -4.7, +0.9 and -6.5 fps.
  CPU (`knobcpu.py`, % of one core): with it off, image 2's host thread went from 84.9% to 88.0% busy while
  HighGraphics stayed at 94.5-94.6%, which is what a cheaper dispatch on image 2 looks like. 34 (`biswf`) adds nine
  pairs for that knob alone.
- 19 (the regress run of `exe/cand3`) ended at once: no candidate came out ahead (B-A below 0 with p under 0.1).

**biswf (10-19 04:36-04:46, the same spot and exe, 3 boots, `SPU_BI_SWITCH=0` and `PERF_NULL=1` only): the bi-switch
saves image 2 CPU, but the frame rate does not show it.**

| Knob (B side) | batch | A fps | B fps | B-A | Paired p | Pairs |
|---|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` | biswf | 119.57 | 118.67 | -0.75% | 0.233 | 9 |
| `SPU_BI_SWITCH=0` | pooled with cand3 | 116.23 | 114.77 | -1.26% | 0.108 | 15 |
| `PERF_NULL=1` | biswf | 121.18 | 120.27 | -0.75% | 0.204 | 9 |
| `PERF_NULL=1` | pooled with cand3 | 117.37 | 116.49 | -0.75% | 0.214 | 15 |

- Against the bar (past 1.5%, twice the in-boot A/A's 0.75%; paired p under 0.05), the pooled -1.26% fails on both
  counts. Net of the A/A line it is about -0.5%, the low end of the 0 to +2% estimate (section 3). No PR, by the house
  rule; the branches (ps3recomp 8a107ad + ed39c83, port 022548a) stay local. Decision 3 asks whether to keep it anyway.
- CPU (`knobcpu.py`): with it off, image 2's host thread went from 85.8% to 88.6% busy (cand3: 84.9% to 88.0%), and
  HighGraphics, the FIFO thread and main stayed within 0.4 points. So the switch does take about 3 points of a core
  off image 2, roughly half the ~6.5% that the indirect dispatch costs. At this balance image 2 is not the thread that
  sets the frame (HighGraphics waits 1.1 ms of an 8.9 ms frame for it), so the saving stays CPU time.
- The in-boot A/A read -0.75% in both batches. Each window starts 1 s after a `setenv` of its own, so the console
  command is the same on both sides; ABBAAB leaves B a third of a window later on average, which would take a drift of
  2% per window to explain. It is noise at p 0.2, but it is why the bar comes from the null line and not from 0.

**crtest2 (10-19 04:48-05:09, one slot): the change passes everything that ran, but the 7-function lift never
finished.**
- Tests: all 22 `tools/test_*.py` and `check_ppu_scaffold.py` pass on both sides (base d5dec60 and the change, CI=1).
- Conformance suite: 1,533 checks on the base and 1,813 on the change, 0 failed on either. The 280 extra checks test the
  whole register (CR logical ops, `mcrf`, `mfcr`, `mtcr`/`mtcrf`) against a bit-numbered model of the 32-bit CR. The
  existing compare and record-form checks now read it through `ppu_cr_pack`. 40 (`cr_xval.sh`) runs the 280 against
  the base lifter.
- libsre, the whole module lifted both ways: the base lifter remakes the 10-16 lift byte for byte (libsre's A side),
  and the CR lift equals it after the CR rewrite. That covers 3,069 branches, 19,412 negated branches, 9,318
  compares, 198 CR logical ops, 431 `mcrf`, 231 `mfcr`, 97 `mtcrf`, 44,270 record-form CR0 updates and 217
  `stwcx.`/`stdcx.`. No CR field name is left. In the header, only the struct field and the helper block differ.
- The 7-function lift (both lifters, `-j 1`) hit its 600 s timeout on both sides, at a full core and 2.6 GB. A short
  function list does not cut the lifter's work the way that step assumed; the whole EBOOT takes about 41 s at
  `-j 16` (docs/53). I did not look into why tonight. The compile and code counts after it did not run either.
- `cr_prov.py` then printed "0 of 0 function(s) the same" with exit 0, so it passes an empty lift. v1's gate would
  still have stopped, on the missing mini-lift files.
- So the gate is now in two halves. `cr_gate2.sh` covers the tests, the conformance suite and libsre: clean, exit 0
  at 05:10. `cr_lift.sh` v2 lifts the whole EBOOT with both lifters and requires three things: the base lift equals
  recomp_master byte for byte, every CR chunk differs from it only in CR code, and so do the 7 functions
  (`seven.diff`). That is the 7-function diff the coordinator asked for, inside a check of every function. As before,
  no CR game run starts without `lift_cr/OK`.
- crxval (40, 05:23): the base lifter passes the change's 1,813 checks too, the 280 new ones included. So the
  bit-numbered model agrees with the lifter every title runs on today.

**crlift (36, v2, 10-19 05:31-05:33): the CR lift differs from the base lift only in CR code.** ps3recomp 32e8bf4
(`perf/ppu-cr-fields`, local) and d5dec60 each lifted the whole EBOOT in 40 s at `-j 16`, with master's hooks.
- Every chunk and the header equal the base lift after the CR rewrite (`cr_norm2.py`). Chunks 000-011 each have
  16,000-35,000 compares, 16,500-35,400 conditional branches, 75-790 CR logical ops, 77-411 `mfcr`, 36-268 `mtcrf` and up to 480
  `vcmp*.` CR6 updates; record-form CR0 updates are rare in the EBOOT (0-8 per chunk; libsre has 44,270). 012 and 013
  (809 and 72 functions without a CR use, and the function table) are byte for byte the same. No
  CR field name is left after the rewrite. The 7 functions (`seven.diff`, 874 changed lines) are CR-only too.
- The A side: the base lift is not byte-equal to recomp_master. It differs in one thing only, where the 12 insn-hook
  host functions are declared. ps3recomp #57 (in d5dec60) moved those `extern "C"` lines from `ppu_recomp.h` into the
  chunks that call each hook, and recomp_master was lifted before it. `cr_declcheck.py` shows all 15 files equal once
  those lines are set aside, with the same 12 hooks on each side. A declaration emits no code, and the port only
  defines these hooks, never calls them, so `exe/base` compiles exactly the base lifter's functions. `cr_lift.sh` v2
  wanted byte equality and wrote `MISMATCH`; after `cr_declcheck.py`, `lift_cr/OK` was written by hand, saying so.
  37-39 had already exited at once on the missing OK, so they run again as 41-43.

**crbuild (41 and 44, 10-19 05:35-05:42): `exe/crf` (4be26cbc0833).** 41 compiled all 14 chunks in 274 s at 12 jobs,
then stopped on `port/gen/libsre_glue.c`. That file includes `"../../ps3recomp/runtime/prx/prx_loader.h"`, which
compiles only because the lift's include directory (`work/recomp_master`) sits two levels below the main checkout,
so the path reaches that checkout's ps3recomp. `lift_cr/recomp` has no such neighbour. `$L/ps3recomp` is now a
junction to `$L/wt-cr` (prx_loader.h is the same file in both), and 44 (`q_crend3.sh`, with 45 and 46 behind it)
finished the build in 4 s. The exe is 296.3 MB against `exe/base`'s 299.4 MB.

Machine code of 12 lifted functions: the CR test's 7 (the ring writers around `0xB90F0` and `0x103878`) and five
more of HighGraphics' hottest in prof2 (`func_00CFEC70`, `func_00CF73F8`, `func_00CFA090`, `func_000F88E0`,
`func_000F1FF0`); `cr_codegen.py`, `runs/cr_codegen.txt`:

| | instructions | CR loads | CR stores | other CR accesses | memory operands | bytes |
|---|---|---|---|---|---|---|
| `exe/base` | 62,254 | 475 | 489 | 36 | 24,508 | 269,904 |
| `exe/crf` | 61,536 | 22 | 506 | 52 | 23,520 | 265,120 |

The loads of the packed word, each the link of a store-to-load chain, are gone (475 to 22). What stays is one byte
store per compare and a byte test per branch. Instructions -1.2%, memory operands -4.0%, code -1.8%.

**crreg (45, 10-19 05:4x): `exe/crf` passes at 30 fps. The 240 fps failure is a bug in regress.sh, not in the
change.** At 30 fps all four shots matched the good set (s1_spawn <- frame_183 ... s4_on <- frame_236). At 240 fps
s1_spawn was taken (frame_814 at t=87 s), then s2_walk, s3_turn and s4_on each read "no new frame".
- The cause: `shot()` picks the newest dump with `ls "$RUN/shots" | sort | tail -1`. `sort` compares names as text,
  so once the dumps pass `frame_999.bmp` (`frame_1000.bmp` sorts before it), `tail -1` returns `frame_999.bmp` from
  then on. `before` and `now` are then always equal. At 240 fps the dumps pass 999 just before s2_walk (45 had 2,424 by
  the end); at 30 and 120 fps they stay under 300 and 700.
- It happens to any exe, and the coordinator has had it in the report since its validation run of master at 10-18
  22:49 (`$S/overnight/val/v1.log`: s1_spawn <- frame_816, then s2-s4 "no new frame"; 30 and 120 fps passed). Its v2
  run used a fixed copy and passed at 240. Port PR #72 (`tools/regress-by-frames`) replaces `shot()` with a wait for
  new files picked by dump number, so the bug goes away when #72 merges. No other tooling change is needed.
- For tonight, `sort -V` (version order) in both lines is enough. `$L/regfix/regress.sh` is master's copy (from lc46)
  with only that change, beside lc46's `deslib.sh`, unchanged. `perfreg2.sh` is `perfreg.sh` pointed at it; it stays
  lane-local. 47 runs `exe/crf` at 240 through it, and 49 runs `exe/base` the same way as the control.
- So a 240 fps run of master's regress.sh says nothing about s2-s4 whenever its dumps pass 999 before s2_walk, and
  at the regress spot (the Tutorial, which runs past 240 fps uncapped) they do. 45's 240 fps run is not a result.

**crreg2 (47, 10-19 05:52-05:54): `exe/crf` passes at 240 fps with the fixed copy.** All four shots ok against the good
set, close to master's exe in the coordinator's v2 run at 240 with its own fixed copy:

| shot | crf frame | crf mean / changed | v2 frame | v2 mean / changed |
|---|---|---|---|---|
| s1_spawn | 809 | 4.2 / 0.3% | 807 | 4.7 / 1.1% |
| s2_walk | 992 | 9.0 / 3.6% | 988 | 8.2 / 3.5% |
| s3_turn | 1067 | 13.1 / 10.4% | 1066 | 11.8 / 9.0% |
| s4_on | 1201 | 15.3 / 14.9% | 1197 | 15.0 / 14.2% |

With 45's 30 fps pass, `exe/crf` has passed regress at both rates, so 48 (the A/B) runs. 49, the control, passed too:
`exe/base` through the same copy at 240 (frames 812/996/1072/1209, means 4.4/9.4/11.3/16.7, all ok).

**crab4 (48, 10-19 05:54-06:14, the deep Palace save `m02`, 960 Hz, 6 ABBA rounds, all 12 boots valid): no
frame-rate gain. HighGraphics does about a fifth less work, and the frame waits on the other two threads.**
`exe/base` (00df402ef0d9) against `exe/crf` (4be26cbc0833).

| | A (base) | B (crf) | B-A |
|---|---|---|---|
| profile-window fps, boot means | 121.95-124.30 (median 122.93, spread 1.9%) | 121.85-125.60 (median 123.28, spread 3.0%) | rounds +1.19, +1.99, +1.06, -0.49, -0.57, -1.21%: median +0.49%, paired p 0.55 |
| gap fps, boot means | 126.80-130.70 (median 129.25, spread 3.0%) | 125.52-129.96 (median 129.00, spread 3.4%) | rounds +0.87, +0.32, +0.17, -1.21, -0.57, -1.01%: median +0.42%, paired p 0.50 |
| 1% low (profile windows) | 84.0 | 83.5 | |

Both are well inside aa2's bars (2.8% and 4.5%), and B is faster in only 3 of 6 rounds. By the house rule, no PR.

Per-thread CPU from the same boots (`abthreads.py`: each boot's two profile sessions, paired by round):

| % of the window | A (base) | B (crf) | B-A | paired p |
|---|---|---|---|---|
| HighGraphics busy | 96.7 | 91.2 | -5.5 | <0.001 |
| HighGraphics in guest syscalls | 13.2 | 29.9 | +16.7 | <0.001 |
| FIFO thread busy | 92.5 | 93.1 | +0.6 | 0.17 |
| image 2's host thread busy | 88.9 | 89.6 | +0.7 | 0.52 |
| main busy | 32.3 | 30.9 | -1.4 | 0.28 |
| main in guest syscalls | 68.8 | 70.3 | +1.5 | 0.006 |
| cores busy | 5.71 | 5.59 | -0.12 | 0.018 |

- The syscall rise is `sys_timer_usleep`, ~33 µs each: about 28,000 calls in an 8.2 s window on the base and about
  60,500 with CR fields, every boot on each side (A 9.9-12.3% of the window, B 26.5-29.7%). HighGraphics' `cond_wait`
  share fell a little (1.3-2.2% to 0.7-1.4%). These are the waits docs/28 and prof2 put on HighGraphics: its reply
  wait on image 2 and the full-ring sleep (`func_00B5D8D8`). Busy falls less than the syscall share rises because
  that wait spins in `SwitchToThread`, which counts as busy.
- So at the same frame rate, HighGraphics' time outside guest calls fell from about 87% to 70% of the window: about
  19% less work a frame on the thread that sets the frame on the base. The codegen above shows where that comes from.
- The frame rate does not follow because the frame is now set by the other two threads of the chain, which slope2
  found within a few points of a full core (FIFO 92-93%, image 2 88-90%). This is the case slope2 described: a cut on
  one thread pays only until the next one saturates. prof3 (`perfprof.sh` on `exe/crf`) traces which of them it is.
- So CR fields are a CPU saving today, like the bi-switch but larger: 0.12 cores, and the HighGraphics headroom that
  other work needs before it shows in frames. 51-54 test the bi-switch inside the CR build.

**prof3 (50, 10-19 06:19-06:22, `exe/crf`, the deep Palace save, R3 phase skipped): HighGraphics now waits more
than a quarter of each frame.** Windows 116.0-120.6 fps, gap fps 125.2-134.2, 5.48 cores busy (prof2 on the base:
5.64). Shares below are single-counted (`profsym2.py`; the digest's own lists still come from master's double-counting
`profsym.py`).

| frame chain, median ms (`chain.py`) | prof2, base (01:5x) | prof3, crf |
|---|---|---|
| frame | 8.91 | 8.16 |
| HighGraphics working | 7.72 | 5.80 |
| HighGraphics in `usleep` polls | 1.08 | 2.28 |
| image 2 napping | 0.82 | 0.60 |
| FIFO thread behind HighGraphics | 4.59 | 3.66 |
| render_frame | 2.85 | 2.69 |

- The two sessions are hours apart (the base read ~110 fps in prof2 and ~123 fps in crab4), so compare shapes, not
  absolute frame times. HighGraphics' working time drops from 87% to 71% of the frame, the same as crab4's syscall
  shares.
- HighGraphics: the ring writers `func_00103878` (4.7% -> 2.1%) and `func_000B90F0` (3.9% -> 2.0%) are less than
  half what they were. The `usleep` spin and sleep in ntdll (`+0x1623C0`, `+0x161DC0`) went from 13.1% to 26.7% of its
  samples. The coherent store path (`spu_lockline_*`, `vm_write32`, `spu_ll_acquire`, `spu_coh_notify_write`) is now
  the biggest piece of work left, about 10.7%, as before.
- Image 2 (busy 89.6%): the same profile as on the base (region `0x4498` 9.8%, `0x8CB8` 8.0%, `0x3C90` 7.5%,
  `mfc_submit` 5.9%, `spu_wrch` 5.4%, `spu_hook_ring_fetch` 5.1%). It naps less (0.60 ms a frame) and polls more:
  18-19% of its steps under `SPU_RATE`, against 14-15% in prof2.
- FIFO thread (busy 93.1%, the busiest thread now): `render_frame` 9.9%, VCRUNTIME140 copy and fill bodies 16.1%,
  `d3d12_draw_indexed` 8.4%, `gcm_rsx_process_fifo_unlocked` 7.1%, `upload_strip_vp_indexed` 6.3%,
  `rt_writeback_store` 5.7%; about the same as prof2.
- So HighGraphics waits on image 2, and image 2 polls more, too. That is what back-pressure from the FIFO thread
  would look like, and also what an image 2 limit plus a per-frame handoff would look like. The in-boot bi-switch A/B
  in `exe/crfb` (53) separates the two: if image 2 sets the frame, the switch now shows in frames; if the FIFO thread
  does, it shows only as image 2 CPU, as in biswf.

**crbbuild and crbreg (51-52, 10-19 06:21-06:30): `exe/crfb` (3d0f8edf2667), CR fields with the bi-switch.** The
runtime is the local merge `scratch/perf-crb` (d6231a5: 32e8bf4 + ed39c83, no conflicts), image 2 is the switched
lift `exe/cand3` linked (66d1ae4fbc2f), and the port and the PPU lift are `exe/crf`'s. The new build dir compiled 328
files in 278 s; its configure lines match `exe/crf`'s. Regress passes at 30 fps (frames 182/206/217/235, means
3.6/5.6/5.9/2.7) and at 240 fps with the fixed copy (809/996/1073/1208, means 4.0/8.5/13.2/14.1), all shots ok.

**crbk (53, 10-19 06:30-06:35): cut short.** At 06:34:35, 49 s into the second boot, `perfknob.sh` got a SIGTERM
from outside the lane. Its levelcap and game kept running, and the slot lock had already handed the slots on: a
devcheats build (`devfix:nozi1`) started beside the game at 06:34:52. The lane ended that levelcap at 06:35:55 by
ending its `sleep` children (15 in 10 s), so it reached "done", stopped the game and put the save back. Nothing
signalled levelcap or the game. The first boot had finished (3 pairs a knob, background CPU 12.6% at its start):

| Knob (B side) | A fps | B fps | B-A | Paired p | Pairs |
|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` | 114.97 | 116.33 | +1.19% | 0.42 | 3 |
| `PERF_NULL=1` | 115.40 | 115.03 | -0.32% | 0.39 | 3 |

- CPU (`knobcpu.py`, that boot): with the switch off, image 2 went from 88.8% to 91.7% busy (+3.0, as in biswf),
  HighGraphics from 91.9% to 90.3%, the FIFO thread from 94.3% to 93.4%.
- One boot answers nothing, and the sign is the wrong way for "the switch pays". 55 runs the three boots again.
- The SIGTERM came from another agent's test harness (the save-lock lane's `sigtest4.sh`), which the coordinator
  has stopped. `crb_knob2.sh` now guards against it.

**crbk2 (55, 10-19 06:40-06:49, `exe/crfb`, the deep Palace save, 3 boots, 9 pairs a knob): with CR fields, the
bi-switch shows in the frame rate.**

| Knob (B side) | A fps | B fps | B-A | Paired p | Pairs |
|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` (the switch off) | 121.50 | 118.98 | -2.08% | 0.041 | 9 |
| `PERF_NULL=1` (in-boot A/A) | 121.02 | 121.50 | +0.39% | 0.47 | 9 |

- That clears the knob bar: past twice the A/A (0.78%, so the 0.8% floor applies), with p under 0.05. Without CR
  fields (`exe/cand3`, biswf), the same knob read -0.75% in one batch and -1.26% pooled over 15 pairs, neither
  significant.
- CPU (`knobcpu.py`): with the switch off, image 2 went from 86.9% to 90.2% busy (+3.3, the same saving as in biswf),
  HighGraphics stayed at 90.8%, and the FIFO thread went from 93.6% to 92.8%.
- So once CR fields take the load off HighGraphics, image 2 is close enough to setting the frame that its 3 points
  pay. 56 measures the pair against `exe/base` across builds.

**crfb (56, 10-19 06:49-07:10, `exe/base` against `exe/crfb`, the deep Palace save, 6 ABBA rounds, all 12 boots
valid): the pair does not clear the cross-build bar.** The Windows session was unlocked between 06:40 and 06:49, so this run
presented to the screen (`RSX_PRESENT=1`, where crab4 and crbk2 ran with 0), and the coordinator saw user input at
06:50. No boot shows input the script did not make: no `[cellPad] keyboard input` line (the first mapped key pressed
while the game has focus prints one), exactly two profile sessions per boot (F11 would add one), and nothing from
F9, F10 or Alt+Enter. The absolute rates are lower than crab4's (base 115.8 against 123.1 profile fps).

| | A (base) | B (crfb) | B-A |
|---|---|---|---|
| profile-window fps, boot means | 114.65-118.10 (median 115.40, spread 3.0%) | 113.90-118.10 (median 115.25, spread 3.6%) | rounds +0.04, -1.95, +2.16, -2.11, -0.65, +1.26%: median +0.30%, paired p 0.77 |
| gap fps, boot means | 120.20-124.49 (median 121.89, spread 3.5%) | 121.63-124.52 (median 123.31, spread 2.3%) | rounds +2.15, +1.84, -0.04, +1.61, +0.02, +0.52%: median +1.33%, B faster in 5 of 6, paired p 0.049 |
| 1% low (profile windows) | 73.9 | 76.6 | |

| % of the window (`abthreads.py`) | A (base) | B (crfb) | B-A | paired p |
|---|---|---|---|---|
| HighGraphics busy | 95.4 | 88.6 | -6.8 | <0.001 |
| HighGraphics in guest syscalls | 16.2 | 31.5 | +15.3 | <0.001 |
| image 2's host thread busy | 87.3 | 85.1 | -2.2 | 0.029 |
| FIFO thread busy | 92.9 | 93.3 | +0.4 | 0.45 |
| cores busy | 5.66 | 5.50 | -0.17 | 0.040 |

- The gap fps moved +1.33% with p 0.049, against aa2's 4.5% bar for gap fps; the profile fps did not move. Neither
  counts. The in-boot result (the switch is worth 2.1% inside the CR build) and this one do not contradict each
  other: a ~2% step is below what a 6-round cross-build A/B can resolve here (aa2's round deltas have an sd of ~1.4%
  for profile fps, ~2.2% for gap fps), and this run had the screen and possibly someone at the PC in it.
- The CPU side is clear: HighGraphics -6.8 points busy and +15.3 points waiting, image 2 -2.2 points, 0.17 cores in
  all. The FIFO thread, at 93%, is the one thread neither change touches, and it is now the busiest.
- By the house rule, no PR for either change. Decisions 3 and 6 carry the numbers. A rerun of this A/B with the
  session locked and nothing else running would settle the gap-fps reading: the command is
  `env PERF_SAVE=deep PERF_MAP=m02 bash perfab.sh <new tag> exe/base/des_boot.exe exe/crfb/des_boot.exe 6`.

**slope2 (10-19 05:09-05:23, the deep Palace save, `exe/causal`, 2 boots, 6 pairs per knob): every thread of the
chain is close to saturated.** A slope is the frame time added per millisecond of work added on that thread.

| Knob (B side) | Thread | Added ms a frame | A fps | B fps | B-A | p | Slope | 10-17 (docs/28) |
|---|---|---|---|---|---|---|---|---|
| `TMP_SLOW_FLIP_US=500` | HighGraphics, per flip | 0.50 | 115.3 | 110.9 | -3.9% | 0.040 | 0.69 | 0.77 |
| `TMP_SLOW_COH_NS=50` | HighGraphics, per coherent store (41,662 a frame) | 2.08 | 117.9 | 100.9 | -14.5% | <0.001 | 0.69 | |
| `TMP_SLOW_PRESENT_US=500` | FIFO, per present | 0.50 | 116.1 | 110.6 | -4.7% | 0.005 | 0.85 | 0.87 |
| `TMP_SLOW_DRAW_NS=200` | FIFO, per draw call (6,416 a frame) | 1.28 | 116.1 | 104.5 | -10.0% | <0.001 | 0.75 | |
| `TMP_SLOW_RING_NS=25` | image 2, per ring command (38,456 a frame) | 0.96 | 118.8 | 101.3 | -14.7% | <0.001 | 1.51 | |
| `TMP_SLOW_SC=1,108,*,300` | main, per `sys_cond_signal` | 0.60 | 116.1 | 113.5 | -2.2% | 0.051 | 0.34 | |

- CPU with each knob on (`knobcpu.py`, % of a core, A then B): HighGraphics 95-96 (99.0 under `COH_NS`), image 2 88
  (92.7 under `RING_NS`), the FIFO thread 91-93. All three are within a few points of a full core, and slowing any
  of them costs 0.7 to 1.5 times the added work.
- These are slowdown slopes. Near a bottleneck they say more about what hurts than about what a speed-up gives. The
  bi-switch (above) took about 3 points of a core off image 2, which by `RING_NS`'s 1.51 would be worth ~4% if the
  slope held both ways; it gave ~0.5% net. Under `RING_NS`, image 2 becomes the limiter (92.7% busy, HighGraphics
  down to 91.4%). The 1.51 probably also counts HighGraphics' full-ring sleeps (`func_00B5D8D8`, a
  `sys_timer_usleep` wrapper), each longer than the delay that caused it.
- So a cut on one thread pays only until the next of the three saturates. That matches docs/28's "+1-2% per good
  trim". Changes that speed up all lifted PPU code at once, CR fields (#12) or PGO (#3), are the ones that can do more,
  and even those stop at the FIFO thread (~92% busy) or image 2 (~88%).

**logwrap (10-19 01:37-01:48, `m08`, 960 Hz, 3 rounds): no measurable cost.** ps3recomp 3380439
(`fix/log-count-wrap`, exe 937ebe8f61c0) against `exe/base` (d5dec60, 00df402ef0d9), all six boots valid.
- Profile-window fps, boot means: A 318.7 / 325.8 / 329.4 (median 325.8, spread 3.3%), B 324.1 / 304.5 / 315.5 (median
  315.5, spread 6.2%). Round deltas +1.7%, -6.5%, -4.2%: median -3.2%, paired p 0.34, inside aa1c's 8.7%.
- Gap fps: A median 339.4, B 337.2; round deltas -0.6%, -3.3%, +2.5%: median +0.1%, p 0.79, inside 6.0%.
- Logs: all 77 tags print the same count per boot on both sides; `[mfc-list] ISSUE` reaches its limit of 32 in all six
  boots. `logcmp.py` first listed 60 differing tags. They were all interleaving: threads write at once, so a line can
  start "[D[mfc-list] ..." and the leading-tag count files it under a garbled tag. `logcmp2.py` counts each clean tag
  wherever it appears in a line.
- The live save was identical after every boot.

**prof1 (10-19 01:08, `m08`, at the 240 Hz cap, so a map of the Tutorial and not of the Palace):**
- Every window read 238-240 fps, 794 draws a frame. Busy share (tslack, W2): image 2's host thread 74.5% (napping
  13-15%, polling 33-34% of its steps), the FIFO thread 64.3%, HighGraphics 55.9%, main 30.7%; 3.1 cores busy.
- Image 2: 29.8M steps/s, 7.3M full channel writes/s, 4.9M DMAs/s, 7.3M GETLLARs/s. Hottest (share of its
  profile samples, each counted once; see the profsym note below): `spu_region_00004498` 8.8% (the command dispatch
  Stage 1 rewrites), `spu_wrch` 4.7%, `spu_lockline_read_ok` 3.5%, `spu_getllar_read` 3.3%, `mfc_submit` 3.1%;
  37% of its samples are waits (the cap).
- FIFO thread: `gcm_rsx_process_fifo_unlocked` 11.4%, `rsx_process_method` 8.8%, memcpy (VCRUNTIME140) 8.6%,
  `render_frame` 5.3%, `d3d12_draw_indexed` 4.8%; 27% waits.
- Image 10 after the R3 press: no loop. Its runs rose by ~1,000 a second like image 7's, its host thread sat in waits
  (96% in ntdll), and the cores busy fell from 3.10 to 2.84 (fps sat at the cap, so it could not show a cost). So
  tonight's run did not reproduce the soak's drain-resume loop; r3probe (32) looked again at the deep Palace save
  (05:24-05:30, 2 boots): no spin in either boot, image 10 kept running normally (about 1,000 runs a second before and
  after the press), and the gap fps moved -2.2% and +0.3% after the press, inside the noise. The loop needs something
  the soak had and these short boots do not; it stays a soak-only lead.
- Its order file had 61 functions (the console prints 40 rows of each by-address table unless `PROF_RIP_ROWS` says
  more), so ord0/ord1 were dropped and prof2 rebuilds it at the Palace with every sampled address.

**prof2 (10-19 01:51-01:57, deep Palace save `m02`, 960 Hz, `exe/base`): the Palace map.**
- Windows 105.8-112.0 fps in the profile sessions (1% lows 64-72), gap fps 109.2-116.1, 2,254 draws a frame in every
  window. That is docs/28's spot and rate (~110-120 fps), so the lane's numbers from here on compare with docs/28's.
- Busy share (tslack, W2, 5.64 cores busy): HighGraphics 96.0% (p90 98.7%), the FIFO thread 92.2%, image 2's host
  thread 88.8%, main 34.0%, ChrInsManThread 23.8%, vpj_worker 22.7%, seven pool `spu_async_thread`s ~18% each. All
  three chain threads are within a few percent of a full core, as in docs/28.
- Frame chain (trace, 220 frames, median): frame 8.91 ms; HighGraphics works 7.72 ms and polls 1.08 ms in `usleep`
  (its wait for image 2's reply); image 2 naps 0.82 ms; render_frame 2.85 ms; the FIFO thread trails HighGraphics by
  4.59 ms. Image 2 slept 10.7% of the span (242 naps), image 7 7.1%. The ping-pong of docs/28 is still the shape:
  HighGraphics waits for image 2 about 12% of the frame, image 2 waits for HighGraphics about 9%.
- HighGraphics (profile samples, each counted once): `usleep`'s `SwitchToThread` spin (ntdll) 11.0%; the ring
  writers `func_00103878` 4.7% and `func_000B90F0` 3.9% (16-byte commands with a full fence after each, then the put
  word; `usleep(30)` while the ring is full); the coherent store path 10.7% (`spu_lockline_unlock_ea` 2.6%,
  `spu_lockline_lock_ea` 2.5%, `vm_write32` 2.5%, `spu_coh_notify_write` 1.6%, `spu_ll_acquire` 1.5%), the same
  ~10% as docs/28 measured at ~105 fps; waits 3.2%; then `func_00CFEC70` 2.7%, `func_00A43718` 2.3%,
  `func_00CF73F8` 2.3%, `func_00CFA090` 2.2%, `func_000F88E0` 2.2%, `ps3_indirect_call` 1.8%, and a long tail of
  lifted functions (the top 40 rows hold 71% of the samples).
- Image 2: 34.5-35.3M steps/s, 2.5M full channel writes/s, 10.0-10.2M DMAs/s, 2.4-2.5M GETLLARs/s, ~145 naps/s
  (napping 2-3%, polling 14-15% of its steps while `SPU_RATE` runs). Hottest: waits (its naps) 10.3%, region
  `0x4498` 10.1% (the command dispatch Stage 1 rewrites), region `0x3C90` 7.7%, region `0x8CB8` 7.2%, `mfc_submit`
  6.3%, `spu_wrch` 5.1%, `spu_hook_ring_fetch` 4.3%, `mfc_for` 4.2% (the lookup `perf/spu-mfc-ctx` shortens), region
  `0x7140` 3.5%.
- FIFO thread: `render_frame` 10.4%, `d3d12_draw_indexed` 8.4%, `gcm_rsx_process_fifo_unlocked` 7.6%, waits 6.5%,
  `upload_strip_vp_indexed` 5.4%, `rsx_process_method` 5.2%, `rt_writeback_store` 5.2%, `rsx_fetch_vertex_arrays`
  5.1%, `vp_get_vs_mode` 3.2%, and 14.9% in three VCRUNTIME140 routines between `strstr` and the `memcmp`/`memcpy`/
  `memset` exports (8.5%, 3.8%, 2.6%; internal entry points, most likely the copy and fill bodies). render_frame's split
  in a profile session: draw-prepass 1.55 ms, vp-pass 1.10 ms, wait+store 0.51 ms; texture lookups 0.86 ms (5,629 a
  frame), SRV work 1.38 ms a frame (both inflated by the session's own timing).
- **profsym note.** The first digests of prof1 and prof2 showed most exe functions at about twice these shares (prof2:
  `func_00103878` 8.1%, region `0x4498` 19.4%, `render_frame` 19.3%). `tools/des/profsym.py` starts summing at any
  line with "--- exclusive" in it, and a `profrip` dump has a fourth table, "--- exclusive by address ---", so each
  sample in an exe function is counted once per table. prof1 printed 40 address rows, so only its top functions were
  inflated; prof2 printed all of them (`PROF_RIP_ROWS=4096`). DLL rows were not doubled (the two tables name them
  differently). The numbers above come from the per-function table alone (`$L/profsym2.py`); the repo tool now reads
  one table (port branch `tools/profsym-one-table`, 41607bb, pushed; PR text in `$L/pr_tools-profsym-one-table.md`). docs/28's profile shares look single-counted (its coherent store
  path at ~105 fps adds up to the same ~10% as prof2's).
- `vpj_worker` and main are mostly waiting (ntdll 78% and 67% of their samples).
- Image 10 after the R3 press: again no loop. Its runs rose by ~950 a second (+4,732 in 5 s; image 7 +2,364), its host
  thread was in waits (77% of its samples in ntdll), cores busy stayed at 5.64, and the windows after the press read the
  same as before (108.9 and 110.9 against 109.3-112.0). The `SPU_RATE` lines for image 10 are garbage (e.g. 8.9e18
  GETLLARs a second): image 10 runs on the shared `spu_async_thread` pool, and the per-second rate subtracts counters
  from different host threads. Image 2 has a thread of its own, so its lines are fine.
- `order.txt`: 822 sampled exe functions, the top 10 hold 29.2% of the exe samples and the top 100 81.5%. The relinks
  with it (`ord1b`) and with an empty list (`ord0b`) were clean (no compiles, no order warnings).

**Image 10 (coordinator lead from the 10-19 soak).** From the first R3 press on, image 10 (`spu_0009_at_0180C600`, a
SPURS task; by its strings, Havok Animation's sample-and-combine job) re-entered its code at LS `0xB33C` through
`spu_drain_call` about 2.7M times a second. Static reading of the lift:
- `0xB2B8` releases a Havok semaphore in main memory: one word, `[port:8][waiters:8][count:16]`, updated under
  GETLLAR/PUTLLC. `0x4F80` calls it once at its end, after its DMA work.
- `0xB33C` runs GETLLAR and checks that the count is at most 1000. Above that it runs `stop 0`, an assert. RPCS3 stops
  emulation on an unknown stop code ("Unknown STOP code"), so on the PS3 this count never goes above 1000.
- `0xB380` adds n to the count, wakes min(waiters, n) through `sys_spu_thread_send_event` (`0xD420`) and runs PUTLLC.
  A failed PUTLLC goes back to `0xB33C` through the trampoline, not through `spu_drain_call`. Waking more than one
  waiter is a second `stop 0`.
- Our `spu_stop` returns on an unknown code (unless `YDKJ_STOP_HALT=1`), with `pc` still at `0xB33C`, and
  `spu_drain_call` resumes there: GETLLAR, compare, stop, for as long as the count stays above 1000. So the loop is
  the assert, not failing PUTLLCs (an earlier version of this draft said that). The fault is upstream of it: in our
  port this semaphore is released more often than it is acquired.
- It holds one host core for the rest of the session. Whether it also costs frames is what prof1's R3 phase and
  `r3probe.sh` (step 14) measure: the gap fps before and after the press, and after halting the task with
  `YDKJ_STOP_HALT=1` (the runtime then ends the task as if it returned).
- No perf A/B presses R3, so none of them includes this loop.

Fix directions, once r3probe has the word and its readers: find why the count grows (who acquires this semaphore on
the PPU side; `find32` lists the guest words that point at the line), and, in the runtime, end a SPURS task on a
`stop` nobody services instead of resuming it at its function entry. The second is what `YDKJ_STOP_HALT=1` does today;
it needs a check that no policy module or other image relies on the return.

## 5. Decisions (open at the time)

1. **Order file, if it pays:** commit a list of lifted function names plus a link flag? The names are guest addresses,
   the same class as `port/insn_hooks.txt`. It would be regenerated per lift.
2. **PGO: settled (10-19, 17:2x), a developer and measurement tool only.** Nothing goes to players: no profile is
   shipped or downloaded, and setup has no profile step. Measured +6.3% profile fps and +7.9% gap fps at the deep
   save (pgo1, section 6). The tool is `tools/pgo/pgo.sh` (port PR, docs/58). No PGO-only runs follow (the Tutorial
   and tool A/Bs were dropped), and the perf stack gets no PGO build.
3. **CPU-only savings:** keep a change such as Stage 1 for its CPU time if its frame-rate gain stays inside the noise?
   **Settled on the evening of 10-19:** with the batch in, Stage 1 (the bi-switch) clears the bar on its own (stackk5,
   -4.53% with it off, p 0.005), so it is ps3recomp #73 and port #83.
   The house rule drops it. Stage 1 is now measured (section 4, biswf): it takes about 3 points of a core off image 2
   (85.8% against 88.6% busy; cand3 84.9% against 88.0%), while the frame rate with it off read 1.26% lower over 15
   pairs (p 0.11), against 0.75% lower for the A/A line. It would pay in frame rate where image 2 sets the frame
   (docs/48: a busier ring, or a slower CPU). The branches are local and unpushed: ps3recomp `perf/spu-bi-switch`
   (8a107ad runtime, ed39c83 lifter) and port `perf/spu-bi-switch` (022548a); `bisw_check.sh` (15) has its lifter
   checks.
   - With CR fields (Decision 6) under it, the switch does reach the frame: in `exe/crfb`, turning it off cost 2.08%
     over 9 pairs (p 0.041, crbk2), past the knob bar. Against `exe/base` across builds, the pair read +0.3% profile
     and +1.3% gap fps (p 0.77 and 0.049; crfb, run with the screen on and someone possibly at the PC), inside the
     cross-build bars. So the two together are worth about 2% at most at this spot, below what a cross-build A/B
     resolves; a quiet rerun of crfb is in section 4.
   - In the stack (stackk1, section 6), with the FIFO cuts in as well, turning the switch off costs 0.9% (p 0.44),
     inside the noise: image 2 does not hold the frame there. So no PR on 10-19; the branches stay local, to measure
     again once the render thread (section 7) makes image 2 the limit.
4. **AVX2/BMI2 for the lift:** worth measuring, given that it drops pre-2013 CPUs?
5. R3's result feeds docs/53's open question: switch the recipe to `--chunk-lines 150000` or keep 600k.
6. **The condition register as eight field bytes (#12): settled, merged on 10-19 at 18:47** (ps3recomp #69, port
   #82). The question was whether to take it, given that every title has to re-lift. Prototype: ps3recomp
   `perf/ppu-cr-fields` (32e8bf4; results: section 4, rows 35-56).
   - Measured (crab4, 6 rounds at the deep Palace save): the frame rate does not move (+0.5% median, paired p 0.55,
     against a 2.8% bar), so by the house rule there is no PR. But HighGraphics' time outside guest calls falls from
     about 87% to 70% of the window at the same frame rate (its `usleep` waits go from 13% to 30%, p < 0.001 in every
     round), and the process uses 0.12 cores less. The frame is now held by the FIFO thread and image 2. Like
     Decision 3, this is a CPU saving that turns into frames only once those two get faster.
   - Paired with the bi-switch (`exe/crfb`, 51-56): the switch then pays in-boot (2.1%), but the pair against the base
     stays inside the cross-build bars (+0.3% profile, +1.3% gap fps), with HighGraphics -6.8 points busy, image 2
     -2.2 and 0.17 cores less. The FIFO thread (93%) is what both leave in place. The local merge
     `scratch/perf-crb` (d6231a5) and `exe/crfb` are kept for a quiet rerun.
   - If taken, it needs two PRs merged together: this ps3recomp branch, and a port change for the three sites below
     (only in the scratch worktree `wt-portcr` tonight, never committed, at the coordinator's request).
   - Checked: the tools tests and the conformance suite (1,813 checks, 280 of them new whole-CR checks that the
     current lifter also passes), the executable and libsre each lifted whole with both lifters (they differ only in CR
     code), a build that boots and plays, and regress at 30 and 240 fps.
   - ps3recomp: every CR emitter in the lifter (compares, record forms, conditional branches, the CR logical ops,
     `mcrf`, `mfcr`/`mtcr`/`mtcrf`, `stwcx.`/`stdcx.`, the `vcmp*.` record forms and `vcmpbfp.`), the context struct
     and four helpers in the generated header, and `runtime/ppu/ppu_context.h` (the struct and the helper bodies). In
     the runtime, only those helper bodies and the `PPU_CR_BIT` macro in `ppu_ops.h` touched the packed word; every
     other site already goes through the helpers (the compare macros in `ppu_ops.h`, the `stwcx.`/`stdcx.` helpers in
     `ppu_memory.h`). No HLE library reads CR. Three tests and two docs change with it.
   - This port: `des_motion_blur` (its cr7 EQ flip) and `warp_chr` (saves and restores CR around a guest call) in
     `des_options.cpp`; `des_menu_tick` (the same save and restore) in `des_menu_sfx.cpp`; on the dev-cheats branch,
     `des_dev.cpp`'s `set_cr7` and H11's `ctx->cr = (ctx->cr & ~0xFu) | 8u` (that is `ppu_cr_set(ctx, 7, 8)`). A save
     and restore becomes `ppu_cr_pack`/`ppu_cr_unpack`, a field edit `ppu_cr_get`/`ppu_cr_set`. docs/33 quotes
     `ctx->cr`. With the field renamed, a site that is missed fails to compile instead of reading the wrong bits.
   - The cost: a full re-lift of the executable and of libsre (the lift rewrites every chunk, so the next build
     recompiles all 14 of them, ~15-25 min), and the same for every other title on the fork. An old lift and the new
     runtime disagree on what the eight bytes mean, so the re-lift is not optional.
   - 10-19 afternoon (section 6): CR fields are what lets the FIFO cuts count. Together, with the bi-switch, +6.0%
     profile and +7.2% gap fps over the base (stackab1), against +0.5% for CR fields alone and nothing for the cuts
     alone. **Settled: merged at 18:47** as ps3recomp #69 (the same change as 74e12cd and 32e8bf4), port
     #82 (the five sites; `des_dev.cpp` is on master since #74) and the FIFO cuts, ps3recomp #70. The
     merged master needs the re-lift of the executable and libsre before it builds.

## 6. 10-19 afternoon: the FIFO thread, the stack and PGO

Run 56 left the pair (CR fields + the bi-switch) inside the cross-build bars, with the FIFO thread (93%) as the thread
neither change touches. The afternoon follows three lines, in this order of expected gain:

1. **PGO** (Decision 2) speeds up all three saturated threads at once: lifted PPU code (HighGraphics), lifted SPU code
   (image 2) and the runtime's C (the FIFO thread). The lane is `$S/lanes/pgo` (PLAN.md there): clang-cl 22.1.8 FE
   instrumentation through a compiler launcher (`pgocc.py`), so the tree's CMake files stay as they are; continuous
   mode (`%c`, `-mllvm -runtime-counter-relocation`) so the profile survives levelcap's `taskkill /F`; the lift's
   `vm_*_inl` accessors left out of the instrumentation (`profile_list.txt`). The A side is `exe/pbase`: the same
   sources (port 589cb27, ps3recomp b249b7a, the same lift bytes) through `nozi.py`, which does what `pgocc.py base`
   does (drops `/Zi`), with the same configure. Nothing of the profile is committed: it is made from running the game.
2. **The stack** (`exe/stack1`, ps3recomp `scratch/perf-stack2`, never pushed): CR fields (74e12cd), the bi-switch
   (2fcf923, 0df45d4, image 2's switched lift 66d1ae4fbc2f) and the FIFO cuts below, on b249b7a, with the port's three
   CR sites changed in a scratch worktree (`wt-portstack`). If the FIFO thread is what holds the frame after the pair,
   the FIFO cuts show in frames here and not on the base.
3. **FIFO cuts** (ps3recomp `perf/fifo-vs-memo`, local): 40ced98 (a re-sent vertex program word or constant that equals
   what is stored is not a change: the program version and the constant generation stay, so `vp_get_vs_mode`'s memo
   and `VP_CB_REUSE` keep hitting; the memo becomes 2-way and checks the program bytes; `vp_record_cb` reads c[0] from
   the state), bc48c67 (a hash names the sampler table to try first, in place of comparing the draw's 128-byte key
   with up to 128 tables). Each part has a live knob: `RSX_VP_UCODE_CMP`, `RSX_VCONST_CMP`, `RSX_VP_MEMO` (2 new, 1
   the old memo), `RSX_VCB_C00`, `RSX_SAMP_HASH`. Left out on purpose: a per-draw state memo in the VP pass (skipping
   unchanged `SetPipelineState`, viewport and root arguments). It needs a reset wherever a root signature or heap
   changes inside the pass (the AA pass, the scaled-target passes), for 1-2% of the FIFO thread.

| # | Step | Slots | What |
|---|---|---|---|
| 57 | `perfreg2.sh fifo1reg exe/fifo1 30 240` | 2 | `exe/fifo1` = `exe/pbase` + 40ced98 (relink): regress at 30 and 240 fps. Passed (240: s1 2.8/0.3%, s2 9.2/3.4%, s3 10.0/6.7%, s4 16.0/15.9%, all ok); the live save identical |
| 58 | `perfknob.sh fifok1 exe/fifo1 3 knobs_fifo1.txt` | 2 | each part of 40ced98 off in turn, in-boot (below) |
| 59 | `smoke.sh` | 1 | the lane's smoke test (16:04): every check ok but one, FE use's `.text$hot` sign in the map of the tiny test program; IR use has it. So the lane uses IR instrumentation (`PGO_KIND=ir`) |
| 60 | `q3d.sh`: `pgo_build.sh gen` (IR, 3 jobs) | 2 | the instrumented build. The first attempt (6 jobs) was stopped after 2 minutes: each instrumented chunk compile held ~2.9 GB (2.2 GB resident) and free memory fell to 0.2 GB. At 3 jobs every object compiled (16:12-16:28, chunks 165-285 s each), then the link stopped: `duplicate symbol: __declspec(dllimport) getenv`, in `boot_main.cpp.obj` and in `ucrt.lib`. boot_main.cpp's getenv cache defines `__imp_getenv`. The profile runtime (built /MT) calls plain `getenv`, and ucrt's import member for `getenv` also defines `__imp_getenv`. Fixed in the lane only: `getenv_thunk.obj` (defines `getenv` as a jump through `__imp_getenv`) goes into gen links through `pgo_port.bat`'s `_LINK_`, so ucrt's member stays out and the profile runtime's reads go through the same cache. The tree does not change |
| 61 | `stack_build.sh stack1 lift` (5 jobs) | 2 | q3d moved on to it at 16:28 when the gen link failed. `q3e.sh` replaces q3d and lets it finish |
| 62 | `q3e.sh`: the gen relink | 2 | 16:32-16:36, after the stack build (61) finished at 16:36:01. Only the link ran. verify: 7,941,713 counters, the profile runtime and its counter bias linked, no `libcmt`; exe md5 aaa5350b7851 |
| 63 | `pgo_run.sh deep1 exe/gen`, `merge.sh` | 2 | 16:36-16:41 at the deep save: the level at 13 s, a 77 MB profile file in continuous mode, 120 s settled, then the 14-step route (walks, camera turns, a walk back). The blind walk ran into enemies and the player died once (seen on screen): the profile includes a death and a respawn. The instrumented exe ran at about 70-85 fps. The live save was identical. Merged: 116,389 functions, 28,580 of them executed, 1.74e12 counts |
| 64 | `pgo_build.sh use ... deep1.profdata` (4 jobs) | 2 | 16:41-16:53 (11 minutes; the profile's fingerprint matched the gen build's inputs). verify: `.text$hot` 12.3 MB, `.text$unlikely` 220.6 MB (code the profile never ran), no profile sections; exe md5 435a2b9e6c1b |
| 65 | `perfab.sh pgo1 exe/pbase use_deep1 6` | 2 | PGO against the base, 6 rounds, 16:53-17:12: +6.3% profile fps and +7.9% gap fps, B faster in all 6 rounds (pgo1, below) |
| 66 | `perfab.sh stackab1 exe/pbase exe/stack1 6` | 2 | the stack against the base, 6 rounds, 17:12-17:32: +6.0% profile fps and +7.2% gap fps, B faster in all 6 rounds (stackab1, below) |
| 67 | `perfknob2.sh stackk1 exe/stack1 3 knobs_stack2.txt` | 2 | in the stack, 17:32-17:44: every FIFO cut off at once costs 6.7% (p<0.001), the bi-switch off 0.9% (p 0.44), the null line 0.35% (stackk1, below) |
| 68 | `perfreg2.sh stack1reg exe/stack1 30 240` | 2 | 17:44-17:48: passed at 30 and 240 fps (stack1reg, below) |
| 69 | `perfreg2.sh pgo1reg use_deep1 30 240` | 2 | dropped with the PGO runs at 17:2x, but it had started (17:48) before the placeholder that was to skip it; it swaps the save, so it ran to the end. Passed at 30 and 240 fps |
| 70 | `deepshot.sh deepshot1 exe/pbase exe/stack1` | 2 | 17:52-17:55, after a perf run's window was seen black: one boot per exe at the deep save with the A/B settings, a frame dumped every 1,500 frames. Both draw the Palace correctly in every dump (deepshot1, below). Not a measurement |
| 71 | `stack_build.sh stack2` | 2 | 17:55: `exe/stack2` (md5 5233bde805bd) = `exe/stack1` + the FIFO batch (b9bf61c on `scratch/perf-stack3`, the same change as `perf/fifo-batch` 0a54361, merged as ps3recomp #71). Only the runtime recompiled and relinked |
| 72 | `perfknob2.sh stackk2 exe/stack2 3 knobs_batch.txt` | 2 | the batch inside the stack, 17:56-18:12: both parts off cost 3.7% (p<0.001), the checksum workers alone 2.1% (p 0.005); no pipeline differed in the `RSX_PSO_IN_REC=2` windows (stackk2, below) |
| 73 | `perfreg2.sh stack2reg exe/stack2 30 240` | 2 | 18:12-18:16: passed at 30 and 240 fps (stack2reg, below) |
| 74 | `stack_build.sh stack3` | 2 | 18:26: `exe/stack3` (md5 b013b4ac18f8) = `exe/stack2` + the eight-way VS memo (6f65e27, kept on the local branch `perf/vs-memo-ways`). Only the runtime recompiled and relinked |
| 75 | `perfknob2.sh stackk3 exe/stack3 3 knobs_memo.txt` | 2 | the memo inside the stack, 18:27-18:40: no gain. The old two-way memo reads +0.60% (p 0.62), four ways +1.13% (p 0.07) (stackk3, below) |
| 76 | `perfreg2.sh stack3reg exe/stack3 30 240` | 2 | 18:40-18:44: passed at 30 and 240 fps. The window, saved during both runs, showed the game |
| 77 | `perfknob2.sh blackwin1 exe/stack3 1 knobs_bw.txt`, the window saved every 8 s | 2 | 18:45-18:48, after a report that the window went black a few times: black from the first time `RSX_VP_MEMO` went from 1 back to the default (blackwin1, below) |
| 78 | `stack_build.sh stack4` | 2 | 18:52: `exe/stack4` (md5 c4fe7d5bcbb4) = `exe/stack2` + the memo fix (63427f0 on `scratch/perf-stack5`; the same change is e1119c8 on `perf/fifo-vs-memo`, ps3recomp #72) |
| 79 | `perfknob2.sh blackwin2 exe/stack4 1 knobs_bw.txt`, the window saved every 8 s | 2 | 18:53-18:56: the window stayed drawn, black in 0 of the 21 saves in the level (blackwin2, below) |
| 80 | `perfknob2.sh stackk4 exe/stack4 3 knobs_cuts.txt` | 2 | 18:56-19:07: every cut off costs 2.54% (p 0.012), the A/A line +1.08% (p 0.38) (stackk4, below) |
| 81 | `perfab.sh stackab2 exe/pbase exe/stack4 6 SPU_BI_SWITCH=0` | 2 | 19:07-19:27: +4.81% profile fps (p 0.001) and +2.98% gap fps (p 0.014), B faster in 6 of 6 rounds; image 2 now holds the frame (stackab2, below) |
| 82 | `perfreg2.sh stack4reg exe/stack4 30 240` | 2 | 19:27-19:31: passed at 30 and 240 fps (stack4reg, below) |
| 83 | `perfknob2.sh stackk5 exe/stack4 3 knobs_bisw.txt` | 2 | 19:37-19:46: turning the bi-switch off costs 4.53% (p 0.005), the A/A line +0.61% (p 0.24) (stackk5, below) |
| 84 | `stack_build.sh stack5` | 2 | 19:49: `exe/stack5` (md5 7c9202b3d982) = `exe/stack4` + `FIFO_PUT_WAKE` (8d0f347 on `scratch/perf-stack2`; from master it is the local branch `perf/fifo-put-wake`, cc0ee92) |
| 85 | `perfknob2.sh stackk6 exe/stack5 3 knobs_wake.txt` | 2 | 19:49-19:59: no gain: `FIFO_PUT_WAKE=0` reads -0.34% (p 0.83), with a noisy A/A line (+3.75%, p 0.17) (stackk6, below) |
| 86 | `build.sh` in the merged2 lane: the masters after the 18:47 merges, re-lifted with the CR lifter | 2 | 19:31-19:37 failed (the port worktree had no `ps3recomp` junction, so `port/gen/libsre_glue.c` missed `prx_loader.h`); 19:58-19:59 passed with it: `work/tmp/merged2` (release) and `work/tmp/merged2-dev` (DES_DEV=ON), the release scanner clean on both |
| 87 | `perfreg2.sh merged2reg exe/merged2 30 240` | 2 | 19:59-20:03: passed at 30 and 240 fps, all four shots ok at both; the live save identical (below) |
| 88 | `cutdiag.sh` (cutflip lane): docs/62's fix built into the merged2 dev build, the arrival cutscene at 120 fps | 2 | 20:03-20:05: built (1 file compiled). With `DES_CUTEULER=0` the step at 13.3 s kept the nearest key (jumps 4); with the blend it blended (Euler flips 4, jumps 0). The live save identical (below) |

**pgo1 (65): PGO against the base, cross-build, 6 rounds, at the deep save.** A is `exe/pbase`, B `use_deep1`: the same
sources (port 589cb27, ps3recomp b249b7a) and the same lift, built the same way except for the profile.

| | A (base) | B (PGO) | B-A, median of the rounds | Rounds B faster | Paired p |
|---|---|---|---|---|---|
| Profile-window fps, boot median | 122.95 | 130.98 | +7.80 fps (+6.34%) | 6 of 6 | 0.001 |
| Gap fps, boot median | 127.64 | 137.58 | +10.19 fps (+7.94%) | 6 of 6 | <0.001 |
| 1% low (profile windows) | 82.25 | 89.45 | +8.8% | | |
| Median present interval | 8.00 ms | 7.50 ms | | | |

- Both bars cleared by a wide margin (2.8% profile, 4.5% gap). The smallest round was +3.45% (profile) and +4.28%
  (gap). B's boots spread no wider than A's (3.7% and 2.9%, against 3.9% and 6.4%).
- The threads (abthreads.py) stay as busy as before: HighGraphics 95.4 to 96.3%, the FIFO thread 91.9 to 91.4%, image
  2 88.0 to 86.9%, main 31.0 to 29.6% (no change past p 0.05). With the frame 6-8% faster at the same load, each of
  the three does its frame's work 6-8% faster: PGO sped up the lifted PPU code, the lifted SPU code and the runtime's
  C together, which is what was hoped for. HighGraphics' share in guest syscalls fell from 13.8 to 12.1% (p 0.012).
- This is the profile's best case: it was made at the same spot as the A/B. Code the route never ran is compiled cold
  (`.text$unlikely`, 220.6 of 272.3 MB of code). That would matter for shipping, but PGO stays a developer tool
  (Decision 2), so the Tutorial A/B that would have measured it (`pgotut1`) was dropped.

**stackab1 (66): the stack against the base, cross-build, 6 rounds, at the deep save, 17:12-17:32.** A is
`exe/pbase`, B `exe/stack1`: CR fields, the bi-switch (image 2's switched lift) and the FIFO cuts (40ced98, bc48c67),
on b249b7a. The session was unlocked by then, so this batch ran with `RSX_PRESENT=1`; pgo1 ran locked, with 0. Each
A/B is consistent inside, but pgo1's and stackab1's absolute rates are not comparable.

| | A (base) | B (stack) | B-A, median of the rounds | Rounds B faster | Paired p |
|---|---|---|---|---|---|
| Profile-window fps, boot median | 121.67 | 129.45 | +7.35 fps (+6.03%) | 6 of 6 | <0.001 |
| Gap fps, boot median | 127.10 | 136.19 | +9.10 fps (+7.16%) | 6 of 6 | <0.001 |

- Both bars cleared (2.8% profile, 4.5% gap). The smallest round was +4.98% (profile) and +3.69% (gap).
- The threads (abthreads.py): HighGraphics 95.0 to 92.3% (-2.8, p 0.001), and its share in the kernel 15.6 to 26.9%
  (+11.3, p<0.001). It now waits for the others, and its waits go through the OS. The FIFO thread 92.5 to 91.6%
  (p 0.36), image 2 88.0 to 89.2% (p 0.063), main 29.7 to 28.9%.

**stackk1 (67): inside the stack, in-boot, 3 boots, 9 pairs a line** (B = that part off):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` | 128.52 | 127.37 | -0.90% | +0.07 ms | 0.44 |
| every FIFO cut off (`RSX_VP_UCODE_CMP=0`, `RSX_VCONST_CMP=0`, `RSX_VP_MEMO=1`, `RSX_VCB_C00=0`, `RSX_SAMP_HASH=0`) | 126.04 | 117.62 | -6.68% | +0.56 ms | <0.001 |
| `PERF_NULL=1` (A/A) | 128.70 | 128.26 | -0.35% | +0.03 ms | 0.76 |

- The FIFO cuts carry the stack's gain: with all of them off, the stack loses 6.7%, about all of its 6.0% over the
  base. On the base they did not move the frame (fifok1, below), because HighGraphics set it there. CR fields are
  what lets them count: they take HighGraphics off the limit, the FIFO thread then sets the frame, and cuts to it
  show. So the two go together: CR fields alone +0.5% (crab4), the FIFO cuts alone nothing (fifok1), both together
  +6.0% (with the bi-switch, which adds nothing measurable here).
- The bi-switch does not show in the stack (-0.9%, p 0.44). Without the FIFO cuts it did (crbk2: -2.08% with it off,
  p 0.041). Image 2 is not what holds the frame once the FIFO thread is lighter.
- fifok1's odd first line (the program compare off read +2.1% on the base) does not come back: with every cut off
  together, the stack is 6.7% slower.
- **Withdrawn (18:50):** the cut line's A windows after its first B window, and the null line, ran with the stale-shader
  bug of the black window (77-79, above), so -6.68% is not a clean reading. stackk4 (80) replaces it: every cut off costs 2.54% (p 0.012; the A/A line +1.08%, p 0.38), in the stack with the batch.

**stack1reg (68): passed** at 30 fps (s1-s4 ok) and 240 fps (s1 4.6/1.0%, s2 7.6/2.6%, s3 10.8/8.3%, s4 16.0/15.8%,
all ok); the live save identical.

**deepshot1 (70): what the perf runs draw.** During stackab1 and stackk1 a perf run's window was seen black, with a
dark grey rectangle over its top-right quarter, while its title read 127 fps and 2,255 draws. One boot of each exe at
the same save and settings, dumping a frame every 1,500 frames, shows the Palace drawn correctly in both (7 and 8
dumps). So a run that switches no knob draws the real scene. The black window had another cause, found at 18:46
(the black window, 77-79, below).

**The FIFO batch (71-73): texture checksums on worker threads, each draw's pipeline in its record.** ps3recomp
`perf/fifo-batch` (0a54361, one commit on master; merged as #71 at 18:47): rows 1 and 3 of the study's table
below, each with a live knob.
- `TEX_CSUM_THREADS` (default 2, up to 8; 0 = the inline checksum as before): at the start of the draw prepass, worker
  threads checksum the texture slots the previous frame used, in the order that frame first bound them. Each result is
  tagged with the frame's serial, the guest offset and the length, and the prepass takes it only when all three match;
  otherwise it checksums inline as before. Every texture is still checked every frame, against its bytes as of the
  prepass, as before.
- `RSX_PSO_IN_REC` (default 1; 0 = the VP pass looks the pipeline up again, as before; 2 = both, logging the first 8
  draws whose two pipelines differ): the prepass keeps each draw's pipeline in its record, stamped with the frame's
  serial, and the VP pass uses it.

**stackk2 (72): the batch inside the stack, in-boot, 3 boots, 9 pairs a line** (B = that part off):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `TEX_CSUM_THREADS=0,RSX_PSO_IN_REC=0` (both parts off) | 131.04 | 126.17 | -3.72% | +0.30 ms | <0.001 |
| `TEX_CSUM_THREADS=0` (the checksum workers off) | 134.30 | 131.46 | -2.12% | +0.16 ms | 0.005 |
| `RSX_PSO_IN_REC=2` (the record's pipeline and a second lookup, compared) | 131.78 | 132.81 | +0.78% | -0.06 ms | 0.58 |
| `PERF_NULL=1` (A/A) | 133.54 | 133.10 | -0.33% | +0.02 ms | 0.71 |

- The batch is worth 3.9% in the stack (both parts off: -3.72%, p<0.001, 0.30 ms a frame), past the knob bar. The
  checksum workers carry most of it (-2.12% with them off, p 0.005).
- The pipeline in the record shows only with the checksums inline: then it adds the rest (the two lines differ by
  1.6 points). With the workers on, a second lookup costs nothing measurable (`RSX_PSO_IN_REC=2`, +0.78%, p 0.58):
  the FIFO thread is light enough there that another thread holds the frame with it.
- `RSX_PSO_IN_REC=2` also checks the record: across its 9 windows (54 s, about 7,000 frames of 2,255 draws), no
  draw's recorded pipeline differed from a fresh lookup (no `[PSO]` line in any boot's log).
- The measured exe is `scratch/perf-stack3` (b9bf61c). The PR's commit, `perf/fifo-batch` 0a54361 on master, is the
  same change with a local renamed (`pso_in_rec`: `pso_rec` is also a type name) and a plain `\n` in the `[PSO]`
  line; it passes a syntax-only compile with the build's flags.

**stack2reg (73):** passed at 30 fps (s1 1.9/0.1%, s2 6.5/3.2%, s3 5.2/2.3%, s4 2.5/0.1%) and 240 fps (s1
3.4/0.2%, s2 8.1/3.9%, s3 9.8/6.4%, s4 15.8/15.7%), all ok; the live save identical.

**The eight-way VS memo (74-76).** Row 5 of the FIFO study below: `vp_get_vs_mode` was still 4.3% of the FIFO
thread in prof4 (the stack with the batch), and it does its sizing, hash and cache scan only on memo misses. 6f65e27
keeps up to 8 programs per start, the most recently used first (`RSX_VP_MEMO=N`, 2 to 8, default 8; 2 = the two-way
memo of 40ced98). The program hash is unchanged, since the pipeline library may key on it.

**stackk3 (75): in-boot, 3 boots, 9 pairs a line** (B = the knob set, A = the eight ways):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `RSX_VP_MEMO=2` (the two-way memo) | 129.32 | 130.10 | +0.60% | -0.05 ms | 0.62 |
| `RSX_VP_MEMO=4` | 133.98 | 135.49 | +1.13% | -0.08 ms | 0.07 |
| `PERF_NULL=1` (A/A) | 133.69 | 133.48 | -0.16% | +0.01 ms | 0.85 |

- Eight ways are no faster than two: with the two-way memo the frame reads 0.6% faster, inside the noise, and four
  ways 1.1% faster (p 0.07), inside the knob bar too. So the remaining misses are not starts that switch among a few
  programs. Where the 4.3% goes (the copy compare on hits, or new programs) would take a profile of this build.
- The commit stays local (`perf/vs-memo-ways`; no PR), and row 5 of the study is closed.

**stack3reg (76):** passed at 30 fps (s1 2.1/0.1%, s2 7.2/2.2%, s3 6.5/2.5%, s4 3.5/0.2%) and 240 fps (s1 2.4/0.8%,
s2 8.6/4.2%, s3 7.3/2.8%, s4 7.7/3.7%), all ok; the live save identical. A capture of the game window during each
run (PrintWindow, the window's own content) showed the game drawn.

**The black window (77-79): a stale vertex shader after a live switch of `RSX_VP_MEMO`.** During stackab1 and stackk1
a perf run's window was seen black, with a flat dark grey rectangle over exactly its top-right quarter (640x360 of
1280x720), while the title read 127 fps and 2,255 draws; at 18:45 it was reported black a few times.
- blackwin1 (77) saved the window every 8 s through one perf-style boot with stackk1's cut line and the null line.
  The window showed the game through the settle time, the first window with the cuts on and the two with them off.
  From the first window with them back on (18:46:48) it was black, the title still counting 129-138 fps; drawn again
  in the next window with them off, and black from the end of the cut line to the end of the boot (11 of the 21 saves in the level).
- The cause is in 40ced98's memo. The one-entry mode (`RSX_VP_MEMO=1`, part of the cut line) writes entries with no
  copy of the program (length 0) and leaves the copy pointer and end bit from before. Back at two ways, the copy check
  compared 0 bytes, so it took such an entry for any program at that start, and draws got a stale vertex shader. A
  full-screen pass drawn with the wrong program covers the top-right quarter: that is the grey rectangle.
- Only live switches of `RSX_VP_MEMO` to 1 and back reach it. Play, regress and cross-build A/Bs never switch it, and
  the two-way memo always keeps a copy. The fix (e1119c8, ps3recomp #72; 63427f0 in the stack) makes the copy check
  skip an entry without a copy. #70 had merged at 18:47, before the fix was ready, so the fix is a PR of its own.
- Hit by it: stackk1's cut line (the A windows after its first B window, 2 of each boot's 3 pairs) and its null line
  (both sides), and fifok1's `RSX_VP_MEMO=1` line (its A windows after the first B) and the two lines after it (both
  sides). stackk4 (80) measures the cut line again with the fix. Not hit: every cross-build A/B (stackab1, pgo1 and
  the earlier ones), stackk2 and stackk3 (no switch to 1), and fifok1's first two lines.
- blackwin2 (79), the same boot on the fixed exe: the window showed the game in all 21 saves in the level (mean brightness 32.0-32.1, as in blackwin1's drawn saves; a black one reads below 2), through the same switches of `RSX_VP_MEMO` to 1 and back.

**stackk4 (80): the FIFO cuts in the stack again, with the fix, in-boot, 3 boots, 9 pairs a line** (B = every cut off;
the stack here also has the batch):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `RSX_VP_UCODE_CMP=0,RSX_VCONST_CMP=0,RSX_VP_MEMO=1,RSX_VCB_C00=0,RSX_SAMP_HASH=0` (every cut off) | 130.61 | 127.29 | -2.54% | +0.20 ms | 0.012 |
| `PERF_NULL=1` (A/A) | 129.96 | 131.36 | +1.08% | -0.09 ms | 0.38 |

- With the fix, every cut off costs 2.54% (0.20 ms a frame, p 0.012), past the knob bar, while the A/A line reads
  +1.08% (p 0.38). So the FIFO cuts are worth about 2.5% in the stack with the batch, not stackk1's 6.7%. In stackk1,
  the A windows after the first switch hit stale memo entries, which skip work, so they read fast.
- An audit of the seven knobs these A/Bs switch (the five cuts, `TEX_CSUM_THREADS`, `RSX_PSO_IN_REC` and
  `SPU_BI_SWITCH`) found no other state that a live switch leaves stale: each either checks again before it trusts what
  it kept, or keeps nothing that depends on the knob.
- The audit's agent indexed a ps3recomp worktree from about 18:56:30 to 18:56:59, during boot 1's load and before its
  first measured window (the windows start 40 s after the level starts loading), so no window was affected.

**stackab2 (81): the four PRs together against the base, cross-build, 6 rounds, at the deep save.** A is `exe/pbase`
(b249b7a), B `exe/stack4`: CR fields (#69 and port #82), the FIFO cuts with the fix (#70), the batch (#71), and the
bi-switch's lift with the switch off (`SPU_BI_SWITCH=0` on both sides; it does nothing on the base).

| | A (base) | B (the four PRs) | B-A, median of the rounds | Rounds B faster | Paired p |
|---|---|---|---|---|---|
| Profile-window fps, boot median | 119.50 | 125.57 | +5.75 fps (+4.81%) | 6 of 6 | 0.001 |
| Gap fps, boot median | 125.42 | 129.59 | +3.74 fps (+2.98%) | 6 of 6 | 0.014 |

- Profile fps clears its 2.8% bar. Gap fps does not clear its 4.5% bar, though B was faster in all 6 rounds.
- The background was busier than in stackab1: VS Code's C++ indexer (cpptools) took 0.7-4.3 s of CPU per profile
  session, against 0.6-1.7 s then, and the whole background 4.9-19.7% of the CPU, against 2.3-8%. The base read 1.8%
  lower than in stackab1 (119.50 against 121.67). B (125.6) also reads below stackab1's B (129.5), but the two ran two
  hours apart, so their absolute rates do not compare.
- render_frame: 3.0 ms on A, 2.1 ms on B (stackab1's B: 2.7 ms). The 1% low went from 79.7 to 85.2 fps.
- The threads (abthreads.py): the FIFO thread 92.2 to 81.8% busy (-10.4 points, p<0.001), image 2 87.3 to 91.7%
  (+4.4, p 0.008), HighGraphics 95.1 to 91.5% (-3.6, p<0.001; its share in the kernel 16.3 to 29.1%). With the batch
  in, the FIFO thread no longer holds the frame; image 2 does. That is the case Decision 3 was waiting for, so the
  bi-switch (off on both sides here) is measured again in stackk5.

**stack4reg (82):** passed at 30 fps (s1 4.1/1.8%, s2 7.1/2.0%, s3 6.8/2.8%, s4 13.1/20.4%) and 240 fps (s1 4.2/1.4%, s2 9.0/5.1%, s3 11.7/9.0%, s4 15.7/15.3%), all ok; the live save identical. s4 at 30 fps (20.4% of pixels) is the highest that shot has read, but inside its check.

**stackk5 (83): the bi-switch on master's stack, in-boot, 3 boots, 9 pairs a line** (B = `SPU_BI_SWITCH=0`):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `SPU_BI_SWITCH=0` (the gotos off) | 128.84 | 123.01 | -4.53% | +0.36 ms | 0.005 |
| `PERF_NULL=1` (A/A) | 132.39 | 133.20 | +0.61% | -0.05 ms | 0.24 |

- With the batch in, image 2 holds the frame (stackab2), and the switch takes about 3 points of a core off image 2: off,
  it now costs 4.53%, past the knob bar. Before the batch (stackk1, the FIFO thread the limit) it cost 0.9%, inside
  the noise. That settles Decision 3: the switch is ps3recomp #73 and port #83, as `perf/bi-switch` from master in
  both repos (the same diffs as the old local `perf/spu-bi-switch` branches, which stay). regress: stack4reg (82), on
  the same exe.

**FIFO_PUT_WAKE (84).** docs/67 found that 10.3% of the FIFO thread's samples are its kick wait, `WaitForSingleObject`
with the 1 ms `FIFO_KICK_MS` timeout, and that nothing sets the kick in Demon's Souls: only a guest thread polling a
dry fence queue does, and image 2 DMAs `put` instead. The walker reads `put` once per drain, so after each drain it
slept the whole 1 ms with segments already queued. `FIFO_PUT_WAKE` (live, default 1) skips the wait when `put` moved
since the last drain read it, and an SPU PUT that stores `put` sets the kick event while the present thread waits (a
flag and a seq_cst fence on each side, so one side always sees the other). Each put value skips one wait at most, so a
walker stopped short of `put` still sleeps: it is not docs/28's `FIFO_SPIN_US` dead end.

**stackk6 (85): FIFO_PUT_WAKE in the stack, in-boot, 3 boots, 9 pairs a line** (B = `FIFO_PUT_WAKE=0`, the old wait):

| Knob (B) | A fps | B fps | B-A | Frame time | p |
|---|---|---|---|---|---|
| `FIFO_PUT_WAKE=0` | 127.26 | 126.82 | -0.34% | +0.03 ms | 0.83 |
| `PERF_NULL=1` (A/A) | 125.18 | 129.88 | +3.75% | -0.31 ms | 0.17 |

- No gain. The FIFO thread does not hold the frame here (stackab2: 82% busy, image 2 92%), so its idle time was not on
  the frame's path, as docs/67 expected ("+0-3% now"). The run was also noisier than the others tonight: windows from
  113 to 134 fps inside one boot, and 13-19% background CPU, from six static-analysis agents' searches.
- Kept on the local branch `perf/fifo-put-wake` (cc0ee92, from master), no PR. Measure it again once the FIFO thread
  holds the frame (after image 2's ring loops), with a profile beside it to see the kick wait's share go: this A/B
  could not show that the wait went.

**The merged masters (86, 87).** After the 18:47 merges, master in both repos (ps3recomp 4a76fc0, port 53a047a) was built in a lane of
its own, with the EBOOT, libsre and the SPU images re-lifted by master's lifter (the CR fields need the re-lift; image 2
lifts without the bi-switch, which master does not have yet). The first build failed: the lane's port worktree had no
`ps3recomp` junction, so `port/gen/libsre_glue.c` missed `../../ps3recomp/runtime/prx/prx_loader.h`. With the junction
it built `work/tmp/merged2` (release, DES_DEV=OFF) and `work/tmp/merged2-dev` (DES_DEV=ON), and the release scanner
passed on both.
- regress passed on the release exe at 30 fps (s1 2.8/0.4%, s2 6.2/1.7%, s3 5.5/2.1%, s4 3.5/0.2%) and 240 fps (s1
  2.4/0.1%, s2 8.6/4.0%, s3 13.9/10.8%, s4 16.1/15.8%): all four shots ok at both, and the live save identical.
- Neither exe has the memo fix (#72) or the bi-switch (#73, #83). Play never switches `RSX_VP_MEMO`, so the memo bug
  cannot show in them.

**The cutscene diagnostic (88).** docs/40 traced the arrival cutscene's hitch above 30 fps to t = 13.3 s: one step of the camera's keys turns
3.14 rad while it moves 0.01 m, and `des_cut_cam`'s guard keeps the nearest key for any turn over 0.5 rad. docs/62 (an
agent's static work this evening) found that 0x00054D48 builds the rotation as Rx(x) Rz(z) Ry(y), with row vectors, so
(x + pi, y + pi, pi - z) is the same rotation. Its fix blends to those other angles when they sit within 0.5 rad of the
earlier key (`DES_CUTEULER`, live, default 1, read only under `DES_CUTFPS=1`). The diagnostic built the fix into the
merged2 dev build and ran the arrival cutscene twice at 120 fps with `DES_CUTLOG=1`:

| Run | At t = 13.308 s | Calls between keys | Blended | Euler flips | Jumps |
|---|---|---|---|---|---|
| `DES_CUTEULER=0` (the old guard) | "a jump (move 0.01 m, turn 3.14 rad, 0.01 to the other angles), nearest key kept" | 3000 | 2996 | 0 | 4 |
| The blend | "an Euler flip (...), blended to the other angles" | 3000 | 3000 | 4 | 0 |

- The keys at the step are (-1.6427 1.5704 0.0000) and (1.4980 -1.5616 3.1416); the later key's other angles are
  (-1.6436 1.5800 0.0000), within 0.01 rad of the earlier key. So the step is the same rotation in the other Euler set,
  not a shot change, and the blend goes through it.
- The fix is the local branch `fix/cut-euler-blend` (port, 796565b, from master; its message says how it was run). It
  has not been through regress.sh, and nobody has watched it yet. The exe is `work/tmp/cutblend` (DES_DEV=ON: the
  merged masters plus the fix).

**Vsync in these A/Bs.** The runtime presents with vsync by default (`RSX_VSYNC=1`, libs/video/rsx_d3d12_backend.c),
and none of the perf tools set it, so with `RSX_PRESENT=1` (the session unlocked, as tonight) a window cannot read
above the display's refresh, about 143 Hz on this PC. Tonight's windows sat at 110-135 fps, under it, but a B side
near it reads short, so the gains, if anything, are understated. From 10-20 the perf runs set `RSX_VSYNC=0` (the repo
versions of the tools in docs/63 do).

**fifok1 (58): the FIFO cuts on the base, in-boot, 3 boots, 6 s windows** (B = that part off):

| Knob (B) | A fps | B fps | B-A | p | FIFO thread busy, B-A |
|---|---|---|---|---|---|
| `RSX_VP_UCODE_CMP=0` | 118.17 | 120.66 | +2.11% | 0.010 | +0.5 |
| `RSX_VCONST_CMP=0` | 117.96 | 119.82 | +1.58% | 0.29 | +0.3 |
| `RSX_VP_MEMO=1` | 119.71 | 118.04 | -1.39% | 0.22 | +1.4 |
| `RSX_VCB_C00=0` | 120.37 | 119.34 | -0.85% | 0.39 | +1.9 |
| `PERF_NULL=1` (A/A) | 119.39 | 118.69 | -0.59% | 0.54 | +0.1 |

- As expected at the base, where HighGraphics (95%) sets the frame, no part moves the frame rate in the direction of
  the cut. Each part takes a little FIFO time off (turning one off adds 0.3-1.9 points of a core; about 4 points
  together, of ~90%).
- The one line past the knob bar goes the other way: with the program compare off, the frame rate read 2.1% higher
  (p 0.010; boot by boot +3.6%, +0.5%, +2.3%), while the FIFO thread works more with it off, not less. The compare
  itself runs either way (the knob only decides whether the program version moves), so the cost would have to come
  from the version staying put, and its only readers are the VS memo and `VP_CB_REUSE`, which both skip work then.
  Earlier runs' first lines (crbk2, cand3, biswf) show no bias toward B. With five lines, one at p 0.01 can be chance;
  line 2 of stackk1 (all cuts off together) is the second reading.
- **Partly withdrawn (18:50):** the `RSX_VP_MEMO=1` line's A windows after its first B window, and both sides of the
  last two lines, ran with the stale-shader bug of the black window (section 6, 77-79). The first two lines were
  before any switch to 1, so they stand.

**The FIFO thread: what to cut next (static study, 10-19 afternoon; nothing built or run).** The thread is ~93% busy
after CR fields and the bi-switch, so it is the next limiter of the stack. The study read the code at bc48c67
(`perf/fifo-vs-memo`) against prof3's Palace profile (`exe/crf`, 3,715 FIFO samples; 1% of the thread is ~0.085 ms a
frame). prof3's build has neither 40ced98 nor bc48c67, so its `vp_record_cb` c[0] read-back (3.1%) and
`vp_bind_sampler_table` (4.0% inclusive) are already cut, and its `vp_get_vs_mode` share (3.9%, the byte-wise FNV of
the program) was measured with the one-entry memo and with the program version moving on every re-sent word.

| Where the thread goes (prof3) | Exclusive | Inclusive |
|---|---|---|
| `d3d12_draw_indexed` | 8.4% | 38.4%, plus 9.4% in the memcpy of vertex spans into `vf_buf` |
| `render_frame` | 9.9% | 30.0%; 5.8% of the thread is the inlined `tex_csum` loop |
| the FIFO parse (`gcm_rsx_process_fifo_unlocked`, `rsx_process_method`) | 13.0% | spread over 90+ addresses |
| `upload_strip_vp_indexed` (CPU vertex path, strips) | 6.3% | 7.0% |
| `rt_writeback_store` (the byte-swap row copy into guest memory) | 5.7% | 5.8% |
| `rsx_fetch_vertex_arrays` | 5.0% | |
| `vp_get_vs_mode` | 3.9% | |

render_frame's phases (prof3, per frame): store 0.45-0.49 ms, draw-prepass 1.40-1.48 (texture lookups 0.72-0.77 for
~5,630 lookups), VP pass 0.92-0.98, submit 0.09-0.10, present 0.01: 2.95-3.14 ms in all.

Candidates, by saving per day of work (the study's estimates; each gets a live knob that defaults to today's path):

| # | Change | FIFO time saved a frame | Cost | Risk |
|---|---|---|---|---|
| 1 | `tex_csum` in a parallel pre-pass over the frame's unique textures (2-3 workers or the idle vpj worker); every texture is still checked every frame | 0.3-0.45 ms (4-5%) | 40-80 lines | low |
| 2 | render_frame stage 1: the VP pass, submit and Present on one render thread, woken by an event and joined at the next flip; frames with occlusion queries stay inline; two draw lists, one per parity half | 1.0-1.1 ms (12-13%) | ~300 lines, 2-3 days | medium: a race shows as flicker or missing draws |
| 3 | each draw's PSO kept in its record (`vp_get_fp_pso` and `dr_cube_mask` run twice a draw today, `fp_hash_at` then `fp_units_at` compare twice) | 0.1-0.2 ms | 40-60 lines | low |
| 4 | triangle lists and quads (and the strips that fall back) on the GPU fetch path; first a `VP_FETCH_STATS=1 VP_VSTAT=60` run to see which of the 7 fallback tests dominates | 0.5-1.3 ms | 250-350 lines, 3-4 days | medium |
| 5 | `vp_get_vs_mode` with more memo ways (each way compares a copy of the program, so it stays exact); only if a profile of a build with 40ced98 still shows it hot | up to 0.3 ms | 20-40 lines | low |
| 6 | write back only the targets the guest reads (#9 in section 3) | up to 0.45 ms | 1-2 days | high: a missed target is a wrong image in live play only |

Not cheap: the FIFO parse (13%, no single hot spot), the vertex-span memcpy (a guest-change check for vertex data
that skinning and particles rewrite in part). The study's notes, with file:line for every claim, are in
docs/66-fifo-thread-study.md. Rows 1 and 3 together are about 0.5 ms (6%) for a day; row 2 doubles that. In the stack, where
the FIFO thread holds the frame, a cut on it shows in frames until image 2 (~85%) or HighGraphics saturates again.

**What the profile's counts show (deep1; nothing changed from them yet).** Besides feeding the use build, the profile
counts how often every block ran in those 5 minutes (about 24,000 frames of the instrumented exe). A function's
hottest block, set against its call count, shows its loops. Shares are prof3's sampled exclusive time on that thread.
The counts are per IR block before vectorization, so a vectorized loop does 4 to 8 of them per pass.

| Function | Hottest block | Calls | Per call | Where, and its share | What the loop is |
|---|---|---|---|---|---|
| `wb_conv_row` | 81.6e9 | | | the FIFO thread, in `rt_writeback_store` (5.8%) | the write-back's word loop, ~3.4M words a frame (candidate 6 above) |
| `ps3_hle_call` | 30.4e9 | 295.6M | ~103 | HighGraphics 1.1%, main 0.4% | `ppu_hle.cpp:786`: a linear scan of the context-handler list on every HLE call, before the NID table |
| image 11 (`spu_0010_at_01818080`), region `0x3A8` | 12.2e9 | | | the shared SPU pool threads (15-20% of a core each, mostly waiting) | image 11's main loop; not a core-wide spin |
| `gcm_note_spu_put` | 11.2e9 | 698M | 16 | image 2 1.4% | `gcm_ea2io_range` scans all 16 IO mappings for every SPU PUT, and most PUTs are not to RSX memory, so they scan to the end |
| `rsx_fetch_vertex_arrays` | 9.6e9 | | | the FIFO thread 5.0% | the per-vertex convert |
| `vp_get_vs_mode` | 5.9e9 | | | the FIFO thread 3.9% (before 40ced98) | the byte-wise FNV of the program |
| `spu_coh_notify_write` | 5.9e9 | 738M | 8 | HighGraphics 3.3% | checks all 8 SPU contexts' reservations on every call |

The three scans (`ps3_hle_call`, `gcm_ea2io_range`, `spu_coh_notify_write`) are cheap to cut: a hash or direct index
for the handler list, a bounding range of the active mappings checked before the scan, and a count of valid
reservations checked first. Each is 1-3% of one thread, so below the bar alone; they are listed for a later batch
(item 1 of the decisions in the report, its small trims).

## 7. Where the multi-day jobs start (for 10-20)

Both take more than an afternoon, so neither was started on 10-19. **Order changed that evening:** with the batch
in, image 2 holds the frame (stackab2) and the bi-switch pays (stackk5), so image 2's ring loops come first and the
render thread second. The evening's static work is at the end of this section.

**render_frame's second half on a render thread** (the FIFO study's Stage 1, docs/66-fifo-thread-study.md Q1): 1.0-1.1 ms
of render_frame's 2.95-3.14 ms (12-13% of the FIFO thread), about 300 lines and 2-3 days. The batch above already
keeps each draw's pipeline in its record, which this needs.
- The split: the FIFO thread keeps the GPU wait and the deferred write-back store, setup, the target prepass and the
  draw prepass. One render thread, woken by an event (no spinning), runs the VP pass and its post,
  `rt_writeback_record` and `ResolveQueryData`, Close and Execute, Present and `move_to_next_frame`. At the next flip
  the FIFO thread waits for its done event, then runs `wait_for_gpu`, the store and `zq_finish`. Knob `RSX_RF_THREAD`
  (0 = today's inline path), read at flip.
- What changes with it: two draw lists by parity (the FIFO thread fills frame N+1's while the render thread reads frame
  N's); `s_composite_src` snapshotted at the hand-off, with the end-of-frame resets kept on the FIFO thread; frames that
  issued occlusion queries (`s_zq_n > 0`) run inline, because the guest may read a report right after the flip; the
  guest reads at flip time (texture checksums, fragment ucode, target checksums) and the live state reads stay on the
  FIFO thread.
- The swap chain's locking against the resize path and `d3d12_present`: no new lock is needed. Every swap-chain call
  is in render_frame's tail (or init and shutdown), so they all move with Present, and the next frame joins the render
  thread first (docs/68 section 3.2).
- Measure: `RSX_RF_THREAD=0` as the B line of an in-boot A/B in the stack, then a cross-build A/B. A race shows as
  flicker or missing draws, so regress at 30 and 240 fps and a look at a live run.

**docs/48's native ring loops for image 2 (its Stages 0, 2 and 3).** First, since the evening of 10-19: with the batch
in, image 2 holds the frame (stackab2), and Stage 1, the bi-switch, pays (stackk5, -4.53% with it off). docs/48
expects +8-14% where image 2 sets the frame; docs/60 maps Stage 2 to edits and expects +3-5% from it alone (at most
+6-8%).
- Stage 0 first: `SPU_RING_HIST` (a command histogram, the 0x3C90 slow-path rate, the context's image fields), a fresh
  profile of image 2 and HighGraphics, and the causal slope again.
- Stage 2: `spu_hook_ring_post` as a native block at 0x4808-0x4818, doing 0x3C90's fast path in C (knob
  `SPU_NATIVE_POST`). No lifter change.
- Stage 3: a native steady-state loop inside the fetch block (0x4618-0x4730) that does each hot inline command whole
  (knob `SPU_NATIVE_LOOP`).
- docs/48's record and replay harness checks each stage against recorded all-lifted batches before any game run.

### Added on the evening of 10-19: the static work, mapped to edits

During the evening's runs, up to eight agents did static work (no builds, runs, indexing or fetches while an A/B ran).
Each wrote a doc of its own. The ones with a patch now have a local branch from master: committed, not pushed, no PR,
and not built unless the table says so.

**What each doc settles:**
- **docs/67, the FIFO thread's waits.** The thread waits about 10% of the time, not 20%, and it is one wait: the 1 ms
  kick wait, which nothing sets in Demon's Souls. The frame is latency-bound on the HighGraphics and image 2 hand-off:
  HighGraphics waits 1.35 ms a frame for image 2's reply to command 35, then image 2 idles 1.2-1.45 ms until
  HighGraphics writes the next frame's first command, so 2.6-2.8 ms of the 7.2 ms frame runs only one of the two. Its
  first fix, `FIFO_PUT_WAKE`, is measured above (no gain now). Its second, HighGraphics waiting on the reply word
  instead of its 64-poll timer fallback, is not written yet (about a day).
- **docs/68, the render thread.** The cut is at `RF_CP(5)`. The FIFO thread keeps everything up to the legacy pass,
  then runs a hand-off: it snapshots the batch, runs the end-of-frame resets and the bookkeeping that reads FIFO-time
  state, and wakes one render thread. The render thread runs the VP pass, the post tail, submit, the dumps, Present,
  `move_to_next_frame` and `swap_resize`, and the next render_frame joins it first (knob `RSX_RF_THREAD`, read at the
  flip; 0, the default, is today's path). All in `rsx_d3d12_backend.c`: about 200 new lines, 40 edited and 300 moved
  as they are, about 2 days, and a day more if query frames need the fallback (its Stages 1a and 1b).
  - **The swap chain needs no lock.** Every swap-chain call is in render_frame's tail, or in init and shutdown, so they
    all move with Present and the join orders them. `d3d12_present` reaches the swap chain only through render_frame,
    and the window thread never calls DXGI: it publishes the size with an Interlocked store.
  - Beyond docs/66: the per-parity draw lists change what `s_d3d.draws` means for three readers after the hand-off;
    the render thread must never call `vp_get_fp_pso` (it reads guest fragment ucode and a table that `vp_vs_insert`
    reallocates), so the VP pass's fallback lookup moves into the hand-off; `pso_lib_tick` and the titlebar block stay
    on the FIFO side.
  - Order: Step 0, a Palace run with `ZPASS_LOG` and no build, counts the occlusion-query frames, which must run
    inline; Step 1, the refactor with no thread (regress must not change); Step 2, the thread; Step 3, the A/Bs. The
    main risk is many query frames, which would cancel the gain; docs/66's 0.1 ms for submit and queries suggests few.
- **docs/60, image 2's next cut.** Stage 2 mapped to exact edits, not yet a patch: `spu_hook_ring_post`, a native block
  at 0x4808-0x4818 that does 0x3C90's fast path in C (knob `SPU_NATIVE_POST`: 0 off, the default; 1 on; 2 check
  mode). It meets the bi-switch at loc_00004808, and each works without the other. Expected: likely +3-5%, at most
  +6-8%. The SPU PUT trim is a ready patch: one bit per 1 MB page, so a PUT that lands in no IO mapping skips the
  16-slot scan (`GCM_SPU_PUT_FILTER`, live: 1 on, the default; 0 the old scan; 2 scan and report a mapped PUT whose
  bit is clear). Expected +0.5-1.5%, so it needs the in-boot knob A/B. It merges with `perf/fifo-put-wake` either way.
- **docs/61, two small trims, both ready patches.** The HLE dispatcher finds a NID's handler through a hash index
  beside each table instead of two linear scans (`PS3_HLE_HASH`, live; 1.1% of HighGraphics). The lock-line notify
  skips the SPU contexts whose last reservation is another line (`SPU_COH_HINT`, live; 3.3% of HighGraphics). The
  notify trim carries the risk: a missed notify can freeze the game. docs/61 section 2.4 argues none is missed.
- **docs/62, the cutscene camera's Euler flip.** Confirmed in a run above (88).
- **docs/63, the A/B tools.** The perf lane's tools as repo tools in `tools/des`: `exeab.sh` (cross-build),
  `knobab.sh` (live knobs) and `regfps.sh` (regress per frame rate) on a shared `perflib.sh`, with `quiet.ps1`,
  `wincap.ps1` and the stats scripts; `tools/des/PERF.md` says how to run them. No machine paths, saves or game files;
  `RSX_VSYNC=0` in uncapped runs; a PrintWindow check of the window after each in-boot window, which stops the batch on
  a black level. The default vblank is 960000, since the pinned save loads the Tutorial (past 300 fps); the Palace
  takes `PERF_SAVE=<dir> PERF_MAP=m02`. Not run from the repo yet.
- **docs/64, GPU fetch for triangle lists and quads.** Mapped to edits, no patch. Lists and quads alone save about
  0.15-0.25 ms of the FIFO thread (+2-3% where it holds the frame). Most of the CPU vertex path's cost is strips that
  fall back (about 1.15 ms); copying their vertices to the GPU (step 7) could save 0.6-0.85 ms, if a stats run (step
  0) shows which test sends them back. 3.5-4 days in all, knob `VP_FETCH_LISTS`.
- **docs/65, three lifter gaps.** All three confirmed and fixed, with tests: `bdnzf`-style branches were always taken
  and never decremented CTR; FP record forms never set CR1; `mcrfs` had no handler. More gaps are listed there, not
  patched. Any lifter change needs a re-lift and a regress run on every title before it merges.

**The branches** (each from master: ps3recomp 4a76fc0, port 53a047a):

| Repo | Branch | Tip | What | State |
|---|---|---|---|---|
| ps3recomp | `perf/fifo-vs-memo` | e1119c8 | the memo fix | PR #72 |
| ps3recomp | `perf/bi-switch` | 331e100 | the bi-switch | PR #73; measured (stackk5) |
| port | `perf/bi-switch` | 93ba9c4 | image 2 lifted with it | PR #83 |
| ps3recomp | `perf/spu-ring-hist` | a44df7d | docs/48 Stage 0, the ring histogram | local; not built (later #78) |
| ps3recomp | `perf/fifo-put-wake` | cc0ee92 | docs/67's first fix | local; built and measured, no gain now (later #79) |
| ps3recomp | `perf/spu-put-io` | 296dfa2 | the PUT trim (docs/60) | local; not built (later #74) |
| ps3recomp | `perf/hle-dispatch-table` | 3ee0611 | the HLE trim (docs/61) | local; not built (later #75) |
| ps3recomp | `perf/coh-notify-mask` | fbd0957 | the notify trim (docs/61) | local; not built (later #76) |
| ps3recomp | `fix/lifter-bdnz-cr1` | 45ceeed | the lifter gaps (docs/65) | local; not built, the Python checks pass (later #77) |
| port | `fix/cut-euler-blend` | 796565b | the cutscene blend (docs/62) | local; built and run once (88) (later #84) |
| port | `tools/ab-perf` | 63a85b2 | the A/B tools (docs/63) | local; tooling, not run from the repo yet (later #85) |

**The review of the four perf PRs** (ps3recomp #69, #70, #71 and port #82; static, before the merges): no crash or
wrong-image bug in them apart from the memo bug (now #72). It found:
- **A build break outside them.** The unmerged port branch `feat/mkb-layout` (1aaa0b7, 12 commits not in master, no
  PR) still uses `ctx->cr` in `port/des_options.cpp` (its own copy of the DES_WARP code) and `port/des_menu_sfx.cpp`.
  Since #69 the context has `crf[8]` in its place, so the branch no longer builds against ps3recomp master, and a
  DES_DEV=ON build would also define `des_debug_tick` twice. The main tree's uncommitted scratch/snag changes have the
  same `ctx->cr` uses. Fix: rebase onto master, drop the DES_WARP copy, and use the CR helpers as #82 did.
- **#71, performance only, theoretical.** Cancelling `tcj_dispatch` does not stop workers already in their loop: they
  run to a copy of `n` taken under the lock. The FIFO thread then spins until they finish the previous frame's jobs,
  whose sums nothing reads. Normally the wait is zero; a preempted worker and a short VP pass make it show. Fix:
  re-read `n` (or a generation number) on each job.
- **Pre-existing, not regressions:** the three lifter gaps (now `fix/lifter-bdnz-cr1`, docs/65), and the texture
  checksum taken after the conversion (`libs/video/rsx_d3d12_backend.c`, the texture upload): a texture the guest
  writes while it converts keeps the old image until its bytes change again.
