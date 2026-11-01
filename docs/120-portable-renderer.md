# 120. A portable renderer beside today's D3D12 renderer (design, 2026-10-30)

The goal: render on D3D12, Vulkan and later Metal from one RSX core, with as little per-platform code as possible. It
must lose nothing: today's renderer stays exactly as it is, one setting away.

This builds on docs/113 (Vulkan beside D3D12). Figures marked **read** were seen in the code; **inferred** ones are
estimates no run has checked.

## Terms

- **Today's renderer:** `ps3recomp/libs/video/rsx_d3d12_backend.c`. It is one file of 16,472 lines, and every Demon's
  Souls rendering fix lives in it. On Linux it runs on vkd3d.
- **HAL** (hardware abstraction layer): a small set of our own types and calls, such as `gpu_device`, `gpu_buffer` and
  `gpu_pipeline`. Each graphics API implements them in a **backend**.
- **Core:** the RSX logic above the HAL: draw records, vertex fetch, the texture cache, targets, write-back, ZPASS,
  pipeline keys and the index. It knows no graphics API.
- **Route:** which renderer runs, chosen at start with `RSX_BACKEND`.
- **Capture:** one frame recorded with `RSX_REPLAY_CAP` (or F9). `rsx_replay` plays it back through a renderer in
  seconds, with no game.

## Short answer

- **Today's renderer is frozen.** These files are never edited:
  - `rsx_d3d12_backend.c` and `.h`;
  - `port/ui/src/render_d3d12.cpp` and `render_d3d11.cpp`.
  
  The new path copies their logic into new files.
- **The routes** (`RSX_BACKEND`):
  - `d3d12`, the default: today's renderer, unchanged;
  - `vulkan`: the new path on our own Vulkan backend;
  - `hal-d3d12`, for developers: the new path on its own D3D12 backend, to compare against today's;
  - `metal` / `auto`: on macOS, NRI's Metal backend first, then our Vulkan on KosmicKrisp, then MoltenVK.
