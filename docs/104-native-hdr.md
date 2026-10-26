# 104. Native HDR output (2026-10-24)

docs/103 measured what the game has above white and proposed two stages: HDR output (Stage A) and the game's own
highlights (Stage B). This doc is what was built: both stages, off by default. Labels: **measured** (a run says so),
**read** (seen in the code or a shader), **inferred**.

## Short answer

- **Settings, Display, HDR: On.** It needs HDR turned on in Windows for the display the window is on. Turn off RTX HDR
  and Auto HDR for the game while it is on.
- **What changes** (measured in the Tutorial, section 4):
  - Shadows use gamma 2.2, as on a TV, instead of the lighter sRGB curve Windows gives SDR windows.
  - White sits at Windows' SDR white level ("SDR content brightness").
  - The game's own highlights go above white, up to the display's peak: openings to the sky, the lantern.
    That is about 1% of the picture when they are in view, and none in most views.
- **With HDR off, or on an SDR display, nothing changes.** The swap chain stays RGBA8 and every pipeline is the one it
  was.
- On this PC's display (280-nit peak, 240-nit white) there is little room above white. Most of the visible change is the
  shadows. A brighter HDR display shows more of the highlights.

## 1. Settings

| Row | Knob | Values |
|---|---|---|
| HDR | `RSX_HDR` | 0 off (default), 1 on. Live. |
| HDR brightness | `RSX_HDR_WHITE` | Paper white in nits. 0 (default) = Windows' SDR white level for that display, 200 if it cannot be read. |
| HDR peak | `RSX_HDR_PEAK` | Nits. 0 (default) = the display's own peak, 1000 if it cannot be read. |
| HDR shadows | `RSX_HDR_GAMMA` | 0 gamma 2.2 (default), 1 the sRGB curve (lighter, as Windows shows SDR). |
| (none) | `RSX_HDR_SCENE_FP` | `fp[:limit],...`: the program that clamps the finished scene, and its new limit. The port sets `B42C01:2` (run default). Empty = Stage A only. |
| (none) | `RSX_HDR_SNAP` | A file path: the swap chain buffer once, raw (an `HDRP w h` line, then RGBA halves), plus a log line with the brightest value and the share above paper white. Debug. |

All of them apply live. The runtime asks the display again when the window is activated, the display setup changes or
the window moves to another display.

## 2. Output (Stage A)

In ps3recomp `libs/video/rsx_d3d12_backend.c`, "HDR output" section (read):

- **Display:** `IDXGIOutput6::GetDesc1` of the output the window is on. HDR is on when its colour space is
  `G2084_NONE_P2020`; `MaxLuminance` is the peak. The SDR white level comes from `DisplayConfigGetDeviceInfo`
  (type 11, SDR white level).
- **Swap chain:** while HDR is wanted, `ResizeBuffers` to `R16G16B16A16_FLOAT` and colour space scRGB
  (`G10_NONE_P709`: linear, 1.0 = 80 nits). If the swap chain refuses scRGB, it goes back to RGBA8 and stays SDR until
  the display is asked again.
