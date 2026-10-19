# In-engine cutscenes at the unlocked frame rate (spike)

Date: 2026-10-18. Status: research only. Nothing was built. Run requests R1-R3 were run later the same day; section 5 has the results.

Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`).

Three labels are used below:
- **Read**: seen in the lifted code (`work/recomp`), in the EBOOT bytes, or in our source.
- **Measured**: taken from screenshot bursts that already existed on disk (made on 2026-10-16, before the 30 fps lock, with a scratch `cut.sh` harness). Nothing new was run for this spike.
- **Inferred**: a conclusion that still needs a check in a run. The run requests in section 5 cover these.

## Short answer

- **What steps at 30 Hz is the cutscene camera, and only the camera.** The camera track of a cut ("sibcam") has keys at whole frames (time = frame / 30). `0x005F71D0` finds the keys on either side of the cut time t. It works out how far t is between them (s, 0 to 1). Then it takes the earlier key when s < 0.5 and the later key otherwise. It never blends them (read). The arrival cutscene's track has a key on every frame (inferred from the cadence). So its camera moves 30 times a second at any frame rate.
- **Everything else follows the frame time.** The cutscene clock advances by the frame's dt. Characters and props are posed from their Havok animations at that same t. The field of view is a smooth Hermite curve. Subtitles and events read the same clock (all read). In the bursts, the character moves on every present (measured).
- **That is the judder.** At 120 fps the background holds for three presents and jumps on the fourth, while the character keeps walking. On screen the character creeps against the frozen camera, then snaps back when the camera catches up (+4, +4, +4, -18). At 60 fps the background moves on every other present and the character goes back and forth (-3, +3) (measured).
- **A correction.** docs/28 and the comment above `fps_on_flip_request` in `port/des_options.cpp` say "RemoMan moves things at 30 Hz while the animation follows the frame time". RemoMan's clock and the objects it poses are per frame. Only the camera is stepped.
- **Recommended fix:** three instruction hooks in `0x005F71D0` (one per channel: position, rotation, scale). Each blends the two keys by s instead of picking one. Put this behind an opt-in `DES_CUTFPS=1`, which also skips today's cutscene 30 fps lock. Effort is about 1.5 days, including the re-lift, rebuild and test runs. Risk is low. The default stays the 30 fps lock, as the game shipped.

## 1. The cutscene pipeline

### Read

**The clock.** The frame update `0x0001B8D0` reads dt from `0x018C8DB4`; the port writes that float at each flip request. It calls the task manager `0x00138A98(*0x01B387B8, dt)`, which runs each group's objects with f1 = dt. The node execute `0x00135900` calls `obj->vtable[3](obj, dt, node)` every frame for nodes in state 1 or 2. It has no accumulator and skips no frames. RemoMan is registered in group 3 at priority 3, and RemoStep in group 3 at priority 2. Both are standard step machines (dispatchers `0x001A24D8` and `0x001A5EB8`).

**Playback** is RemoStep step 7, `0x001A5B28`, every frame:
- `0x00612300(scene, dt)` advances the scene. Per cut, `0x00611F98` calls `0x005FD730(cut, dt)`. It passes 0.0 instead when the pause flag `scene+0x17` is set.
- `0x005FD730` advances the cut time: t = `cut+0x1C` + dt, clamped to the cut's length (`0x005F9EE0`). Then it samples everything at that t:
  - the camera, `0x00615208(*(cut+8), t, 1)` (call at `0x005FD818`);
  - the parts, `0x00601A80` (local time) then `0x006019D0` (Havok pose sample through the skeleton's vtable);
  - the entries, `0x00603188` then `0x006060C0`. These are models bound to the player's equipment; they get bone matrices at `entry+0x6C` and `entry+0x70`;
  - the events (SFX, SE, tae), through `0x003D7888`..`0x003D79E8`, tested against time windows.
- `0x00611F98` copies the camera matrix at `(cut+8)+0x10` to `scene+0x2310`, and the field of view at `(cut+8)+0xC` to `scene+0x2350`. A cut change goes through `0x005FCA10` and `scene+0x2354`.
- Back in step 7, the matrix goes to `RemoStep+0x70` and the field of view to `RemoStep+0xB0`.
- Subtitles: `0x00720EF0` and `0x007232E8` with the id at `scene+0x22C4` and the time at `scene+0x22C8`. They follow the cut clock.

**The camera handoff**, every frame:
- RemoMan step 4, `0x001A26F8`, calls `0x00218E90` at `0x001A2910`. That copies `RemoStep+0x70` into the camera object at `camobj+0xE0` (4 rows), stores the field of view at `+0xDC` and sets the flag at `+0xD8`. The camera object is `*(RemoMan+0x1C)`.
- The in-game step `0x00144160` calls `0x00219BF0(camobj, dt)` at `0x00144B68`. When the flag is set, its cutscene branch (`0x0021AB28`) copies `camobj+0xE0` into the two cameras at `camobj+4` and `camobj+8` (+0x10 each) and the field of view to their +0x50.

None of these links holds a value from one frame to the next.

**The 30 Hz step: `0x005F71D0`.** `0x00615208` samples the track at t plus a fixed per-cut offset (an integer frame from the camera data, divided by 30). It calls `0x00614DE8` for the matrix (call at `0x0061526C`) and `0x00614EE0` for the field of view (`0x006152EC`). `0x00614DE8` calls `0x005F71D0` at `0x00614E30`. That is the function's only caller.

Data, with D = `*(camtrack+4)`:
- Key list: `*(*(D+0xC)+0x38)` points at {+0 key array, +4 key count, +8 kind}. Kind 3 means keyed; any other kind gives the default (0, 0, 0, 1) for every channel.
- Key: 0x20 bytes. +0x00 frame (int). +0x04 position index. +0x10 rotation index. +0x1C scale index.
- Channel header H = `*(D+8)`. +0x14 points at a table of 12-byte x, y, z entries; an index selects one entry. +0x2C, +0x2F and +0x32 are one byte per channel. When the byte is 0 the channel reads the table; otherwise it reads as zero.

What it does:
1. The search loop (`0x005F76F0`) walks the keys. Key time = frame / 30.0 (30.0 at `0x018EA6D8`). It keeps the last key at or before t (t0) and the first key at or after t (t1).
2. At `0x005F724C` and `0x005F7250`: s = (clamp(t, t0, t1) - t0) / (t1 - t0) into f10, with t1 - t0 in f11. When t0 = t1, s = 0.
3. For each channel it copies both keys' vectors to the stack (w = 1.0). Then it compares s with 0.5 (the float at `0x018EA834`) and loads one of the two:

   | Channel | Load | Earlier key | Later key | After the load |
   |---|---|---|---|---|
   | position | `lvx v4` at `0x005F72EC` | `r1+0xA0` | `r1+0xB0` | none |
   | rotation, radians | `lvx v12` at `0x005F7394` | `r1+0xD0` | `r1+0xA0` | wrapped to [-pi, pi]: x - 2pi * round(x / 2pi), constants at `0x01709F00` (1/2pi) and `0x01709EC0` (2pi) |
   | scale | `lvx v2` at `0x005F743C` | `r1+0x70` | `r1+0xA0` | none |

   The opcodes at the three addresses were checked in the EBOOT (`7C8048CE`, `7D8048CE` and `7C4048CE`).
4. `0x000562F8` builds a 4x4 matrix from v2 (on the diagonal, so a scale), v3 (Euler angles, through `0x00054D48`) and v4 (translation). `0x00614DE8` turns that into the view matrix: it negates one axis and rebuilds another with a cross product.

So the camera's position and rotation are the nearest key's, exactly. The switch to the next key happens at t = (k + 0.5) / 30.

**The field of view is smooth.** `0x00614EE0` calls `0x005F65F8`. Its keys are 16 bytes: frame, value and two tangents. It evaluates a cubic Hermite at s, then converts the result through tan and atan. With no keys it returns 0.7505 rad (`0x018EB038`).

### Inferred

- The arrival cutscene's camera track has a key on every frame (section 2 shows one camera move per 1/30 s). A track with sparse keys would show long holds and a jump halfway between keys. That is the same at 30 fps, so it is part of how the cut was made.
- On the PS3 at 30 fps, t lands on whole frames and s is 0 every frame, so the nearest key is the exact key. Nobody could see the step there.
- Gameplay at 120 fps does not judder this way because the gameplay cameras are not sibcam tracks.

## 2. The judder, explained

### Measured

These are existing bursts in `work/tmp/cut_c120`, `cut_c60` and `cut_c30` (game-derived; never commit them). Each burst is 8 consecutive presents of the arrival cutscene, taken N seconds after the level's script opened. They come from a build before the 30 fps lock, at `DES_FPS=120`, 60 and 30. The 120 run's log says "real frame time, no skip frames, vblank at the cap" and "[vblank] clock thread at 120.000 Hz". Shifts are horizontal pixel offsets between consecutive presents, found by phase correlation on Hann-windowed crops. Top, left and right are background (fog, walls, sky). Centre is mostly the walking character.

| Burst | Region | x shift between presents (px) |
|---|---|---|
| 120 fps, 12 s | top / left / right | -12,0,0,0,-12,0,0 / -14,0,0,0,-14,0,0 / -10,0,0,0,-10,0,0 |
| | centre | -9,+2,+2,+2,-9,+2,+2 |
| 120 fps, 18 s | top / left / right | 0,0,0,-18,0,0,0 / 0,0,0,-8,0,0,0 / 0,0,0,-8,0,0,0 |
| | centre | +4,+4,+4,-18,+4,+4,+4 |
| 120 fps, 6 s | every region | moves only between presents 2-3 and 6-7 |
| 60 fps, 12 s | top / left / right | -8,0,-8,0,-8,0,-8 / -10,0,-10,0,-10,0,-11 / -5,0,-5,0,-6,0,-6 |
| | centre | -3,+3,-3,+3,-3,+3,-4 |
| 60 fps, 18 s | left / right | 0,0,-8,-8,0,-6,0 / 0,0,-6,-10,0,-8,0 |
| 30 fps, 12 s | top / left / right | -10 to -12 / -12 to -14 / -8 to -10, every present |
| | centre | -1 to -3, every present |
| 30 fps, 18 s | centre | -6 to -8, every present |

What the numbers show:
- At 120 the background is exactly still for three presents, then moves. At 60 it moves on every other present. At 30 it moves on every present. That is one camera move per 1/30 s.
- At 60, the 18 s burst once moves on two presents in a row. The cadence follows the cut clock, which uses the real frame time. It does not follow the present count.
- The character moves on every present at every rate.

### Inferred: the model

At 120 the cut clock gains 1/120 s per present. The character, posed at t, walks +4 px a present on screen against a camera that is not moving. On every fourth present t crosses a half-frame and the camera jumps one key. The background shifts -18 px. At the character's depth the shift is about -22 px, so that present nets +4 - 22 = -18 for the character. Over four presents the character nets about -6 px. That is one 30 fps frame's worth: the 30 fps bursts show -6 to -8 px per present in the centre at 18 s.

At 60 it is +3 on the held present. On the next, +3 plus a camera shift of about -6 at the character's depth gives -3. docs/28's centre-crop numbers (+4, +4, +4, -18 and -4, +3) are this same pattern.

Blending the two camera keys removes both effects together. The camera would move a quarter key per present at 120, in time with the character.

## 3. Options

### (a) Blend the two camera keys (recommended)

The cut clock is already real time and every other sample already interpolates. So "real-time clock with interpolated samples" comes down to one change: blend the camera keys.

**Hooks** (ps3recomp `--insn-hooks`, the same tool as `des_cam_rate`; see docs/28, "Camera catch-up at any frame rate"). Add to `port/insn_hooks.txt`:

```
# Cutscene camera (sibcam) keys. 0x005F71D0 loads the nearest 30 Hz key
# (s < 0.5 ? earlier : later); with DES_CUTFPS the hook blends the two by s.
0x005F72EC=des_cut_cam   # lvx v4: position (earlier r1+0xA0, later r1+0xB0)
0x005F7394=des_cut_cam   # lvx v12: rotation in radians, before the wrap (earlier r1+0xD0, later r1+0xA0)
0x005F743C=des_cut_cam   # lvx v2: scale (earlier r1+0x70, later r1+0xA0)
```

**Host function** (`port/des_options.cpp`), in outline:

```cpp
extern "C" void des_cut_cam(ppu_context* ctx, uint32_t addr)
{
    if (!cutfps_on()) return;                          /* DES_CUTFPS, live */
    const double s = ctx->fpr[10];                     /* (t - t0) / (t1 - t0) */
    const double seg = ctx->fpr[11];                   /* t1 - t0, seconds */
    if (!(s > 0.0 && s < 1.0) || std::fabs(seg * 30.0 - 1.0) > 0.01) return;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    uint32_t a, b; int v; bool angle = false;
    switch (addr) {
    case 0x005F72ECu: a = sp + 0xA0; b = sp + 0xB0; v = 4; break;
    case 0x005F7394u: a = sp + 0xD0; b = sp + 0xA0; v = 12; angle = true; break;
    case 0x005F743Cu: a = sp + 0x70; b = sp + 0xA0; v = 2; break;
    default: return;
    }
    float out[4];
    for (int i = 0; i < 4; i++) {
        const float x = vm_readf(a + 4 * i), y = vm_readf(b + 4 * i);
        float d = y - x;
        if (angle) d -= 6.2831853f * std::nearbyint(d / 6.2831853f);   /* the short way round */
        out[i] = i < 3 ? x + d * (float)s : x;
    }
    /* + the cut guard below */
    /* store out[] into ctx->vr[v] as four big-endian floats, the layout lvx left */
}
```

Notes:
- **Segments longer than one frame keep the nearest key.** Those are holds or jumps that were made on purpose, and blending them would turn a cut into a slow pan.
- **Rotation** is blended on the angles, the short way round. The code after the hook wraps the result again. Per-key changes are small, so this matches a proper rotation blend closely enough.
- **Cut guard** (cheap insurance): in the position hook, skip the blend when one 1/30 s step moves the camera more than about 2 m. Remember that for the rotation and scale hooks of the same call; position is always evaluated first. Also skip the rotation blend when an angle turns more than about 0.5 rad in one step. Log each skip under `DES_CUTLOG=1` to learn whether any cutscene needs it. Each remo cut has its own track, so a jump inside one track should be rare.
- **The knob.** `DES_CUTFPS=1` (live, `PS3_KNOB`) does two things. It makes `fps_on_flip_request` skip the cutscene switch, the way the scratch `DES_DIAG_NOCUTLOCK` does today. And it turns the hooks on. Unset or 0 keeps today's behaviour exactly: the lock, and hooks that return at once. A settings-window checkbox can follow later.
- **Leave the 0.5 at `0x018EA834` alone.** It is a shared constant of that file's float pool. Patching it cannot blend anyway.
- **Another way:** `--override-funcs` could replace `0x005F71D0` with a host version. That means rewriting the key search, the channel bytes and the vector call to `0x000562F8`. It is more code and more risk than three hooks that change only the loaded vector.

**Effort:** about 1.5 days.
- Hook, knob and log: 2-3 hours.
- Re-lift with the new hook list: changing `insn_hooks.txt` touches `ppu_recomp.h`, so all 14 chunks rebuild (~15 min).
- Run requests R1-R7: half a day.
- A look at a few other cutscenes (boss intros, the Nexus) for sparse tracks or guard hits: half a day.
- docs/28 and the source comment fixed, PR.

**Risk:** low. See section 4.

### (b) Keep 30 Hz, interpolate the rendered transforms (not recommended)

Make the in-between frames in the port. Match draws across two game frames, then blend their transform constants and skinning matrices in the RSX/D3D12 path. Much of the per-draw state is built on the SPUs and in the HighGraphics ring, so matching is hard, and every effect, particle and post pass would need a rule. It also adds a frame of latency. Effort is weeks, and the risk of artefacts is high. Section 1 shows the game already has everything needed at the source.

### (c) Clamp the animation to RemoMan's clock (keep as the default)

Keeping everything on the cutscene's 30 Hz step is what the current lock does: 30 mode, a 1/30 step and the PS3's vblank while a RemoMan slot is full (`fps_on_flip_request`, `cutscene_playing`, slots at `*0x01B3A28C` +0x28/+0x40/+0x58/+0x70). Stepping only the animation while rendering faster would still show a new picture only 30 times a second. Nothing is gained over the lock. The lock stays the default and the fallback.

## 4. Risks of (a)

- **Audio and subtitles.** Subtitles read the cut clock (read), so they stay with the picture. Voice and music start from cut events and then play in real time. No sync between the cut clock and the audio clock was found in the code read. Unlocked, the cut clock follows real frame time, but the port clamps dt to 50 ms. A hitch longer than that leaves the picture behind the sound by the rest. The lock has the same kind of drift: there a 100 ms frame still counts as 33 ms. Check by ear at the end of a long cutscene (R7).
- **Frame events.** Events are tested against time windows on the cut clock (read). More frames means the same events at finer times. No test against an exact frame number was found in the cutscene code read.
- **Camera cuts.** Each cut has its own track. A cut change goes through `0x005FCA10`, which samples the new cut's own track (it calls `0x00615208` at `0x005FCA58`). `0x005F71D0` reads the keys of one track per call, so the blend never mixes two tracks (read). A jump inside one track would blend over one 1/30 s step (one to three in-between presents at 60-120). The guard covers it.
- **Physics.** Cloth, hair and ragdolls step with the frame dt, as in gameplay at 120. The step is probably `0x001D1E30`, which `0x0001B8D0` calls with dt. That path is already tested by playing.
- **Loads during a cutscene.** A cutscene's slot fills during the level load, so today the lock covers the load screen too. With `DES_CUTFPS=1` the load runs unlocked like any other. Loading itself does not depend on the frame rate.
- **Movies.** cellSail movies (the intro, the endings) are not RemoMan. They keep their own clock and stay 30 fps content. Neither the lock nor this change touches them.
- **Other cutscene channels.** `0x005FD730` samples only the camera, parts, entries and events. Lighting or fog driven by other curves was not traced. If the after-fix bursts still show a 30 Hz step in brightness or fog, look there next.
- **The follow cameras.** `des_cam_rate` does nothing in a cutscene today because dt is 1/30 there. With `DES_CUTFPS=1` it sees the real dt. That is correct if the follow camera keeps updating under the cutscene. Check the handover to gameplay at the end (R7).
- **Knob off.** The lock is as today, and the hooks return before touching anything.

## 5. Run requests

R1-R3 use the existing `port/build_cam/des_boot.exe` (built 2026-10-18), which has the scratch `DES_DIAG_NOCUTLOCK` knob. They were run on 2026-10-18; the results come first, below. R4-R7 need a build with the hooks; `port/build_cutfps` stands for it below.

### R1-R3 results (2026-10-18)

The runs used the commands below from a scratch copy of the harness, one run at a time, each with four bursts of 8 consecutive presents (bursts 1-4, at about 6, 13, 19 and 26 s). The frame logs show each run held its rate: median step 8.34 ms at 120 (p90 8.63), 16.70 ms at 60 (p90 19.06) and 33.45 ms at 30 (p90 42.73). The player's save was unchanged after each run.

| Run | Burst | Region | x shift between presents (px) |
|---|---|---|---|
| R1, 120 | 1 | top / left | -1,0,0,0,-1,0,0 / -3,0,0,0,-3,0,0 |
| | 2 | top / left / right | 0,0,0,-12,0,0,0 / 0,0,0,-15,0,0,0 / 0,0,0,-12,0,0,0 |
| | | centre | +4,+4,+3,-8,+4,+4,+4 |
| | 3 | top / left / centre | 0,0,-24,0,0,0,-20 / 0,0,-28,0,0,0,-26 / 0,0,-22,0,0,0,-22 |
| | 4 | top / left | +12,0,0,0,+11,0,0 / +13,0,0,0,+12,0,0 |
| R2, 60 | 2 | top / left / right | -7,0,-7,0,-7,0,-7 / -10,0,-10,0,-10,0,-10 / -5,0,-5,0,-5,0,-5 |
| | | centre | -3,+3,-3,+3,-3,+3,-4 |
| | 3 | left / right | -10,0,-9,0,-9,-8,-7 / -10,0,-8,0,-8,-8,-8 |
| R3, 30 | 1 | left | -2 on every present |
| | 2 | top / left / right | -6 to -7 / -9 to -10 / -4 to -5, on every present |
| | 3 | every region | -9 to -53, on every present (a fast camera move; low correlation peaks) |
| | 4 | top / left | +14 falling to +10 / +16 falling to +12, on every present |

What this shows:
- Section 2 holds on today's build. At 120 the background holds for three presents and moves on the fourth, in every burst, and the centre creeps +4 then snaps back. At 60 it moves on every other present, with the -3, +3 centre pattern. At 30 it moves on every present.
- At 60, burst 3 moves on three presents in a row at the end. The cadence follows the cut clock (real frame time), not the present count.
- The bursts do not land at exactly the same cut time in each run, because they are timed from the level script and the load time varies. Burst 4 lines up best: R1's jump (+12, with a +15 vertical shift) is the size of one R3 present (+14 to +11, +15). Compare the other bursts by cadence, not by size.
- **Reference for R4 and R5: burst 4.** With the fix, expect about +3 px on every present at 120 (a quarter of R1's +12 jump) and about +6 at 60, with no zero holds in between.

Common setup:
- Run from the repository root.
- `levelcap.sh` plays from the pinned regression save (`work/tmp/regress/save`) and puts the player's save back when it exits. It sets `RSX_PRESENT` from the lock state by itself.
- It stops pressing buttons once the level's script opens, so the arrival cutscene plays and is not skipped.
- Give each run its own name: levelcap clears `lc_<name>` at start, and that folder holds the save backup.
- An agent should point `DES_SCRATCH` at its scratchpad instead of `work/tmp/cutfps`.
- The bursts need consecutive presents. So: `LC_EVERY=1`, `CELLMARK_DUMP_SKIP=100000000` (no periodic dumps) and `CELLMARK_DUMP_CHANGED=0` (keep the identical presents; they are the signal).

**R0. The burst script** (once). It takes 8 presents at about 6, 13, 19 and 26 s into the cutscene:

```
Set-Content -Encoding ascii work/tmp/cutfps_bursts.txt -Value "setenv CELLMARK_DUMP_NOW=8","sleep 2","setenv CELLMARK_DUMP_NOW=0","sleep 4","setenv CELLMARK_DUMP_NOW=8","sleep 2","setenv CELLMARK_DUMP_NOW=0","sleep 4","setenv CELLMARK_DUMP_NOW=8","sleep 2","setenv CELLMARK_DUMP_NOW=0","sleep 4","setenv CELLMARK_DUMP_NOW=8","sleep 2","setenv CELLMARK_DUMP_NOW=0"
```

**R-A. The analysis script.** It is not in the repository. Add it as `tools/des/cutjudder.py` in a tooling commit, separate from the fix, or run it from a scratch copy. It prints each region's shift between consecutive presents for each burst of 8:

```python
# cutjudder.py <shots dir> [n]: per-region shift between consecutive presents
# (phase correlation), for each burst of n dumped frames (default 8).
import glob, os, sys
import numpy as np
from PIL import Image

