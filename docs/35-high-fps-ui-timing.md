# UI animation speed above 30 fps (2026-10-18)

With `DES_FPS` above 30, these ran fast:
- the loading souls: the two soul icons by the small "Demon's Souls" logo, bottom left, at boot and on level loads. Fixed; see below.
- menu text that is too long for its box and scrolls sideways, such as the key guide along the bottom of the inventory. Fixed; see "Scrolling menu text".
- on the HUD, the soul counter rolling up after a soul gain, the bars' fill (the trail after damage or stamina use, and a heal filling the HP bar) and the bars' length when the max changes. Fixed; see "HUD effects".
- the character creation screen's previews, which the right stick turns. Fixed; see "Character creation previews".

The other HUD and menu animations that were checked already follow the frame's time.

## The loading souls

Each soul is a flipbook of 100 cells from a 16 x 8 sheet (a cell is 256 x 512 of the 4096 x 4096 texture), and the two souls are 50 cells apart. The game moves each soul one cell per update and runs the update once a frame, so at 240 fps the souls turned 8 times as fast. Two screens draw them, with separate code.

### Boot screen

- The frame update 0x0001B8D0 reaches 0x0015AE10 once a frame: through 0x00138A98 and 0x001372F8, then by indirect calls to 0x00135900, 0x001A5EB8 and 0x0015AE10.
- 0x0015AE10 moves the screen's fade (+0x1C, a float) one step a call. With consts = `*(r2 - 0x7C90)`, it goes up by the step at consts - 0x7EE4 to the maximum at consts - 0x7F64, or down to 0 while the flag byte at `*(consts - 0x7F7C)` is set.
- It then calls 0x0015AA00, which adds 1 to each soul's cell (+0x20 and +0x24, back to 0 at 100) and draws the logo and the souls (fragment program B40481).
- Only the boot screen uses this object (0x40114880 in our runs). Level loads do not call 0x0015AE10.

### Level loads

- The NowLoading dialog (DsMenuDlgNowLoading) draws the souls as layout parts. Parts soul_0 to soul_99 hold the 100 cells and are drawn off-screen every frame. Parts icon_soul1 and icon_soul2 are the two icons you see.
- 0x008CCB08 finds the icons, formats `soul_%d` with the cells at dialog +0xF8 and +0xFC, and copies those parts' images into the icons.
- The dialog's update, 0x008CD078, takes dt in f1; its vtable slot is at 0x0186FC04. While the dialog is up, it adds 1 to both cells once a call. While the dialog is hidden, it sets them to 0 and 50. Its fade (+0x100) already goes by dt.
- 0x008CC248 is an out-of-line copy of the same step. Nothing calls it: no code calls it directly, and no table holds its function descriptor (0x01958F00).
- A level load that starts a cutscene already runs at 30 fps (`fps_on_flip_request`), so the souls looked right there. Loads without a cutscene ran unlocked.

### The fix

`port/des_options.cpp` hooks both updates: `load_souls_rate` on 0x0015AE10 and `nowloading_souls_rate` on 0x008CD078. With `DES_FPS` set, each hook:
- counts the frame in 30 Hz steps. The boot hook uses kFrameDt; the dialog hook uses its own dt argument, f1. Both are capped at 50 ms, like the game's step.
- takes whole steps, rounded (the carry starts at 0.5), and keeps the rest for the next frame.
- before the game's code runs, sets each cell one short of where that many steps lead, so the game's own +1 lands there. At 30 fps that is one step a frame, as on the PS3.

Two details differ between the hooks:
- The boot hook also scales the fade's step by the frame's length.
- The dialog hook puts the cells back if the update did not step them, which happens when the dialog is not active.

`DES_UIFIX=0` turns both hooks off live. Without `DES_FPS`, nothing changes.

Measured in cells per second. Time comes from the frame log; the cells come from the draw log (boot) or a hook log (level loads). The level loads ran with the cutscene lock off (`DES_DIAG_NOCUTLOCK=1`, scratch).

| | boot screen, steady part | level load |
|---|---|---|
| 30 fps | 30.0, one a frame | 30.0, one a frame |
| 60 fps | 29.6 | 30.3 |
| 240 fps | 29.6 | 29.8 |
| 240 fps, `DES_UIFIX=0` | 226 | 208 |

