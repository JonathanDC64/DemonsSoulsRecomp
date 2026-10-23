<div align="center">

# Demon's Souls Recomp

**Demon's Souls, running natively on PC.**

</div>

This is a PC port of *Demon's Souls* (PS3, 2009). The game's original code has been converted to run directly on Windows and Linux, with no emulator.

You need your own copy of the game: the US version, **BLUS30443**. This project includes no game files.

## Features

- Higher frame rates, with the game's timing fixed so it still plays the way it should
- Higher resolutions and ultrawide screens
- Keyboard and mouse controls, laid out like the Souls games on PC
- Controller support, including Xbox and PlayStation controllers
- Change which buttons and keys do what
- An in-game menu for settings, most of which apply right away
- Windows, and Linux (newer and less tested)
- Online play (experimental)

## Status

Early. The game is playable from the start through the first level, Boletarian Palace 1-1.

## What you need to play

- Your own copy of Demon's Souls for the PS3, US version (BLUS30443)
- A PC running Windows 10 or 11, or Linux
- A graphics card that supports DirectX 12

## How to play

An easy setup tool is on the way. It will turn your copy of the game into the PC version for you.

Until then, the game has to be built from source: see [Building from source](#building-from-source).

Once the game is running, press **F1** (or **L3 + R3** on a controller) to open the menu and its settings.

## Building from source

This is the technical part. The full notes are in [`docs/`](docs/).

**Tools**

| Windows | Linux |
|---|---|
| Visual Studio 2022 Build Tools, LLVM (clang), CMake, Ninja, Python 3, Git Bash | clang or GCC, CMake, Ninja, Python 3, SDL2, Vulkan and FFmpeg development files, vkd3d ([details](docs/76-native-linux.md#building)) |

**From your copy of the game**

- The game disc's files (`PS3_GAME`)
- [RPCS3](https://rpcs3.net), to decrypt the game's `EBOOT.BIN`
- `libsre.prx` from your PS3 firmware, decrypted

**Steps**

1. Get this repository, and its runtime inside it:
   ```sh
   git clone https://github.com/JonathanDC64/DemonsSoulsRecomp.git
   cd DemonsSoulsRecomp
   git clone https://github.com/JonathanDC64/ps3recomp.git
   ```
2. In RPCS3, decrypt `PS3_GAME/USRDIR/EBOOT.BIN` (*Utilities > Decrypt PS3 Binaries*) to `work/game/EBOOT.ELF`. Copy `PS3_GAME/ICON0.PNG` next to it, for the program's icon.
3. Convert the game's main code:
   ```sh
   python ps3recomp/tools/ppu_loader.py work/game/EBOOT.ELF -o work/loader
   python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
     --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
     --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post,0x724E40=des_menu_tick" \
     --insn-hooks @port/insn_hooks.txt --shadow-stack --chunk-lines 150000 --output work/recomp -j 16
   python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
   python scripts/apply_boot_patches.py work/recomp
   ```
   Then the SPU code with `python scripts/build_spu.py --eboot work/game/EBOOT.ELF` ([docs/09](docs/09-spu-spurs.md)), which first cuts the SPU programs out of the EBOOT, and libsre with `python scripts/build_libsre.py <libsre.prx>`. The lifter's flags above are also in `scripts/lift_ppu.json`, which setup reads; change both together.
4. Compile the runtime, then the game. On Windows:
   ```sh
   cmake -S ps3recomp -B ps3recomp/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
   cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
     -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=
   cmake --build ps3recomp/build --target ps3recomp_runtime rsx_shader_precompile
   cmake --build port/build --target des_boot --parallel 4
   ```
   On Linux, see [docs/76](docs/76-native-linux.md#building).
5. Copy `tools/des/local.env.example` to `tools/des/local.env`, set `PS3_VFS_ROOT` in it to the folder that holds `PS3_GAME`, and start the game with `tools/des/play.sh`.

Everything made from your copy of the game stays in `work/` and `gamedata/`, which git ignores. **Never share your build:** it contains code converted from your game.

## Legal

This project contains only its own code, scripts and documentation. It has no game code, no game files, no Sony firmware and no keys; you make those from your own copy ([docs/05](docs/05-legal.md)).

*Demon's Souls* is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment, FromSoftware or Atlus.

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and its contributors: the recompiler and runtime this port is built on
- [RPCS3](https://github.com/RPCS3/rpcs3): a reference for how the PS3 behaves (no RPCS3 code is copied)
- [RmlUi](https://github.com/mikke89/RmlUi) for the menus, and [vkd3d](https://gitlab.winehq.org/wine/vkd3d) for Linux

## License

MIT, for this repository's own code, scripts and documentation ([`LICENSE`](LICENSE)). ps3recomp is MIT-licensed by its authors.
