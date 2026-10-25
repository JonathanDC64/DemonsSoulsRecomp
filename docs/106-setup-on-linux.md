# 106. Setup on Linux (as built)

Date: 2026-10-25. Status: built, and run end to end in WSL (Ubuntu 24.04). Not yet tried on a real Linux PC or a
Steam Deck.

docs/87 is the design. This note says what was built, where it differs from that design, how to try it, and what
is left.

## Short answer

- A Linux player downloads `DemonsSoulsRecomp-X.Y.Z-linux.tar.xz`, unpacks it anywhere and starts `des_host`.
  Setup's screens are the same as on Windows (docs/88, docs/89).
- As on Windows, setup downloads a pinned Python and a pinned compiler (clang 22.1.8), lifts the game and builds
  the game part, `des_game.so`, on the player's PC. Nothing is installed: everything stays in the data folder.
- The PC needs x86-64 Linux with glibc 2.35 or newer, a Vulkan driver, and `curl` and `tar`.
- In WSL: lift 35 s, compile about 3.5 min, then the game reached the Nexus.

## 1. What comes from where

| | What | Notes |
|---|---|---|
| **The player's system** | glibc 2.35 or newer | the floor of both the host and the compiler (section 3) |
| | a Vulkan driver and `libvulkan.so.1` | vkd3d opens the loader at run time |
| | X11 or Wayland, and PulseAudio, PipeWire or ALSA | SDL2 opens the desktop's own libraries when it starts |
| | `curl` and `tar` on PATH | only for the first Python download (section 2) |
| | `systemd-inhibit` | optional: keeps the PC awake during the build |
| **The download** (`-linux.tar.xz`) | `des_host`, executable, and `des_host.map` | built in CI (`host.yml`, `host-linux`) |
| | `libSDL2-2.0.so.0` (SDL 2.32.10) | beside the host; `RUNPATH $ORIGIN` finds it |
| | `libvkd3d.so.1`, `libvkd3d-shader.so.1`, `libvkd3d-utils.so.1` (vkd3d 2.1) | the same |
| | `licenses/SDL2/`, `licenses/vkd3d/` | zlib; LGPL 2.1 or later |
| | setup's engine, the lifters, ps3recomp's sources | the same tree as the Windows zip |
| **Setup's downloads** | Python 3.13.17 (python-build-standalone 20261021) | 35,356,674 bytes, pinned in the host |
| | the compiler, `llvm-linux-x64-22.1.8-2.tar.xz` | 97,274,644 bytes, pinned in `toolchain.json` |

What the player does **not** need: a C or C++ compiler, libc headers (`libc6-dev`), libstdc++, SDL2 or vkd3d
packages, root, or Steam.

## 2. What setup does on Linux

The same steps as on Windows (docs/81). What is Linux's own:
- **The data folder** is `$XDG_DATA_HOME/DemonsSoulsRecomp`, else `~/.local/share/DemonsSoulsRecomp`.
- **The first Python**: the host fetches python-build-standalone's 3.13.17 with the system's `curl` and unpacks it
  with its `tar`. Both are looked up on PATH, absolute folders only. The host checks the SHA-256 itself and writes
  the same stamp as on Windows. python.org has no embeddable Python for Linux.
- **The compiler**: `fetch.py` unpacks the tar.xz with `tarfile`'s "data" filter, which keeps the execute bit. It
  refuses absolute paths, `..`, device files and links that leave the folder.
- **The compile**: `compile.py` reads `game_build_linux.json`. clang does both the compile and the link (through
  `ld.lld`). The objects read no header from outside the source tree, the lift and the compiler's own folder, so no
  libc headers are needed. A new compile starts only while there is memory for it (`MemAvailable` in
  `/proc/meminfo`), and a compiler that the kernel ends with SIGKILL counts as out of memory.
- **Keeping awake**: `systemd-inhibit` holds a logind lock for the build, where it exists.
- **If the launcher dies**: the engine waits on a pidfd of the launcher and ends its own process group (the lifter
  and the compilers) when the launcher ends.