Over the whole boot screen, fewer cells are drawn than wall time predicts. During boot stalls (frames of about 100 ms) the screen is not updated at all, as on the PS3.

## How it was found

- **Frame dumps:** dump frames at boot with `CELLMARK_DUMP_DIR` and track the centroid of the bright pixels above the logo (x 60..300, y 470..570 in 1280x720). Use `CELLMARK_DUMP_EVERY=8` at 240 fps: with every frame dumped, the synchronous BMP write slows the game enough to hide the bug. This gave about 39 px/s at 30 fps and about 140 px/s at 240.
- **Memory scan:** a scratch hook took four snapshots of memory one flip apart and listed the words with a constant step. Two heap words stepped by +1 a flip, 50 apart: the boot souls' cells.
- **Write watch:** a hardware watch on those words gave the writer 0x0015AA00, called from 0x0015AE10. `tools/des/sym.py` turned the host stack into guest functions.
- **Level loads:** a vertex log of every draw on the loading screen showed the icons as s16 quads at (94, 530) and (214, 541) px with one-cell UV rects. The UTF-16 strings `icon_soul1` and `soul_%d` led to 0x008CCB08 and the cell fields.
- **Callers:** the vtable slot was found by searching the EBOOT for 0x008CD078's function descriptor and for words that point at it.

## What it was not

These were ruled out in the first attempt; they still hold:
- **The game's frame step.** kFrameDt (0x018C8DB4) is the real flip-to-flip time at boot. Scaling it by 1/8 left the souls as they were, since they do not use it. Layout objects (base update 0x00F63698) get f1 equal to this step, and the layout animation player (0x00F6F8F0) is dt-based.
- **Float constants rewritten in guest memory at the first flip:**
  - every 1/30 (34 sites), 1/60 (6), 30.0 (50) and 60.0;
  - the engine loop's 1/30 at 0x018C916C, loaded at 0x0002BAA4 and posted as message 0x8058 to the movie player task (vtable 0x01871638; its state machine at 0x0094AB90 keeps its clock at +0x2C);
  - the 2 degree step (0.0349066) at 0x018F96C0 in 0x008CF6CC, near the NowLoading code.

Two conclusions of the first attempt were wrong:
- **"The screen is not drawn by the main game loop."** An indirect-call histogram (`PS3_ICALL_GATE`) over the boot screen did not show 0x0015AE10, yet the main loop reaches it once a frame. Do not rule a function out from that histogram alone.
- **"No wisp state."** The scan looked for floats. The state is two integer cell indices.

## Scrolling menu text

Menu text that does not fit its box scrolls sideways. The inventory's key guide (the button list along the bottom: Select, Command Menu, Close, Change Item List Display, Change Status Display) is 58 px too long for its 1088 px box, so it scrolls whenever the inventory is open.

- The class is DsMenuDlgObjScrollMsg (vtable 0x018690F0). Its update, 0x00798CD8 (slot 17, dt in f1), moves the text's offset (+0x20, a float) left by 1.5 px each call. It does so while the message (+0x28) is the one it had at the last call (+0x2C). When the message changes, or is -1, it sets the offset to 0. Constants: consts = `*(r2 - 0x7208)`; the step is at consts - 0x7FA0.
- Slot 5, 0x00798AC8, places the text when it is drawn. It gets the draw rect in r4. If the text fits, it sets the offset to 0. If not, it adds clamp(offset + 90, -overflow, 0) to the rect's x. Once the offset is past -overflow - 180, it sets the offset back to the start, 0. The 90 px margin is at consts - 0x7F94 and the start at consts - 0x7F8C. Every test is a range test, so a part step is fine.
- At 30 fps that gives a 2 s hold, the scroll at 45 px/s, another 2 s hold, then a restart: a 5.3 s cycle for the key guide. The update runs once a frame, so at 240 fps all of it ran 8 times as fast.
- Nothing else steps the offset. The detector below also reported it from the key guide dialog (DsMenuDlgKeyGuide, +0x130) and from two parent layout objects. Those reports were the same field: the ObjScrollMsg object lies within the first 0x400 bytes of each of them.

### The fix

`scroll_msg_rate` in `port/des_options.cpp` hooks 0x00798CD8. After the game's code, it checks that the update made its step: the message is the same as at the last call, and the offset is exactly the old value minus 1.5. If so, it sets the offset to the old value minus 1.5 x dt x 30. dt is f1, capped at 50 ms like the game's step. At 30 fps it leaves the step alone. `DES_UIFIX=0` turns it off live, along with the souls hooks. Without `DES_FPS`, nothing changes.

