# 76. Native Linux: the D3D12 backend on vkd3d (2026-10-21)

Demon's Souls runs as a native Linux program, built from the same code as
the Windows exe. It boots, loads the save's level and renders it as on
Windows. This page says how, what is missing, and how to build and run it.

It replaces the plan in docs/75. docs/75 expected a new Vulkan device layer
for the renderer, 30-45 days of work and 65-110 for all of native Linux.
That layer is not needed: the D3D12 backend runs unchanged on vkd3d.

Tested in WSL (Ubuntu 24.04) only, where Vulkan is lavapipe, Mesa's CPU
renderer. No GPU, no real Linux PC and no Steam Deck has run it yet.

## What runs

| Part | State |
|---|---|
| Lifted PPU and SPU code, libsre, SPURS, threads, files, saves | as on Windows: boots to the level in about 13 s headless |
| Rendering | `rsx_d3d12_backend.c` on vkd3d 2.1, in an SDL window: the opening and the level match the Windows frames |
| Shaders | all 3,652 DXBC files in a Windows shader cache, and all 3,605 HLSL programs dumped from a run, go through vkd3d-shader; the Windows cache can be copied over as is |
| Audio | SDL2 (as before off Windows). `AUDIO_MUTE=1` now mutes it too |
| Keyboard pad | SDL's keyboard state, with the same `PAD_KEYS` bindings as Windows |
| Controllers | SDL2 game controllers (the existing SDL branch of `cellPad.c`) |
| Window | SDL: resizable, Alt+Enter borderless, F9 / F10 / F11 hot keys, Escape closes |

Measured under lavapipe: about 4 fps at 1280x720 with ~590 draws. That is
the CPU drawing every pixel. A GPU driver (Mesa RADV / ANV, NVIDIA) has not
been tried.

## How it fits together

- **vkd3d** is Wine's D3D12 on Vulkan, as a library (`libvkd3d`,
  `libvkd3d-utils`, `libvkd3d-shader`; LGPL-2.1, linked as shared
  libraries). It provides the D3D12 interfaces and `D3DCompile`. It has no
  DXGI.
- **`libs/video/rsx_vkd3d_host.h`** (ps3recomp) is what the backend takes
  from Windows: vkd3d's headers, `runtime/platform/win32_compat.h` for the
  Win32 types and calls, a few MSVC spellings, and the part of DXGI the
  backend uses (factory, swap chain, adapter; 17 methods).
- **`libs/video/rsx_vkd3d_dxgi.c`** implements that DXGI. The swap chain's
  buffers are its own Vulkan images wrapped as vkd3d resources. The backend
  renders to them and moves them to `PRESENT`, which vkd3d is told means
  "ready to copy" (`VKD3D_RESOURCE_PRESENT_STATE_TRANSITION`). `Present`
  blits the current buffer into a Vulkan swap chain image on vkd3d's queue
  and presents it. `RSX_VSYNC=0` presents at once (immediate mode, else
  mailbox).
- **The device** is made there too, not by vkd3d-utils: vkd3d-utils' device
  signals its own event objects, and the backend waits on win32_compat
  events. It also needs the window's surface extensions and
  `VK_KHR_swapchain`. `VKD3D_VULKAN_DEVICE=<index>` picks the GPU.
- **The window** is SDL's, made on its own thread, like the Win32 window.
  That thread waits on SDL's events; other threads post it the title,
  borderless on/off, and the close at shutdown. The display's refresh rate
  comes from SDL's display mode, in place of DWM.
- **Calling conventions:** vkd3d declares its functions `ms_abi` on x86-64
  (`WINAPI`, `STDMETHODCALLTYPE`). The host header puts `WINAPI` back to
  nothing after vkd3d's headers, because the backend's thread procs are
  handed to win32_compat's (plain C) `CreateThread`.
- **Guest memory** off Windows is one 4 GB-aligned `mmap` with
  `MAP_NORESERVE` (the kernel backs a page on first touch). It was a
  `calloc`: not page-aligned, so every `sys_memory_allocate` failed and the
  game panicked in `DLStdAllocator` before its first thread.

