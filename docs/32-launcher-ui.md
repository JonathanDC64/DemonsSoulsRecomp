# 32. The wrapper UI (start screen, in-game menu, settings)

The same wrapper UI as FFCCEOT-Recomp (which took it from Dusklight), with a
Demon's Souls theme. Code in `port/ui/src`, styles and fonts in
`port/ui/res`, the embedding helper in `port/cmake/EmbedFiles.cmake`,
licences in `port/ui/NOTICE.md`.

## What it does

- **Start screen** over the game as it boots: "DEMON'S SOULS", a silver
  rule, *PC port*, then **Play**, **Settings** and **Quit**. The game's
  logos and notices run behind it, darkened from the left; it gets no input
  until Play. Esc or Circle on the start screen asks to quit.
- **In-game menu**: F1 or Esc on the keyboard, or **L3 + R3** on a gamepad
  (Settings > Interface can switch this to Select + Start). It is a bar at
  the top with **Settings** and **Quit**. Esc, Circle, F1 or Start close it.
- **Quit** asks first ("Return to the desktop? Progress since the game last
  saved will be lost.") in a blood-red window, then posts WM_CLOSE to the
  game window.
- **Settings**: tabs Display, Game, Controls, Interface, About, the left
  pane showing the value, the right pane showing the choices and help. Rows
  that only apply at the next start say so. A row whose option was set in
  the environment shows that value, can't be changed, and its help says why.
- After Play, a toast at the top says how to open the menu (setting *Menu
  hint*).

While any of it is showing, the game sees neutral pads
(`cellPad_set_host_hold(1)`) and gets the input back only once every game
key, F1, Esc, the mouse buttons and every pad button and stick are back at
rest, so the press that closed the menu never reaches the game.

## Settings

`settings.ini` (`key = value`) lives in
`%APPDATA%\DemonsSoulsRecomp\DemonsSouls\` (where SDL_GetPrefPath would put
it), or next to the exe if a `portable.txt` is there, or wherever
`DES_SETTINGS` points. Keys are the run options' own environment variable
names. At startup, before the game reads its options, each value becomes its
environment variable unless that variable is already set: **explicit
environment always wins.** Changes are saved when the settings window closes
and on every change.

| Row (tab) | Key | Values | Applies |
|---|---|---|---|
| Frame rate (Display) | `DES_FPS` | "" = 30 fps original, 30, 60, 120, 144, 165, 240 | next start |
| V-sync (Display) | `DES_VSYNC` (sets `RSX_VSYNC`) | 1 / 0 | at once (live knob) |
| Short boot logos (Game) | `DES_SKIP_INTRO` | 0 / 1 | next start |
| Intro movies (Game) | `PS3_SAIL_MOVIES` | 1 / 0 | from the next movie |
| Continue automatically (Game) | `DES_AUTO_CONTINUE` | 0 / 1 | next start |
| Sound (Game) | `AUDIO_MUTE` (inverted) | 0 / 1 | at once (live knob) |
| UI scale (Interface) | `ui_scale` | 0 = auto, 75, 100, 125, 150 | at once |
| Start screen (Interface) | `show_start_screen` | 1 / 0 | next start |
| Menu hint (Interface) | `menu_hint` | 1 / 0 | at once |
| Gamepad menu buttons (Interface) | `pad_menu_chord` | 0 = L3 + R3, 1 = Select + Start | at once |

Live values go through `ps3_setenv` (the runtime's getenv cache and
PS3_KNOB). Controls is information only (the menu keys, the keyboard pad
map); rebinding is not done. About has the version (git short hash at
configure time), the licence, the settings file's path and the credits.

`play.sh` sets `AUDIO_MUTE=0` for every session, so under play.sh the Sound
row shows as set from outside. Launching the exe directly, it is a normal
setting.

## Run options (knobs)

| Knob | Effect |
|---|---|
| `DES_UI=0` | The UI is not installed at all (no overlay, no settings.ini). |
| `DES_SKIP_UI=1` | No start screen this run (the menu is still on F1). |
| `DES_UI_START=1` | Start screen even on a scripted or test run. |
| `DES_SETTINGS=<path>` | Use this settings.ini (also makes test runs read and write one). |
| `DES_UI_OPEN=menu` / `settings` | Open the menu bar or the settings window at startup. |
| `DES_UI_SHOT=<a.bmp>[,frame][;<b.bmp>,frame...]` | Write the back buffer, UI included, at that presented frame (default 300). |
| `DES_UI_KEYS=<frame>:<KEY>[,...]` | Press keys at presented frames: F1 ESC UP DOWN LEFT RIGHT ENTER PGUP PGDN TAB SPACE. With DES_UI_SHOT, a test can walk the menus. |
| `DES_UI_NO_ANIM=1` | No transitions. |
| `DES_UI_SOUND=0` | No UI sounds. |

No start screen when the run is scripted: a tools/des test run
(`PS3_PIDFILE` set and `DES_KIND` not `play`), `DES_AUTO_CONTINUE`,
`PAD_FILE` or `EXIT_AFTER_MS`. Test runs also neither read nor write
settings.ini unless `DES_SETTINGS` is given, so regress, levelcap, exeab and
friends see exactly the options they pass.

Example, the screenshots used while building it (desrun, locked session):

```
DES_SCRATCH=<scratch> RSX_PRESENT=0 bash tools/des/desrun.sh 75 <scratch>/run.txt DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 \
  DES_UI_START=1 DES_SETTINGS=<scratch>/settings.ini \
  "DES_UI_SHOT=<scratch>/start.bmp,120;<scratch>/set60.bmp,380" \
  "DES_UI_KEYS=140:DOWN,160:ENTER,220:DOWN,280:ENTER,300:DOWN,320:DOWN,340:ENTER,400:ESC"
```

## How it is built

| FFCCEOT | Here |
|---|---|
| GL3 renderer in the game's GL context | RmlUi's stock D3D11 renderer (`Backends/RmlUi_Renderer_DX11`) on a D3D11On12 device made from the game's D3D12 device and queue. Each swap-chain buffer is wrapped (`CreateWrappedResource`, PRESENT in and out) with an RTV; per frame: Acquire, BeginFrame, Render, EndFrame(rtv), Release, Flush. Composited premultiplied. |
| SDL3 window events and gamepads | **No SDL.** The window procedure hook queues keyboard and mouse messages (RmlUi's Win32 helpers convert the keys); the gamepads are XInput plus the runtime's `pad_hid_sony_poll`, the same readers and mapping the game's pad uses. |
| `Presenter::present` callback | `rsx_d3d12_set_overlay` (ps3recomp): `wndproc` and `frame` hooks, both on the vblank thread here. |
| settings.ini next to the exe | the pref dir (or portable.txt), mapped onto the DES_* / PS3_* options; env wins |
| start screen over generated art | over the game itself, booting behind it; gradient and vignette in RCSS, no images |
| ROM setup, Wii setup, presets, spell ring, update check, report, ui_script | not taken |
| game SFX through `cfie::queue_sound` | `g_des_ui_sound(int id)` (des_ui.h): des_menu_sfx.cpp queues the game's menu sound for the main thread (docs/33) |

Files:

- `des_ui.cpp`: `des_ui_early_init()` (called first in
  `ps3_title_options_init`, des_options.cpp), the overlay hooks, the input
  queue, gamepad reads, the input hold, the debug knobs.
- `host.cpp`: D3D11On12, the renderer, system and file interfaces (the
  embedded files), fonts, scale, the BMP screenshot.
- `settings.cpp`: settings.ini, the option table, pinned (env) detection,
  harness detection.
- `settings_window.cpp`, `prelaunch.cpp`, `menu_bar.cpp`, `overlay.cpp`:
  the documents. `input.cpp`: pad state to UI keys with repeat. The rest is
  Dusklight's framework (document stack, windows, panes, buttons, modals,
  tabs), unchanged apart from its headers.
- `des_ui` is its own static library linked into des_boot: des_boot's
  compile flags don't change (the lifted chunks don't rebuild). Third-party
  code is built with `/O2` (the port configures with an empty build type).

### Cost when hidden

The frame hook's idle path is a few loads: not awake, no queued input, no
fresh chord in `g_pad0_buttons` (pad 0 as the game last read it), no
screenshot due. It then returns: no RmlUi update, no pad reads, no GPU work.
After the last document hides it stays awake about 0.6 s for the closing
transitions and while the hint toast shows, then sleeps again. The D3D11On12
device and RmlUi are created lazily, the first time the UI wakes (or on the
first frame when the start screen shows).

Measured with `levelcap` A/B (the same exe, `DES_UI=0` against the UI
installed and hidden), see the end of this file.

### Gamepad chord

L3 + R3 by default: Start and Select open the game's own menus, L3 is not
used by Demon's Souls, and R3 (lock-on) is the only game action the chord
touches; the game sees that press before the menu opens. While the UI is
showing, Start also toggles the menu.

## Theme

`theme.rcss` tokens: text bone (215, 210, 195), accent near-white bone
(236, 232, 220), borders silver (148, 150, 154), surfaces near-black
(11, 11, 12) at 80-96 %, focus soul-white (200, 215, 225) at low alpha,
danger blood red (#6E1A17 border, #A8241D heading). Borders 1 dp, corners
2-4 dp. Cinzel for headings and the title, Alegreya Sans for body text,
Material Symbols for icons. `prelaunch.rcss` is rewritten (no picture, no
golds): menu buttons in bone that light up white with a soul-white left
edge and a fading band.

## Not done

- Sounds are the game's own menu sounds (docs/33, `port/des_menu_sfx.cpp`),
  played on the game's main thread; they need the `0x724E40=des_menu_tick`
  override in the lift. Not yet checked by ear.
- Rebinding, display options (window size, fullscreen, AA), the other items
  of the future-features list.
- The gamepad path is untested on hardware here (no pad on the test
  machine); keyboard, scripted keys and the hold are tested.
- The fetched sources need network access at configure time; the future
  patcher (no installs) will need them vendored or downloaded by its own
  script.

## Verification (2026-10-17)

All on a locked session (RSX_PRESENT=0; the frame hook still runs), with
scripted keys and DES_UI_SHOT:

- Start screen, Settings from it, every tab, a choice changed (Frame rate to
  60), back out, Play (the hint toast shows, the screen fades), F1 opens the
  menu bar, Settings from the bar, Esc twice closes everything: all drawn as
  expected.
- settings.ini written on change (DES_SETTINGS in the scratch dir) with
  `DES_FPS = 60`; rows set from the command line (DES_SKIP_INTRO,
  PS3_SAIL_MOVIES, AUDIO_MUTE from deslib) show their value and can't be
  changed.
- Quit, then Quit Game in the question: `[boot] window closed -- exiting`,
  exit code 0.
- Test harness runs (levelcap) print no `[ui]` line at all: no settings
  file, no start screen, the UI asleep.
- Cost hidden, levelcap A/B of the same exe, Palace (pinned regression
  save), DES_FPS=60, vblank at 2000 Hz so nothing caps, RSX_PRESENT=0,
  4 windows of 8 s each: `DES_UI=0` 367.7 fps, UI installed and hidden
  368.7 fps (noise). At the usual VBLANK_MHZ=240000 both arms sit on the cap
  (239.9 / 240.0).

Not tested: a real gamepad (the chord, the stick and button mapping in the
menus, the hold release), the mouse, a visible window (RSX_PRESENT=1), and
window sizes other than 1280x720.
