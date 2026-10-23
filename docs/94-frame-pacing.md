# Frame pacing: even frames and G-Sync / FreeSync (2026-10-23)

A player reported microstutter at 60 fps on a 144 Hz display, and asked for two options: one that fixes frame pacing, and one for VRR (G-Sync, FreeSync). Both are now choices of the V-sync setting (`DES_VSYNC`). This doc covers:
- why 60 fps stutters on that display;
- what each choice does in the runtime and in the port;
- what was measured;
- what is left.

Tags: **(measured)** run on this PC; **(inferred)** not run.

## 1. Why 60 fps judders at 144 Hz (measured)

The game makes its frames evenly: presents come every 16.7 ms, with a spread (sd) of 0.7 ms. But a frame can only change on a refresh, and a 143.9 Hz display refreshes every 6.95 ms. 60 fps is 2.4 refreshes a frame, so frames stay on screen 2, 3, 2, 2, 3 refreshes: 33 ms, then 21 ms. That is the judder.

DXGI's statistics show it. `FRAME_LOG=<file>` (ps3recomp) logs each present with `PresentCount` and `PresentRefreshCount`. Where `PresentCount` steps by 1, the step in `PresentRefreshCount` is how many refreshes that frame was on screen. Over a 25 s walk at the player's settings (render scale 200%):

| Frame rate, V-sync on | Frames | Held 2 refreshes | Held 3 | Uneven |
|---|---|---|---|---|
| 60 fps | 1,496 | 896 | 598 | 40% |
| 72 fps | 1,799 | 1,691 | 52 (and 55 held 1) | 6% |

- **V-sync off changes nothing in a window:** the compositor still shows a frame only on a refresh. The counts were the same.
- **72 fps divides the refresh, but 6% are still uneven.** The game's vblank clock is a timer at 72.000 Hz, while the display runs at 143.9 / 2 = 71.95. The two drift apart, and as the frames cross a refresh boundary, the timer's wake-up jitter makes a run of frames land on either side of it: one held 3, the next 1.

## 2. The V-sync setting

| Choice | `DES_VSYNC` | What it does |
|---|---|---|
| On (default) | `1` | Presents at the next refresh, as before. |
| Even frames | `even` | Every frame stays on screen the same number of refreshes (section 3). |
| G-Sync / FreeSync | `vrr` | Presents at once and caps the rate just under the refresh, for a display that follows the game (section 4). |
| Off | `0` | Presents at once, with tearing where the system allows it, as before. |

- **Live.** The port reads `DES_VSYNC` at each flip request (`pacing_apply` in `des_options.cpp`). It sets the backend's `RSX_VSYNC` and the vblank clock from it.
- **The UI no longer sets `RSX_VSYNC`.** The port does that now. An `RSX_VSYNC` given at start still wins, and pins the row as before.
- **A settings.ini from before** has `1` or `0`, which mean what they did.

## 3. Even frames

### 3.1 The runtime (ps3recomp)

- **The display's timing.** `rsx_d3d12_display_timing(&vblank)` returns the refresh period, plus the time of one vblank, in performance-counter ticks. Any thread can call it. Its sources, in order:
  1. **The swap chain's statistics.** After a present, at most every 100 ms, it reads `SyncQPCTime` and `SyncRefreshCount`: the vblank the last shown frame went up at. Over a second of those, they give the period. This is the display the frames actually reach, even with two monitors at different rates.
  2. **DWM's composition timing**, if the statistics are missing or stale.
  3. **The window's display mode**, in whole Hz, with no vblank time.

  The statistics and DWM are used only while they agree with the display mode within 1.5%. Under VRR the "refreshes" are the frames themselves, and that check keeps them out. `rsx_d3d12_display_refresh_mhz()` gives the same rate in millihertz.
- **A vblank clock on the display's vblanks.** `ps3_vblank_lock_display(n)` (`boot_main.cpp`) makes the clock thread tick every n refreshes. Each tick goes on the nearest vblank of the display's grid, `vblank + k * period`. So the game's frames keep their place in the refresh, and there is no drift. While only the period is known, the clock keeps that period with its own phase. While neither is known, it falls back to the rate `ps3_vblank_set_mhz` set. 0 hands the clock back to its timer.
- **`RSX_VSYNC=2..4`.** `Present` gets that sync interval, so each frame is held that many refreshes at least.

  This matters when a frame is a little late: the frames after it move a refresh later, instead of one frame showing n + 1 refreshes and the next n - 1. It costs one refresh of latency from then on, in place of a judder.

  The overlay's repaints, over a held frame, still present at interval 1.