Measured on screen with a scratch log of the key guide's x at each draw (`DES_PLACELOG`), with the inventory open for 14 s:

| | speed while moving | cycle |
|---|---|---|
| game's own mode (no `DES_FPS`) | 44.9 px/s | 5.31 s |
| 30 fps | 45.0 px/s | 5.31 s |
| 60 fps | 45.5 px/s | 5.30 s |
| 240 fps | 44.9 px/s | 5.60 s |
| 240 fps, `DES_UIFIX=0` | 358.6 px/s | 0.67 s |

The 240 fps cycle is longer because it had five long frames (98 to 185 ms) as the inventory opened. A frame counts at most 50 ms, here as everywhere else in the game.

### Checked and already right

- These go by the frame's time at any rate: the menu cursor glow, the tab arrows, the menu slides, the HUD item slot highlight, and the title's PRESS START pulse (a 1.0 s period at both 30 and 240 fps).
- The menu layout timers are dt-based. The frame update loads kFrameDt into f1 and passes it down 0x00138A98, 0x001372F8, 0x00135900, 0x00143920, 0x00141050, 0x00348EF8, 0x00355EA0 and 0x0035FB10 to 0x003624B0, which adds it to the timer at +0x10.

### How it was found

- **Draw logs:** a vertex log of every draw at 30 and at 240 fps, with the screen elements matched across frames and their rates of change compared over real time. The cursor glow, tab arrows, slides, slot highlight and title pulse came out the same at both rates.
- **Update detector:** a scratch hook (`DES_FRAMELOCK`) on the update (vtable slot 17) of every dialog and dialog object class, 159 functions. It snapshots the first 0x400 bytes of the object before and after each call. It reports any field that changed by the same amount on every call while dt varied: ints by a small step 30 calls running, floats 60 calls running. Run at 240 fps over the HUD and the menus, it reported only ObjScrollMsg's +0x20, once from the object itself and three more times from objects around it.
- **Finding the vtables:** in the EBOOT, a vtable is two zero words (offset-to-top and RTTI) followed by the slots. Slots 10 to 13 match within each family: 0x00F6DA58, 0x00F5E7C8, 0x00F53D18 and 0x00F53DF8 for dialogs; 0x00F638E8, 0x00F638B8, 0x00F633F0 and 0x00F633C0 for dialog objects. The base updates are 0x0071A6B8 (dialogs) and 0x00F63698 (dialog objects).
- **Class names:** each vtable's address is stored in the class's constants block, next to pointers to its ASCII and UTF-16 names.

## HUD effects

The detector above only sees animations that run for 30 to 60 calls in a dialog update. docs/38 read the HUD's classes for fields that move a fixed amount per call instead. Three do; test runs confirmed them and they are fixed here. The rest of what it read goes by the frame's time: the boss and enemy bar timers, the lock-on mark, the item pickup pop-up, the save icon, captions and the death screen's texts (docs/38, section 3).

### Soul counter

- DsMenuDlgFEGetSoul's update, 0x007A8C60 (vtable 0x018696C0, dt in f1). Fields: +0xF8 souls before the gain, +0xFC how much of the gain is counted, +0x100 the gain, +0x104 the "+N", +0x108 the hold timer, +0x110 the hold time (1.5 s).
- Souls come in through two menu values, read by 0x00720F20 and cleared by 0x00720F50. 0x76 is added to the gain and 0x77 to the "+N". The update clears 0x76 whenever it reads it, even when 0x77 is 0, so a test that sets them must write both at once.
- Each call moves +0xFC one step towards +0x100. The step depends on what is left: 1 up to 10, 2 up to 100, 12 up to 1,000, 112 up to 10,000, and so on up to 11,111,112 (immediates; table in docs/38, 2.1). The total shown is +0xF8 + +0xFC. At the gain it holds by dt, then clears.
- A gain of 1,000 to 1,500 takes 130 calls: 4.3 s at 30 fps, 0.54 s at 240.

### Bar fill

