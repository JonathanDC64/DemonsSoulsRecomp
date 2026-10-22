# 63. A/B and window-check tools

Status: 2026-10-19, merged as port #85. A tooling patch that brings the
perf lane's scratch A/B tools into `tools/des/`. How to run them is in
`tools/des/PERF.md` (part of the patch); this note says what each tool does, the
measuring method, what changed from the scratch versions, and what is left.

## Why

The Palace perf work (docs/56) was measured with scripts written for one PC and
one session: hard-coded paths, a slot lock in the session's scratch directory, a
CPU check that knew the agents' process names, and a frozen copy of the pinned
save. Three things went wrong with them on 2026-10-19 that the repo versions
guard against:

- **Vsync.** The runtime presents with vsync by default (`RSX_VSYNC=1`, ps3recomp
  `libs/video/rsx_d3d12_backend.c`, the present path). None of the perf scripts
  set it, so with `RSX_PRESENT=1` an "uncapped" run was held near the display's
  refresh (about 143 Hz on the test PC) and could not show a gain past it. The
  repo versions set `RSX_VSYNC=0` for uncapped runs (the backend falls back to
  vsync and logs `RSX_VSYNC=0: tearing is not supported` when the system has no
  tearing support; a boot that logs it while presenting is marked invalid) and
  write `RSX_VSYNC` and `RSX_PRESENT` into each batch header. An `RSX_VSYNC` in
  the environment pins the V-sync setting (`port/ui/src/settings.cpp`), and
  test runs read no settings.ini, so nothing overrides it.
- **A black window read as a win.** A live knob switch left a stale vertex
  shader, the level went black, and the in-boot A/B counted the cheaper black
  frames as a gain. The in-boot A/B now saves the game window once after each
  measured window (in the 2 s gap, outside the timed window) and stops the boot
  and the batch if the level is black.
- **Captures.** One capture with `CopyFromScreen` caught another window. All
  captures use `PrintWindow` on the game's own window (by process id).

## The tools

| Repo file | From (scratch) | Relation | What it does |
| --- | --- | --- | --- |
| `tools/des/perflib.sh` | `perflib.sh` | new (shared) | Batch helpers on top of `deslib.sh`: refuse while any des_boot.exe runs, CPU quiet check, user-save snapshot and check (under the save-folder lock), cutscene skip, run validity, lock-state `RSX_PRESENT`, `RSX_VSYNC=0` for uncapped runs, batch header, window capture. |
| `tools/des/quiet.ps1` | `quiet.ps1` | new | Total CPU over N seconds (GetSystemTimes, not a localized perf counter) and the busiest processes; optionally the share outside a list of process names. |
| `tools/des/exeab.sh` | `perfab.sh` | replaces the body, same job and arguments | Cross-build A/B: alternating boots (AB, BA, ...), 6 rounds by default, two 8 s profile windows a boot plus the unperturbed gap after each, quiet and save checks, validity, `abstat.py` verdict. |
| `tools/des/knobab.sh` | `perfknob2.sh` | extends, same arguments plus a knobs file | In-boot A/B of live knobs: ABBAAB windows per knob line, several knobs per line, several boots (`KAB_BOOTS`), window check after every window, `knobstat.py` verdict. |
| `tools/des/abboot.sh` | (none) | small change | Presses through a cutscene while it settles when the caller sourced `perflib.sh` (knobab.sh); the idle time comes from `AB_IDLE` (default 1500 s, as before) so long knob lists fit; `kprof.sh` is unchanged. |
| `tools/des/regfps.sh` | `perfreg2.sh` | new (sits beside `regress.sh`) | `regress.sh` once per frame rate (default 30 and 240) on one exe, from the pinned save, in a scratch copy of the regress directory, with the batch save checks. |
| `tools/des/abstat.py` | `abstat.py` | new | Per side: medians, spread, per-round deltas, paired t on the round deltas, Welch t on boot means, for profile fps and gap fps, against separate noise bars. |
| `tools/des/knobstat.py` | `knobstat.py` | new | Per knob line: A and B fps, the B-A change in fps and frame time paired over the adjacent A/B windows of each boot, paired p, bar. |
| `tools/des/abthreads.py` | `abthreads.py` | new | Per-thread busy share (HighGraphics, FIFO, image 2, main, cores busy, syscall share) per boot from the profile sessions of an exeab.sh batch, side means, B-A, paired p. |
| `tools/des/wincap.ps1` | `pwloop.ps1` + `darkcheck.py` | new (replaces both) | Saves one des_boot window's client area with `PrintWindow` (or one every `-Every` s until the game exits) and prints its mean brightness. |
| `tools/des/PERF.md` | - | new | How to run them (single-line PowerShell). |
| `tools/des/local.env.example` | - | small change | Commented `PERF_QUIET_*` entries for the optional per-PC part of the quiet check. |
| `tools/des/kabsum.sh`, `kprof.sh`, `levelcap.sh`, `regress.sh`, `deslib.sh` | - | unchanged | kabsum.sh still reads the `<tag>.out` files (an extra boot column does not bother it); levelcap.sh and regress.sh do each run's save swap under the PR #75 lock. |

