# Internal resolution scaling

Date: 2026-10-18. Status: built (`RSX_SCALE`, and Settings -> Display ->
Render resolution). "What was built" at the end says where it differs from
the plan below.

The aim is to render at a multiple of the game's 1280x720 (for example 1.5x,
2x or 3x) and show the result at window size. Aspect ratios come later, and
are covered at the end.

Today everything renders at 1280x720. `blit_to_window` then stretches that
image to the window with a linear filter. A bigger window therefore gives a
bigger image, not a sharper one.

File references are to `ps3recomp/libs/video/rsx_d3d12_backend.c` ("BE")
unless another file is named. Line numbers are approximate (+/-10) and were
taken from master on 2026-10-18.

## What renders at a fixed size today

- **The display targets.** `s_d3d.render_targets[]` are RGBA8 at the size
  given at init (1280x720; BE ~1324). They are separate from the swap chain.
  Draws to a display offset land here (`current_rt_off`, ~5410). So does any
  surface whose clip equals the display size (`RT_DISPLAY_BY_SIZE`, ~5420).
- **One shared depth buffer.** It is D24S8 at a fixed 2048x2048 (~1388). It
  is used when no zeta surface is bound.
- **Offscreen colour targets (`OffRT`, up to 16).**
  - They are keyed by RSX offset. The size comes from the guest SURFACE_CLIP
    (16 to 2048), otherwise the display size (`current_rt_off` ~5426).
  - `off_rt_get` (~5710) recreates a slot when w, h or the format change.
  - A new target is filled from guest memory at wxh (~5789).
- **Depth targets (`ZetaRT`, up to 8).** They are keyed by offset and only
  grow (`zeta_get`, ~5649).
- **Viewport.**
  - The vertex shader epilogue uses only z from the guest's viewport
    scale/offset; x and y pass straight through (BE ~5306,
    rsx_vp_decompiler.c ~645).
  - The guest viewport rect from NV4097_SET_VIEWPORT_* is set as the D3D12
    viewport. The scissor is set to the same rect (~8220).
  - The guest scissor is stored but never applied (rsx_commands.c ~929).
- **Clears** are always full-surface (~8177-8206).
- **WPOS** is the raw `SV_Position` (rsx_fp_decompiler.c ~370). No constant
  carries the target size, a Y flip or the pixel centre. SHADER_WINDOW is not
  decoded.
- **Texture sampling.**
  - Shaders use normalized UVs with `Sample()`; there is no `Load()` and no
    `GetDimensions()`.
  - Unnormalized textures are scaled by `rsx_texscale = 1/guest size`
    (~5163), which comes from the texture registers, not the host surface.
- **Copies.**
  - `screen_copy_capture` (~6291): the whole display target goes into
    `s_screen_copy`, at display size.
  - `subvp_capture` (~577, off by default): boxes of the display target.
  - `off_rt_snapshot` (~6221): a same-size copy, made when a draw samples
    the target it renders to.
  - `composite_present` (~6406).
- **Colour write-back (the exposure path).**
  - `rt_writeback_record` (~5959) copies every used OffRT whole into a
    readback buffer.
  - `rt_writeback_store` (~6028) then converts it on the CPU into the guest
    layout: linear with the guest pitch, or swizzled. It assumes host size
    equals guest size.
  - This is how the game reads back its luminance. docs/28 covers why it
    matters: without it the scene is too dark or too bright.
- **ZPASS occlusion queries** (`zq_*`, ~6648-6697) report raw sample counts.
  The sun flare uses them (docs/29 Y2).
- **Present.**
  - The swap chain is created at the init size and resized on WM_SIZE
    (`swap_resize`, ~1105).
  - The guest image reaches the swap chain through a full-screen triangle in
    an aspect-fit viewport (`blit_to_window`, ~1051).
  - The wrapper UI draws on the swap buffer at window size.
- **Not decoded at all:** point size, line width, polygon offset and MSAA.
  Nothing there needs scaling.

## How RPCS3 does it

Paths are relative to `rpcs3/Emu/` in the RPCS3 source.

- **Setting.** `resolution_scale_percent` runs from 25 to 800 (default 100).
  `min_scalable_dimension` defaults to 16 (system_config.h ~166-169). The
  scale is re-checked at each flip, and surfaces are re-cloned at the new
  scale (`surface_store::sync_scaling_config`).
- **The rule** (`apply_resolution_scale`, RSX/Utils/rsx_utils.h ~364):
  - A surface is scaled only if its larger side is above
    `min_scalable_dimension`, so small targets such as a 16x16 luminance
    target stay at 1x.
  - Sub-rects follow the decision made for their parent surface.
