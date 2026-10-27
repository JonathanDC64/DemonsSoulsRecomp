# 113. Vulkan beside D3D12 (study, 2026-10-27)

The question: what would it take to render natively on Vulkan, with the same Vulkan code on Windows, Linux and later
other systems? D3D12 stays as well, so the port would have two renderers. The user doesn't know yet how vkd3d
performs, so measuring it is part of the plan (section 5).

This updates docs/75 (10-21), which studied the same route before native Linux ran on vkd3d (docs/76). Figures marked
**read** were seen in the code; **inferred** ones are estimates no run has checked. Days are working days, all
inferred.

## Short answer

- **The route is still docs/75's route (a).** Split `rsx_d3d12_backend.c` into a core that knows the RSX and two
  device layers, D3D12 and Vulkan.
  - Every Demon's Souls fix stays in the core, once.
  - Each layer only creates GPU objects, records commands and presents.
- **Size:** about 50–80 days to reach parity, including the menu, HDR and the pipeline caches (inferred). docs/75
  said 30–45 days for the renderer alone. The backend has grown 14% since then, and that figure left out the menu
  and HDR.
- **Measured on 10-27 (section 5.1): vkd3d-proton costs about 2.5%.** On this PC it read 466 fps against native
  D3D12's 476 at the pinned save, about 45 µs a frame (0.3% at 60 fps, 0.7% at 144). So on Windows a native Vulkan
  renderer buys almost no speed. Its gains are Linux HDR, a path to macOS, and dropping a dependency. Wine's vkd3d,
  which the Linux build uses, is not measured yet.
- **Defaults once it works:**
  - Windows keeps D3D12, with Vulkan as a choice (Settings > Display > Renderer, after a restart).
  - Linux moves to native Vulkan. The vkd3d build stays for one release as a fallback.
- **What stays per platform:** only finding the Vulkan library and making a surface from the window, plus the
  Windows-only check for whether HDR is on (section 4). The window, input and audio code doesn't change.

## 1. What is there today (read)

| Part | Now |
|---|---|
| `libs/video/rsx_d3d12_backend.c` | 15,813 lines (13,919 on 10-21); 531 D3D12/DXGI call sites; 1,467 lines naming D3D12 or DXGI types; 411 functions. Demon's Souls renders only through this file. |
| Linux | The same file on Wine's vkd3d 2.1 (`PS3RECOMP_RSX_D3D12_VKD3D`), with `rsx_vkd3d_dxgi.c` (706 lines) standing in for DXGI over an SDL window. |
| `rsx_live_draw.c` (9,991 lines) | A second D3D12 path, opt-in (`RSX_LIVE_DRAW=1`), not used by Demon's Souls. |
| `rsx_vulkan_backend.c` (3,435 lines, upstream) | Offscreen, one command buffer per draw, waits each time, FIFO present through a readback. Runs on the shared `rsx_draw_engine.c`, which has none of the Demon's Souls fixes (docs/75's table still holds). Not a base to build on. |
| `rsx_shader_spirv.cpp` (140 lines, upstream) | HLSL to SPIR-V through glslang's HLSL front end, with a binding contract for Vulkan. Reusable as is. |
| Shaders | The RSX decompilers write HLSL. Windows compiles it with FXC (`D3DCompile`, 23 call sites) and keeps a `.dxbc` disk cache; Linux uses vkd3d's `D3DCompile`. |
| The menu | `port/ui/src/render_d3d12.cpp`, 2,382 lines: RmlUi with its layers and filters, the game's models, HDR. It draws on the game's device, queue and back buffers through the overlay API (`rsx_d3d12_overlay_*`). |

## 2. What goes into it

### 2.1 Split the backend (10–15 days)
- **The core** keeps:
  - RSX state, draw records and the render thread;
  - vertex fetch and the texture cache, sampler tables and the surface tracking;
  - render scale, the knobs and the pipeline index (docs/97).
- **A device-layer interface:**
  - resources, views, samplers and pipelines by key;
  - draws, copies and clears, readbacks and occlusion queries;
  - present and the overlay hooks.
- **The D3D12 layer** is today's calls moved behind that interface, with no change in behaviour.
- **Check:** a regression A/B against the current exe before anything Vulkan lands. Frames must match.

### 2.2 The Vulkan device layer (15–25 days)
- Vulkan 1.3, with dynamic rendering and synchronization2. That's on every current driver, including Mesa and the
  Steam Deck.