### 3.2 The port

- **The rate.** With `DES_FPS` set, the rate goes to the nearest one that divides the display: the refresh over n, with n = round(refresh / DES_FPS) and at least 1.
  - 60 becomes 72 at 143.9 Hz.
  - 30 becomes 28.8, and 144 becomes 143.9.
  - Unlocked becomes the refresh rate.

  The game runs in 60 mode, flipping each vblank. The clock ticks every n refreshes, and each frame is held n.

  n stays the same while DES_FPS is within 0.6 of it, so a display at 150 Hz with 60 fps (2.5) does not flip between 2 and 3 as its rate reading wobbles.
- **The step.** The frame step (`kFrameDt`) is still the real time since the last flip request. But when that is within a quarter frame of a whole number of frames, it becomes exactly that many frames (n refreshes each). So the game moves by the time each frame is actually on screen, and not by when its request happened to come.
- **The game's own 30 fps mode and the in-engine cutscenes** keep the PS3's 59.94 Hz vblank, because their camera keys are 1/30 s apart.
  - Where the display runs at a multiple of 59.94 (within 0.6%: 60, 119.88, 120, 240 Hz), the clock goes to the display's vblanks too. The game then runs 0.1% faster at 60.00 Hz. A frame spans two vblanks, so it is held twice the refreshes per vblank: 2 at 60 Hz, and 4 (DXGI's most) at 120 and 240 Hz.
  - Elsewhere (144 Hz) they present as with On, and the log says so. G-Sync / FreeSync is the cure for them there.
- **`VBLANK_MHZ` given at start** turns the lock off: the user's vblank wins, as it does over the frame rate cap.

### 3.3 Measured

These runs used the player's settings, with the regression save, scripted and muted, on this PC (143.906 Hz). Each was the arrival cutscene, then a 26 s walk. The figures are over the walk's 25 s.

| `DES_FPS`, Even frames | Rate | Frames | Held the same | Game step | Game time / real |
|---|---|---|---|---|---|
| 60 | 72.0 fps | 1,796 | 100% (2 refreshes) | 13.90 ms, every frame | 1.000 |
| 144 | 143.9 fps | 3,594 | 3,593 (1 refresh); 1 held 2 | | 1.000 |
| 30 | 28.8 fps | 719 | 99.4% (5 refreshes); 2 held 4, 2 held 6 | | 1.000 |

At 30, each frame needs 5 refreshes, but DXGI's sync interval stops at 4. So a frame that is late shows for 6, and the next for 4, instead of the later frames moving one refresh. That happened twice in 25 s.

## 4. G-Sync / FreeSync

- **Presents at once,** with `DXGI_PRESENT_ALLOW_TEARING`, as Off does. Microsoft's guidance for VRR in a flip-model swap chain is sync interval 0 with tearing allowed.
- **The rate is capped a little under the refresh**, at refresh × (1 − refresh / 3600): 138 fps at 144 Hz, 59 at 60 Hz. Above the display's range, VRR stops and the display tears or waits again.
  - The cap moves only by more than 1 fps, so it does not flip as the rate reading wobbles.
  - Below the cap, DES_FPS is kept.
  - The 30 fps mode and the cutscenes are not capped. A VRR display doubles their frames below its range (LFC).
- **The display and the driver have to have it on.** The game cannot turn on VRR itself. On NVIDIA:
  - "Set up G-SYNC" with "Enable for windowed and full screen mode";
  - FreeSync / Adaptive-Sync on in the monitor's menu;
  - DisplayPort, for a monitor whose HDMI is 2.0;
  - the game window in front (borderless fullscreen is the surest).

  NVIDIA's "G-SYNC Compatible Indicator" (Display menu) shows when it is working.
- **Measured here:** only the cap. The test runs never take the window's focus (`PS3_NO_USER_INPUT`), so VRR did not engage. At `DES_FPS=144` the game ran at 138.0 fps, and the fixed 144 Hz display held 94% of the frames 1 refresh. With VRR working, each frame should be shown as it comes (inferred).

## 5. Not done

- **Native Linux:**
  - The Vulkan swap chain (`rsx_vkd3d_dxgi.c`) presents any sync interval above 0 as FIFO, one refresh.
  - It keeps no frame statistics.
  - SDL gives the refresh in whole Hz.

  So there, Even frames gives the rate rounding and the step, but neither the hold nor the lock to the display's vblanks.
- **The Enhanced preset** still sets 60 fps with V-sync On. On a 144 Hz display, that is the judder in section 1.
- **First-use pipeline hitches** (docs/93 section 3) are separate from pacing, and still there on a first start.