- **Surfaces** keep both sizes: the host image is scaled, and the guest
  width, height and pitch are kept alongside it (VK/VKRenderTargets.h).
- **Viewport.**
  - The D3D-style viewport is the scaled surface clip.
  - The guest viewport goes into the vertex shader as a scale/offset matrix
    normalized by the unscaled clip, so it does not depend on the resolution
    (Core/RSXDrawCommands.cpp ~563).
- **Scissor.** It is guest scissor intersected with viewport, then scaled (RSXThread.cpp
  ~1925).
- **WPOS.** `wpos_scale = +/-1/scale` and `wpos_bias` (window height for a
  bottom origin, -0.5 for integer centres) are applied to `gl_FragCoord` in
  the fragment prologue (RSXDrawCommands.cpp ~693; RSXFragmentPrologue.glsl
  ~40).
- **Readback** (write colour buffers). If the host image is larger than the
  guest surface, RPCS3 first scales it down to a native-size helper image
  (linear for colour, nearest for depth), then copies that to the buffer
  (VK/VKTextureCache.h ~231).
- **Memory to surface.** Upload at native size, then scale up
  (VKRenderTargets.cpp ~695-789).
- **Sampling a scaled target.** Normalized coordinates, plus a per-texture
  `texcoord_xform` for sub-rects (Common/texture_cache.h ~425).
- **Point size and line width** are multiplied by the scale. Polygon offset
  and ZPASS counts are not.

## Plan

Each step keeps scale 1 bit-identical to today's output (check with
`tools/des/regress.sh` and `tools/des/capdiff.py`). The knob is
`RSX_SCALE=<percent>`, read once at init. A Display row in the wrapper UI's
Settings comes last; it is a Restart setting because targets are sized at
init.

### Step 0: find where the scene is drawn (half a day)

Find out which targets the 3D scene draws into: the display target, or an
offscreen target that a post pass then copies to the display. Use
CELLMARK_DUMP and the draw fingerprints from F9 (docs/28).

The answer decides how much step 1 alone gains. If the scene is drawn into
an OffRT, step 1 sharpens only the HUD and the last pass.

### Step 1: scale the display targets (2 to 3 days)

- Create `render_targets[]` and the shared depth at `scale x size`. The
  shared depth becomes `2048 x scale`, capped at 16384.
- Keep the guest size in `s_d3d.width/height`, and add
  `s_d3d.host_w/host_h`. Every check that compares a guest size with the
  display size must keep using the guest size:
  - `RT_DISPLAY_BY_SIZE`
  - the screen-as-texture match (~7722)
  - `subvp_note`
  - the `composite_present` clamp
- Viewport: scale the guest viewport rect when the target is scaled. Because
  x and y pass straight through the vertex shader, scaling the D3D rect is
  enough; no shader change is needed.
- Scissor: scale the same rect.
- WPOS: add a fragment constant `wpos_scale = 1/scale` (1 for 1x targets)
  and emit `position.xy * wpos_scale` for input 0 in rsx_fp_decompiler.c.
  - This changes the shader text, so it must go in the PSO/shader cache key.
    Using a constant keeps one shader for all scales.
  - Look for WPOS users first: if no DeS shader reads it, this can wait.
- `s_screen_copy` and `subvp` captures at host size. Samplers use normalized
  UVs, so the shaders don't change.
- Present needs no change: `blit_to_window` already scales the display
  target into the window, and now it scales down or 1:1 instead of up.
- ZPASS: divide the counts by scale^2 (rounding up, and never 0 when the host
  count was non-zero), so the flare's visibility thresholds see the same
  numbers as at 1x.
- Debug readbacks (dumps, F9 captures, `RTT_SAVERT`) can run at host size.
  `capdiff.py` then only compares captures taken at the same scale.

### Step 2: scale the offscreen targets (4 to 6 days)

This step matters if step 0 shows the scene is drawn offscreen.

- `OffRT` and `ZetaRT` keep their guest size (`gw/gh`) next to the host size
  (`w/h`). Use RPCS3's rule: scale only if `max(gw, gh) > 16`. The 16x16
  exposure target, and any 1x1 to 16x16 reduction chain, stays 1x.
- Raise the 2048 cap on host sizes to `2048 x scale` (D3D12's limit is
  16384).
- **Fill from guest memory** (~5789): upload at the guest size into a 1x
  staging texture, then draw it scaled into the target. One full-screen
  triangle pass is enough; `blit_init` can be reused.
- **Write-back** (`rt_writeback_record`): for a scaled target, first draw a
  linear downscale into a 1x scratch target of the same format, then copy
  that to the readback buffer. `rt_writeback_store` stays as it is, since it
  sees guest-size data. Depth has no write-back today, so there is nothing
  to do there.
