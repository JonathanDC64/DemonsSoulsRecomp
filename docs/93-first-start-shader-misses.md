# Shaders compiled at a first start (2026-10-23)

A player reported stutters after starting the game, on the first start after setup, and asked whether setup's shader pass was applied. It was: the start found the cache up to date and used its 3,603 shaders. But 19 more shaders were compiled during play, and the GPU pipelines are not prepared at all. This doc gives the numbers, the two causes of the 19, and what is left.

Tags: **(measured)** counted on this PC; **(inferred)** not run.

## 1. The player's session (measured)

From the session log, the package's cache folder and its file times. Settings: 60 fps, render scale 200%, 16x anisotropic filtering, the output filter on, online on.

- The start printed "shaders: cache up to date": setup's pass and the game's agree on the cache and its stamp.
- 19 shaders were compiled in play: 2 vertex and 17 pixel shaders.
  - 5 in the menus.
  - 14 in 0.46 s as the level appeared, all on the render thread, so in one frame.
  - None after that.
- 226 pipelines were built in play and 19 loaded. Pipelines are built the first time a draw needs one, then kept in `pso_library.bin`; the next start loads them.

## 2. Reproduced (measured)

The runs use a copy of the setup package, a copy of the player's save and the player's graphics settings, all scripted, muted and offline. Each run is the arrival cutscene (about 40 s, at 30 fps), then a 26 s walk.

- The same 19 shaders were compiled in play: the same cache keys as the player's.
- At the default settings (30 fps, 100%), 4 were.

### 2.1 The causes

1. **The executable's own programs.** The game's executable holds 8 Cg programs of its own (docs/30): the engine's post and display passes. Setup's pass read only the shader archives. The game drew 2 vertex and 2 pixel ones of these at the menus and as the level appeared. These are the 4 misses at 100%.
2. **The 2D forms above 100% scale.** A draw without a depth test on a scaled target gets the sprite-edge clamp (docs/37). The clamp's pixel shader had the scale in its text (`200.0 / 200.0`), and setup's pass, which runs at no scale, never built that form. At 200%, 16 pixel shaders were compiled in play this way:
   - 14 from the archives: ds_filter (11), ds_menu (2) and dbgfont (1). This was found by stripping the clamp from each kept HLSL text, then finding the base text's cache entry in a pass over one archive at a time.
   - 2 from the executable.
   - Any other scale has its own text, so a change of scale would have compiled them all again.

### 2.2 The fix

- **ps3recomp.**
  - The clamp reads half the scale from the draw's constants (`rsx_alphatest.w`), so one shader and one pipeline serve every scale.
  - `rsx_shader_precompile_add_flags(..., RSX_PRECOMPILE_2D)` builds a program's 2D forms too.
- **Port.**
  - The pass marks ds_filter, ds_menu and dbgfont as 2D.
  - It finds the executable's 8 programs by their headers and queues them, also as 2D.
  - Setup runs it as `des_host --shaders <game folder> <EBOOT.elf>`. A start reads main's ELF.
  - The stamp records the executable, and its version is now 2, so every cache gets one new pass.
- **Setup's pass:** 1,429 programs (was 1,421) and 3,654 shaders (was 3,603), in 20.8 s on 15 threads (was 22.3 s).
- **After the fix:** 0 shaders were compiled in play.

### 2.3 Frames over 45 ms in the level

| Run | As the level appears (~1.5 s) | As play starts (~40 s) | Shaders compiled in play | Pipelines built in play |
|---|---|---|---|---|
| Before, first start | 282 ms | none | 19 | 169 |
| Before, second start | 49 ms | none | 0 | 1 |
| After, first start, the driver new to the new shaders | 364 ms | 173 ms | 0 | 171 |
| After, first start again | 121 ms | 59 ms | 0 | about 170 |

- After the cutscene, every run holds 60 fps through the walk: frames of 16 to 20 ms.
- In the "first start again" run (PERF=1), the window as the level appeared built 51 pipelines in 60 ms. The window's misses took 110 ms in all.
- As play started, it built 35 pipelines and loaded 12 vertex shaders from the cache, in 90 ms.

## 3. What is left: the pipelines

On a first start, the pipelines are still built as the game first draws with them, on the render thread.

- **This PC.**
  - It takes about 1.2 ms a pipeline here, because the graphics driver's own cache has seen these shaders before.
  - The first run after the fix met the new 2D shaders (new text, so new to the driver) and took 364 ms where the next took 121 ms. So a driver that has never seen the game's shaders costs more.
- **A new player's PC** sees every pipeline for the first time (inferred).
- **Later starts** load the pipelines from `pso_library.bin`: 49 ms at the same point.

The cures are docs/49's:
- predict the pipelines from the game's files and build them before play;
- or, for what is not predicted, build a frame's missing pipelines on several threads before that frame (docs/49 section 3).

Neither is built.

## 4. Not covered

- The session had online play on: wandering ghosts every 20 s, bloodstains and messages. A ghost's equipment and effects are new draws, so new pipelines. The test runs are offline, so these were not measured.
- A program outside the three archives that is drawn without a depth test still compiles its 2D form in play, once. `fp_variants.txt` does not record the form, so a cache emptied by a decompiler change refills it in play again.
