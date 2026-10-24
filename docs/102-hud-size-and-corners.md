# 102. HUD size, the HUD in the corners, and hiding parts of it (2026-10-24)

New settings for the in-game HUD: a size, its four corner groups pushed out to the screen's corners on a wide
picture, and a switch to hide each group. Labels: **measured** (a run says so), **read** (seen in the code, the EBOOT
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

- **Each group grows or shrinks from its own corner** of the screen; the boss's health bar from the bottom middle.
  At 16:9 with the defaults nothing changes: the game draws its HUD as before (measured, section 4).
- **Wide pictures.** With Corners, the bars and the equipment move to the left edge, the area name and the souls to
  the right edge. Menus, pop-ups and the boss bar stay in the 16:9 band in the middle, as before (measured).
- **The three stray markers are gone.** docs/39 section 8 listed a red triangle, a green triangle and a blue ring in
  the left band on a wide picture. They are the HUD's own "HP icon", "SP icon" and "MP icon", placed at x = -168,
  left of the 16:9 canvas, where the PS3 never shows them (read). They are no longer drawn on a widened canvas.
- **Per-group sizes: not offered.** The mechanism would allow one size per group at no extra cost, but four more
  number rows add little: the groups share one look and read best at one size. Easy to add if wanted (section 5).
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
| load at 21:9 | Nexus to Boletaria 1-1 | RESULT |

## 5. Per-group sizes

`group_rect()` already takes a size per call; a size per group would be four more knobs and four number rows. Not
done, for the reason in the short answer.

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

## 9. Address table

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