- `off_rt_snapshot`, and sampling a target as a texture, need no change:
  same-size copies, normalized UVs.
- Depth-as-colour (`d2c`) reads the zeta at its host size. That's fine
  because the UVs are normalized.
- Memory: at 2x, every scaled target takes 4 times the memory. Sixteen
  RGBA16F targets at 2560x1440 come to about 470 MB, which is acceptable. Log
  the total once at the first frame.

### Step 3: settings and checks (1 day)

- Add a "Render resolution" row in Settings -> Display: 100 / 150 / 200 /
  300%, as a Restart setting. Change it to Live later if targets can be
  recreated on a frame boundary, as RPCS3 does at each flip.
- Check at 200% in the Palace, the Nexus and Boletaria: shadows (ZetaRT),
  exposure (write-back), the sun flare (ZPASS), fog walls, particles, the
  HUD and FMVs (the cellSail frames are uploaded at their own size).
- Watch the frame rate: at 2x the GPU fills 4 times the pixels. Measure uncapped
  (VBLANK_MHZ=240000), with alternating A/B runs.

## Risks

- Shadow maps are zeta targets sized by the game (often square and above
  16), so they get scaled too. That gives sharper shadows, but any shader
  that maps by texel size through a constant, rather than through the
  texture, would be off. Nothing like that has been found (no `Load()`), but
  test it early.
- Bloom and blur passes made of chains of half-size targets will scale with
  the rest. Their kernels are in UV units, so they stay the same size on
  screen.
- `rsx_live_draw.c` (Track B, opt-in) has its own swap chain and blits. It
  stays at 1x; the knob applies to the D3D12 backend only.

## Aspect ratio, afterwards

RPCS3's DeS patches change the projection's aspect ratio (bef32 at
0x018CEFBC and 0x0190433C) and centre the HUD for 21:9 and 32:9 (0x018F0174,
0x00718CFC, 0x00718D68). The memory note `demons-souls-rpcs3-patches` has
them.

On our side:
- the display target becomes `guest_h x aspect` wide (for example 1720x720
  for 21:9) times the scale
- `blit_to_window` letterboxes against the new aspect
- the game's floats are written once at boot, from a des_options hook

This is independent of scaling, but it needs step 1's split between guest
size and host size.

## What was built

The code is in ps3recomp commits on `feat/rsx-scale`: the offscreen
targets, the display buffers, the ZPASS counts, display-only batches, the
write-back of scaled targets, the live knob, texture filtering and sprite
edges. This repo adds the Settings rows: Display -> Render resolution, 100 /
150 / 200 / 300%, a Live setting that writes `RSX_SCALE`, and Texture
filtering and Anisotropic filtering (below).

### Step 0: where the scene is drawn

Read with `RT_OFFDBG` at 100% in the tutorial level. The scene is drawn
offscreen, so steps 1 and 2 were done together; step 1 alone would only
have sharpened the HUD.

- Scene: colour targets 0x7A0000 and 0x5870000 (1280x720), depth 0xB50000.
- Shadow map: depth-only zeta 0x4860000, 2048x2048.
- Post: 0x4260000 (1280x720), then a bloom chain at 640x360, 320x180 and
  160x90.
- Exposure: 64x64 and 16x16 targets, which the game reads back.
- Two 128x128 targets, 0x17B0000 and 0x17D0000.
- Display buffers 0x10000 and 0x3D0000 receive the last pass and the HUD.

### Where it differs from the plan

- **Threshold.** A target is scaled only if its larger side is above
  `RSX_SCALE_MIN` (default 64), not 16. This keeps the 64x64 and 16x16
  exposure targets at 1x, so their write-back is unchanged.
- **Fill from guest memory.** `off_rt_get` expands the guest image
  nearest-neighbour on the CPU, into the upload buffer, instead of a GPU
  pass. It only runs when a target is created.
- **Write-back.** Scaled targets are written back too, as the plan said.
  `off_rt_downscale` draws each one down to a guest-size copy (one texel per
  guest pixel, `Load`), and that copy is read back as a 1x target is. A first
  version skipped them, since every target the game reads is under the
  threshold, but the guest memory under a scaled target then went stale. All
  16 offscreen slots are in use in menus (this happens at 1x as well:
  "[RT] all 16 offscreen targets in use"), so targets are recreated often,
  each filled from guest memory. At 200% the Profile List showed coloured
  noise below its footer, where the menu draws nothing over a new target.
- **Display-only batches.** A batch starts on the display buffer and may
  never change target (the loading screens). Its draws must use the display
  buffer's scale; with a 1.0 default they covered the top-left quarter on
  every other frame, and the loading screens flickered between a small and
  a full-size image.
