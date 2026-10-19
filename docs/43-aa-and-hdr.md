# Anti-aliasing and HDR output (spike)

Date: 2026-10-18. Status: research only. Nothing was built or run, and nothing was measured.

Code references are to ps3recomp's fork master at `f15a4f0` (RSX_SCALE merged, PR #50). The local ps3recomp checkout is older than that. "The backend" means `ps3recomp/libs/video/rsx_d3d12_backend.c` at that commit, and `BE:1102` is a line in it. The live draw engine (`rsx_live_draw.c`, opt-in with `RSX_LIVE_DRAW`) is not the Demon's Souls path and is out of scope.

Two labels are used below:
- **Read**: seen in our source, RPCS3's source, a capture manifest, a run log, or a vendor document.
- **Inferred**: a conclusion that still needs a check. Section 10 lists what could not be settled.

Costs are either published (the source is named) or extrapolated (marked Inferred). No performance runs were made for this spike.

## Short answer

Recommended order of work:
1. **Better output filter** (0.5 to 1 day). The final scale pass takes one bilinear tap. Use an area filter when the render is bigger than the window, and a bicubic filter when it is smaller. RSX_SCALE then works as proper supersampling at any ratio, and the default 100% picture gets sharper on 1440p and 4K screens. FSR1 can follow as a third choice (1 to 2 more days).
2. **Post AA before the HUD, starting with FXAA** (2 to 3 days). The game draws its HUD into the same target as the scene, after its post chain. The backend replays each frame at its end, so it can find the first HUD draw before it starts and run the AA pass just before it. HUD text stays sharp.
3. **SMAA 1x on the same hook** (2 to 3 more days). Sharper than FXAA, and MIT licensed.
4. **HDR.** First try Windows Auto HDR and NVIDIA RTX HDR on an HDR display (half a day, no code). For the port's own HDR: scRGB output with a paper-white setting (4 to 6 days), then optional highlight expansion that leaves the HUD at paper white (2 to 3 days; it reuses the AA hook).

Not recommended:
- **Forced MSAA** (8 to 15 days, high risk). Demon's Souls never asks for MSAA. Forcing it runs into the game's mid-frame colour and depth reads, a depth buffer shared with 128x128 targets, the write-back and the guest-memory fill. Alpha-tested edges would still be aliased.
- **TAA**, and the upscalers built on the same inputs (DLSS, FSR2/3, XeSS). The game makes no motion vectors and the backend does not know the camera. Weeks of work for a result that ghosts.
- **CMAA2.** A compute path with UAVs, for quality close to SMAA.

Effort:

| Item | Days | Risk |
|---|---|---|
| A. Output filter (area down, bicubic up), live setting | 0.5 to 1 | Low |
| A2. FSR1 upscale option | 1 to 2 more | Low |
| B. AA hook before the HUD, with FXAA, live setting | 2 to 3 | Low to medium |
| C. SMAA 1x on the hook | 2 to 3 more | Low to medium |
| D. CMAA2 | 3 to 4 more | Medium (skip) |
| E. Forced 4x MSAA on the scene target | 8 to 15 | High (skip) |
| F. TAA / DLSS / FSR2 | 20 or more, open-ended | Very high (skip) |
| H0. Try Auto HDR and RTX HDR (no code) | 0.5 | None |
| H1. scRGB output, paper white, UI path, live switching | 4 to 6 | Medium |
| H2. Highlight expansion with a HUD mask (needs B) | 2 to 3 more | Medium |
| H3. Native HDR scene (research) | 10 to 20 | High |
| **Recommended path: A, B, C, H0, H1, H2** | **11 to 17** | |

Top risks:
- The AA insertion rule was worked out on one in-game frame. The title, menus, cutscenes and other areas may be laid out differently. A wrong guess blurs the HUD or skips AA. Log the chosen op and keep fallback knobs.
- HDR cannot be judged without an HDR display. It is not known whether this PC has one.
- The wrapper UI assumes an RGBA8 back buffer in three places. With HDR, the UI has to go through its own RGBA8 texture.
- Highlight expansion of an 8-bit picture can band in skies and fog, and it changes the look. Keep it off by default.
- FXAA 3.11's licence is unclear. Check it before copying the header, or write our own pass. SMAA is MIT.

## 1. What the backend does today (Read)