- **On Windows,** Settings > Display > Renderer offers D3D12 (today's renderer) or Vulkan, after a restart. If Vulkan
  fails to start, the game starts on D3D12 and says why.
- **All new code sits behind the CMake option `PS3RECOMP_RSX_HAL`.** With it off, every object file is the same as
  before. Dropping the idea means turning the option off, or deleting the new folders.
- **Nothing ships until it matches today's renderer:**
  - the new D3D12 route must give bit-identical frames;
  - the Vulkan route must be within a strict image rule;
  - both must reach the same fps.

## Layout (ps3recomp)

| Folder or file | What it holds |
|---|---|
| `libs/video/rsxcore/` | The core: `rsxc_*.inc` parts copied from today's renderer, range by range, and one small file per backend that compiles them (`rsxc_d3d12.c`, later `rsxc_vk.c` and `rsxc_null.c`). `copy_manifest.txt` names today's base commit, every copied range and every rule the copy changed it by. |
| `libs/video/gpu/` | The HAL contract (`gpu.h`, `gpu_cmd_ops.h`, `gpu_vt_cmd.h`); the backends (`gpu_d3d12*`, `gpu_vk*`, `gpu_null*` for headless tests, `gpu_nri_metal.cpp` on macOS); the shader compile service (`gpu_shader.*`). |
| `libs/video/rsx_out.*` | The front. It picks the route once at start; the port calls `rsx_out_*` instead of `rsx_d3d12_*`. On the `d3d12` route each call goes straight to today's function. |

**Rules:**
- Names exported from the new code start with `rsxc_`, `gpu_` or `rsx_out_` (checked by `tools/des/hal_symbols.sh`).
- A fix to today's renderer also lands in the core. `tools/des/core_sync.sh` lists the ones not yet ported.

## Keeping today's speed

- **Recording is copied as it is.** The record path makes no API calls (read): `d3d12_draw_indexed`, `vp_record_cb`
  and the vertex fetch.
- **The core is one source, compiled once per backend.** Each backend's commands, and the few device calls made per
  draw, are `static inline`, so nothing on the draw path goes through a function table. About 6,400 draws a frame at
  9-12 calls each would otherwise cost about 77,000 indirect calls a frame (inferred). Only one backend runs in a
  process, so the copies cost disk space, not memory.
- **The handles used every draw** (pipeline, descriptor table) hold the native object, so the replay looks nothing up.
- **Neutral values use D3D12's numbering** (DXGI formats, resource states, the swizzle encoding), so the copied code
  needs no translation. Vulkan and Metal map them with small tables.

## Vulkan

- **Versions:** Vulkan 1.3 on Windows and Linux; 1.2 plus dynamic rendering and synchronization2 on macOS, for
  KosmicKrisp.
- **The binding model matches today's root signature**, so the HLSL doesn't change:
  - two dynamic uniform buffers and one dynamic storage buffer, at today's offsets;
  - one descriptor set per draw window, made up front;
  - today's 128 sampler tables;
  - one push constant.
- **Every need has a native feature:**
  - swizzle;
  - depth clamp;
  - occlusion queries that count samples;
  - host memory as a GPU buffer, for the write-back (this gives Linux the GPU write-back too);
  - scRGB HDR;
  - tearing;
  - frame statistics;
  - a pipeline cache.
- **Two fallbacks for hardware that lacks a feature:**
  - **D32S8** where D24S8 is missing (AMD on Linux, Apple GPUs). The depth-as-colour decode already works from a float
    depth (read).
  - **A vertex-shader HUD clamp** where geometry shaders are missing (Mac).
  
  Both can be forced on this PC with knobs (`RSX_DEPTH_FORMAT`, `RSX_CLAMP_FORM`) and compared.
- **Settings > Display > Renderer:** D3D12 (the default) or Vulkan, after a restart. It is the `RSX_BACKEND` row in
  settings.ini, applied at start as every restart row is, and an `RSX_BACKEND` from the environment wins. It shows only
  when the runtime has the Vulkan route (`PS3RECOMP_RSX_HAL_VULKAN` and `_SPV`, read from its CMakeCache.txt). `hal-d3d12`
  stays an environment value for developers.
- **When Vulkan does not start,** the game starts on D3D12 and says why: in the log
  (`[rsx_out] route d3d12 (...)`) and in a report's `system.txt` (`Renderer: d3d12 (...)`).
  - A start that fails at a step the renderer sees (loader, instance, adapter, features, device, window, swap chain,
    targets, shaders, rings) is undone, and D3D12 starts in the same process. `RSX_FAIL_START_AT=<step>` forces one.
  - A start that dies in the driver: a Vulkan start writes `renderer_starting` in the data folder (`RSX_STATE_DIR`, which
    the port sets) and removes it at its first frame. A start that finds it uses D3D12; the start after that tries
    Vulkan again. Test runs have no data folder, so no mark.

## Shaders

- **The text builders are copied word for word** (`rsxc_shader.c`), so the shader cache keys are today's. The new
  D3D12 backend shares the `.dxbc` cache, byte for byte.
- **A CI test** feeds thousands of generated RSX programs to both copies and checks the texts match.

| Target | Compiler |
|---|---|
| D3D12 | FXC, today's exact call |
| Vulkan | glslang to SPIR-V at first; DXC once it passes the same tests on the game's shaders |
| NRI Metal | DXC to DXIL, then Apple's Metal Shader Converter, with its NaN/Inf optimisation off |

- **Text edits for the new targets:**
  - stage locations by semantic;
  - NaN guards written as integer tests, so no fast-math pass removes them;
  - the vertex-fetch address clamped to the buffer.
- **New targets get their own cache files and keys,** which include the compiler and its version.

## Gates

| Gate | What must hold |
|---|---|
| G0 | Shared code changes leave today's HLSL texts and cache keys byte-identical. |
| G1 | With `PS3RECOMP_RSX_HAL` off, the build is the same as before. With it on and route `d3d12`, regress passes and an exe A/B reads even. |
| G2 | `hal-d3d12` against today's renderer, on a set of captures: identical frames (0 differing bytes), identical per-draw fingerprints (`RSX_DRAWPRINT`), ZPASS values and pipeline descriptions; regress passes live. |
| G3 | Vulkan against today's renderer: identical per-draw fingerprints, clean validation, and at least 99.95% of pixels within ±1/255, no 16×16 block mean above 1.0, at most 0.01% of pixels off by more than 8/255. HDR frames within 1e-3, except at most 0.01% of pixels within 8/255. Unrelated captures replayed back to back are not compared. |
| G4 | Speed: replay CPU time and an exe A/B even (−0.5% at most for `hal-d3d12`, −1.0% for Vulkan); frame pacing and first-start hitches no worse. Anything slower is the user's decision. |

The tools: `tools/des/halgate.sh` (replays and comparisons), `tools/des/halimg.py` (the image rule),
`tools/des/capset.sh` (taking captures), plus `regress.sh`, `exeab.sh` and `soak.sh`.

## Order

0. **Groundwork:** the HAL contract and the build option; the shader text copy and its identity test; the gate tools;
   a first run of the game's shaders through the other compilers.
1. **The new path on D3D12,** gated by G2 and G4. Developers only. First a word-for-word copy of today's renderer
   behind the route switch, identical by construction. Then it moves onto the HAL one piece at a time, and every
   piece is checked against the captures before the next.
2. **Vulkan on Windows,** gated by G3 and G4; then the Settings row and the fallback.
3. **Setup's shader pass and the pipeline stores for each backend,** alongside phase 2.
4. **Linux on Vulkan,** with a CI run on lavapipe under validation and a test kit for testers on real Linux PCs.
   vkd3d stays the Linux default until testers' numbers match.
5. **The platform module** (`plat.h`), in parallel. It fixes Linux's per-job SPU threads (inferred: about 650 a
   second) and moves the window to the main thread for macOS.
6. **macOS:** docs/118's CPU work, NRI Metal, and the Vulkan fallback, built in CI on Apple Silicon and tested by
   people with Macs.
