# Demon's Souls static recompilation for PC

A native Windows port of *Demon's Souls* (PS3, 2009, BLUS30443 / USA), built by **static recompilation**: the game's PowerPC and SPU machine code is translated to C ahead of time, compiled with clang, and linked against a reimplementation of the PS3 system libraries. Rendering goes through Direct3D 12.

It is built on [ps3recomp](https://github.com/sp00nznet/ps3recomp); our changes are in the fork [JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp).

**This repository contains no game code or data.** You need your own copy of the game. See [Legal](#legal).

## Status

The first level is playable, from boot to Boletaria (2026-10):

- Boot, the system-data auto-save notice, the logos and the intro movies (with sound and subtitles).
- Title screen, character creation, the tutorial, the Nexus and Boletarian Palace 1-1.
- Lit and textured scenes with shadows, the HUD, particles, light shafts and haze.
- Saving and loading, ATRAC audio and music, keyboard and DualSense (USB or Bluetooth, over HID).
- Frame rate: the game's own 30 fps cap by default, and any cap from 30 to 500 fps, or none, with `DES_FPS` (see [Running](#running)). Boletarian Palace runs at about 120 fps uncapped (measured with `DES_FPS=60 VBLANK_MHZ=240000`; physics above 60 fps is less tested). Shaders are precompiled, so the game never stops to compile one.

Known issues:

- Some transparent effects (a fog wall, the soul-form aura, fog in the first cutscene) vanish for single frames.
- Above 30 fps, cutscenes switch back to 30 on their own: the cutscene camera moves in 30 Hz steps, and they judder otherwise. `DES_CUTFPS=1` keeps them at the frame rate and blends the camera between its steps (opt-in). Above 60, physics is untested (RPCS3 warns of trouble there).
- Only Demon's Souls is tested on this branch of ps3recomp.

The full history, root causes and tools are in [`docs/`](docs/). Start with [`docs/28-in-game.md`](docs/28-in-game.md), which covers the current state.

## Requirements

- Windows 10 or 11 with a Direct3D 12 GPU.
- Visual Studio 2022 or its Build Tools (MSVC, Windows SDK), LLVM (`clang-cl`), CMake and Ninja. Python 3 with `ps3recomp/requirements.txt` installed.
- Git Bash, for the scripts in `tools/des/`.
- Your own Demon's Souls disc or dump (BLUS30443), and [RPCS3](https://rpcs3.net) to decrypt `EBOOT.BIN`.
- For the SPURS path: your own decrypted `libsre.prx` from your PS3 firmware.

## Building

The build is not yet a single command. The steps, in order:

1. Clone this repository, and clone the fork into `ps3recomp/` inside it:
   ```
   git clone https://github.com/JonathanDC64/DemonsSoulsRecomp.git
   cd DemonsSoulsRecomp
   git clone https://github.com/JonathanDC64/ps3recomp.git
   ```
2. Decrypt `PS3_GAME/USRDIR/EBOOT.BIN` with RPCS3 (Utilities > Decrypt PS3 Binaries) to `work/game/EBOOT.ELF`. Copy `PS3_GAME/ICON0.PNG` to `work/game/` as well: the build makes the exe's icon from it (`scripts/make_icon.py`, standard library only). Without it the exe has no icon.
3. Analyse the executable: `python ps3recomp/tools/ppu_loader.py work/game/EBOOT.ELF -o work/loader` writes `EBOOT.functions.json` and `EBOOT.imports.json`.
4. Lift the PPU code and apply the boot shim. The commands are in [`docs/27`](docs/27-relift-auto-save-notice.md#re-lift-recipe):
   ```
   python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
     --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
     --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post,0x724E40=des_menu_tick" \
     --insn-hooks @port/insn_hooks.txt --shadow-stack \
     --output work/recomp -j 16
   python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
   python scripts/apply_boot_patches.py work/recomp
   ```
5. Lift the SPU images and jobs (`scripts/build_spu.py`; see [`docs/09`](docs/09-spu-spurs.md) and [`docs/26`](docs/26-first-pixels-loading-screen.md)), and libsre (`scripts/build_libsre.sh <libsre.prx>`). `build_spu.py` also puts the runtime's native blocks in place of parts of an image (`NATIVE_BLOCKS`; today the fetch step of the graphics ring consumer, image 2), so lift with the fork's `spu_lifter.py`.
6. Configure and build the runtime and the game:
   ```
   cmake -S ps3recomp -B ps3recomp/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
   cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
     -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=
   cmake --build ps3recomp/build --target ps3recomp_runtime rsx_shader_precompile
   cmake --build port/build --target des_boot --parallel 4
   ```
   Recompiling the 14 lifted chunks takes about 15 minutes. The lift keeps a guest call stack for backtraces (`--shadow-stack`), but the build compiles it out, which is about 2% faster; configure the port with `-DDES_SHADOW_STACK=ON` to keep it for `ds_dump_shadow`.

## Running

Copy `tools/des/local.env.example` to `tools/des/local.env` and set `PS3_VFS_ROOT` to the folder that holds `PS3_GAME`. Then:

```
tools/des/play.sh
```

Options go after it as `NAME=value`:

- `DES_ASPECT=21:9`: a wider picture for an ultrawide display (any `w:h`, such as `2560:1080` or `32:9`, a size such as `3440x1440`, or `auto` for the display's shape). The view gets wider at the same height (the same change as RPCS3's "Aspect Ratio" patch), the window opens in that shape, and fullscreen fills the display. The game still draws 1280 pixels across, stretched, so a higher render resolution (Settings, or `RSX_SCALE=150` and up) keeps it sharp. The HUD and menus stay 16:9 in the middle, and the lock-on reticle and enemy health bars stay on their targets; full-screen fades and the dim behind menus may still cover only the middle. Shapes narrower than 16:9 keep 16:9 with bars. Read at start.
- `DES_HUD=stretch`: with `DES_ASPECT`, spread the HUD and menus across the full width instead. `DES_HUD_FADES=1` (experimental) keeps the HUD centred but stretches the full-screen fade and the dim behind menus across the full width.
- `DES_CUTFPS=1`: with `DES_FPS` above 30, in-engine cutscenes stay at that frame rate instead of switching to 30, and the camera is blended between its 30 Hz keys (live).
- `DES_DOF=0`: no depth of field (live).
- `DES_FPS=60`: the game's 60 fps mode, with real frame times. Any other number from `31` to `500` sets the frame cap to that many frames a second, and `DES_FPS=unlocked` removes the cap (the game runs as fast as the machine allows). `DES_FPS=30` keeps 30 with real frame times.
- `DES_MOTION_BLUR=0`: no camera motion blur (live; the same change as RPCS3's "Disable Motion Blur" patch).
- `DES_VSYNC=0`: present each frame as soon as it is drawn, with tearing, instead of waiting for the display's refresh (the default, `DES_VSYNC=1`). With vsync, a frame rate above the display's refresh (say `DES_FPS=240` on a 144 Hz display) is shown at the refresh. If the system cannot tear, it keeps vsync and says so once in the log.
- `DES_SKIP_INTRO=1`: skip the boot logos (`DES_SKIP_INTRO=2` shows only the last one).
- `DES_SKIP_INTRO_MOVIE=1`: skip the opening movie (the advert after the logos). The game's other movies still play.
- `DES_AUTO_CONTINUE=1`: presses through the title screen into the first save in the list.
- `PS3_SAIL_MOVIES=0`: skip every movie.

For example `tools/des/play.sh DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 DES_FPS=60`.

A start screen (Play, Settings, Quit) shows over the boot logos. In the game, F1 (L3 + R3 on a gamepad) opens a menu with Settings and Quit; the game gets no input while it is open, and the menus use the game's own menu sounds. Settings are saved to `%APPDATA%\DemonsSoulsRecomp\DemonsSouls\settings.ini` (next to the exe if a `portable.txt` is there); an option given on the command line wins over the file. `DES_UI=0` turns all of it off, and `DES_SKIP_UI=1` skips the start screen once. The frame rate can be changed in Settings during play. The window can be moved and resized without stopping the game (the game is scaled to it, letterboxed), and Alt+Enter switches to borderless fullscreen. Details in [`docs/32`](docs/32-launcher-ui.md).

The keyboard and mouse work as in the Souls games on PC, with a gamepad or without: W A S D move (Left Alt held to walk), the mouse turns the camera (Mouse look, below), the left button attacks (R1) and Shift + left button is the strong attack (R2), the right button guards (L1) and Left Ctrl or Shift + right button parries (L2), Space rolls, backsteps and dashes (Circle), C jumps while dashing (L3; with `DES_DASH_JUMP=1`, or Settings, Controls, Jump with Space again, letting go of Space and pressing it again at once while dashing jumps too, as in the Souls games on PC), Q or the middle button locks on (R3), E is the action (Cross), R uses an item (Square), F two-hands (Triangle), the arrows are the D-pad, the wheel switches the spell (up) and the item (down) and with Shift the right and left hand, Esc is the game's menu (Start) and G or Tab is Select. A chord works alone: Shift + left button is R2 and not R1 as well. The game's own menus still show the PS3's buttons. The layout needs the menus (with `DES_UI=0` the keyboard is cellPad's plain PS3 pad: the arrows, Z X A S, Enter and Tab).

Settings, Controls, Change Buttons chooses which gamepad button presses each of the PS3 pad's buttons, and which keys, mouse buttons or wheel notches work each action: two each, any of them with Shift, Ctrl or Alt held for a chord. Restore Default Buttons puts back the game's gamepad buttons and the layout above. From the command line: `PAD_MAP=cross=circle,circle=cross` (the game's button = the gamepad's button) and `PAD_KEYS=cross=e|enter,r2=shift+mouse1,l3=none` (changes to the keyboard layout: an action = up to four inputs separated by `|`, each a letter, digit, key name, `f1` to `f24`, a virtual-key code, `mouse1` to `mouse5` or `wheelup` / `wheeldown`, with an optional `shift+`, `ctrl+` or `alt+` in front; `none` for none). Button names: cross circle square triangle l1 r1 l2 r2 l3 r3 start select up down left right; the keyboard also has ls_up ls_down ls_left ls_right (the left stick), rs_up rs_down rs_left rs_right (the right stick) and walk.

Settings, Controls, Mouse look turns the camera with the mouse as well as the right stick (off by default; `DES_MOUSE_LOOK=1` from the command line, live). In play the cursor is hidden and held in the window (on the title screen, in loading screens and in cutscenes it is free); F1, switching to another window or moving the window frees it too, and it is taken again when you come back, once the mouse buttons are up. While it has the mouse, the game gets the mouse's buttons and wheel as the keyboard layout binds them. `DES_MOUSE_SENS` is the sensitivity in degrees per count of the mouse (default `0.09`, a full turn per 4000 counts), and `DES_MOUSE_INVERT_Y=1` and `DES_MOUSE_INVERT_X=1` invert it. The camera turns once a game frame, so it is smoothest at 60 fps or more. While locked on, a move of the mouse to the left or right switches targets as a flick of the right stick does (`DES_MOUSE_FLICK`: how far, in degrees of turn at that sensitivity; default 15, 0 turns it off). It needs the menus (not with `DES_UI=0`).

The window opens at once. The first start after an update then fills the shader cache in `gamedata/shader_cache` behind a progress bar (under a minute; Skip leaves the rest to compile as the game draws them), so the game never stops to compile one. This runs the game for up to an hour (`play.sh 120` for two) with sound, at normal priority on every CPU, and logs to `work/tmp/play.txt`. Saves go to `gamedata/dev_hdd0/home/00000001/savedata`. F9 captures a frame for `rsx_replay`, F10 saves screenshots, and F11 starts and stops a profile session (frame times and the worst frames, in `work/tmp/prof`). The debug and test tools are described in [`docs/28`](docs/28-in-game.md#debug-and-test-tools-2026-10-14).

## Performance

The frame is a chain of host threads: HighGraphics (a PPU thread) fills a command ring, an SPU program (image 2) turns the commands into RSX commands, and the FIFO thread records them and renders the frame with Direct3D 12 at each flip. The game waits for the RSX to finish a frame before it builds the next one, so each of these threads is on the frame's path. The work so far, the measurements behind each change and the things that did not help are in [`docs/28`](docs/28-in-game.md), from "Palace frame rate" on.

The scripts in `tools/des/` measure a change: `knobab.sh` alternates a live knob within one run, `exeab.sh` alternates two builds, `kprof.sh` profiles named threads, and `soak.sh` runs about six minutes at `DES_FPS=120` (the check before any change to the atomics).

## Layout

```
docs/       findings, root causes and tools, numbered in the order they happened
port/       the Demon's Souls glue: CMake, heap shim, options, stubs
scripts/    lift, patch and build helpers
tools/des/  run, regression, capture and analysis scripts
tools/ghidra/  headless Ghidra helpers for reading the game's code
ps3recomp/  the recompiler and runtime (separate repository, not tracked here)
work/       your decrypted executable, lifted code and logs (gitignored)
gamedata/   the emulated hard drive, with your saves (gitignored)
```

## Legal

This project distributes only its own code, scripts and documentation. It contains no game executable, no lifted or decompiled game code, no game assets, no Sony firmware and no keys; those stay on your machine, made from your own copy (see [`docs/05-legal.md`](docs/05-legal.md) and `.gitignore`).

*Demon's Souls* is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment, FromSoftware or Atlus.

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and its contributors: the recompiler and runtime this is built on.
- [RPCS3](https://github.com/RPCS3/rpcs3), used as a behavioural reference for the PS3 system libraries. No RPCS3 code is copied.

## License

MIT, for this repository's own code, scripts and documentation ([`LICENSE`](LICENSE)). ps3recomp is MIT-licensed by its authors.
