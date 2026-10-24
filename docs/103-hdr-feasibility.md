# 103. HDR: what the game has to give, and what it would take (2026-10-24)

Can the port output HDR, and would it look better? docs/43 section 8 planned HDR output in 2026-10-18 without
measuring anything. This doc measures how much brightness the game's own picture has above white, and updates that
plan. Labels: **measured** (a run says so), **read** (seen in the code, a shader or a frame capture), **inferred**.

## Short answer

- **Feasible.** Recommended order:
  1. **Now, no code: NVIDIA RTX HDR** (section 5.1). Tried on this PC on 2026-10-24: it works with the port and
     "looks quite nice". For players with an RTX GPU this is the answer today.
  2. **Stage A, HDR output** (section 5.2, 3.5 to 5 days): correct shadows and a paper-white setting. The game
     otherwise looks as it does today.
  3. **Stage B, the game's own highlights** (section 5.3, 2.5 to 4 days): the brightness the game already
     computes above white, which it throws away today.
  4. **Not proposed:** docs/43's stage 2 (guessing highlights from the SDR picture, with a HUD mask) and its native
     HDR (dropping the scene programs' clamp). Stage B makes the first unnecessary, and the second gains almost
     nothing (measured, section 3).
  5. All off by default. With HDR off, or on an SDR display, the picture stays bit for bit as today.
- **Demon's Souls is a dark game** (measured): the middle pixel of a frame sits at 3 to 18% of white.
  - Above white is rare: under 0.02% of the picture in most views.
  - It reaches about 1% when an opening to the sky or a lantern is in view.
  - The brightest pixels reach 2x white as a signal, about 4.6x white as light.
- **On an HDR desktop today, the shadows are too bright** (read and inferred, section 4). Windows shows the
  game's SDR picture with the sRGB curve, which makes the game's typical dark levels 1.6 to 4.5 times brighter than
  a gamma 2.2 display would. Stage A fixes this. On this PC it is probably the most visible change.
- **This PC's Dell has little room above white** (measured, section 1): a 280-nit peak against a 240-nit SDR white
  level. Highlights can be at most 1.17x brighter than white at today's setting, or 1.75x with the game's white at
  160 nits. A 600 to 1000-nit display would show much more.

## 1. This PC's displays (measured)

Read with `IDXGIOutput6::GetDesc1` and `DisplayConfigGetDeviceInfo` (two small scratch probes, section 8).

| | Dell S2721DGF (main) | SyncMaster |
|---|---|---|
| HDR | supported, on | not supported |
| Bits per colour | 10 | 8 |
| Luminance (DXGI) | min 0.035, max 280, full-frame 280 nits | max 270 nits |
| Primaries | close to DCI-P3 (R 0.686,0.309; G 0.265,0.668; B 0.150,0.058) | sRGB |
| SDR white level (the "SDR content brightness" slider) | 240 nits | none |
| Refresh | 143.91 Hz | |

GPU: NVIDIA GeForce RTX 5070 Ti.

## 2. How the game builds its picture

From a frame capture in the tutorial (`RSX_CAP`, 793 ops), the composite program's HLSL (`FP_PICK`), and docs/28 and
docs/43.

| Op | Target | What |
|---|---|---|
| up to 669 | T = `0x7A0000` (RGBA8) | the scene. Each scene program ends `saturate(colour x exposure / 2)` (docs/28) |
| 670, 694 | `0x5870000` | copies of T (fp `0xB40081`) |
| between | `0x4260000` (1280x720, RGBA8), 160x90 targets | effects, depth of field, bloom, luminance and exposure |
| 713 | T, RGB | composite, fp `0xB42C01`: reads `0x4260000` (the depth-of-field result) and `0x4242840` (160x90 bloom) |
| 714, 715 | `0x4260000`, then T | motion blur (fp `0xB41181` copies T out, fp `0xB40D01` blurs it back) |
| 716 to 792 | T | the HUD, blended |
| 793 | the display buffer | display copy, fp `0x181`: T through a 256x1 LUT at local memory `0x3E0` |

What this means:
- **T holds half the display value** (read). The scene stores colour x exposure / 2. The composite's HLSL is:

  `out = M x (saturate(dof x c0.z) + bloom x c1.x) + c5` (`M` a colour matrix from constants 2 to 4)

  `c0.z` is the factor 2 back (inferred: the snapshot of T times 2 looks like the game's picture, and the composite's
  output median matches it, section 3).
- **So the game keeps 2x headroom and then clips it** (read). In T and in the RGBA8 targets after it, display white
  is 0.5 and the target's own clip at 1.0 is twice white. The composite's `saturate` cuts everything above white,
  and only then adds bloom.
- **The exposure keeps the average near a 0.18 grey** (docs/28: ToneMapBank key 0.18). Measured in the Nexus: the
  middle pixel is at 0.180 of white.
- **The LUT is an identity ramp** (measured: all 256 entries read from guest memory in the tutorial at the default
  brightness, entry i = (255, i, i, i)). docs/28 found the same in a Nexus cutscene. Whether the game's brightness
  setting rewrites it was not checked.

## 3. How much is above white (measured)

A scratch probe build (section 8) made T `R16G16B16A16_FLOAT` and took snapshots. Values are in display units (1.0
= display white). "Above 2x" is lost even in the original's 8-bit target.

**The scene alone**, before the first post op, with the scene programs' clamp dropped:

| View | Middle pixel | Mean luminance | Above 1x | Above 1.5x | Above 2x | Brightest |
|---|---|---|---|---|---|---|
| Nexus, candles and the statue | 0.180 | 0.185 | 0.044% | 0.008% | 0.002% | 2.30 |
| Tutorial cutscene, lit player in fog | 0.058 | 0.122 | 1.034% | 0.020% | 0 | 1.78 |
| Same cutscene, a later frame | 0.074 | 0.163 | 0.158% | 0.037% | 0.014% | 2.28 |
| Tutorial tunnel, start | 0.034 | 0.043 | 0 | 0 | 0 | 0.76 |
| Tunnel, camera tilted | 0.030 | 0.038 | 0.005% | 0 | 0 | 1.37 |
| Tunnel, looking up at an opening to the sky | 0.037 | 0.054 | 0.991% | 0.026% | 0 | 2.00 |
| After a short walk | 0.035 | 0.077 | 0.007% | 0 | 0 | 1.41 |
| Turned around | 0.046 | 0.080 | 0.001% | 0 | 0 | 1.32 |

**After the composite** (effects, depth of field and bloom included), with the composite's clamp dropped. Same
save, same camera moves, a second run:

| View | Middle pixel | Mean luminance | Above 1x | Above 1.5x | Above 2x | Brightest |
|---|---|---|---|---|---|---|
| Tutorial cutscene | 0.078 | 0.108 | 0.016% | 0.005% | 0 | 1.89 |
| Tunnel, start | 0.039 | 0.064 | 0 | 0 | 0 | 0.93 |
| Tunnel, camera tilted | 0.031 | 0.044 | 0.004% | 0 | 0 | 1.37 |
| Looking up at the opening | 0.039 | 0.071 | 0.973% | 0.045% | 0.025% | 2.12 |
| After a short walk | 0.047 | 0.082 | 0.007% | 0 | 0 | 1.43 |
| Turned around | 0.055 | 0.083 | 0 | 0 | 0 | 0.89 |

What it means:
- **Highlights are small and rare**: candle flames, the player's lantern, an opening to the sky, lit edges of
  armour. Most views have under 0.02% of the picture above white.
- **The bright fog at the tunnel's exit is not a highlight.** It stays under white (0.93) even with bloom added.
- **Above the 8-bit target's clip there is almost nothing** (0 to 0.025%). Dropping the scene programs' clamp
  (docs/43 section 8.5) would gain nothing visible.
- **As light**, if the signal is gamma 2.2, 2x white as a signal is 2^2.2 = 4.6x white as light.
- **Not measured**: sunlit outdoor areas (Boletarian Palace 1-1), fire, spells, the Shrine of Storms. The trip to
  1-1 needs a warp that only an old scratch build has. These areas may have more above white.

## 4. Shadows on an HDR desktop today (read and inferred)

With Windows HDR on, Windows shows an SDR window's picture with the piecewise sRGB curve at the SDR white level
(Microsoft's Advanced Color page; docs/43 section 8.1). The game was made for TVs, which are closer to gamma 2.2 or
darker. In the dark range where Demon's Souls lives, the two differ a lot:

| Signal | sRGB curve, nits at 240 | Gamma 2.2, nits at 240 | sRGB is brighter by |
|---|---|---|---|
| 0.02 | 0.37 | 0.04 | 8.5x |
| 0.034 (tunnel's middle pixel) | 0.63 | 0.14 | 4.5x |
| 0.05 | 0.94 | 0.33 | 2.9x |
| 0.10 | 2.41 | 1.51 | 1.6x |
| 0.18 (the grey key) | 6.53 | 5.52 | 1.2x |
| 0.50 | 51.4 | 52.2 | about the same |

So today, on this PC's HDR desktop, the game's shadows look lifted and grey (inferred from the curves; not compared
on the screen). Stage A decodes with gamma 2.2 by default (sRGB as an option) and fixes this.

## 5. Options

### 5.1 No code: RTX HDR, Auto HDR

- **RTX HDR** (NVIDIA app; read: NVIDIA's announcement and secondary reports):
  - Needs an RTX GPU, Windows HDR on, and Auto HDR off for the game. The NVIDIA app turns it on per program or
    with Alt+Z, Game Filter.
  - Works with DX12. Multi-monitor support came in the NVIDIA app with driver 565.90 or newer, for an extended
    desktop with an HDR main display and an SDR side display. That is this PC's setup.
  - Borderless windows work in some games and not in others.
  - It brightens the HUD as well, and costs some frame rate.
  - It also maps the shadows with its own curve, so it may fix section 4 by itself.
  - **Tried with the port** (2026-10-24, this PC: RTX 5070 Ti, the Dell with Windows HDR on): it works with `des_host.exe` and "looks quite nice" (the user). Frame rate, shadows and the HUD were not
    measured separately.
- **Auto HDR**: Windows decides which DX11 and DX12 games get it. There is a per-app off switch (Settings, Display,
  Graphics) but no way to force it on. Not tried.
- Either one sets the bar that Stages A and B must beat. Never stack them on top of Stages A and B: the picture
  would be mapped twice.

### 5.2 Stage A: HDR output (docs/43 section 8.3, updated)

What changed since docs/43:
- **The wrapper UI is D3D12 now** (port #107), so the D3D11On12 parts of docs/43 are gone.
- **The UI draws straight onto the swap buffer** in a fixed RGBA8 format (`port/ui/src/render_d3d12.cpp`,
  `kColorFormat`; read). With FP16 swap buffers it must draw into its own RGBA8 texture. A small pass then blends
  that texture onto the swap buffer: decode, scale by paper white, blend.

The rest is as docs/43 planned:
- **Detection**: `GetDesc1` on the output that holds the window. Check again when the window moves or the display
  setup changes. Read the SDR white level from `DisplayConfigGetDeviceInfo`.
- **Swap chain**: FP16 plus `SetColorSpace1(G10_NONE_P709)` (scRGB) while HDR is on. `swap_resize` needs its own
  trigger for a format change, because today it returns early when the size is unchanged.
- **Blit** (`blit_to_window`): decode gamma 2.2 (sRGB as an option), then multiply by paper white / 80. Paper white
  defaults to the SDR white level.
- **Pictures**: screenshots and the crash-report picture read RGBA8, from the game frame plus the UI texture.

Effort: 3.5 to 5 days.

### 5.3 Stage B: the game's own highlights (new; replaces docs/43 stage 2)

Keep what the composite throws away:
1. **T and `0x4260000` as FP16** while HDR is on. The second one holds both the depth-of-field result and the
   motion-blur copy.
2. **Write-back**: these targets go back to guest memory converted to RGBA8, clamped, so the game sees the format it
   asked for. The probe skipped the write-back; a real build must not.
3. **Composite**: replace `saturate(dof x 2)` with `min(dof, 1) x 2`. That keeps the original's 8-bit limit (2x
   white) but stops the clip at white. The port names the program, as it names programs for the FXAA hook
   (`aa_before_fp_load`).
4. **Motion blur** (fp `0xB41181`, `0xB40D01`): check that its programs don't clamp; drop a clamp if one does.
5. **Display copy**: in HDR, our own pass replaces fp `0x181`. It reads T, applies the LUT to the 0 to 1 part, keeps
   what is above 1, and writes an FP16 game frame. The LUT stays in because the brightness setting may change it.
6. **Output curve**: identity up to a knee, then a smooth roll-off that ends at the display's peak (`MaxLuminance`).
7. **The HUD, menus, title, loading screens and movies** all draw with values up to 1, so they land at paper white
   with no mask. The HUD draws after the composite; the others have no composite.

Effort: 2.5 to 4 days.

### 5.4 Not proposed

- **docs/43 stage 2** (an expansion curve on the SDR picture plus a HUD mask): it guesses highlights from 8-bit SDR.
  Stage B uses the game's own values and needs no mask.
- **docs/43 native HDR** (scene programs without their clamp): it adds 0 to 0.025% of the picture (section 3), for
  10 to 20 days of risk.

## 6. What to expect on this PC (inferred from sections 1, 3 and 4)

- **Shadows**: correct instead of lifted. Probably the biggest change.
- **Highlights**: the lantern, candles and openings to the sky get a little brighter than white. How much brighter:
  - 1.17x at paper white 240 nits (today's SDR level);
  - 1.75x at paper white 160 nits.
- **Everything else**: the same picture.
- **A brighter HDR display** (600 to 1000 nits) would show the full 2x signal (4.6x light) without squeezing it.

## 7. Risks

- **Effects without a clamp.** In an FP16 T, additive effects can add up above 1 where the RGBA8 target clipped
  them. The `min(dof, 1)` in step 3 keeps the original's limit at the composite, but check fog and particle-heavy
  scenes.
- **Banding.** The highlight range comes from 8-bit targets: 128 steps between 1x and 2x white. Probably fine for
  small highlights; check skies.
- **HUD over highlights.** Half-transparent HUD panels over a highlight let some of it through, so they glow a little.
- **GPU cost.** An FP16 T doubles that target's memory traffic: 7.4 MB instead of 3.7 MB at 720p, and 29 MB instead
  of 15 MB at `RSX_SCALE` 200. This only applies with HDR on. Measure before merging.
- **Only two places measured** (the Nexus and the tutorial), on one save.
- **Double mapping** with RTX HDR or Auto HDR on top. The setting's help text should say so.

## 8. How it was measured

Scratch only, never committed. A probe on the D3D12 backend (worktree branch `probe/hdr-headroom`, not pushed):
- `HDR_PROBE=7A0000`:
  - that target is created `R16G16B16A16_FLOAT`;
  - its write-back is skipped;
  - the fragment decompiler leaves out the clamp of a program's last instruction when it is a DIV (the scene
    programs' tone-map tail).
- `HDR_PROBE_SNAP=<file>`: once per value, T is copied inside the replay and read back after the frame.
  - By default the copy is taken just before the first draw that samples T after at least 50 scene draws, so the
    snapshot holds the scene only.
  - The file is an `HDRP w h` line followed by RGBA half floats; a summary line goes to stderr.
- `HDR_PROBE_FP=<fp>`: that program loses every clamp, and the snapshot is taken right after its first draw into T.
- Runs:
  - The Nexus: on the archstone save.
  - The tutorial: on the pinned regression save, after skipping its opening cutscene (START, then CROSS). Camera
    moves came from `PAD_STICK`.
  - All muted, offline and scripted, with the window shown.
- The LUT was read with the debug console's `mem C00003E0 1024`.
- The display facts came from two scratch tools: a PowerShell DisplayConfig query and a small `GetDesc1` program.
- `hist.py` turned the raw files into the tables in section 3, plus preview images with the parts above white
  marked.

## 9. Open questions

- Sunlit areas, fire and spells (section 3).
- Whether the brightness setting changes the LUT, a constant or the exposure.
- RTX HDR works with `des_host.exe` (section 5.1). Still open: its frame-rate cost, and how it treats the wrapper
  UI.
- Whether the motion-blur programs clamp.
- Whether the game ever reads T's guest memory, which decides whether the write-back is needed at all.

## Sources

- Microsoft: [Use DirectX with Advanced Color on high/standard dynamic range displays](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range).
- NVIDIA: [NVIDIA App Beta Adds G-SYNC Controls, RTX HDR Multi-Monitor Support & More](https://www.nvidia.com/en-ph/geforce/news/nvidia-app-beta-update-g-sync-controls-rtx-hdr-multi-monitor/).
- RTX HDR requirements (secondary): [freedom251.com](https://freedom251.com/?p=911049) and [geekchamp.com](https://geekchamp.com/?p=1070314).
- Windows 11 Auto HDR per-app switch: [ElevenForum](https://www.elevenforum.com/t/turn-on-or-off-auto-hdr-in-windows-11.891).
- docs/28 (the scene's tone map, the LUT), docs/43 (the earlier HDR plan, the frame layout).
