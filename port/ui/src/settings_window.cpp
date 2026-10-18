// Adapted from Dusklight (CC0), src/dusk/ui/settings.cpp, by way of FFCCEOT-Recomp's settings_window.cpp: the same
// window, tabs, rows and help panes. Adapted for Demon's Souls: the rows are the port's run options (README, "Running"),
// stored in settings.ini under their environment variable names (settings.hpp). A row's value is shown on the left,
// its choices or its help on the right. A row whose option was set in the environment when the game started is
// disabled (the environment wins) and says so.
#include "settings_window.hpp"

#include "app.hpp"
#include "bool_button.hpp"
#include "controls_window.hpp"
#include "input.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "report.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace ui {
namespace {

void open_url(const char* url) {
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// What heads a row's help: the option is set from outside, or it takes effect at the next start.
Rml::String with_note(const Rml::String& key, Rml::String help) {
    const settings::Def* d = settings::find(key);
    if (settings::pinned(key)) {
        const Rml::String value = settings::pinned_value(key);
        return "<status-badge class=\"warning\">Set to " + escape(value.empty() ? Rml::String("(empty)") : value) +
               " in the environment when the game started (" + escape(key) +
               "): that value is used, and wins over this setting.</status-badge><br/>" + help;
    }
    if (d && d->apply == settings::Apply::Restart) {
        return "<status-badge class=\"info\">Takes effect the next time the game starts.</status-badge><br/>" + help;
    }
    return help;
}

// An on/off row for a "0"/"1" option. `inverted`: On means "0" (AUDIO_MUTE shown as Sound).
SelectButton& bool_row(Pane& leftPane, Pane& rightPane, const Rml::String& label, const Rml::String& key,
    Rml::String helpText, bool inverted = false) {
    helpText = with_note(key, std::move(helpText));
    const auto value_of = [key, inverted] {
        const std::string v = settings::pinned(key) ? settings::pinned_value(key) : settings::get(key);
        const bool on = !v.empty() && v != "0";
        return inverted ? !on : on;
    };
    auto& button = leftPane.add_child<BoolButton>(BoolButton::Props{
        .key = label,
        .getValue = value_of,
        .setValue =
            [key, inverted, value_of](bool value) {
                if (settings::pinned(key) || value == value_of()) {
                    return;
                }
                settings::set(key, (value != inverted) ? "1" : "0");
            },
        .isModified = [key] { return !settings::is_default(key); },
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_rml(helpText);
    });
    return button;
}

// A row with a list of choices on the right, the current one marked. `values` and `labels` line up; a value from
// settings.ini that is not in the list shows as itself. `extra` adds to the list, below the choices (Frame rate's
// Custom).
SelectButton& choice_row(Pane& leftPane, Pane& rightPane, const Rml::String& label, const Rml::String& key,
    std::vector<Rml::String> values, std::vector<Rml::String> labels, Rml::String helpText,
    std::function<void()> onChange = {}, std::function<void(Pane&)> extra = {}) {
    helpText = with_note(key, std::move(helpText));
    const auto current = [key] { return settings::pinned(key) ? settings::pinned_value(key) : settings::get(key); };
    const auto index_of = [values, current] {
        const auto it = std::find(values.begin(), values.end(), current());
        return it == values.end() ? -1 : int(it - values.begin());
    };
    auto& button = leftPane.add_select_button({
        .key = label,
        .getValue =
            [labels, index_of, current] {
                const int i = index_of();
                return i >= 0 ? labels[i] : "Custom (" + current() + ")";
            },
        .isModified = [key] { return !settings::is_default(key); },
    });
    leftPane.register_control(button, rightPane,
        [key, values = std::move(values), labels = std::move(labels), index_of, onChange, extra = std::move(extra),
            helpText = std::move(helpText)](Pane& pane) {
            for (int i = 0; i < int(labels.size()); ++i) {
                pane.add_button({
                                    .text = labels[i],
                                    .isSelected = [index_of, i] { return index_of() == i; },
                                    .isDisabled = [key] { return settings::pinned(key); },
                                })
                    .on_pressed([key, value = values[i], onChange] {
                        play_sound(kSoundItemChange);
                        settings::set(key, value);
                        if (onChange) onChange();
                    });
            }
            if (extra) {
                extra(pane);
            }
            if (!helpText.empty()) {
                pane.add_rml("<br/>" + helpText);
            }
        });
    return button;
}

// A row that only shows something (a value on the left, its explanation on the right).
void info_row(Pane& leftPane, Pane& rightPane, const Rml::String& label, const Rml::String& value, Rml::String helpRml,
    std::function<void(Pane&)> extra = {}) {
    leftPane.register_control(
        leftPane.add_select_button({
            .key = label,
            .getValue = [value] { return value; },
        }),
        rightPane, [helpRml = std::move(helpRml), extra = std::move(extra)](Pane& pane) {
            pane.add_rml(helpRml);
            if (extra) extra(pane);
        });
}

// Frame rate's fixed choices. Any other DES_FPS (a number from 30 to 500, des_options.cpp's fps_parse) is its Custom
// choice, whose number the Custom frame rate row sets; fps_custom keeps that number while a fixed choice is on.
const std::vector<Rml::String> kFpsValues = {"", "30", "60", "120", "144", "165", "240", "unlocked"};
constexpr int kFpsMin = 30;
constexpr int kFpsMax = 500;

std::string fps_setting() {
    return settings::pinned("DES_FPS") ? settings::pinned_value("DES_FPS") : settings::get("DES_FPS");
}

bool fps_is_custom() {
    return std::find(kFpsValues.begin(), kFpsValues.end(), fps_setting()) == kFpsValues.end();
}

// The Custom frame rate: DES_FPS while it is custom, else the last one set.
int fps_custom() {
    const std::string v = fps_is_custom() ? fps_setting() : settings::get("fps_custom");
    return std::clamp(std::atoi(v.c_str()), kFpsMin, kFpsMax);
}

// The software this program is made of (mirrors port/ui/NOTICE.md).
struct Credit {
    const char* name;
    const char* use;
    const char* licence;
    const char* url;  // empty: no page to open
};
constexpr Credit kCredits[] = {
    {"ps3recomp", "The recompiler and runtime the port is built on, by Ned Heller (sp00nznet) and its contributors.", "MIT",
     "https://github.com/sp00nznet/ps3recomp"},
    {"RmlUi", "The menus.", "MIT", "https://github.com/mikke89/RmlUi"},
    {"FreeType", "Text in the menus. Portions of this software are copyright © 2025 The FreeType Project "
                  "(www.freetype.org). All rights reserved.",
     "FreeType Licence (FTL)", "https://freetype.org"},
    {"Dusklight", "The start screen, menu bar and settings windows are adapted from its interface code.", "CC0",
     "https://github.com/TwilitRealm/dusklight"},
    {"Cinzel and Alegreya Sans", "The fonts of the menus.", "SIL Open Font Licence 1.1", "https://fonts.google.com"},
    {"Material Symbols", "The icons of the menus.", "Apache-2.0", "https://fonts.google.com/icons"},
    {"RPCS3", "A behavioural reference for the PS3 system libraries. No RPCS3 code is copied.", "", "https://rpcs3.net"},
};

}  // namespace

SettingsWindow::SettingsWindow(bool prelaunch) : mPrelaunch(prelaunch) {
    add_tab("Display", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        choice_row(leftPane, rightPane, "Render resolution", "RSX_SCALE", {"100", "150", "200", "300"},
            {"100% (original)", "150%", "200%", "300%"},
            "Draws the game at a multiple of the PS3's 1280x720: 150% is 1920x1080, 200% 2560x1440, 300% "
            "3840x2160. The picture is then fitted to the window, so a higher setting is sharper even in a smaller "
            "window. The HUD and menus are drawn at the same size. Costs GPU time: about four times the pixels at "
            "200%. Applies at once.<br/><br/>(RSX_SCALE)");
        choice_row(leftPane, rightPane, "Texture filtering", "RSX_TEXFILTER", {"0", "1", "2", "3"},
            {"Original", "Nearest", "Bilinear", "Trilinear"},
            "How the game's surface textures are smoothed. Original uses the filter the game picks for each "
            "texture. Nearest shows the texels as sharp squares, a pixelated look. Bilinear smooths within each "
            "mipmap level; trilinear also blends between levels, so there are no visible bands where the detail "
            "drops off. Textures the game reads as data (colour grading, the screen itself) always keep the game's "
            "filter, because changing it breaks their colours. Applies at once.<br/><br/>(RSX_TEXFILTER)");
        choice_row(leftPane, rightPane, "Anisotropic filtering", "RSX_ANISO", {"0", "1", "2", "4", "8", "16"},
            {"Original (4x)", "Off", "2x", "4x", "8x", "16x"},
            "Keeps textures sharp when they are seen at a steep angle, such as floors and walls into the distance. "
            "Original uses the level the game asks for, which is 4x on most surfaces. Costs a little GPU time at "
            "16x. Applies at once.<br/><br/>(RSX_ANISO)");
        bool_row(leftPane, rightPane, "Depth of field", "DES_DOF",
            "The game's depth of field: distant scenery is blurred, more in some areas than others. Off draws it "
            "sharp. Applies at once.<br/><br/>(DES_DOF)");
        choice_row(leftPane, rightPane, "Frame rate", "DES_FPS", kFpsValues,
            {"30 fps (original)", "30 fps, real frame time", "60 fps", "120 fps", "144 fps", "165 fps", "240 fps",
                "Unlocked"},
            "The game's own frame rate mode. The original 30 fps mode draws a near-empty frame when one runs late. "
            "The others give the game the real time of each frame; 60 uses the game's 60 fps mode, and the other "
            "rates set the frame cap to the number chosen. Unlocked has no cap: the game runs as fast as the "
            "machine allows, and with V-sync on the display still shows at most its refresh rate. Custom is the "
            "rate set in Custom frame rate, below. In-engine cutscenes stay at 30 fps, as on the PS3. Above 60 fps "
            "the game is less tested. Applies at once.<br/><br/>(DES_FPS)",
            {}, [](Pane& pane) {
                pane.add_button({
                                    .text = "Custom",
                                    .isSelected = [] { return fps_is_custom(); },
                                    .isDisabled = [] { return settings::pinned("DES_FPS"); },
                                })
                    .on_pressed([] {
                        play_sound(kSoundItemChange);
                        settings::set("DES_FPS", std::to_string(fps_custom()));
                    });
            });
        auto& customFps = leftPane.add_child<NumberButton>(NumberButton::Props{
            .key = "Custom frame rate",
            .getValue = fps_custom,
            .setValue =
                [](int value) {
                    if (settings::pinned("DES_FPS")) {
                        return;
                    }
                    settings::set("fps_custom", std::to_string(value));
                    settings::set("DES_FPS", std::to_string(value));
                },
            .isDisabled = [] { return settings::pinned("DES_FPS"); },
            .isModified = fps_is_custom,
            .min = kFpsMin,
            .max = kFpsMax,
            .suffix = " fps",
        });
        leftPane.register_control(customFps, rightPane,
            [helpText = with_note("DES_FPS",
                 "A frame rate of your own, from 30 to 500 fps: Frame rate's Custom choice. Changing it here "
                 "picks Custom. Left and right change it by one; confirm to type a number. Applies at once."
                 "<br/><br/>(DES_FPS)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });
        bool_row(leftPane, rightPane, "V-sync", "DES_VSYNC",
            "Presents wait for the display's refresh. Off shows each frame as soon as it is drawn, with tearing: "
            "needed to see a frame rate above the display's refresh rate. Applies at once.<br/><br/>(DES_VSYNC)");
        bool_row(leftPane, rightPane, "Motion blur", "DES_MOTION_BLUR",
            "The game's camera motion blur. Off draws the scene sharp while the camera moves. Applies at once."
            "<br/><br/>(DES_MOTION_BLUR)");
    });

    add_tab("Game", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Start");
        choice_row(leftPane, rightPane, "Boot logos", "DES_SKIP_INTRO", {"0", "1", "2"},
            {"Show (original)", "Skip", "Last logo only"},
            "The company logos at boot. Skip goes straight past them, as RPCS3's Skip Intro patch does. Last logo "
            "only shows the last one, about 2 s. Either way the game takes its own way to the title screen."
            "<br/><br/>(DES_SKIP_INTRO)");
        bool_row(leftPane, rightPane, "Skip opening movie", "DES_SKIP_INTRO_MOVIE",
            "Skips the opening movie after the logos (the title screen also replays it when left idle). The title "
            "screen's own movie, the one after character creation and the endings still play. Applies from the "
            "next movie.<br/><br/>(DES_SKIP_INTRO_MOVIE)");
        bool_row(leftPane, rightPane, "Continue automatically", "DES_AUTO_CONTINUE",
            "After Play on the start screen, presses through the notices and the title screen into the last "
            "save, without touching a button.<br/><br/>(DES_AUTO_CONTINUE)");

        leftPane.add_section("Sound");
        bool_row(leftPane, rightPane, "Sound", "AUDIO_MUTE",
            "The game's sound and music. Off mutes them (the game keeps its timing). Applies at once."
            "<br/><br/>(AUDIO_MUTE)",
            true);
    });

    add_tab("Controls", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        const Rml::String chord = input::chord_label(settings::get_int("pad_menu_chord"));
        leftPane.add_section("This menu");
        info_row(leftPane, rightPane, "Open the menu", "F1 / " + chord,
            "Over the game, F1 or Esc on the keyboard, or " + escape(chord) +
                " on a gamepad (Settings, Interface, changes the gamepad buttons). The game does not see the keys and "
                "buttons while a menu is open.");
        info_row(leftPane, rightPane, "In the menus", "Arrows, Enter, Esc",
            "Keyboard: the arrows move, Enter chooses, Esc goes back; Page Up and Page Down change tabs. The mouse "
            "works too.<br/><br/>Gamepad: the D-pad or the left stick moves, Cross chooses, Circle goes back, L1 and "
            "R1 change tabs, Start closes the menu.");

        leftPane.add_section("The game");
        leftPane.register_control(
            leftPane.add_group_button({.text = "Change Buttons"}).on_pressed([this] {
                play_sound(kSoundClick);
                push(std::make_unique<ControlsWindow>());
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Choose which gamepad button or key presses each of the PS3 pad's buttons in the game.");
            });
        info_row(leftPane, rightPane, "Gamepads", "XInput, DualSense, DualShock 4",
            "Xbox-style (XInput) controllers, and the DualSense, DualSense Edge and DualShock 4 over USB or "
            "Bluetooth, are read directly. The game sees the first one as the PS3's pad.");
        info_row(leftPane, rightPane, "Keyboard", "Arrows, Z X A S",
            "Without a gamepad the keyboard is the PS3 pad:<br/>"
            "<b>Arrows</b> D-pad (and left stick)<br/>"
            "<b>Z</b> Cross &nbsp; <b>X</b> Circle &nbsp; <b>A</b> Square &nbsp; <b>S</b> Triangle<br/>"
            "<b>Q</b> L1 &nbsp; <b>W</b> R1 &nbsp; <b>1</b> L2 &nbsp; <b>2</b> R2<br/>"
            "<b>Enter</b> Start &nbsp; <b>Tab</b> Select<br/><br/>"
            "These are the defaults; Change Buttons changes them.");
    });

    add_tab("Interface", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        choice_row(leftPane, rightPane, "UI scale", "ui_scale", {"0", "75", "100", "125", "150"},
            {"Auto", "75%", "100%", "125%", "150%"},
            "The size of these menus. Auto follows the display's scale in Windows' display settings; the "
            "percentages are of that. Has no effect on the game's own menus.",
            [] { apply_scale(); });
        bool_row(leftPane, rightPane, "Start screen", "show_start_screen",
            "Shows the start screen (Play, Settings, Quit) over the game as it boots. Off goes straight to the game; "
            "the menu is still there with F1. Never shown when the game is started by a test script or with Continue "
            "automatically.");
        bool_row(leftPane, rightPane, "Menu hint", "menu_hint",
            "After Play, a note at the top of the screen says how to open this menu.");
        choice_row(leftPane, rightPane, "Gamepad menu buttons", "pad_menu_chord", {"0", "1"},
            {"L3 + R3", "Select + Start"},
            "The buttons that open this menu over the game, pressed together. L3 + R3 (both sticks pressed in) is "
            "least in the way: the game also sees R3, which locks on. Select + Start also opens the game's own "
            "menu.");
    });

    add_tab("About", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("This program");
        info_row(leftPane, rightPane, "Version", kVersion,
            "Demon's Souls, PC port: a static recompilation of the PlayStation 3 game.<br/><br/>Version " +
                escape(kVersion) + ".",
            [](Pane& pane) {
                pane.add_button("Open the project page").on_pressed([] {
                    play_sound(kSoundClick);
                    open_url("https://github.com/JonathanDC64/DemonsSoulsRecomp");
                });
            });
        info_row(leftPane, rightPane, "ps3recomp", kPs3recompVersion,
            "The recompiler and runtime this program was built with: ps3recomp, from this project's fork. A release "
            "of the fork shows its tag (des-...), anything else its commit.<br/><br/>" + escape(kPs3recompVersion) +
                ".");
        {
            const Rml::String sha = kDumpSha256;
            Rml::String grouped;  // (in fours of 16, so the help pane can wrap it)
            for (size_t i = 0; i < sha.size(); i += 16) grouped += (i ? " " : "") + sha.substr(i, 16);
            info_row(leftPane, rightPane, "Game dump", sha.empty() ? Rml::String("unknown") : sha.substr(0, 12),
                "The game executable this program was built from: PS3_GAME/USRDIR/EBOOT.BIN, decrypted. Its SHA-256 "
                "is the same for every copy of the same release of the game.<br/><br/>" +
                    (sha.empty() ? Rml::String("Not known: the build did not find work/game/EBOOT.ELF.") : grouped));
        }
        info_row(leftPane, rightPane, "Licence", "MIT",
            "This port's own code is under the MIT licence. The game itself is not: it needs your own copy of Demon's "
            "Souls.<br/><br/>Demon's Souls is a trademark of Sony Interactive "
            "Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment, "
            "FromSoftware or Atlus.");
        info_row(leftPane, rightPane, "Settings file", settings::path().empty() ? "(not kept)" : "settings.ini",
            settings::path().empty()
                ? Rml::String("This run does not keep its settings (a test script started it).")
                : "The settings are kept in<br/>" + escape(settings::path()) +
                      "<br/><br/>An environment variable of the same name (DES_FPS=60 ...) wins over the file." +
                      (report::data_dir().empty()
                           ? Rml::String()
                           : "<br/><br/>The same folder holds the game's log (logs), crash reports (crashes) and "
                             "issue reports (reports). Your saves are in the game's own folders, not here."));

        leftPane.add_section("Made with");
        for (const Credit& credit : kCredits) {
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = credit.name,
                    .getValue = [] { return Rml::String(); },
                }),
                rightPane, [&credit](Pane& pane) {
                    pane.add_text(credit.use);
                    if (credit.licence[0]) pane.add_rml(std::string("<br/>Licence: ") + escape(credit.licence));
                    if (credit.url[0]) {
                        pane.add_button("Open the website").on_pressed([url = credit.url] {
                            play_sound(kSoundClick);
                            open_url(url);
                        });
                    }
                });
        }
    });
}

void SettingsWindow::update() {
    Window::update();
}

void SettingsWindow::hide(bool close) {
    settings::save();
    Window::hide(close);
}

}  // namespace ui
