# 91. The setup screens' options: presets and a few choices (2026-10-22)

After the build, setup asks how the player wants to play, as in the setup design: first a preset, then a few
choices set from it. This note says what the two pages hold, what they write, and where they differ from the
design, and why.

Code: the port branch `feat/setup-options`, on the start screens (docs/88). Labels: **measured** (a run says so),
**read** (seen in the code or a doc), **inferred** (reasoned, not run).

## Short answer

- **When the build ends, the page says "The game is built"** and offers "Choose your options". Before, it said
  "Ready to play" at once.
- **"How do you want to play?"** shows three cards: Original, Enhanced (Recommended) and Custom, with a table of what
  Enhanced changes. Picking a card selects it, and the page stays.
- **"A few choices"** has six options set from the picked preset: Display mode, Frame rate, Render resolution, Aspect
  ratio, Controls and Online play (with the online name under it). Each one can be changed in place.
- **Nothing is written until Finish.** Finish stores the preset's settings, the choices over them and the preset in
  settings.ini, then the page says "Ready to play" with a line that sums the options up. Closing setup halfway writes
  nothing, and the options are offered again next time.
- **A rebuild does not ask again.** Once a preset is stored, the end of a later build (Update, Repair, Start over)
  goes straight to "Ready to play".
- **The online name is never empty.** When there is none, setup picks one (a Demon's Souls word and four digits,
  such as Stonefang-4821) and says so. A typed name that is not valid keeps the one there, and the page says why.

## 1. The pages

### 1.1 How do you want to play?

| Card | What it says | What it writes |
|---|---|---|
| Original | The game exactly as it was on the PS3. No enhancements at all. | 30 fps, 100% (720p), the game's own texture and anisotropic filtering, bilinear scaling, a window, 16:9 |
| Enhanced (Recommended) | Sharper and smoother, the same game. | 60 fps, the render resolution matched to the display (at most 200%), 16x anisotropic filtering, SMAA, sharp scaling, fullscreen, 16:9 |
| Custom | Starts from the settings you have now. | Nothing of its own: "A few choices" starts from the settings in use |

Original sets anti-aliasing off. Both presets also set motion blur and depth of field on, texture filtering, the HUD, the cutscene
frame rate and V-sync to their defaults. So picking one after the other leaves nothing of the first (read:
`setup_options.cpp`, `preset_settings`).

The matched resolution is the largest of 100%, 150% and 200% whose picture (720 lines times the percent) fits the
display's height: 200% (1440p) on a 1440p or 4K display, 150% on a 1080p one. 300% is left to Settings: on a 4K
display it costs much more GPU time for a picture few can tell from 200%.

"What changes" lists the rows where Original and Enhanced differ (frame rate, render resolution, anisotropic filtering, anti-aliasing, scaling filter, display mode), the Enhanced values in the accent colour. The rows that are the same (motion blur and depth of field, aspect ratio) are left out, so the page fits 720 dp (docs/89 section 2).

The pad: Left and Right move between the cards, Cross picks one, Down goes to Continue (not to the button under the
card), Up comes back to the picked card. The page opens with the focus on the picked card: Enhanced the first
time, else the stored preset.

### 1.2 A few choices

| Option | Choices | The line under it |
|---|---|---|
| Display mode | Window, Fullscreen | Alt+Enter switches at any time. |
| Frame rate | 30, 60, 120, and the display's refresh rate when it is above 60 (144, 165, ...) | Your display runs at 144 Hz. Above the refresh rate: "..., so it shows at most N frames a second." |
| Render resolution | 720p, 1080p, 1440p, 4K | Your display is 2560x1440. Above it: "Higher than that costs GPU time for little gain." |
| Aspect ratio | 16:9, Match the display | Says whether the display is 16:9, wider (more at the sides) or narrower (the picture stays 16:9) |
| Controls | Gamepad, Keyboard and mouse | Keyboard and mouse turns mouse look on (DES_MOUSE_LOOK). The keyboard works with either. |
| Online play | On, Off | What online play brings (messages, bloodstains, phantoms), or what Off leaves out |
| Online name (under Online play, while it is on) | The name and Change name (the on-screen keyboard; a keyboard types into it too) | 3 to 16 letters, digits, - or _. Not your PSN name. |

A value set elsewhere that is not one of the choices (Unlocked frame rate, 175%, a 21:9 shape) shows as one more
choice, so Custom never loses it.

The lead line says where the values came from: "Set from Original.", "Set from Enhanced." or "Set from your
settings.", then "Change any of them, or keep them as they are."

Back keeps the choices: Continue again with the same preset finds them as they were left. Picking another preset
starts again from that preset.

### 1.3 Ready to play

After Finish, the end of the build reads "Ready to play", with two rows: Built (the version and how long it took)
and Options, for example "Enhanced: 120 fps, 1440p, the display's shape, fullscreen, keyboard and mouse, online
on." Then Play and Settings. The start screen shows the same Options row when the game is ready.

If setup was closed before Finish, the start screen says "The game is built" and offers "Choose your options".

