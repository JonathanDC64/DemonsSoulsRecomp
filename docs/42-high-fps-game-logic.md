# Game logic that steps per frame above 30 fps (research, 2026-10-18)

With `DES_FPS` above 30, anything the game moves by a fixed amount on each update, without the frame time, runs too fast: 8 times as fast at 240 fps. docs/35, 38 and 40 cover the UI, the HUD and the cutscenes. docs/28 covers the follow camera's chase, the steep-slope slide and the movement stuck test. This doc covers the rest: the camera's R3 reset and lock-on, and a scan of the game code for other per-frame eases and steps.

It was read from the code only. Nothing was run. Lines that are guesses say "inferred".

Sources: the disassembly of `work/game/EBOOT.ELF`, read with small scratch scripts (a pattern scan for eases and steps, a caller and reference finder); the lift (`work/recomp`), to see how calls are emitted; RPCS3's `patch.yml` (the DeS entries); web search results about RPCS3's 60 fps patch.

Terms used below:
- **Per frame** or **per call**: the field moves once each time the function runs, whatever dt is.
- **Ease**: `cur += (target - cur) * k`. Fix: `k' = 1 - (1 - k)^(dt * 30)`, which is what `des_cam_rate` does.
- **Keep**: `cur = base + (cur - base) * r`. Fix: `r' = r^(dt * 30)`, like the decay half of `des_slide_rate`.
- **Step**: `cur += s` per call. Fix: `s' = s * dt * 30`.
- **dt**: the frame time, `kFrameDt` at 0x018C8DB4. Most updates also get it in f1.
- **C**: the follow camera, ChrExFollowCam (update 0x00119670). docs/41, section 2.1, has its fields.

## Answer

1. **0x0011A100 is the R3 camera reset when nothing is in lock-on range** (read). The camera swings back behind the character. It eases pitch and yaw toward the target by k = C+0x224 = 0.3 a frame. A 180° swing takes 15 frames: 0.5 s at 30 fps, 62 ms at 240, which looks like a snap. Fix: add `0x0011A100=des_cam_rate` to `port/insn_hooks.txt`.
2. **The same k drives lock-on** (read). While a target is held, the camera turns toward it by 0.3 of the angle outside a 3° dead zone, each frame. Three loads of +0x224 there are not hooked either: 0x0011CF84, 0x0011CF98 and 0x0011D2D4. At 240 the camera snaps onto a new target in about 54 ms instead of 0.43 s, and trails a strafing target by 1/8 of the 30 fps lag. Fix: three more `des_cam_rate` lines. Items 1 and 2 are the most visible ones left: every R3 press and every fight.
3. Outside the camera, the scan found three more real per-frame steps. None is as visible:
   - **Noise radius** (read; that enemies hear with it is inferred). Sound events (a position, a radius of 0.5 to 13 m, a life of 0.5 to 2 s) lose 1% of their radius each frame (x0.99 at 0x006A3990), while their life counts down by dt. At 240 a 2 s sound covers about a quarter of the radius-time it covers at 30, so enemies probably hear the player less. You see it only through enemy behaviour.
   - **Character fades** (read; what triggers them is inferred). Two of the 20 node types in a character's effect list fade the model by a fixed step per call: type 16 (0.05 a call, 0x0025EB20) and type 14 (1/30 a call, 0x002654E8). At 240 their fades are 8 times as fast: 0.67 s becomes 83 ms, 1 s becomes 125 ms.
   - **Pitch return after a scripted camera angle** (read). Keep factor C+0x18C = 0.3 a frame (0x0011C15C). It takes about 4 frames at any rate: 133 ms at 30, 17 ms at 240. Low.
