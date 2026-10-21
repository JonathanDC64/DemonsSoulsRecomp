# 76. Native Linux: the D3D12 backend on vkd3d (2026-10-21)

Demon's Souls runs as a native Linux program, built from the same code as
the Windows exe. It boots, loads the save's level and renders it as on
Windows. This page says how, what is missing, and how to build and run it.

It replaces the plan in docs/75. docs/75 expected a new Vulkan device layer
for the renderer, 30-45 days of work and 65-110 for all of native Linux.
That layer is not needed: the D3D12 backend runs unchanged on vkd3d.

Tested in WSL (Ubuntu 24.04) only: on lavapipe (Mesa's CPU renderer) and
on an RTX 5070 Ti through Mesa's dzn driver (Vulkan on WSL's D3D12). No real
Linux PC, native Linux GPU driver or Steam Deck has run it yet.

## What runs

| Part | State |
|---|---|
| Lifted PPU and SPU code, libsre, SPURS, threads, files, saves | as on Windows: boots to the level in about 13 s headless |
| Rendering | `rsx_d3d12_backend.c` on vkd3d 2.1, in an SDL window: the opening and the level match the Windows frames |
| Shaders | all 3,652 DXBC files in a Windows shader cache, and all 3,605 HLSL programs dumped from a run, go through vkd3d-shader; the Windows cache can be copied over as is |
| Audio | SDL2 (as before off Windows). `AUDIO_MUTE=1` now mutes it too |
| Movies | the PAMF movies (MPEG-2 video, LPCM sound) decode through FFmpeg's libavcodec, loaded at run time: the opening advert plays all 3,896 frames, as on Windows |
| Wrapper UI | start screen, menu (F1, Esc, or L3+R3), settings, controls, trophies, problem reports: RmlUi through its own D3D12 renderer |
| Keyboard and mouse | the UI's layouts (the Souls layout by default) and bindings, mouse buttons and wheel, mouse look |
| Keyboard pad | without the UI (`DES_UI=0`): SDL's keyboard state, with the same `PAD_KEYS` bindings as Windows |
| Controllers | SDL2 game controllers (the existing SDL branch of `cellPad.c`), for the game and the menus |
| Window | SDL: resizable, Alt+Enter borderless, F9 / F10 / F11 hot keys; Escape closes only without the UI |

Measured at 1280x720, frame cap off, the tutorial's first room (~790
draws):

| Vulkan driver | fps |
|---|---|
| lavapipe (CPU) | about 4 |
| dzn on an RTX 5070 Ti, SDL on X11 | 55-58 |
| dzn on an RTX 5070 Ti, SDL on Wayland (`SDL_VIDEODRIVER=wayland`) | 59-61 |

Both dzn rows sit at WSLg's 60 Hz display, so they say little about the GPU
path's own cost. Frames dumped on dzn match the Windows frames. A native
Linux GPU driver (Mesa RADV / ANV / NVK, NVIDIA) has not been tried.

## How it fits together

- **vkd3d** is Wine's D3D12 on Vulkan, as a library (`libvkd3d`,
  `libvkd3d-utils`, `libvkd3d-shader`; LGPL-2.1, linked as shared
  libraries). It provides the D3D12 interfaces and `D3DCompile`. It has no
  DXGI.
- **`libs/video/rsx_vkd3d_host.h`** (ps3recomp) is what the backend takes
  from Windows: vkd3d's headers, `runtime/platform/win32_compat.h` for the
  Win32 types and calls (threads, locks, events, timers), and the part of
  DXGI the backend uses (factory, swap chain, adapter; 17 methods).