REGIONS = {  # (y0, y1, x0, x1) as fractions of the frame
    'top': (0.00, 0.25, 0.00, 1.00), 'left': (0.20, 0.80, 0.00, 0.25),
    'right': (0.20, 0.80, 0.75, 1.00), 'centre': (0.25, 0.75, 0.25, 0.75),
    'bottom': (0.75, 1.00, 0.00, 1.00)}

def crop(a, y0, y1, x0, x1):
    h, w = a.shape
    c = a[int(h * y0):int(h * y1), int(w * x0):int(w * x1)]
    c = c - c.mean()
    return c * np.hanning(c.shape[0])[:, None] * np.hanning(c.shape[1])[None, :]

def shift(a, b):
    f = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    r = np.fft.ifft2(f / (np.abs(f) + 1e-9)).real
    y, x = np.unravel_index(r.argmax(), r.shape)
    h, w = r.shape
    return (x - w if x > w // 2 else x), (y - h if y > h // 2 else y), round(float(r.max()), 2)

files = sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.bmp')))
n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
for k in range(0, len(files) - n + 1, n):
    ims = [np.asarray(Image.open(f).convert('L'), dtype=np.float32) for f in files[k:k + n]]
    print('burst %d from %s' % (k // n, os.path.basename(files[k])))
    for name, rr in REGIONS.items():
        cs = [crop(a, *rr) for a in ims]
        s = [shift(cs[i + 1], cs[i]) for i in range(n - 1)]
        print('  %-6s dx %s dy %s peak %s' % (name, [v[0] for v in s], [v[1] for v in s], [v[2] for v in s]))
```

**R1. Baseline at 120, lock off** (today's exe):

```
& "C:\Program Files\Git\bin\bash.exe" -c "DES_SCRATCH=work/tmp/cutfps LC_SETTLE=5 LC_MAX=300 LC_EVERY=1 tools/des/levelcap.sh base120 work/tmp/cutfps_bursts.txt DES_EXE=port/build_cam/des_boot.exe DES_FPS=120 DES_DIAG_NOCUTLOCK=1 CELLMARK_DUMP_SKIP=100000000 CELLMARK_DUMP_CHANGED=0 DES_FRAMELOG=work/tmp/cutfps/frames_base120.txt"
```

Then `python tools/des/cutjudder.py work/tmp/cutfps/lc_base120/shots`.

| Result | Meaning |
|---|---|
| top/left/right: three 0s, then a jump, repeating; centre: small creep, then a snap | the camera step as in section 2, on today's build |
| background moves every present | something else changed; re-read section 1 against the new lift |
| `step_ms` in the frame log well above 8.3 during a burst | the dumps slowed the frames. Read the cadence against the logged steps, not the present count |

**R2. Baseline at 60:** R1 with `DES_FPS=60`, name `base60`, frame log `frames_base60.txt`. Expect background moves on about every other present, with an occasional double.

**R3. Reference at 30:** R1 with `DES_FPS=30`, name `base30`. Expect every present to move, steadily. Keep its per-present background shift; R4 and R5 are compared with it.

**R4. The fix at 120:** R1 with `DES_EXE=port/build_cutfps/des_boot.exe DES_CUTFPS=1` in place of `DES_DIAG_NOCUTLOCK=1`, name `fix120`.

| Result | Meaning |
|---|---|
| Background moves on every present by about a quarter of R3's shift (for example -3, -3, -3, -3 where R3 shows -12); centre steady, no sign flips | fixed |
| Still three 0s and a jump | the hooks are not running: check the lifted `func_005F71D0` for the three `des_cut_cam` calls, and `DES_CUTFPS` |
| Smooth, but one present per burst has a low peak and a big jump | a real shot change, or a jump the guard missed; see `DES_CUTLOG=1` |
| Smooth camera, but brightness or fog still steps at 30 Hz | another stepped cutscene curve (section 4, "Other cutscene channels") |

**R5. The fix at 60:** R4 with `DES_FPS=60`, name `fix60`. Expect half of R3's shift on every present.

**R6. Knob off on the new exe:** R4 without `DES_CUTFPS`, name `fixoff120`. Expect the log to show `[des] DES_FPS: cutscene, 30 fps`, and bursts like the 30 fps lock (steady). Then run regress at the default and at `DES_FPS=120`:

```
& "C:\Program Files\Git\bin\bash.exe" -c "DES_EXE=port/build_cutfps/des_boot.exe tools/des/regress.sh"
```

```
& "C:\Program Files\Git\bin\bash.exe" -c "DES_EXE=port/build_cutfps/des_boot.exe DES_FPS=120 tools/des/regress.sh"
```

**R7. Eyes and ears** (the user, with sound):

```
& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 DES_EXE=port/build_cutfps/des_boot.exe DES_FPS=120 DES_CUTFPS=1"
```

Watch any in-engine cutscene: the one at a level load, a boss intro, the Maiden in the Nexus. Check:
- the camera pans smoothly and the character no longer vibrates;
- the voice matches the lips and the subtitles at the end of the cutscene;
- shot changes are clean cuts, with no quick swoop between shots;
- the handover to gameplay looks as it does at 30.

### R4-R6 results (2026-10-18)

The runs used the lane build of `feat/cutscene-camera-blend` (`port/build_cutfps` below) and the same harness as R1-R3, one run at a time. In the cutscene, the frame logs show fix120 at a median step of 8.34 ms (p90 8.68), fix60 at 16.67 ms (p90 17.06) and fixoff120 at 33.37 ms (p90 33.78), locked at 30 as today. The player's save was unchanged after each run.

| Run | Burst | Region | shift between presents (px) |
|---|---|---|---|
| R4, 120 | 1 | left / bottom | x -1 / x +2, on every present |
| | 2 | top / left / right | x -4 on every present, in all three |
| | 3 | top / left / right / centre | x -4 to -3 / -5 to -4 / -3 / -4, on every present |
| | 4 | top / right / centre | y +1 on every present |
| R5, 60 | 1 | left / bottom | x -1 / x +3 then +6 to +5, on every present |
| | 2 | top / left / right | x -4 / -5 / -2, on every present |
| | 3 | top / left / right / centre | x -4 / -6 to -4 / -6 to -4 / -4 to -3, on every present |
| | 4 | top | y 0, +2, +2, +2, +6, +4, +2 |
| R6, 120, knob off | 1 | left / bottom | x -3 / x +9 to +11, on every present |
| | 2 | top / left / right | x -10 to -14 / -13 to -17 / -10 to -14, on every present |
| | 3 | every region | x -26 to -63, on every present (the fast camera move) |
| | 4 | top | y +4 on every present |

What this shows:
- The fix holds. At 120 and at 60 the background moves on every present, with no zero holds and no jumps; R1 held for three presents and jumped on the fourth, and R2 held on every other present.
- The speed matches the 30 fps lock. Burst 4 lands in a vertical pan in these runs: the top moves +4 px a present at 30 (R6) and +1 a present at 120 (R4), the same 120 px a second. The bursts land at slightly different cut times than in R1-R3, so the +3 px expected from R1's burst 4 does not apply as such.
- R5's burst 4 varies from present to present (0 to +6). The blend follows the cut time, which follows real frame time, so a long frame moves the camera further.
- `[cutlog]`: at 120, 3000 calls between keys, 2996 blended, keys apart 0, jumps 4, one index unset 0, loads holding neither key 0. At 60, 1000 calls, 998 blended, jumps 2, the rest 0. Both runs log one jump at t = 13.3 s, a 0.01 m move with a 3.14 rad turn. That is most likely the rotation's Euler angles flipping by pi rather than a real turn (not checked). Either way the guard kept the nearest key for that step, as designed, so the camera did not swing through the blend.
- The log lines are as expected: `cutscene, unlocked (DES_CUTFPS)` and `keys blended (first at t=0.009 s)` at 120 (0.017 s at 60); `cutscene, 30 fps` with the knob off.
- Regress (R6's two commands, with the lane build): both pass, all four shots ok against the accepted baseline.
- R7 (eyes and ears, with sound) is still to do.

## 6. Follow-ups once (a) lands

- docs/28, "Cutscenes judder above 30": RemoMan does not move things at 30 Hz. The camera track is sampled by nearest key. Fix the matching comment above `fps_on_flip_request` in `port/des_options.cpp` (and the summary at the top of that file). PR #50 does this and adds `DES_CUTFPS`.
- Whether `DES_CUTFPS` later follows `DES_FPS` by default is the user's call after R7. The default stays as shipped.
- A "Cutscenes at full frame rate" checkbox in the settings window.

## 7. Address table

| Address | What | Use |
|---|---|---|
| 0x018C8DB4 | frame dt (float), written by the port at each flip request | reference |
| 0x0001B8D0 | frame update: dt to the task manager and physics | reference |
| 0x00138A98, 0x00135900 | task manager update; node execute (vtable[3] with dt, every frame) | reference |
| 0x01B3A28C | RemoMan object pointer; slots +0x28/+0x40/+0x58/+0x70 | lock detection (exists) |
| 0x001A24D8, 0x001A5EB8 | RemoMan and RemoStep step dispatchers | reference |
| 0x001A5B28 | RemoStep step 7 (playback); camera to RemoStep+0x70 / +0xB0 | reference |
| 0x001A26F8 | RemoMan step 4; calls 0x00218E90 at 0x001A2910 | reference |
| 0x00218E90 | camera into camobj+0xE0 (matrix), +0xDC (field of view), flag +0xD8 | reference |
| 0x00144160, 0x00144B68 | in-game step; its call to 0x00219BF0 | reference |
| 0x00219BF0, 0x0021AB28 | camera object update; its cutscene branch | reference |
| 0x00612300, 0x00611F98 | scene update; per-cut update (camera to scene+0x2310 / +0x2350) | reference |
| 0x005FD730 | cut update: t = cut+0x1C + dt; samples camera, parts, entries, events | reference |
| 0x00601A80, 0x006019D0 | part local time; Havok pose sample | reference |
| 0x00603188, 0x006060C0 | entries: pose to bone matrices | reference |
| 0x003D7888..0x003D79E8 | cut event accessors (time windows) | reference |
| 0x00720EF0, 0x007232E8 | subtitles (scene+0x22C4 id, +0x22C8 time) | reference |
| 0x00615208 | camera track sample at t plus the per-cut offset | reference |
| 0x00614DE8 | view matrix from the track; calls 0x005F71D0 at 0x00614E30 | reference |
| 0x005F71D0 | key search and nearest-key choice: the 30 Hz step | hook site |
| 0x005F724C, 0x005F7250 | t - t0; s into f10 (segment length in f11) | read by the hook |
| 0x005F72EC | `lvx v4`, position key | hook |
| 0x005F7394 | `lvx v12`, rotation key (radians, before the wrap) | hook |
| 0x005F743C | `lvx v2`, scale key | hook |
| 0x018EA834 | 0.5, the choice threshold (shared constant) | do not patch |
| 0x018EA6D8 | 30.0, frames to seconds | reference |
| 0x000562F8, 0x00054D48 | 4x4 from scale, Euler rotation and translation; Euler to rotation | reference |
| 0x00614EE0, 0x005F65F8 | field of view: Hermite curve; 0.7505 rad default at 0x018EB038 | reference |
