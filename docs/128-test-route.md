# 128. The test route inside the game (DES_ROUTE)

`DES_ROUTE` walks a fixed route through a level from inside the game, on the game's own clock. It saves named
screenshots and writes a frame-time summary. It is the first part of the tester kit (docs/120, phase 4): testers on
Linux and macOS can run it with no Python and no debug console. `tools/des/regress.sh` can use it too
(`REGRESS_ROUTE=game`).

## How to run it

| Knob | What it does |
|---|---|
| `DES_ROUTE=regress` | plays regress.sh's route: the spawn point, a walk, a turn, a walk on, a shot after each |
| `DES_ROUTE=look` | the tester kit's: no walking; a shot, three turns of the camera with a shot after each, then a pan |
| `DES_ROUTE=<file>` | plays a route file (below) |
| `DES_ROUTE_OUT=<folder>` | where the results go (default `des_route` in the current folder) |
| `DES_ROUTE_QUIT=1` | ends the game when the route is done |
| `DES_ROUTE_WAIT=<s>` | wall seconds to wait for the level before giving up (default 600) |
| `DES_ROUTE_SETTLE`, `_WALK`, `_TURN`, `_ON` | the regress route's four times (defaults 35.9, 8, 1.5, 5 game seconds) |
| `DES_ROUTE_AB=KNOB=value` | the regress route adds `s5_ab` with the knob set and `s6_back` with it cleared |

Use it with `DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1` and a save that loads into the level. The game reaches the level
by itself, plays the route, and with `DES_ROUTE_QUIT=1` ends.

## What it writes

- `<name>.bmp` for each shot.
- `route.txt`: the timeline. It has the anchor, each shot's game time, and each hold's length in frames, plus how long
  the game read the left stick. The last line is `done: ok`, `done: missing` or `done: no_level`.
- `summary.json`: `result`, the shots saved and missing, then the frames from the moment the anchor is found to the
  route's end:
  - the frame count, seconds, and mean fps;
  - the median frame time, and the 1% and 0.1% lows (from the 99th and 99.9th percentile frame times);
  - the hitches (frames over 2.5 times the median) and the worst frame;
  - game time over real time (1.0 when the game keeps its pace).
- `route_steps.txt`: the steps that were played.

## Route files

One step a line; `#` starts a comment. Times are game seconds.

| Step | Meaning |
|---|---|
| `anchor` | wait for the level: the opening cutscene's end, or with no cutscene the player present for 10 s |
| `wait <s>` | the next step comes s game seconds later |
| `shot <name>` | save the next frame with draws as `<name>.bmp` |
| `hold <s> lx ly rx ry` | hold the sticks for s game seconds (0-255, 128 is centred; ly 0 is forward) |
| `set KNOB=value` | set a knob while the game runs (an empty value clears it) |

The regress route as a file:

```
anchor
wait 35.9
shot s1_spawn
wait 0.8
hold 8 128 0 128 128
wait 2.3
shot s2_walk
wait 0.8
hold 1.5 128 128 255 128
wait 2.3
shot s3_turn
wait 0.8
hold 5 128 0 128 128
wait 2.3
shot s4_on
```

## How it keeps time

- **Game time** is the sum of the steps the game was handed: 1/30 s a frame at the original 30 fps, the frame's own
  time under `DES_FPS`. So each step is the same game moment at any frame rate and on a slow PC.
- **Every step is planned from the anchor.** A hold does not move the steps after it. A step waits only for a shot
  that is still being saved.
- **Holds are frame-exact.** The stick is set at one flip request and let go at the one where the frames since then
  have been handed the hold's time, less half a step. At 30 fps an 8 s hold is 240 frames.
- **Shots:** each shot gets its own dump folder (`CELLMARK_DUMP_DIR`) and asks for the next frame with draws
  (`CELLMARK_DUMP_NOW`), so exactly one file arrives.

The Python route (`tools/des/regress_route.py`) did the same from outside: it read the frame log, predicted the game
time, and sent the console wall-clock stick windows that it corrected while they ran.

## Checked (2026-11-03)

regress.sh on the same exe, the pinned save, Windows, D3D12:

| Route | Shots against the good set | Holds (frames) | s1_spawn, s4_on after the anchor |
|---|---|---|---|
| In the game (`REGRESS_ROUTE=game`) | 4 of 4 ok | walk 240, turn 45, walk on 150 | 35.90 s, 59.70 s |
| Python | 4 of 4 ok | walk 240, turn 45, walk on 150 | 35.90 s, 59.66 s |

In both, the game read the left stick on exactly the frames of each walk. The in-game route's holds start on the
planned frame; the Python one's start up to 0.03 s later (the console's delay).

## The tester kit (`tools/testkit/`)

`bash run_tests.sh <game folder>`. A player's download doesn't hold the kit, so a tester gets `tools/testkit` from the
repository or the source zip. It finds the program through `setup/release-info.txt`. Its README is for testers.

- **Route:** `DES_ROUTE=look` by default, built into the game. It does no walking: it stays where the save puts the
  player, so it suits any save. It takes a shot, three 5 s turns of the camera at full stick with a shot after each,
  then a 30 s pan for frame times. (The camera starts a turn slowly: 3 s from rest turns it about 15 degrees, 9 s
  about 150. A stick at 160 is inside the game's dead zone.) `DES_KIT_ROUTE=regress` walks from the first level's
  spawn instead.
- **Renderers:** each one in turn (Linux: `vulkan` then `legacy`, the old renderer on vkd3d; Windows: `d3d12` then
  `vulkan`; `DES_KIT_RENDERERS` sets them). Each runs twice: with an empty shader cache, then with the one the first
  run made.
- **Each run** plays from a copy of the tester's save (`PS3_SAVEDATA_ROOT`), muted, offline (`DES_ONLINE=0`), at the
  original 30 fps (`DES_FPS=0`), and with the tester's input left out (`PS3_NO_USER_INPUT=1`). It ends itself
  (`DES_ROUTE_QUIT=1`), or is stopped after `DES_KIT_MAX` seconds (900).
- **The archive,** `des_test_results_<date>.tar.gz` beside the script, holds:
  - per run: the log, the shots, `route.txt` and `summary.json`;
  - `results.txt`: one line a run;
  - `system.txt`: OS, CPU, RAM, the desktop session, `vulkaninfo --summary` where installed, and each run's route and
    GPU lines.
  
  The shader caches are deleted before packing (made from the game's files, and large).
- **Names out:** the home folder, the user name and the computer name are replaced in every text file. If any are left
  (in any case: Windows paths ignore it), nothing is packed and the files are listed.
- **The shots are for looking at, not for pixel diffs.** Fog and particles differ from run to run: two runs on one
  renderer differ about as much as D3D12 and Vulkan do (a mean of 3-5/255).
- bash 3.2 and later, so the macOS one works when there is a Mac build. No Python, no `jq`, no `timeout`.

## Next

- A `.command` wrapper for macOS once there is a Mac build (docs/120, phase 6).
- regress.sh's default moves to the in-game route once more runs agree.
