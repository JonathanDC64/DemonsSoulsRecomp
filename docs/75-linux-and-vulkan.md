# 75. Linux builds, Vulkan, and a renderer choice on Windows (study, 2026-10-21)

This page checks docs/36 (Linux and macOS survey) and docs/46 part B
(platforms) against the current code. It adds two questions those pages
did not answer: how far the Vulkan backend is from what Demon's Souls needs,
and whether Windows should offer a choice of renderer. This is a code
reading only: nothing was built or run on Linux.

Each figure is marked either **read** (seen in the code at the place given)
or **inferred** (a conclusion no run has checked). Day counts are working
days, all inferred.

## Short answer

- **First Linux release: Proton.** Run the Windows build under Proton or
  Wine. About 5-10 days of testing and fixes, against roughly 65-110 days
  for a native Linux build. This needs a Linux PC or a Steam Deck to test
  on.
- **Native Linux, later:** a POSIX build plus a Vulkan renderer. Split the
  D3D12 backend into a shared core and a device layer, then write a Vulkan
  device layer (about 30-45 days). Do not build on the existing Vulkan
  backend: it runs on a different draw engine that has none of the Demon's
  Souls fixes.
- **A renderer choice on Windows:** not now. Keep D3D12 as the only Windows
  renderer until the Vulkan layer passes the regression runs on Linux. Then
  "Renderer: Direct3D 12 / Vulkan" costs about 3-5 days. It would not raise
  the frame rate: the frame is limited by CPU threads (HighGraphics, the
  FIFO, SPU image 2), not the GPU.

## Windows-only code, by area

| Area | State | Native Linux work |
|---|---|---|
| Threads, sync, timers, VirtualAlloc, WaitOnAddress | shimmed (`runtime/platform/win32_compat.h/.c`); Linux CI runs the tests | none |
| Guest memory (4 GB) | Windows reserves it and commits on touch from a VEH. Off Windows it is a `calloc` of 3.5 GB (`boot_main.cpp`) | one 4 GB + guard `mmap` with `MAP_NORESERVE`: about 20 lines |
| Guest-pointer trap | a `sigaction` SIGSEGV/SIGBUS handler already sits next to the VEH (`ppu_loader.cpp`) | none |
| Diagnostics (console, thread snapshot, profiler, hang watchdog, `ps3_diag_report`) | Windows only, about 1,300 lines; stubs elsewhere | not needed to run; 3-5 days later |
| Process setup (timer resolution, priority, crash filter, stack-overflow handler) | Windows only | `sigaltstack` and a crash signal handler: about 1 day |
| File system | dirent native on POSIX | Linux paths are case-sensitive, and NTFS hides a case mismatch today. Log failed opens on the first boot. |
| Sockets, RNG | Winsock and BSD branches; loopback tests on Linux CI; `/dev/urandom` | none |
| Lifted code | every `__declspec` has a GCC branch; `<intrin.h>` sits behind `_MSC_VER`; the boot scaffold compiles with 0 errors on gcc and clang | the Demon's Souls lift itself has never been compiled on Linux |
| Renderer | `rsx_d3d12_backend.c`: 13,445 lines, all D3D12/DXGI/FXC | see below |
| Audio | WASAPI on Windows, SDL2 elsewhere (`cellAudio.c`); SDL2 only ran in CI with the dummy driver | 1-2 days of testing |
| Movies | Media Foundation's MPEG-2 decoder (`sail_movie.cpp`); off Windows every movie ends at once | FFmpeg in sail_movie: 2-4 days |
| Input | XInput, keyboard pad (VK codes), raw-input mouse, the DualSense/DS4 HID reader: all inside the Windows branch of `cellPad.c`; the SDL2 branch is 115 lines of controller polling | SDL scancodes, relative mouse, SDL's Sony pad support: 4-6 days |
| Menu and UI | RmlUi's DX11 renderer on D3D11On12, Win32 platform layer; report, crash dialog, mouse and des_ui use Win32 APIs (3,580 lines) | RmlUi ships Vulkan and SDL backends: 8-13 days |
| Port's own `des_*.cpp` | three Win32 call sites (`des_prework.cpp`, `des_options.cpp` F8, `des_aspect.cpp`) | about 1 day |
| `port/CMakeLists.txt` | MSVC-style flags, `.lib` import, D3D/XInput/Winsock links, RC files | a GCC/Clang branch: 1-2 days |

In total, about 5,000 lines of non-renderer platform code to replace, plus
a renderer device layer.

