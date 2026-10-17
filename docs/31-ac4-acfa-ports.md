# 31 — What the Armored Core 4 and For Answer ports do that we could use

Two community ports of FromSoftware PS3 games from around the same time as Demon's Souls, both by SulphurS16 ("Project Glintburn"):

- Armored Core 4: https://github.com/SulphurS16/AC4-Port (read at `db47b24`, release 0.5.0, 2026-10-16)
- Armored Core: For Answer: https://github.com/SulphurS16/ACFA-Port (read at `8d6d7ca`, release 0.7.0, 2026-10-16)

This page covers ideas and approaches only. No code was copied.

**Licences.** Both use the **MIT** licence, with one added paragraph saying it covers the source only and grants nothing for the game, its keys or system software. GitHub lists the licence as "Other" because of that paragraph. The AC4 licence also says it includes code from the ACFA port under the same terms. Third-party code: zlib 1.3.1 (zlib licence). The build tools are downloaded, not included (`THIRD_PARTY.md`).

**What the public trees leave out.** Both repositories were created on 2026-10-16 as exports of private trees. The first commit says "Import the ACFA native-port tree (the earlier ACFA project's commit 4f9ab10)". Code comments cite files the export does not include:
- notes: `docs/GAME_NOTES.md`, `docs/STATUS.md`, `rsx_facts.md`, `local/research/spu_facts.md`, `docs/INPUTS.md`, `docs/RELEASE.md`;
- tools: `tools/shader_manifest.py`, `tools/spu_manifest.py`, `ratecheck.py`;
- the test and autopilot scripts.

There are no issues or PRs, and the commit messages have titles only. The commits carry `Co-Authored-By: Claude Opus 5.5`. Everything below comes from the code, its comments, the commit titles, the CHANGELOGs and the READMEs. Where those don't settle something, this page says so.

## The two ports in short

Both ports share one codebase (AC4 began as a copy of the ACFA tree, and fixes move between them):

- **Source platform:** PS3, from the US discs: AC4 is BLUS30027 (01.01) and ACFA is BLUS30187 (01.01 with update 01.35).
- **Method:** static recompilation with **their own C++ recompiler**. It is not ps3recomp, XenonRecomp or rexglue.
  - `src/recomp/`: the PPU-to-C++ recompiler (`ac4-recomp analyze|disasm|func|fingerprint|emit`).
  - `src/spurecomp/`: an SPU-to-C++ recompiler, plus an SPU interpreter as a fallback.
  - `src/runtime/`: the HLE runtime (lv2, cellGcm, cellAudio, cellSail and so on).
  - Size: AC4 is ~3.6 M PPC instructions, ~54,000 functions, 99.3% code coverage, with "every indirect jump resolved". ACFA is ~4.5 M instructions and ~71,000 functions. For comparison, our lift has ~94,000 functions.
- **Firmware run as recompiled code (LLE)** from the player's own system software 4.93: 10 modules (libsre/SPURS, libsail, libpamf, libdmux, libdmuxpamf, libadec, libvpost, libatrac3plus, libatxdec, libat3dec), plus `flashATRAC.pic` from dev_flash. See [`cellSysmodule.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/hle/cellSysmodule.cpp) and [`known_inputs.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/common/known_inputs.cpp).
- **Graphics:** **Direct3D 11**. RSX methods are translated directly (`src/runtime/rsx/`, `src/runtime/gpu/renderer.cpp`). Vertex and fragment programs are translated to HLSL (`vp_hlsl.cpp`, `fp_hlsl.cpp`) and compiled with d3dcompiler_47 into a disk cache. A "shader manifest" (`data/shader_manifest.txt`) lists programs by file, offset and length in the player's disc files, with no game bytes. Setup compiles them on all but one core before the first run ([`shader_warm.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/shader_warm.cpp)). D3D11 has no PSOs, so pipeline-creation stutter does not apply to them.
- **Toolchain:** pinned **llvm-mingw**, CMake and Ninja zips (SHA-256 in `data/toolchain.txt`). They unpack into the port's data folder; nothing is installed system-wide. Generated code is built with `-march=haswell` (AVX2, FMA, BMI2), and the launcher checks the CPU first.
- **Player flow:** one launcher exe. It runs the player's own RPCS3 to decrypt EBOOT.BIN and the 10 modules, verifies every input by SHA-256, downloads the toolchain, recompiles and builds the game on the player's PC, and then starts it. Saves and settings live in `Saved Games\<title>`.
- **Features they ship:**
  - display: native-resolution rendering with a 50-200% render scale; windowed, borderless and exclusive fullscreen; monitor choice;
  - frame rate: 30, 60, 120, 144, the display's rate, or uncapped;
  - input: a raw-input mouse camera, keyboard and mouse in the game's own menus, DS4/DualSense over HID, and XInput;
  - system dialogs and an in-game PC SETTINGS page drawn inside the game's picture.

