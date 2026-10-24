# 102. HUD size, the HUD in the corners, hiding parts of it, and the HUD editor (2026-10-24)

New settings for the in-game HUD: a size, its four corner groups pushed out to the screen's corners on a wide
picture, a switch to hide each group, and an editor that moves and resizes each one on the game's picture. Labels: **measured** (a run says so), **read** (seen in the code, the EBOOT
or a heap dump), **inferred**.

## Short answer

- **Settings** (Game tab, section HUD; all change while playing):

  | Row | Option | Values |
  |---|---|---|
  | HUD size | `DES_HUD_SIZE` | percent, 50 to 150 in the window (the option takes 25 to 200); default 100 |
  | HUD on wide screens | `DES_HUD` | `""` Corners (new default), `centre` (the old centred 16:9 band), `stretch` |
  | Health, magic, stamina | `DES_HUD_BARS` | 1 shows (default), 0 hides |
  | Area name | `DES_HUD_AREA` | as above |
  | Equipment | `DES_HUD_EQUIP` | as above |
  | Souls | `DES_HUD_SOULS` | as above |
  | Edit HUD | `DES_HUD_<PART>_POS`, `DES_HUD_<PART>_SIZE` | a button: the HUD editor (section 9) |

- **Each group grows or shrinks from its own corner** of the screen; the boss's health bar from the bottom middle.
  At 16:9 with the defaults nothing changes: the game draws its HUD as before (measured, section 4).
- **Wide pictures.** With Corners, the bars and the equipment move to the left edge, the area name and the souls to
  the right edge. Menus, pop-ups and the boss bar stay in the 16:9 band in the middle, as before (measured).
- **The three stray markers are gone.** docs/39 section 8 listed a red triangle, a green triangle and a blue ring in
  the left band on a wide picture. They are the HUD's own "HP icon", "SP icon" and "MP icon", placed at x = -168,
  left of the 16:9 canvas, where the PS3 never shows them (read). They are no longer drawn on a widened canvas.
- **The HUD editor** (section 9) shows the game with a box over each part: drag to move, drag a corner to resize,
  the eye to hide or show, Reset all to put everything back. Each part stays on screen. The cursor shows what a drag
  will do.
- **HUD text on a wide picture is right** (section 7): it is squeezed with the icons and stretched back with them. An
  earlier note in this branch said otherwise; a closer look showed it was wrong.
- **Text now grows and shrinks with its group** (section 6). The first version moved text but kept its size: at 50%
  the item count ran into the item name. A player test found it; section 4 had said "text included" by mistake.
- **The loading screen hides the corner groups** (section 3). On a wide picture the loading art covers only the 16:9
  middle, so the bars, equipment, area name and souls showed in the side bars during a load. A column of soul wisps showed
  at the right edge as well: the loading screen's own animation frames, parked off the 16:9 canvas.
- **The boot screen is no longer stretched** on a wide picture (section 8). Its logo now sits at the left edge of the
  16:9 middle, like the menus.

## 1. How the HUD is built (read)