- DsMenuDlgObjSmoothGauge (vtable 0x01868C28) draws the HP, MP and stamina bars, and the boss's and enemies' HP bars. It moves the fill shown (+0x3C) towards the target (+0x20) by +0x44 going down (0.012: the trail after damage or stamina use) or +0x4C going up (0.048 on the HP trail bar, 0.038 on the HP bar; 10 means at once). It snaps once it is within a step.
- The steps happen in its draw callbacks, 0x0078EDC8, 0x0078EB60 and 0x0078F020 (slots 5 to 7), which get no dt. Each HUD bar takes two calls a frame, slot 5 then slot 6, so a trail drains 0.72 of the bar a second at 30 fps. The delay before a trail starts (+0x54 against +0x5C) goes by dt.

### Bar length

- DsMenuDlgObjGauge's update, 0x00789BC0 (vtable 0x018688A8, dt in f1), moves the length shown (+0x30) towards the target length (+0x2C). It moves half the gap a call while the gap is over 2 px, then 1 px, then snaps. ObjSmoothGauge's update, 0x0078E2E8, calls it directly (0x0078E340).
- PCGauge's update, 0x007B2CD0, sets each bar's target length to its full length times min(1, max / cap). The caps are floats at PCGauge +0x294 (HP), +0x298 (MP) and +0x29C (stamina). The HP cap is 1900 in the test save.
- Halving the HP bar takes 8 calls: 0.25 s at 30 fps, 33 ms at 240.

### The fix