## Engine overlap with Demon's Souls

- **Middleware in common:**
  - **MultiStream / MO sound**: ACFA patches `MOMainThread` races; Demon's Souls has `MOMainThread`, `MOMixerThread`, `MOStreamingThread` and the `_synth2` SPU. See docs/28 "dropped MultiStream reply".
  - **SPURS**: both run libsre as recompiled code; so do we for part of it.
  - **cellSail movies.**
  - **Lua**: AC4 uses **Lua 5.0**, with `luaD_throw`, setjmp and longjmp ([`lua_hooks.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/lua_hooks.cpp)). Demon's Souls runs Lua as well (`luabnd`, the `RequestRemo` binding).
- **File formats:** `.gitignore` lists `.bnd .bdt .bhd .tpf .drb .fmg .000-.003`, the same FromSoftware families as ours.
  - The game code reads them itself, so the ports parse only one format: **DRB** (FromSoftware's menu layout) inside a **BND3** with zlib inside, to add a menu row ([`system_menu.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/system_menu.cpp)).
  - No FLVER, MTD, DCX or param work appears in the code.
  - Shaders ship loose as `.fpo`/`.vpo` (`USRDIR/shader/**`, `sfx/shader/**`). We use `.shaderbnd` (BND3 + CgBinaryProgram).
- **Havok, Dantelion/DL, Edge:** the code doesn't mention them, so whether AC4/ACFA use them can't be told from these repos.
- **Frame timing (how they unlock the frame rate):**
  - Both games already advance their simulation by measured time. The frame pacer counts vblanks: it flips every 1 or 2, and AC4's present routine `0x1406b8` shows a frame twice to cap at its target.
  - So the unlock only raises the emulated vblank rate, and only during gameplay. Menus and movies stay at up to 60 Hz: "some menu fades count frames (twice as fast at 120 fps)", and the system movie player (libsail) "stalls or starts late" above 60 Hz.
  - Gameplay is detected from **guest thread names**: `FcsManager` alive inside a `*SortieScene` thread ([`frame_policy.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/frame_policy.cpp)).
  - Equal game speed at every rate is checked with "per-flip motion and distinct-frame checks". `ratecheck.py` is not published.
  - The repos don't say how they handle physics at high frame rates, beyond "same movement and turning at 60 and 120".
- **Function identification:** `ac4-recomp fingerprint` writes per-function hashes. The "exact" hash masks branch displacements, TOC offsets and lis/low pairs; the "shape" hash masks every 16-bit immediate. Each line also lists the function's calls, including import NIDs. They are used to match functions "between two builds of related code (e.g. the same engine in another game)" ([`recomp/main.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/recomp/main.cpp)).
- **Resolution and aspect:**
  - Surfaces are stored at (16:9 picture height / 720) × render scale.
  - Clip space stays in the game's surface size. Fragment programs get WPOS in game pixels, so only the storage resolution changes.
  - MSAA surfaces are stored super-sized, which makes them supersampled.
  - The 16:9 picture is shown with borders. There is no ultrawide support.

## Ideas worth taking, by payoff

Effort: S = a day or less, M = a few days, L = a week or more. "Have" says what our port has today.

### 1. Native-resolution rendering with a render scale
- **What:** render targets are kept on the GPU at the game's size × scale, where scale = the output's 16:9 height / 720 × the player's 50-200%. Positions stay in the game's clip space, and WPOS is divided back to game pixels in the fragment shader (`fp_alpha.zw`). Other details:
  - A new scale applies at a frame boundary, after the window size has held for 20 frames.
  - Surfaces are re-created at the new size with their content stretched in.
  - A surface sampled as a texture gets per-unit `tex_scale`.
  - ZPASS pixel counts are converted back to game samples, so a sun glare's strength doesn't change with the scale.