- **Live.** `RSX_SCALE` is a live knob. When it changes, the next batch
  (GPU idle, last write-back stored) drops everything sized from it
  (`rsx_scale_apply`): offscreen colour and depth targets, display buffers,
  the shared depth buffer, the screen copy and readbacks. Colour targets
  refill from guest memory. The step 3 note about RPCS3 recreating at a flip
  is what this does.
- **WPOS.** No constant was added, because no Demon's Souls fragment program
  read WPOS in the test runs. With `RSX_SCALE` set, the decompiler logs once
  if one does ("a fragment program reads WPOS, which RSX_SCALE scales").
- **ZPASS.** Each query's count is divided by its target's host/guest pixel
  ratio, rounded up. Not seen live: the tutorial level and Stonefang issue
  no queries, so the sun probe has not been checked at a scale.
- **Scissor.** It is still the viewport rect, scaled with it. The guest
  scissor is still not applied.
- **Range.** `RSX_SCALE` takes 50 to 800 (percent). Host sizes are capped at
  16384.
- The guest size is kept for everything the game compares: texture sizes
  against the display, `RT_DISPLAY_BY_SIZE`, `subvp_note`, the guest
  viewport and the window's aspect in `blit_to_window`. Copies, captures and
  frame dumps use the host size, so `capdiff.py` and `shotdiff.py` only
  compare sets taken at the same scale.

### Results

- At 150%, 200% and 300% the opening cutscene and gameplay look as they do
  at 100%, only sharper: HUD, exposure, shadows, bloom and fog. Frame dumps
  come out at 1920x1080, 2560x1440 and 3840x2160. There were no device
  errors.
- Frame rate in the tutorial level (DES_FPS=240, VBLANK_MHZ=240000, an
  RTX 5070 Ti): 240 at 100% (the cap), 240 at 150%, 236 at 200%, 226 at
  300%.
- At 100% every size and ratio is the guest's, so the output is the same as
  before.
- Switching live in the tutorial level, 100 -> 200 -> 100 -> 300 -> 150%:
  each frame after a switch is complete and at the new size, with no device
  errors.
- Not checked yet: the Nexus, Boletaria (the sun flare), FMVs and other
  areas at a scale.

### Lines along HUD and menu edges

At 200% thin lines ran along panel edges and between the pieces of framed
buttons. The menus are quads cut from texture atlases. At 1x a quad drawn
1:1 puts each pixel centre on a texel centre, so linear filtering never
reaches past the sprite. At 2x the outermost pixel centre is a quarter texel
from the edge, and the filter blends in the next sprite in the atlas. A
diagnostic that forced nearest filtering removed the lines; snapping 2D
vertices to the guest pixel grid did not, so overlap was not the cause.

The fix (`dr_uvclamp`, `uvclamp_gs`, `fp_inject_uvclamp`): a draw without a
depth test on a scaled target gets a geometry shader that passes each
triangle's texture-coordinate bounds (TEXCOORD0-7, xy) to the pixel shader
as flat inputs, for triangles whose vertices all have w = 1. The pixel
shader keeps each coordinate half a guest pixel inside the bounds (fwidth
times half the scale), where the 1x edge pixel samples. Both triangles of a
quad carry the quad's bounds, so the diagonal is not clamped, and interiors
are unchanged. 3D draws (depth-tested) and 1x targets do not get it.
`RSX_SCALE_UVCLAMP=0` turns it off.

Checked on the System menu at 200%, downscaled and compared with 100%: the
seam columns at the button joints and in the footer are gone. At DES_FPS=240
uncapped it costs nothing measurable (240 fps either way).

### Texture filtering

Asked for with resolution scaling, and independent of it:

- **Anisotropic filtering.** The game's own level (TEXTURE_CONTROL0 bits
  4-6) was ignored and every sampler used `MaxAnisotropy = 1`. Demon's
  Souls asks for 4x on its trilinear surface textures, and that is now
  applied by default, as RPCS3 does. `RSX_ANISO` (1-16, live) replaces it.
  It applies to mipmapped units whose min and mag filters are linear, never
  to depth comparison. The regression set at 100% still passes.
- **Filter override.** `RSX_TEXFILTER` (live): 1 nearest, 2 bilinear, 3
  trilinear. It changes only the units the game samples with mipmaps (min
  filter 3-6), which are its surface textures. Colour-grading LUTs, render
  targets and other data are sampled without mipmaps and keep the game's
  filter, because forcing it there changes colours: a LUT read nearest bands,
  and one read linear where the game reads it nearest blends entries.

Both are part of the sampler-table key, so a change makes new tables.