- **Descriptors:** 1,024 texture slots and 128 sampler tables, as on D3D12, through descriptor indexing.
- **Pipelines:** by the same keys, with a `VkPipelineCache` on disk.
- **Images:** layouts and barriers, the depth formats (D24S8 where the device has it, else D32S8), and write-backs
  to guest memory at guest size.
- **Queries and reading back:** occlusion queries for ZPASS, readbacks for captures and screenshots.
- **The library:** loaded at run time (`vulkan-1.dll`, `libvulkan.so.1`), as the upstream backend does, so a PC
  without Vulkan still starts on D3D12.

### 2.3 Shaders (4–7 days)
- Compile the same HLSL to SPIR-V with glslang (`rsx_shader_spirv.cpp`).
  - Link it into the host statically: nothing to install.
  - Fall back to DXC's SPIR-V output only if glslang rejects some of the game's programs. The risky ones are the
    fetch-form vertex shaders and the fragment programs over 32 KiB.
- The backend's own shaders compile the same way: FXAA, the output filter, the HDR pass and the blits.
- A SPIR-V disk cache beside the `.dxbc` one.
- Setup's shader pass and the first-start warm-up (docs/93) make Vulkan pipelines too.

### 2.4 Presenting (4–6 days)
- A `VkSwapchainKHR`, with the present modes mapped from `RSX_VSYNC`:
  - FIFO for on;
  - IMMEDIATE for off, with tearing;
  - MAILBOX where it fits;
  - VRR as the driver gives it.
- Even frames and frame pacing (docs/93) through `VK_KHR_present_wait` where the driver has it.
- HDR through `VK_EXT_swapchain_colorspace` (scRGB FP16, or HDR10) and `VK_EXT_hdr_metadata`. That gives Linux HDR,
  which vkd3d's swap chain can't, having no colour-space methods (docs/104).

### 2.5 The menu (6–10 days)
- `render_d3d12.cpp` splits like the backend: an RmlUi core (layers, filters, models, HDR maths) and two device
  layers.
- The overlay API gains a Vulkan form: device, queue, swap chain images and the semaphores to wait on and signal.
- RmlUi's own Vulkan renderer (`Backends/RmlUi_Renderer_VK.cpp`) has no layers, filters or shaders (its GL3 renderer
  does), so it's no shortcut.

### 2.6 Errors and reports (1–2 days)
- The graphics-lost dialog (DRED on D3D12) reads `VK_EXT_device_fault` where the driver has it, otherwise only "the
  device was lost".
- Problem reports say which renderer ran.

### 2.7 Choosing the renderer (3–5 days)
- `RSX_BACKEND=d3d12|vulkan`, and a Settings > Display row "Renderer", applied after a restart.
- If Vulkan fails to start on Windows, the game falls back to D3D12 and says so.
- CI builds Vulkan on both systems.

### 2.8 Testing (7–12 days, then each change on both)
- Regression runs and save tests on each renderer.
- The capture tools (F9/F10, capdiff, `rsx_replay`) are D3D12-only and need a Vulkan path.
- NVIDIA on this PC. AMD and Intel need testers.

**Total: 50–80 days** (inferred).

## 3. Who gains what

- **Linux:** no translation layer and its HLSL compiler; HDR; the Steam Deck on native Vulkan.
- **Windows:** a second renderer for when a D3D12 driver misbehaves (no such reports yet). Probably no frame rate
  gain: Palace is limited by CPU threads (HighGraphics, the FIFO, SPU image 2; docs/72), not by the GPU.
- **Later:** macOS through MoltenVK (section 6).
- **Cost:** each fix to a device layer lands twice, and every renderer change needs testing on both.

## 4. What stays per platform

- **Finding Vulkan:** the library's name, or SDL's loader on Linux. One small function.
- **The surface:**
  - `vkCreateWin32SurfaceKHR` from the window thread's HWND on Windows;
  - `SDL_Vulkan_CreateSurface` on Linux;
  - one function per platform, as the portable-code rule asks.
- **Whether HDR is on:** Windows asks DXGI, as `hdr_on` does today; Vulkan's colour spaces don't say.
- **Outside the renderer:** the window, input and audio stay as they are, Win32 on Windows and SDL on Linux. Moving
  Windows to SDL too is a separate choice and isn't needed for Vulkan.

## 5. How vkd3d performs: measure first

**What we know (read):**
- Linux ships on Wine's vkd3d 2.1.
- The only runs were in WSL, on Mesa's dzn (Vulkan on D3D12, capped by WSLg at 60 Hz) or on lavapipe.
- Neither run says what vkd3d costs on a real Linux driver.

