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

## Stutter when breaking obstacles

Above 30 fps, breaking a barricade or crate made the game stutter for about half a second. The frame log lined up
with the F8 marks: the frames that stepped the physics took 20-120 ms, the others 4-7 ms.

- **Cause.** The main thread waits for the step (`sys_cond_wait`) on the Havok PPU Thread, which polls its SPU jobs
  with `usleep(1)` (poll sites `0x010CF834`, `0x010D000C`, `0x010D0F8C`, `0x010D3B44`). After 64 back-to-back short
  sleeps, ps3recomp's `sys_timer_usleep` sleeps on the high-resolution timer (about 0.5 ms), so each poll came back
  late. Debris makes many short SPU jobs, so a step after a break added up hundreds of late polls.
- **Not the cause.** The SPU float fix (the same step cost on both, interleaved), the number of moving bodies
  (killing every enemy at once leaves 110-190 bodies awake, and the steps stay at 9-13 ms), and the in-between pose
  hooks (about 0.04 ms a frame).
- **Fix.** ps3recomp #89: sleeps of at most `USLEEP_YIELD_US` (default 10) keep the precise spin in a streak.
  Only the Havok thread sleeps that briefly; the other pollers ask for 30 us or more.

| 10 replayed breaks, 2 rounds | Worst step frame per break | Step frames over 16 ms |
|---|---|---|
| before | 12-54 ms | 56, 50 |
| after | 12-22 ms | 9, 7 |

Normal play on the deep Palace save is unchanged (same build, knob off and on, 3 runs: -0.07%, +0.5%, -0.2%).

The breaks were replayed from a player's recording: the frame log with the camera yaw, then a scratch probe that kills
every enemy, warps to where the player stood before each roll, and rolls along the same world direction (stick angle =
heading minus camera yaw).

## Moving platforms (2026-10-26)

Above 30 fps the player jittered while riding the Palace 1-4 elevator (`o2480_3000` in `m02_03_00_00.msb`).

- **Cause.** The elevator is a keyframed rigid body, and the world step moves it. Under the cap it moved only on
  step frames, and the player standing on it moved with it: at 200 fps, a 6 cm jump every 33 ms with still frames
  between. Lifts, turning bridges and opening doors are keyframed bodies too.
- **Fix.** `des_phys_rate` steps the world every frame, as `DES_PHYS_HZ=0` does, while the player is in or on a
  moving keyframed body and for 0.25 s of game time after. The hold keeps a test that misses for a frame from
  switching the cap on and off. On those frames `des_phys_kf` leaves the keyframe's 1/dt as the game set it.
- **The test.** It goes by position, against the bodies of the world's active simulation islands:
  - The world is `*(*(r31 + 0x10) + 4)` in the physics update (vtable `0x018B4F98`). It lists its active islands at
    +0x28 (data, size); an island lists its bodies at +0x48.
  - A moving keyframed body (vtable `0x018B4810`) has inverse mass 0 (+0x19C) and a linear (+0x1A0) or angular
    (+0x1B0) velocity. The level's fixed bodies have none, and corpses and debris have mass.
  - Characters' own bodies (the bones' hit bodies, keyframed to the animation) are on collision layer 8 (the low 5
    bits of the filter word at +0x2C), and on layer 6 once the character is dead (the player's after a 40 m fall:
    `0x01000008` became `0x01000026`). Both are left out. The paternosters' cabins and the big 2-1 lift are on
    layer 12.
  - The body's bounding box is held as 32-bit broadphase integers (+0x30 min, +0x40 max). The broadphase
    (`*(world + 0x54)`) maps them back to metres: `q / scale - offset`, with its offset at +0x70 and scale at +0x90
    (2500 and about 429,470 per metre in 2-1).
  - The player is on the body when the player's position (`P + 0x10`, `P = *(*(chr + 0x20) + 0x10)`) is in the box
    widened by 0.3 m.
- **Why not the character proxy.** The first version looked at the proxy's list of touched bodies (`*(P + 0x34)`,
  +0x8C). That caught the 1-4 elevator, but not the Stonefang paternosters: the proxy's contact list (+0x74) stays
  empty on level ground and in the middle of a cabin's floor. Only a side contact (a rail, a wall) shows, so the
  fix worked or not depending on where the player stood.
- Drawing the platform between steps would not help: the player stands on the stepped body, so the player would
  still move in steps.
- `DES_PHYS_RIDE=0` keeps the cap on a ride too. `DES_PHYS_RIDE_LOG=1` prints each switch with the body.

One session on the deep Palace save, elevator up and down (38 s each way) at each setting. Jitter is how far the
player's height strays from a straight line fitted over the 0.2 s around each frame (rms); still frames are frames
that moved less than a fifth of the ride's mean step.

| Setting | fps | Jitter, up / down | Still frames |
|---|---|---|---|
| 30 fps | 30 | 0.5 / 1.5 mm | 0% |
| Unlocked, cap 30, `DES_PHYS_RIDE=0` (before) | 185-190 | 15.1 / 14.9 mm | 44% / 42% |
| Unlocked, cap 30, fix | 196-198 | 1.3 / 1.9 mm | 1% |
| Unlocked, no cap (`DES_PHYS_HZ=0`) | 186-189 | 1.8 / 1.7 mm | 1% / 0% |

That table is from the first version (the proxy's touched bodies). With the position test, in a second session: before
15.4 / 15.3 mm and 57% / 54% still frames, fix 1.3 / 1.2 mm and 0%, at 211-217 fps. The elevator ran at 1.78 m/s in
both.

### Stonefang 2-1 paternosters

2-1 has two paternosters: one by the start (cabins at x 89-92) and one by the gear (`o6140_0000`, entity 1019, cabins
at x 185-187). Each cabin is three keyframed bodies: the cabin block and two rails. Rides on a copy of a save at the
2-1 start, standing on the cabin's floor in the middle and 0.7 m / 0.8 m off it both ways, 5-6 s each, measured the
same way as above:

| Setting | fps | Jitter, start / gear | Still frames |
|---|---|---|---|
| 30 fps | 30 | 1.6 / 2.4 mm | 0% |
| Unlocked, cap 30, `DES_PHYS_RIDE=0` (before) | 340-370 | 22.6-22.7 / 15.6-15.7 mm | 77-87% |
| Unlocked, cap 30, fix | 295-331 | 2.4-3.0 / 2.7-5.5 mm | 0-6% |
| Unlocked, no cap (`DES_PHYS_HZ=0`) | 313-345 | 1.6 / 2.8 mm | 0-2% |

The cabins move at the same speed in every setting, ridden or not: 2.57 m/s by the start and 1.78 m/s by the gear.
That was measured in a later session from the cabin body's own position against real time, 3 s per setting.

## Open

- Per-body settings (damping, quality type, max speeds) come from the game data and were not read.
- The Havok inactivity test and TOI code run on the SPU and were not read.