- **Spellings.** The backend uses forms clang takes on every host, not
  MSVC's: `__builtin_bswap32`, `__builtin_clz`, `_Alignas`,
  `__attribute__((always_inline))`, `swprintf`. It joins paths with `/`
  (Windows takes it too), and finds a folder at the last `/` or `\`. The
  runtime is built with clang (clang-cl on Windows); cl.exe is not
  supported.
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

On Windows the backend does what it did: each change is behind `_WIN32`,
or a portable form of the same thing. The one visible difference is the `/`
after the folder in the shader cache's paths.

### The wrapper UI

The UI draws through `port/ui/src/render_d3d12.cpp`: RmlUi's D3D11 renderer
rewritten for D3D12, on the game's device and queue, ahead of Present.
vkd3d has no D3D11On12, which is what the UI drew with on Windows before;
Windows now uses the D3D12 renderer too, and keeps the D3D11On12 path
(`render_d3d11.cpp`) for one release behind `DES_UI_RENDER=d3d11` (docs/32,
"The menu renderer").

- **Drawing.** The UI goes into a layer, a 2x multisampled target; all
  layers share one stencil buffer, which holds the clip mask. Filters
  (opacity, blur, drop shadow, the colour matrices, mask images) work in
  single-sampled targets of the same size. `end_frame` composites the base
  layer over the back buffer in premultiplied alpha. The shaders are
  RmlUi's, compiled by vkd3d's `D3DCompile`. The gradient's stops are
  unrolled, so it indexes nothing at run time. Up to three frames are in
  flight, each with its own allocator and its own ring for constants and
  quads. Geometry and texture uploads use blocks of powers of two from 4 MB
  upload pages. What RmlUi releases is freed once the GPU is past the frames
  that used it.
- **Host.** `port/ui/src/host.cpp` is the one host for both platforms;
  `renderer.hpp` is what each renderer implements. Images (the trophy icons)
  are decoded with stb_image. Off Windows the display scale comes from SDL's
  DPI, the cursor's shape is set on the window's thread, and the clipboard is
  RmlUi's own, inside the game.
- **Input.** The runtime's SDL window thread hands the overlay the messages
  Windows would, made from SDL's events (`rsx_d3d12_backend.h`):
  - keys as virtual-key codes, with the repeat and Alt bits;
  - `WM_CHAR` from text input;
  - the mouse's buttons, moves and wheel;
  - focus, size and close.
  Raw mouse motion arrives as `WM_RSX_MOUSE_MOTION`. `port/ui/src/mouse_sdl.cpp`
  holds the cursor with SDL's relative mouse mode for mouse look. The game's
  keyboard and mouse state comes from `cellPad_host_key_down`, which reads
  SDL's state. `platform.hpp` is that input format off Windows: the Win32
  types, virtual-key codes and window messages. `keys.cpp` maps virtual
  keys to RmlUi's.
- **One code path.** Where both platforms do the same thing, the UI runs
  the same code on both: `portable.hpp` (a steady clock, a name compare
  that ignores case, the local time), `std::thread` for the watch thread,
  `report::open_file` for files. Where a fact differs, one small function
  has a version per platform, and its callers have no `#ifdef`:
  - `keys::key_down`, `keys::key_name`: the key state and the keys' names;
  - `host::window_dpi` and the system interface, at the top of `host.cpp`;
  - `mouse::client_size`, `mouse::hold_buttons`;
  - `read_xinput` (empty off Windows), `input::pad_support`;
  - in `report.cpp`: the process id, the command line, the environment,
    file truncate and seek, the user's and the PC's names, the home folder,
    and the system facts.
  The crash dialog, the minidumps, `hdr_on` and `other_modules` stay
  Windows-only.