4. Everything else that was checked uses dt (section 3): the stamina drain tick, the regen accumulator, the camera manager's blend, the other effect-list timers, the noise life, and Havok (physics and animation, from 0x00FF0000 up).
5. Reports from players of RPCS3's 60 fps patch (2019) say the jump distance changes, and that sliding down a ladder can drop you through the floor (the second is from Dark Souls with DSfix, said to apply here too). The jump report fits the movement stuck test that `des_move_post` already fixes (inferred). Both need a run (section 5).
6. Two side notes. The ChrFollowCam update 0x00122C40 has no callers (read), so its five lines in `insn_hooks.txt` never run. 0x00798878, a twin of ObjScrollMsg's update with the same -1.5 px step a call, has no callers either.
7. `DES_FRAMELOCK` and `DES_FLWATCH` cannot see any of this. They wrap functions with `ppu_register_function`, which only catches calls through a vtable or a pointer. Every function here is reached by a direct `bl`, which the lift emits as a plain C call (`func_00119670(ctx)` at `ppu_recomp_000.cpp:317601`). So a longer hook list would not help. The run request logs from inside `des_cam_rate` instead: it already runs every frame inside 0x00119670, with r31 = C.

## 1. Candidates

| # | What | Code | Constant | Read? | Trigger | At 240 fps | Fix |
|---|---|---|---|---|---|---|---|
| 1 | Lock-on: the camera turns to the target and tracks it | 0x00119670. k loads: 0x0011CF84 (f13, pitch, used at 0x0011CFF0), 0x0011CF98 (f12, yaw, used at 0x0011D090), 0x0011D2D4 (f11, both axes, used at 0x0011D2F0/0x0011D2F4) | C+0x224 = 0.3, outside the dead zone C+0x228 = 3° | read | R3 with an enemy in range, then strafe | snaps onto the target, tracks with almost no lag | 3 x `des_cam_rate` |
| 2 | R3 reset, nothing in range | 0x00119670. k load 0x0011A100 (f12, pitch at 0x0011A16C; copied to f10 at 0x0011A108 for yaw at 0x0011A204) | same | read | R3 with no enemy near, camera turned away | 180° in 62 ms instead of 0.5 s | 1 x `des_cam_rate` |
| 3 | Noise radius | list update 0x006A3970, run each frame by 0x0067A1C8. Load 0x006A3990 (lfs f12), store 0x006A39B4. Twin 0x006A2E78 (load 0x006A2E8C), no callers | 0.99 (0x018EDC0C), kept a frame | mechanics read; enemy hearing inferred | rolling, attacking, running (inferred) | 9% of the radius left after 1 s (74% at 30) | keep hook at 0x006A3990 and 0x006A2E8C |
| 4 | Character fade-in, and fade by distance | effect node type 16, 0x0025EB20. Step loads 0x0025EB88 (in) and 0x0025EC14 (out); result multiplies the model alpha chr+0x368 | 0.05 (0x018D6848) a call | read; triggers inferred | a character fading in; in distance mode, a character near the player | 20 calls: 0.67 s becomes 83 ms | step hook at 0x0025EB88 and 0x0025EC14 |
| 5 | Character model fade with SFX | effect node type 14, 0x002654E8. Step loads 0x00265C88 (in) and 0x0026558C (out) | 1/30 (0x018D691C) a call | read; trigger unknown | unknown | 30 calls: 1 s becomes 125 ms | step hook at 0x00265C88 and 0x0026558C |
| 6 | Pitch return after a set-angle | 0x00119670. Load 0x0011C15C (f11, used at 0x0011C1A4) | C+0x18C = 0.3, kept a frame | read | after a scripted camera angle (C+0x12C) while timer C+0x124 runs | about 4 frames either way | keep hook at 0x0011C15C |
| 7 | UI: ObjScrollMsg twin | 0x00798878, -1.5 px a call | 1.5 (0x018F303C) | read: no callers, no references | none | none | none needed |

## 2. Details

### 2.1 R3 reset (0x0011A100)

