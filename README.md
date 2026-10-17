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
- Frame rate: the game's own 30 fps cap by default, and 60 or more with `DES_FPS` (see [Running](#running)). Boletarian Palace runs at about 110 fps with the 60 fps mode uncapped (measured with `DES_FPS=60 VBLANK_MHZ=120000`). Shaders are precompiled, so the game never stops to compile one.

Known issues:

- Some transparent effects (a fog wall, the soul-form aura, fog in the first cutscene) vanish for single frames.
- Above 30 fps, cutscenes switch back to 30 on their own: the game's cutscene manager moves things at 30 Hz, and they judder otherwise. Above 60, physics is untested (RPCS3 warns of trouble there).
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
2. Decrypt `PS3_GAME/USRDIR/EBOOT.BIN` with RPCS3 (Utilities > Decrypt PS3 Binaries) to `work/game/EBOOT.ELF`.
3. Analyse the executable: `python ps3recomp/tools/ppu_loader.py work/game/EBOOT.ELF -o work/loader` writes `EBOOT.functions.json` and `EBOOT.imports.json`.
4. Lift the PPU code and apply the boot shim. The commands are in [`docs/27`](docs/27-relift-auto-save-notice.md#re-lift-recipe):
   ```
   python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
     --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
     --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post" --shadow-stack \
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

- `DES_FPS=60`: the game's 60 fps mode, with real frame times. `DES_FPS=61` to `240` also raises the frame cap to that many frames a second. `DES_FPS=30` keeps 30 with real frame times.
- `DES_VSYNC=0`: present each frame as soon as it is drawn, with tearing, instead of waiting for the display's refresh (the default, `DES_VSYNC=1`). With vsync, a frame rate above the display's refresh (say `DES_FPS=240` on a 144 Hz display) is shown at the refresh. If the system cannot tear, it keeps vsync and says so once in the log.
- `DES_SKIP_INTRO=1`: shorter boot logos.
- `DES_AUTO_CONTINUE=1`: presses through the title screen into the first save in the list.
- `PS3_SAIL_MOVIES=0`: skip the intro movies.

For example `tools/des/play.sh DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 DES_FPS=60`.

It first compiles the game's shaders into `gamedata/shader_cache` (about 20 s the first time, a few seconds after), so the game never stops to compile one. This runs the game for up to an hour (`play.sh 120` for two) with sound, at normal priority on every CPU, and logs to `work/tmp/play.txt`. Saves go to `gamedata/dev_hdd0/home/00000001/savedata`. F9 captures a frame for `rsx_replay`, F10 saves screenshots, and F11 starts and stops a profile session (frame times and the worst frames, in `work/tmp/prof`). The debug and test tools are described in [`docs/28`](docs/28-in-game.md#debug-and-test-tools-2026-10-14).

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