- **One dialog.** The in-game HUD is one EzMenu dialog, `DsMenuDlgPCGauge` (vtable 0x018698A0), the size of the
  1280x720 canvas. Poking its rect moved the whole HUD at once: bars, area name, equipment, souls (measured with the
  console's `poke32`).
- **Its children.** 86 layout objects: array at dialog +0x0C, count at +0x10. Each object's record (+0x1C) starts with
  a pointer to its name in the layout file, in UTF-16. One name has a full-width letter: `F20-00_arms_ｒ`.
- **Hosted dialogs.** Some children host a dialog of their own: the four equipment slots (`F20-00_magic`,
  `F20-00_item`, `F20-00_arms_l`, `F20-00_arms_ｒ`, each a `DsMenuDlgFeShortcut`), the souls (`soul_param`, a
  `DsMenuDlgFEGetSoul`), the boss bar, the lock-on marker (`DsMenuDlgTargettingSite`) and the enemy bars
  (`DsMenuDlgFEEnemyGauge`).
- **The groups**, by name (`kPieces` in `port/des_hud.cpp`):

  | Group | Grows from | Pieces |
  |---|---|---|
  | Health, magic, stamina | top left | `plate_*`, `bar_*`, `outline_*`, `*_endbar`, the eye emblem (`icon_eye_normal`, `_phantom`, `_ghost`, `_blackphantom`, `piece17`), the status icons (`icon_state00..18`, `icon_broke_00..18`) |
  | Area name | top right | `map_name`, `eye_BG_s`, `icon_eye_normal1`, `_phantom1`, `_ghost1` |
  | Equipment | bottom left | `F20-00_*` (the four slots), `text_magic_name`, `text_item_name` |
  | Souls | bottom right | `soul_param` |
  | Boss bar | bottom middle | `F20_boss_HP0`, `F20_boss_HP1` (sized, never hidden) |
  | Off the canvas | (as the bars) | `HP icon`, `SP icon`, `MP icon` |
  | Unchanged | | `F20_LockCursor`, `enemy_HP0..7` (they follow the world), `F20_state_abnormal` (a plate in the middle), any name not listed |

  The log names the counts once per HUD dialog: `[des] HUD: dialog 0x48601390, 86 pieces: bars 59, area 5,
  equipment 6, souls 1, boss 2, off the canvas 3, others 10` (measured, Nexus and 1-1).

## 2. How a dialog is drawn (read)

- **The draw.** Every dialog's draw is vtable +0x4C, 0x00F60BD0. It maps the dialog's layout rect (+0x84: left, top,
  right, bottom) onto its current rect (+0x74), scale and offset together, and draws each child through that. Text
  moves with it but keeps its size: its glyphs are sized by the font (section 6).
- **A child is drawn** only while bit 0x40000000 of its +0x18 is set (the object draw, 0x00F62A88).
- **0x00F60BD0 is only a dispatcher.** It picks the real draw (0x00F5EAC0, or 0x00F5F878 when +0x9C is set) and
  leaves it on the lift's trampoline, as a tail call. Ghidra shows the two as one function. A wrapper that calls the
  lifted 0x00F60BD0 and then puts things back undoes them before the draw runs: the first build did exactly that and
  changed nothing on screen. The wrapper now drains the trampoline after each call (as `des_chrins.cpp` does).
- **Nothing else lasts past the draw.** The dialog's own shape (0x018840E8) draws nothing (its draw is 0x00F9CCD0,
  `blr`), and the draw restores the renderer's matrix and colour at its end.

## 3. What the wrapper does (`port/des_hud.cpp`)

- 0x00F60BD0 is wrapped with `ppu_register_function` (the vtable call goes through it; nothing calls it directly).
  Every dialog but PCGauge goes straight to the game's draw.
- For PCGauge: each group gets a rect, the dialog's rect grown by the size about the group's anchor and moved by the
  corner shift. Groups with the same rect share one draw. For each draw, only that group's objects keep their bit;
  then the rect and the bits go back, so the game never sees them changed. Objects the game itself hides stay hidden.
- With size 100%, all groups shown and the game's canvas (16:9, or `stretch`), it calls the game's draw once and
  does nothing else.
- The corner shift is the canvas's extra width on each side, L = (720 x A - 1280) / 2 (`des_hud_side_width()` in
  `des_aspect.cpp`): -L for the left groups, +L for the right ones, none with `centre`.
- The knobs are read again after any setenv (`ps3_env_gen`), so the settings window changes them while playing.
  A change is logged: `[des] HUD: size 80%, corners on a wide picture, hidden: area name`.
- **The loading screen.** `DsMenuDlgNowLoading` (vtable 0x0186FBC0) is drawn over the HUD; in the 16:9 game it covers
  it. On a wide picture it covers only the middle band. Its draws go through the same wrapper, which notes them; while
  it was drawn in this frame or the one before, the groups moved to the corners are not drawn. They come back with
  the game when the loading screen goes.
- **The loading screen's soul frames.** Its parts `soul_0` to `soul_99` hold the frames of the two soul icons'
  animation (`des_options.cpp`, `nowloading_souls_rate`) and sit right of the 1280-wide canvas, where a 16:9 screen
  never shows them. A widened canvas showed them as a column of wisps at the right edge (also before this change).
  While the canvas is widened they are not drawn; the two icons still take their images from them.
