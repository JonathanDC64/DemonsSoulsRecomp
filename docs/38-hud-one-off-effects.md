# HUD effects that move per frame, not per second (research, 2026-10-18)

With `DES_FPS` above 30, some HUD effects run too fast. They move a fixed amount on every update or draw call, and ignore the frame time. At 240 fps such an effect runs 8 times as fast as at 30 fps.

This doc covers the HUD classes on this lane's list: the soul counter, the HP/MP/stamina bars, the boss and enemy bars, the lock-on mark, item pickup, the save icon, captions and the death screen. It was read from the code only. Nothing was run. Lines that are guesses say so.

**Status.** Three test runs have since confirmed candidates 1 to 3, and they are fixed: docs/35, "HUD effects", has the fix and the numbers. Candidate 4 needs no fix. Section 4 is the plan as it was written. The runs drove the game from a script instead, because the test save is in the tutorial, where you cannot die.

Sources: the disassembly of `work/game/EBOOT.ELF`, checked against the lift (`work/recomp/ppu_recomp_003.cpp`), and Ghidra decompiles (`tools/ghidra/dump.sh` on `work/ghidra/DeS`).

Terms used below:
- **Update** is vtable slot 17 (+0x44): r3 = this, f1 = dt (the frame step).
- **Message**: objects take messages through slot 25 (+0x64): r4 = id, r5 points at {vtable, value, type}.
- **Per call** means the field moves once each time the function runs, whatever dt is.
- **r30** is the function's constants base, loaded from the TOC.

## 1. Candidates

| # | Effect | Class and function | Field | Step per call | Instructions | Visible? |
|---|---|---|---|---|---|---|
| 1 | Soul counter rolls up after a soul gain | DsMenuDlgFEGetSoul, update 0x007A8C60 | +0xFC, int | 1, 2, 12, 112, ... depending on how much is left. Immediates in the code (0x007A8D24..0x007A8D74) | add 0x007A8D7C, store 0x007A8D80 | Yes. +1,000 souls: 4.3 s at 30 fps, 0.54 s at 240 |
| 2 | HP damage trail, heal fill, MP and stamina trails, boss and enemy HP bars | DsMenuDlgObjSmoothGauge, draw callbacks 0x0078EDC8, 0x0078EB60, 0x0078F020 | +0x3C, float (fill shown) | Down: +0x44 (default 0.012 at 0x018F2C2C). Up: +0x4C (0.048 at 0x018F3BD0 or 0.038 at 0x018F3BDC; default 10.0, which is instant) | slot 6: down 0x0078ED18/0x0078ED20, up 0x0078EDAC/0x0078EDB4 (all sites in 2.2) | Yes. The HP trail drains 8 times as fast at 240 |
| 3 | Bar length eases when max HP/MP/stamina changes | DsMenuDlgObjGauge, update 0x00789BC0 (also run by every ObjSmoothGauge) | +0x30, float (length in px) | Half the gap while over 2 px (0.5 at 0x018F2A04), then 1 px (0x018F29FC) | down 0x00789C64/0x00789C68, up 0x00789CE8/0x00789CEC | A little. 200 px: 0.3 s at 30 fps, 38 ms at 240 |
| 4 | Short wait before a caption fades out | DsMenuDlgCaption, update 0x008E4968 | +0xF8, int | −1, from 2 | 0x008E4AF8/0x008E4AFC | No (2 frames) |

Outside the list, one more per-call step is already being fixed: the menu text scroll, DsMenuDlgObjScrollMsg +0x20, −1.5 px per call. The working tree's `scroll_msg_rate` handles it (see section 3).

## 2. Details per class

### 2.1 Soul counter: DsMenuDlgFEGetSoul (update 0x007A8C60)

Vtable 0x018696C0. r30 = 0x018FB7A8 (TOC 0x019ADE54). Parts: `text_param_soul` (the total, u"%d") and `text_param_getsoul` (the gain, u"+%d").

Fields (read from code):

| Field | Type | Meaning |
|---|---|---|
| +0xF8 | int | souls before the gain |
| +0xFC | int | how much of the gain has been counted so far |
| +0x100 | int | the gain to count. Flag 0x76 is added to it on each gain |
| +0x104 | int | the "+N" shown. Flag 0x77 is added to it |
| +0x108 | float | hold timer |
| +0x110 | float | hold time |
| +0x114 | byte | 1 while idle. The first gain plays the "+N" layout animation (speed 5.0 at 0x018F3830, dt-based) |