**Measurements, cheapest first:**
1. **This PC, D3D12 against vkd3d-proton.**
   - Put vkd3d-proton's `d3d12.dll` and `d3d12core.dll`, and DXVK's `dxgi.dll`, beside a scratch copy of
     `des_host.exe`.
   - Run the Palace perf run on both, with the same GPU and driver.
   - This measures vkd3d-proton (what Proton uses), not Wine's vkd3d, which the native Linux build uses.
   - It needs two downloads, pinned, into the scratchpad.
2. **A Linux PC or Steam Deck**, three ways on the same machine:
   - the native build on Wine's vkd3d;
   - the Windows build in Proton (vkd3d-proton);
   - Windows itself, where it dual boots.

**What decides:**
- **If vkd3d-proton is close to native D3D12 on the render thread:** there is a cheaper first step, moving the Linux
  build from Wine's vkd3d to vkd3d-proton.
  - To check: whether vkd3d-proton builds as a native Linux library.
  - It has no HLSL compiler, so the build would keep vkd3d-shader or glslang for that.
  - Native Vulkan then waits for HDR or macOS.
- **If it's slow, or Wine's vkd3d is much slower than vkd3d-proton:** native Vulkan pays on Linux first.

### 5.1 Measured on this PC (2026-10-27)

**Setup:**
- RTX 5070 Ti, the same driver on both sides.
- The same `des_host.exe` and `des_game.dll` on both sides. B also has vkd3d-proton 3.0.1's `d3d12.dll` and
  `d3d12core.dll`, and DXVK 3.1.1's `dxgi.dll`, beside them. B's log names vkd3d-proton.
- `exeab.sh`, 6 rounds of alternating boots, plain windows, at the pinned save (m08), uncapped (`DES_FPS=unlocked`,
  `RSX_VSYNC=0`), presenting.
- Each side has its own copy of the shader cache, and B its own vkd3d-proton cache.
- One warm-up round first, so B's pipeline library and caches were built before the batch.

**Results:**

| | Boot means | Median | Spread |
|---|---|---|---|
| A, native D3D12 | 465.6–483.7 fps | 476.6 | sd 6.1 |
| B, vkd3d-proton | 461.4–470.1 fps | 466.4 | sd 2.9 |

- B was slower in 5 of 6 rounds. The median was −11.7 fps (−2.45%); paired t p = 0.026.
- That is inside exeab's noise bar of 4.5%, but consistent.
- Per frame, that's 2.10 ms against 2.15 ms: about 45 µs more. As a fixed cost per frame it's 0.3% at 60 fps and
  0.7% at 144 fps.
- B's frame rate was steadier than A's (sd 2.9 against 6.1 fps).

**On a first start:** the warm-up boot on vkd3d-proton read −9%. Its pipeline library, which a native driver had
written, didn't fit and was rebuilt (2,471 pipelines), and vkd3d-proton filled its own cache. After that, each B boot
loaded all of them.

**Not measured yet:**
- a heavier scene: the deep Palace, with more draws a frame, which is where a cost per draw would show;
- Wine's vkd3d, which the native Linux build ships;
- AMD and Intel drivers.

**What it means:** on Windows, vkd3d-proton is close to native. The cheaper first step in the list above holds:
- a "Vulkan" choice that loads vkd3d-proton beside the exe;
- on Linux, a check of the native vkd3d-proton build against Wine's vkd3d.

Native Vulkan can wait until HDR on Linux or macOS asks for it.

**Licences:** vkd3d-proton is LGPL-2.1 and DXVK is zlib, so both can ship as separate DLLs.

## 6. macOS and the rest

- **MoltenVK** lacks some things the RSX path uses:
  - no triangle fans (the portability subset; the core would turn fans into lists);
  - no D24S8 on Apple GPUs.
- **The CPU side:** Apple silicon also needs an arm64 lift or Rosetta 2 (docs/46 B4).
- Upstream has a Metal backend too.
- None of this is in the 50–80 days.

## 7. Plan

0. Measure vkd3d (section 5). Done on Windows (5.1): vkd3d-proton costs about 2.5% at 470 fps. Still to do: the deep
   Palace, and Linux hardware.
1. Split the backend, with D3D12 only and no change in behaviour: regression A/B, then merge (10–15 days).
2. Bring up the Vulkan layer and shaders on Linux first, against the vkd3d build, behind `RSX_BACKEND=vulkan`
   (20–30 days).
3. Presenting, HDR, the menu on Vulkan, and device lost (11–18 days).
4. On Windows: the surface function, the Settings row and the fallback (3–5 days).
5. Parity testing on both renderers (7–12 days). Then Linux defaults to Vulkan, and vkd3d stays one release.
