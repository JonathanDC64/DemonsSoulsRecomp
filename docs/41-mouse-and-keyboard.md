# Mouse and keyboard play (spike, 2026-10-18)

A docs-only spike. Nothing was built or run. Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`), the release the port uses.

Two labels are used below:
- **(read)**: I read it in the source or in the disassembly (`ps3recomp/tools/ppu_disasm.py --va`). Constants were read from the ELF.
- **(inferred)**: my conclusion, not yet checked in a run. The run requests in section 6 check these.

## Summary

> **Correction (2026-10-18, after a run).** Approach (b) as written below does not work: writing yaw (+0x134) and pitch (+0x130) at the entry of 0x00119670 does not turn the camera. It shipped that way in port #55, and a run that fed counts in (`md1`) showed the hook taking them and writing the angles, but the update rebuilds both angles from other state every frame, so the writes are gone by the next frame. The fix PR (`fix/mouse-look-camera`) puts the turn where the update really takes it; until that section is rewritten from it, read sections 2.3 and 6's mouse look requests with this in mind.

> **Correction 2 (2026-10-19, runs mf1, mk1 and mk2).** Mouse look works as port #67 (`fix/mouse-look-camera`) does it. The update rebuilds yaw (+0x134) and pitch (+0x130) from the eye and the target every frame, and adds the stick's turn from 0x0011C758 on, only on a frame that took the stick-moved path. So the turn goes in at three hooks. The entry, 0x00119670, takes the counts, and drops them while +0x12C, +0x240 or +0x243 is set. 0x0011C910 makes the stick test see a move while a turn is waiting (path 0x0011CCD8). 0x0011C758 adds the turn after the rebuild. The game then clamps pitch to [+0x1D4, +0x1D0], wraps yaw and places the camera in the same frame. In mf1, 300 counts turned the camera 54.00 degrees at 30 and at 240 fps, with no drift.
>
> This answers run requests R3 and R4. **Lock-on is +0x240, read at the update's entry** (read). While a target is held, the camera manager (0x00115370) copies the look-at point from its +0x80 to the camera's +0x230, and the flag from its +0x90 to +0x240, before every update. The update clears the flag, so it reads 0 from outside the update. A test can detect the lock by writing a marker into +0x230: the game overwrites it within a frame while locked on. The character flag "+0x310 bit 0x800" is bit 0x800 of the word at +0x314 (an `ld` of +0x310, masked in its low word). It selects the second camera, likely the bow's aim view, not lock-on. **No menu-open flag was found.** Manager +0x74, chr+0x314, `*0x01B3824C` and `*(*0x01B3D6E0)+0x1E4` stayed the same with the START menu open, and the camera and the stick still work there.
>
> **The lock-on flick works** (approach (a) in section 2.3). With a target held, sideways counts past 15 degrees become a 3-read pulse of the right stick. mk2 switched targets 2 of 2 with the mouse at 30 and at 120 fps (the right stick also 2 of 2), and a 9-degree move kept the target.
>
> **L3 while dashing does nothing: Demon's Souls has no jump** (checked by hand in RPCS3, 2026-10-19). The runs match that. L3 reached the game in every run, during a dash the game's own numbers confirm (wanted speed 3.67 to 5.70 m/s, stamina 95 to 80 in 2 s), and nothing happened. The game reads L3 as digital1 & 0x02 (0x00C45670, bit table 0x019DC468), the same way as R3 and START, which work. In docs/47's run r4, the game's pad reads saw L3 in 15 samples during a 5.34 m/s dash, no input slot byte changed and the player did not rise. docs/42's cw6 saw the same.

- **Mouse look: write the camera angles directly (approach b).** Add the mouse's yaw and pitch to the follow camera's angles once per game frame, just before the camera's own update runs. Hook point: the entry of the follow-camera update, 0x00119670. The game then clamps pitch and wraps yaw itself.
- **Not a virtual right stick (approach a), except as a fallback.** The game puts a 0.37 radial deadzone on the stick. At the default camera speed it turns at most 120 deg/s, or 260 deg/s after 0.6 s at full tilt. A mouse fed through that feels slow and laggy. Keep (a) only for lock-on target switching and the aim view.
- **Keyboard: split movement from the D-pad.** Today the arrow keys press the D-pad and also move the left stick, so walking with them also switches weapons, spells and items. Movement keys should drive the left stick only, with a walk key that shortens the deflection.
- **Mouse buttons can already be bound** through `PAD_KEYS` with virtual-key codes (0x01 left, 0x02 right, 0x04 middle, 0x05 and 0x06 side). Untested. They need gating, so that the click that focuses the window, or a title-bar drag, does not attack.
- **Wheel:** turn each notch into a D-pad press that lasts a counted number of game pad reads.
- **Window:** read raw mouse input (WM_INPUT) on the window thread. Hide and clip the cursor while playing. Release it when a wrapper document is open, when the window loses focus, and while the window is moved or resized.
- **Game menus stay key and pad driven.** A mouse cursor in them, and keyboard glyphs in place of PS3 ones, are out of scope.
- **Effort:** about 5.5 to 7.5 days for the core. The main risks are the lock-on and aim states (which flags to trust), the feel at 30 fps, and focus edge cases.

## 1. The input path today

### 1.1 Host side: cellPad merges the pads and the keyboard (read)

File: `ps3recomp/libs/input/cellPad.c`.

- `pad_poll_backend()` (line 581) polls the host at most every 4 ms (`PAD_POLL_INTERVAL_MS`, line 512). The order is:
  1. XInput (`pad_poll_xinput`, line 290);
  2. Sony HID (`pad_poll_hid`, line 346);
  3. `PAD_MAP`, applied to the pads only (`pad_apply_map`, line 557);
  4. the keyboard (`pad_poll_keyboard`, line 234);
  5. the UI hold (`cellPad_set_host_hold`, line 614): it zeroes everything while a wrapper document is open.
- `pad_poll_keyboard()` reads `GetAsyncKeyState` for the 16 keys in `s_pad_keys`. It does so only while the foreground window belongs to our process (`pad_host_window_focused`, line 197). On focus loss it releases everything if no pad is connected.
- The default keys (line 210):

  | PS3 button | Key |
  |---|---|
  | Select / Start | Tab / Enter |
  | L3 / R3 | none |
  | D-pad | arrow keys |
  | L2 / R2 | 1 / 2 |
  | L1 / R1 | Q / W |
  | Triangle / Circle / Cross / Square | S / X / Z / A |

- With no pad connected, the left stick is built from the D-pad bits (0 or 255 per axis). With a pad connected, an arrow key overrides that axis of the pad's left stick. The keyboard never moves the right stick (always 128). So an arrow key presses the D-pad and moves the stick at the same time.
- `PAD_KEYS` takes `button=key` pairs. A key is a letter, a digit or a number (`strtol`, line 229). A number is a Windows virtual-key code. So `0x01` (left button), `0x02` (right), `0x04` (middle), `0x05` and `0x06` (side buttons) are valid values. `GetAsyncKeyState` reports mouse buttons too, so `PAD_KEYS="r1=0x01,l1=0x02,r3=0x04"` should already work. Not tested. `PAD_KEYS` is a live knob.
- `cellPadGetData` (line 707) builds the PS3 report: digital buttons, right and left stick bytes, pressure values. Its test knobs help with the run requests:
  - `PAD_STICK="lx,ly,rx,ry[,from,until]"` (line 916): hold the stick bytes from `from` to `until` seconds. The console's `setenv` changes it live, and the seconds then count from that moment. Only a new value restarts the window.
  - `PAD_FILE=<path>` (line 870): closed loop. Writing `0x0010 2` to the file presses D-pad up for 2 polls. A poll here is one game call of `cellPadGetData` on port 0, so N counts game reads.
  - `PAD_TRACE=1` (line 677): prints the number of `cellPadGetData` calls per port once a second.
  - `PAD_SCRIPT`, `PAD_SWEEP`.
  - `g_pad0_reads` (line 705) counts port 0 reads, for code in the port.
- Real pads go through a radial deadzone (`pad_stick.h`; `PAD_STICK_DEADZONE` 7849 of 32767, `PAD_STICK_ANTI_DEADZONE` 13 %). The keyboard path writes bytes directly.

### 1.2 Game side: from cellPadGetData to the camera (read)

- `cellPadGetData` is imported at 0x016B8B44 (NID 0x8B72CDA1).
- The Dantelion pad device (vtable 0x01879820, update in slot 17 at 0x00C46338) calls it. It turns the bytes into floats: (b / 255 - 0.5) * 2, with Y negated. No deadzone at this layer.
- The game reads pads through PadMan. `PadMan::GetPadDeviceForIdx` is 0x001C4AE0 (the name comes from a comment in RPCS3's FreeCam patch for this game). It has 58 callers.
- **The game's own stick processing is 0x001C3000.** It takes two analog ids and returns a 2D vector:
  - m = length of (x, y);
  - m <= 0.37: zero;
  - otherwise the length becomes min((m - 0.37) * 1.7241, 1.0), so full scale is reached at m = 0.95.
  - The constants sit in the pool at *(TOC-0x7BF4): -0x7F98 = 0.37, -0x7F8C = 1.7241, -0x7FC8 = 1.0.
  - Analog ids 16/17 are the left stick, 18/19 the right stick.
- Left stick getters: 0x001B6960 and 0x001B6998 (ids 17, 16).
- Right stick getters used by gameplay: 0x001B6A80 (X) and 0x001B6B40 (Y). Each multiplies by +1 or -1 from the options object (returned by 0x0090BCF0): byte +0x2A for X, +0x2B for Y. These are the camera axis options (Normal/Reverse) (inferred).
- The player's PadManipulator update is 0x0030F3C4. A second variant is 0x0030F7A4. Both store the camera input as a vector into the manipulator: **+0x40 = right stick Y (pitch input), +0x44 = right stick X (yaw input)**. The stores are at 0x0030F48C and 0x0030F93C.
- The character reaches its manipulator through 0x00270110: `*(*(chr + 0x20) + 0x174)`. The player character is `*(*0x01B3D6E0 + 0x1BC)` (`kChrMan` in `port/des_options.cpp`).
- Other right-stick readers:
  - getters without the invert option: 0x001B68D8, 0x001B6898, 0x001B6910 and 0x001B69D0, called from 0x00381194, 0x003811A4, 0x004F6AEC and 0x004F6BA4 (purpose not checked);
  - 0x0017C08C and 0x00183B50, through the PadMan float getters 0x001B3F58 and 0x001B3F90 (inferred: model viewers in menus);
  - the debug free camera `DbgCamControls` at 0x0012B2F8 (name from the RPCS3 patch).
- The game also imports cellMouse and cellKb:
  - `cellMouseInit` / `cellKbInit` are called from 0x00B7BFA8 and 0x00B7BF30 (Dantelion input setup);
  - `cellMouseGetData` from 0x00C1A2D8 and 0x00C1A4E0; `cellKbRead` from 0x00C0DEFC and 0x00C0E0B4 (Dantelion device classes).
  - In ps3recomp, `cellMouse.c` has host feed calls (`cellMouse_moveEvent`, `_buttonEvent`, `_wheelEvent`), but nothing calls them. So the game sees no mouse today.
  - (inferred) No gameplay code seems to use these devices. They are probably for Dantelion's debug tools. Not worth chasing: a direct hook gives better control.

### 1.3 The window, and where a mouse can enter

What the code does (read):
- The D3D12 backend creates the game window on its own thread by default (`RSX_WINDOW_THREAD=1`; `open_window`, line 942, and `window_thread`, line 926, in `ps3recomp/libs/video/rsx_d3d12_backend.c`). That thread runs a plain `GetMessageA` loop. It never waits on the game.
- `d3d12_wndproc` (line 744) handles WM_SIZE, the borderless toggle (WM_APP+0x52, Alt+Enter) and DPI. Then it passes the message to the overlay's wndproc, before its own key handling. After that it handles session lock (line 781), close, Esc (closes the game when no overlay is set), F9, F10 and F11.
- The overlay hook is `rsx_d3d12_overlay` (`rsx_d3d12_backend.h`, line 73): `wndproc` on the window thread, `frame` and `release` on the render thread. There is one slot, used by the wrapper UI.
- The wrapper UI (`port/ui/src/des_ui.cpp`):
  - `wndproc` (line 228) always takes F1 and Esc. It takes the other keys (except F9 to F12) and the mouse only while a document is open (`sCapturing`, line 101). It calls `SetCapture` on a mouse button press (line 272). It does not handle the side buttons (WM_XBUTTON*).
  - `frame` (line 497) sets `sCapturing` on the render thread. It holds the game's input while a document is open (`cellPad_set_host_hold(1)`). It gives input back only when the pads are neutral and `keyboard_neutral()` (line 365) sees no key held. That check covers virtual keys 1 to 0xFE, so mouse buttons count too.
- The window class uses the arrow cursor (`create_window`, line 857). Nothing calls `RegisterRawInputDevices`, `ClipCursor` or `ShowCursor`. There is no WM_INPUT handling.

Proposal (inferred):
- **Raw input, not cursor deltas.** Register the mouse for WM_INPUT (usage page 1, usage 2) on the game window, without RIDEV_NOLEGACY, so the wrapper UI keeps its normal mouse messages. Without RIDEV_INPUTSINK, WM_INPUT arrives only while the window is in front, which is what we want. Raw deltas skip Windows pointer acceleration and do not stop at the screen edge. Cursor deltas (recentring with `SetCursorPos`) would get both problems. Remote desktop and some tablets send absolute positions (MOUSE_MOVE_ABSOLUTE); turn those into deltas, or ignore them.
- **Where:** a small generic module in ps3recomp, fed from `d3d12_wndproc`. It keeps atomic counters (dx, dy, wheel, buttons). The game side takes them with one call, for example `host_mouse_take(&dx, &dy, &wheel)`. Other titles can use it, and a later Linux or macOS backend only has to feed the same counters.
- **Capture (cursor hidden and clipped to the client area)** when all of these hold. Clip to the client area, not the window, so a hidden cursor can never reach the title bar or the borders:
  - the window is in front (WM_ACTIVATE active, foreground PID check as in cellPad);
  - no wrapper document is open (`sCapturing` false), including the on-screen keyboard and the startup and crash modals;
  - mouse look is turned on in the settings.
- **Hide the cursor** with WM_SETCURSOR (HTCLIENT: `SetCursor(NULL)`), rather than the `ShowCursor` counter.
- **Release** (unclip, show the cursor, drop pending counts) on:
  - WM_ACTIVATE inactive, WM_KILLFOCUS, WM_CAPTURECHANGED;
  - WM_ENTERSIZEMOVE (the user drags or resizes the window); clip again on WM_EXITSIZEMOVE;
  - a document opening; the render thread posts a message to the window thread when `sCapturing` flips, as the borderless toggle already does;
  - session lock (WM_WTSSESSION_CHANGE is already handled);
  - exit.
- **Clip again** after WM_SIZE, the borderless toggle and WM_DPICHANGED. Windows can drop the clip (alt-tab, Ctrl+Alt+Del, UAC prompts), so set it again on every activation.
- **Swallow the click that focuses the window.** Otherwise it reaches the game as R1. A simple rule: after the window becomes active, ignore mouse buttons until all of them have been released once.

## 2. Mouse look

### 2.1 How the game turns the camera (read)

The objects:
- The camera manager update is 0x00115370 (vtable 0x01855FB8). It is called from 0x001410C4, 0x00144640 (inside the in-game frame update 0x00144160) and 0x00174154. Its arguments: r5 = the player character, f1 = dt.
- Manager fields:
  - +0x60: the follow camera (ChrExFollowCam);
  - +0x64: a second camera (update 0x00110608);
  - +0x6C: a third camera;
  - +0x74: the camera mode, a five-way switch at 0x001155C4.
- The ChrExFollowCam constructor is 0x00117DD0; its vtable is 0x01856018.
- **The follow-camera update is 0x00119670.** Only the manager calls it, at 0x00115CF8 and 0x00115E68 (r3 = camera, r5 = character, f1 = dt). It runs once per game frame with the real dt.

The turn, step by step:
1. At 0x00119B98 the update loads the camera input vector, manipulator +0x40 (pitch input, yaw input). If the camera is inactive (+0x298 == 0) the vector is zero (0x0011C090).
2. The stick path runs only when there is no set-angle request (+0x12C == 0) and no look-at target (+0x240 == 0) (0x00119B9C to 0x00119BB0), and no auto-turn is running (+0x243 == 0, 0x0011C740).
3. If the stick length is at most 0.998 (pool *(TOC-0x7D0C) -0x7D68), the ramp +0x128 resets to 0. Then pitch += input Y * rate +0x1E8 * dt and yaw += input X * rate +0x1EC * dt (0x0011C890 to 0x0011C8A8). Yaw is wrapped to 2 pi.
4. Pitch is clamped to [+0x1D4, +0x1D0] (0x0011C8B8 and 0x0011D3A4). The new angles go to +0x130 (pitch, 0x0011C8CC) and +0x134 (yaw, 0x0011C8D8).
5. At full tilt (length above 0.998) the ramp path runs instead (0x0011D508 onward). The ramp grows: +0x128 += dt / +0x1CC, up to 1.0 (0x0011D568 to 0x0011D584). The rate becomes lerp(slow rate, fast rate +0x200/+0x204, ramp). Yaw is stored at 0x0011D670. Pitch eases back toward its limit with k = +0x220 (a des_cam_rate site, 0x0011D6E4) and is stored at 0x0011D72C.
6. Every frame, the rates are rebuilt at 0x0011988C to 0x00119900:
   - +0x1E8 / +0x1EC = lerp(+0x1F0 / +0x1F4, +0x1F8 / +0x1FC, s * s);
   - +0x200 / +0x204 = lerp(+0x208 / +0x20C, +0x210 / +0x214, s * s);
   - s = +0x218. The manager writes it every frame from the options object: clamp(options +0x8 / 10, 0, 1) (call to 0x0090BCF0 at 0x00115404, store at 0x00115494). This is the camera speed setting (inferred).

ChrExFollowCam fields that matter for mouse look. The names are mine, from how the code uses each field. The defaults are the constants the constructor stores (0x001180E4 to 0x001181EC), read from its constant pool at *(TOC-0x7D0C) = 0x018D6190. A parameter file may change them later; run request R1 checks the live values.

| Field | Meaning | Default |
|---|---|---|
| +0x128 | turn ramp, 0..1 | 0 |
| +0x12C, +0x12D | set-angle request, one-shot flag | 0 |
| +0x130 | pitch, radians | |
| +0x134 | yaw, radians | |
| +0x138, +0x13C | requested pitch and yaw (manager writes them) | |
| +0x1CC | ramp time | 0.6 s |
| +0x1D0 / +0x1D4 | pitch limits | 1.222 (70 deg) / -0.698 (-40 deg) |
| +0x1D8, +0x1DC | not checked | 0.698, -0.698 |
| +0x1E8, +0x1EC | slow turn rate, rebuilt every frame | 2.094 rad/s (120 deg/s) |
| +0x1F0, +0x1F4 | slow rate at camera speed 0 | 1.396 rad/s (80 deg/s) |
| +0x1F8, +0x1FC | slow rate at camera speed 10 | 4.189 rad/s (240 deg/s) |
| +0x200, +0x204 | fast turn rate, rebuilt every frame | 4.538 rad/s (260 deg/s) |
| +0x208, +0x20C | fast rate at camera speed 0 | 3.142 rad/s (180 deg/s) |
| +0x210, +0x214 | fast rate at camera speed 10 | 8.727 rad/s (500 deg/s) |
| +0x218 | camera speed / 10 | 0.5 |
| +0x21C, +0x228 | not checked | 1.047, 0.0524 |
| +0x220 | pitch soft-limit ease, per frame | 0.06 |
| +0x224 | auto-turn ease, per frame | 0.3 |
| +0x230, +0x240, +0x241 | look-at target point and flags (from manager +0x80, +0x90, +0x91) | |
| +0x243, +0x244, +0x24C, +0x250 | auto-turn flag, timer, target pitch and yaw | |
| +0x298 | active | |

With s = 0.5 the formulas give a slow rate of 120 deg/s and a fast rate of 260 deg/s. Those are exactly the constants the constructor stores into +0x1E8 (0x001181C4) and +0x200 (0x001181CC). So s = 0.5 is the default, which fits a camera speed setting of 5 out of 10 (inferred).

So, at default settings: partial stick turns at stick * 120 deg/s. Full tilt starts at 120 deg/s and climbs linearly to 260 deg/s over 0.6 s. Full tilt here means a raw stick length of about 0.95 or more, after the 0.37 deadzone.

### 2.2 Approach (a): mouse to a virtual right stick

How: in cellPad, convert the counts gathered since the last game read into a right-stick deflection, held until the next read.
- Wanted turn per frame: d = counts * degrees per count.
- Processed length the game needs: m' = d / (rate * dt). Keep it under 0.998 to stay out of the ramp.
- Raw length to send: 0.37 + 0.58 * m'. Each axis byte is then round(127.5 + 127.5 * raw length * that axis's share of the direction).

Limits (from the code above):
- **Turn speed is capped** at about 120 deg/s at the default camera speed. A 180 degree turn takes at least 1.5 s. Allowing the ramp gives 260 deg/s, but only after 0.6 s.
- **Resolution:** a stick byte has 128 steps per side, and the deadzone leaves about 74 of them. Slow, precise aiming gets coarse.
- **The mapping depends on game state** it has to read: dt per frame (`kFrameDt`, 0x018C8DB4), the camera speed option (or the live rate at +0x1EC), and the invert options. Get any of these wrong and the sensitivity drifts.
- **Lag:** one pad read per game frame, plus the 4 ms poll cache.

What it is good for: anything that reads the right stick without the follow camera. Lock-on target switching, the aim view (if it reads the stick), and the menu model viewers.

### 2.3 Approach (b): write yaw and pitch into the camera

> See the correction at the top: the angles written here are rebuilt by the update every frame.

How (inferred design, using code that exists):
- **Hook the entry of the follow-camera update, 0x00119670.** Use the lifter's insn hooks (`port/insn_hooks.txt`, as `des_cam_rate` does). The hook runs right after the first instruction (`li r0, 4080`), which leaves r3 (the camera) and f1 (dt) untouched (read). Adding a site needs a re-lift.
- In the hook:
  1. Take the counts (`host_mouse_take`).
  2. If the camera is not in its free-look state, drop them and return. That means any of: inactive (+0x298 == 0), a set-angle request (+0x12C), a look-at target (+0x240), auto-turn (+0x243).
  3. Add counts * k to yaw +0x134 and pitch +0x130, with k = sensitivity in radians per count and the invert setting applied.
  4. Clamp pitch to [+0x1D4, +0x1D0]. Read the limits from the camera each time, in case other code changes them.
- The update then runs as normal. The stick input still adds on top, pitch is clamped again, yaw is wrapped, and the camera position, collision and matrix are built from the new angles in the same frame.
- **Why before the update, not after:** after the update the camera's position and matrix for this frame are already built from the old angles. Writing the angles after it would leave the matrix and the angles one frame apart and add a frame of lag. Player movement follows the camera's direction (inferred), so the two should agree within a frame.
- **Frames where the angles are rebuilt:** on some frames the update takes another path (a local flag at r1+0xF90, path 0x0011D4CC) that works the angles out again from positions. The mouse movement of such a frame is lost (inferred). Probably rare; check in testing.
- **Per frame, not per 30 Hz step:** the update runs once per game frame with variable dt. So the hook runs once per frame too. Counts never depend on dt, so the sensitivity is the same at 30, 60 and 120 fps. At 30 fps the view moves in 33 ms steps; 60 fps or more feels much better.
- **Interaction with the camera's eases:** mouse look changes only the angles. The position chase eases (des_cam_rate) still apply and are already corrected for dt. The auto-turn ease at 0x0011A100 (k = +0x224 = 0.3 per frame) is not in `insn_hooks.txt` (inferred: it runs faster above 30 fps). That is a separate high-fps item, but it decides how fast the camera swings after a set-angle event while the mouse is gated.
- **Lock-on (inferred):** while a target is held, the camera tracks it (probably the +0x240 look-at path), so free look does not apply. A fast horizontal mouse flick can become a short virtual right-stick pulse (approach a), to switch targets. What I read: the PadManipulator variant 0x0030F7A4 compares the absolute value of each processed right-stick component against 0.1 (0x0030F948 to 0x0030F970; the 0.1 is at *(TOC-0x7A64) -0x7E34). Above it, it makes a virtual call (0x0030FC58). That looks like the target switch.
- **Aim view (bow):** the second camera (update 0x00110608) calls 0x001B9AA0 at 0x001107B8. That function returns -1, 0 or +1 from two bits of a PadMan button word (+0x122). The camera adds that times its +0x13C to its +0x138 and clamps it, like a zoom (read). That this is the bow aim view is inferred. The aim direction comes from somewhere else (not found). Use approach (a) there until it is found.

A sketch of the hook, next to `des_cam_rate` in `port/des_options.cpp`:

```cpp
// insn_hooks.txt: 0x00119670=des_mouse_look   # follow-camera update entry: r3 = camera
extern "C" void des_mouse_look(ppu_context* ctx, uint32_t /*addr*/) {
    const uint32_t cam = uint32_t(ctx->gpr[3]);
    int dx = 0, dy = 0, wheel = 0;
    host_mouse_take(&dx, &dy, &wheel);              // wheel goes to cellPad, not here
    if (!s_mouse_look || !vm_read8(cam + 0x298)) return;
    if (vm_read8(cam + 0x12C) || vm_read8(cam + 0x240) || vm_read8(cam + 0x243)) return;
    float yaw   = vm_readf(cam + 0x134) + s_sign_x * dx * s_rad_per_count;
    float pitch = vm_readf(cam + 0x130) + s_sign_y * dy * s_rad_per_count;
    pitch = std::clamp(pitch, vm_readf(cam + 0x1D4), vm_readf(cam + 0x1D0));
    vm_writef(cam + 0x134, yaw);                    // the update wraps yaw
    vm_writef(cam + 0x130, pitch);
}
```

The signs of yaw and pitch are not known yet (run request R1).

### 2.4 Recommendation

Use **(b)** for free look in normal play. Use **(a)** only where (b) does not apply: lock-on target switching, the aim view, and anything else that reads the right stick. Gate the two so they never both turn the camera in the same frame. (b) gives 1:1 aim, no turn speed cap, no deadzone, no dependence on frame rate, and same-frame response.

Prior art (docs/31, item 9): the AC4 and ACFA ports turn raw mouse counts into camera angles at 0.09 degrees per count times a sensitivity, with no smoothing, acceleration or dead zone. They write the angles into the game's look-control yaw and pitch after the look control's update, and the game still places and collides the camera. Start from the same model. In Demon's Souls the stick turn and the camera placement happen in the same function (0x00119670), so the equivalent point is the entry of that function.

Knobs (proposed; default off, per the original-behaviour rule): `DES_MOUSE_LOOK=0|1`, `DES_MOUSE_SENS` (degrees per count, for example 0.09), `DES_MOUSE_INVERT_Y=0|1`. Expose them in the Controls tab.

## 3. Mouse buttons, wheel and keyboard movement

### 3.1 Proposed default layout (inferred)

| Action (PS3) | Key or button |
|---|---|
| Move (left stick) | W A S D |
| Walk (shorter stick) | Left Alt (held) |
| R1 / L1 | left mouse / right mouse |
| R2 / L2 | Left Shift + left mouse / Left Shift + right mouse (needs chords), or the side buttons |
| R3 (lock-on) | middle mouse or Q |
| Circle (roll, back step, dash) | Space |
| Cross (interact) | E |
| Square (use item) | R |
| Triangle (one or two hands) | F |
| D-pad up / down | wheel up / wheel down |
| D-pad left / right, and up / down too | arrow keys (D-pad only, no stick) |
| Start / Select | Enter / Tab |

This is close to the default PC layouts of FromSoftware's later games (Q lock-on, E interact, R item, Space roll, Shift with a click for the strong attacks), so it should feel familiar. Esc stays the wrapper menu (`is_menu_key`, des_ui.cpp line 215), so the game's Start cannot be on Esc. Several of today's defaults move: W (R1), A (Square) and S (Triangle) become movement, Q (L1) becomes lock-on, and X and Z (Circle, Cross) are freed. The Controls tab's defaults (`kDefaultKeys`, controls_window.cpp line 36) must change with cellPad's.

### 3.2 Mouse buttons

- Today: bindable through `PAD_KEYS` hex codes (section 1.1). Read with `GetAsyncKeyState`.
- Problems with that (inferred):
  - the click that focuses the window counts as a press;
  - dragging the title bar holds the left button;
  - a click while the cursor is over another window of the same process (a dialog) still counts.
- Fix: take the buttons from WM_INPUT (`RAWMOUSE.usButtonFlags`) only while the cursor is captured, and pass them to cellPad as a host mouse-button mask. `PAD_KEYS` then names them (`mouse1` to `mouse5`). The wrapper's input hold already covers them (`keyboard_neutral()` includes virtual keys 1 to 6).
- Both `GetAsyncKeyState` and raw input report the physical buttons, so a left-handed button swap in Windows does not apply. The Controls tab covers that: rebind the buttons.
- Chords (Shift + click for R2 and L2) need a small binding extension: a key can carry a modifier. About half a day.

### 3.3 Wheel

- The wheel gives notches, not a held state. Turn each notch into a D-pad press. Queue notches that come quickly.
- Count presses in **game pad reads**, not milliseconds. The game reads the pad about once per frame (inferred; `PAD_TRACE=1` shows it), so a 33 ms press is two reads at 60 fps and one at 30. cellPad already counts port 0 reads (`g_pad0_reads`), and `PAD_FILE` already holds a press for N reads.
- Proposal: press for N reads, then release for N reads, then the next notch. N comes from run request R6 (expected 1 or 2).
- In the game, D-pad up switches spells and down switches items. Left and right switch weapons and stay on the arrow keys.

### 3.4 Keyboard movement as an analog stick

- Build the left stick from four movement keys, separate from the D-pad keys. Opposite keys cancel. Diagonals need no normalising: the game clamps the processed length to 1.0 (0x001C3000), so a diagonal is not faster (read).
- **Walk modifier:** in this game the stick deflection picks walk or run. Scale the raw length down while the walk key is held. The raw length must stay above 0.37 or the character does not move at all (read). I did not find the walk and run threshold in code; run request R5 measures it. Until then, a guess: a raw length of about 0.55 (about 0.3 after the game's deadzone) should walk.
- The movement keys produce bytes 0, 128 or 255 per axis, as the arrows do today. With a walk key, the byte for a raw length r is round(127.5 - 127.5 * r) for up or left, and round(127.5 + 127.5 * r) for down or right, on the pressed axes. For a diagonal, use r / sqrt(2) on each axis, so the walk speed is the same in all eight directions.
- With a pad connected, movement keys override the pad's left stick, as the arrows do today.
- Dash is holding Circle (Space) while moving, as on the pad.
- Lock-on 8-direction movement is a separate roadmap item, not part of this.

## 4. Menus and on-screen prompts

- The game's own menus are driven by the pad (PadMan buttons and the D-pad). Keyboard navigation already works through the key bindings: arrows, Enter or Cross, Circle.
- **A mouse cursor in the game's menus: out of scope for the first version.** The AC4 port does it (docs/31, item 9): it finds the menu item under the cursor and injects the game's own key presses. Here that needs the layout and item positions of every Demon's Souls menu screen reversed. That is weeks of work (inferred).
- The wrapper UI (F1) already supports the mouse. It needs the release rules from section 1.3, so the cursor is visible and free whenever a document is open.
- **PS3 glyphs in prompts** ("Cross: examine") stay. Showing keyboard glyphs needs texture or font replacement. The port has no texture replacement path today (I found none in `ps3recomp/libs/video`). A follow-up, after a texture replacement feature.
- The Controls tab (`port/ui/src/controls_window.cpp`, uncommitted) captures bindings for `PAD_KEYS` only from key presses (`InputEvent::KeyDown` in des_ui.cpp line 434, read through `input::take_captured_key`). Binding mouse buttons and the wheel needs capture from mouse messages as well.

## 5. Effort and risks

### 5.1 Effort

| Step | Work | Where | Days |
|---|---|---|---|
| 1 | Movement keys separate from the D-pad, walk key, new defaults, Controls tab rows | `ps3recomp/libs/input/cellPad.c`, `port/ui/src/controls_window.cpp` | 1 |
| 2 | Raw mouse input on the window thread, counters API, cursor hide, clip and release rules, swallow the focusing click | `ps3recomp/libs/video/rsx_d3d12_backend.c` plus a small input module; `port/ui/src/des_ui.cpp` reports open documents | 1.5 to 2 |
| 3 | Mouse buttons and wheel to pad bits (wheel pulses counted in reads), names in `PAD_KEYS`, mouse capture in the Controls tab | `cellPad.c`, `controls_window.cpp`, `port/ui/src/input.cpp` | 1 |
| 4 | Mouse look (b): hook at 0x00119670, state gates, pitch clamp, sensitivity and invert settings, re-lift | `port/des_options.cpp` (or a new file), `port/insn_hooks.txt` | 1 to 1.5 |
| 5 | Fallback (a) for lock-on switching and the aim view; tuning at 30, 60 and 120 fps | `cellPad.c`, port | 1 to 2 |
| | Mouse cursor in game menus; keyboard glyphs | | out of scope |

Total: about 5.5 to 7.5 days. The ps3recomp parts (steps 1 to 3 and the cellPad side of 5) go in a ps3recomp PR first; the port PR depends on it.

### 5.2 Risks

- **State flags (inferred):** +0x240 (look-at target) and +0x243 (auto-turn) are read in code, but which of them is the lock-on state is a guess. The character flag at +0x310 (bit 0x800) is read by both the camera manager (0x00115578) and the lock-on PadManipulator variant (0x0030F97C); it is another candidate. If the gate is wrong, the mouse fights the camera or does nothing in some states. Run request R3.
- **Feel at 30 fps:** one angle update per game frame. At 30 fps that is coarse. Recommend 60 fps or more for mouse play.
- **Latency:** counts are taken at the camera update. The frame then has to render and present: about one to two frames, the same as the stick today.
- **Focus edge cases:** alt-tab, a focusing click, title-bar drags, Windows dropping the clip, session lock, the on-screen keyboard, the startup shader modal and the crash dialog. Each needs a release and re-capture rule (section 1.3).
- **Overlay conflicts:** the wrapper UI and the game share the mouse. Rule: a document open means the cursor belongs to the UI and the game gets nothing. The existing input hold and its neutral check already stop a click that closes a document from reaching the game.
- **Moving the mouse with the game's menu open:** if the game still turns the camera behind its menu, the mouse would too. Run request R4.
- **High fps:** (b) does not depend on dt. (a) does, through `kFrameDt`. The unhooked auto-turn ease (0x0011A100) runs faster above 30 fps (inferred), but this is unrelated to the mouse.
- **Very fast mice:** 4 to 8 kHz mice send many WM_INPUT messages. Reading them in batches (`GetRawInputBuffer`) may be needed (inferred).
- **Portability:** raw input is Win32 only. Keep the counters API generic for later Linux and macOS backends.

## 6. Run requests

Use the usual run form, through Git Bash: `tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 [knobs]`.

Most requests use the debug console. Add `PS3_DEBUG=<scratch>/dbg.txt` to the knobs. To run a command, write it into that file (`echo "stat" > <scratch>/dbg.txt`); the answer is appended to `<scratch>/dbg.txt.out`, ending with `[dbg] done` (`ps3recomp/runtime/ppu/tests/boot_main.cpp`, line 903). The commands used below:
- `find <hex start> <hex length> <hex bytes>`: every 4-byte-aligned place the bytes sit in guest memory, at most 32 hits;
- `mem <hex address> <decimal length>`: a hex dump, at most 4096 bytes;
- `setenv K=V`: changes a live knob.

Wait for `[dbg] done` in the `.out` file before writing the next command. `tools/des/desprobe.sh` can also run a list of commands from a file (`CMDS`, with `sleep N` lines).

Guest memory is big-endian. Some floats as they appear in a dump: 0.5 = `3F 00 00 00`, 1.0 = `3F 80 00 00`, 2.094 = `40 06 0A 92`, 4.538 = `40 91 36 1E`.

**R1. Camera fields and signs (no build).**
- Set: `PS3_DEBUG` as above.
- Do: in game, stand still with no target. Run `find 10000 4fff0000 01856018` to find the follow camera by its vtable. Skip the hit at 0x018CE26C (the constant pool in the ELF). The other hit is the camera; call it C. If more than one hit is left, the live one has a nonzero byte at C+0x298 and changing values at C+0x130 and C+0x134 when the camera turns. Then run `mem <C+0x120> 384` four times:
  1. idle;
  2. right stick fully right, held for 2 s;
  3. right stick halfway right;
  4. right stick fully up.
- Without a pad, the console can hold the right stick: `setenv PAD_STICK=128,128,255,128,0,3` (fully right for 3 s), `setenv PAD_STICK=128,128,220,128,0,3` (about 0.6 after the game's deadzone), `setenv PAD_STICK=128,128,128,0,0,3` (fully up). Run the `mem` command about 2 s after each.
- Log: the four dumps. Note which way +0x130 moves when looking up, and which way +0x134 moves when turning right.
- Expect: +0x128 climbs to 1.0 within about 0.6 s only at full tilt and stays 0 at half tilt. +0x1E8/+0x1EC near 2.094, +0x200/+0x204 near 4.538, +0x218 = 0.5.

**R2. Camera speed option.**
- Do: if the game's options have a camera speed setting, set it to the lowest and the highest value and repeat `mem <C+0x1E0> 64`.
- Log: +0x218 and +0x1E8 for each value.
- Expect: +0x218 = setting / 10, and +0x1E8 moving between 1.396 and 4.189.

**R3. Lock-on, auto-turn and aim states.**
- Do: dump `mem <C+0x120> 384` in each state:
  1. locked on (R3);
  2. just after releasing the lock;
  3. during a camera swing the game does by itself;
  4. bow aim, if a bow is at hand;
  5. right after a cutscene.
- Also find the camera manager with `find 10000 4fff0000 01855fb8`. Skip the hit at 0x018CE0B8 (the constant pool). Run `mem <hit+0x60> 32`: +0x74 is the mode.
- Also read the character flags: `mem 01B3D6E0 4` gives P, `mem <P+0x1BC> 4` gives the character, then `mem <chr+0x310> 8`. The flag is bit 0x800 of the low word, which is bit 0x08 of the byte at chr+0x316.
- Log: +0x12C, +0x240, +0x241, +0x243, +0x298, manager +0x74 and the character flag word, per state.
- Goal: which flag means locked on, and which means aiming.

**R4. Right stick with the game's menu open.**
- Do: open the in-game menu (Start) and push the right stick.
- Log: whether the camera turns behind the menu. Also the manipulator's input: from the character above, `mem <chr+0x20> 4` gives M, `mem <M+0x174> 4` gives the manipulator, then `mem <manip+0x40> 16`.
- Goal: decide whether mouse look must be gated while the game's menu is open.

**R5. Walk and run threshold, and the game deadzone.**
- Set: `PS3_DEBUG` as above, and `DES_FRAMELOG=<scratch>/frames.txt` (it logs the player's ground speed every half second).
- Do: on flat open ground, with hands off the pad, run `setenv PAD_STICK=128,<ly>,128,128,0,4` for ly = 90, 82, 78, 70, 60, 50, 40, 30 and 0, one at a time. Each holds the stick forward for 4 s. Then compare a keyboard-style diagonal, `setenv PAD_STICK=0,0,128,128,0,4`, with straight ahead, `setenv PAD_STICK=128,1,128,128,0,4` (a new value is needed to restart the window).
- Log: for each ly, still, walk or run, plus the ground speed lines from the frame log.
- Expect: no movement for ly of 81 and above (raw length 0.37 or less). For reference, the length after the game's deadzone is about 0.03 at ly 78, 0.14 at 70, 0.27 at 60, 0.41 at 50, 0.55 at 40, 0.68 at 30 and 1.0 at 0. Find where walking turns into running. The diagonal should run at the same speed as straight ahead.

**R6. Shortest D-pad press the game takes (for the wheel).**
- Set: `PAD_FILE=<scratch>/pad.txt`, and `PAD_TRACE=1` for reads per second.
- Do: in game, write `0x0010 1` to the file (D-pad up for 1 read), then `0x0010 2`, then `0x0010 3`. D-pad up switches the spell and D-pad down (`0x0040`) the item; use whichever slot has more than one entry in the test save. Repeat at `DES_FPS=60` and `DES_FPS=120`.
- Log: the smallest N that switches the slot every time, at each frame rate, and the `trace GetData p0=` count per second.

**R7. Mouse buttons today (no build).**
- Set: `PAD_KEYS=r1=0x01,l1=0x02,r3=0x04` (or `setenv` it from the console; it is live).
- Do: in game, click left, right and middle. Then click into the window after alt-tabbing away. Then drag the window by its title bar. Then open F1, click around, and close it with a click.
- Log: which of these attack, block or lock on.
- Expect: the clicks work. The focusing click and the title-bar drag attack too (the reason for the gating in section 3.2). Clicks inside F1 do not reach the game.

## Appendix: addresses

All are read in code unless marked.

| Address | What |
|---|---|
| 0x016B8B44 | `cellPadGetData` import stub (NID 0x8B72CDA1) |
| 0x00C46338 | Dantelion pad device update (vtable 0x01879820, slot 17): bytes to floats |
| 0x016B8BC4, 0x016B8AA4 | `cellMouseInit`, `cellMouseGetData` stubs |
| 0x016B8AC4, 0x016B8C44 | `cellKbInit`, `cellKbRead` stubs |
| 0x001C4AE0 | `PadMan::GetPadDeviceForIdx` (name from the RPCS3 FreeCam patch) |
| 0x001C3000 | stick vector from two analog ids; radial deadzone 0.37, full at 0.95 |
| 0x001B6960, 0x001B6998 | left stick getters (ids 17, 16) |
| 0x001B6A80, 0x001B6B40 | right stick X, Y getters with the invert options (options +0x2A, +0x2B) |
| 0x0090BCF0 | returns the options object (+0x8: camera speed, inferred) |
| 0x001B9AA0 | -1, 0 or +1 from two bits of PadMan button word +0x122 (used by the second camera) |
| 0x0030F3C4, 0x0030F7A4 | PadManipulator updates; camera input to manipulator +0x40 (pitch) and +0x44 (yaw) |
| 0x0030F948 to 0x0030F970 | in 0x0030F7A4: each right-stick component against 0.1 (target switch, inferred) |
| 0x00270110 | character to manipulator: `*(*(chr + 0x20) + 0x174)` |
| 0x01B3D6E0 | `kChrMan`; +0x1BC = player character |
| 0x00115370 | camera manager update (vtable 0x01855FB8) |
| 0x00115404, 0x00115494 | manager reads the options and stores the follow camera's +0x218 |
| 0x00117DD0 | ChrExFollowCam constructor (vtable 0x01856018) |
| 0x018CE26C, 0x018CE0B8 | constant pool entries that hold the follow-camera and manager vtable addresses |
| 0x00119670 | ChrExFollowCam update; proposed mouse-look hook at its entry |
| 0x00119B98 | update loads the camera input vector |
| 0x0011C740 to 0x0011C8D8 | stick turn, slow path; pitch and yaw stores |
| 0x0011D508 to 0x0011D72C | stick turn, ramp path |
| 0x0011988C to 0x00119900 | turn rates rebuilt from camera speed |
| 0x0011A100 | auto-turn ease (k = +0x224), not dt-corrected (inferred) |
| 0x00110608 | second camera update (aim view, inferred) |
| 0x0012B2F8 | `DbgCamControls`, the debug free camera (name from the RPCS3 patch) |
| 0x018C8DB4 | `kFrameDt`, the frame step in seconds |