Why each one lands where it does:

- **exeab.sh replaced in place.** perfab.sh does exeab.sh's job (two builds,
  alternating boots, profile windows at one spot) with more care; two
  cross-build tools would split the method, so exeab.sh keeps its name and
  arguments and gets the better body. Visible to a caller: the default rounds
  (2 to 6), the order (A B A B to A B / B A), `RSX_VSYNC=0`, and the output in
  a batch directory.
- **knobab.sh extended.** perfknob2.sh is knobab.sh with knob lists, several
  boots and checks. The repo version keeps knobab.sh's own flow (abboot.sh, the
  script sends each console command) rather than perfknob2.sh's command file,
  because the window check has to run between windows.
- **regfps.sh beside regress.sh.** It only loops regress.sh over frame rates
  with the batch checks; regress.sh's route, shots and save handling are left
  as they are.
- **perflib.sh new, deslib.sh unchanged.** deslib.sh is the per-run library
  (staging, pid files, the save lock); the batch logic (quiet PC, save list,
  validity, header, capture) is shared by three batch scripts and has no place
  there yet.
- **quiet.ps1, wincap.ps1 new.** Nothing in the repo measures the whole PC's
  CPU (cpusample.sh samples des_boot itself) or captures a window.
- **abstat.py, knobstat.py, abthreads.py new; kabsum.sh kept.** kabsum.sh stays
  the quick mean of a `.out` file; the new scripts add the spread, the paired
  tests, the gap fps and the per-thread shares.

Not brought over:

- `grab2.sh` / `withslot.sh` (the slot lock that kept runs and builds apart) and
  `perflock.sh` (the lane's build lock). The repo already has a run lock: the
  save-folder lock from port PR #75 (`des_save_lock` in `deslib.sh`), which
  `levelcap.sh` and `regress.sh` take for every run, so no two game runs
  overlap. Builds are not locked; instead every boot starts only after the
  quiet check passes (a build is far above the bar). The tools take no build
  lock because they never build.
- `perfprof.sh` (the deep profile run). It needs three scratch-only helpers
  (`tslack.exe`, built from a C file, `chain.py`, `mkorder.py`) and a save other
  than the pinned one. `kprof.sh` covers per-thread profiling in the repo, and
  `abthreads.py` gives per-thread shares across an A/B. See "What is left".

## The method

- **Where:** by default the pinned regression save (`work/tmp/regress/save`,
  git-ignored), which loads the Tutorial (map `m08`, about 800 draws a frame,
  300+ fps uncapped; docs/56). docs/28's Palace numbers (about 120 fps) come
  from a deep Palace save at Palace 1-1 (`m02`), which is not in the repo flow:
  `PERF_SAVE=<dir> PERF_MAP=m02`. Standing still, uncapped: `DES_FPS=60
  RSX_VSYNC=0`, `PS3_PRIORITY=normal PS3_AFFINITY=all`, through the title with
  `DES_AUTO_CONTINUE=1`, 40 s after the level starts loading.
- **Vblank:** it must stay well above the frame rate measured (at 120 Hz a
  +1.9% change read +0.9%). `VBLANK_MHZ=240000` (docs/28) is enough at the
  Palace, but at the Tutorial spot every window read 240.0 fps (docs/56), so
  the default is 960000, which caps neither spot. A boot with a profile window
  within 10% of the vblank rate is marked invalid.
- **Across builds (exeab.sh):** alternate the exes, A B then B A, so a drift
  over the batch cancels; 6 rounds, so 6 boots a side. Each boot takes two 8 s
  profile windows, each followed by 10 s with nothing armed. Two numbers per
  window: the profile session's own fps (it times every lookup and syscall, so
  it reads low) and the gap fps from `FRAME_LOG` (every present, unperturbed).
  The boot (mean of its windows) is the unit: windows in one boot are not
  independent.