- **Text** in a group drawn at another size is scaled with it (section 6).

## 4. Tests (measured, 2026-10-24)

The archstone save in the Nexus, offline, player build of this branch, frames saved from the console
(`CELLMARK_DUMP_NOW`), settings changed live with `setenv`:

| Shot | Settings | Seen |
|---|---|---|
| default | 16:9, 100% | the game's HUD, unchanged |
| 75% / 150% | `DES_HUD_SIZE` | each group smaller / larger towards its corner; text moved but kept its size (this row first said "text included": wrong, see below) |
| hide area | `DES_HUD_AREA=0` | the area name and its emblem gone, the rest as before |
| hide all | all four 0 | no bars, area, equipment or souls |
| 21:9 Corners | `DES_ASPECT=2560:1080` | bars and equipment at the left edge, area and souls at the right; no stray markers |
| 21:9 Centred | `DES_HUD=centre` | the old centred band, without the markers |
| 21:9 Corners 80% | | both together |
| 32:9 Corners | `DES_ASPECT=32:9` | groups at the far corners |

The game ran on to the end of each run; nothing in the log.

Second round, after a player test (the same save and build kind, this branch):

| Shot | Settings | Seen |
|---|---|---|
| 50% | `DES_HUD_SIZE=50` | "The Nexus", the souls' 0, "Crescent Moon Grass" and the item count half size; the count no longer runs into the name |
| 150% | `DES_HUD_SIZE=150` | the same text one and a half times the size, the whole item name shown |
| 21:9, 60% | `DES_ASPECT=2560:1080` | text at 60% in the corners |
| boot at 21:9 | from the start, a frame every 15 | the boot logo the right shape; logos, title, profile list, Nexus as before |
| load at 21:9 | the first load, title to the Nexus, a frame every 12 | the loading art in the middle; the side bars stay black, with no HUD group or soul column (this row first said "Nexus to Boletaria 1-1": that trip needs a developer build's warp) |

## 5. Per-group sizes

Offered through the HUD editor (section 9): `DES_HUD_<PART>_SIZE`, a percent of HUD size, set by dragging a
corner. The settings window has no number rows for them.

## 6. Text size (read and measured)

- **Why text kept its size.** The text shape draw (0x00F9E0A8) asks the renderer's transform for its scale and passes
  it to the font's "set scale" (font vtable 0x01865E10 +0x28, 0x0071B498), which is `blr` for the menu font. The rect
  is scaled; the glyphs are not.
- **Where glyph size comes from.** Each text starts with the font's +0x08 (0x0071B728) and a font number. The 2D drawer
  queues that number as a command (0x00716BE0, 0x000AF2A8: the playback's function descriptor 0x01920248, then the
  number as a 32-bit word). The playback, 0x000B15C8, looks the number up in a table of 13 entries of 20 bytes
  (0x0071B380; number 13 or more reads entry 0) and writes the text state (+0x24 in the object at +0x38 of the list's
  owner): the glyph scale at +0x38 and +0x3C, a base size at +0x48 (24), and the line spacing at +0x94 (base size x
  scale y). The layout (0x00B3C068, then 0x00B35118) sizes each glyph by +0x38 and +0x3C.
- **The HUD's fonts** (measured): 5 (scale 0.833), 6 (0.917), 7 (1.0) and 8 (1.083). The text shape then sets the
  line spacing again (font +0x38, 0x0071B5A0, played back by 0x000B14C8 into +0x94): 0 for the HUD's one-line texts.
- **The fix** (`port/des_hud.cpp`). While a group is drawn at another size, the font number gets the size in its top
  bits (1/1024ths at bit 16, bit 31 set). The playback's wrapper puts the plain number back for the game's playback,
  restores the word after, and multiplies +0x38, +0x3C and +0x94 by the size. A line spacing set in such a group is
  scaled too, passed exactly as 1/64ths of a pixel with bit 15 set. Numbers the game passes are below 13, so a tagged
  number never meets a real one.

## 7. Text on a wide picture, and known limits

- **HUD text on a wide picture is right** (measured). An earlier note in this branch said text was too wide on a
  wide picture. It is not:
  - The glyphs are in canvas units, like the icons. A probe on the glyph quad builder logged the same item name at
    16:9 and live 32:9: every glyph had the same width (11.54 units for the first), and the whole line moved by
    -640 (L at 32:9) with its corner group.
  - In the saved frames (1280x720, before the display stretch), "Crescent Moon Grass" is 204 pixels wide at 16:9 and
    about 103 at 32:9; "The Nexus" 115 and 57. Text is squeezed by one half, the same as the icons.
  - The 32:9 crop stretched back to double width matches the 16:9 crop letter for letter, only softer.

  The softness is the price of every wide picture today: the game still draws 1280 pixels across, and the display
  stretches them (docs/39, section 2, option (b)). A higher render scale (`RSX_SCALE`) gives that detail back.
- **How text is drawn** (read and measured), for later work:
  - text shapes (vtables 0x01883738..0x01883BE8) all draw through 0x00F9E0A8;
  - that gets the font object from the renderer (renderer vtable 0x01865C80, +0x5C; font at renderer +0x70, vtable
    0x01865E10), whose draw (+0x10, 0x0071B6D0) calls the renderer's text draw 0x00719B58;
  - 0x00719B58 puts the text rect through the renderer's matrix and queues a text command (0x000B06B0) in the 2D
    drawer's list (renderer +0x24). The command's first word is a function descriptor (0x01920228): the command is
    played back by 0x000B0D60;
  - 0x000B0D60 lays out the string (0x00B35118, glyph metrics from the font's +0x18, 0x00A183E8) and draws it
    (0x00A34698): per glyph, the font's renderer (vtable 0x01872608, made by 0x00A17CF8) builds a quad at
    +0x10, 0x009D1790, from the layout's corners in canvas units;
  - the game's own font subclass (vtables 0x018758C0 and renderer 0x018758F8, quad at 0x00B5FDE8) is not used for
    the HUD (measured: no calls in the Nexus).
- **Narrower pictures** (16:10, 4:3) are still shown 16:9 with bars (docs/39).
- **Pop-ups outside PCGauge** keep their size: item pick-ups (`DsMenuDlgGetItem`), messages, the boss's name, the
  menus. They stay in the 16:9 band on a wide picture.

## 8. The boot screen and the other canvases (read and measured)

- **What was wrong.** On a wide picture the boot screen's logo (bottom left, while the game first loads) was stretched
  sideways (player report). docs/39 had left it open: 0x00096348 records three more 1280x720 canvases at boot, and
  nothing widened them.
- **What the log shows** (21:9 run from the start): besides the HUD's canvas, six records of the whole canvas pass
  through the 0x3A handler (0x00CD4800), the three from boot and three more later, each with its own render context.
- **The fix** (`port/des_aspect.cpp`). `des_hud_ctx` already runs in the handler before it reads a record's words
  (0x00CD4854). It now also takes any other record of the whole canvas (top 0, bottom 720, left + right = 1280, left
  0 or less) and gives it the HUD canvas's width, [-L, 1280 + L], so whatever is drawn on it keeps its shape. Each
  such context is listed, and a live change of the picture's shape rewrites their matrices as it does the HUD's.
  With `DES_HUD=stretch` nothing is widened, as before.
- **Seen** (frames from the run, stretched to 21:9 as the display shows them): the logo the right shape, at the left
  edge of the 16:9 middle. The notices, the company logo, the title, the profile list and the Nexus look as before.

## 9. The HUD editor

**What it is.** Settings, Game tab, HUD: **Edit HUD**. The settings window steps aside and the game shows, with a
box over each of the four parts, named above it:
- **Move:** drag inside a box. A part can't leave the screen.
- **Resize:** drag a corner (top left, bottom left or bottom right). The part keeps its shape, and the opposite
  corner stays put. A resize that would cross the screen's edge stops there.
- **Hide or show:** click the eye in the circle at the top right. A hidden part's box goes dim and its eye is crossed
  out.
- **Reset all** puts all four back (place, size, shown).
- **Done**, Esc, Circle or F1 go back to Settings. Left and right move between Reset all and Done on a keyboard or a
  gamepad.

**Cursors.** Over a box: move (four arrows). On a corner: the diagonal resize arrow for that corner. On the eye and
the buttons: the hand. On a part whose settings came from outside (a pinned setting): unavailable, and it can't be
changed. RmlUi's Win32 backend knows one resize cursor only, so `host.cpp` adds `nwse-resize` and `nesw-resize`
(Windows' IDC_SIZENWSE and IDC_SIZENESW, SDL's SIZENWSE and SIZENESW).

**The settings** (also usable without the editor, like the other options):

| Option | Values |
|---|---|
| `DES_HUD_BARS_POS`, `DES_HUD_AREA_POS`, `DES_HUD_EQUIP_POS`, `DES_HUD_SOULS_POS` | `"x,y"`: moved by that much from where the other HUD settings put it, in the game's 1280x720 units; default `"0,0"` |
| `DES_HUD_BARS_SIZE`, `DES_HUD_AREA_SIZE`, `DES_HUD_EQUIP_SIZE`, `DES_HUD_SOULS_SIZE` | percent of HUD size, 25 to 300; default 100 |
| `DES_HUD_BARS` ... | the eye (section 3) |

While a box is dragged the game gets each step at once. settings.ini is written when the drag ends.

**Kept on screen.** The draw cuts a move back so the part's pieces stay inside the canvas the picture shows
([-L, 1280 + L] by [0, 720], L from section 3), or no further out than with no move. The editor shows the same cut, so
a box stops at the edge. A move given in settings.ini or the environment is cut back the same way from the first
frame (measured: `DES_HUD_SOULS_POS=-2000,-2000` puts the souls in the top left corner).

**How a part's box is found** (`port/des_hud.cpp`; read and measured):
- **An object's rect.** 0x00F62E68 (r3 = answer, r4 = object) gives an object's rect in its dialog's layout, from its
  shape (vtable +0x2C). The wrapper calls it on the draw's thread, with a frame of its own on the guest stack.
- **When.** Every 4th HUD draw while the editor is open, and for 120 draws after its last request, or while a part is
  moved or resized. A part's box is kept while none of its pieces is drawn.
- **Parked pieces.** Pieces wholly outside the layout don't count. A moved or resized part also leaves them out of
  its draw:
  - The bars' outlines (`outline_hp`, `_sp`, `_mp`) are drawn parked at y = -140 to -78, above the canvas. Moved
    down, one showed as a thin bar at the top of the screen.
  - The HP, SP and MP icons at x = -168 (section 1) showed in the same way once the bars moved right. They are no
    longer drawn once the bars are moved or resized.
- **Hosted dialogs.** `soul_param` hosts a `DsMenuDlgFEGetSoul` (vtable 0x018696C0, the word at the piece's +0x48).
  That dialog is as big as the canvas (rect 0,0 to 1276,708; layout 0,0 to 1280,720), and its six pieces sit at the
  bottom right. For a piece as big as the layout that hosts a dialog whose current rect is the piece's rect, the box
  is that dialog's drawn pieces, mapped from its layout onto its rect.

  The first version boxed the whole screen for the souls, and that box caught every drag. The equipment slots host
  dialogs too (`DsMenuDlgFeShortcut`, vtable 0x018697B0). Their own rects are the slots, so they are used as they
  are; inside, each slot keeps plates parked above it.
- **Not on screen yet.** Until a part has been seen drawn, the box of all its pieces stands in. This covers an area
  name the game is not showing at the moment.
- **Measured (the Nexus, 16:9):** bars 65,43 to 351,173; area 891,65 to 1202,139; equipment 82,480 to 507,671; souls
  991,627 to 1201,667.

**The picture in the window.** The editor places the boxes on the picture as the D3D12 backend shows it: fitted to the
window at `RSX_ASPECT` (which `DES_ASPECT` sets), else 16:9, in the middle.

**Drags.** The boxes use RmlUi's own drag (`drag: drag` in `hud_editor.rcss`). Its events reach the box wherever the
mouse goes, also outside the window, and see the button go up there. With plain mouse events, a drag past the
window's edge stopped getting moves and missed the release.

**Tests** (measured, 2026-10-24). The archstone save in the Nexus, offline, this branch. The editor was driven with
`DES_UI_MOUSE`, a new test hook (`des_ui.cpp`, beside `DES_UI_PRESS`: `x,y[,down|up][;...]` in back buffer pixels,
one setenv per drag), and the window was saved with PrintWindow:

| Step | Done | Seen |
|---|---|---|
| open | Settings, Game, Edit HUD | a box over each part, with its name and eye; Settings hidden |
| move | bars dragged by +300,+180 | `DES_HUD_BARS_POS=300,180`; the bars there, no stray icons or outlines |
| resize | the souls' top left corner dragged out | 140%, the bottom right corner where it was (`+32,+21`) |
| eye | the area name's eye | `DES_HUD_AREA=0`; the name gone, its box dim, the eye crossed out |
| Reset all | Left, Enter | all four back |
| Esc | | back in Settings, on Edit HUD |
| 21:9 | `DES_ASPECT=2560:1080` | boxes on the corner groups; the bars dragged past the window's top left corner stop at the screen's corner (`-65,-43`), the eye and corners moved inside the box |
| settings at start | `DES_HUD_SOULS_POS=-2000,-2000`, `DES_HUD_EQUIP_SIZE=60` | the souls in the top left corner; the equipment at 60% from its corner |

**Limits.**
- Only the four parts move. The boss bar, pop-ups and menus stay where the game puts them.
- A part's box follows its drawn pieces. The bars' box narrows while the long health background is not drawn, so a
  part pushed against an edge can shift a little when it comes back.
- The cursors were not seen in a test (a window capture has no cursor).

## 10. Address table

| Address | What |
|---|---|
| 0x018698A0 | `DsMenuDlgPCGauge` vtable (the HUD) |
| 0x00F60BD0 | dialog draw, vtable +0x4C: tail-calls 0x00F5EAC0 or 0x00F5F878 (+0x9C) |
| 0x00F62A88 | object draw: needs bit 0x40000000 of +0x18 |
| dialog +0x0C, +0x10 | children array, count |
| dialog +0x74, +0x84 | current rect, layout rect (left, top, right, bottom) |
| dialog +0x94 | byte, 0 = draws nothing |
| object +0x1C | layout record; +0 = UTF-16 name |
| 0x00F9E0A8 | text shape draw |
| 0x0071B728, 0x00716BE0 | font +0x08: begin a text with a font number; queues it (0x000AF2A8) |
| 0x000B15C8 | plays the font number back: glyph scale (text state +0x38, +0x3C), base size +0x48, line spacing +0x94 (function descriptor 0x01920248) |
| 0x0071B380 | the font number's entry: table of 13 x 20 bytes, scale at +0x0C, +0x10 |
| 0x0071B5A0, 0x000B14C8 | font +0x38: line spacing; its playback into +0x94 |
| 0x0071B498 | font +0x28, "set scale": does nothing |
| 0x0186FBC0 | `DsMenuDlgNowLoading` vtable (the loading screen; parts `soul_0`..`soul_99` hold the soul frames) |
| 0x00CD4800, 0x00CD4854 | the 0x3A canvas handler; the hook before it reads the words (`des_hud_ctx`) |
| 0x01865C80 | menu renderer vtable (+0x48 set matrix, +0x4C get matrix, +0x5C font) |
| 0x00719B58, 0x000B06B0 | renderer text draw; queues the text command |
| 0x000B0D60 | plays the text command back (function descriptor 0x01920228) |
| 0x009D1790 | glyph quad, font renderer vtable 0x01872608 +0x10 |
| 0x00F62E68 | an object's rect in its dialog's layout (r3 = answer, r4 = object), from its shape's vtable +0x2C |
| object +0x48 | the dialog a piece hosts, for the pieces that host one (`soul_param`, the equipment slots) |
| 0x018696C0 | `DsMenuDlgFEGetSoul` vtable (the souls, hosted by `soul_param`) |
| 0x018697B0 | `DsMenuDlgFeShortcut` vtable (an equipment slot) |
