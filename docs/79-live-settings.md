# Settings that applied only at the next start (2026-10-21)

Nine rows in the settings window said "Takes effect the next time you start the game". This note makes four of them live and says why the other five stay as they are.

Labels as in docs/48: **measured** (from a run), **read** (seen in the code or the lift), **inferred**.

## Short answer

| Row | Setting | Now | How |
|---|---|---|---|
| Aspect ratio | `DES_ASPECT` | **live** | the cameras built so far get the new aspect; the default words, the HUD and `RSX_ASPECT` follow (section 2) |
| HUD | `DES_HUD` | **live** | the HUD's matrix is rewritten in place (section 2.3) |
| Boot logos | `DES_SKIP_INTRO` | **live** | read at each update of the logo dialog. The logos only play at start, so a change shows on a logo that is on screen, or at the next start |
| Continue automatically | `DES_AUTO_CONTINUE` | **live** until the first level loads | read at each pad poll: turning it on at the title presses through to the last save, turning it off stops the presses. After the first level there is nothing left for it to do |
| Online | `DES_ONLINE` | next start | section 3 |
| Server, Custom address | `DES_NET_SERVER`, `DES_NET_CUSTOM` | next start | section 3 |
| Online name | `DES_NET_NAME` | next start | section 3 |
| P2P port | `DES_NET_P2P_PORT` | next start | section 3 |

The four live rows now apply as soon as they change, from the settings window or the console's `setenv`. A change of aspect ratio while playing gives the same picture as starting with that aspect ratio (section 4).

## 1. What "applies at start" meant for each

- **`DES_ASPECT` and `DES_HUD`** (docs/39). At start, `des_aspect.cpp` writes the aspect into the game's two default words, before the first camera is built. Each camera copies the default into itself when it is built, so a later write reached only the cameras built after it. The HUD canvas is set once, when boot records it.
- **`DES_SKIP_INTRO`** was read once into a static, and the logo dialog's hook was installed only when it was on.
- **`DES_AUTO_CONTINUE`**: the same, for the pad hook.
- **The network rows** set the runtime's network knobs once at start (`apply_network` in `port/ui/src/settings.cpp`).

## 2. A live aspect ratio

### 2.1 What a running game holds (measured, console, in the first level)

- `find32 3FE38E39` (16:9 as a float) found 22 words. 15 of them are cameras: fov 0.75, the aspect, near 0.1 and far 3000 at +0x50 to +0x5C, with a vtable at +0 (0x018561B0, 0x01856390, 0x018565D8). Two are the default words. The rest are copies inside per-frame data, which alternate between two buffers 64 KB apart.
- A read and write guard on one camera page showed the game writing the fov and aspect of some cameras every frame (0x002190E0): they are copies of another camera.
- **Poking every 16:9 word to 21:9** gave a wider view at once, squeezed into 1280x720 as with `DES_ASPECT=21:9` at start.
- **Poking only the cameras with a vtable** (and the two default words) did the same: the per-frame copies and the frame data followed by the next frame.

So the cameras built before the change are what a live change has to reach.

### 2.2 The cameras

- **Listing them.** Both base camera constructors copy the default table into the camera (0x0012F030, 0x0012F098). An instruction hook after each one's store of the aspect (`des_cam_ctor`, 0x0012F074 and 0x0012F0DC, r29 = the camera) puts the camera on a list. The list keeps up to 1,024 cameras. When it is full, the cameras that no longer hold the current aspect go first, then the older half.
- **One constructor is left out.** 0x000D5980 builds a camera with the base constructor and then gives it its own target's shape (width over height, 0x000D5B24). Starting with a wider aspect never changes that camera, so `des_cam_own` (after that store, r19) takes it off the list.
- **A change.** The new aspect goes into the two default words, and into every listed camera that holds the old one exactly, with a fov between 0.01 and 3.1 and 0 < near < far. A camera that was freed, and its memory reused for anything else, fails that test and is skipped.
- **When.** `des_aspect_poll` runs at each flip request and does nothing unless the console's `setenv` count changed. The settings window's change goes through `ps3_setenv`, so it bumps that count. Each camera's aspect is one 4-byte write, so a camera never sees half a value. At worst one frame mixes old and new cameras.

### 2.3 The HUD