`port/des_options.cpp` hooks the updates and callbacks. With `DES_FPS` set, each counts the frame in 30 Hz steps (the game's step, at most 50 ms, times 30):
- `get_soul_rate` on 0x007A8C60 takes whole steps, with a carry, like the souls hooks. If the game took its one step, the hook takes that many steps from where the counter was. The number on screen was set from the game's step, so while it rolls it can be a step off the count. It ends on the total.
- `gauge_fill_rate` on the three callbacks reads the frame step (kFrameDt). If the callback moved the fill by exactly one step, the hook moves it by the step times the frame's 30 Hz steps instead, not past the target.
- `gauge_len_rate` on 0x00789BC0 and 0x0078E2E8 uses f1. It makes the half-gap step 0.5 to the power of the frame's 30 Hz steps, and the 1 px step that many px, not past the target.

At 30 fps the hooks leave the game's step alone. `DES_UIFIX=0` turns them off live, with the other hooks here.

### Measured

Three runs on the pinned test save, which is in the tutorial. A scratch driver used the debug console and the pad file:
- three backsteps in a row for the stamina trail;
- a 10 m warp up for damage (you cannot die in the tutorial);
- the item slot for a heal;
- +1,500 souls, with both menu values set in one 8-byte write;
- the HP cap doubled and put back. Max HP itself does not work for this, since the game writes it back every frame.

A scratch field log gave each change's time and flip. The PC was busy with other work during the runs, so with the fix off, the "240 fps" runs went at about 50 to 70 fps. What shows the frame lock is that the number of calls stays the same.

| | 30 fps | 240 fps, `DES_UIFIX=0` | 240 fps | unlocked |
|---|---|---|---|---|
| Soul roll, +1,500 | 130 calls, 4.38 s | 130 calls, 2.43 s | 4.31 s | 4.29 s |
| Stamina trail, 0.42 of the bar | 0.57 s | 36 calls, 71 ms | 0.55 s | 0.58 s |
| HP trail | 0.73 of the bar a second (all of it in 1.37 s) | | 0.083 of the bar in 99 ms, as at 30 fps* | 0.083 in 99 ms |
| Heal fill, 0.18 of the bar (trail bar, HP bar) | 33 ms, 67 ms | | 46 ms, 62 ms | 47 ms, 63 ms |
| HP bar length, halved and back | 8 calls, 234 ms and 218 ms | 8 calls, 116 ms and 136 ms | 229 ms and 218 ms | 234 ms and 234 ms |

\* At 30 fps, 0.083 of the bar is six steps and the snap: three frames, 100 ms.

Not run: the death screen, since the test save cannot die. docs/38 read its timer as dt-based. A bar's length changing in play (a level up, or going between body and soul form) uses the same code as the cap test.

## Character creation previews

The character creation screen shows the character in a preview that the right stick turns: the whole body from Create Profile on, and the face in Edit Appearance.

- The body's step is 0x008DB170 and the face's is 0x008DA0E0. Each gets r3 = the preview and f1, f2 = the stick's x and y, which PadMan's getters 0x001B3F58 and 0x001B3F90 scale by pi/120. It adds 1.5 times the stick to the yaw (+0x154, wrapped to pi) and the pitch (+0x158, clamped to -20 to 70 degrees for the body and -30 to 70 for the face).
- It runs once a frame and gets no dt: 2.25 degrees a frame at full tilt, 67.5 degrees a second at 30 fps and 540 at 240.

### The fix

`des_viewer_rate` in `port/des_options.cpp` runs after the first instruction of both steps (one `--insn-hooks` line each) and scales f1 and f2 by the frame's 30 Hz steps (kFrameDt times 30). At 30 fps it leaves them alone. `DES_UIFIX=0` turns it off live, with the other hooks here.

### Measured

One run per column on the pinned test save, from the title screen. A scratch driver held the right stick through the debug console (`PAD_STICK`), and a scratch log printed the preview's angles on each call.

| Right stick | 30 fps | 240 fps, `DES_UIFIX=0` | 240 fps |
|---|---|---|---|
| Body, right for 0.5 s | +36.0 degrees, 16 calls | +274.5 degrees, 122 calls | +34.9 degrees, 124 calls |
| Body, left for 1.0 s | -69.9 | -540.0 | -68.4 |
| Body, down for 0.3 s | +22.5 | +90 (to the clamp) | +20.8 |
| Face, right for 0.5 s | +36.0 | +276.7 | +34.3 |

The small differences come from 30 fps rounding the stick's time to whole frames.

### Screens checked

The update detector and the draw logs ran at 30 and at 240 fps over:
- the title flow: the notice, PRESS START, the title menu, the two network dialogs and the profile list;
- character creation: Name with the on-screen keyboard, Gender and its confirm dialog, Class, Edit Appearance (Origin, Random, the Age and Gender sliders, Hairstyle, Hair Color, Details with General, Eyes and the RGB hair colour) and the discard dialog;
- the Start menu: Inventory with its command menu and both display toggles, Equipment with an item list, Status and the Stat Guide, World Tendency, System with Options (Camera Speed), Load Profile and the Quit Game dialog, and tab changes with L2 and R2.

Only the previews were frame-locked. The cursor glows, text pulses, fades and pop-ups take the same time at both rates. The sliders' hold-to-repeat goes by the frame's time too, though not exactly: it measured about 27 steps a second at 240 fps and about 20 at 30 fps with real frame times (`DES_FPS=30`).

## Scratch tools used

These are kept outside the repo and rebuilt from this description:
- `DES_WISPSCAN=<flip>`: four memory snapshots one flip apart; prints the words with a constant integer or float step.
- `WATCH_ADDR=<a>,<b> WATCH_RW=3 WATCH_N=<n> WATCH_AT=1`: hardware watches on guest addresses, with host stacks, in the D3D12 backend.
- `VA_FP=<fp>|FFFFFFFF` with `VA_FROM`, `VA_TO` and `VA_N`: log each draw's vertex attributes.
- `DES_DIAG_NOCUTLOCK=1`: leave the frame rate unlocked in cutscenes, to test a level load without the 30 fps lock.
- `DES_SOULLOG=1`: log each souls hook call (dt, steps, cells before and after).
- `DES_FPATCH=addr=value,...`: rewrite floats at the first flip.
- `DES_DTSCALE`: scale the step.
- `DES_UIDTMSG`: log the movie task's message senders.
- `DES_VIEWER_LOG=1`: log each call of the preview steps (yaw, pitch, stick, 30 Hz steps).
- `DES_FRAMELOCK=1`: the update detector above.
- `DES_SCROLLLOG=1`: log each scroll update call (dt, message, offset before and after).
- `DES_PLACELOG=1`: hook slot 5 (0x00798AC8) and log the offset and the rect's x before and after each call.
- `DES_FLWATCH=fn:offset[r|p],...` with `DES_FLWATCH_OUT=<file>`: on top of `DES_FRAMELOCK=1`, log each change a hooked update makes to the listed fields (r: a float; p: print the value once), with the time and flip.
- `DES_GAUGELOG=1`: hook the three ObjSmoothGauge draw callbacks and log each call that moves the fill (target, fill before and after, steps, flags).
- `VA_FROM` read live, so the console's `setenv` can start and stop the vertex log; the log buffered, since writing each field held 240 fps down to 75.
