# 89. The setup screens' look: the setup design in RmlUi (2026-10-22)

The progress screen merged in #125 (docs/88) used the wrapper UI's small modal. The setup design (the demo page shown on 10-21) draws setup as the whole window. This note lists how the two differed, describes the shell that the screens now share, and says where the build still differs from the demo, and why.

Code: the port branch `feat/setup-look`, from master 2d66556. Labels: **measured** (a run says so), **read** (seen in the code or a doc), **inferred** (reasoned, not run).

## Short answer

- **The setup screen is now the whole window**, as in the design: a dark backdrop, a rail on the left (the name, the steps, the version) and the page on the right. Before, it was a 720dp modal over black.
- **The progress page follows the design.**
  - It shows the percent, the time left and one bar, then the build's tasks, each with a spinner, a tick or a cross and its count.
  - Stop is red and asks "Stop the build?" over the dimmed page.
  - A failed run gets its own page: a red callout, the engine's last lines, then Report the problem, Try again and Show the full log.
  - A finished run says "Ready to play".
- **The engine, the line protocol and what each button does are unchanged.** Only the look and the layout changed.
- **`setup_page.hpp` is the shell for every setup screen.** The start, game files and Before Start screens (the local `feat/setup-start-screens` branch) move onto it before their PR.
- **Tested** (measured): with a stand-in engine (section 5), one scripted run goes through a run, Details, the stop question, a stop, Resume, a failure, the report question, Try again and a finished run. Every page was seen in screenshots, and the user's save was untouched.

## 1. How the merged screen differed from the design