- **Statistics (abstat.py):** per side the median and spread over windows and
  over boots, per-round deltas, the median B vs A, the paired t on the round
  deltas and a Welch t on the boot means. A change counts when the median delta
  clears the bar, the paired p is under 0.05, and B is on the same side in all
  but at most one round. Bars across builds: **2.8% for profile fps and 4.5%
  for gap fps** (boot-to-boot spread of one build). Override with
  `PERF_BAR_PROF` / `PERF_BAR_GAP`.
- **Inside one boot (knobab.sh):** both sides share the boot, so the
  boot-to-boot spread drops out. ABBAAB windows of 6 s per knob line, paired
  over the adjacent A/B pairs. Bar about **1-1.5%** (`PERF_BAR_KNOB`, default
  1.5). Use it for any change that has a live knob.
- **A quiet PC:** total CPU at most `PERF_QUIET_MAX` % (default 30) before the
  batch and before each boot, with no game running; the batch waits up to
  `PERF_QUIET_WAIT` s (default 900) and then stops. Nothing else heavy while a
  batch runs; the background CPU before each boot is in `runs.txt`.
  `PERF_QUIET=0` skips the check, for try-outs only.
- **RSX_PRESENT** is pinned once per batch from the lock state: 0 while the
  Windows session is locked (otherwise every frame waits on DWM and every run
  reads about 4 fps), 1 otherwise. A batch never mixes the two: if the session
  locks under a presenting batch, the boot that saw it is marked invalid and
  the batch stops before the next.

## Safety

- **Saves.** No save or game file is in the repo. `levelcap.sh` / `regress.sh`
  already copy your save aside, swap in the pinned one under the save
  lock and put yours back with a check. On top of that each batch copies
  your save once to `<batch>/usersave_bak` (it refuses if that exists and
  never deletes it) and takes an md5 list, both under the save lock, and after
  every boot compares the save with the list (under the lock, so another run's
  swap is not mistaken for a change); a difference stops the batch. Nothing
  `cd`s into the save folder or removes it.
- **Processes.** A batch refuses to start while any des_boot.exe runs, and
  stops before the next boot if one has appeared (you started playing). It
  stops no game itself: when it exits it ends only the levelcap.sh (or boot)
  it started, whose own trap stops that run's game (`des_kill_own` / the pid
  file) and puts the save back.
- **Logs** stay quiet (`PS3_VERBOSE=0` from deslib); the batch log has a few
  lines per boot (quiet check, start, validity and fps, save check) plus the
  verdict; the game's own output stays in each boot's `run.txt`. Waits are on
  conditions (the level loading, the boot ending, the console reply); the
  quiet check polls every 30 s.

## Changes from the scratch versions

- Paths: the repo root comes from the script's location (`deslib.sh`), output
  goes under `$DES_SCRATCH/<tag>/` (default `work/tmp/<tag>`), exes are
  arguments (`DES_EXE` for knobab.sh and regfps.sh). No frozen save copies:
  `PERF_SAVE` (default the pinned save) is passed to levelcap as `LC_SAVE`.
  exeab.sh and knobab.sh keep their arguments; their `<tag>.out` moves from
  `$DES_SCRATCH/` into the batch directory.
- No slot lock: the batch relies on the save-folder lock that every
  levelcap.sh / regress.sh run takes, plus the checks before each boot (no
  des_boot.exe running, the session not locked under a presenting batch, a
  quiet PC).
- `RSX_VSYNC=0` on uncapped runs; `RSX_VSYNC`, `RSX_PRESENT`, `VBLANK_MHZ`, the
  repo commit and the exe md5s in each batch header; the no-tearing fallback
  makes a presenting boot invalid.
- Default vblank 960000 as in the lane (240000 caps the pinned save's Tutorial
  spot), plus the near-cap check; default rounds 6 (was 4). exeab.sh's run cap
  grows with `PERF_SETTLE`, and a boot with fewer than its 2 windows is marked
  invalid.
- One noise bar per metric (2.8% / 4.5% / 1.5%) instead of one 0.8% bar.
- The quiet check counts total CPU with GetSystemTimes (the perf counter path
  is localized on non-English Windows) and no longer knows this PC's agent
  processes: the "outside" share is opt-in (`PERF_QUIET_OURS`,
  `PERF_QUIET_OUT`, e.g. in `local.env`).
- Save snapshot and check through `deslib.sh` (`des_save_files`,
  `des_backup_save`) under `des_save_lock`, instead of `cd` + `md5sum`.