**Toolchain.** Everything already builds with clang-cl, and the runtime
rejects real cl.exe. The lifted code needs GCC/Clang features
(`__builtin_bswap`, `__int128`, musttail, `-fwrapv`, `-msse4.1`), so
Clang or GCC on Linux is a direct fit. Lifted functions take a context
pointer and call the runtime through `extern "C"`, so the SysV and Win64
calling conventions never mix. LP64 against LLP64 is a low risk: guest
state uses fixed-width types, and the shim refuses Interlocked on a plain
`long`. The boot risk is static-init order (`.ctors` against `.CRT$XCU`),
because SPU workloads register through constructors.

## The Vulkan backend as it is

**What exists:**
- `libs/video/rsx_vulkan_backend.c`: 3,433 lines, Linux only. Its CMake
  option is off by default and is an error on Windows.
- `boot_main.cpp` offers only D3D12, Metal or null. Only the
  `ps3recomp_host` test harness picks Vulkan.

**How it works:**
- It is synchronous: "every clear, draw and present is one one-shot command
  buffer, submitted and waited on".
- It presents with FIFO only, and keeps images in the GENERAL layout.
- It drives the shared `rsx_draw_engine.c`. That engine was ported from
  `rsx_live_draw.c`, not from `rsx_d3d12_backend.c`, the backend Demon's
  Souls runs on.
- Its shaders come from the same HLSL generators as D3D12, compiled to
  SPIR-V with glslang's HLSL front end.

**Activity:**
- CI runs 13 synthetic scenes on lavapipe, and a Twisted Metal port runs
  its menus on it.
- Since 10-13 the D3D12 backend got 109 commits; Vulkan and the engine got
  3.

**What Demon's Souls has on D3D12 that the Vulkan path lacks:**

| D3D12 | Vulkan / shared engine |
|---|---|
| per-frame draw records (16,384) on a render thread, presented at FIFO flips | synchronous, per draw |
| strips and fans across index ranges, depth-only passes, shadow sampling, depth read as colour, self-sampling copy, ZPASS occlusion counts | partly; no occlusion queries at all |
| RSX_SCALE, with targets written back at guest size | missing |
| FXAA (RSX_AA), output filter, aspect, anisotropic filtering | missing |
| shader disk cache, precompile, fetch-form vertex shaders, pipeline library | missing (no VkPipelineCache) |
| VS memo, sampler-table hash, texture checksums and vertex fetch on worker threads | missing |
| window on its own thread: resizable, borderless with Alt+Enter, DPI-aware, tearing with RSX_VSYNC=0 | fixed-size SDL window, FIFO only |
| the overlay API the menu draws through (set_overlay, repaint, wake, tick, device/queue/back buffers) | missing |
| F9/F10 captures, fingerprints, `rsx_replay` | pass dump only |

**Two routes to Vulkan parity:**
- **(a) Split, then a second device layer.** Split `rsx_d3d12_backend.c`
  into an API-neutral core and a D3D12 device layer, then write a Vulkan
  device layer. The core keeps the draw records, vertex fetch, texture
  cache, sampler tables, scaling and knobs. Every Demon's Souls fix stays in
  one place. **30-45 days. Recommended.**
- **(b) Bring the shared engine up to Demon's Souls.** That means finding
  every fix again, in code that has never run the game. 40-70 days, and
  riskier.

For (a), keep the HLSL generators and compile with glslang first, since it
is already built and tested. If glslang rejects some of the game's programs
(fetch-form vertex shaders, fragment programs over 32 KiB), fall back to
DXC's SPIR-V output. Cache the SPIR-V on disk and keep a VkPipelineCache.

## Linux players through Proton

**Evidence it can work:**
- A Killzone 3 ps3recomp port runs its Windows D3D12 build under Proton.
- docs/46 B1 matched every import of `des_boot.exe` to a Wine counterpart.

**What could break:**
- **Run-time shader compiles (highest risk).** `D3DCompile`, also used by
  the startup shader work, would go through Wine's own d3dcompiler_47. Fix:
  ship Microsoft's redistributable d3dcompiler_47.dll, or set
  `WINEDLLOVERRIDES=d3dcompiler_47=n,b`.
- **The menu.** D3D11On12 needs DXVK 2.2 or newer and vkd3d-proton. If it
  fails, the game runs with no menu. Fix: move the menu to RmlUi's DX12
  renderer (2-4 days), which also drops D3D11On12 on Windows.
- **Movies.** Whether Media Foundation's MPEG-2 decoder works through
  winegstreamer is not known. The fallback is FFmpeg in sail_movie, which
  native Linux needs anyway.
- **Depth.** The backend uses D24S8 and reads it back. AMD's Vulkan drivers,
  including the Steam Deck's, have no D24S8, so vkd3d-proton substitutes
  another format. Check depth write-back and shadows on a Deck.
- **Pads.** With Steam Input on, a pad may show up twice (`PAD_NO_HID=1`
  already handles that). On the Deck, Steam Input should present the
  built-in controls as an XInput pad.