- **Swap chain.** `DXGI_FORMAT_R8G8B8A8_UNORM`, 2 buffers, `FLIP_DISCARD`, plus `ALLOW_TEARING` when the system has it (BE:1385-1436). `ResizeBuffers` keeps RGBA8 (`swap_resize`, BE:1156-1170). The swap chain is held as `IDXGISwapChain3`, which already has `SetColorSpace1` and `CheckColorSpaceSupport`.
- **Game frame.** Draws into a display buffer (an offset registered with `cellGcmSetDisplayBuffer`) go to the backend's own RGBA8 frame target at host size, `render_targets[fi]` (BE:1203-1221; `current_rt_off`, BE:5578). Every other surface is an offscreen target (`off_rt_get`, BE:5944). Those are RGBA8 unless the RSX format is a float format (BE:5636).
- **Replay at frame end.** Draws are recorded during the frame and replayed by `render_frame` (BE:7718) at the flip. A pre-pass walks every draw record before the main loop (about BE:8015-8290). Each record knows which texture units sample an offscreen target (`tex_rt[]`) or a depth surface (`tex_z[]`). Only units the fragment program reads are filled in (`fp_units_at`, about BE:8131-8141). So the backend can look over the whole frame before it draws anything.
- **Final scale pass.** `blit_to_window` (BE:1102) draws one full-screen triangle from `render_targets[fi]` into the swap buffer. Its pixel shader takes one `MIN_MAG_MIP_LINEAR` sample from a 1-mip texture, which is plain bilinear, and writes alpha 1 (`blit_init`, BE:1022-1070). The viewport keeps the frame's aspect ratio, with black bars.
- **End-of-frame order.** `guest_fb_present` and `composite_present` (both unused by DeS), the dump or screen copy, `rt_writeback_record`, then `blit_to_window`, close and execute (BE:9171-9228). Then the overlay's frame callback draws the wrapper UI onto the swap buffer, and `Present` runs (BE:9406-9431).
- **Wrapper UI.** RmlUi's D3D11 renderer draws straight onto the swap buffer through D3D11On12 (`port/ui/src/host.cpp`). Three places assume RGBA8:
  - `rsx_d3d12_back_buffer_format` returns the constant (BE:12251);
  - `copy_to_staging` makes an RGBA8 staging texture for screenshots and the crash-report picture (`host.cpp:196-209`);
  - `crash_dialog.cpp:128` copies that format.
- **No MSAA anywhere.** Every PSO and every resource is made with `SampleDesc.Count = 1` (game PSOs: BE:4212). `NV4097_SET_SURFACE_FORMAT` is stored whole, and its antialias field (bits 12-15) is never decoded (`rsx_commands.c`, about line 132).
- **Demon's Souls asks for no MSAA.** An `RT_OFFDBG` log from boot into the save's level has 10 distinct surface format words. Examples: `0x090A0148` for the 1280x720 scene at `0x7A0000`, and `0x0B0B0150` for a depth-only 2048x2048 pass (the shadow map). All 10 have the antialias field at 0.
- **RSX_SCALE.** 50 to 800% (BE:51-55). Targets of 64 px or less (`RSX_SCALE_MIN`) stay at guest size (BE:66-74). The settings menu offers 100, 150, 200 and 300% (docs/37).

## 2. Where the HUD lands in a Demon's Souls frame (Read)

From an `RSX_CAP` manifest of an in-game frame in the first level (1919 draws). Ops are in replay order.

| Ops | Target | What | Notes |
|---|---|---|---|
| up to 1789 | `0x7A0000`, depth `0xB50000` | the scene, depth test on | each scene program ends in the tone map (docs/28) |
| 1790 | `0x6370000` | copy of `0x7A0000` (fp `0xB40081`) | mid-scene colour read |
| 1791-1817 | `0x7A0000` | blended effects, depth test on, no depth write | 1791-1792 sample `0x7A0000` itself (through the snapshot); later ones bind the depth surface |
| 1818 | `0x6370000` | second copy of `0x7A0000` | |
| 1821 | `0x7A0000`, **alpha only** (mask 8) | depth-of-field mask from depth (fp `0xB41501`) | alpha now holds the DoF mask |
| 1822-1837 | 640x360 down to 16x16 | DoF blur, bloom, luminance, exposure | |
| 1838 | `0x7A0000`, RGB | composite (fp `0xB42C01`) | |
| 1839-1840 | `0x4D60000`, then `0x7A0000` RGB | motion blur (fp `0xB41181`, then `0xB40D01`, which reads depth) | last full-screen post op |
| 1841-1918 | `0x7A0000`, RGB | **HUD**: 78 draws, fp `0x79FE81` and `0xB4FF01`, blended, depth test off | |
| end of frame | display buffer | display copy, fp `0x181`, through a 256x1 LUT | |