Each call:
1. When souls come in (flags 0x77 and 0x76), add them to +0x104 and +0x100, and set +0x108 to 0 (0x007A8CEC..0x007A8D10).
2. While +0xFC differs from +0x100, add one step to +0xFC (0x007A8D7C, stored at 0x007A8D80). Then clamp it at +0x100 (0x007A8D90/0x007A8D94). This path never reads dt.
3. When they are equal, add dt to +0x108 (0x007A9020..0x007A9030). When it reaches +0x110, all fields are cleared and +0xF8 becomes the player's souls (`*(*(*0x01B4EF9C + 8) + 0x78)`, 0x007A905C..0x007A9070).
4. The total shown is +0xF8 + +0xFC (0x007A8E14).

The step depends on d = +0x100 − +0xFC, the amount still to count. The numbers are immediates (lis/ori/addi at 0x007A8D24..0x007A8D74 and 0x007A9078..0x007A9290), not data:

| d | step |
|---|---|
| 1 to 10 | 1 |
| 11 to 100 | 2 |
| 101 to 1,000 | 12 |
| 1,001 to 10,000 | 112 |
| 10,001 to 100,000 | 1,112 |
| 100,001 to 1,000,000 | 11,112 |
| 1,000,001 to 10,000,000 | 111,112 |
| 10,000,001 to 100,000,000 | 1,111,112 |
| over 100,000,000 | 11,111,112 |

When d is negative, the same steps apply with the sign flipped. Whether spending souls goes through this path was not read (guess: it may).

How long a roll takes, counted from the table:

| Gain | Calls | 30 fps | 240 fps |
|---|---|---|---|
| +50 | 30 | 1.0 s | 0.13 s |
| +1,000 | 130 | 4.3 s | 0.54 s |
| +10,000 | 205 | 6.8 s | 0.85 s |

The hold after the roll is dt-based, so it lasts the same time at any frame rate.

Fix idea: count 30 Hz ticks per object (add dt × 30 each call) and run the step once for each whole tick, with the same table. At 30 fps that is one step per call, as now. The step is an integer, so ticks keep the original numbers; scaling the step would not.

### 2.2 Bar fill: DsMenuDlgObjSmoothGauge (draw callbacks)

Vtable 0x01868C28. r30 = 0x018FABB0 (TOC 0x019ADE20). It is a DsMenuDlgObjGauge (2.3) with a smoothed fill.

Fields (read from code):

| Field | Type | Meaning | Set by |
|---|---|---|---|
| +0x20 | float | target fill, 0 to 1 | message 0x7D0 |
| +0x3C | float | fill shown | message 0x7D6 sets +0x20 and +0x3C together (instant) |
| +0x44 | float | step down per call | 0x7D2. Default 0.012 (0x018F2C2C) |
| +0x4C | float | step up per call | 0x7D8. Default 10.0 (0x018F2C34), which means instant |
| +0x50 | byte | moving | |
| +0x51 | byte | delay is over | |
| +0x54 | float | delay timer | |
| +0x5C | float | delay | 0x7D4. Default 1.5 s (0x018F2C3C) |
| +0x24, +0x40, +0x48 | float | target, shown and step for Y | 0x7D1, 0x7D7, 0x7D3 |

The messages go through ObjGauge's handler 0x0078A390 (0x7D0, 0x7D1, 0x7D5) and this class's handler 0x0078E508 (jump table at 0x0078E5A4 for 0x7D2..0x7D8). The constructor sets the defaults (0x0078E3A0, 0x0078E428, 0x0078E8D0).

The Y path is off. It only runs when +0x48 > 0, and nothing sends 0x7D3.

**The delay is dt-based.** The update 0x0078E2E8 first runs ObjGauge's update (a direct call at 0x0078E340). Then, while +0x50 is set, it adds dt to +0x54 (0x0078E354..0x0078E364). When +0x54 reaches +0x5C, it sets +0x51 and clears +0x54.