- **Harmless:** the lock-screen detection and SetThreadExecutionState are
  stubs under Wine. Commit-on-touch works, at one signal per 64 KB page.

**The real gate is the setup's build step.**
- Today a player needs the developer tools (Visual Studio, LLVM, CMake,
  Ninja, Python). That is not practical under Proton.
- Route B in docs/52 bundles clang, lld-link and an embeddable Python. Under
  Wine those are ordinary Win32 programs. Things to check:
  - the job object's kill-on-close;
  - the Python worker processes;
  - memory: one /O2 chunk peaks near 3 GB, and the Deck has 16 GB shared
    with its GPU, so the job count must follow RAM.
- A full compile is about 41 CPU-minutes on the dev PC: maybe 20-40 minutes
  on a Deck.

## A renderer choice on Windows

**For:**
- a fallback when a D3D12 driver misbehaves (no reports of that yet);
- one renderer shared with Linux, and with macOS through MoltenVK;
- the Vulkan path would be tested on the main development PC;
- RPCS3 itself ships only Vulkan and OpenGL.

**Against:**
- No frame rate gain: Palace is CPU-limited (docs/72), and a 720p guest
  frame is light for the GPU.
- Each renderer fix lands twice unless route (a) shares the core.
- Twice the testing: regression baselines, F9 captures, capdiff and
  `rsx_replay` (D3D12 only today) for each backend and each GPU vendor.
- The menu would need RmlUi's Vulkan renderer and a Vulkan overlay API.

**If it is added:**
- Settings > Display: "Renderer: Direct3D 12 (recommended) / Vulkan",
  applied after a restart. Help text: "Try Vulkan if the game shows
  graphics problems."
- A knob: `RSX_BACKEND=d3d12|vulkan`.
- If Vulkan fails to start, fall back to D3D12 and say so.
- Reports record which renderer ran.
- With route (a) done, the Windows side is small: load vulkan-1.dll,
  create the surface from the HWND, lift the CMake gate.

## Plan

**Phase 0: Proton (5-10 days; needs a Linux PC or Steam Deck)**
1. Run the Windows build as a non-Steam game. Check boot, the menu, audio,
   pads (Steam Input and DualSense over HID), the intro movie, shader
   compiles, saves, frame rate, vsync and tearing, borderless.
2. Ship d3dcompiler_47.dll and confirm Wine loads it.
3. Fix what the run finds: the menu on RmlUi's DX12 renderer if D3D11On12
   fails; FFmpeg in sail_movie if movies fail.
4. Once Route B (docs/52) exists, check its build step under Wine.
5. A README note for Linux and Steam Deck players.

**Phase 1: POSIX build, headless boot (6-12 days; can run alongside
Phase 0)**
1. A GCC/Clang branch in `port/CMakeLists.txt`.
2. In boot_main: the `mmap` guest space, Vulkan in the renderer selection, a
   `sigaction` crash handler.
3. The three Win32 call sites in `des_*.cpp`.
4. Boot to the title screen on the null backend. Watch static-init order,
   path case and thread stacks.

**Phase 2: Vulkan parity through route (a) (30-45 days)**
1. Split the D3D12 core from its device layer (8-12 days).
2. The Vulkan device layer: descriptors for 1,024 texture slots and 128
   sampler tables, pipelines and cache, barriers, swap chain modes,
   readbacks, occlusion queries (12-20 days).
3. Shaders through glslang, with SPIR-V and pipeline disk caches (3-5
   days).
4. Parity testing per backend with the regression runs and capture diffs
   (5-10 days).

**Phase 3: native window, menu, input, audio, movies (16-27 days)**
- An SDL2 window: borderless, resizable, DPI-aware.
- RmlUi's Vulkan renderer and SDL platform layer, and a Vulkan overlay
  API.
- Crash reports through a signal handler and a text backtrace.
- SDL keyboard, relative mouse and Sony pads.
- SDL audio tuning, and FFmpeg movies.
- Settings, shader cache and logs in XDG folders. The cache sits beside the
  exe today, and an AppImage is read-only.

**Phase 4: packaging (10-18 days)**
- An AppImage with SDL2 and the FFmpeg libraries. FFCCEOT-Recomp ships a
  Linux AppImage of about 210 MB.
- A Linux version of the setup.
- Steam Deck: a controller layout and the 1280x800 screen.
- A Linux CI job.

**Phase 5 (optional): Vulkan as a Windows option (3-5 days, plus testing on
both renderers from then on)**

**Top risks:**
- Wine's HLSL compiler;
- MPEG-2 movies under Proton;
- the depth-format substitution on AMD;
- memory on a Deck during the build step;
- static-init order and path case on the first native boot.

arm64 is not covered here (docs/46 B4).