| | The design | Merged (#125) and the local start screens |
|---|---|---|
| Window | The whole window, 16:9, dark gradient backdrop | A small modal (720dp at most) over black |
| Rail | "Demon's Souls" in Cinzel over a fading rule; the steps Your game, Build, Options, Play, each with a round numbered mark (a tick when done) and a line under it ("40%, about 9 min left"); the version at the foot | None |
| Heading | Cinzel at 30px, mixed case, with a muted line under it | The modal's capitals title with an icon on the right |
| Progress | The percent and the time left on one line, one 8px bar, then a task list (spinner, tick or cross; name and a line under it; the count on the right) | A list of phases with badges, then two bars (the step's and the whole run's) and text lines |
| Task names | The lift as two tasks (the main processor's code, the SPU programs); Compile, Link, Check the build, Prepare shaders | "Convert the game's code", "Build the game" |
| Buttons | Cinzel capitals in a thin silver frame: primary (soul-white and a glow), danger (red: Stop), quiet (text only: Back, Show the full log) | Equal boxes in the body font, no kinds |
| Stop | "Stop the build?" over the dimmed page: Stop in red, Keep building | "Stop setup?" in a second modal |
| Failure | A page, "The build stopped": a red callout with what failed, a line about the report and the log's last lines; Report the problem, Try again, Show the full log | A modal over the screen: Try again, Report, Details, Close |
| Report | Says what the file holds first, then saves it | Saves at once and shows where |
| Done | "Ready to play": rows for the build, the imported save and trophies and the options; Play, Settings; where the menu is | "The game is ready": Play or Close |
| Pad buttons | None | A row at the bottom, whose words were not drawn (a bare text node in a flex row) |
| Before the build | Welcome (three rows), "Where is your game?" (three route cards: RPCS3, decrypted files, Show me how), the RPCS3 and folder pages, "Ready to build" (rows and Start building) | Small modals: the start screen, the game files screen, Before Start |
| Order | Welcome, the game, Ready to build, Build, options during the build, Ready to play | Start, game files, Before Start, progress, done |

The colours and fonts were already the same: theme.rcss holds the design's tokens (read).

The user's decisions (10-22):
- Build the RPCS3 route.
- Build the options screens.
- The welcome says that setup downloads its build tools once, and the task list has a "Download the build tools" line.
- Keep the pad's buttons, small, at the bottom.

## 2. The shell (setup_page.hpp, setup.rcss)

`SetupPage` is a `Document` with its own RML: theme.rcss, controls.rcss and setup.rcss. global.rcss comes with every document, so the pad's face-button icons moved there from window.rcss.

- **Backdrop.** Three layers, as the design's: a linear gradient to the right, a vignette, and a faint glow at 72% / 40%. It is opaque, so the game below does not show.
- **The rail.** Each screen calls `set_rail` with its steps, and each step is Todo, Now, Busy or Done, with an optional line under it. Now has the soul-white bar and the gradient. Done has the tick. The foot shows the version and ps3recomp's.
- **The page.** `set_heading(title, lede)`, then the screen's parts in `content()`, and its buttons in `footer_row()` (with `footer_end()`, a second row at its right end: the start page's Settings and Quit). Only the parts scroll. The buttons stay in view: on a short page they follow the parts, on a long one they sit above the pad's buttons, with a line between them while the parts scroll behind. The parts come from helpers:
  - `panel` (stacked or not);
  - `found` (a mark, a key and a value);
  - `callout` (err, warn or info);
  - `text` (any tag);
  - `actions_row`.
  In setup.rcss there are also `setup-log`, `setup-field`, `setup-tag` and `progress.setup-meter`.
- **Size.** While a setup page is open (anywhere in the stack, so Settings or the on-screen keyboard over it keep its size), the UI scales with the window's height: 720 px high is 1x, in quarter steps rounded down (1080p 1.5x, 1440p 2x, 2160p 3x), never below the display's DPI scale, times the UI scale setting (host.cpp `render`). So a page has the same layout at 720p, 1080p and 1440p, and each page fits in 720 dp with nothing to scroll (measured, 10-22: every page at 1280x720 and at 2560x1440). Three pages were trimmed to fit: the game folder page (when all the decrypted files are there, their row joins the game's panel; three missing files are listed, not six), the build page (a one-line lede, closer task rows) and the preset page (docs/91).
- **A ratio change and RmlUi.** After the dp ratio changes, RmlUi works a length out again only where it is written in dp. A length from a variable (`var(--space-md)`, most of the theme's sizes) kept its old size: at 1440p the start page, made before the change, was drawn at half size (measured). So a change gives each document a new copy of its style sheet (host.cpp `restyle_documents`), which works every property out again. It happens only on a change: a DPI change, or the window's height crossing a step.
- **Buttons.** `set_buttons(buttons, parent, actions)` takes an action list, each with a label, a function and a look (Plain, Primary, Danger or Quiet). A button with no function is shown but cannot be pressed. A pressed button's action runs in `update()`, because it may rebuild the buttons. If the old button had the focus, the new button in its place gets it.
- **The pad.**
  - Lines of buttons come from `nav_lines()`. Left and Right move along a line, and Up and Down go to the next line.
  - Cross presses. Circle calls `back()`. L1 and R1 call `scroll()`.
  - Start and F1 do nothing here.
  - `set_hints` draws the pad's buttons at the bottom, small. Each word is in its own element.
- **A question over the page.** `ask(title, body, actions, focused)` opens the question in the same document: a 560dp box over the page, dimmed to 72%. The focus goes to `focused`, the safe button. Circle presses the last button. An action closes the question first and then runs, so it can open another question (as Report does with its result).
- **Opening and closing.** The rail and the page fade in over 0.2 s; the backdrop does not fade. When a page closes, it is hidden at the end of its fade, as `WindowSmall` does, with the same fallback.

## 3. The progress page (setup_window.cpp)

- **Running.**
  - Heading: "Building the game for this PC", with "Setup translates the game's PS3 code into a program for this PC. The first time takes a while." under it.
  - Panel: the percent, the time left (`Eta`, after its first minute), the bar, and the tasks.
  - Buttons: Stop (Danger) and Details (Quiet; it shows the engine's last 12 lines under the panel).
  - The note beside the buttons says when the PC is kept awake.
  - The rail: Your game done; Build now, its line "52%, about 3 minutes left"; Play to do.
- **Tasks.** One line for each phase of the plan. The lift becomes two lines: the main processor's code while the count's step is `ppu`, then the SPU programs once it is `spu`.

  | Line | Name | The line under it | Count |
  |---|---|---|---|
  | check | Check the game files | Every file is read once | MB read |
  | fetch | Download the build tools | Only the first time ("Already downloaded" when skipped) | MB downloaded |
  | stage | Copy the game files | Into setup's own folder | |
  | ppu | Translate the main processor code | The game's code, one function at a time | functions |
  | spu | Translate the SPU programs | The code for the PS3's helper processors | programs |
  | compile | Compile | Builds des_game.dll from the translated code | files |
  | shaders | Prepare the shaders | So that the game starts smoothly | (no engine phase yet) |

  A skipped line reads "Already done". Only the running line, or the failed one, shows its count ("13 / 33").
- **Stop question.**
  - Title "Stop the build?", with "The finished parts are kept. Next time, setup carries on from here."
  - Buttons: Stop (Danger) and Keep building, which has the focus.
- **Failed.**
  - The heading by the kind of error: "Some game files need fixing" for `game`, else "The build stopped". The line under it is the kind's to-do (protocol.py's ERRORS).
  - A red callout holds the engine's sentence. For a `step` or `internal` error it adds the line about the report. For `game` it adds up to five of the check's files. Under that come the engine's last 12 lines.
  - Buttons: Report the problem (Primary) and Try again for `step` and `internal`. Otherwise, Try again or Check again (Primary).
  - Then Show the full log (Quiet). It opens the failed step's log, or setup's run.log, in the system's viewer, and is shown only if one of them exists.
  - Close (Quiet).
- **Report question.**
  - It first says what the file holds: setup's log, this PC's processor, memory, graphics card and system, and the program's version, and nothing from the game.
  - "Save and open the issue page" writes the zip and opens the form (as before). The question then shows where the zip is.
- **Stopped.** "The build is stopped", with Resume (Primary) and Close.
- **Done.** "Ready to play", "The game is built for this PC.", a Built row (the version and how long it took), Play (or Close without a launch hook), and "In the game, F1 or L3 + R3 opens the menu." This version has no build step: "Setup finished" and Close, as before.

## 4. Where the build differs from the demo, and why

- **No title bar.** The window has the system's own.
- **No dashed rings.** RmlUi draws only solid borders (read, StyleSheetSpecification.cpp has no border-style). So the busy rail mark is a dimmer solid ring, and a task still to do uses the radio_button_unchecked glyph.
- **The spinner is a glyph, turning.** It was a 16px ring of border with one bright side, and it came out uneven: RmlUi draws borders without smoothing (seen in the 10-22 shots). It is now Material Symbols' progress_activity arc, which the font draws smoothed.
- **No fixed-width font for the log's lines.** The UI embeds Cinzel, Alegreya Sans and Material Symbols only (port/ui/CMakeLists.txt), so the lines use the body font, smaller.
- **No outline on the focused button.** RmlUi has no outline, so the focused button looks like the hovered one: soul-white frame, a lighter background and a glow.
- **No Options step on the rail yet.** It comes with the options screens. Link, Check the build and Prepare shaders are not tasks either: the engine links inside its compile, and has no check or shaders phase yet (docs/49).
- **The rail's "Your game" has no line under it.** The design's "From RPCS3" or "Decrypted files" comes with the start screens.
- **The Done page has only the Built row.** The save, trophies and options rows come with the RPCS3 route and the options.
- **Before saving, the Report question does not show the file's name.** The name has the time it is saved in it.
- **Details stays on the running page** as a quiet button. It is not in the demo; it is what the screen already had.
- **The pad's buttons stay**, small, at the bottom (the user's choice).
- **The page's buttons never scroll.** In the demo they scroll with the page. The user asked that Continue, Back and the like never need scrolling, and that pages need as little scrolling as possible at the usual sizes.

## 5. Testing the screens without a build

`tools/setup/tests/fake_engine.py` stands in for the engine. It prints the protocol on a clock and does nothing else.

To use it:
1. Copy it to `tools/setup/run_setup.py` beside the exe that the test runs. A test run stages the exe in `<DES_SCRATCH>/bin`, so the copy goes in `<DES_SCRATCH>/bin/tools/setup/run_setup.py`.
2. Run with `DES_UI_OPEN=setup DES_SETUP_PYTHON=python` and a fresh `DES_SETUP_DATA`.
3. `FAKE_SETUP_MODES=hold,fail,ok` gives one mode for each run in turn. `FAKE_SETUP_SECONDS=6` makes each run take 6 s.
4. `FAKE_SETUP_APP=<DES_VERSION>` makes a finished run leave `ready.txt`, so the host finds the game Ready.
5. `DES_REPORT_NO_OPEN=1` keeps the report from opening a browser.

At about 30 presented frames a second (measured), these keys and shots went through every page:
`DES_UI_KEYS=420:RIGHT,440:ENTER,500:ESC,650:LEFT,700:ENTER,900:ENTER,1300:ENTER,1450:ESC,1500:RIGHT,1550:ENTER` and shots at 400, 480, 600, 850, 1250, 1400 and 1950. Each run of the stand-in holds its last state, so a shot that comes late shows the same page.

## 6. Next

- **The start screens, on the shell, in the design's order:**
  - welcome (or the returning states);
  - "Where is your game?" with the RPCS3 route;
  - the folder page;
  - "Ready to build".
  docs/88 rows 1 to 3 change with them.
- **The RPCS3 route.** RPCS3 keeps no decrypted files unless the player runs its Decrypt PS3 Binaries (research of 10-22), which bears on that route.
- **The options pages** (presets, "A few choices"), shown during the build, and the rail's Options step.