Notes:
- The manifest lists the display copy as op 1, because a replay cuts frames at display clears (docs/28). In live play it is the last draw before the flip. docs/28's black-flicker entry says the dropped records took "the display copy at the end" with them.
- A 15 fps skip frame is a display clear plus the display copy (docs/28). It shows the last finished `0x7A0000` again.
- In `cap_027` the 256x1 LUT was an identity ramp (docs/28, Nexus colours).
- The game has a brightness dialog with a 0 to 10 value (docs/33: `0x008EC7B8`, value at `+0xFC`). What the value changes was not traced.

What this means:
- The HUD and the in-game menus are drawn into the scene target. AA at present time would also hit HUD text.
- AA that spares the HUD has to run inside the frame: in this frame, between op 1840 and op 1841.
- The post ops and the HUD write RGB only, and alpha holds the DoF mask. An AA pass must also write RGB only. It must not take luma from alpha, which is FXAA's default.

## 3. Post-process AA (FXAA, SMAA, CMAA2)

### 3.1 Where it runs

**Option 1: before the HUD (recommended).** `render_frame`'s pre-pass picks the op. The main loop (BE:8519) runs the AA pass when it gets there.

The rule (Inferred; it matches the frame above). It uses only the frame's draw records, so it depends on guest state, not on timing.
1. Find the display copy: the last draw into the display buffer that samples an offscreen target. The target it samples is the scene target, T (`0x7A0000`). No display copy: no AA this frame.
2. Count the depth-tested draws into T. Fewer than about 16: no AA. That skips 2D frames (title, loading, movies) and skip frames.
3. Find the last draw into T, before the display copy, that samples an offscreen target or a depth surface (`tex_rt[]` or `tex_z[]`). Here that is op 1840 (motion blur). With `DES_MOTION_BLUR=0` it should be op 1838 (Inferred).
4. Run AA right after that draw.

The pass:
- Copy T with `off_rt_snapshot` (BE:6643). That copy already exists for draws that sample their own target; ops 1791-1792 use it on T every frame. So no new memory.
- Draw the filter from the copy into T, with an RGB write mask.
- Restore what the main loop expects: the root signature and descriptor heaps (set once, BE:8465-8472), and a forced target rebind for the next draw (`cur_rt` tracking, BE:8497-8657).

Fallbacks, as knobs:
- `RSX_AA_BEFORE_FP=0x79FE81,0xB4FF01`: insert before the first draw that uses one of these programs.
- `RSX_AA_AT=present`: run on the finished frame (option 2).
- A log line when the choice changes: `[AA] T=0x7A0000 at op 1841 of 1919, after fp 0xB40D01`.

**Option 2: at present.** Filter `render_targets[fi]` into a scratch target, and let `blit_to_window` scale the scratch target. About half a day less work. It blurs HUD text and the menus. The wrapper UI is drawn later, on the swap buffer, so AA never touches it in either option.

### 3.2 Which filter

| | FXAA 3.11 | SMAA 1x | CMAA2 |
|---|---|---|---|
| Passes | 1 pixel shader | 3 pixel shaders (edges, weights, blend) | compute shaders |
| Extra memory at 720p | none (the snapshot) | edges RG8 + weights RGBA8 = 5.5 MB, lookup textures 180 KB | small, but the output must be a UAV |
| Look | softest; also blurs texture detail | sharper; better on long edges | sharp, conservative |
| Licence | unclear: the header says "all rights reserved", Wikipedia says 3-clause BSD | MIT (Read: repository README) | Apache-2.0 (Read: repository) |
| Fit | quickest way to prove the hook | best quality for the work | needs a compute root signature and a copy back; little gain over SMAA |

Notes:
- FXAA and SMAA expect gamma-space input. T holds the values that go to the screen: the tone map writes them, and the LUT was identity in `cap_027`. So the filters can run on T as it is (Inferred).
- Luma has to come from RGB (green, or a weighted sum), because alpha is the DoF mask.
- The pass runs at host size, so it combines with RSX_SCALE. AA at 200% followed by an area downscale gives the cleanest edges.
- AA touches only T, which is display-sized, so the 64 px RSX_SCALE threshold does not matter here.