How it starts (read):
1. The lock-on controller's update is 0x00384210 (r31 = controller). When R3 is newly pressed (r17) and byte +0x1059 is set, it calls 0x00273DC8, then sets byte +0x92 = 1 on the camera manager (controller+0x1014) at 0x003855E8. (Inferred: +0x1059 marks the case where R3 found nothing to lock.)
2. The camera manager (ChrCam, update 0x00115370) checks +0x92 each frame. If it is set, it sets C+0x242 = 1 and clears +0x92 (0x0011552C to 0x00115548). Just before that it copies manager +0x90/+0x91 into C+0x240/+0x241 and clears them (0x00115520 to 0x00115528). So the lock-on code must set +0x90 again every frame to keep the camera's look-at.
3. The camera turns C+0x242 into an auto-turn (0x0011C1FC to 0x0011C284). Target yaw C+0x250 is the yaw of the vector at C+0x80/C+0x88, through 0x00113270 (inferred: the character's facing). Target pitch C+0x24C is C+0x140 when C+0x255 is 0. It sets C+0x243 = 1 and the timer C+0x244 = C+0x248 (0.3 s).

Each frame while C+0x243 is set (0x0011A0A4 to 0x0011A360):
- The timer C+0x244 goes down by dt (0x0011A0B4; dt is in f26).
- Pitch C+0x130 += wrap(C+0x24C - pitch) * k (0x0011A16C). Yaw C+0x134 += wrap(C+0x250 - yaw) * k (0x0011A204). k = C+0x224, loaded at 0x0011A100.
- When both gaps are under 1° (0.01745 at 0x018CE410), it clears C+0x243 and C+0x244 (0x0011A35C/0x0011A360). Stick input also ends it once the timer is used up.

k is 0.3: the constructor 0x00117DD0 first stores the base default 0.1 (0x00118090), then 0.3 (0x001181DC) with the rest of ChrExFollowCam's values (+0x1A8 = 0.4, which matches the comment in `insn_hooks.txt`). The other constructor, 0x00118288, stores 0.1 but has no callers. The run reads the live value anyway, in case a camera param overwrites it.

Timing: the gap left after n frames is 0.7^n. From 180° to 1° is 15 frames: 0.5 s at 30 fps and 62 ms at 240. With the fix it takes 0.49 s at 240 (117 frames).

The hook is safe: f12 is not written between 0x0011A100 and its use at 0x0011A16C, and the copy to f10 at 0x0011A108 comes after the hook, so the yaw gets the new k too.

### 2.2 Lock-on (0x0011CF84, 0x0011CF98, 0x0011D2D4)

- While a target is held, C+0x240 is 1 every frame (2.1, step 2). The look-at path eases pitch (0x0011CFF0) and yaw (0x0011D090) toward the direction of the target point (C+0x230) by k = C+0x224. It moves only the part of the gap outside the dead zone C+0x228 (0.0524 rad = 3°, set at 0x00118094 and 0x001181A4). Another branch (0x0011D2C8 to 0x0011D2F4) moves both axes from one length, with k loaded at 0x0011D2D4. All three loads were checked: the register is not written between the load and its use.
- Locking on with the target 90° to the side: the gap outside the dead zone falls by 0.7 a frame, so it is within 1° in 13 frames. That is 0.43 s at 30 fps and 54 ms at 240. Fixed, about 0.42 s at 240.
- Strafing while locked: the camera trails the target's bearing by about turn rate x 0.078 s at 30 fps, but turn rate x 0.0097 s at 240 (plus the dead zone in both). Fixed, it is turn rate x 0.091 s at 240.
- This also answers docs/41's open question (its R3 run request): +0x240 is the lock-on look-at; +0x243 is the R3 reset.

### 2.3 Noise radius (0x006A3970)

- The owner is the global object at *(0x01B49BD8); call it S. Each frame, the game's step 0x00143F00 calls 0x0067A1C8(S, dt) (at 0x001440E0). That updates four sub-objects with dt, then three lists with 0x006A3970(list, dt): S+0x60, S+0x90 and S+0xC0 (0x0067A264 to 0x0067A28C). Each list keeps its begin at +0x4 and its end at +0x8. Entries are 0x30 bytes.
- Entry fields (read): +0x00 position (vec4), +0x10 owner, +0x14 radius, +0x18 time left, +0x1C and +0x20 two more floats from the push.
- Each frame (0x006A3990 to 0x006A39C4): radius *= 0.99 and time left -= dt. Entries whose time is up are removed (0x006A39D8 to 0x006A3A48). The 0.99 is loaded once, before the loop.
- Pushing an event (0x006A52E0, for list S+0x60 through 0x006794B0): the radius is f1 x a factor, and the time comes from a type table at 0x019DA504 {type, factor, time}. Type 1: 0.5, 2 s. Type 2: 6, 2 s. Type 3: 7, 2 s. Type 4: 10, 2 s. Type 5: 8, 2 s. Types 7 and 8: 1, 0.5 s. Type 9: 13, 0.5 s. Type 10: 5, 0.5 s. A new event from the same owner inside an old one's radius refreshes the old one instead (0x006A3628 tests distance < f1 + radius).
- Who pushes: character code, for example 0x0027FC48 (type 8). It takes two values from chr+0x304 (inferred: character params) and skips character states 3, 4, 10 and 11. It is called from 0x0026CF00, 0x0026D47C, 0x0026D630, 0x0026D6D4 and 0x0026DAF0. Other pushes come from 0x00271A34, 0x0027FDB4, 0x0027FE98 and 0x002820A0.
- Inferred: these are sound events for enemy hearing. The radii and lifetimes fit that. The code that reads the lists for the AI was not found.
- At 240 fps the radius is 0.99^240 = 9% of its start after 1 s, against 0.99^30 = 74% at 30. Averaged over a 2 s life, it is 21% of the start radius at 240 and 75% at 30. Over a 0.5 s life, 58% and 93%.
- Fix: a keep hook (`r' = r^(dt * 30)`) after 0x006A3990, and after 0x006A2E8C in the single-entry twin 0x006A2E78 (no callers found; harmless to hook).

### 2.4 Character effect list (node types 16 and 14)

- Each character has a list of effect nodes at +0x3BC of an object (r26 in 0x00287D40). 0x00287D40 is called from the character updates at 0x002965E0, 0x00296A28 and 0x00298BC4, with dt in f1. Each frame, 0x00268F58 sets the model alpha multiplier chr+0x368 to 1.0 (0x00269020 to 0x00269028), then runs each active node through 0x00268C28 with dt in f1. That dispatches on the node type, `(word0 >> 1) & 0x7FFF`: 20 types, jump table at 0x00268C70. Nodes multiply chr+0x368 by their own fade.
- Nodes are added with an id, an SFX id and a dummy poly. Example: the switch at 0x00283990 calls 0x002611E0(list, id 10101 to 10154, SFX 14995 to 24026, dummy poly 7 to 226). Inferred: these are the visuals of SpEffects, such as weapon buffs.
- **Type 16 (0x0025EB20).** The fade +0x10 (0 to 1) moves 0.05 a call: up at 0x0025EB84 to 0x0025EB98, down at 0x0025EC10 to 0x0025EC24. It is clamped to [0, 1], then chr+0x368 *= +0x10. Without the distance flag (+0x14 bit 0x40000000) it fades in to 1, and the node ends (0x0025EBA0 to 0x0025EBBC). With the flag, the direction follows the distance from this character to the player (the player is `*(*0x01B3D6E0 + 0x1BC)`): it fades out beyond +0xC and back in under +0x8 (0x0025ED20 to 0x0025ED5C). A fade is 20 calls: 0.67 s at 30 fps, 83 ms at 240.
- **Type 14 (0x002654E8).** +0x10 moves 1/30 a call: in at 0x00265C80 to 0x00265C9C, out at 0x00265584 to 0x0026559C. It is passed to 0x000D81A0 on the model (chr+0x6A4) with r4 = 9 to 12 (inferred: a material parameter). A sub-state byte at +0x15 picks the case. Two cases use SFX ids 22616/22617 (0x00265CCC to 0x00265CE4). A fade is 30 calls: 1 s at 30 fps, 125 ms at 240.
- Other types read: 17 (inline at 0x00268CC0) and the out-of-line 0x0025E060 add dt x -1.5 to +0x8; 19 (inline at 0x00268EF0) subtracts dt. The other 16 handlers were not read.
- Fix: a step hook (`s' = s * dt * 30`) after each step load: 0x0025EB88 and 0x0025EC14 (f0), 0x00265C88 and 0x0026558C (f13). Each register is used once, right after the load. These handlers may run on a worker thread (inferred), so the hook should read `kFrameDt` and touch nothing else.

### 2.5 Pitch return after a set-angle (0x0011C15C)

- While the timer C+0x124 runs (down by dt at 0x0011A384 to 0x0011A394), the set-angle flag C+0x12C is set (0x0011A3A4), and there is no look-at or auto-turn, the code does: pitch = C+0x140 + wrap(pitch - C+0x140) * C+0x18C (0x0011C118 to 0x0011C1F4). The same block is also reached from 0x0011C8E0.
- C+0x18C is 0.3 (constructor, 0x00118140). Keeping 0.3 of the gap a frame, the pitch is within 1% in 4 frames: 133 ms at 30 fps, 17 ms at 240. Hard to see.
- Fix: a keep hook at 0x0011C15C (f11, used at 0x0011C1A4).
- Inferred: set-angle events are scripted camera changes. The EBOOT has the script names `ResetCamAngle` and `CamReset`.

### 2.6 Fix sketch

`port/insn_hooks.txt`, with the existing symbol:

```
0x0011A100=des_cam_rate   # k = +0x224 (0.3): R3 reset (auto-turn), pitch; f10 copy feeds yaw
0x0011CF84=des_cam_rate   # k = +0x224: lock-on look-at, pitch
0x0011CF98=des_cam_rate   # k = +0x224: lock-on look-at, yaw
0x0011D2D4=des_cam_rate   # k = +0x224: lock-on look-at, both axes
```

Two new hooks in `port/des_options.cpp`, built like `des_cam_rate` (lfs only; do nothing without `DES_FPS`, at 30 fps or when steps == 1; a live knob to turn them off):

```cpp
/* keep factor r, cur = base + (cur - base) * r: r' = r^(dt * 30) */
extern "C" void des_keep_rate(ppu_context* ctx, uint32_t addr)  // 0x006A3990, 0x006A2E8C, 0x0011C15C
{   ... const double r = ctx->fpr[fd];
    if (r > 0.0 && r < 1.0) ctx->fpr[fd] = (float)std::pow(r, (double)steps); }

/* step s per call: s' = s * dt * 30 */
extern "C" void des_step_rate(ppu_context* ctx, uint32_t addr)  // 0x0025EB88, 0x0025EC14, 0x00265C88, 0x0026558C
{   ... ctx->fpr[fd] = (float)(ctx->fpr[fd] * steps); }
```

All of these give the 30 fps behaviour at any frame rate, so they fit "original behaviour by default". Changing the hook list touches `ppu_recomp.h`, so all 14 chunks rebuild (about 15 min). The five ChrFollowCam lines can stay; they never run.

## 3. Checked and dt-based

| What | Where | How dt is used |
|---|---|---|
| Camera manager blend between two cameras | 0x00115618 to 0x0011562C | weight +0x78 -= dt / +0x7C |
| R3 reset timer | 0x0011A0B4 | -dt |
| Set-angle timer | 0x0011A384 | -dt |
| Noise life | 0x006A39B0 | -dt |
| Effect node types 17 and 19, and 0x0025E060 | 0x00268CC8, 0x00268EF4, 0x0025E090 | +-dt x k |
| Stamina drain tick | 0x002860EC to 0x00286658 | +0x144 += dt; -1 stamina each 0.1 s (0x019DA2A8) |
| A regen accumulator (inferred: HP or MP) | 0x00286148 to 0x00286190 | +0x400 += dt x rate; whole units go to +0x3E8 |
| Movement stuck factor | 0x002EC2B8 | fixed (`des_move_post`) |
| Steep-slope slide | 0x001FEAF4, 0x001FED50 | fixed (`des_slide_rate`) |
| A vector ease in the proxy code | 0x001FF160 | k is the step's +0x20, the step's dt (inferred) |
| A vector ease | 0x00381030 | k from f1, dt (scan) |
| Havok: physics, animation, vehicles | 0x00FF0000 and up (`hkp...`, `hk...` class names) | Havok steps by dt (inferred) |
| ChrFollowCam | 0x00122C40 | no callers; never runs |
| The other constructor of ChrExFollowCam | 0x00118288 | no callers |

## 4. Not found, or not checked

- Hit stop, damage flash, item pickup glow, doors, elevators, FFX particles and sound fades: the scan found no per-frame pattern for them in the game code (0x00010000 to 0x00F00000). They were not read further.
- Integer per-frame counters were not scanned (too many loops look the same). One was seen: the game step 0x00143F00 counts +0x48 down by 1 a call from 2 (0x00144004 to 0x0014400C). Two frames; harmless.
- Jump and ladder: no lead in the code. See the run.
- The scan finds float eases and steps whose factor comes from a constant or a field. It misses steps built from params or kept in ints.
- Scan size: about 1,000 matches in the EBOOT; 149 outside the camera code after removing the ones scaled by dt. The game code ends below 0x00F00000; Havok is above.
- RPCS3's patch does the same as `DES_FPS` (FlipMode, plus a measured dt capped at 0.05 s) and nothing about per-frame eases. Its note warns: "Vblank rate can be modified to increase the framelimit beyond 60 FPS. However, it's not recommended as users may encounter physics issues." So everything above happens in RPCS3 at 60 fps too, at twice the 30 fps rate.
- Web, second hand (search snippets; the pages refused direct reads): a 2019 thread on the RPCS3 forums, "60 fps in Demon's Souls affects jump performance", says a jump lands somewhere else at 60 fps, and a reply says sliding down ladders can drop you through the floor (from Dark Souls). News coverage of the patch reported that at 120 fps, running into geometry slowed the player (since fixed here by `des_move_post`) and that cutscenes ran fast (handled by docs/40).

## 5. Run request

One batch covers every item. Use one scratch build with:
- the four new lines from 2.6 (a re-lift), so that `DES_CAMFIX=0` and `=1` compare the bug and the fix in the same build;
- two scratch logs inside `des_cam_rate` (port code only), printed once per flip, on the first `des_cam_rate` call after the flip (a counter bumped by a flip-request hook, like the one `DES_FRAMELOG` installs):
  - `DES_CAMWATCH=1`: flip, dt, the hook address, C (= r31), C+0x130, C+0x134, the bytes C+0x240 to C+0x243, C+0x244, C+0x24C, C+0x250, C+0x224, C+0x18C.
  - `DES_NOISELOG=1`: S = *(0x01B49BD8); for each of the lists at S+0x60, S+0x90 and S+0xC0 that is not empty, each entry's index, +0x10, +0x14 and +0x18. Same thread as the noise update (both run in the game's frame step; inferred).
