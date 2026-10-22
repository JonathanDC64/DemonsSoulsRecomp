<div align="center">

# Demon's Souls PC

**A native PC port of *Demon's Souls* (PS3, 2009), made by static recompilation.**

Windows (Direct3D 12) &nbsp;·&nbsp; Linux (Direct3D 12 on vkd3d, early) &nbsp;·&nbsp; Version 0.1.0

</div>

> [!IMPORTANT]
> This repository contains **no game code or data**. You need your own copy of the game: the US disc, **BLUS30443**. See [Legal](#legal).

## Contents

- [About](#about)
- [Status](#status)
- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Running](#running)
- [Settings](#settings)
- [Controls](#controls)
- [Online play](#online-play)
- [Performance](#performance)
- [Problems and reports](#problems-and-reports)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [Releases](#releases)
- [Legal](#legal) · [Credits](#credits) · [License](#license)

## About

The game's machine code is translated to C ahead of time and compiled into a native program. There is no emulator at run time.

| Part | What it does |
|---|---|
| **Lifting** | The game's PowerPC (PPU) and SPU code is translated to C, function by function. |
| **Runtime** | [ps3recomp](https://github.com/sp00nznet/ps3recomp) reimplements the PS3's system libraries. This port uses the fork [JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp). |
| **Graphics** | The PS3's RSX commands are drawn with Direct3D 12. On Linux the same renderer runs on [vkd3d](https://gitlab.winehq.org/wine/vkd3d). |
| **Port** | This repository: the Demon's Souls glue, the menus and settings, fixes for high frame rates, and the build and test tools. |

## Status

The game is playable from boot through **Boletarian Palace 1-1**.

| Area | State |
|---|---|
| Start | Boot logos, intro movies with sound and subtitles, the title screen, character creation |
| Areas | The tutorial, the Nexus, Boletarian Palace 1-1 |
| Graphics | Lit and textured scenes, shadows, the HUD, particles, light shafts and haze |
| Sound | Sound effects and music |
| Saves | Saving and loading |
| Input | Keyboard and mouse (the Souls PC layout), Xbox controllers, DualSense and DualShock 4 |
| Frame rate | 30 fps (the original) up to 500 fps, or unlocked |
| Online | New: a Network tab for the community servers. Tested only against a server on the same PC so far ([Online play](#online-play)) |
| Linux | Native build. Tested in WSL only so far ([docs/76](docs/76-native-linux.md)) |

### Known issues

- **Tested areas:** testing so far covers the tutorial, the Nexus and Boletarian Palace 1-1.
- **Cutscenes above 30 fps:** they play at 30 by default, because their camera moves in 30 Hz steps. *Cutscene frame rate* in Settings keeps your frame rate and smooths the camera.
- **High frame rates:** above 60 fps is less tested than 30 and 60.
- **Online:** new. It has been tested only against a server on the same PC, not a live one. Co-op and invasions are not tested yet, and RPCS3 and PS3 players are not in your world.
- **Linux:** no shader precompile at start, no pipeline cache between runs, no crash dialog ([docs/76](docs/76-native-linux.md#missing)).
- **Building:** not yet a single command. A setup tool is planned ([docs/52](docs/52-player-setup.md)).
- **ps3recomp:** only Demon's Souls is tested on this fork's branch of the runtime.

## Features

### Graphics

- **Render resolution** up to 300% (4K), with a sharp scaling filter.
- **Ultrawide** pictures (21:9, 32:9, or the display's shape). The HUD stays 16:9 in the middle, or stretches.
- **Anti-aliasing** (FXAA), anisotropic filtering and texture filtering.
- **Depth of field** and **motion blur** can be turned off.
- **Window or borderless fullscreen.** The window resizes while the game runs.
- **No shader stutter on Windows:** the first start compiles the game's shaders behind a progress bar.

### Frame rate

- **30 fps (original) to 500 fps, or unlocked,** with V-sync on or off.
- **Game timing fixed for high frame rates**, where the game assumed 30 fps:
  - movement speed, the camera's eases and steep-slope slides;
  - particles, character fades, loading souls and menu text;
  - the bow's aim view and the character creation preview;
  - ragdolls and breakables: physics steps at most 30 times a second and is drawn smoothly between steps;
  - other players and wandering ghosts.

### Controls

- **Keyboard and mouse** like the Souls games on PC, with optional **mouse look**.
- **Gamepads:** Xbox controllers, DualSense and DualShock 4, over USB or Bluetooth.
- **Remapping** for gamepad buttons, keys and mouse buttons (*Change Buttons*).
- **On-screen keyboard** in the game's menus, usable with a controller.

### Interface

- **Start screen** (Play, Settings, Quit) and an **in-game menu** (F1, or L3 + R3).
- **Settings** that mostly apply at once, while the game runs.
- **Trophies:** a pop-up when you earn one, and a Trophies window.
- **Problem reports:** one click zips what a bug report needs, even from a frozen game.

## Requirements

| | Windows | Linux |
|---|---|---|
| **System** | Windows 10 or 11, a Direct3D 12 GPU | A Vulkan driver. Tested in WSL (Ubuntu 24.04) only |
| **Compiler** | Visual Studio 2022 or its Build Tools (MSVC, Windows SDK), and LLVM (`clang-cl`) | clang or GCC |
| **Build tools** | CMake, Ninja, Git Bash (for `tools/des/`) | CMake, Ninja |
| **Libraries** | — | SDL2, the Vulkan headers and loader, FFmpeg's headers (`libavcodec-dev`), vkd3d 2.1 built from source |
| **Python** | Python 3 with `ps3recomp/requirements.txt` | the same |

**Game files**, on either system:

- Your own Demon's Souls disc or dump (**BLUS30443**).
- [RPCS3](https://rpcs3.net), to decrypt `EBOOT.BIN`.
- Your own decrypted `libsre.prx`, from your PS3 firmware.

## Building

The build is not yet a single command. It takes six steps:

| # | Step | Output |
|---|---|---|
| 1 | Get the code | this repository, with `ps3recomp/` inside |
| 2 | Decrypt the game | `work/game/EBOOT.ELF` |
| 3 | Analyse the executable | `work/loader/` |
| 4 | Lift the PPU code | `work/recomp/` |
| 5 | Lift the SPU code and libsre | the SPU images and `work/libsre/` |
| 6 | Compile | `port/build/des_boot.exe` |

**1. Get the code.** Clone this repository, then the fork into `ps3recomp/` inside it. Use the fork's master, or at least the commit named in [`ps3recomp.pin`](ps3recomp.pin).

```sh
git clone https://github.com/JonathanDC64/DemonsSoulsPC.git
cd DemonsSoulsPC
git clone https://github.com/JonathanDC64/ps3recomp.git
```

**2. Decrypt the game.**

- In RPCS3, *Utilities > Decrypt PS3 Binaries*: decrypt `PS3_GAME/USRDIR/EBOOT.BIN` to `work/game/EBOOT.ELF`.
- Copy `PS3_GAME/ICON0.PNG` to `work/game/` as well. The build makes the exe's icon from it; without it the exe has no icon.

**3. Analyse the executable.** This writes `EBOOT.functions.json` and `EBOOT.imports.json`.

```sh
python ps3recomp/tools/ppu_loader.py work/game/EBOOT.ELF -o work/loader
```

**4. Lift the PPU code** and apply the boot shim (details in [docs/27](docs/27-relift-auto-save-notice.md#re-lift-recipe)):

```sh
python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
  --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
  --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post,0x724E40=des_menu_tick" \
  --insn-hooks @port/insn_hooks.txt --shadow-stack \
  --output work/recomp -j 16
python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp
python scripts/apply_boot_patches.py work/recomp
```

**5. Lift the SPU code and libsre.**

- `scripts/build_spu.py` lifts the SPU images and jobs ([docs/09](docs/09-spu-spurs.md), [docs/26](docs/26-first-pixels-loading-screen.md)). It also swaps in the runtime's native blocks for parts of the graphics ring consumer (image 2), so use the fork's `spu_lifter.py`.
- `scripts/build_libsre.sh <libsre.prx>` lifts libsre.

**6. Compile** the runtime, then the game. Compiling the 14 lifted chunks takes about 15 minutes.

<details>
<summary><b>Windows</b></summary>

```sh
cmake -S ps3recomp -B ps3recomp/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
  -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=
cmake --build ps3recomp/build --target ps3recomp_runtime rsx_shader_precompile
cmake --build port/build --target des_boot --parallel 4
```

</details>

<details>
<summary><b>Linux</b></summary>

Build vkd3d 2.1 from its release tarball first. Ubuntu 24.04's package (1.2) has no HLSL compiler.

```sh
./configure --prefix=$HOME/vkd3d && make -j && make install
export PKG_CONFIG_PATH=$HOME/vkd3d/lib/pkgconfig
```

Then the runtime and the port:

```sh
cmake -S ps3recomp -B rt -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_RSX_D3D12_VKD3D=ON -DPS3RECOMP_FFMPEG_RUNTIME=ON
ninja -C rt ps3recomp_runtime
cmake -S port -B build -G Ninja -DPS3RECOMP_DIR=<ps3recomp> -DPS3RECOMP_BUILD_DIR=rt -DRECOMP_DIR=<lift>
ninja -C build
```

Each lifted chunk needs about 2 GB of memory to compile. More in [docs/76](docs/76-native-linux.md#building).

</details>

<details>
<summary><b>Build options</b></summary>

| CMake option | Effect |
|---|---|
| `-DDES_DEV=ON` | A developer build with test cheats (god mode, attack power, movement speed, `DES_WARP`) in a Developer tab ([docs/55](docs/55-dev-cheats.md)). |
| `-DDES_SHADOW_STACK=ON` | Keeps the guest call stack for backtraces (`ds_dump_shadow`). Off by default, which is about 2% faster. |
| `-DDES_DEBUG_INFO=ON` | Builds with debug information. |

**Developer builds:**

- Configure them in their own folder, such as `port/build_dev`. CMake keeps the option in the folder's cache, and `tools/des/` uses `port/build`.
- Run one with `tools/des/play.sh DES_EXE=port/build_dev/des_boot.exe`.
- A developer build says what it is: in the window title, in the log, and at configure and link time.
- **Never share one.** Before sharing any build, run `python tools/ci/check_release_no_dev.py <folder or zip>`.

</details>

## Running

1. Copy `tools/des/local.env.example` to `tools/des/local.env`.
2. Set `PS3_VFS_ROOT` in it to the folder that holds `PS3_GAME`.
3. Start the game:

```sh
tools/des/play.sh
```

**Run options** go after it as `NAME=value`, for example:

```sh
tools/des/play.sh DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 DES_FPS=60
```

- Most settings have a run option, shown in their help text and in the [Settings](#settings) tables.
- A run option given on the command line wins over the settings file.
- `play.sh` runs the game for up to an hour; `play.sh 120` gives it two.

**On the first start** after an update (Windows), the game compiles its shaders behind a progress bar. It takes under a minute. *Skip* leaves the rest to compile as the game draws them.

### Options with no settings row

| Option | Effect |
|---|---|
| `PS3_SAIL_MOVIES=0` | Skips every movie. |
| `DES_UI=0` | No start screen, menus or settings. The keyboard becomes a plain PS3 pad: arrows, Z X A S, Enter and Tab. |
| `DES_SKIP_UI=1` | Skips the start screen once. |
| `DES_HUD_FADES=1` | Experimental, for wide pictures: full-screen fades and the dim behind menus cover the whole width. |

### Where files go

| What | Where |
|---|---|
| Settings | `%APPDATA%\DemonsSoulsRecomp\DemonsSouls\settings.ini` |
| Logs and reports | the same folder: `logs\`, `crashes\`, `reports\` |
| Portable mode | a `portable.txt` next to the exe puts that folder in `data\` beside it |
| Saves | `gamedata/dev_hdd0/home/00000001/savedata` |
| Shader cache | `gamedata/shader_cache` |
| `play.sh`'s log | `work/tmp/play.txt` |

**On Linux:**

- The data folder is `~/.local/share/DemonsSoulsRecomp/DemonsSouls`.
- `tools/linux/appimage.sh` packs your own build into one AppImage, for example for a Steam Deck ([docs/76](docs/76-native-linux.md#an-appimage-of-your-build)).
- The AppImage holds code made from your copy of the game, so **never share it**.

## Settings

Open the menu (F1, or L3 + R3 on a gamepad) and choose **Settings**.

- Rows apply at once, except those marked **↻**, which apply at the next start.
- A row's run option (in the tables below) works on the command line too, and wins over the saved setting.

<details open>
<summary><b>Display</b></summary>

| Setting | Choices | Default | Run option |
|---|---|---|---|
| Display mode | Window, Fullscreen (also Alt+Enter) | Window | `DES_FULLSCREEN` |
| Aspect ratio ↻ | 16:9, 21:9 (three sizes), 32:9, Match the display | 16:9 | `DES_ASPECT` |
| HUD ↻ | Centred 16:9, Stretched | Centred 16:9 | `DES_HUD` |
| Render resolution | 100%, 150% (1080p), 200% (1440p), 300% (4K) | 100% | `RSX_SCALE` |
| Scaling filter | Bilinear, Sharp | Bilinear | `RSX_OUTFILTER` |
| Texture filtering | Original, Nearest, Bilinear, Trilinear | Original | `RSX_TEXFILTER` |
| Anisotropic filtering | Original (4x), Off, 2x, 4x, 8x, 16x | Original | `RSX_ANISO` |
| Anti-aliasing | Off, FXAA | Off | `RSX_AA` |
| Depth of field | On, Off | On | `DES_DOF` |
| Frame rate | 30 fps (original), 30 fps real frame time, 60, 120, 144, 165, 240, Unlocked, Custom (30-500) | 30 fps (original) | `DES_FPS` |
| Cutscene frame rate | 30 fps (original), Same as Frame rate | 30 fps | `DES_CUTFPS` |
| V-sync | On, Off | On | `DES_VSYNC` |
| Motion blur | On, Off | On | `DES_MOTION_BLUR` |

- `DES_FPS` takes a number from 30 to 500, or `unlocked`.
- `DES_ASPECT` also takes any `w:h` or `WxH`, such as `3440x1440`, or `auto`.

</details>

<details>
<summary><b>Game</b></summary>

| Setting | Choices | Default | Run option |
|---|---|---|---|
| Boot logos ↻ | Show, Skip, Last logo only | Show | `DES_SKIP_INTRO` (0, 1, 2) |
| Skip opening movie | On, Off | Off | `DES_SKIP_INTRO_MOVIE` |
| Continue automatically ↻ | On, Off | Off | `DES_AUTO_CONTINUE` |
| Sound | On, Off | On | `AUDIO_MUTE=1` mutes |

</details>

<details>
<summary><b>Controls</b></summary>

| Setting | Choices | Default | Run option |
|---|---|---|---|
| Change Buttons | gamepad buttons, keys and mouse buttons | the game's | `PAD_MAP`, `PAD_KEYS` |
| Mouse look | On, Off | Off | `DES_MOUSE_LOOK` |
| Mouse sensitivity | a percentage | 100% | `DES_MOUSE_SENS` (degrees per count, default 0.09) |
| Invert vertical | On, Off | Off | `DES_MOUSE_INVERT_Y` |
| Invert horizontal | On, Off | Off | `DES_MOUSE_INVERT_X` |
| Switch targets | Off, Short, Medium, Long move | Medium | `DES_MOUSE_FLICK` (0, 8, 15, 30) |

</details>

<details>
<summary><b>Network</b></summary>

| Setting | Choices | Default | Run option |
|---|---|---|---|
| Online ↻ | On, Off | On | `DES_ONLINE` |
| Server ↻ | The Archstones, Local test server, Custom | The Archstones | `DES_NET_SERVER` |
| Custom server ↻ | an IP address or host name | — | `DES_NET_CUSTOM` |
| Online name ↻ | 3 to 16 letters, digits, `-` or `_` | — | `DES_NET_NAME` |
| Matchmaking password | any text | none: play with everyone | `DES_NET_PASSWORD` |
| World tendency | With the server's, Yours only | With the server's | `DES_NET_TENDENCY` |
| Message ratings | Every 5 minutes, Every 30 seconds | Every 5 minutes | `DES_NET_RATING_POLL` |
| P2P port ↻ | 1024-65535 | 3658 | `DES_NET_P2P_PORT` |
| Your movement updates | Original, Smoother | Original | `DES_NET_SYNC` |
| Messages | On, Off | On | `DES_NET_MESSAGES` |
| Bloodstains | On, Off | On | `DES_NET_BLOODSTAINS` |
| Ghosts | On, Off | On | `DES_NET_GHOSTS` |

</details>

<details>
<summary><b>Interface</b></summary>

| Setting | Choices | Default |
|---|---|---|
| UI scale | Auto, 75%, 100%, 125%, 150% | Auto |
| Start screen | On, Off | On |
| Menu hint | On, Off | On |
| Trophy pop-ups | On, Off | On |
| Gamepad menu buttons | L3 + R3, Select + Start | L3 + R3 |

</details>

## Controls

### Keyboard and mouse

The layout follows the Souls games on PC. The mouse buttons and the wheel need **Mouse look** on.

| Action | Keyboard and mouse | PS3 button |
|---|---|---|
| Move | W A S D | Left stick |
| Walk | Left Alt (hold) | — |
| Camera | Mouse | Right stick |
| Attack | Left button | R1 |
| Strong attack | Shift + left button | R2 |
| Guard | Right button | L1 |
| Parry | Left Ctrl, or Shift + right button | L2 |
| Roll, backstep, dash | Space | Circle |
| Action | E | Cross |
| Use item | R | Square |
| Two-hand | F | Triangle |
| Lock on | Q, or middle button | R3 |
| Left stick press | C | L3 |
| D-pad | Arrows | D-pad |
| Switch spell / item | Wheel up / down | D-pad up / down |
| Switch right / left hand | Shift + wheel up / down | D-pad right / left |
| Game menu | Esc | Start |
| Select | G or Tab | Select |

- A chord works alone: Shift + left button is R2, not R1 as well.
- The game's own menus still show the PS3's buttons.

### Gamepads

- **Windows:** Xbox controllers (XInput), DualSense and DualShock 4, over USB or Bluetooth.
- **Linux:** any controller SDL knows.

### Remapping

**Settings > Controls > Change Buttons** sets which gamepad button presses each PS3 button, and which keys, mouse buttons or wheel notches do each action. Each action takes two inputs, each with an optional Shift, Ctrl or Alt. *Restore Default Buttons* puts back the original layout.

<details>
<summary><b>Remapping from the command line</b></summary>

| Option | Format | Example |
|---|---|---|
| `PAD_MAP` | the game's button `=` the gamepad's button | `PAD_MAP=cross=circle,circle=cross` |
| `PAD_KEYS` | an action `=` up to four inputs separated by `\|`; `none` for none | `PAD_KEYS=cross=e\|enter,r2=shift+mouse1,l3=none` |

- **Inputs:** a letter, digit or key name; `f1` to `f24`; a virtual-key code; `mouse1` to `mouse5`; `wheelup` or `wheeldown`. Each can have `shift+`, `ctrl+` or `alt+` in front.
- **Buttons:** `cross circle square triangle l1 r1 l2 r2 l3 r3 start select up down left right`.
- **Keyboard only:** `ls_up ls_down ls_left ls_right` (left stick), `rs_up rs_down rs_left rs_right` (right stick) and `walk`.

</details>

<details>
<summary><b>Mouse look details</b></summary>

- In play the cursor is hidden and held in the window. It is free on the title screen, in loading screens and in cutscenes.
- F1, switching to another window or moving the window frees it. It is taken again when you come back, once the mouse buttons are up.
- The camera turns once per game frame, so mouse look is smoothest at 60 fps or more.
- While locked on, a sideways mouse move switches targets, like a flick of the right stick. *Switch targets* sets how far.

</details>

### Hotkeys

| Key | Action |
|---|---|
| F1, or L3 + R3 on a gamepad | Open or close the menu (the game gets no input while it is open) |
| Alt+Enter | Window or fullscreen |
| F9 | Capture the frame for `rsx_replay` (a developer tool) |
| F10 | Save the next presented frames as screenshots |
| F11 | Start or stop a profile session (frame times and the worst frames, in `work/tmp/prof`) |

## Online play

> [!NOTE]
> Online play is new. So far it has been tested only against a server on the same PC, not against a live server.

- **Setup:** set it in **Settings > Network**. Online is on by default, but it needs an **online name**; with none, the game stays offline.
- **Servers:**
  - *The Archstones*, a community server run by a volunteer (the default);
  - a *local test server* on this PC;
  - *Custom*, any address you type.
- **What you share:** messages, bloodstains, ghosts, world tendency and message ratings, with everyone on the same server.
- **Co-op and invasions** connect you straight to the other player, so they can see your IP address, as on a PS3. They have not been tested yet.
- **Who is in your world:** only players of this port on the same server. RPCS3 and PS3 players are not.
- **Kept offline:** test runs (the `tools/des/` scripts other than `play.sh`) unless given `DES_ONLINE=1`, and any session with a developer cheat on.

More in [docs/69](docs/69-online-play.md) and [docs/46](docs/46-network-and-platforms.md).

## Performance

| Where | Frame rate |
|---|---|
| Deep in Boletarian Palace 1-1, uncapped | about **163 fps** (147 before the image 2 changes of 2026-10-21) |

Measured on a Ryzen 7 9800X3D with an RTX 5070 Ti, with V-sync off ([docs/72](docs/72-image2-dma-and-native-loop.md)).

**How a frame is made.** Three host threads each run close to a full core, and each is on the frame's path:

1. **HighGraphics**, a game thread, fills a command ring.
2. **Image 2**, an SPU program, turns those commands into RSX commands.
3. **The FIFO thread** reads the RSX commands and draws the frame with Direct3D 12.

> [!TIP]
> To measure past the display's refresh rate, turn V-sync off: `DES_FPS=unlocked RSX_VSYNC=0 DES_VSYNC=0`. With V-sync on, the frame rate stops at the display's refresh, for example 144 fps.

**Tools** in `tools/des/`:

| Script | What it does |
|---|---|
| `exeab.sh` | Alternates two builds, run after run (an A/B) |
| `knobab.sh` | Alternates a live option within one run |
| `kprof.sh` | Profiles named threads |
| `regress.sh` | Walks a fixed route and compares screenshots with a known good set |
| `soak.sh` | About six minutes at 120 fps, the check before any change to the atomics |
| `tools/pgo/pgo.sh` | A profile-guided build, 6 to 8% faster: a developer tool only ([docs/58](docs/58-pgo-builds.md)) |

The work so far, the measurements behind each change and the dead ends are in [docs/28](docs/28-in-game.md) (from "Palace frame rate" on) and [docs/71](docs/71-physics-step-rate.md) to [docs/73](docs/73-physics-step-after-breaks.md).

## Problems and reports

- **Report an issue** (in the menu bar) saves `reports\issue-<time>.zip`: a screenshot, your system and settings, and the log. It also works when the game has frozen.
- **After a crash or a hang**, a dialog offers *Report on GitHub* with the same kind of zip.
- **No private data or game content goes in a report:** [docs/74](docs/74-problem-reports.md) lists what they hold.
- **Logs** are in the data folder's `logs\` (see [Where files go](#where-files-go)).

## Repository layout

| Folder | What it holds |
|---|---|
| `docs/` | Findings, root causes and tools, numbered in the order they happened |
| `port/` | The Demon's Souls glue: CMake, options, fixes for high frame rates, stubs |
| `port/ui/` | The menus and settings (RmlUi) |
| `scripts/` | Lift, patch and build helpers |
| `tools/des/` | Run, test, A/B, capture and analysis scripts |
| `tools/ci/`, `tools/release/` | Release checks and packaging |
| `tools/ghidra/` | Headless Ghidra helpers for reading the game's code |
| `tools/linux/` | An AppImage of your own Linux build |
| `tools/pgo/` | Profile-guided builds (a developer tool) |
| `ps3recomp/` | The recompiler and runtime: a separate repository, not tracked here |
| `work/` | Your decrypted executable, lifted code and logs (git-ignored) |
| `gamedata/` | The emulated hard drive, with your saves (git-ignored) |

## Documentation

| Topic | Read |
|---|---|
| The current state, and the debug and test tools | [docs/28](docs/28-in-game.md) |
| Menus, settings, the data folder and reports | [docs/32](docs/32-launcher-ui.md), [docs/74](docs/74-problem-reports.md) |
| High frame rates | [docs/40](docs/40-cutscenes-unlocked-fps.md), [docs/42](docs/42-high-fps-game-logic.md), [docs/71](docs/71-physics-step-rate.md) |
| Resolution, ultrawide and the HUD | [docs/37](docs/37-resolution-scaling.md), [docs/39](docs/39-ultrawide-and-hud.md) |
| Keyboard and mouse | [docs/41](docs/41-mouse-and-keyboard.md) |
| Online play | [docs/69](docs/69-online-play.md), [docs/46](docs/46-network-and-platforms.md) |
| Native Linux | [docs/76](docs/76-native-linux.md) |
| Performance | [docs/72](docs/72-image2-dma-and-native-loop.md), [docs/73](docs/73-physics-step-after-breaks.md), [docs/58](docs/58-pgo-builds.md) |
| SPU and SPURS | [docs/09](docs/09-spu-spurs.md) |
| Setup plan, versions and releases | [docs/52](docs/52-player-setup.md), [docs/54](docs/54-versioning-and-release.md) |
| Legal | [docs/05](docs/05-legal.md), [docs/51](docs/51-legal-decryption.md) |

## Releases

- Versions follow [Semantic Versioning](https://semver.org); this is **0.1.0** ([`VERSION`](VERSION)).
- **A release is source code only:** a zip of this repository and the matching ps3recomp. It has no binaries and nothing made from the game.
- You build a release the same way, with your own game files ([docs/54](docs/54-versioning-and-release.md)).

## Legal

This project distributes only its own code, scripts and documentation. It contains:

- no game executable;
- no lifted or decompiled game code;
- no game assets;
- no Sony firmware and no keys.

Those stay on your machine, made from your own copy ([docs/05](docs/05-legal.md), [`.gitignore`](.gitignore)). A CI check fails when a game-derived file shows up in the repository.

*Demon's Souls* is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment, FromSoftware or Atlus.

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and its contributors: the recompiler and runtime this port is built on.
- [RPCS3](https://github.com/RPCS3/rpcs3): a behavioural reference for the PS3's system libraries. No RPCS3 code is copied.
- [RmlUi](https://github.com/mikke89/RmlUi): the menus.
- [vkd3d](https://gitlab.winehq.org/wine/vkd3d): Direct3D 12 on Vulkan, for Linux.

## License

MIT for this repository's own code, scripts and documentation ([`LICENSE`](LICENSE)). ps3recomp is MIT-licensed by its authors.