- **The blit** (the game's frame scaled into the window) gets its own pipelines, bilinear and Sharp, with one mapping
  after the sample:
  1. Add what T holds above 1 (Stage B, section 3).
  2. Decode with gamma 2.2, or the sRGB curve.
  3. Above a knee, roll the brightest channel off towards the peak, keeping the colour's ratios:
     `m' = k + r (1 - exp((k - m) / r))`, with `r = peak/white - k`.
     The knee `k` is 0.8 x peak/white when peak/white is 1.25 or less, else max(1, 0.6 x peak/white).
     On this PC (280 / 240) white itself lands at 238 nits.
  4. Multiply by white / 80.
- **The menus** (port `ui/src/render_d3d12.cpp`): on an FP16 back buffer the UI's finished layer goes through a
  program of its own (`kHdrOut`) that decodes the same way and scales by the same paper white, so menus match the
  game. The runtime tells the UI the scale (`rsx_d3d12_hdr`). Read-backs (UI screenshots, the problem report's picture)
  convert the FP16 buffer back to 8-bit SDR.

## 3. The game's own highlights (Stage B)

How the game builds its picture (docs/103 section 2, read): the scene target T (0x7A0000, RGBA8) holds colour x
exposure / 2, so its own limit is 2x white. The composite (fp 0xB42C01) reads the depth-of-field result, doubles it,
clamps it to 1, adds bloom, and writes T again. The display copy (fp 0x181) then reads T through a 256-entry table into
the display buffer.

What the runtime does while HDR is on and `RSX_HDR_SCENE_FP` names a program (read):

1. **Finds T from guest state:** the largest target the frame's display copy samples, at display size. It also finds
   each target T is copied into after the named program (Demon's Souls: 0x4260000, motion blur copies T out and blurs
   it back). What one batch finds applies from the next, since formats are chosen at a batch's start.
2. **Makes them FP16** (`rt_dxgi`). They still go back to guest memory in the title's format (`OffRT.gdxgi`, through
   the downscale pass).
3. **The named program, drawing into T, clamps at its limit instead of 1:** a pipeline variant whose
   `_v = saturate(_v)` becomes `clamp(_v, 0.0, 2)`.
4. **A draw into any other target reads T and its copies clamped to 1**, as their 8-bit format held them: a variant
   that wraps those units' samples in `saturate()`. Draws into T or its copies read them whole.
5. **The blit adds `max(T - 1, 0)`** to the display copy's picture, which read T clamped to 1.

Step 4 matters. Without it, the display copy looked T up in its table with a sampler that wraps: a T of 1.3 read the
entry for 0.3. The sky came out at white or darker, and only pixels where T passed 2 showed as highlights (measured:
0.025% of the picture instead of 0.97%). Four programs get a variant in the Tutorial (measured, log): the composite (one
clamp raised), and the display copy 0x181, the copy 0xB40081 and the downsample 0xB41801 (one sample clamped each).

The log says what was found and built:

```
[D3D12] HDR: on (paper white 240 nits, peak 280 nits, gamma 2.2)
[D3D12] HDR: the game's highlights on
[D3D12] HDR: FP16 scene target 0x7A0000, copies 0x4260000 0x0 0x0 0x0
[D3D12] HDR: fp 0xB42C01's variant, 1 clamp raised
[D3D12] HDR: fp 0x181's variant, 1 sample clamped
```

## 4. Results (measured, 2026-10-24)

Tutorial, regression save, 1280x720, this PC's Dell (HDR on, 280-nit peak, 240-nit SDR white):

| View | Above paper white | Brightest | Notes |
|---|---|---|---|
| Spawn (no sky in view) | 0% | 238 nits | white rolled off to 238 |
| Looking up at the sky opening, lantern lit | 0.97% | 280 nits (the peak) | 99.9th percentile 280 nits |
| Same view, `RSX_HDR_PEAK=1000` | 0.97% | 923 nits | 99.9th percentile 558 nits |

- T at the blit (debug snapshot): 0.97% above 1, brightest 2.13. That matches the probe right after the composite in
  docs/103, so nothing after the composite loses the highlights.
- The picture converted back to SDR matches the HDR-off frame. The sky opening and the lantern are the only areas
  above white.
- HDR off and on again while playing: the swap chain switches each time, and the highlights come back with it.
- The settings window on the FP16 back buffer: same look as SDR once converted back.
- Frame time (`tools/des/knobab.sh`, `RSX_HDR=1` switched live, Tutorial, uncapped): 493 fps off, 485 fps on,
  -1.5% or 0.03 ms a frame, p 0.39 (under the tool's 1.5% bar). The first window with HDR on includes the one-time
  pipeline builds (section 7). T and one copy are FP16 (twice the bytes of RGBA8) while HDR is on.

## 5. Limits and differences

- **No highlights on held frames:** while a menu is open the overlay repaints the held frame, which has no T to add.
  The picture under the menu is the SDR-range one.
- **Draws into T before the composite are no longer clipped at T's 8-bit limit** when they blend past it (additive
  effects). The composite still clamps at 2, so the result stays inside the original's 2x range; blurred highlights
  can come out slightly wider (inferred).
- **In-game brightness** (Options, Brightness, 0 to 10, default 5) is the display copy's table, which applies up to
  white. It bends the curve between black and white and keeps both ends; the highlights above white are added after
  it and do not change with it (measured, section 7).
- **Linux:** no HDR. vkd3d's swap chain has no colour-space methods, so the display always reads as SDR there.
- **The D3D11 menu renderer** ignores HDR. Windows uses the D3D12 one.
- **Other titles:** Stage A works for any title. Stage B needs the title's scene program named in
  `RSX_HDR_SCENE_FP`; without it nothing is promoted.

## 6. How it was tested

- Scratch runner (not committed): boots the regression save into the Tutorial, skips the opening cutscene, turns the
  camera with `PAD_STICK`, and takes `RSX_HDR_SNAP` snapshots. A small script reads a snapshot: nits percentiles, the
  share above paper white, an SDR preview and a map of the pixels above white.
- Debug knobs used to find the table problem: `FP_SHOW` on the composite (a constant 1.5 everywhere still showed
  nothing above white), `FP_KILL` on the motion blur write-back, `FP_PICK` for the HLSL of the copies (no colour clamp
  in them), and a snapshot of T itself.
- HDR off: the regression route (`tools/des/regress.sh`) against the accepted set.

## 7. Follow-ups (2026-10-24, later)

### FXAA with HDR

FXAA (`RSX_AA=1`) used to be skipped while HDR kept T in FP16, because its pass had an RGBA8 pipeline only. It now has
an FP16 pipeline as well and runs on T in the same place as in SDR: after the post chain, before the HUD. The filter
works on values above 1 as it does below.

Measured (Tutorial, regression save, `RSX_HDR=1 RSX_AA=1`): the log says `[AA] FXAA on T=0x007A0000 1280x720` where it
used to say the scene target was not RGBA8. With FXAA switched off live between two snapshots of the same view, the
difference is along edges (the character's outline, the arch, the textured floor), the HUD is untouched, and the
brightest value is the same.

### Switching HDR on, and the pipeline index

Turning HDR on asks for pipelines the SDR picture never uses. In the Tutorial (measured): 91 rows, 81 of them draws into
T and its copy, which are FP16 now (the HUD is drawn into T too), and 10 HDR variants (the composite's raised clamp
and 9 draws whose reads of T are clamped). None were in the pipeline index, so each was built the moment HDR came on.

The index rows now carry the variant (an optional 15th field, `hdrv`, docs/97), the start-up pass makes the variant's
text, and the 91 rows are in the index. Measured on a shader cache without its pipeline library, HDR on from the start:

| Index | Built at the start | Draws that waited for the driver |
|---|---|---|
| Before (658 rows) | 658 | 91 |
| With the HDR rows (749) | 749, in 0.5 s | 0 |

Other areas will add rows of their own (menus and effects drawn into T); merge the seen files of HDR play as usual.

### In-game brightness

The display copy's table (local memory 0x3E0, 256 entries), read with the debug console at three settings of Options,
Brightness (measured; input and output out of 255):

| Input | 0 | 5 (default) | 10 |
|---|---|---|---|
| 16 | 1 | 16 | 33 |
| 64 | 20 | 64 | 92 |
| 128 | 72 | 128 | 154 |
| 192 | 152 | 192 | 207 |
| 255 | 255 | 255 | 255 |

- 5 is the identity: the game as it was made.
- Above 5 it lifts shadows and mid-tones (about gamma 0.74 at 10); below 5 it darkens them (about gamma 1.84 at 0, with
  the darkest shades crushed to black).
- Black and white stay where they are, so with HDR the picture still meets paper white at the same point, and the
  highlights above it are added unchanged. Raising the setting makes the scene brighter but the highlights stand out
  less.
- With HDR on, the HDR rows are the better tools: HDR brightness (paper white) scales the whole picture, highlights
  included, and HDR shadows (the sRGB curve) lifts only the darkest shades.