**The step is per call, with no dt.** It runs in the draw callbacks, which receive a rect to fit to the fill:
- slot 5, 0x0078EDC8: float rect;
- slot 6, 0x0078EB60: int rect;
- slot 7, 0x0078F020: 16-bit rect.

All three run the same steps for X:
1. If the gap |+0x3C − +0x20| is no more than the step for that direction, +0x3C snaps to +0x20.
2. Otherwise +0x50 is set. Once +0x51 is set, +0x3C moves one step toward the target: down by +0x44, or up by +0x4C. It stops at the target.
3. When the gap is under 1e-8 (0x018F2BB0), +0x50, +0x51 and +0x54 are cleared.
4. Then the rect's right edge is set to left + width × +0x3C.

| Callback | Down (fsubs, stfs) | Up (fadds, stfs) | Y down |
|---|---|---|---|
| slot 5, 0x0078EDC8 | 0x0078EF38, 0x0078EF40 | 0x0078F004, 0x0078F00C | 0x0078EFE0, 0x0078EFE4 |
| slot 6, 0x0078EB60 | 0x0078ED18, 0x0078ED20 | 0x0078EDAC, 0x0078EDB4 | 0x0078ED88, 0x0078ED8C |
| slot 7, 0x0078F020 | 0x0078F290, 0x0078F298 | 0x0078F324, 0x0078F32C | 0x0078F300, 0x0078F304 |

**Calls per frame: two.** The draw routine 0x00F9A938 calls slot 5 on the object (0x00F9AA80), then slot 7 (0x00F9AB04) or slot 6 (0x00F9ABCC) on the same object. The test runs counted two calls a frame on each HUD bar, slot 5 then slot 6.

Which bars use it, from the setup code:

| Dialog (setup) | Part | Delay (0x7D4) | Step up | Step down | Shows (guess) |
|---|---|---|---|---|---|
| PCGauge (0x007B1058) | bar_0_hp | 1.0 s (0x018F3A74) | 0.048 (0x018F3BD0) | 0.012 | the trail left when HP drops |
| PCGauge | bar_1_hp | 0.1 s (0x018F3B58) | 0.038 (0x018F3BDC) | 10.0 (0x018F3BE4), instant | the HP bar: drops at once, fills on a heal |
| PCGauge | bar_0_mp | 1.0 s | instant | 0.012 | MP trail |
| PCGauge | bar_0_sp | 1.0 s | instant | 0.012 | stamina trail |
| BossGauge (0x007A2DB8) | HP_bar0, HP_bar2 | 1.0 s | 0.048 (0x018F3530) | 0.012 (0x018F3538) | boss HP trail |
| BossGauge | HP_bar1 | 0 s | 0.038 (0x018F3540) | 10.0 (0x018F34A8), instant | boss HP bar |
| FEEnemyGauge (0x007A5D58) | HP_bar1 | 0.7 s (0x018F36D0) | instant | 0.012 | HP trail over an enemy |

These are plain DsMenuDlgObjGauge objects, with no smoothing: bar_1_mp, bar_1_sp, the plates, outlines and end bars, and FEEnemyGauge's HP_bar0.

PCGauge's update 0x007B2CD0 sends the targets every frame:
- 0x7D0 (target only, so the bar moves) to bar_1 at 0x007B30EC and to bar_0 at 0x007B3994;
- 0x7D6 (instant) at 0x007B301C when the max changed.

BossGauge's 0x007A2218 sends 0x7D0 at 0x007A2640, 0x007A26A8 and 0x007A271C, and 0x7D6 at 0x007A2B58 and 0x007A2BB0.

Other code that sends 0x7D2, 0x7D4, 0x7D7 or 0x7D8 was not read: 0x0081E260, 0x0081E47C, 0x0090DC30, 0x0090DC44.

Rates with one step per frame:

| Effect | Per call | 30 fps | 240 fps |
|---|---|---|---|
| HP trail (bar_0_hp), after the 1 s delay | −0.012 | 0.36 bar/s | 2.88 bar/s |
| Trail after losing half the bar | 42 calls | 1.4 s | 0.17 s |
| Heal fill (bar_1_hp), after the 0.1 s delay | +0.038 | 1.14 bar/s | 9.1 bar/s |
| Heal from empty to full | 27 calls | 0.9 s | 0.11 s |

