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

Known issues:

- Some transparent effects (a fog wall, the soul-form aura, fog in the first cutscene) vanish for single frames.
- The game runs at about 28-30 fps (its own cap is 30) and frame pacing is uneven. Higher frame rates, shader precompilation and faster loading are still to do.
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
     --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free" --shadow-stack \
     --output work/recomp -j 16
   python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
   python scripts/apply_boot_patches.py work/recomp
   ```
5. Lift the SPU images and jobs (`scripts/build_spu.py`; see [`docs/09`](docs/09-spu-spurs.md) and [`docs/26`](docs/26-first-pixels-loading-screen.md)), and libsre (`scripts/build_libsre.sh <libsre.prx>`).
6. Configure and build the runtime and the game:
   ```
   cmake -S ps3recomp -B ps3recomp/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
   cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
     -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=
   cmake --build ps3recomp/build --target ps3recomp_runtime
   cmake --build port/build --target des_boot --parallel 4
   ```
   Recompiling the 14 lifted chunks takes about 15 minutes.

## Running

Copy `tools/des/local.env.example` to `tools/des/local.env` and set `PS3_VFS_ROOT` to the folder that holds `PS3_GAME`. Then:

```
tools/des/play.sh
```

This runs the game for up to an hour (`play.sh 120` for two) with sound, at normal priority on every CPU, and logs to `work/tmp/play.txt`. Saves go to `gamedata/dev_hdd0/home/00000001/savedata`. F9 captures a frame for `rsx_replay`, F10 saves screenshots. The debug and test tools are described in [`docs/28`](docs/28-in-game.md#debug-and-test-tools-2026-10-14).

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
