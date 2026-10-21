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
- `DES_RAGINTERP` (default 1): ragdolls are drawn between steps. Havok's ragdoll pose readers (`0x012D85E8`,
  `0x012D9288`) turn each bone's body transform into a position and rotation (`0x0109FB30`). Hooks after that call
  (`0x012D86C4`, `0x012D9354`) put in the pose between the start and end of the body's last step, from its swept
  transform (centre of mass at +0x120 and +0x130, rotations at +0x140 and +0x150, local centre of mass at +0x160).
  The blend is the game time since the body got a new step over the step length, so the drawn pose runs one step
  (33 ms) behind. A body that sleeps shows its own transform. `DES_RAGINTERP_LOG=1` checks the swept end against
  the transform (they matched for every body tested).
- Breakables, debris and loose objects are drawn between steps too. The game reads their body transform through
  `0x0109F250`, which copies an hkTransform into a 4x4 matrix; a hook after its last store (`0x0109F2C4`) writes
  the in-between pose into the matrix when the source is a moving dynamic rigid body (vtable `0x018B4810`,
  inverse mass above 0). Keyframed bodies and copies of anything else are left alone.
- `DES_PHYS_HZ=0` steps every frame (the old behaviour above 30 fps). Other values set the cap; 60 was tried and
  is worse than 30 (below).

## Corpses falling through the floor

Corpses that sank through the floor and fell out of the level were not the step rate. The SPU has no Inf or NaN:
the refined reciprocal estimate of 0 is the largest finite float, and RPCS3's approximate float mode (the one it
runs Demon's Souls with) also gives 0 for a multiply by 0. Our SPU helpers (`runtime/spu/spu_helpers.h` in
ps3recomp) gave Inf for 1/0 and then NaN for Inf * 0, and Havok's SPU code passed it on. ps3recomp branch
`fix/spu-xfloat-approx` matches RPCS3 (`fi` on the bit fields with a clamp, `fm`/`fma` zero cases, clamped
`fs`/`fms`/`fnms`). Which case Havok hit was not traced. The fix stops the falls with or without the cap.

| Warp test: runs where a corpse fell out of the level | Old SPU floats | Fixed SPU floats |
|---|---|---|
| Unlocked, no cap | 7 of 10 | 0 of 10 |
| Unlocked, no cap, the two runtimes interleaved | 3 of 4 | 0 of 4 |
| Unlocked, cap 30, with the breakables hook | 2 of 6 | 0 of 4 |

## Tests

All runs use a scratch probe that warps the player next to an enemy. Unlocked is about 200-400 fps.

| Test | 30 fps | Unlocked | Unlocked + cap 30 |
|---|---|---|---|
| Already-dead corpses, warp next to them, walk about: runs where a corpse fell through the floor (old SPU floats) | 2 of 7 | 7 of 10 | 2 of 6 (cap 60: 3 of 6) |
| Kill a soldier next to the player (Palace 1-1): samples over 1 m/s 3-12 s after death | 7, 0, 0 | 5, 6, 16 | 3, 1, 0 |
| The same with the player unhurt (`DES_GOD=1`), death phase | 2, 2, 3, 3 | 8, 7, 18, 8 | 2, 6, 2, 4 |
| The same, then walk into the corpse | 6, 0, 2, 4 | 32, 8, 0, 4 | 3, 0, 0, 39 |
| Death phase, fixed SPU floats and the breakables hook | | | 5, 2, 2, 12 |

- The cap brings death twitching back to the 30 fps level.
- Dragging a corpse by walking through it throws it at 5-11 m/s at 30 fps too; there is no RPCS3 reference for it yet.
- Walking into corpses is noisy at every rate. One capped run sank a corpse 2 m. The warp test also sinks corpses
  at 30 fps, so the original game does this too, less often.
- At level load, 30-40 bodies drop for a few seconds and settle the same way at 30 fps and unlocked.
- Drawn pose of a soldier's pelvis after death, unlocked: without `DES_RAGINTERP` it holds for 4-5 frames and then
  jumps (up to 5.7 cm in a frame); with it, it moves every frame (at most 0.75 cm).

## Open

- Per-body settings (damping, quality type, max speeds) come from the game data and were not read.
- The Havok inactivity test and TOI code run on the SPU and were not read.