With k steps per frame, multiply the rates by k and divide the times by k. The 8x gap between 30 and 240 fps stays.

Fix idea: the callbacks get no dt, so read the frame step (kFrameDt, as `des_cam_rate` does). Wrap the three callbacks. If +0x3C moved by exactly one step, set it to (old value ± step × dt × 30), stopping at +0x20. This keeps the 30 fps speed in bars per second for any k, as long as k does not change with the frame rate.

### 2.3 Bar length: DsMenuDlgObjGauge (update 0x00789BC0)

Vtable 0x018688A8. r30 = 0x018FA98C (TOC 0x019ADE10).

Fields (read from code):
- +0x2C, float: target length in px. Message 0x7D5 sets it to the value × +0x34.
- +0x30, float: the length shown.
- +0x34, float: the full length, taken in slot 27 (0x0078A080).

Each call, with no dt, with d = |+0x2C − +0x30|:
- d over 2.0 (0x018F29F4): move by d × 0.5 (0x018F2A04), half the gap.
- d from 1.0 to 2.0: move by 1.0 (0x018F29FC).
- d of 1.0 or less: snap.

Stores: 0x00789C68 (down, fsubs at 0x00789C64), 0x00789CEC (up, fadds at 0x00789CE8), 0x00789CC4 (snap).

PCGauge sends 0x7D5 = max / cap to every bar each frame, so a bar's length follows the max. Guess: it eases when the max changes, for example soul form to body form and back, a level up, or a Cling Ring. Those triggers were not read.

A 200 px change settles in 9 calls: 0.3 s at 30 fps, 38 ms at 240. A quick ease becomes a near snap.

ObjSmoothGauge's update calls 0x00789BC0 directly (`bl` at 0x0078E340). A function wrapper only sees calls through the vtable, so a wrapper on 0x00789BC0 misses the smooth bars. A wrapper on 0x0078E2E8 as well catches both, and so does an `--insn-hooks` hook at the stores.

Fix idea: move by gap × (1 − 0.5^(dt × 30)) while over 2 px, and by dt × 30 px below that.

### 2.4 Caption: DsMenuDlgCaption (update 0x008E4968)

Vtable 0x01870528. What it shows was not read.

+0xF8 (int) is set to 2 on every call while a caption is wanted (0x008E4AC0). When the request goes away, it counts down by 1 per call (0x008E4AF8, 0x008E4AFC). At 0, the state (+0x100) goes to 3, and the fade-out layout animation plays (0xF8A8B8, speed 1.0 at 0x018FA1E0, dt-based).

So the wait is 2 frames: 67 ms at 30 fps, 8 ms at 240. Too short to see. No fix needed.

## 3. Checked and dt-based