- **Play**: `des_host` opens `des_game.so` with `dlopen` (ps3recomp #122). The module has no NEEDED entry and
  exports `des_game_init` alone. Saves go beside the game, as on Windows.

## 3. Where it differs from docs/87

| docs/87 proposed | Built | Why |
|---|---|---|
| glibc floor 2.31 (Steam Runtime "sniper") or 2.35 | **2.35**: the host is built on `ubuntu-22.04` | decided 2026-10-24. LLVM's own Linux clang needs 2.35 anyway, so a lower floor for the host alone gains nothing |
| libstdc++ built in | **LLVM's libc++, built in** | ubuntu-22.04's libstdc++ 12 has no `<format>`, which the UI uses. The host needs no C++ library from the system, not even `libgcc_s` |
| SDL2 from the system (open question) | **SDL2 2.32.10 beside the host** | decided 2026-10-24: one known version on every PC |
| an AppImage | **a tar.xz** | asked for 2026-10-24. Setup's updater (docs/105) swaps plain files in the install folder; a tar keeps the execute bit, a zip unpacked by Python does not |
| the compiler: clang, ld.lld, clang's headers | the same, plus **`lib/libxml2.so.2`** and **zstd's licence** | LLVM links its lld with libxml2 as `libxml2.so.2`, which new distros no longer have; it is built in CI from GNOME's pinned 2.9.14 source with every option off. LLVM's Linux clang and lld have zstd 1.4.8 built in, so its BSD licence ships |
| Python: a standalone build or the system's | **python-build-standalone 3.13.17**, pinned | decided 2026-10-24: the same 3.13 line as Windows, the same on every PC |
| movies through the system's FFmpeg | **no movies yet** | the FFmpeg loader needs FFmpeg's headers in CI, and which FFmpeg to use is open (section 7) |

## 4. CI

All jobs pin their actions by commit SHA.
- **`host.yml`, `host-linux`**: des_host on `ubuntu-22.04` with LLVM 22.1.8's own clang and libc++, SDL2 and vkd3d
  built from pinned sources (and Khronos' Vulkan-Headers 1.3.275, since vkd3d 2.1 needs 1.3.228 or newer).
  `tools/ci/check_host_elf.py` then fails on any NEEDED library players may not have, a missing `RUNPATH $ORIGIN`, a
  shared C++ library or a `GLIBC_` version above 2.35. The artifact is `des_host-linux-<sha>`.
- **`llvm-archive.yml`, `target: linux`**: makes the compiler download from LLVM's 1.94 GB
  `LLVM-22.1.8-Linux-X64.tar.xz`, checked like the Windows one (size, SHA-256, LLVM's attestation, the GPG
  signature). The smoke step builds a `.so` with the game part's flags from the download alone, loads it and calls
  it. It makes a draft release; the maintainer publishes it.
- **`ci.yml`**:
  - `setup-tests`: setup's tests on `windows-2025` and `ubuntu-22.04`;
  - `build-desc-windows` and `build-desc-linux`: check that `game_build.json` and `game_build_linux.json` still
    match CMake's commands.
- **`release.yml`** packages the Linux tarball with the `host-linux` artifact and attaches it to the draft.
  **`release-check.yml`** makes it twice from stand-ins: the same bytes, and `des_host` executable at the top.

## 5. Measured

- The host: `des_host` 11.4 MB. It needs libc, libm, the loader, and SDL2 and vkd3d from its own folder. Newest
  symbol `GLIBC_2.35`.
- The Linux tarball: about 12 MB; two builds give the same bytes.
- A full setup in WSL (Ubuntu 24.04, 2026-10-25), with the CI-built host and the pinned downloads:
  - check, stage and lift: 35 s (92,993 PPU functions, 14 SPU images);
  - fetch and compile: about 3.5 min with the real clang 22.1.8; `des_game.so` 252,602,280 bytes;
  - Play from the install folder, started with no arguments: the Nexus, with the HUD, 19 s after the start. vkd3d
    ran on Mesa's Dozen (Vulkan on D3D12) in WSL, so this says nothing about frame rates on a real Linux PC.

## 6. Trying it in WSL

1. Take a Linux tarball (a release's, or `package.py build --linux-host <folder>` around a `des_host-linux-<sha>`
   artifact) and unpack it in WSL's own file system, not under `/mnt/c`: Windows' drives do not keep the execute
   bit.
2. Until the `llvm-linux-22.1.8-2` release is published, setup cannot download the compiler (a draft needs a
   sign-in). Put the file at `<data folder>/downloads/llvm-linux-x64-22.1.8-2.tar.xz`. `fetch.py` checks its size
   and SHA-256 and uses it.
3. Start `./des_host`. WSLg shows the window. Point setup at the game files as on Windows.
4. Test runs stay muted on Linux (docs/76).

## 7. Not done yet

- **Publish `llvm-linux-22.1.8-2`** (a draft now). Until then a Linux setup cannot fetch its compiler.
- **Movies** (FFmpeg): the Linux host has none. Open: the system's FFmpeg opened at run time (the loader exists,
  docs/76) or a pinned build beside the host.
- **Shader precompile** on Linux (docs/93, docs/97): not checked yet.
- **A real Linux PC**: a Steam Deck, Ubuntu 24.04 and one rolling distro, each on its own GPU driver; and a
  frame-rate check there (WSL sits at 60 Hz, docs/76).
- **Updates on Linux** (docs/105): the update check names the tarball, and the restart starts the program's own
  path. Once both are on master, the tarball should carry `release-files.txt`, as the zip does.
- **vkd3d's source**: the tarball ships vkd3d's licence (LGPL 2.1 or later). The LGPL also asks for the library's
  source or a written offer: attaching vkd3d 2.1's source to each release would cover it.
- **No `curl`**: a PC without it cannot fetch the first Python. A `wget` fallback would cover minimal installs.

## Sources

- docs/76 (native Linux), docs/81 (setup), docs/86 (the compiler download), docs/87 (the design), docs/105
  (updates).
- ps3recomp #122 (the game module on ELF), and this port's `host.yml`, `llvm-archive.yml`, `ci.yml`, `release.yml`.