- `DES_FRAMELOG=<file>` as it is: one line per flip with the pad, the player position and the speed factor.

Without a re-lift, the two logs alone still show the bug (the 240 fps rows below). The fix columns then wait for the re-lift. If no port change is wanted at all, the read-only memory sampler used for docs/28 can sample C instead (find C by its vtable 0x01856018, as in docs/41 run R1); console `mem` reads are too slow for a 62 ms swing. Screenshot bursts with phase correlation can also time R1 and R2.

Setup: the pinned Tutorial save, `DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1`, the debug console, `RSX_PRESENT` by lock state. Skip the opening cutscene. Run the steps three times: `DES_FPS=30`; `DES_FPS=240 DES_CAMFIX=0`; `DES_FPS=240 DES_CAMFIX=1` (`setenv` changes both live). The camera log relies on some `des_cam_rate` site running every frame. docs/28 saw 7 sites do that while walking. Standing or locked on is not known, but with the four new lines the R3 and lock-on code calls `des_cam_rate` itself. Without them, gaps in the flip numbers show missed frames, and `DES_CAMLOG=1` names the sites that ran (one line per 20,000 calls).

Steps:
- **R1, R3 reset.** Stand still with no enemy near (the start area). Hold the right stick to one side until C+0x134 has moved by about pi, so the camera looks at the character's front. Release, wait 0.5 s, press R3 once, wait 2 s. Three times. Log: `DES_CAMWATCH` from the press until C+0x243 is back to 0.
- **R2, lock-on.** Walk to about 8 m from the first enemy. Turn the camera 90° away with the right stick. Press R3 to lock, wait 1 s. Hold the left stick fully left for 3 s (strafe around it), release, press R3 to unlock. Log: `DES_CAMWATCH` (C+0x240 should be 1 while locked), `DES_FRAMELOG`.
- **R3, noise.** In an open spot: 3 rolls (circle), 3 attacks (R1) at a wall, a 3 s dash (hold circle and the stick). Log: `DES_NOISELOG`.
- **R4, jump.** On flat ground with 10 m free: dash for 2 s, then press L3 (Demon's Souls jumps with L3 while dashing), and hold the stick until landing. Three jumps at each rate. Log: `DES_FRAMELOG` (position, speed factor mv+0x128).
- **R5, ladder**, only if the Tutorial has one within reach: climb up (cross at the ladder), then slide down (inferred: circle on the ladder, as in Dark Souls). Log: `DES_FRAMELOG`, and a screenshot at the bottom.
- **R6, character fades**, only if a re-lift is done anyway: add scratch hooks after the stores of the fade value at 0x0025EB98, 0x0025EC24 (type 16), 0x00265C9C and 0x0026559C (type 14), printing r31, +0x10 and dt. Then play R1 to R5 as above and note whether types 16 or 14 fire anywhere in the Tutorial. Without these hooks the fades cannot be seen in a log.

What to expect:

| Check | Log | 30 fps | 240, fix off | 240, fix on |
|---|---|---|---|---|
| R3 reset, 180° to 1° | C+0x134 from the press | 15 frames, 0.5 s | 15 frames, 62 ms | ~117 frames, 0.49 s |
| Reset timer (control) | C+0x244 | -dt a frame | -dt a frame | -dt a frame |
| k live | C+0x224, C+0x18C | 0.3, 0.3 | 0.3, 0.3 | 0.3, 0.3 |
| Lock-on, 90° swing | C+0x134 until within 4° of the target | ~13 frames, 0.43 s | ~13 frames, 54 ms | ~0.42 s |
| Strafe lag | C+0x134 against the target's bearing | rate x 0.078 s, plus 3° | rate x 0.0097 s, plus 3° | rate x 0.091 s, plus 3° |
| Noise radius | +0x14 against time | x0.74 a second | x0.09 a second | x0.09 (needs the keep hook) |
| Noise life (control) | +0x18 | -1 a second | -1 a second | -1 a second |
| Jump | horizontal distance and air time | baseline | the same within 5% if fine | |
| Speed factor in the jump | mv+0x128 | 1.0 | 1.0 (`des_move_post`) | |
| Ladder slide | vertical speed; floor reached | baseline | the same if fine; faster means a per-frame step | |

An item is confirmed when its per-frame count matches at 30 and at 240 with the fix off, while its control matches in seconds. A fix is confirmed when the 240 fix-on row matches 30 fps in seconds.

### Results (2026-10-18)

The runs used the fix branch (`fix/cam-reset-noise-rate`: the seven hook lines of 2.6, re-lifted) plus two scratch additions that are not in its PR: the logs above (`DES_CAMWATCH`, `DES_NOISELOG`, and `DES_STEPLOG`, which prints which keep sites run), and the `DES_WARP` debug teleport (its own PR). A script drove each session through the debug console and the pad file: it skipped the opening cutscene, calibrated the left stick against the camera yaw, then ran each step at the three settings, switched live. Sessions cw1-cw6, one game at a time, pinned Tutorial save; the player's save was unchanged after each.

Measured:

| Check | 30 fps | 240, fix off | 240, fix on |
|---|---|---|---|
| R3 reset, gap ~60-90° to ~1° (C+0x243 set) | 12 frames, 0.40 s | 12 frames, 0.050-0.065 s | 96-99 frames, 0.41-0.42 s |
| k and r live (C+0x224, C+0x18C) | 0.3, 0.3 | 0.3, 0.3 | 0.3, 0.3 |
| Noise radius kept after 1 s (median over 23-27 sounds; cw3 / cw1) | x0.77 / x0.84 | x0.29 / x0.42 | x0.77 / x0.75 |
| Noise life clock (control) | 0.81 s a second | 0.88 | 0.86 |
| Manual turn, 1.2 s of full right stick (control) | 270° | 270° | 270° |
| Lock-on swing, 46-71° until settled (C+0x240 set; cw6) | 0.20 s | 0.033 s | 0.22-0.24 s |
| Lock-on, camera lag behind a strafing target (cw5, cw6; rough, strafe speeds differed) | 19-20° | 10° | 24° |
| Jump: circle + L3 while dashing (cw6) | no jump in 3 tries | no jump in 3 tries | no jump in 3 tries |

What this shows:
- R3 reset: the per-frame count matches at 30 and at 240 with the fix off (12 frames), and with the fix on the swing takes the 30 fps time (0.41 s against 0.40 s). Confirmed, and the fix works.
- Noise radius: at 240 with the fix off a sound keeps a third to a half of what it keeps at 30 after a second; with the fix on it matches 30. The life clock runs at the same rate at 30 and at 240, so only the radius was per frame. Confirmed, and the fix works. (The life clock reads 0.81-0.88 s a second although game time ran at 0.98-1.0 of real time; why it is under 1 was not checked.)
- The manual camera turn is dt-based already and the fix leaves it alone.
- Lock-on: at 240 with the fix off, the camera snaps onto a new target in 33 ms instead of the 0.20 s it takes at 30. With the fix on it takes 0.22-0.24 s. Confirmed, and the fix works. While the player strafes around a locked target, the camera trails it by 19-20° at 30, 10° at 240 with the fix off and 24° with it on. The strafe speeds differed between the passes, so that row is only a rough check.
- Jump: not checked. In three scripted tries at each setting (dash for 2 s, then circle + L3 for 6 frames, stick held), the character never jumped: the height stayed on the floor and stamina showed no jump cost. How the game reads its jump input needs a look before this check can run.

Notes for the next runs:
- C+0x240 (lock-on) reads 0 from outside the game at any time; inside the camera update it is 1 for the whole lock. The byte only holds during the update. The driver reads the lock state from the `[camwatch]` rows instead.
- `PAD_FILE` takes a new press only once the current hold is over (cellPad.c). A press written late leaves a gap: circle lifts, and circle + L3 after a gap is a roll (stamina -19), not a jump. Queue the presses behind each other so circle never lifts.
- One session (cw2) slowed to a steady 9.7 fps for 30 s, every frame taking about 103 ms (a timed wait that expires each frame). The same plateau came up on a build without the fix hooks (master plus #50's idle cutscene hooks, after 27 s at 240 fps), so it is not from the fix. Update (2026-10-19): it was the ChrIns freeze that port #53 fixed: in cw2's frame log the game is frozen from 63 s to the end, and the ~10 fps is HG_Watchdog, which keeps presenting while the game threads are stuck. The other slow stretches, in cw4 and cw5, were build load: their frame times spread from 10 ms to over 500 ms instead of sitting at 100-110 ms.
