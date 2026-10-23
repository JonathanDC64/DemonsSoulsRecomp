# Foot IK above 30 fps (issue #140)

Date: 2026-10-22. Status: the cause was found by reading the code, then measured. The fix is `des_ik_rate` behind `DES_IKFIX` (on by default), and it was measured too.

## The report

Issue #140: on build 0.1.0-dev.677, native Linux, `DES_FPS=165` with V-sync off, the game ran at 86-108 fps. In the Tutorial's indoor halls the feet "do not place flatly ... contort to strange angles and jutter a little mid step". This happened while standing and turning on broken floor slabs, and while running down corridors. The reporter thought it might also be the original game.

## Where the foot IK runs

Once per character per update, on the PPU:

- 0x00287D40, with the update's dt in f31, calls 0x002F0EB0. That calls the movement object's (chr+0x20) virtual function at 0x64: the foot IK driver, 0x002F5B08 (vtable 0x0185BAE0).
- For each leg, the driver calls 0x001CD560 (from 0x002F5C6C for the right leg, 0x002F5CA8 for the left). That calls Havok's `hkaFootPlacementIkSolver::doFootPlacement` (0x00FF4DC0), which ends in the two-joint IK (0x00FF6AD8).
- The setup (0x001CCEF0, 0x001CCFF8) sets every solver gain to 0.8: on/off, ascending, descending, planted and raised. Foot locking is off.

## Four smoothing steps that ignore dt

Each runs once an update with a fixed amount, tuned for 30 updates a second. Above 30 fps they settle faster in real time, so the hips and feet follow every bump in the ground instead of gliding over it.

| Step | Site | Each update | Settles in (tau) at 30 | at 95 | at 240 |
|---|---|---|---|---|---|
| Pelvis offset moves 0.2 of the lower foot's error | 0x002F5DB8 (`lfs f13`, 0.2), then `fmadds` | ease 0.2 | 149 ms | 47 ms | 19 ms |
| Pelvis offset kept 0.6 when neither foot hits the ground | 0x002F5CCC (`lfs f0, 0x220(r31)`) | keep 0.6 | 65 ms | 21 ms | 8 ms |
| The solver's weight moves toward on or off | 0x00FF5680, 0x00FF58CC (`lfs f11, 0x68(r12)`, onOffGain) | ease 0.8 | 21 ms | 6.5 ms | 2.6 ms |
| Each foot's height moves toward its target | 0x00FF54D4 (`fmuls f13`: the product of two 0.8 gains) | ease 0.64 | 33 ms | 10 ms | 4 ms |

The ankle is turned to the ground's surface normal under each foot with no smoothing at any frame rate (read in the code, not measured). On broken slabs, that tilt is part of the original game's look at 30 fps too. The jitter and the fast snapping are the frame-rate part.

## The fix

`des_ik_rate` (`port/des_options.cpp`), six lines in `port/insn_hooks.txt`:

- 0x002F5B14 (`fmr f30, f1`) keeps the driver's dt, for the other five sites in the same call (thread-local).
- At each ease site the loaded amount k becomes 1 - (1 - k)^(30 dt). At the keep site, r becomes r^(30 dt). So one second of updates does what one second does at 30 fps, whatever the rate.
- The foot height is hooked once, on the product of its two gains (0x00FF54D4). Hooking each gain separately would smooth it too much.
- At 30 fps 30 dt is 1, so nothing changes: the original game is untouched. Nothing changes without `DES_FPS` either.
- `DES_IKFIX=0` turns it off; live.
- `DES_IKLOG=<file>` writes one line per update for the player only: time (ms), dt, pelvis offset, and the lower foot's error (or `air`).

The solver is Havok's, so every character with foot IK gets the fix, not only the player.

## Measured

The test: a player's start of the fix's build, on a copy of the pinned save, muted, with the opening cutscene skipped. A developer build's `DES_WARP` put the player on the stairs at (139.48, 1.45, -16.02), part of the Tutorial route from the start. Before each phase the player was warped back there, and the driver logged the phase's start in the IK log's time. The phases were: stand, turn left, turn right, a slow walk, a slow walk back, and a run down the stairs. Each turn and each warp moves the feet, and the pelvis settles after it. The slow walks were too gentle to move the player.

The pelvis ease's settling time. While the target holds still, the lower foot's error shrinks by a fixed ratio each update; tau = dt / -ln(ratio), from every pair of updates with the error between 3 mm and 15 cm:

| Run | Frame | tau (median) | Per update |
|---|---|---|---|
| 30 fps | 33.4 ms | 148.8 ms | 0.800 |
| 240 fps, `DES_IKFIX=0` | 4.1 ms | 19.3 ms | 0.800 |
| 240 fps, `DES_IKFIX=1` | 4.1 ms | 143.6 ms | 0.972 |

The pelvis offset during the turns and the run. "Path" is how far it moves per second. "Jitter" is its mean distance from its own 100 ms average:

| Run | Turn left: jitter | Turn right: jitter | Run: path | Run: jitter | Run: highest |
|---|---|---|---|---|---|
| 30 fps | 0.15 mm | 0.28 mm | 0.16 m/s | 1.34 mm | +0.006 m |
| 240 fps, fix off | 0.58 mm | 0.99 mm | 0.34 m/s | 3.75 mm | +0.072 m |
| 240 fps, fix on | 0.17 mm | 0.28 mm | 0.14 m/s | 0.96 mm | +0.032 m |

The offset in 100 ms steps from the start of the run:
- At 30 fps it glides from -0.175 to -0.013 over 400 ms.
- At 240 fps with the fix off it reaches 0 within 200 ms and then jumps with each step (-0.098, +0.006, +0.015, -0.092).
- With the fix on it glides again, from -0.175 to -0.013 over 500 ms.

At 30 fps the jitter is not there, so RPCS3 was not run for comparison. The first test spot, the tunnel at the Tutorial's start, is flat: the pelvis offset stayed at 0 there at 30 and at 240.

Regress, with the fix on: all four shots ok at 30 fps (frame step 33.33 ms, game/real 0.999) and at 240 fps (`DES_FPS=240`, V-sync off; frame step 4.52 ms over the route). On this split build regress can stop at 20 s with "run ended before the level": the PID file appears only after the 289 MB game DLL loads, about 18 s in. A second try passed.

## Not done

- The foot height and the solver's weight are fixed by the same rule, but only the pelvis was logged. Their effect was not measured on its own.
- The reporter's spot (the slabs by the crates in the indoor halls) was not reached; the stairs stood in for it.
- Not run on Linux. The hooks are in the lifted game code, so they apply on every platform.