| Class | Functions | What runs each call | Verdict |
|---|---|---|---|
| DsMenuDlgPCGauge | update 0x007B2CD0; helpers 0x007AE3D8, 0x007B0808, 0x007B0C50, 0x007AF080, 0x007AF650 | Works out ratios, sends the bar messages, sets texts. +0x2AC/+0x2B0 keep last frame's values. The helpers sort, set texts and drop references | no step of its own (its bars: 2.2, 2.3) |
| DsMenuDlgBossGauge | update 0x007A2CD8; 0x007A1E40; 0x007A2218 | +0x110 += dt against 2.3 s (0x019DA8B4); +0x104 += dt against 1.0 s; ratios and messages | dt |
| DsMenuDlgFEEnemyGauge | update 0x007A6B98; 0x007A61D8 | Per-enemy timers +0x14/+0x18 += dt against 1.5 s (0x019DA8B8). Places the bar over the enemy. Layout animations | dt (its bar: 2.2) |
| DsMenuDlgFloatingPCGauge | update 0x008AEFB0; 0x008AEA98 | Places the bar (a range check from 0.1 to 60, then screen bounds) and sends 0x7D0 targets | no step |
| DsMenuDlgFECondition | update 0x007A4D60; 0x007A48A8 | No read-modify-write of its fields | no step |
| DsMenuDlgTargettingSite | update 0x007B6178; 0x007B5B68; 0x007B5EF0 | 0x007B5B68 centres the mark on the target. 0x007B5EF0 is a state machine (+0xF8, states 1 to 4) that plays layout animations | dt |
| DsMenuDlgGetItem | update 0x007B7BB8; 0x007B7968, 0x007B75E8, 0x007B7068 (reads input) | No field moved by a fixed amount | no step |
| DsMenuDlgObjGetItemData | no update of its own (base 0x00F63698); handler 0x007BB088 | Sets texts | no step |
| DsMenuDlgQuest | update 0x007BCB40: base 0x00781BB8 (passes dt on), 0x007BC2F0 (texts, list), slot 35 0x007814F0 (layout animation) | Only reference drops and a state store | no step |
| DsMenuDlgFeShortcut | update 0x007ADB80; 0x007AB920, 0x007AD238, 0x007AA998 | Layout animations at speeds 1.1, 1.0 and −1.0 | dt |
| DsMenuDlgSaveIcon | update 0x007B49C0 | Layout animation while flag 0x8B is set; visibility | dt |
| DsMenuDlgTextEffect: YOU DIED and the other big texts | update 0x008E2198 | +0xFC += dt (0x008E2694, 0x008E2698), compared with a time from 0x007222A0 (flag 0x35). At the end: +0x104 = 1.0 or 2.0 (guess: fade-out speed), flag 0x5E = 1. The text is a layout animation. Parts F23_DemonDestroy, F23_YouDied, F23_YouRevived, F23_YouRegained, F23_TargetDestroyed, F23_YouDide_phantom, F23_BlackPhantomDestroy are picked by flag 0x35 = 1..7 (table at 0x018704C8) | dt |
| FEGetSoul's hold, ObjSmoothGauge's delay | 0x007A9020, 0x0078E354 | dt timers (2.1, 2.2) | dt |
| DsMenuDlgObjGauge draw callbacks | 0x00789F50, 0x00789FE0, 0x00789DF8 | Scale the rect by the fill | no step |
| Base classes | DsMenuDialog 0x0071A6B8, EzMenuDialog 0x00F58000 (uses 60 × dt), object base 0x00F63698, layout animation player 0x00F6F8F0, play-at-speed 0xF8A8B8 | | dt |
| DsMenuDlgObjScrollMsg (key guide and help line text that scrolls when too long) | update 0x00798CD8, vtable 0x018690F0 | +0x20 −= 1.5 (0x018F303C) per call (0x00798D68, 0x00798D6C). An earlier `DES_FRAMELOCK=1` run at 240 fps flagged this field | per call; `scroll_msg_rate` in `port/des_options.cpp` handles it (docs/35) |

## 4. Run requests

One run covers every candidate. None of this logging exists yet: below is what to add and what to look for.

### Logging to add

Add scratch logs behind environment variables, like `DES_SCROLLLOG`:

| Log | Wrap | Print | When |
|---|---|---|---|
| L1 soul counter | 0x007A8C60 | flip, dt (f1), +0xF8, +0xFC, +0x100, +0x104, +0x108, +0x110 | each call while +0xFC ≠ +0x100 or +0x108 > 0 |
| L2 bar fill | 0x0078EDC8, 0x0078EB60, 0x0078F020 | flip, slot, object, +0x20, +0x3C before and after, +0x44, +0x4C, +0x50, +0x51; and the number of calls per object per flip | each call where +0x3C changed or +0x50 is set |
| L3 smooth bar update | 0x0078E2E8 | flip, object, dt, +0x54, +0x5C, and +0x2C/+0x30 before and after | each call while +0x50 is set or +0x2C ≠ +0x30 |
| L4 plain bar length | 0x00789BC0 | flip, object, +0x2C, +0x30 before and after | each call where +0x2C ≠ +0x30 |
| L5 which bar is which | 0x007B2CD0 (PCGauge) | this, and the objects at this+0xF8 (bar_0_hp), +0xFC (bar_1_hp), +0x110 (bar_0_mp), +0x114 (bar_1_mp), +0x124 (bar_0_sp), +0x128 (bar_1_sp) | once |
| L6 death screen (control) | 0x008E2198 | flip, dt, +0xF8, +0xFC | each call while +0xF8 ≠ 0 |

All six functions are reached through vtables, so `ppu_register_function` wrappers see them. The exception is the direct call in 2.3, which is why L3 also logs the length.

