# Linux (AppImage) and macOS: what a port needs (survey, 2026-10-18)

Short answer: the ps3recomp runtime already builds on Linux and macOS, and its CI covers both. What Demon's Souls runs on is still Windows-only: the renderer, window, input, audio, wrapper UI and build files.

This doc lists what exists, what blocks a port, and an order to do it in. No code has changed yet.

## What already works off Windows

- **CI** (`ps3recomp/.github/workflows`): `linux.yml` (gcc and clang, null renderer), `linux-vulkan.yml` (lavapipe plus the validation layer, every `ps3recomp_host` scene) and `macos.yml` (arm64). They build the runtime library and the host test scenes, not a game.
- **POSIX shim:** `runtime/platform/win32_compat.h` and `.c` (about 3,300 lines). Maps SRW locks, condition variables, events, threads, QueryPerformanceCounter, WaitOnAddress and VirtualAlloc to pthreads, clock_gettime and mmap. Most of the runtime's Win32 calls go through it.
- **Guest memory:** `vm.h` has a full mmap/mprotect path, including 16K pages (Apple Silicon).
- **Guest pointer faults:** `ppu_loader.cpp` has a sigaction SIGSEGV/SIGBUS path next to the Windows vectored handler.
- **Other renderers:**
  - `rsx_vulkan_backend.c`: Linux, SDL2 window, HLSL to SPIR-V through glslang.
  - `rsx_metal_backend.m`: HLSL to SPIR-V to MSL through spirv-cross.
  - Both drive the shared `rsx_draw_engine.c`. Other ps3recomp ports report Twisted Metal running on Linux/Vulkan and inFamous on macOS.
- **Input and audio:** SDL2 paths in `cellPad.c` and `cellAudio.c`.
- **Movies:** an optional FFmpeg path (`PS3RECOMP_FFMPEG_RUNTIME`) besides Media Foundation.

## Blockers, biggest first

1. **Renderer.** DeS has only ever run on `rsx_d3d12_backend.c` (about 12,000 lines, FXC shader model 5). The Vulkan and Metal backends use a different draw engine (`rsx_draw_engine.c`) that waits on every submit and has never run DeS. All the DeS fixes (sampler tables, index ranges, present at flip, particles, the vertex-shader prework) live in the D3D12 backend. There are two ways forward:
   - Bring the shared draw engine up to what DeS needs. Most of the work goes here.
   - Port the D3D12 backend's logic to Vulkan with the same structure. This keeps the DeS fixes, but it is a large port.
   - A third, cheaper route for Linux only: run the Windows build under Wine/Proton with vkd3d-proton. It needs no code changes and is worth trying first; it says nothing about macOS.
2. **Wrapper UI** (`port/ui`, about 6,200 lines). It draws through D3D11On12 on the game's D3D12 device (`host.cpp`), takes input from Win32 messages and XInput (`des_ui.cpp`), and its crash dialog is a Win32 window. It needs RmlUi's GL3 or Vulkan renderer and its SDL platform backend. Crash reports need replacing too: dbghelp minidumps, WIC screenshots and `SHGetKnownFolderPath`.
3. **Window and input.** The D3D12 backend owns a Win32 window and message loop. The keyboard-as-pad uses `GetAsyncKeyState` and exists only on Windows. Moving the window to SDL2, as the Vulkan backend already does, would serve every platform.
4. **Boot scaffold** (`runtime/ppu/tests/boot_main.cpp`).
   - Off Windows, guest memory is `calloc(1, 0xE0000000)` (line 2040). It needs a reserve plus commit-on-fault, as on Windows.
   - Renderer selection picks D3D12, Metal on Apple, or null; Vulkan is not wired in.
   - The watchdog, profiler and debug console exist only on Windows.
5. **`port/CMakeLists.txt`.** All of it is clang-cl: `/bigobj /O2 /clang:-fwrapv /clang:-msse4.1`, `ps3recomp_runtime.lib`, d3d12/dxgi/xinput links, `/MAP`, an RC icon. It needs a gcc/clang branch: `-fwrapv`, `-msse4.1` only on x86-64, the `.a` library, and SDL2/dl links.
6. **arm64 (Apple Silicon).** SSE in `spu_helpers.h` is behind `__SSE4_1__`, but `<intrin.h>` is included in `spu_context.h`, `spu_helpers.h` and `spu_coherency.c` and needs checking. Lifted code without SSE4.1 will be slower; NEON paths for the hot SPU helpers would win it back.
7. **Port files.** `des_prework.cpp` uses FindFirstFileA and GetModuleHandleA. `sail_movie.cpp` needs the FFmpeg path for the intro movie and cutscenes.

## Suggested order

1. Try the Windows build under Proton/Wine with vkd3d-proton on Linux. If it runs, ship that for Linux for now and note it in the README.
2. Make `port/CMakeLists.txt` build on Linux with the null renderer, and get the game to boot headless (logs only). This flushes out the compile, link, boot scaffold and memory issues on their own.
3. Pick the renderer route and bring up the title screen on Vulkan.
4. Move the window and input to SDL2; move the UI to RmlUi GL3 or Vulkan with SDL.
5. Package an AppImage: bundle SDL2 and the FFmpeg libraries; the game files stay outside, as on Windows.
6. macOS: Metal through the same draw engine, a `.app` bundle, arm64 tuning.

## Numbers used above

- `#if _WIN32` 456 times in 80 files.
- `_MSC_VER` 34 times in 18 files, mostly around `__declspec`.
- `windows.h` in 61 files.
- clang-cl is required on Windows because the code uses `__int128`, `__atomic` builtins and musttail (`ps3recomp/CMakeLists.txt`, near line 345). The gcc and clang flag paths are already generic.