- **Reports.**
  - The data folder is `$XDG_DATA_HOME/DemonsSoulsRecomp/DemonsSouls` (else
    `~/.local/share/...`).
  - The log is teed through a pipe.
  - Screenshots are PNGs written by stb_image_write.
  - Links and folders open with `xdg-open`.
  - Crashes (a fault nothing handled, or an abort) go through win32_compat's
    exception shim and write `crashes/crash-<time>.txt` with a backtrace,
    plus the end of the log. There is no dialog.
  - Hangs: when the game has stopped and the render thread does not answer
    the menu key, the watch thread writes `crashes/hang-<time>.txt` (the
    runtime's snapshot) and `.log`. There is no dialog.

stb is fetched at configure time, as RmlUi and FreeType are.

## Building

Linux packages: clang (or GCC), CMake, Ninja, SDL2 (`libsdl2-dev`), the
Vulkan headers and loader (`libvulkan-dev`), a Vulkan driver, and FFmpeg's
headers for the movies (`libavcodec-dev`).

vkd3d: 2.1 from the release tarball. Ubuntu 24.04 packages vkd3d 1.2, which
has no HLSL compiler. Building it needs flex, bison, Perl's JSON module
(`libjson-perl`) and the SPIR-V headers (`spirv-headers`). Install it to a
prefix and point pkg-config at it:

    ./configure --prefix=$HOME/vkd3d && make -j && make install
    export PKG_CONFIG_PATH=$HOME/vkd3d/lib/pkgconfig

The runtime, then the port:

    cmake -S ps3recomp -B rt -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_RSX_D3D12_VKD3D=ON \
      -DPS3RECOMP_FFMPEG_RUNTIME=ON
    ninja -C rt ps3recomp_runtime
    cmake -S port -B build -G Ninja -DPS3RECOMP_DIR=<ps3recomp> -DPS3RECOMP_BUILD_DIR=rt -DRECOMP_DIR=<lift>
    ninja -C build

`DES_VKD3D=OFF` builds the headless null backend instead. The wrapper UI is
built with `DES_VKD3D`. `DES_UI=OFF` leaves it out (`port/des_ui_stub.cpp`),
and `DES_UI=0` at run time turns it off. The 14 lifted chunks take about 2 GB
of memory each to compile at `-O2`.

## Running

The same environment as on Windows (`tools/des/deslib.sh`), plus:

- `PS3_VFS_ROOT`: the disc folder (`PS3_GAME` inside).
- `PS3_SAVEDATA_ROOT`, `PS3_HDD1_ROOT`, `PS3_SHADER_CACHE`: where saves, the
  game's HDD cache and the shader cache go. Set all three to keep a test run
  away from anything else.
- The working folder needs `work/libsre/libsre_image.bin` (libsre is loaded
  from there).
- Movies load FFmpeg's libavcodec when one starts: the major version the
  build's headers named (`libavcodec.so.60` on Ubuntu 24.04, FFmpeg 6.1).
  Without it a movie ends as soon as it starts, as before, and the log says
  `[sail_movie] no MPEG-2 video decoder`. `PS3RECOMP_FFMPEG=0` turns it off.
- Under WSL, sound plays on the PC's speakers: test runs set
  `SDL_AUDIODRIVER=dummy` or `AUDIO_MUTE=1`.
- Under WSL, controllers: WSL sees no Bluetooth devices. A USB controller
  can be passed through with usbipd-win (installed on Windows); this has not
  been tried.

### On the GPU in WSL (dzn)

WSL passes the Windows GPU through as D3D12 (`/dev/dxg`,
`/usr/lib/wsl/lib/libd3d12.so`). NVIDIA and AMD ship no Linux Vulkan driver
for WSL; Mesa's dzn ("Dozen") is Vulkan 1.2 on that D3D12. Ubuntu's Mesa
does not build it, so build it alone into a prefix (a minute or so), and pick
it per run:

    # packages: python3-mako, directx-headers-dev, wayland-protocols, and the
    # libxcb-*-dev / libx11-xcb-dev / libxshmfence-dev headers for its X11 WSI.
    # Mesa 25.2 needs meson >= 1.4 (Ubuntu 24.04 has 1.3.2): meson.py runs from
    # its release tarball without installing.
    meson setup build mesa-25.2.8 --prefix=$HOME/mesa-dzn --buildtype=release       -Dvulkan-drivers=microsoft-experimental -Dgallium-drivers= -Dplatforms=x11,wayland       -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false       -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled
    ninja -C build install
    export VK_DRIVER_FILES=$HOME/mesa-dzn/share/vulkan/icd.d/dzn_icd.x86_64.json

`vulkaninfo --summary` then lists each Windows GPU as "Microsoft Direct3D12
(...)". vkd3d takes the first discrete one; `VKD3D_VULKAN_DEVICE=<index>`
picks another. dzn is not a conformant Vulkan driver: good for checking the
game at speed, not for judging Linux performance.

## Missing

| Item | Note |
|---|---|
| Startup shader precompile | `rsx_shader_precompile.c` is Windows-only; shaders compile as the game draws them |
| Pipeline cache across runs | vkd3d's `CreatePipelineLibrary` is a stub; the backend carries on without one, and pipelines are built again each run |
| Crash and hang dialog, F8 mark | Win32 code; not ported. Crash and hang reports are written, with no dialog |
| Runtime snapshot | the runtime's `ps3_diag_report` (what every thread is doing) and its debug console (`PS3_DEBUG`) are Windows-only; reports and hang reports say the snapshot is not available |
| IME | RmlUi's text fields take SDL's text input; no IME window |
| Frame statistics | the swap chain reports none (`present_queued` and `FRAME_LOG` take "cannot say") |
| A native GPU run | WSL with dzn only so far (above); no real Linux driver yet |
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

ps3recomp (feat/linux), for the wrapper UI:

- `video: the SDL window hands the overlay Windows' messages`
- `input: cellPad_host_key_down, and SDL controllers for the UI`
- `video: portable spellings in the D3D12 backend`

ps3recomp (feat/linux-movies):

- `codec: movies off Windows through FFmpeg`
- `tests: sail_movie end to end, and the codec tests in Linux CI`

Port (feat/linux):

- `build: des_boot on Linux`
- `ui: the wrapper UI off Windows, on its own D3D12 renderer`
- `ui: portable calls instead of Windows-only ones`