- **Where:** [`renderer.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/renderer.cpp) ("Render scale (true resolution rendering)", `update_render_scale`, "Pixel counts"), [`fp_hlsl.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/fp_hlsl.cpp) (WPOS).
- **Maps to:** `libs/video/rsx_d3d12_backend.c` and `rsx_live_draw.c` (fixed 1280x720 today). The obstacles:
  - Our render-target write-back to guest memory (the exposure value, `RSX_WRITE_COLOR_DEFER`) would need a downsample.
  - The 2D-engine copies between surfaces.
  - The sun-probe ZPASS count (`df9d0f9`, 576 at 1x) must be divided by scale², or the flare brightens with resolution.
- **Effort:** L. **Payoff:** high; this is the largest visible upgrade. **Have:** no.

### 2. A player flow that needs no installs: RPCS3 command line, verified inputs, a pinned portable toolchain
- **What:**
  - The launcher copies the player's RPCS3 into a work folder with its own settings (no welcome screen, no update check), then runs `rpcs3 --installfw PS3UPDAT.PUP` and `rpcs3 --decrypt <file>` for EBOOT.BIN and each `.sprx`.
  - It reads `--help` first to check that this RPCS3 has both options.
  - It brings RPCS3's own questions forward for the player to answer.
  - Every decrypted file is accepted only by SHA-256 (`known_inputs.cpp`, with known hashes of other firmware versions for clearer errors).
  - The toolchain zips are pinned by SHA-256 and unpacked privately.
  - Game-derived manifests hold positions only, so the repository contains no game bytes.
  - Updates rebuild only what changed. The launcher carries the sources and unpacks them as needed.
- **Where:** [`launcher/rpcs3.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/launcher/rpcs3.cpp), [`prep/import.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/prep/import.cpp), [`data/toolchain.txt`](https://github.com/SulphurS16/AC4-Port/blob/main/data/toolchain.txt), [`launcher/setup.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/launcher/setup.cpp), [`launcher/install.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/launcher/install.cpp).
- **Maps to:** our README's manual steps 2-6 (decrypt in RPCS3's menu, then a separate libsre.prx) and the "patcher: no extra deps" goal. Parts we could take now:
  - `--decrypt` for EBOOT.BIN and libsre.sprx (S).
  - Known-good SHA-256 checks of the inputs (S).
  - Pinned portable zips in place of installed tools. Our steps need Python and MSVC with clang-cl. A Python embeddable zip pinned by hash would cover the lifters. llvm-mingw would drop the Visual Studio requirement, but our runtime would first have to build with mingw clang: check SEH/VEH, MSVC intrinsics and the D3D12 headers.
- **Effort:** L for the whole flow; S for the RPCS3 command-line and hash pieces. **Payoff:** high, because players cannot use the port without it. **Have:** no.

### 3. Display modes: a resizable window, borderless and exclusive fullscreen, monitor choice
- **What:**
  - Borderless at the display's resolution is the default; windowed and exclusive are the other modes.
  - F11 or Alt+Enter toggles fullscreen.
  - In exclusive fullscreen the swap chain and the size the game sees follow the display mode.
  - If the display drops out in exclusive fullscreen, the game re-enters on Alt+Tab instead of mode-switching in a loop.
  - System dialogs keep the window's mode.
- **Where:** [`gpu/window.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/window.cpp), [`gpu/d3d11_device.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/d3d11_device.cpp); CHANGELOG 0.4.0 "Display".
- **Maps to:** `rsx_d3d12_backend.c`. Today it makes a `WS_OVERLAPPEDWINDOW`, disables Alt+Enter and never calls `ResizeBuffers`. F11 is taken by our profiler, so a different key is needed.
- **Effort:** M. **Payoff:** high, and #1 needs it. **Have:** no.

### 4. A vblank locked to the display, with tearing for VRR at other refresh rates
- **What:** when the display's refresh is a whole multiple of the target (60, 120, 180, 240 Hz), the emulated vblank waits on `IDXGIOutput::WaitForVBlank` every Nth display vblank, so each game frame stays up for the same number of refreshes. Otherwise:
  - At 144 or 165 Hz it ticks from a clock and presents with `ALLOW_TEARING`, so VRR displays show frames evenly. Exclusive fullscreen never tears.
  - With no display, or one that doesn't wait, the clock paces the game.
  - The output is re-checked every 120 presents, in case the window moved to another monitor.
  - DXGI frame statistics are counted only when `SyncRefreshCount == PresentRefreshCount`, which gives "refreshes per frame" and displayed-frame interval percentiles.