Windows builds the same code as before: every change in the backend is
behind `_WIN32`, or produces the same strings there.

## Building

Linux packages: clang (or GCC), CMake, Ninja, SDL2 (`libsdl2-dev`), the
Vulkan headers and loader (`libvulkan-dev`), and a Vulkan driver.

vkd3d: 2.1 from the release tarball. Ubuntu 24.04 packages vkd3d 1.2, which
has no HLSL compiler. Building it needs flex, bison, Perl's JSON module
(`libjson-perl`) and the SPIR-V headers (`spirv-headers`). Install it to a
prefix and point pkg-config at it:

    ./configure --prefix=$HOME/vkd3d && make -j && make install
    export PKG_CONFIG_PATH=$HOME/vkd3d/lib/pkgconfig

The runtime, then the port:

    cmake -S ps3recomp -B rt -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_RSX_D3D12_VKD3D=ON
    ninja -C rt ps3recomp_runtime
    cmake -S port -B build -G Ninja -DPS3RECOMP_DIR=<ps3recomp> -DPS3RECOMP_BUILD_DIR=rt -DRECOMP_DIR=<lift>
    ninja -C build

`DES_VKD3D=OFF` builds the headless null backend instead. The wrapper UI is
off by default (`DES_UI=OFF`, `port/des_ui_stub.cpp`). The 14 lifted chunks
take about 2 GB of memory each to compile at `-O2`.

## Running

The same environment as on Windows (`tools/des/deslib.sh`), plus:

- `PS3_VFS_ROOT`: the disc folder (`PS3_GAME` inside).
- `PS3_SAVEDATA_ROOT`, `PS3_HDD1_ROOT`, `PS3_SHADER_CACHE`: where saves, the
  game's HDD cache and the shader cache go. Set all three to keep a test run
  away from anything else.
- The working folder needs `work/libsre/libsre_image.bin` (libsre is loaded
  from there).
- Under WSL, sound plays on the PC's speakers: test runs set
  `SDL_AUDIODRIVER=dummy` or `AUDIO_MUTE=1`.

## Missing

| Item | Note |
|---|---|
| Wrapper UI (start screen, menu, settings, reports) | Its renderer is D3D11On12, which vkd3d does not have; RmlUi's Vulkan or SDL renderer and an SDL platform layer. docs/75 put this at 8-13 days |
| Movies | `sail_movie.cpp` uses Media Foundation; off Windows a movie ends at once. FFmpeg, 2-4 days |
| Startup shader precompile | `rsx_shader_precompile.c` is Windows-only; shaders compile as the game draws them |
| Pipeline cache across runs | vkd3d's `CreatePipelineLibrary` is a stub; the backend carries on without one, and pipelines are built again each run |
| Mouse, F8 mark, problem reports, crash dialog, hang watchdog | Win32 code; not ported |
| Frame statistics | the swap chain reports none (`present_queued` and `FRAME_LOG` take "cannot say") |
| A GPU run | lavapipe only so far. In WSL a GPU needs Mesa's `dzn` driver (Vulkan on D3D12), which Ubuntu's Mesa does not ship |
| vkd3d-proton | faster and more complete than Wine's vkd3d, but built for Wine/Proton; not tried as a library |
| Packaging | an AppImage carrying vkd3d's libraries (LGPL: shared, with its notice) |
| CI | a Linux job that builds the runtime with `PS3RECOMP_RSX_D3D12_VKD3D=ON` |

vkd3d prints four `fixme` notes: read-only depth views (flags ignored),
root signature flags, the pipeline library stub, and aliasing barriers.
None has shown a visible fault yet.

## Changes

ps3recomp (feat/linux):

- `runtime: guest memory off Windows is one 4 GB-aligned mapping`
- `audio: AUDIO_MUTE mutes the SDL output too`
- `platform: win32_compat's LONG64 is int64_t; MoveFileExA, DeleteFileA`
- `video: the D3D12 backend on vkd3d, off Windows`
- `input: the keyboard pad off Windows`

Port (feat/linux): `build: des_boot on Linux`.