`DES_FRAMELOCK=1` (the scratch scanner in the working tree) can stay on. It should flag FEGetSoul +0x0FC: an int step of +2 or +12, repeated 30 times. It cannot see the bar steps, because they happen in draw callbacks, outside the updates it wraps. The ObjGauge ease is not a constant step either.

### Steps

Start from a save in the Tutorial or the Nexus with `DES_FPS=240 DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1`, and skip the opening cutscene.

1. Wait for the HUD. L5 prints the bars.
2. **Stamina:** roll 3 times in a row (circle), then stand still for 3 s. Look at bar_0_sp in L2 and L3.
3. **Damage:** take one hit. There are enemies in the Tutorial; from the Nexus, the first soldiers in 1-1 are a few steps from the archstone. Then stand still for 4 s. bar_1_hp should drop at once. bar_0_hp should wait 1 s, then drain.
4. **Heal:** use a healing grass from the item slot (square), then wait 3 s. bar_1_hp fills after 0.1 s; bar_0_hp follows after 1 s.
5. **MP:** if the character has a spell, cast it once. bar_0_mp behaves like bar_0_hp in step 3.
6. **Souls:** kill an enemy, or use a soul item. Wait until the counter stops and the "+N" goes away. Look at L1.
7. **Max change:** level up Vitality or Endurance at the Maiden in Black (Nexus). Or put on or take off a Cling Ring in soul form, or use a Stone of Ephemeral Eyes to go back to body form. Look at L3 and L4 on the bars, plates and outlines. A level up spends souls, so L1 may show the counter going down (guess).
8. **Switch to 30 fps:** run `setenv DES_FPS=30` on the debug console (`DES_FPS` is read at each flip). Repeat steps 2 to 4, and step 6 if there is a second soul item.
9. **Last, die** (a fall works). L6 should take the same number of seconds at either frame rate. If the character was in body form, the HP bar gets shorter in soul form, so L3 and L4 show the ease again.

### What to expect

k is the number of bar callback calls per frame (from L2's count; 1 or 2).

| What | Log | 30 fps | 240 fps, if frame-locked |
|---|---|---|---|
| Soul roll, +1,000 | L1 +0xFC | 130 calls, 4.3 s | 130 calls, 0.54 s |
| Soul hold | L1 +0x108 reaching +0x110 | same seconds at both (control) | |
| HP trail | L2 bar_0_hp +0x3C | −0.012 per call, 0.36·k bar/s | −0.012 per call, 2.88·k bar/s |
| HP trail delay | L3 bar_0_hp +0x54 | 1.0 s | 1.0 s (control) |
| Heal fill | L2 bar_1_hp +0x3C | +0.038 per call, 1.14·k bar/s | +0.038 per call, 9.1·k bar/s |
| Stamina and MP trails | L2 bar_0_sp, bar_0_mp | like the HP trail | like the HP trail |
| Callback calls per frame | L2 count | k | the same k |
| Bar length, 200 px change | L3/L4 +0x30 | 9 calls, 0.3 s | 9 calls, 38 ms |
| YOU DIED | L6 +0xFC | same seconds at both (control) | |

A candidate is confirmed when its per-call numbers match at both rates while the controls match in seconds.

## 5. Notes

**Class labels.** Some addresses reached this lane under the wrong class name. Each class below is read from the constants block that points at the update's vtable:

| Address | Class |
|---|---|
| 0x007ADB80 | DsMenuDlgFeShortcut (not FEGetSoul) |
| 0x007B7BB8 | DsMenuDlgGetItem (not TargettingSite) |
| 0x007BCB40 | DsMenuDlgQuest. DsMenuDlgObjGetItemData (vtable 0x01869DD8) has no update of its own |
| 0x007A6B98 | DsMenuDlgFEEnemyGauge |

**docs/35.** Its first version called 0x008D0AB0 the DsMenuDlgNowLoading update. That is DsMenuDlgRanking's update (vtable 0x0186FCD8). NowLoading's update is 0x008CD078 (vtable 0x0186FBC0), the one the loading-souls fix hooks. The current docs/35 has it right.

**Reading the tables.** A vtable address points at slot 0, and two zero words come before it. A class's name is the first DsMenu* string after the constants-block word that points at its vtable.
