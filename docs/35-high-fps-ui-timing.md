# UI animation speed above 30 fps (2026-10-18)

With `DES_FPS` above 30, two things ran fast:
- the loading souls: the two soul icons by the small "Demon's Souls" logo, bottom left, at boot and on level loads. Fixed; see below.
- some HUD and menu animations. Still open; see the last section.

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

## HUD and menu animations (open)

- The menu layout timers are dt-based. The frame update loads kFrameDt into f1 and passes it down 0x00138A98, 0x001372F8, 0x00135900, 0x00143920, 0x00141050, 0x00348EF8, 0x00355EA0 and 0x0035FB10 to 0x003624B0, which adds it to the timer at +0x10.
- The animations that run fast are therefore likely per-frame integer counters, like the souls' cells. Next: run the same memory scan (integer steps of 1 a flip) while one is on screen, then a write watch on what it finds.

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