### 1.4 The rail

Every setup page's rail now has four steps: Your game, Build, Options, Play, as in the design. Options shows a tick
once a preset is stored (so a rebuild shows it done), and is the current step on the two options pages.

## 2. What Finish writes

`settings::store` (new) writes a value to settings.ini without putting it in this process's environment: the game,
started after setup, reads the file. A value put in the environment would reach the game as one set from outside,
and its row in Settings would show as pinned (read: `settings.hpp`).

Finish stores, in order: the preset's settings (none for Custom), the six choices and the online name, and
`setup_preset` (`original`, `enhanced` or `custom`), a UI-only key that also tells the screens the options were
picked.

Display mode takes effect at once: the window follows the stored value every frame (`sync_fullscreen`), and setup
and the game share the window. So Finish with Fullscreen switches the window before Ready to play (measured: the
Ready to play shot is 2560x1440). The other options take effect when the game starts.

**With the launch hook (docs/88 section 3, port #135):** the game reads settings.ini into its environment once,
at its start (`des_ui_early_init` calls `settings::load_and_apply_env`), and a variable already in the environment
wins over the file. Play starts the game in a new process with the environment this one started with (saved before
`main`, so nothing that setup set carries over), and the new process reads what Finish stored. Measured with #135
(docs/88 section 11): `RSX_SCALE = 150` and `DES_FPS = 60` from settings.ini took effect in the game's process.
Without the hook (a runtime without it, the game already running below the screens), the options other than Display
mode apply at the next start.

## 3. The online name

The name is required for online play, and a player who removes it should not end up offline without knowing why.
So:

- "A few choices" never shows an empty name. If there is none (or it is not valid), it picks one with
  `settings::suggest_net_name()`: one of 16 words from the game (Boletaria, Stonefang, Latria, Nexus, ...), a hyphen
  and four digits. The line under it says "Picked for you; change it if you like."
- A typed name that is empty or not valid keeps the name there, and the line says so: "Online play needs a name,
  so Stonefang-4821 was kept."
- The Network tab in Settings does the same (same rules, same messages), so the name cannot be emptied there either.

What a new name means for the server (read: dessego's tables, keyed by the name): the server knows a player only by
the name. A new name is a new player: messages and bloodstains left under the old name stay, but their ratings no
longer reach this player. Two players with the same name are the same player to the server. The Network tab's help
says this in one line.

## 4. Where the build differs from the design, and why

| The design | The build | Why |
|---|---|---|
| Enhanced lists FXAA | Enhanced turns on SMAA | First the user left anti-aliasing off, because many players dislike FXAA's soft picture. Since SMAA came (2026-10-28, docs/43 section 3.7), Enhanced uses it: it smooths edges and keeps textures sharp. FXAA stays in Settings. |
| The picked card's left bar renders grey (a CSS order slip in the demo) | Soul-white, like the rail's current step | What the design meant |
| Custom: "Starts from your RPCS3 settings." | "Starts from the settings you have now." | The RPCS3 route (docs/90) is not built yet. When it imports RPCS3's settings, Custom will start from them. |
| No aspect ratio on "A few choices" | Aspect ratio: 16:9 or Match the display | The user asked for it |
| The online name in its own block, a text field, no checks | Under Online play, with Change name and the checks of section 3 | A text field needs a keyboard; the on-screen keyboard works with the pad. The checks are the user's decision. |
| Options can be done during the build ("Done, back to the build") | After the build only | The design's own decision ("Options come after the build"); nothing reached the other path |
| Controls: "DualSense found. Its buttons show as PS3 buttons." | "The keyboard works as well. Buttons show as PS3 buttons." | Setup does not look for pads yet |
| Frame rate 30, 60, 120, 144 | 30, 60, 120 and the display's rate | The design's 144 was its PC's refresh rate |
| A bar over both pages: "Build finished. Finish your options, then play." | No bar | The rail already says Build Done, and without the bar the preset page fits 720 dp with its buttons in view (the user's rule: no scrolling for Continue or Back) |
| "What changes" lists every option | Only the options that change | The same rule; a table called "What changes" needs only those |

The user's other decisions (10-22): frame rates above the display's refresh rate stay offered, with the line under
the option saying what happens; the matched resolution stops at 200%; "A few choices" follows every preset; a
rebuild does not ask again.

## 5. Tested

(measured, unless marked)

- Scripted run with the stand-in engine (docs/88 section 5): a finished build, "Choose your options", Original
  picked and Enhanced picked again, Continue, 120 fps, Match the display, Online play off and on, Change name with a
  digit typed on the on-screen keyboard, Finish, Ready to play, Play. Every page was seen in screenshots. The run's
  settings.ini (its own, DES_SETTINGS) held the preset's values, the choices and `setup_preset = enhanced`. The
  user's save was untouched.
- Built: des_host (host-only) with clang-cl; no new warnings.
- Not tested: a real build (the stand-in engine compiles nothing), Linux (inferred: `setup::display()` asks SDL there).