- knobab.sh keeps the abboot.sh flow (the script sends the console commands
  itself) so it can capture the window between windows; perfknob2.sh handed a
  command file to levelcap and could not.
- The window check folds `pwloop.ps1` and `darkcheck.py` into one capture:
  client area only (no title-bar crop that depends on DPI), DPI-aware, console
  windows skipped (Windows reports a console window as the attached program's),
  brightness computed in the same process (no PIL), black below `PERF_BLACK`
  (default 2 of 255). A minimized window has no client area: it is logged, and
  on a boot's first window it stops the batch like any failed capture. It is
  off when `RSX_PRESENT=0` (nothing is presented, so the window holds no
  frame) and the header says so.
- regfps.sh needs no `sort -V` fix: master's regress.sh already takes its shots
  through `regress_route.py`. Each rate runs with its own `DES_SCRATCH`, so
  regress.sh's backup of your save (in its `regress_run`) is never
  removed by the next run. Rates other than 30 run with `RSX_VSYNC=0`, so
  "240" runs at 240 and not at the display's refresh.
- knobstat.py drops the causal-slope columns (`TMP_SLOW_*` knobs live only on a
  scratch branch). knobab.sh takes only orders made of AB / BA pairs (other
  orders give knobstat.py no pairs). abthreads.py skips boots that are not
  valid in `runs.txt`, as abstat.py does.
- From a static review of the patch: the "is a game running" check reads all
  of tasklist's output before testing it (`| grep -q` under `pipefail` can
  read false while the game runs); a cutscene counts as open by the last
  `DES_FPS: cutscene` line, not by counting starts against ends (a
  `DES_CUTFPS` change logs a second start); knobab.sh runs each boot in the
  background with an EXIT trap, so stopping the batch ends the boot and puts
  the save back; abboot.sh's idle time comes from `AB_IDLE` (default 1500 s as
  before; knobab.sh sets it to the run cap) and the boot stops sending
  commands once its run has ended; exeab.sh and knobab.sh exit non-zero when
  they stop early (PERF.md lists the codes); regfps.sh only raises its exit
  status.

## What is left

- The tools have been used since: `exeab.sh` in docs/68 section 7.3, and
  `knobab.sh` for the A/Bs of docs/80. An A/A batch (`exeab.sh` with the same
  exe twice) to confirm the bars on a new build is still worth a run.
- `wincap.ps1` changed from the tested scratch capture (client-area crop, DPI
  awareness, in-process brightness); check one capture by eye. If the first
  capture of a boot fails (no window, PowerShell error) the batch stops
  ("window check failed"; `PERF_WINCHECK=0` runs without the check); a later
  failed capture is logged as "no capture" in `wincheck.txt` and does not stop
  it. Each capture starts PowerShell and compiles its helper (about 1-2 s,
  inside the 2 s gap; the next window waits if it takes longer).
- The near-cap check reads the profile-window fps, which the profile session
  lowers; a gap fps held by the vblank while the profile fps stays under 90%
  of it would pass. 960000 keeps both far below the cap at both spots.
- regfps.sh runs regress.sh in the foreground: if regfps.sh is stopped, the
  regress.sh run goes on to its end (and puts the save back itself).
- The window check covers knobab.sh only; exeab.sh boots run a command file
  inside levelcap.sh and are not captured.
- With `RSX_PRESENT=0` (locked session) there is no window check. The runtime's
  `CELLMARK_DUMP_NOW=1` frame dump (what regress.sh uses) would work without
  presenting.
- `perfprof.sh` stays in scratch until `tslack`, `chain.py` and `mkorder.py`
  have a home in the repo.
- Builds are not locked out of a batch; the quiet check catches a build only
  when it is running at the check.
- desrun.sh (under levelcap.sh) still clears every staged test copy
  (`des_kill_own`) when it launches. A batch checks that no des_boot.exe runs
  just before, and other levelcap.sh / regress.sh runs wait on the save lock,
  so only a bare desrun.sh run started in those few seconds could be caught.
  Your own game (play.sh's copy, or the exe started by hand) has no
  test-run marker and is never stopped.
- play.sh does not take the save-folder lock (true of every levelcap.sh /
  regress.sh run, not new here). A game started while a boot has the pinned
  save swapped in plays the pinned save, and the boot's restore then puts the
  user's save back over that play. The batch stops before its next boot when
  it sees the game; play.sh waiting on the lock would close the gap.