- **The record is not replayed** (measured). docs/39 left this open (R2). After boot, the HUD canvas's six words [0, 720, 0, 1280, -128, 128] are nowhere in guest memory but the game's constant. The 0x3A handler (0x00CD4800) turns them into an ortho matrix at +0x90 of the render context once (read), and the list's memory is reused.
- **The matrix** (measured). 116 copies of that matrix were in memory, and 11 stayed at one address. Widening each of the 11 in turn moved the HUD for one of them only: the source the HUD is drawn from. Poking it moved the HUD on the next frame, and nothing put it back.
- **Finding it in code.** `des_hud_canvas` now notes where the record's first word is, whatever the canvas. A new hook in the handler (`des_hud_ctx`, after 0x00CD4854: r5 = the command's first word, r3 = the context) notes the context that handles that record.
- **A change** writes the matrix's x scale (+0x90) and x offset (+0xC0), computed as the handler computes them from [-L, 1280 + L], while the matrix still holds the values for the old canvas and its y scale is -2/720.
- WorldToScreen, the on-screen tests and the plates already read L at each call. The plates' wrappers are now installed at start whatever the HUD is, and do nothing at 16:9.

### 2.4 `RSX_ASPECT`

- It follows the aspect ratio unless it was set at start. It was already a live knob in the D3D12 backend.
- The window keeps its size, and the picture is fitted into it. Only the first window is opened at the aspect ratio's shape.

### 2.5 Not followed

- **A render context's first projection** (0x0190433C) in contexts built before the change. The constructors 0x00CD4E58 and 0x00CD54B8 make it from that default word, once. The game draws through the cameras' projections: in the poke test the wider view was right with those contexts left alone.

### 2.6 Needs a re-lift

The four new hooks are in `port/insn_hooks.txt`. A build from an older lift fails to link without them, so re-lift with the recipe in the README after this merges.

## 3. Why the network rows stay "at the next start" (read)

- **Online.**
  - Off to on: the runtime picks the network functions once at boot. With `PS3_NET_ONLINE` set, `ppu_sysprx_register` registers the host sockets under libnet's NIDs before the offline model does, and the first registration of a NID wins (`libs/network/sysNet.h`). An offline session has no host sockets to switch to.
  - On to off: the game has signed in and holds its PSN session. Taking the network away mid-session is a disconnection to the game, not a setting.
- **Server and Custom address.** `PS3_NET_HOSTS` is read at each name lookup, so a change would reach the next connection. But the game signs in to the server once a session and keeps that session's state. Its next requests would go to a server that never saw the sign-in.
- **Online name.** The runtime settles the NP identity on first use and keeps it (`np_psnr_identity`), and the game keeps its own copy of the NP ID from the start.
- **P2P port.** The P2P socket is bound to it when the game opens it.

Each of these is part of the game's sign-in, which it does once a session, so the rows keep their note.

## 4. Tests (measured)

- **Setup.** A developer build of this branch on fork master, on the regression save, in the first level at 30 fps. The session was locked, so the game drew without presenting (`RSX_PRESENT=0`) and the frames were checked in the frame dumps.
- **How.** Each change was a `setenv` on the debug console, which goes through the same `ps3_setenv` as the settings window. `find32` then showed which words hold which aspect.

| Test | Result |
|---|---|
| 16:9 start, then live 21:9 | All 22 aspect words hold 21:9 (the cameras, the two default words, the frame-data copies), and none hold 16:9. Log: "12 of 16 listed cameras changed", HUD moved. The frame shows the wider view, with the HUD in the centre band |
| Live 32:9, then HUD Stretched, then Centred | Each shows on the next frame |
| Live back to 16:9 | All 22 words hold 16:9, none 21:9 or 32:9. The HUD is where it was at the 16:9 start |
| 20 switches between 32:9 and 16:9, 0.4 s apart | No crash. All 22 words hold 16:9 afterwards |
| 21:9 start, live 16:9, live 21:9 | The same 22 words switch each time. The 21:9 start's frame and the live 21:9 frames match (mean difference in the HUD strip: 0.7 and 1.3 out of 255) |
| Boot logos on at start; `DES_SKIP_INTRO=1` at the first notice, before the logos | No logo frames. The level loaded 8 s after the presses began; with the logos left on it took 36 s |
| Continue automatically off at start, on at the notice | The level loaded 8 s later |
| Continue automatically on at start, off at the first pad poll | No level in 45 s. Turned on again: the level loaded 36 s later (the logos played first) |
| `regress.sh` at 30 fps, `REGRESS_AB="DES_ASPECT=2560:1080"` | All four shots ok (at most 2.6% changed). `s5_ab` is wider with the HUD centred; `s6_back` looks as before |

**Not run:**
- a presenting window, so no black-window check (the session was locked);
- the settings window's own controls (the change to `apply_env` that lets the two rows' empty values through was read, not run);
- a change during an area load or a cutscene;
- the camera list across many area loads (it is capped at 1,024 by design).

**Seen, not caused by this change.** With a centred HUD, at a 21:9 start too, three small markers show in the left side band: a red triangle, a green triangle and a blue ring. They are drawn left of the 16:9 canvas, where the game never shows them.
