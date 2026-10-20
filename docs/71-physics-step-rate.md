# Physics step rate above 30 fps (2026-10-20)

Above 30 fps, corpse ragdolls twitch after death, fly about, and sometimes sink through the floor when the player
walks into them. RPCS3's DeS FPS patch has the same warning: above 60 fps "users may encounter physics issues".
`DES_PHYS_HZ` (default 30) fixes most of it by stepping Havok at most 30 times a second.

## Cause

- The physics update `0x001D1E30` steps the Havok world once a frame with the frame's dt (f31). There is no fixed
  step and no substepping inside Havok (its step at `0x011DBEB8` takes dt as given).
- Havok counts some things in steps, not seconds. The solver removes a fixed share of each overlap per step
  (tau 0.6, 4 iterations), so at 400 steps a second a resting ragdoll is pushed apart 13 times as often
  (inferred from Havok's design; the solver runs on the SPU and was not read). Sleep
  checks run on a 16-step cycle (counter at world+0x2F7, `0x011DC254`): 0.53 s at 30 fps, 40 ms at 400.
- The world settings are Havok's defaults apart from solver type, gravity, broadphase size, simulation type and
  border behaviour (setup `0x001D2C68`, world constructor `0x011C2900`). None of them is tuned for small steps.

## Fix

- `des_phys_rate` runs after `0x001D1E64` (`lbz r28, 0x14(r31)`, the update's own skip byte). Frames shorter than
  1/n add up. Each time they reach 1/n the world steps exactly 1/n; on the other frames the step is skipped
  (r28 = 1). A frame of 1/n or longer steps its own dt, so 30 fps and below is the game as it was.
- `des_phys_kf` runs after `fdivs f31, f0, f1` at `0x001EF6D0` and `0x001DF83C`. These two passes drive a
  character's bodies to its animation pose through Havok's hard keyframe `0x0131BAF8`, with a velocity of
  (distance to the pose) times 1/dt. Under the cap the world steps 1/n, so the hook sets 1/dt to n. Without it, a
  velocity sized for a 2.5 ms frame is carried for 33 ms and the bones shoot ahead (one test hit 1222 m/s).
- The player's position still changes every frame (the step does not move it), so walking is not choppy.
  Ragdolls and other physics objects move 30 times a second.
- `DES_PHYS_HZ=0` steps every frame (the old behaviour above 30 fps). Other values set the cap; 60 was tried and
  is worse than 30 (below).

## Tests

All runs use a scratch probe that warps the player next to an enemy. Unlocked is about 200-400 fps.

| Test | 30 fps | Unlocked | Unlocked + cap 30 |
|---|---|---|---|
| Already-dead corpses, warp next to them, walk about: runs where a corpse fell through the floor | 2 of 7 | 7 of 10 | 2 of 6 (cap 60: 3 of 6) |
| Kill a soldier next to the player (Palace 1-1): samples over 1 m/s 3-12 s after death | 7, 0, 0 | 5, 6, 16 | 3, 1, 0 |
| The same with the player unhurt (`DES_GOD=1`), death phase | 2, 2, 3, 3 | 8, 7, 18, 8 | 2, 6, 2, 4 |
| The same, then walk into the corpse | 6, 0, 2, 4 | 32, 8, 0, 4 | 3, 0, 0, 39 |

- The cap brings death twitching back to the 30 fps level.
- Walking into corpses is noisy at every rate. One capped run sank a corpse 2 m. The warp test also sinks corpses
  at 30 fps, so the original game does this too, less often.
- At level load, 30-40 bodies drop for a few seconds and settle the same way at 30 fps and unlocked.

## Open

- Ragdolls move at 30 Hz on a high refresh display. Smoothing them would need the bone poses blended between steps.
- Per-body settings (damping, quality type, max speeds) come from the game data and were not read.
- The Havok inactivity test and TOI code run on the SPU and were not read.
