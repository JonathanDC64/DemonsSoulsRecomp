# Ultrawide aspect ratios and HUD placement (spike)

Date: 2026-10-18. Status: research only. Nothing was built or run.

Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`). That is the release the port uses, and the one RPCS3's patches target.

Two labels are used below:
- **Read**: seen in the lifted code (`work/recomp`), in the EBOOT bytes, or in our source.
- **Inferred**: a conclusion that still needs an in-game check. The run requests in section 5 cover these.

How it was found:
- reading the lifted functions;
- scans of TOC-relative loads for the aspect floats and for every 1280.0 / 720.0 / 640.0 / 360.0 constant;
- the DsMenuDlg vtable and class-name scan from the UI-timing work (docs/35);
- a comparison with the DeS entries in RPCS3's `patch.yml`.

A load made through some other base register would not show up in these scans.

## Short answer

- **Projection.** The game builds each perspective camera with a 16:9 aspect taken from a data table, and its FOV is vertical. If a wider aspect is written into the two table entries, the view gets wider with the same vertical FOV (Hor+). Culling follows, because it reads the same camera field. RPCS3's "Aspect Ratio" patch does exactly this and nothing more.
- **Picture.** The guest keeps rendering 1280x720. The backend shows the picture at the chosen aspect:
  - first stretched, as RPCS3 does;
  - later with wider host render targets ("anamorphic"), so the picture stays sharp.
- **HUD canvas.** The HUD is drawn on a 1280x720 canvas that one recorded ortho command sets up. Widening that canvas to [-L, 1280+L] keeps the HUD at 16:9 in the centre. That is what RPCS3's HUD patch does. We can do the same with one instruction hook instead of a code patch.
- **Beyond RPCS3.** Two more fixes are needed:
  - The HUD pieces that follow the world (lock-on reticle, floating HP bars) need WorldToScreen and three on-screen tests fixed.
  - Full-screen fades and dims need widening.
- **Later.** An edge-anchored HUD and a HUD size setting come afterwards, through the dialog rects.
- **Effort.**

  | Stage | Days |
  |---|---|
  | First usable version (aspect plus centred HUD) | 5 to 8 |
  | Sharp wide targets | 2 to 4 more |
  | Edge anchoring and HUD size | 5 to 8 more |
  | Everything, including live switching | 13 to 22 |

- **Top risks:**
  - geometry missing at the screen edges because of the game's own visibility system;
  - cameras that set their own aspect (cutscenes, previews);
  - screen-space effects that look stretched at 32:9;
  - the extra cost of the wider view and of more pixels.

## 1. How the game sets its projection aspect

### Read

- **Two default tables.** Each holds 4 floats, 8 bytes apart:

  | Table | fov | aspect | near | far |
  |---|---|---|---|---|
  | 0x018CEFB4 | 0.750492 rad (43.0 degrees) | 1.7778 at **0x018CEFBC** | 0.1 | 3000 |
  | 0x01904334 | pi/2 | 1.7778 at **0x0190433C** | 0.1 | 1000 |

  The two aspect words are the two addresses in RPCS3's "Aspect Ratio" patch (`bef32 0x018cefbc` and `0x0190433c`).
- **Game camera constructors.** func_0012F030 and func_0012F098 copy the first table into the camera:

  | Offset | Field |
  |---|---|
  | +0x50 | fov |
  | +0x54 | aspect |
  | +0x58 | near |
  | +0x5C | far |

  The aspect store (`stfs f12,0x54(r29)`) is at 0x0012F074 in func_0012F030 and at 0x0012F0DC in func_0012F098.
- **Render-context constructors.** func_00CD4E58 and func_00CD54B8 load the second table (at 0x00CD5058 and 0x00CD56B8). They pass it to func_00CD3CE0, which gives a render context its first projection. Inferred: a later 0x39 command replaces it.
- **Who loads the aspect words.** The two game camera constructors are the only code that loads 0x018CEFBC. The two render-context constructors are the only code that loads 0x0190433C.
- **Recording the projection.** func_0012EE08 reads +0x50..+0x5C and records command **0x39** [fov, aspect, near, far] into the game's render command list (func_00CEF0A0 begins a command and func_00CEF0C0 ends it).
- **Building the matrix.** On the render side, func_00CD4C80 handles 0x39. It reads the four floats and calls func_00CD3CE0(fov, aspect, near, far), which builds:
  - x scale = cot(fov/2) / aspect
  - y scale = cot(fov/2)

  So the FOV is vertical.
- **Frustum.** The camera's frustum, func_0012F100, reads the same four fields (+0x50, +0x54, +0x58 and +0x5C). Its one direct caller is at 0x002A54D0.
- **Scene setup.** func_0021D280 sets up the scene:
  - It allocates its cameras on the heap (0x60 bytes, through func_00A941F8) and builds them with func_0012F030.
  - Its callers are 0x0013CF48, 0x0013D5A0, 0x00156DE4 and 0x00176B68. It runs when a mode starts, not every frame.
  - Its twin, func_0021F498, has no callers. RPCS3's FreeCam patch even uses it as a code cave.
- **Video-out aspect.** No code was found that copies the video-out aspect into a camera.

### Inferred

- **Hor+.** Write the aspect A into both defaults before the cameras are built. The result is Hor+. Culling widens with the view, because the projection and the frustum read the same field.
- **Horizontal FOV.** At the default 43 degree vertical FOV:

  | Aspect | Horizontal FOV |
  |---|---|
  | 16:9 | 70 degrees |
  | 21:9 | 86 degrees |
  | 32:9 | 109 degrees |

  The follow camera may set its own fov. If so, the vertical FOV stays whatever the game picks.
- **Timing.** A camera built before the write keeps 16:9, so the write has to come first. Do it right after the EBOOT is loaded, or at the latest in a hook that runs during the boot logos.
- **Changes while playing.** A change during play reaches only cameras built afterwards, which means after the next area load.
- **Live switching.** It can come later:
  1. An instruction hook at 0x0012F074 and 0x0012F0DC records each camera as it is built.
  2. On a change, rewrite +0x54 only where the vtable word (+0x0) is still the camera's and +0x54 still holds the old aspect. That check is based on guest state, so a freed camera is never written.
- **Not covered.** Any code that writes +0x54 after construction is not covered. Cutscene cameras or previews might do this. See R5 and R6.

### Narrower screens (16:10, 4:3)

- **What happens with a smaller aspect.** With a vertical FOV, an aspect below 16/9 crops the sides (Hor-). RPCS3's 16:10 and 4:3 values do this, and the HUD gets squeezed.
- **Proposal.** 16:10 and 4:3 keep 16:9 and letterbox. `blit_to_window` already does this today, so there is no work and no risk.
- **Optional Vert+ later (1 to 2 days).** Fill the screen instead of letterboxing:
  - Raise the vertical FOV so the horizontal FOV stays the same: fov' = 2 * atan(tan(fov/2) * (16/9) / A).
  - Do it in the camera, or in both func_0012EE08 and func_0012F100. Changing only one of those would let culling drift from the picture.
  - Make the HUD canvas taller: top = -T, bottom = 720 + T, with T = (1280/A - 720) / 2.

## 2. How a wider picture reaches the screen

### What the backend does today (read)

The source is `ps3recomp/libs/video/rsx_d3d12_backend.c` with RSX_SCALE (ps3recomp #50); docs/37 describes it.

- **Host sizes.** Guest targets keep the game's sizes. Each target also has a host size from `rt_host_dim(w, h, n)`:
  - n x RSX_SCALE, with one percentage for both axes;
  - a target whose larger side is at most `RSX_SCALE_MIN` (64) stays at 1x.
- **Already per axis.** Most of the rest already handles each axis separately:
  - the viewport scale (`host_w/width`, `host_h/height`, and per target `tgt_sx`/`tgt_sy`);
  - the fill from guest memory in `off_rt_get`;
  - the write-back ratio in `off_rt_downscale`;
  - the sub-viewport copies;
  - the ZPASS normalisation (`s_zq_area` holds each query target's host/guest area ratio).
- **Single percentage.** Two functions use the single percentage: `dr_uvclamp` and `fp_inject_uvclamp`. They keep 2D UVs half a guest pixel inside their sprite on scaled targets.
- **Present.** `blit_to_window` fits the guest display size (`s_d3d.width x s_d3d.height`, 1280x720) into the window. It centres the picture and draws black bars. So a 21:9 window shows a 16:9 picture with side bars today.
- **Port overlay.** The port's overlay (RmlUi) draws on the swap buffer at window size, with a DPI-based scale (`port/ui/src/host.cpp`, `render()`). `port/ui/src/des_ui.cpp` follows the buffer size. The overlay does not depend on the game picture.

### Options

**(a) Wider guest targets** (the game renders, say, 1706x720 itself). Rejected. The game sizes all of these from 1280x720 (see the table in section 3):
- its own buffers;
- its ortho canvases;
- its post passes;
- its movie calls.

The RSX display buffer is also set up through cellVideoOut/GCM at 1280x720. Each of these would have to be found and patched.

**(b) Stretch at present** (RPCS3's "Stretch To Display Area").
- How it works:
  - The guest is unchanged.
  - The wider view is squeezed into 1280x720 (times RSX_SCALE).
  - `blit_to_window` stretches it to the largest rect of aspect A that fits the window.
- Cost: none.
- Downside: horizontal detail drops by k = A / (16/9). That is 1.33 at 21:9 and 2.0 at 32:9. Raising RSX_SCALE compensates, but it also adds vertical pixels.

**(c) Anamorphic host targets.**
- How it works:
  - The guest still sees 1280x720.
  - Each 16:9-shaped target gets a host size of (w x s x k) by (h x s).
  - Host pixels are then square at A, and the present is a plain fit, since host_w / host_h = A.
- Cost: k times the pixels.
- Changes:
  - **`rt_host_dim` gets an axis.** The x axis gets s x k for 16:9-shaped targets (w x 9 == h x 16: 1280x720, 640x360, 320x180, ...) and s for all others. Shadow maps and other square targets stay uniform. Targets of 64 px or less stay at 1x.
  - **UV clamp.** `dr_uvclamp` and `fp_inject_uvclamp` take two scales, and the PSO key holds both.
  - **Init and present.** The `s_d3d.host_w`/`host_h` init and `rsx_scale_apply` use the axis rule. `blit_to_window` fits `host_w : host_h`.
  - **Copy audit.** Check each copy between targets for a same-scale assumption: `off_rt_snapshot`, `screen_copy_capture`, `composite_present` and the sub-viewport capture.
- Relation to docs/37: this refines its "Aspect ratio, afterwards" note. A wider display target alone is not enough. The scene is drawn offscreen first, so the offscreen 16:9 targets need the same factor.
- Rule risk: a 16:9 target that does not hold screen-shaped content, such as an atlas, would also get the factor. That is harmless with normalised UVs. R8 lists the target shapes.

**Recommendation:** (b) first, as part of step A. Then (c) as step B.

### Post effects and other 16:9 assumptions

- **Read:** the guest still renders 1280x720, so its post passes work as they do today:
  - bloom;
  - depth of field (`des_dof`);
  - motion blur;
  - the luminance readback that drives exposure (docs/28).
- **Inferred: wider on screen.** Screen-space sizes are in 16:9 units, so on screen they become k times wider:
  - blur shapes;
  - the sun-flare sprites (driven by ZPASS, docs/29 Y2);
  - any 2D sprite drawn in the scene pass.

  This is mild at 21:9 (1.33) and visible at 32:9 (2.0). It can be fixed per effect by dividing the sprite's x size by k where the sprite is built. That work is not in the estimates. R1 checks it.
- **Inferred: ZPASS.** ZPASS counts are already normalised by area for each target. Option (c) therefore keeps the flare's visibility test right.

## 3. The HUD and menus

### The HUD canvas (read)

- **Recording.** At boot, the init function func_00025CF0 calls func_0072BA80 (at 0x00025D3C). That function builds the menu render context and calls func_00718918 (at 0x0072BAF4).
- **The command.** func_00718918 records the HUD projection once into that context's command list: command **0x3A** with [top, bottom, left, right, near, far], followed by a 0x3C matrix command.
- **The six words.** They are stored one after another at the list cursor (r11):

  | Instruction | Stores |
  |---|---|
  | 0x00718D30 `stw r0` | top = 0 |
  | 0x00718D4C `stfs f0` | bottom = 720 (from 0x018F016C) |
  | 0x00718D68 `stw r0` | left = 0 |
  | 0x00718D84 `stfs f13` | right = 1280 (from 0x018F0174) |
  | 0x00718DA0 `stfs f12` | near = -128 |
  | 0x00718DBC `stfs f11` | far = 128 |

- **Matrix.** func_00CD4800 handles 0x3A on the render side. It turns the six floats into the ortho matrix.
- **Unused twins.** Only func_00718918 (at 0x00718D0C) and func_00717E70 load 0x018F0174. func_00717E70 is a twin with no callers and no data references. func_0072BC38, a twin of func_0072BA80, has no callers either.

### RPCS3's HUD patch (read)

- **The patch.** It is called "Aspect ratio (HUD/Menus)" and comes in a 32:9 version and three 21:9 versions, for BLUS30443 01.00 only. It needs the "Aspect Ratio" patch and "Stretch To Display Area". The "Aspect Ratio" patch's own note says "UI elements will be stretched".
- **What it writes:**
  - 0x018F0174 = 1280 + L;
  - 0x00718CFC (a nop after the call) = `lis r10,hi(-L)`;
  - 0x00718D68 = `stw r10,0(r11)`, so the left word gets -L.
- **Values:**

  | Aspect | right | left |
  |---|---|---|
  | 32:9 | 1920 | -640 |
  | 21:9 (3840x1600) | 1504 | -224 |
  | 21:9 (3440x1440) | 1500 | -220 |
  | 21:9 (2560x1080) | 1493.33 | about -213 |

  All of them follow L = (720 x A - 1280) / 2.
- **Effect.** The canvas [-L, 1280+L] spans the stretched screen. The HUD's 0..1280 therefore sits in the centre 16:9 band at its normal shape.

### Our version of it (proposed)

- **Hook.** The lift cannot take byte patches. Instead, one instruction hook at **0x00718D84** runs after the right word is stored, when r11 holds that word's address. It does three things:
  - writes -L to r11-4 (the left word);
  - writes 1280+L to r11 (the right word);
  - keeps the address for a live change. This works only if the list is replayed each frame; R2 checks that.
- **Data writes.** No data write is needed: 0x018F0174 can stay 1280.
- **Port side.** It needs a line in `port/insn_hooks.txt` and a host function in `port/des_options.cpp`, following the `des_cam_rate` pattern (docs/28). It then needs a re-lift.
- **Other uses of the hook.** The same hook can also:
  - scale the canvas about its centre for a HUD size setting;
  - make the canvas taller for Vert+.

### What else uses 1280x720 canvas coordinates (read)

Every function that loads one of these constants through the TOC:

| Function | Constants | What it is |
|---|---|---|
| 0x00718918 | 0x018F016C (720), 0x018F0174 (1280) | the HUD canvas record (above) |
| 0x00717E70 | same | twin of 0x00718918, no callers |
| 0x0079EE00 | 0x018F3358 (1280), 0x018F3360 (720) | WorldToScreen (below) |
| 0x007B5AB0, 0x007B6178 | 0x018F3D7C (1280), 0x018F3D84 (720) | TargettingSite on-screen test; 0x007B6178 is its update |
| 0x008AEA98 | 0x018F8BF0 (1280), 0x018F8BF8 (720) | FloatingPCGauge on-screen test |
| 0x0012E638, 0x0012E6A8 | 0x018CEF38 (720), 0x018CEF40 (1280) | ortho camera constructors (their default canvas) |
| 0x00096348 | 0x018CBA4C, 0x018CBA54 | another 0x3A record; called three times from 0x000275A8 (0x00027B8C/BC/EC), which runs at boot; one is the boot screen's. Widened with the HUD's canvas since docs/102 section 8 |
| 0x003E0A10, 0x003E8150 | 0x018DE2DC/E4, 0x018DE568/70 | 0x3A records for the scene and post passes, called from the scene setup 0x0021D280 |
| 0x00095918, 0x003E1768, 0x003E8C68 | (same kind) | twins, no callers |
| 0x00182CF0, 0x008AAAF0 | 1280, 720 | pass 1280x720 to 0x00949228, probably the movie player (its state machine is at 0x0094AB90, docs/35); 0x008AAAF0 sits in DsMenuDlgMovieDisplay's code |
| 0x005F65F8, 0x003E5750 | 1280/720, 640/360 | call 0x0094F978 / 0x0094F950 (probably the movie player too) |
| 0x0071B800 | 0x018F02C4 (0), 0x018F02A8 (-1280), 0x018F02B0 (1280) | a default clip rect [0, -1280, 1280, 1280]; called from ScrollMsg's place slot (0x00798B70), 0x007C0F1C and 0x007C3CCC |
| 0x000A07E8, 0x000B15C8 | 1280 | menu code (calls 0x0071B380 / 0x0071B3F0 / 0x0071B440); unclassified |
| 0x007BFB78 | 1280 | five named calls to 0x00027180; looks like debug parameter ranges |
| 0x0021BEE8, 0x002F5E48, 0x0031B2A8, 0x0031B408, 0x007A61D8, 0x00AEFF20, 0x00AF1400, 0x00B3AFE0, 0x00B3B620 | 1280, 720 or 360 | unclassified |

**WorldToScreen (read).**
- **Mapping.** func_0079EE00 projects a world point and maps it to x = (ndc_x x 0.5 + 0.5) x 1280 and y = (-ndc_y x 0.5 + 0.5) x 720. Its constants are 0.5 (0x018F3310), -0.5 (0x018F3350), 1280 (0x018F3358) and 720 (0x018F3360). It is the only function that loads the 1280 and 720 words.
- **Result.** The result is stored by `stvx v31,0,r29` at **0x0079EF7C**.
- **Callers.** The HUD anchor updates 0x0079EFA0, 0x0079F4A8, 0x0079F780 and 0x0079FCB8 call it. func_00144160 calls those updates at 0x00144C1C..0x00144C50.
- **Where the positions go.** The updates fill screen-position slots in a HUD global:

  | Slot | Written / read through |
  |---|---|
  | lock-on, +0x96C | 0x007231B0, 0x00721F08 |
  | an array at +0x8FC | 0x007231F8, 0x00721EE0 |
  | +0xBA0 | 0x00722138 |
  | +0xDB4 | 0x00722430 |

**On-screen tests on those positions (read).**
- **func_007B5AB0 (TargettingSite).** It tests z >= 0, 0 <= x <= 1280 and 0 <= y <= 720.
  - It loads x at **0x007B5AE8** (`lfs f13,0(r3)`).
  - The 0.0 at 0x018F3D8C is shared by the x, y and z tests.
  - It gets the lock-on slot through func_00721F08.
- **func_007B6178 (TargettingSite's update).** The same test, with x loaded at **0x007B61D4**.
- **func_008AEA98 (FloatingPCGauge).** The same shape, with x loaded at **0x008AEB68**. Its 1280 is at 0x018F8BF0.

### What breaks when the canvas is widened (inferred)

- **World markers are pulled inwards.**
  - Cause: today 0..1280 means the full screen. Under the centred canvas, markers are pulled towards the centre by 1280 / (1280 + 2L). At 32:9, a target at the right edge gets its reticle three quarters of the way across. RPCS3's patch does not touch this.
  - Fix: a hook at **0x0079EF7C** sets x' = 640 + (x - 640) x (1280 + 2L) / 1280.
- **Markers in the side bands would be hidden.**
  - Cause: after the fix, the on-screen tests see x in [-L, 1280+L]. They would treat markers in the side bands as off screen.
  - Fix, in two parts:
    1. Hooks after the x loads (0x007B5AE8, 0x007B61D4, 0x008AEB68) add L to f13.
    2. The two 1280 words (0x018F3D7C and 0x018F8BF0, loaded only by these tests) become 1280 + 2L.

    Then x + L >= 0 and x + L <= 1280 + 2L, which is exactly [-L, 1280+L]. The shared 0.0 stays as it is.
- **Full-screen quads cover only the centre band.** These are drawn in canvas units and would leave the side bands uncovered:
  - the fade, DsMenuDlgFadePlate (vtable 0x0186E1E0; it uses the base update 0x0071A6B8);
  - the dim behind menus, DsMenuDlgCommonBackPlate (vtable 0x01870C30);
  - possibly the cutscene bars and the death screen.

  Fix them per class at layout time: x - L and width + 2L, through the rect (vtable +0x50 set, +0x58 get). R3 and R5 check this.
- **These should stay 16:9 and do so for free:** loading screens, the title, the logos, the menus, and the FMVs if they are drawn on the canvas (R4).
- **Other 2D contexts.**
  - What they are: 0x00096348 records its own 1280x720 canvas three times at boot.
  - Problem: anything drawn there would stay stretched, and if it is HUD text, out of line with its panels.
  - Fix: the same hook pattern on that record. R3 and R4 show whether this is needed.
- **Clip rects.**
  - The game clips some text itself in canvas units; 0x0071B800 holds the default rect. With a centred HUD nothing moves outside 0..1280, so nothing changes.
  - With edge anchoring, text moved past 0 or 1280 could be clipped. That is an H3 risk.
  - The backend ignores the guest scissor (docs/37), so there is no pixel-scissor problem.

### Dialog layout and anchors

- **Read: the dialog framework.** Menus and HUD pieces are EzMenu dialogs (DsMenuDlg* classes). Each class's name sits beside its vtable.
  - **Updates.** A class's update is in a vtable slot. A dialog without its own update uses the base update 0x0071A6B8, which calls the EzMenuDlgObj update 0x00F58000. Layout objects have a base update at 0x00F63698 (docs/35).
  - **Rects.** The rect is read through vtable +0x58 and set through +0x50.
  - **Placement.** Slot 5 places a dialog when it is drawn. docs/35 logged the on-screen x there.
- **Read: the HUD classes** (from the vtable scan):
  - DsMenuDlgPCGauge;
  - DsMenuDlgShortcutItem, ShortcutMagic and ShortcutSelect;
  - DsMenuDlgGameInfoPlate;
  - DsMenuDlgBossGauge;
  - DsMenuDlgKeyGuide;
  - DsMenuDlgGetItem;
  - DsMenuDlgFEGetSoul;
  - DsMenuDlgTargettingSite;
  - DsMenuDlgFloatingPCGauge;
  - DsMenuDlgBloodMsgDisplay and SOSMsgDisplay;
  - DsMenuDlgSaveIcon;
  - DsMenuDlgTextEffect;
  - DsMenuDlgTalk;
  - DsMenuDlgFadePlate;
  - DsMenuDlgMovieDisplay;
  - DsMenuDlgNowLoading.
- **Proposed anchor table.** The positions are from memory of the game. Confirm them with a placement log (R7).

  | Anchor | Dialogs |
  |---|---|
  | Left edge | PCGauge (HP, MP, stamina), the Shortcut* equipment slots, GetItem (pickups) |
  | Right edge | GameInfoPlate (souls), FEGetSoul (soul gain), SaveIcon; KeyGuide wherever it sits |
  | Centre | BossGauge, Talk, TextEffect, the message displays, every menu, NowLoading, MovieDisplay |
  | Widened, not moved | FadePlate, the menu back plates |
  | Follows the world | TargettingSite, FloatingPCGauge (the WorldToScreen fix) |

- **Mechanism.**
  - Add -L or +L to the x of root dialogs of the anchored classes, through the rect set or the place slot. Children should follow their parent.
  - Unknown: whether child rects are relative to the parent.
  - First test: move PCGauge's root rect by -L and see whether the whole gauge group moves.

### HUD size

- **Centred size (cheap).**
  - The canvas hook scales the canvas about its centre, with σ the HUD size (below 1 shrinks the HUD):
    - left = 640 - (640+L)/σ
    - right = 640 + (640+L)/σ
    - top = 360 - 360/σ
    - bottom = 360 + 360/σ
  - WorldToScreen and the on-screen tests use the same mapping in both axes. The 720 words of the tests are loaded only by the tests too.
  - Result: everything shrinks towards the screen centre.
- **Corner-anchored size.**
  - This is what the roadmap asks for: each anchored root scales about its own corner.
  - It needs a scale for each dialog in the EzMenu draw. It is unknown whether one is exposed; the layout animation player may already scale parts.
  - Cost: H3's estimate plus 1 to 2 days.

## 4. Plan, effort and risks

| Step | What | Days | Main risks |
|---|---|---|---|
| A | Aspect setting. Write A to 0x018CEFBC and 0x0190433C before the first camera is built. `blit_to_window` fits A (stretch). 16:10 and 4:3 letterbox at 16:9. Settings entry. regress.sh forced to 16:9. | 2 to 3 | pop-in at the edges; cameras with their own aspect; captures squeezed to 16:9 |
| H2 | Centred HUD. Canvas hook at 0x00718D84. WorldToScreen hook at 0x0079EF7C. On-screen tests: 3 hooks and 2 data words. Widen FadePlate and the back plates. Check the text paths. | 3 to 5 | 2D contexts this does not cover (0x00096348); full-screen pieces found one at a time |
| B | Anamorphic host targets: axis rule in `rt_host_dim`, two scales for the UV clamp, init and apply, present from the host size, copy audit. | 2 to 4 | a copy path that assumes one scale; memory and fps cost of k |
| H3 | Edge-anchored HUD. Anchor table by class, offset the roots, check the clip rect, test every HUD state (normal, soul form, lock-on, boss, pickups, messages, phantoms, Nexus, death). | 4 to 6 | children not relative to the parent; dialogs rebuilt every frame; clip rects |
| H4 | HUD size. Centred through the canvas; corner-anchored inside H3. | 1 to 2 (plus 1 to 2 with H3) | small text gets hard to read |
| Live | Change aspect and HUD without a restart: camera list from the constructor hooks, rewrite the canvas words, `rsx_scale_apply` for the targets. *Done 2026-10-21 (docs/79): the camera list as planned; the HUD through the render context's matrix, since the canvas words are not replayed (section 7).* | 1 to 2 | freed cameras (check the vtable and the old value first) |
| Vert+ | Optional: 16:10 and 4:3 fill the screen. FOV in the camera, taller canvas. | 1 to 2 | every place that writes fov |

**Totals:**

| Stage | Steps | Days |
|---|---|---|
| First usable | A + H2 | 5 to 8 |
| Sharp | + B | 7 to 12 |
| The roadmap's full ask (corner-anchored, resizable HUD) | + H3 + H4 | 12 to 20 |
| Everything | + live switching | 13 to 22 |

The instruction hooks need a re-lift. Batch them, so the lift runs once per step.

**Proposed settings** (Display):
- **Aspect ratio:** Auto (screen), 16:9, 16:10, 21:9 (2560x1080), 21:9 (3440x1440), 21:9 (3840x1600), 32:9.
- **HUD:** Stretched, Centred, Corners.
- **HUD size:** 50 to 100%.

The defaults are 16:9, Centred and 100%, which gives the original look; this follows the "original behaviour by default" rule. In the first version, changes apply on restart.

### Other areas

- **Menus.** Menus are dialogs on the canvas, so they are centred under H2 and stay centred under H3. The menu dims need widening.
- **Cutscene letterboxing.** How the bars are drawn is unknown:
  - If they are canvas quads, they cover only the centre band under H2, and need the same fix as the fades.
  - If they come from the scene's viewport, they follow the backend scale.

  R5 checks this.
- **Loading screens and the title.** These are dialogs, so they are pillarboxed at 16:9 under H2. The boot screen is not drawn by the main loop (docs/35). If it uses another 2D context, it stays stretched (R4).
- **FMV.** The movie calls ask for 1280x720. The FMV is pillarboxed if it is drawn on the canvas (R4).
- **Minimap.** The game has none. The HUD pieces that track the world are the lock-on reticle and the floating HP bars, both fixed in H2.
- **Port overlay.** It is window-sized and DPI-scaled, and does not depend on the game picture. Nothing changes apart from the new settings (`port/ui/src/settings_window.cpp`, `settings.cpp`).
- **Screenshots and tools.**
  - The port's screenshot captures the swap buffer, so it gets the full wide picture.
  - Captures of the render target (F9, `CELLMARK_DUMP_DIR`, `capdiff.py`, regress) are squeezed to 16:9 under step A and have the right shape under step B.
  - `regress.sh` and its baseline must run at 16:9, which is the default.

### Risks

1. **Pop-in at the edges.** The game decides what to draw for each area, and that was tuned for a 70 degree view. A wider FOV may show missing walls, or enemies that appear late at the left and right edges. This stays unknown until R1. If it happens, limit the choices to 21:9 or find the visibility source.
2. **Cameras with their own aspect.** Cutscenes or the character-creation preview could look stretched or squashed (R5, R6).
3. **Screen-space effects stretched by k.** Flare and blur shapes. This is cosmetic.
4. **Other 2D contexts.** Anything drawn in 0x00096348's three canvases stays stretched (R3, R4). Done (docs/102 section 8): every record of the whole canvas is widened as the HUD's; the boot screen's logo was the one seen stretched.
5. **Cost.** A wider view draws more, and step B adds k times the pixels (R8).
6. **Re-lifts.** The instruction hooks need a re-lift, which is slow. Group the hooks.

## 5. Run requests

None of these were run. They need scratch knobs, built on a scratch branch and not committed:

| Knob | What it does |
|---|---|
| `DES_AR=<A>` | writes A to 0x018CEFBC and 0x0190433C before the first camera is built |
| `RSX_FIT=<A>` | `blit_to_window` fits a rect of aspect A, i.e. stretches |
| `DES_HUD=centre` | the 0x00718D84 hook; it logs the two word addresses |
| `DES_HUDMARK=1` | the 0x0079EF7C hook, the three on-screen-test hooks, and 0x018F3D7C / 0x018F8BF0 set to 1280+2L |

Two more tools are needed:
- a way to rewrite two guest words live, for R2 (a knob read at each flip, or the console);
- a write watch on one guest address, for R6.

Use the usual `play.sh` line (pinned save, scratch dir, `RSX_PRESENT` from the lock state) plus the knobs. For window sizes:
- a 2560x1080 window for 21:9;
- a 3840x1080 window for 32:9;
- a window on a 16:9 monitor also works; the picture is just smaller.

**R1. Hor+ and culling.**
- Knobs: `DES_AR=2.3704 RSX_FIT=2.3704`, then 3.5556 for both.
- Where: the Palace start and the first gate, turning slowly, and the Nexus.
- What each result means:

  | Result | Meaning |
  |---|---|
  | Wider view, and round things stay round | Hor+ works through the defaults |
  | Walls, props or enemies pop in at the side edges while turning | the game's visibility is tied to 16:9 (risk 1) |
  | Shadows stop short of the edges | the shadow fit has its own aspect |
  | The title or menu backgrounds are squeezed but the game is right | the write came after some cameras were built; move it earlier |
  | Flare or bloom look wider than tall at 32:9 | risk 3; cosmetic |

**R2. Is the canvas list replayed?**
- Knobs: R1 plus `DES_HUD=centre`. While playing, rewrite the two logged words for another L, for example from 640 to 213 at 32:9.
- What each result means:

  | Result | Meaning |
  |---|---|
  | The HUD moves on the next frame | the list is replayed every frame, and live HUD switching is cheap |
  | Nothing changes | the list is cached or recorded elsewhere, and the HUD setting stays restart-only |

**R3. Centred HUD look.**
- Knobs: `DES_AR=3.5556 RSX_FIT=3.5556 DES_HUD=centre`.
- What to check:
  - HP, MP and stamina, the equipment slots and the souls count sit in the centre 16:9 band at their normal shape.
  - Text sits on its panels. If text is elsewhere or stretched, it comes from another 2D context.
  - The scrolling key-guide text in the inventory is clipped at its window.
  - Fades (a warp from the Nexus, death, an area load) cover the full width, or leave side bands.
  - The dim behind the menus covers the full width.
  - The boss bar sits in the centre.

**R4. FMV, loading screens and the title.**
- Knobs: R3 with `PS3_SAIL_MOVIES=1` and without `DES_SKIP_INTRO`.
- Watch the logos, the opening movie, the title and a loading screen.
- What each result means:

  | Result | Meaning |
  |---|---|
  | Pillarboxed | good |
  | Stretched | drawn on a separate path, which must be found |

**R5. Previews and cutscenes.**
- Knobs: R3.
- What each result means:

  | What you see | Meaning |
  |---|---|
  | The character-creation preview (new game) is squashed | that camera takes the defaults but draws into a 16:9 panel |
  | Shapes in a real-time cutscene (the save-load cutscene, or a boss intro) are stretched | the cutscene sets its own aspect |
  | Letterbox bars only in the centre band | the bars are canvas quads |

**R6. Writes to the camera aspect.**
- Knobs: `DES_AR=2.3704`, plus a write watch on the follow camera's +0x54. Find the camera through a log from the 0x0012F074 hook.
- Play for a minute, including a menu and a cutscene.
- What each result means:

  | Result | Meaning |
  |---|---|
  | No writes | the defaults are enough |
  | Writes | note the writer's address; it needs its own fix |

**R7. Markers and anchors.**
- Knobs: R3 with `DES_HUDMARK=0`, then with `DES_HUDMARK=1`.
- Lock on to an enemy and walk until it is near a side edge. Hit an enemy to show its floating bar.
- What each result means:

  | Result | Meaning |
  |---|---|
  | With 0, the reticle is pulled towards the centre | confirms the inference about RPCS3's patch |
  | With 1, the reticle stays on the target up to the edge and stays visible in the side bands | the marker fix is complete |

- For H3: a placement log of the root dialog rects (like docs/35's `DES_PLACELOG`) fills in the anchor table.

**R8. Cost and target shapes.**
- Knobs: `VBLANK_MHZ=240000 DES_FPS=240` in the Palace. A/B between 16:9 and `DES_AR=3.5556 RSX_FIT=3.5556`, with the same RSX_SCALE.
- Add a log line in `off_rt_get` that prints each target's w x h.
- What to get:
  - the fps drop from the wider view alone (CPU and RSX);
  - the list of target shapes. Those that are 16:9 decide step B's rule.

## 6. Address table

| Address | What | Use |
|---|---|---|
| 0x018CEFBC | default aspect for game cameras (1.7778) | write A (step A) |
| 0x0190433C | default aspect for a render context's first projection (1.7778) | write A (step A) |
| 0x0012F074, 0x0012F0DC | aspect stores in the camera constructors (+0x54) | hooks for the live camera list |
| 0x0012EE08 | records 0x39 from camera +0x50..+0x5C | Vert+ (with 0x0012F100) |
| 0x0012F100 | camera frustum, reads +0x50..+0x5C | keep in step with the projection |
| 0x00CD4C80, 0x00CD3CE0 | 0x39 handler and the perspective matrix (vertical FOV) | reference |
| 0x00718918 | records the HUD canvas (0x3A) once at boot | reference |
| 0x00718D84 | stores the canvas right word (r11) | canvas hook: left = -L, right = 1280+L, centred size |
| 0x00CD4800 | 0x3A handler (ortho matrix) | reference |
| 0x0079EE00 / 0x0079EF7C | WorldToScreen / its result store (r29) | marker hook |
| 0x007B5AE8, 0x007B61D4, 0x008AEB68 | x loads in the on-screen tests | hooks: f13 += L |
| 0x018F3D7C, 0x018F8BF0 | 1280 in those tests | write 1280+2L |
| 0x0186E1E0, 0x01870C30 | FadePlate, CommonBackPlate vtables | widen the rect |
| 0x0071A6B8, 0x00F58000, 0x00F63698 | dialog base update, EzMenuDlgObj update, layout base update | H3 anchors |
| 0x0071B800 | default clip rect [0, -1280, 1280, 1280] | H3 risk |
| 0x00096348 | second 1280x720 canvas record (three contexts at boot) | widened, docs/102 section 8 |
| 0x00949228, 0x0094F978 | movie player calls with 1280x720 | check in R4 |

## 7. Corrections from the first implementation

Found while building the first ultrawide version (branch `feat/ultrawide`, 2026-10-18). They replace what sections 3 and 6 say:

- **Plate rects.** FadePlate's and CommonBackPlate's rect is stored in the dialog at +0x74 (left, top, right, bottom). It is not set through vtable +0x50 and read through +0x58, as "Dialog layout and anchors" and the full-screen plates fix say.
- **A fourth on-screen test.** 0x007A61D8, listed as unclassified in the 1280/720 table, is a fourth on-screen test. It loads x at 0x007A6264. It needs the same treatment as the other three; the implementation hooks all four.
- **WorldToScreen's callers.** 0x0079F780 does not call WorldToScreen. Its callers among the HUD anchor updates are 0x0079EFA0, 0x0079F4A8 and 0x0079FCB8.
- **The canvas record is not replayed (R2, measured 2026-10-21, docs/79).** After boot the six words are nowhere in guest memory but the game's constant. The 0x3A handler (0x00CD4800) turns them into an ortho matrix at +0x90 of the render context once, and that matrix is what the HUD is drawn with. A live HUD change rewrites that matrix (`des_hud_ctx`), not the canvas words.

## 8. Known limitations

- **Three markers in the left side band.** With the HUD centred at a wide aspect ratio, three small markers show in the left band: a red triangle, a green triangle and a blue ring (seen while testing the live aspect change, docs/79, and also at a 21:9 start). They look like the developers' own icons, placed outside the 16:9 canvas where a 16:9 screen never shows them. What the game uses them for is unknown, so hiding them could change something else. Left as they are for now.
  - Docket: find which HUD element draws them (its dialog and the code that places it) and what it is for, then decide whether to keep them off screen at wide ratios.
  - Done (docs/102): they are the HUD's "HP icon", "SP icon" and "MP icon" at x = -168, and are no longer drawn on a widened canvas. docs/102 also moves the HUD's corner groups to the screen's corners (`DES_HUD` "", Corners).
- **The boot screen was stretched** at a wide aspect ratio (player report, 2026-10-24): it is drawn on one of 0x00096348's canvases. Done (docs/102 section 8).
- **The HUD in the corners showed during loads**: the loading screen covers only the 16:9 band. Done (docs/102 section 3): the corner groups are not drawn while it is.