- **Where:** [`gpu/display.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/display.cpp).
- **Maps to:** our vblank thread (59.94 Hz clock since `6dae2e5`) and present. A 59.94 Hz clock on a 60.00 Hz display slips one refresh about every 16-17 s at 60 fps. docs/28 "Frame pacing" also notes 30 fps frames held 4 or 5 refreshes on a 143 Hz display without VRR, and DXGI statistics we found unreliable.
- **Effort:** S-M. **Payoff:** medium-high (smoothness at 60 and 30 on common displays). **Have:** partly (`DES_VSYNC`/tearing, `FRAME_LOG`).

### 5. A rate check: same game speed at 30, 60 and 120
- **What:** they check every frame rate in a mission for the same movement and turning ("per-flip motion and distinct-frame checks", "checked at 60 and 120 fps").
- **Where:** commits `507fabb` and `9e5fc6e` (ACFA) and CHANGELOG 0.1.0 and 0.3.0. The script itself (`ratecheck.py`) is not published.
- **Maps to:** our README's open item "physics above 60 is untested". We already have the pieces: `PAD_STICK` scripted walks, the regress save, the phase-correlation captures from the cutscene work, and the guest player position (via PPU_RWATCH-found addresses). A test that runs the same scripted input at `DES_FPS=30/60/120` and compares the player's position and heading after N seconds would answer it.
- **Effort:** S-M. **Payoff:** high (it closes a known risk before players use more than 60). **Have:** no.

### 6. A player build with the diagnostics compiled out
- **What:** in player builds every developer option is a `static constexpr` member of `Config` (trace, profiler, watchpoints, verify, dump, ...), so the code they gate disappears. Developer builds use `AC4_DEV=1`/`ACFA_DEV=1`.
- **Where:** [`runtime/config.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/config.h); commit `42c547c` (ACFA).
- **Maps to:** our `PS3_KNOB`/getenv knobs and probe globals on hot paths. docs/28 measured several of these: seven getenv calls per draw ~3%, LS probe gates +6.8%, the indirect-call knob tests +2%, and the shadow stack +2.4% (already compiled out with `DES_SHADOW_STACK`). A `PS3_PLAYER_BUILD` that turns the live-knob macros into constants would catch the rest at once. A/B and live-knob tooling stays in the developer build.
- **Effort:** M (many call sites, but mechanical). **Payoff:** medium-high (likely a few % on HighGraphics, image 2 and the FIFO thread, all on the frame's path). **Have:** partly.

### 7. Patching races in MultiStream (the sound middleware)
- **What:** two races ACFA fixed in MultiStream, each by a hook that adds only the missing synchronization:
  - **A stream released between its end and the streaming thread's next tick.** An ended stream is marked inactive (byte +828 = 0) but stays listed until the next tick, and the destructor unregisters only active streams. The result was a use-after-free crash. The fix also unregisters inactive streams.
  - **Sound bank lookups during activation or removal.** Lookups walked a half-built cue table. The fix makes Find on a bank that is not active (state +8 != 2) report not found, and marks a bank removed (3) before teardown.
  - Their reasoning: "windows that the console's scheduling (two hardware threads) rarely opens", made worse when the host is busy.
- **Where:** [ACFA `compat_patches.cpp`](https://github.com/SulphurS16/ACFA-Port/blob/main/src/runtime/game/compat_patches.cpp), [ACFA `hooks.txt`](https://github.com/SulphurS16/ACFA-Port/blob/main/src/runtime/game/hooks.txt) (`0x00ef9078`, `0x00ef8ea8`, `0x00efa5d8`, `0x00ef9ff0` in BLUS30187). AC4 notes that the same windows "may exist here, at other addresses".
- **Maps to:** Demon's Souls' MultiStream code (MOMainThread and the others; docs/28 found its mutex `0x45E` and the `_synth2` SPU). Find the equivalent stream destructor and bank Find/remove functions in our EBOOT, using the field offsets as a fingerprint, and add hooks through `--override-funcs` or port hooks. Preventive: we have no crash in sound code on record. Worth doing before soak tests at high frame rates, or on a busy PC.
- **Effort:** S-M. **Payoff:** medium-high if the code is shared (intermittent crashes at loads and level starts). **Have:** no.

### 8. Transactional saves, a Saved Games folder, backup and restore
- **What:**
  - The save-data utility applies file operations to an in-memory copy. Only a sequence the game finishes is committed: a complete new directory is written and swapped in, and an interrupted commit is repaired on next use.
  - Saves live in `Saved Games\<title>`.
  - The launcher backs up and restores saves.
  - RPCS3 saves work as they are; PS3 saves work once decrypted.
- **Where:** [`hle/save_data.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/hle/save_data.cpp) (header comment); README "Settings and saves".
- **Maps to:** `ps3recomp/libs/system/cellSaveData.c`, which writes files in place with no temporary copy or rename. Demon's Souls saves constantly, so a crash or kill mid-write could corrupt a character.
- **Effort:** S-M. **Payoff:** medium-high (protects player data). **Have:** no.

### 9. Mouse camera, rebindable keys, and the mouse in the game's menus
- **What:**
  - **Camera:** raw mouse counts become camera angles (0.09° per count × sensitivity, with no smoothing, acceleration or dead zone). The angles are written into the game's look-control yaw and pitch after its update. The body then turns toward the camera through the game's own turn command, so the game still places and collides the camera.
  - **Menus:** pointing finds the item under the cursor, and the port injects the game's own key messages.
  - **Input context:** the port tracks whether input goes to gameplay, a menu or a screen. The cursor shows only in menus after a real mouse move.
  - **Keys:** every key can be rebound, two keys per action.
- **Where:** [`game/input_hooks.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/input_hooks.cpp), [`game/menu_mouse.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/menu_mouse.cpp), [`bindings.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/bindings.cpp).
- **Maps to:** `libs/input/cellPad.c` (the keyboard merged into pad port 0, with fixed keys, no mouse) and port hooks by guest address. Demon's Souls needs its own reverse engineering of the camera (yaw and pitch fields of the player camera, the lock-on state). Their method (write angles after the game's update, leave placement to the game) carries over; their addresses don't.
- **Effort:** L. **Payoff:** high for keyboard and mouse players, none for pad players. **Have:** no.

### 10. Cache invalidation by OS write tracking instead of checksums
- **What:** the 4 GiB guest reservation is made with `MEM_WRITE_WATCH`. Once per "collection", `GetWriteWatch(WRITE_WATCH_FLAG_RESET)` is asked only for the 64 KiB granules that cached data lives in. Details:
  - Nearby runs are merged into one call (~3 µs per call).
  - A collection runs lazily: when the FIFO passes the previous put, after a semaphore acquire, and at frame ends.
  - RSX transfers mark pages by hand.
  - The same tracking keeps static vertex streams in buffers of their own while unwritten (a candidate becomes a buffer when seen again in a later frame without writes).
- **Where:** [`memory.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/memory.cpp) (`VirtualAlloc ... MEM_WRITE_WATCH`), [`renderer.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/renderer.cpp) ("Guest page write tracking", "Static vertex data").
- **Maps to:** `ps3recomp/runtime/memory/vm.h` (plain `MEM_RESERVE`, so the flag can be added) and our texture cache-hit test (`tex_csum`, ~235 textures a frame, 6.6% of the FIFO thread, which is on the path again at ~118 fps per docs/28). Caveat: earlier "invalidate at draw time" attempts saved time that HighGraphics then absorbed. Measure with the causal knobs first.
- **Effort:** M. **Payoff:** medium. **Have:** no (checksums).

### 11. Building the lifted code for x86-64-v3 (AVX2, FMA) behind a CPU check
- **What:** generated PPU and SPU code is built with `-march=haswell`, and the launcher refuses CPUs without x86-64-v3. Their `fmadd`/`vmaddfp` use `std::fma`/`_mm_fmadd_ps`, which compile to single instructions.
- **Where:** [`CMakeLists.txt`](https://github.com/SulphurS16/AC4-Port/blob/main/CMakeLists.txt) (`AC4_CPU_FLAGS`), [`recomp/emit.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/recomp/emit.cpp) (FMADD and VMADDFP cases), `prep cpu` ([`prep/main.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/prep/main.cpp)).
- **Maps to:** `port/CMakeLists.txt` (`/clang:-msse4.1` today). Our lifter's `ppu_fmadd_core` calls a real `fma()`. Without `-mfma` that is a CRT call, not an instruction. Separately, `PPU_VMADDFP` in `runtime/ppu/ppu_ops.h` is an unfused `a*c+b`; whether Cell's vmaddfp rounds the product should be checked against RPCS3 before changing it. SSE4.1 alone measured +1.2-1.9%.
- **Effort:** S (flags, a CPU check, one A/B at `VBLANK_MHZ=240000`). **Payoff:** medium (unknown until measured; the ceiling is set by FP-heavy guest code on HighGraphics and main). **Have:** SSE4.1 only.

### 12. D3D work on its own submission thread
- **What:** the FIFO thread records a compact copy of every immediate-context call: commands packed back to back, constant changes as deltas, vertex data written straight into the recorded chunk. An `rsx-submit` thread replays them in order. Batches are handed over when the submit thread is idle and enough draws are queued, and frame end waits on a frame-latency bound. The reason given: "the driver spends about half of the FIFO thread's time inside context calls". A live switch exists for A/B.
- **Where:** [`gpu/submit.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/submit.h), [`gpu/submit.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/submit.cpp).
- **Maps to:** the FIFO thread in `rsx_live_draw.c` and `rsx_d3d12_backend.c`. docs/28 has `d3d12_draw_indexed` hot, with a fifth of it in the driver, and the FIFO thread as one of the two critical chains. In D3D12 the same idea means recording the command list on a second thread from a FIFO-built draw stream.
- **Effort:** M-L. **Payoff:** medium (depends on how much of the FIFO thread is D3D12 call cost; profile first). **Have:** no.

### 13. System dialogs and the on-screen keyboard drawn inside the game's picture
- **What:** save and load lists, yes/no questions, messages and text entry are drawn over the frame with Direct2D and DirectWrite, in the game's style. Mouse, keys or pad drive them. The game reads no pad input while one is open, as on the console. The window keeps its mode, so exclusive fullscreen survives. `--dialog "choose:N,yes,text:..."` scripts them for unattended runs.
- **Where:** [`sysdialog.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/sysdialog.h), [`ui/overlay.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/ui/overlay.cpp).
- **Maps to:** `libs/system/cellMsgDialog.c` (auto-answer heuristic) and `libs/misc/cellOskDialog.c` (a separate host window for the character name, `e4241f9`). Once #3 exists, a separate window will break fullscreen.
- **Effort:** M. **Payoff:** medium. **Have:** partly (works, but outside the game window).

### 14. Holding the frame rate to 60 outside gameplay
- **What:** above 60 the targets apply only in gameplay. Menus, the title and loading run at up to 60 because "some menu fades count frames". Movies run at up to 60 Hz because libsail fails above it.
- **Where:** [`gpu/display.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/gpu/display.cpp) (`target_fps`), [`game/frame_policy.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/frame_policy.cpp).
- **Maps to:** `port/des_options.cpp` `DES_FPS`, which already drops to 30 for cutscenes (RemoMan slots). Whether Demon's Souls' menus and fades count frames at 61-240 has not been checked; our movies keep speed because cellSail hands out frames by the clock. Check menu fades and the bonfire/archstone UI at `DES_FPS=120`. If they run fast, cap the vblank at 60 outside gameplay, detected from guest state as they do.
- **Effort:** S. **Payoff:** medium (correctness at high rates). **Have:** partly.

### 15. ATRAC decoders as recompiled firmware, with flashATRAC.pic
- **What:** both ports run libatrac3plus, libatxdec, libat3dec and libadec from the player's firmware, and give the game `flashATRAC.pic` ("the ATRAC3plus decoder reads it at run time, the only dev_flash file the game opens"). ACFA uses the same MultiStream middleware and works with it.
- **Where:** [`cellSysmodule.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/hle/cellSysmodule.cpp), [`known_inputs.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/common/known_inputs.h).
- **Maps to:** docs/28 "Audio: ATRAC streaming". MultiStream logs "atrac failed to load" without `flashATRAC.pic`, and with it the boot hangs (synth images 10/11 never run). Our cellAtrac is HLE with a host decoder. Running the decoder modules as recompiled code, as they do, might be what lets MultiStream's ATRAC path load. Which sounds use that path, if any, is unknown.
- **Effort:** M. **Payoff:** medium if sounds are missing, otherwise none; it is a lead, not a known bug. **Have:** no.

### 16. Replacement copies of disc files for data patches (their PC SETTINGS menu row)
- **What:** for a disc file with a known SHA-256, the port builds a modified copy once, keeps it under `<data>\generated` with a version number, and serves it in place of the original (`vfs::replace`). The disc is never changed. They use it to add a PC SETTINGS row to the game's SYSTEM menu by editing the DRB in `bind/lang/us/menu/systemsetting.000`. The PC SETTINGS page itself is the overlay from #13.
- **Where:** [`game/system_menu.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/system_menu.cpp), [`vfs.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/vfs.cpp).
- **Maps to:** our VFS (`PS3_VFS_ROOT`) and options UI. Demon's Souls has DRB menus and FMG text too; where its menu DRB sits (which bnd, DCX or not) is still to be checked. The mechanism alone (S-M) would also carry text fixes or UI tweaks without touching the user's dump.
- **Effort:** M-L for an in-game settings row; S-M for the replace mechanism. **Payoff:** medium. **Have:** no.

### 17. A `--verify-native` mode for native replacements
- **What:** every native version of a guest leaf function can run beside the recompiled original. The original's result is used and differences are logged, so the game behaves as without the hooks. Call and work counters are logged at powers of two. Their hooks: a case-insensitive name compare and a pair lookup, for mission-load time.
- **Where:** [`game/native_hooks.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/native_hooks.cpp).
- **Maps to:** our native SPU blocks and hooks (`spu_ring_hook.c`, `spu_hook_put_wait`, `spu_hook_inflate_ls`) and PPU overrides. We ran temporary both-ways checks for each (strip walk, SSE strip loop). A permanent per-hook verify knob would keep that check available after every lifter or runtime change.
- **Effort:** S. **Payoff:** medium (it lowers the cost of every future native hook). **Have:** partly (one-off checks).

### 18. Function fingerprints to support other disc versions
- **What:** the `fingerprint` command described under engine overlap.
- **Where:** [`recomp/main.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/recomp/main.cpp) (`cmd_fingerprint`).
- **Maps to:** every guest address in `port/` (`des_options.cpp`, `--override-funcs`, `apply_boot_patches.py`, the SPU native blocks) is for BLUS30443. Fingerprints of our lift against the EU (BLES00932) or JP (BCJS30022) EBOOT would move those addresses mechanically. Our SPU side already keys native blocks by image hash.
- **Effort:** M. **Payoff:** medium (other regions). **Have:** no.

### 19. Instruction rewrites with match counts, and watching the game's fixed buffers
- **What:** `patches.txt` rules (`imm <function> addis|lis <old> <new> <count>`) rewrite immediates before code generation. If the match count differs, the build stops ("another game version?").
  - AC4 used it to grow two unchecked per-frame draw arenas from 3 MiB to 32 MiB. One mission filled 98.5% at 60 fps "and more at higher rates", which overflowed and crashed.
  - The bigger objects come from port memory through a heap-alloc hook keyed on the caller's LR.
  - Hooks log each arena's high-water marks (50/75/90%/overflow) and full draw-list layers.
- **Where:** [`game/patches.txt`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/patches.txt), [`game/frame_hooks.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/game/frame_hooks.cpp), [`recomp/main.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/recomp/main.cpp) (`--patches`).
- **Maps to:** `scripts/apply_boot_patches.py` (patches the lifted C), plus anything in Demon's Souls that fills fixed per-frame buffers faster at high frame rates. Particles and effects at 120 or 240 are the case to watch. Instrumenting high-water marks costs little once such a buffer is found.
- **Effort:** S per buffer. **Payoff:** medium as prevention. **Have:** no.

### 20. Player options and launcher details (each small)
- Process priority above normal as an option. Their reasoning: "with other programs keeping every core busy, each handoff between the game's threads otherwise waits for a time slice (the frame rate collapses)" ([ACFA `runtime/main.cpp`](https://github.com/SulphurS16/ACFA-Port/blob/main/src/runtime/main.cpp)). Our chain is three threads deep, so it is exposed to exactly this.
- Presets for modest PCs and handhelds, safe mode, and "Reset all settings" (with a .bak).
- Logs scrubbed of the user's profile path and account name, so a player can share one as is ([`common/privacy.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/common/privacy.cpp)).
- Warnings before game data goes into Downloads (Windows' cleanup) or a OneDrive-synced folder.
- Effort S each. Payoff: low-medium. Have: no.

### 21. A check: does Demon's Souls' Lua use setjmp/longjmp?
- **What:** AC4's Lua 5.0 raises script errors with longjmp back to a protected call. The recompiler runs a host `setjmp` at every call site of the guest setjmp, and a guest longjmp jumps to the most recent live call for that buffer, then runs the game's own register restore. `luaD_throw` is hooked to log the message.
- **Where:** [`guest_setjmp.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/guest_setjmp.cpp), [`ppu/ppu_recomp.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/ppu/ppu_recomp.h) (comment at "Guest setjmp/longjmp").
- **Maps to:** `ps3recomp/tools/ppu_lifter.py`, which has no setjmp/longjmp handling. If Demon's Souls' Lua (`luabnd` scripts) can raise an error that a script catches, the lifted longjmp would return to the wrong place. First check whether the EBOOT has setjmp/longjmp and whether anything calls them at run time.
- **Effort:** S to check, M to implement. **Payoff:** unknown (robustness). **Have:** no.

### 22. Smaller tooling ideas
- **Machine-wide CPU use in the perf log, beside our own.** It shows when another program skews a measurement ([ACFA `runtime/main.cpp`](https://github.com/SulphurS16/ACFA-Port/blob/main/src/runtime/main.cpp), watchdog). It maps to `tools/des/knobab.sh` and `exeab.sh`; docs/28 has an A/B spoiled by a busy editor. S, low-medium.
- **Hardware data watchpoints through the x86 debug registers.** Up to four, on every registered thread, they log which recompiled function touches an address. They cost nothing when off ([`datawatch.h`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/datawatch.h)). Our `PPU_RWATCH`/`PPU_WWATCH` sit behind an inline flag test on every guest access (`c477483`). S-M, low.
- **Audio block tags.** A cleared block gets -0.0f sentinels, so the mixer knows whether the writer filled a block and can wait up to 3 ms for it ([`hle/cellAudio.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/hle/cellAudio.cpp)). It could replace our `AUDIO_LAG_BLOCKS=1` (5.33 ms of extra latency). S, low.
- **A guest back-chain walk** (0(r1) to the caller, LR at +16) as a zero-cost backtrace ([`ppu_runtime.cpp`](https://github.com/SulphurS16/AC4-Port/blob/main/src/runtime/ppu_runtime.cpp) `guest_backtrace`). We have back-chain dumps in a few places; making it the fallback of `ds_dump_shadow` when the shadow stack is compiled out would restore console backtraces in the default build. S, low.

## Not applicable, or we already have it

- **The D3D11 renderer as a whole.** Our D3D12 backend already has a pipeline library, startup prebuild, a DXBC disk cache and GPU vertex fetch.
- **The shader manifest and warm-up.** We already extract and compile every program in `.shaderbnd` before the first boot (`precompile_shaders.sh`, 1421 programs), which covers more than a list of programs seen in test runs.
- **SPU code from runtime records, with an interpreter fallback and LS validation.** We lift every SPU image ahead of time, keyed by image hash.
- **The raw big-endian vertex streams decoded by the vertex shader.** We have it for strips and fans (`VP_GPU_FETCH`, `vf_strip_native`).
- **The flip command reserving its words.** `gcm_flip_into_fifo` already calls the title's callback and handles Demon's Souls' past-end ring.
- **The audio period clock on a monotonic timer with drift trim.** Done in `1e4d419`.
- **ZPASS pixel counts from occlusion queries.** Done in `df9d0f9`. Only the render-scale caveat in #1 is new.
- **DXT textures as BC.** Already `DXGI_FORMAT_BC1-3` in the backend.
- **DualShock 4 and DualSense over HID.** Already in `pad_hid_sony.c`.
- **Their own MPEG-2 decoder** (`media/mpeg2_video.cpp`). Our movies already play through cellSail and `sail_movie.cpp`.
- **The AC-specific addresses** (mouse-look setters, menu dispatchers, arena functions, `FcsManager`/`*SortieScene` gameplay detection). Only the methods transfer (#9, #14, #19); we detect cutscenes from RemoMan's slots instead.
- **Their PPU recompiler and its indirect-jump recovery.** ps3recomp's lifter already covers this for Demon's Souls. Their "mulli/ori/xori no longer write CR0" fix (`82afee1`) does not apply: `ppu_lifter.py` already emits those instructions without touching CR0.