### 3.3 GPU cost

Published: FXAA costs 0.88 ms a frame at 1920x1080 on a GTX 460, and 0.39 ms on a GTX 560 (NVIDIA's FXAA sample page). Intel describes CMAA2 as roughly as fast as FXAA. No published SMAA 1x timing was found today.

Extrapolated (Inferred): an RTX 5070 Ti (the GPU in docs/37) has about 8 times the memory bandwidth of those cards (896 vs 115-128 GB/s), and more than that in shader rate. Scaling the published figures by pixel count and dividing by 8:

| T size | Pixels | FXAA | SMAA 1x (1.5 to 2 times FXAA) |
|---|---|---|---|
| 1280x720 (100%) | 0.92 M | 0.02 to 0.05 ms | 0.03 to 0.1 ms |
| 2560x1440 (200%) | 3.7 M | 0.09 to 0.2 ms | 0.15 to 0.4 ms |
| 3840x2160 (300%) | 8.3 M | 0.2 to 0.45 ms | 0.3 to 0.9 ms |

For scale: a frame is 16.7 ms at 60 fps and 4.2 ms at 240 fps.

### 3.4 Risks

- Frame layouts not seen yet: title, character creation, menus over the scene, cutscenes, other areas. The skip conditions, the log line and the fallbacks are the mitigation.
- The same `RT_OFFDBG` log has a second 1280x720 target with the scene depth buffer (`0x5870000`). What it is was not checked. The rule takes T from the display copy, so that target should not confuse it (Inferred).
- FXAA softens texture detail. SMAA does so much less.
- With AA on, the frame is no longer bit-exact with today's. With AA off it must stay bit-identical.

### 3.5 Effort

- FXAA-style shader and the pass: 1 day.
- The rule, the state restore, the knobs and the setting: 1 day.
- Tests across areas, menus and cutscenes: 0.5 to 1 day.
- SMAA 1x on top: 2 to 3 days (three passes, two targets, the lookup textures as headers).

### 3.6 Tests (no perf A/B)

- **AA off.** `regress.sh` against the accepted baseline, and `capdiff.py` on F9 captures. Both must be identical.
- **Insertion op.** Replay `RSX_CAP` captures with `rsx_replay`: first level, Nexus, Palace, a cutscene, the in-game menu, the title. Compare the `[AA]` line with each manifest. AA should come right after the last post op and before the first HUD draw, and be skipped on title, loading and movie frames.
- **HUD untouched.** Dump T with AA off and on. Inside the HUD's opaque boxes (soul counter, bar frames) the two dumps must be identical.
- **Edges changed.** A short Python check on the same two dumps. Changed pixels should sit on scene edges. None in flat areas or in the black bars.
- **Scale.** RSX_SCALE at 150, 200 and 300%, and switching scale and AA live: the same op, no size mismatch, no device removal.
- **Visual.** The user looks at the Palace gate and the Nexus stairs at 100%, with AA off, FXAA and SMAA.

## 4. Supersampling with RSX_SCALE, and the output filter

What exists (Read, docs/37): RSX_SCALE renders the game's targets at 150, 200 or 300%, switchable live. Measured there on an RTX 5070 Ti in the tutorial level with `DES_FPS=240`: 240 fps at 100% (the cap), 240 at 150%, 236 at 200%, 226 at 300%.

What the final pass does with a scaled frame (Read: BE:1102-1150; the ratios are arithmetic):
- At exactly 2:1 (300% in a 1080p window, 200% in a 720p window), one bilinear tap at each pixel centre averages a 2x2 block. That is already a correct box filter.
- At 1:1 (150% in a 1080p window) the copy is exact.
- Other ratios are not handled well. At 1.33:1 (200% in a 1080p window) source texels get unequal weights. Above 2:1 (300% in a 720p window, 400% in a 1080p window) texels are skipped, which shimmers.
- Upscaling is a single bilinear tap, so it is soft. That is the default case: 100% in a 1440p window is 2x, in a 4K window 3x.

Proposal:
- **Downscale:** an area filter. Each output pixel averages the source texels under it, with 4 to 9 bilinear taps depending on the ratio. Optional: average in linear light (decode, average, encode), so thin bright lines do not darken.
- **Upscale:** Catmull-Rom bicubic in 9 bilinear taps, or FSR1 (EASU then RCAS; MIT licence; the same as RPCS3's "FSR" output scaling).
- **Hook:** `blit_init` and `blit_to_window` (BE:1022-1150). Pass the source and viewport sizes as root constants, and pick one PSO per mode.
- **Setting:** `RSX_OUTFILTER` = 0 bilinear (today), 1 sharp (area down, bicubic up), 2 FSR1. Live.

Cost (Inferred): one pass at window size with 4 to 9 taps, about 0.02 to 0.1 ms at 1440p to 4K. FSR1 is two passes.

Effort: 0.5 to 1 day for area and bicubic. FSR1: 1 to 2 more days.

Risk: low. Captures and `regress.sh` read `render_targets[fi]`, not the swap buffer (BE:2417-2419), so they cannot see this change.

Tests:
- Mode 0 leaves the swap buffer bit-identical. Compare a wrapper screenshot (it reads the swap buffer, `host.cpp:317`) before and after.
- Mode 1 with RSX_SCALE=300 in a 1280x720 window: the screenshot must match a Python area downscale of the same frame's `CELLMARK_DUMP` backbuffer within 1 to 2 levels per channel.
- Modes 1 and 2 at 100% in a 4K window: the user compares HUD text and the Nexus railings.

## 5. MSAA on the game's targets (not recommended)

RPCS3 uses the sample count the game asks for, and nothing more (section 7). Demon's Souls asks for none, so RPCS3's MSAA does nothing for it. Here the backend would have to force MSAA, at least on T and its depth buffer. Only those two would get it; the bloom and exposure chains, and the targets that RSX_SCALE_MIN keeps at guest size, would stay 1x. What forcing runs into (Read unless marked):
- **PSOs.** All are 1x (BE:4212). The PSO key and the on-disk pipeline library key would gain the sample count.
- **Mid-frame colour reads of T.** Ops 1790, 1818, 1822, 1825 and 1839 sample T. Each needs a resolve first, or every program that reads T needs a `Texture2DMS` variant.
- **Self-sampling.** Ops 1791-1792 read T while drawing into it. The snapshot would have to be a resolve instead of a copy.
- **Depth reads.** Op 1821 and the motion blur read the depth buffer `0xB50000` as a texture. The soft particles from op 1795 on bind it while it is the read-only depth target. An MSAA depth buffer needs a shader resolve (sample 0, or min) before each read; `ResolveSubresource` does not take depth formats.
- **Shared depth buffer.** The same `0xB50000` is used with two 128x128 targets, `0x17B0000` and `0x17D0000` (`RT_OFFDBG` log). D3D12 needs matching colour and depth sample counts, so those passes need MSAA too, or their own 1x depth buffer.
- **Guest-memory fill.** A new target is filled from guest memory with `CopyTextureRegion` (BE:5944). That cannot write an MSAA resource; it would need a draw.
- **Write-back.** Every used target is copied back to guest memory each frame (BE:6377). MSAA targets need a resolve first.
- **Alpha-tested edges** stay aliased without alpha-to-coverage, which the game never turns on (Inferred).
- **Memory.** 4x on T and its depth buffer: 29.5 MB at 720p (7.4 MB now), 118 MB at 200%.

Cost (Inferred): 4x MSAA on the scene pass, plus about six resolves a frame. Small on a current GPU at 720p, and it grows with RSX_SCALE.

Effort: 8 to 15 days. Risk: high. The gain over SMAA plus RSX_SCALE is limited to edges of opaque geometry.

## 6. TAA (not feasible)

- The game makes no motion vectors.
- The backend does not know the camera. The matrices sit in vertex-program constants, in a layout each program chooses. Reprojection from depth alone would cover static geometry only.
- Skinned characters, cloth and particles have no previous-frame positions.
- Jitter would need a sub-pixel offset in the decompiled vertex programs, for scene draws only (not shadow maps, post ops or the HUD). The game's DoF and motion blur read depth at pixel centres and would see the jitter.
- The history breaks on camera cuts, on the 15 fps skip frames, and against the game's own motion blur.
- DLSS, FSR2/3 and XeSS need the same inputs.
- RPCS3 has no TAA either.

Effort: four weeks or more, for a result that ghosts. Not worth it while SMAA plus RSX_SCALE is available.

## 7. What RPCS3 does (Read: RPCS3 source)

| Setting (`rpcs3/Emu/system_config.h`) | Values | What it does for DeS |
|---|---|---|
| MSAA (line 126) | None, Auto (default) | Auto creates surfaces with the game's own sample count (`RSX/VK/VKRenderTargets.h:233` and `:306`). DeS asks for none, so it does nothing. |
| Force Hardware MSAA Resolve (line 158) | off by default | an accuracy option for games that use MSAA |
| Resolution Scale (line 166) | 25 to 800%, default 100 | the same idea as RSX_SCALE |
| Minimum Scalable Dimension (line 169) | default 16 | ours is 64 (`RSX_SCALE_MIN`), because DeS's 64x64 exposure target must keep its size (docs/37) |
| Anisotropic Filter Override (line 167) | 0 to 16 | our RSX_ANISO |
| Output Scaling Mode (line 179) | nearest, bilinear (default), FSR | FSR1 with RCAS sharpening |

- There is no FXAA, SMAA or TAA setting.
- There is no HDR output. The FSR pass says so: "we do not support HDR natively" (`RSX/VK/upscalers/fsr1/fsr_pass.cpp:152`).
- MSAA is switched off when the Vulkan driver lacks multisampled storage images (`RSX/VK/vkutils/device.cpp:630-633`).
- What broke: white lines or squares above 100% resolution scale ([#3822](https://github.com/RPCS3/rpcs3/issues/3822), docs/29 M10). We saw thin lines along UI panel edges at 200% as well. The UV clamp for 2D draws on scaled targets fixed them (docs/37; `dr_uvclamp`, BE:3730).

## 8. HDR output

### 8.1 What Windows needs (Read: Microsoft's Advanced Color and `DISPLAYCONFIG_SDR_WHITE_LEVEL` pages)

- A flip-model swap chain. We have one.
- One of two formats:
  - FP16 (`R16G16B16A16_FLOAT`) with `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. This is scRGB: linear, Rec.709 primaries, 1.0 = 80 nits. Microsoft recommends it for most apps.
  - `R10G10B10A2_UNORM` with `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (HDR10, PQ).
- Detection: `IDXGIOutput6::GetDesc1`. Its `ColorSpace` is `G2084_P2020` when Windows HDR is on for that display. It also gives `MaxLuminance`, `MaxFullFrameLuminance` and `MinLuminance`.
- Use the output that overlaps the window most. Check again when `IDXGIFactory::IsCurrent` turns false, and when the window moves or the display setup changes.
- SDR white level: `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL`. Nits = `SDRWhiteLevel` / 1000 x 80. This is the "SDR content brightness" slider.
- SDR content in an HDR swap chain should be scaled by that white level / 80, so it matches the desktop.
- On an SDR display an FP16 swap chain still works. The compositor clips it to SDR.
- Today, on an HDR desktop, Windows composes our RGBA8 output as sRGB at the SDR white level. So the game already looks right there. It just has no highlights.

### 8.2 Stage 0: Auto HDR and RTX HDR (no code)

- Windows Auto HDR is for DirectX 11 and 12 games that Windows recognises as games. Whether `des_boot.exe` gets it, for example after the Game Bar's "remember this is a game", was not verified.
- NVIDIA RTX HDR needs an RTX GPU, the NVIDIA app, and Windows HDR on with Auto HDR off. Press coverage says it works with DX9 to DX12 and Vulkan games in fullscreen or borderless mode; one source measured about 8% lower frame rates. Not verified here. The port has a borderless mode.
- Both treat the HUD as part of the picture and may brighten it (Inferred).
- Cost: half a day, with an HDR display. It also sets the bar the port's own HDR has to beat.

### 8.3 Stage 1: scRGB output with paper white

**Swap chain** (BE:1385-1436; `swap_resize`, BE:1156):
- When `RSX_HDR` is on and the window's output reports `G2084_P2020`: FP16 buffers, then `SetColorSpace1(G10_NONE_P709)` once `CheckColorSpaceSupport` allows it.
- Otherwise RGBA8, exactly as today.
- Switching (Windows HDR toggled, or the window moved to another display) goes through `ResizeBuffers` with the new format. `swap_resize` already idles the GPU, calls the overlay's `release` so the UI drops its D3D11On12 views, and re-reads the buffers. It returns early when the size has not changed, so a format change needs its own trigger.

**Mapping**, in the `blit_to_window` pixel shader:
- `lin = decode(c)`. Gamma 2.2 by default, the sRGB curve as an option. Windows uses the sRGB curve for SDR content; on many displays that lifts near-black compared with gamma 2.2 (commonly reported; for DeS's dark areas, Inferred).
- `out = lin x paper_white / 80`. Paper white defaults to the display's SDR white level. A slider from 80 to 500 nits overrides it.
- With the output filter from section 4, decode before filtering.

**The wrapper UI.** RmlUi writes gamma-encoded RGBA8 values. Written into a linear FP16 buffer they would come out too dark, and at 80 nits. So:
- RmlUi draws into its own RGBA8 texture, cleared to transparent, instead of the swap buffer.
- After the overlay callback, a small D3D12 command list composites that texture onto the swap buffer: decode, scale by the UI brightness (default: paper white), blend. That is one more `ExecuteCommandLists` a frame, on the same queue, before `Present`.
- `rsx_d3d12_back_buffer_format` then reports the UI texture's format, which stays RGBA8. `host.cpp` mostly stays as it is.
- Screenshots and the crash-report picture (`copy_to_staging`) must read an RGBA8 image, not the FP16 swap buffer. Either combine `render_targets[fi]` and the UI texture, or let the composite pass also write an SDR copy.

**The game's brightness setting** (docs/33: 0 to 10). It probably rewrites the 256x1 LUT or a shader constant (Inferred). Either way it acts before our mapping, so it keeps working. Advice for players: leave it at the default and set paper white instead.

**Movies** are SDR video. They get paper white like everything else.

**Fallback.** No HDR display, HDR off in Windows, or `CheckColorSpaceSupport` says no: RGBA8 and today's path, bit for bit.

### 8.4 Stage 2: highlight expansion, HUD at paper white

- **Curve.** Identity up to a knee (about 0.75 in linear), then a smooth rise so that SDR white reaches `min(peak, k x paper white)`. Apply it to luminance and keep the colour ratios. Peak defaults to `MaxLuminance`. ITU-R BT.2446's SDR-to-HDR mapping is a standard starting point (Inferred fit; not tried).
- **HUD mask.** At the AA hook (section 3.1), copy T just before the first HUD draw. Just before the display copy, write a mask wherever T differs from that copy. The blit applies the curve only outside the mask. Menus, subtitles, the lock-on reticle and the dimmed scene behind a menu then stay at paper white.
- Frames with no scene draws (title, movies, loading) get no expansion. Skip frames keep the last mask.
- **Cost** (Inferred): one 3.7 MB copy and one R8 mask pass at 720p, under 0.05 ms.

### 8.5 Native HDR (research only)

Every scene program clamps: `saturate(colour x exposure / 2)` (docs/28). Real HDR would drop the clamp for draws into T and make T `R16G16B16A16_FLOAT`. But the game's bloom, DoF, exposure adaptation and the LUT copy were all built for input between 0 and 1. The exposure would change, and the write-back would need a format conversion. 10 to 20 days, with an uncertain result. Not proposed.

### 8.6 Effort, cost and risks

| Part | Days |
|---|---|
| Detection, `IsCurrent`, SDR white level, output choice | 1 |
| Swap chain format switch through the resize path | 1 |
| Mapping in the blit; settings (`RSX_HDR`, paper white, gamma) | 0.5 |
| UI texture, composite pass, extra submit | 1 to 1.5 |
| Screenshot and crash-picture path | 0.5 |
| Tests | 0.5 to 1 |
| **Stage 1** | **4 to 6** |
| Stage 2 (curve, HUD mask, tuning) | 2 to 3 |

GPU cost (Inferred): the mapping adds a decode and a multiply to the existing blit. The UI composite is one window-sized pass, about 0.02 to 0.05 ms at 4K. FP16 buffers take twice the memory: 133 MB for two 4K buffers instead of 66 MB.

Risks:
- No HDR display to test on here (not known).
- Banding when 8-bit gradients are expanded (stage 2).
- Tearing and independent flip with an FP16 windowed swap chain were not checked (Inferred to work).
- Auto HDR or RTX HDR on top of our own HDR would map twice. The setting's help text should say so.

### 8.7 Tests (no perf A/B)

- **HDR off, or an SDR display.** The swap chain stays RGBA8 (log line), `regress.sh` is ok, and a wrapper screenshot is bit-identical to before.
- **Probe.** A knob reads back a few swap buffer pixels after the blit. Expected: black bars 0.0; white UI text paper_white / 80 (2.50 at 200 nits) within 1%; a 50% grey 0.5^2.2 x 2.5 = 0.54.
- **Switching.** Toggle Windows HDR while the game runs (Win+Alt+B). The log shows the switch, the picture continues, and there is no device removal. With two displays, move the window across.
- **Pictures.** Screenshots and the crash-report picture look right with HDR on.
- **Stage 2.** A mask-view knob tints masked pixels. The HUD, menus and subtitles are masked; no scene pixels outside the HUD are.
- **On an HDR display** (the user): at paper white = SDR white level, SDR and HDR should match in brightness. HUD white must not glare. Check sky gradients for banding.
- None of this needs the D3D12 debug layer.

## 9. Settings (proposal)

All live, all off by default, following the "original behaviour by default" rule:

| Variable | Values |
|---|---|
| `RSX_AA` | 0 off, 1 FXAA, 2 SMAA 1x |
| `RSX_OUTFILTER` | 0 bilinear (today), 1 sharp, 2 FSR1 |
| `RSX_HDR` | 0 off, 1 on when the display supports it |
| `RSX_HDR_WHITE` | 0 = the Windows SDR white level, otherwise nits |
| `RSX_HDR_GAMMA` | 0 gamma 2.2, 1 sRGB |
| `RSX_HDR_BOOST` | 0 off, 1 to 3 highlight expansion strength |

The output filter changes no game behaviour, so a default of 1 is also defensible.

## 10. What I could not determine

- The frame layout outside this one first-level frame: title, character creation, loading, cutscenes, the in-game menu, other areas. The AA rule assumes they follow the same pattern or fall under its skip conditions.
- What `0x5870000` (a second 1280x720 target with the scene depth buffer) is for.
- What the brightness setting changes: the LUT, a constant or the exposure. To check: change it, take an F9 capture, and look at the LUT the display copy samples.
- Whether T always holds display-referred values. The LUT was identity in one capture. A non-identity LUT would mean AA runs before a grade, which is still fine for AA.
- Whether `des_boot.exe` gets Auto HDR, and whether RTX HDR works with our swap chain and overlay.
- Whether this PC's display supports HDR.
- Whether `GetDesc1`'s `MaxLuminance` follows the Windows HDR Calibration app's profile.
- Real GPU costs. Every cost above is a published figure or an extrapolation.
- The FXAA 3.11 licence terms.
- Whether the PS3 version uses any AA. The surface words show no MSAA, and the post chain (ops 1821-1840) has no edge pass, so probably none (Inferred).

## Sources

- Microsoft: [Use DirectX with Advanced Color on high/standard dynamic range displays](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range).
- Microsoft: [DISPLAYCONFIG_SDR_WHITE_LEVEL](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level).
- NVIDIA: [FXAA sample](https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/fxaa.htm) (0.88 ms on a GTX 460, 0.39 ms on a GTX 560, 1920x1080).
- Wikipedia: [Fast approximate anti-aliasing](https://en.wikipedia.org/wiki/Fast_approximate_anti-aliasing) (licence note).
- [SMAA repository](https://github.com/iryoku/smaa) (MIT).
- [CMAA2 repository](https://github.com/GameTechDev/CMAA2) (Apache-2.0), and Intel's [CMAA 2.0 article](https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html).
- PCWorld: [Auto HDR deep dive](https://www.pcworld.com/article/546269/auto-hdr-deep-dive-how-and-why-to-use-windows-11s-eye-catching-game-feature.html).
- Wccftech: [RTX HDR announcement coverage](https://wccftech.com/nvidia-rtx-hdr-brings-ai-powered-hdr-to-sdr-only-pc-games-new-game-ready-driver-is-optimized-for-nightingale/).
- RPCS3 source (reference checkout): `rpcs3/Emu/system_config.h`, `rpcs3/Emu/RSX/VK/VKRenderTargets.h`, `rpcs3/Emu/RSX/VK/vkutils/device.cpp`, `rpcs3/Emu/RSX/VK/upscalers/fsr1/fsr_pass.cpp`.
