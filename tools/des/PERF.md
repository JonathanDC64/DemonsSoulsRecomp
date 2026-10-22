# Perf A/B tools

How to measure a frame-rate change. The method and why it is built this way:
docs/63-ab-tools.md. Every command below is one line of Windows PowerShell 5.1,
run from the repository root; the bash scripts go through Git Bash.

## Before a batch

- The batches play from the pinned regression save, `work/tmp/regress/save`
  (git ignores `work/`). Only if there is none yet, pin the current save there
  (with `DES_SCRATCH` unset): `& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/regress.sh pin-save"`.
  Pinning replaces the pinned save, and regress.sh's accepted set belongs to
  that save, so do not re-pin just to run a batch.
- Close the game. A batch refuses to start while any des_boot.exe runs, and it
  stops before the next boot if one appears: it never stops a game it did not
  start.
- Keep the PC quiet: no build, no game, no video. Before the batch and before
  each boot the total CPU must be at most `PERF_QUIET_MAX` % (default 30); the
  batch waits up to `PERF_QUIET_WAIT` s (default 900) for that, then stops.
- Locked or not: while the Windows session is locked the batch renders without
  presenting (`RSX_PRESENT=0`), otherwise every run reads about 4 fps. It
  decides once per batch, and the header says which; if the session locks
  under a presenting batch, the batch stops. Set `RSX_PRESENT` to pin it
  yourself.
- Every batch copies the user's save aside once (`<batch>/usersave_bak`, never
  removed) and checks it is unchanged after every boot; levelcap.sh and
  regress.sh swap the pinned save in and put the user's back for each run.

Output goes to `$DES_SCRATCH/<tag>/` (default `work/tmp/<tag>/`); a tag that is
already there is refused. Exes are paths, absolute or relative to the repo root.
exeab.sh and knobab.sh exit 0 when the batch ran through, 2 on bad arguments, 3
when a des_boot.exe was running or the save folder was not free, 4 when the PC
stayed busy or the session locked, 5 when the user's save changed and (knobab.sh)
6 when the window check stopped it.

## Two builds: exeab.sh

`exeab.sh <tag> <exeA> <exeB> [rounds [ENV...]]` alternates the builds, A B
then B A (6 rounds by default: 6 boots a side), uncapped at the pinned save's
spawn (the Tutorial, `m08`, past 300 fps): `DES_FPS=60 VBLANK_MHZ=960000
RSX_VSYNC=0`, normal priority on all CPUs. Each boot takes two 8 s profile
windows with 10 s of nothing after each. A boot with a window within 10 % of
the vblank rate is marked invalid (`runs.txt`): the vblank must stay well above
the frame rate. For the Palace, give a Palace save: `PERF_SAVE=<dir>
PERF_MAP=m02` (240000 is enough there, at about 120 fps).

    & "C:\Program Files\Git\bin\bash.exe" -c "tools/des/exeab.sh ab_fifo work/tmp/exe_base/des_boot.exe port/build/des_boot.exe"

Read `abstat.txt`: per side the median and spread, per-round deltas, the paired
p and the verdict, for the profile fps (bar 2.8 %) and the gap fps between
windows (bar 4.5 %). A change counts only if it clears the bar, p < 0.05 and B
is on the same side in all rounds but one. `abthreads.txt` has the per-thread
busy shares. An A/A batch (the same exe twice) shows the noise of the day.

A knob on one exe: give the exe twice and the knob to one side,
`EXEAB_ENV_B="SOME_KNOB=1"` (`EXEAB_ENV_A` for side A). Unlike knobab.sh this
gives the gap fps too, which matters for a change on the FIFO thread: the
profile session loads that thread, so a profile window can show a gain the
gap does not.

## Live knobs in one boot: knobab.sh

`knobab.sh <tag> <KNOB=value[,KNOB2=value] | knobs-file> [ENV...]` on
`DES_EXE` (default port/build/des_boot.exe): ABBAAB 6 s windows per knob line
(`KAB_ORDER` takes pairs of AB or BA), A with the knobs cleared, B with them
set; `KAB_BOOTS` boots (default 1).

    & "C:\Program Files\Git\bin\bash.exe" -c "KAB_BOOTS=3 tools/des/knobab.sh kab_try SOME_KNOB=1"

A knobs file has one comparison per line (`KNOB=value` or
`KNOB=value,OTHER=value`; `#` comments). Read `knobstat.txt`: per line the
B-A change in fps and frame time, the paired p over the A/B pairs, and the
verdict against `PERF_BAR_KNOB` (default 1.5 %).

The window check: after each window the game window is saved once
(`shots/`, `wincheck.txt`) in the 2 s gap, with PrintWindow. If the level is
black the boot and the batch stop (`runs.txt` says "level went black"): a
knob switch that breaks rendering must not read as a gain. If the first
capture fails, the batch stops as well ("window check failed"); `PERF_WINCHECK=0`
runs without the check. It is off while `RSX_PRESENT=0`.

## Regression at several frame rates: regfps.sh

`regfps.sh <tag> [fps ...]` runs regress.sh on `DES_EXE` once per rate (default
30 and 240; above 60 with `RSX_VSYNC=0`) from a copy of the pinned save and
the accepted set, and exits with the worst result.

    & "C:\Program Files\Git\bin\bash.exe" -c "DES_EXE=port/build/des_boot.exe tools/des/regfps.sh reg_fifo 30 240"

## One window capture: wincap.ps1

Saves one des_boot window's client area with PrintWindow and prints its mean
brightness (0-255); `-Every <s>` keeps saving until the game exits. The pid is
in the run's pid file (`des_boot.pids` in its DES_SCRATCH).

    powershell -NoProfile -ExecutionPolicy Bypass -File tools/des/wincap.ps1 -ProcessId 12345 -Out work/tmp/shot.png

## Settings

| Variable | Default | Used by |
| --- | --- | --- |
| `PERF_SAVE` | `work/tmp/regress/save` | exeab, knobab: the save to boot (levelcap's `LC_SAVE`) |
| `PERF_MAP` | `m08` (only with the pinned save) | exeab, knobab: the map each boot must load |
| `PERF_VBLANK` | 960000 | exeab, knobab: `VBLANK_MHZ`, well above the frame rate |
| `EXEAB_ENV_A`, `EXEAB_ENV_B` | unset | exeab: `VAR=value` words for one side only |
| `PERF_SETTLE` / `KAB_SETTLE` | 40 | seconds after the level starts loading |
| `KAB_WIN`, `KAB_ORDER`, `KAB_BOOTS` | 6, ABBAAB, 1 | knobab |
| `PERF_BAR_PROF`, `PERF_BAR_GAP`, `PERF_BAR_KNOB` | 2.8, 4.5, 1.5 | the noise bars, % |
| `PERF_QUIET_MAX`, `PERF_QUIET_WAIT` | 30, 900 | the quiet check (`PERF_QUIET=0` skips it: try-outs only) |
| `PERF_QUIET_OURS`, `PERF_QUIET_OUT` | unset | the share outside the batch's processes (local.env) |
| `PERF_BLACK`, `PERF_WINCHECK` | 2, 1 | the window check |
| `RSX_PRESENT`, `RSX_VSYNC` | lock state, 0 | pinned per batch, written in `batch.txt` |
| `REGRESS_SRC` | `work/tmp/regress` | regfps: where the pinned save and accepted set are |
