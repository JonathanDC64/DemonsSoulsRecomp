# UI animation speed above 30 fps (open, 2026-10-18)

With `DES_FPS` above 30, two things run fast:
- the loading souls: the two wisps by the small "Demon's Souls" logo, bottom left, at boot and on loading screens;
- some HUD and menu animations.

This doc records what was measured and what was ruled out, so the next attempt starts from here.

## How to measure

Dump frames at boot with `CELLMARK_DUMP_DIR`. Then track the centroid of the bright pixels above the logo (x 60..300, y 470..570 in 1280x720) across the dumps.

Use `CELLMARK_DUMP_EVERY=8` at 240 fps. With every frame dumped, the synchronous BMP write slows the game enough to hide the bug. The dumper also skips a frame identical to the one before it.

Measured with every 8th frame dumped at 240 fps and every frame at 30 fps:

| fps | wisp speed |
|---|---|
| 30 | about 39 px/s |
| 240 | about 140 px/s, 3.5 to 4 times faster |

## What it is not

None of these changes the wisp speed at 240 fps:
- **The game's frame step.** kFrameDt (0x018C8DB4) is the real flip-to-flip time at boot. Scaling it by 1/8 leaves the wisps as they were. Layout objects (base update 0x00F63698) get f1 equal to this step, and the layout animation player (0x00F6F8F0) is dt-based.
- **Float constants rewritten in guest memory at the first flip:**
  - every 1/30 (34 sites), 1/60 (6), 30.0 (50) and 60.0;
  - the engine loop's 1/30 at 0x018C916C, loaded at 0x0002BAA4 and posted as message 0x8058 to the movie player task (vtable 0x01871638; its state machine at 0x0094AB90 keeps its clock at +0x2C);
  - the 2 degree step (0.0349066) at 0x018F96C0 in 0x008CF6CC, near the NowLoading code.
- **DsMenuDlgNowLoading.** The dialog with parts `icon_soul1`, `icon_soul2` and `soul_%d` has its code at 0x008CC000..0x008D1000. Its per-frame update (vtable slot 15, 0x008D0AB0) is never called during the boot screen. It may be the dialog for level loads only.

## What the boot screen does

- **Main thread:** an indirect-call histogram over 1.5 s of the boot screen (`PS3_ICALL_GATE`) shows almost no per-frame calls on the main thread. The screen is not drawn by the main game loop.
- **Heap scans:** three snapshots of the heap (0x40000000..0x4A500000) and main memory, one flip apart, found these with an exact per-flip step:
  - the movie task clock (1/30 per flip);
  - frame-allocator pointers (they read as floats from 2 to 4 and step by 0x40000);
  - frame counters.
- **No wisp state:** no float or double in PPU memory steps by a constant amount per flip. The wisp state is therefore not a PPU float advanced by a constant step.

## Leads

1. **SPU effect job.** The effect system (DsFx, the `ffxbnd` effects) may step particles on an SPU in 30 fps frame units. Next: find the SPURS job that runs during the boot screen and what step it reads.
2. **Integer frame index.** A texture-pattern or keyframe index in an effect could advance by one per update. The integer scan only looked for steps of 1 to 4 in a 0..1e6 range.
3. **A fallback that needs no reversing:** run loading screens at 30 fps, as cutscenes already do (`fps_on_flip_request`). It needs a reliable "loading screen up" signal from guest state. Engine object +0x6AC (`*0x01AD6298`) is not one: it is set once at boot and never cleared.

## Scratch tools used

These are kept outside the repo and rebuilt from this description:
- `DES_WISPSCAN=<flip>`: snapshot and compare memory.
- `DES_FPATCH=addr=value,...`: rewrite floats at the first flip.
- `DES_DTSCALE`: scale the step.
- `DES_UIDTMSG`: log the movie task's message senders.
