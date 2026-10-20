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
#include "string_button.hpp"
#include "ui.hpp"
#include "version.hpp"
#if DES_DEV
#include "../../des_dev_marker.h"
#endif

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Mouse sensitivity, shown as a percentage of its default (DES_MOUSE_SENS, degrees per count; des_mouse_look in
// des_options.cpp).
constexpr int kMouseSensMin = 5;
constexpr int kMouseSensMax = 500;

double mouse_sens_default() {
    const settings::Def* d = settings::find("DES_MOUSE_SENS");
    return d ? std::atof(d->def) : 0.09;
}

int mouse_sens_percent() {
    const std::string v =
        settings::pinned("DES_MOUSE_SENS") ? settings::pinned_value("DES_MOUSE_SENS") : settings::get("DES_MOUSE_SENS");
    const double sens = std::atof(v.c_str());
    if (!(sens > 0.0) || !(mouse_sens_default() > 0.0)) return 100;  // (des_mouse_look uses the default then)
    return std::clamp(int(std::lround(sens / mouse_sens_default() * 100.0)), kMouseSensMin, kMouseSensMax);
}

// The Network tab's rows (settings.hpp, "Online play"). A value in use: the environment's when set there.
std::string net_value(const char* key) {
    return settings::pinned(key) ? settings::pinned_value(key) : settings::get(key);
}

// A developer build's cheat session (des_dev_boot set a cheat at start and cleared the network knobs; docs/55 section
// 6). Release builds never set DES_DEV_SESSION.
bool net_cheat_session() {
    const char* s = std::getenv("DES_DEV_SESSION");
    return s && std::strcmp(s, "cheats") == 0;
}

std::string trim_spaces(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

int net_p2p_port() {
    const int port = std::atoi(net_value("DES_NET_P2P_PORT").c_str());
    return port < settings::kNetP2pPortMin || port > settings::kNetP2pPortMax ? settings::kNetP2pPortDefault : port;
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

        choice_row(leftPane, rightPane, "Aspect ratio", "DES_ASPECT",
            {"", "2560:1080", "3440:1440", "3840:1600", "32:9", "auto"},
            {"16:9 (original)", "21:9 (2560x1080)", "21:9 (3440x1440)", "21:9 (3840x1600)", "32:9", "Match the display"},
            "The shape of the picture, for ultrawide displays. A wider shape shows more of the scene at the sides, "
            "with the same height as at 16:9. The window opens in that shape, and fullscreen fills an ultrawide "
            "display. The game still draws 1280 pixels across, stretched to the wider shape, so raise Render "
            "resolution to keep it sharp. Match the display uses the main display's shape. Displays narrower than "
            "16:9 (16:10, 4:3) keep 16:9 with bars. The HUD and menus stay 16:9 in the middle (see HUD below); "
            "full-screen fades and the dim behind menus may still cover only that middle.<br/><br/>(DES_ASPECT)");
        choice_row(leftPane, rightPane, "HUD", "DES_HUD", {"", "stretch"}, {"Centred 16:9", "Stretched"},
            "Where the HUD and menus go when Aspect ratio is wider than 16:9. Centred keeps their size and shape in "
            "a 16:9 area in the middle of the screen, and keeps the markers on things in the world (the lock-on "
            "reticle, enemy health bars) on them. Stretched spreads everything across the full width, wider than "
            "it is drawn. No effect at 16:9.<br/><br/>(DES_HUD)");
        choice_row(leftPane, rightPane, "Render resolution", "RSX_SCALE", {"100", "150", "200", "300"},
            {"100% (original)", "150%", "200%", "300%"},
            "Draws the game at a multiple of the PS3's 1280x720: 150% is 1920x1080, 200% 2560x1440, 300% "
            "3840x2160. The picture is then fitted to the window, so a higher setting is sharper even in a smaller "
            "window. The HUD and menus are drawn at the same size. Costs GPU time: about four times the pixels at "
            "200%. Applies at once.<br/><br/>(RSX_SCALE)");
        choice_row(leftPane, rightPane, "Scaling filter", "RSX_OUTFILTER", {"0", "1"}, {"Bilinear", "Sharp"},
            "How the finished picture is fitted to the window. Bilinear is the plain filter used so far. Sharp "
            "averages all the pixels the picture has under each window pixel when it is bigger than the window "
            "(a render resolution above the window's size), so fine detail does not shimmer, and uses a bicubic "
            "filter when it is smaller, which is crisper than bilinear. Applies at once.<br/><br/>(RSX_OUTFILTER)");
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
        choice_row(leftPane, rightPane, "Anti-aliasing", "RSX_AA", {"0", "1"}, {"Off (original)", "FXAA"},
            "Smooths the jagged edges of the 3D scene. FXAA is a filter over the finished scene, run before the "
            "HUD is drawn, so text and the HUD stay sharp. It also softens fine texture detail a little. Costs a "
            "little GPU time, more at a higher render resolution. Applies at once.<br/><br/>(RSX_AA)");
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
            "rate set in Custom frame rate, below. In-engine cutscenes play at 30 fps, as on the PS3, unless "
            "Cutscene frame rate says otherwise. Above 60 fps the game is less tested. Applies at once."
            "<br/><br/>(DES_FPS)",
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
        choice_row(leftPane, rightPane, "Cutscene frame rate", "DES_CUTFPS", {"0", "1"},
            {"30 fps (original)", "Same as Frame rate"},
            "The frame rate of the in-engine cutscenes. Their camera moves in steps of 1/30 s, so above 30 fps they "
            "judder; 30 fps (original) plays them at 30, as on the PS3, and goes back to Frame rate when they end. "
            "Same as Frame rate keeps them at the frame rate and moves the camera smoothly between its steps. "
            "Movies are not affected, and at 30 fps this does nothing. Applies at once.<br/><br/>(DES_CUTFPS)");
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
            "Over the game, F1 on the keyboard, or " + escape(chord) +
                " on a gamepad (Settings, Interface, changes the gamepad buttons). In play Esc is the game's own menu "
                "(Start); in these menus it goes back. The game does not see the keys and buttons while a menu is "
                "open.");
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
                pane.add_text("Choose which gamepad button presses each of the PS3 pad's buttons in the game, and "
                              "which keys, mouse buttons or wheel notches work each action (two each, with Shift, "
                              "Ctrl or Alt for a chord).");
            });
        info_row(leftPane, rightPane, "Gamepads", "XInput, DualSense, DualShock 4",
            "Xbox-style (XInput) controllers, and the DualSense, DualSense Edge and DualShock 4 over USB or "
            "Bluetooth, are read directly. The game sees the first one as the PS3's pad.");
        info_row(leftPane, rightPane, "Keyboard and mouse", "WASD, mouse, Space",
            "The keyboard and mouse work as in the Souls games on PC, with a gamepad or without:<br/>"
            "<b>W A S D</b> move &nbsp; <b>Left Alt</b> (held) walk &nbsp; <b>Mouse</b> camera (Mouse look, "
            "below)<br/>"
            "<b>Left button</b> attack (R1) &nbsp; <b>Shift + left button</b> strong attack (R2)<br/>"
            "<b>Right button</b> guard (L1) &nbsp; <b>Left Ctrl</b> or <b>Shift + right button</b> parry (L2)<br/>"
            "<b>Space</b> roll, backstep, dash (Circle) &nbsp; <b>C</b> left stick press (L3)<br/>"
            "<b>Q</b> or <b>middle button</b> lock on (R3)<br/>"
            "<b>E</b> action (Cross) &nbsp; <b>R</b> use item (Square) &nbsp; <b>F</b> two-hand (Triangle)<br/>"
            "<b>Arrows</b> D-pad &nbsp; <b>Wheel up / down</b> spell / item &nbsp; <b>Shift + wheel up / down</b> "
            "right / left hand<br/>"
            "<b>Esc</b> menu (Start) &nbsp; <b>G</b> or <b>Tab</b> Select<br/><br/>"
            "A chord works alone: Shift and the left button is a strong attack, not an attack as well. The mouse's "
            "buttons and wheel need Mouse look on. Change Buttons changes any of these.");
        leftPane.add_section("Mouse");
        bool_row(leftPane, rightPane, "Mouse look", "DES_MOUSE_LOOK",
            "Turns the camera with the mouse, as well as with the right stick, and gives the game the mouse's "
            "buttons and wheel (Keyboard and mouse, above). In play the cursor is hidden and held in the window; on "
            "the title screen, in loading screens and in cutscenes it is free. Opening this menu (F1), switching to "
            "another window or moving the window frees it too, and the game takes it again when you come back, once "
            "the mouse buttons are up. While locked on, the camera "
            "follows the target, and a move of the mouse to the left or right switches targets (Switch targets, "
            "below). The camera turns once a game frame, so it is smoothest at 60 fps or more (Display, Frame "
            "rate). Applies at once: the game takes the mouse when "
            "this menu closes.<br/><br/>(DES_MOUSE_LOOK)");
        auto& mouseSens = leftPane.add_child<NumberButton>(NumberButton::Props{
            .key = "Mouse sensitivity",
            .getValue = mouse_sens_percent,
            .setValue =
                [](int value) {
                    if (settings::pinned("DES_MOUSE_SENS")) {
                        return;
                    }
                    settings::set("DES_MOUSE_SENS", std::format("{:g}", mouse_sens_default() * value / 100.0));
                },
            .isDisabled = [] { return settings::pinned("DES_MOUSE_SENS"); },
            .isModified = [] { return !settings::is_default("DES_MOUSE_SENS"); },
            .min = kMouseSensMin,
            .max = kMouseSensMax,
            .step = 5,
            .suffix = "%",
        });
        leftPane.register_control(mouseSens, rightPane,
            [helpText = with_note("DES_MOUSE_SENS",
                 "How far the camera turns for a move of the mouse. 100% is 0.09 degrees for each count the mouse "
                 "reports: a full turn for 4000 counts, about 13 cm at 800 dpi. From 5% to 500%; left and right "
                 "change it by 5%, confirm to type a number. Applies at once.<br/><br/>(DES_MOUSE_SENS, in degrees "
                 "per count)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });
        bool_row(leftPane, rightPane, "Invert vertical", "DES_MOUSE_INVERT_Y",
            "Off: moving the mouse forward looks up. On: forward looks down, like an aircraft's controls. Applies at "
            "once.<br/><br/>(DES_MOUSE_INVERT_Y)");
        bool_row(leftPane, rightPane, "Invert horizontal", "DES_MOUSE_INVERT_X",
            "Off: moving the mouse right turns the camera right. On: it turns left. Applies at once."
            "<br/><br/>(DES_MOUSE_INVERT_X)");
        choice_row(leftPane, rightPane, "Switch targets", "DES_MOUSE_FLICK", {"0", "8", "15", "30"},
            {"Off", "Short move", "Medium move", "Long move"},
            "While locked on, a move of the mouse to the left or right switches to the next target that way, as a "
            "flick of the right stick does. How far the mouse has to move: as far as would turn the camera 8, 15 or "
            "30 degrees at the sensitivity above. Applies at once.<br/><br/>(DES_MOUSE_FLICK, in degrees)");
    });

    // Online play (docs/46 A5; settings.cpp's apply_network turns the rows into the runtime's knobs at start).
    add_tab("Network", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Online play");
        auto& online = leftPane.add_child<BoolButton>(BoolButton::Props{
            .key = "Online",
            .getValue =
                [] {
                    const std::string v = net_value("DES_ONLINE");
                    return !v.empty() && v != "0";
                },
            .setValue =
                [](bool value) {
                    if (settings::pinned("DES_ONLINE") || net_cheat_session()) {
                        return;
                    }
                    settings::set("DES_ONLINE", value ? "1" : "0");
                },
            .isDisabled = [] { return settings::pinned("DES_ONLINE") || net_cheat_session(); },
            .isModified = [] { return !settings::is_default("DES_ONLINE"); },
            .valueOverride = [] { return net_cheat_session() ? Rml::String("Off: cheats used") : Rml::String(); },
        });
        leftPane.register_control(online, rightPane, [](Pane& pane) {
            pane.clear();
            Rml::String help;
            if (net_cheat_session()) {
                help = "<status-badge class=\"warning\">Off for this session: a developer cheat was set when the game "
                       "started, and a session with cheats stays offline (DES_DEV_SESSION=cheats). The setting is kept "
                       "for the next start.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_ONLINE",
                "Connects the game to the server chosen below, for messages, bloodstains, phantoms and world "
                "tendency, as on the PS3. Off plays offline. Changing it takes effect the next time the game "
                "starts.<br/><br/>(DES_ONLINE)"));
        });

        std::vector<Rml::String> serverValues, serverLabels;
        Rml::String serverHelp = "The game server to connect to. ";
        for (const settings::NetServer& s : settings::kNetServers) {
            serverValues.push_back(s.key);
            serverLabels.push_back(s.name);
        }
        serverHelp += "Each address is the one the server's own setup page gives for emulators.<br/>";
        for (const settings::NetServer& s : settings::kNetServers) {
            serverHelp += "<br/><b>" + escape(s.name) + "</b>" + (*s.address ? " (" + escape(s.address) + ")" : "") +
                          ": " + escape(s.note);
        }
        choice_row(leftPane, rightPane, "Server", "DES_NET_SERVER", std::move(serverValues), std::move(serverLabels),
            serverHelp + "<br/><br/>(DES_NET_SERVER)");

        auto& customServer = leftPane.add_child<StringButton>(StringButton::Props{
            .key = "Custom server",
            .getValue = [] { return Rml::String(net_value("DES_NET_CUSTOM")); },
            .setValue =
                [](Rml::String value) {
                    if (settings::pinned("DES_NET_CUSTOM")) {
                        return;
                    }
                    value = trim_spaces(value);
                    settings::set("DES_NET_CUSTOM", value);
                    // (as Custom frame rate picks Frame rate's Custom: an address typed here picks Custom)
                    if (!value.empty() && !settings::pinned("DES_NET_SERVER")) {
                        settings::set("DES_NET_SERVER", settings::kNetCustomKey);
                    }
                },
            .isDisabled = [] { return settings::pinned("DES_NET_CUSTOM"); },
            .isModified = [] { return !settings::is_default("DES_NET_CUSTOM"); },
            .maxLength = 253,
        });
        leftPane.register_control(customServer, rightPane, [](Pane& pane) {
            pane.clear();
            Rml::String help;
            const std::string v = trim_spaces(net_value("DES_NET_CUSTOM"));
            if (net_value("DES_NET_SERVER") == settings::kNetCustomKey && !settings::net_address_ok(v)) {
                help = "<status-badge class=\"warning\">" +
                       Rml::String(v.empty() ? "No address yet" : "Not an IPv4 address or a host name") +
                       ": with Server on Custom the game stays offline.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_NET_CUSTOM",
                "The address of a server not in the list, for Server's Custom choice: an IPv4 address "
                "(203.0.113.5) or a host name (des.example.org), as the server's setup page gives it. Typing one "
                "here picks Custom. Confirm to type.<br/><br/>(DES_NET_CUSTOM)"));
        });

        auto& name = leftPane.add_child<StringButton>(StringButton::Props{
            .key = "Online name",
            .getValue = [] { return Rml::String(net_value("DES_NET_NAME")); },
            .setValue =
                [](Rml::String value) {
                    if (settings::pinned("DES_NET_NAME")) {
                        return;
                    }
                    settings::set("DES_NET_NAME", trim_spaces(value));
                },
            .isDisabled = [] { return settings::pinned("DES_NET_NAME"); },
            .isModified = [] { return !settings::is_default("DES_NET_NAME"); },
            .maxLength = 16,
        });
        leftPane.register_control(name, rightPane, [](Pane& pane) {
            pane.clear();
            Rml::String help;
            const std::string v = net_value("DES_NET_NAME");
            if (v.empty()) {
                help = "<status-badge class=\"info\">No name set: online play needs one, so the game stays offline "
                       "until you set it.</status-badge><br/>";
            } else if (!settings::net_name_ok(v)) {
                help = "<status-badge class=\"warning\">Not valid, so the game stays offline: a name has 3 to 16 "
                       "letters, digits, - or _.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_NET_NAME",
                "The name other players and the server know you by, like a PSN online ID: 3 to 16 letters, digits, "
                "- or _. The server keeps your messages and stats under it, so pick one nobody else uses. Confirm to "
                "type.<br/><br/>(DES_NET_NAME)"));
        });

        auto& password = leftPane.add_child<StringButton>(StringButton::Props{
            .key = "Matchmaking password",
            .getValue = [] { return Rml::String(net_value("DES_NET_PASSWORD")); },
            .setValue =
                [](Rml::String value) {
                    if (settings::pinned("DES_NET_PASSWORD")) {
                        return;
                    }
                    settings::set("DES_NET_PASSWORD", trim_spaces(value));
                },
            .isDisabled = [] { return settings::pinned("DES_NET_PASSWORD"); },
            .isModified = [] { return !settings::is_default("DES_NET_PASSWORD"); },
            .maxLength = 32,
        });
        leftPane.register_control(password, rightPane,
            [helpText = with_note("DES_NET_PASSWORD",
                 "Plays only with the players who set the same password: you see only their summon signs and they "
                 "see only yours, and invasions and the Old Monk match only within the group. Messages, "
                 "bloodstains, ghosts and world tendency stay shared with everyone on the server. Empty plays with "
                 "everyone, as on the PS3. The password stays on this PC: the server gets a hash of it. Only "
                 "servers that support it (dessego with matchmaking groups) use it; others match everyone. Applies at "
                 "once; a sign you already put down stays in the old group until it is gone. Confirm to type.<br/><br/>(DES_NET_PASSWORD)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });

        choice_row(leftPane, rightPane, "World tendency", "DES_NET_TENDENCY", {"", "own"},
            {"With the server's", "Yours only"},
            "Online, the game mixes the server's world tendency, the average of its players, into yours: half and "
            "half each time you load your game, and the server's alone for a new character. Yours only keeps the "
            "server out of it, as offline: only what you do changes your world. Your tendency still goes to the "
            "server for the others' average, and as a phantom you still see your host's world. A mix already "
            "saved stays. Applies from the next load.<br/><br/>(DES_NET_TENDENCY)");

        choice_row(leftPane, rightPane, "Message ratings", "DES_NET_RATING_POLL", {"", "30"},
            {"Every 5 minutes", "Every 30 seconds"},
            "How often the game asks the server whether someone rated one of your messages. When someone has, you "
            "are healed and told so. The game asks every 5 minutes, as on the PS3, so a rating can show up to 5 "
            "minutes late. Every 30 seconds sends one small request more every 30 seconds. Applies at once."
            "<br/><br/>(DES_NET_RATING_POLL)");

        auto& port = leftPane.add_child<NumberButton>(NumberButton::Props{
            .key = "P2P port",
            .getValue = net_p2p_port,
            .setValue =
                [](int value) {
                    if (settings::pinned("DES_NET_P2P_PORT")) {
                        return;
                    }
                    settings::set("DES_NET_P2P_PORT", std::to_string(value));
                },
            .isDisabled = [] { return settings::pinned("DES_NET_P2P_PORT"); },
            .isModified = [] { return !settings::is_default("DES_NET_P2P_PORT"); },
            .min = settings::kNetP2pPortMin,
            .max = settings::kNetP2pPortMax,
        });
        leftPane.register_control(port, rightPane,
            [helpText = with_note("DES_NET_P2P_PORT",
                 "The UDP port the game uses to connect directly to other players, for co-op and invasions. 3658 is "
                 "the PS3's. Change it only if another program uses it or your router forwards another one. From "
                 "1024 to 65535; left and right change it by one, confirm to type a number.<br/><br/>"
                 "(DES_NET_P2P_PORT)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });

        choice_row(leftPane, rightPane, "Your movement updates", "DES_NET_SYNC", {"", "15"},
            {"Original (every 6th frame)", "Smoother (at least 15 a second)"},
            "How often the game sends your position to the players in your world. The original sends it every "
            "sixth frame, 5 times a second at 30 fps, and the other players' games chase the last position they "
            "got: when you stop, they see you walk on for most of a second. Smoother sends it at least 15 times a second, "
            "so they see you stop within about a quarter of a second. It changes what your game sends, so it "
            "smooths you on their screens; they need it for you to see them smoother. Under 1 KB a second "
            "more. Applies at once.<br/><br/>(DES_NET_SYNC)");

        leftPane.add_section("This session");
        info_row(leftPane, rightPane, "Network", net_cheat_session() ? Rml::String("Off: cheats used") : settings::net_status(),
            net_cheat_session()
                ? Rml::String("A developer cheat was set when the game started, so this session is offline "
                              "(DES_DEV_SESSION=cheats): cheats never reach other players. Start the game without "
                              "them to go online.")
                : "What the game started with: " + escape(settings::net_status()) +
                      ". The rows above take effect the next time the game starts.");
        info_row(leftPane, rightPane, "About online play", "Read first",
            "Online play is new in this port and less tested than playing offline.<br/><br/>"
            "Co-op and invasions connect you directly to other players, so they can see your IP address, as on a "
            "PS3.<br/><br/>Summoning and being summoned need an RPCN account, which the port does not support yet "
            "(coming later). Messages, bloodstains and phantoms come from the server alone.<br/><br/>"
            "Developer builds: while a cheat is in use the session stays offline, and Online shows Off.");
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
        bool_row(leftPane, rightPane, "Trophy pop-ups", "trophy_popups",
            "When you earn a trophy, a note at the top right shows its icon, grade and name for a few seconds. Off "
            "hides the note only: the trophy is still earned and kept.");
        choice_row(leftPane, rightPane, "Gamepad menu buttons", "pad_menu_chord", {"0", "1"},
            {"L3 + R3", "Select + Start"},
            "The buttons that open this menu over the game, pressed together. L3 + R3 (both sticks pressed in) is "
            "least in the way: the game also sees R3, which locks on. Select + Start also opens the game's own "
            "menu.");
    });

#if DES_DEV
    // Developer builds only (cmake -DDES_DEV=ON; docs/55, des_dev.cpp). The rows are live and never saved.
    add_tab("Developer", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        // des_dev_boot sets the session at start (cheats, online or offline); des_dev.cpp makes it online for good
        // when PS3_NET_ONLINE or PSNR_SERVER is set later. The tab is built again each time it is shown.
        const char* session = std::getenv("DES_DEV_SESSION");
        if (session && std::strcmp(session, "online") == 0) {
            info_row(leftPane, rightPane, "Cheats", "Off: online session",
                "This session is online (Network, Online; or PS3_NET_ONLINE or PSNR_SERVER is or was set), so the "
                "cheats are off until the next start. Turn Online off in the Network tab and start the game again "
                "to use the cheats.");
            return;
        }
        info_row(leftPane, rightPane, "Developer build", "Cheats compiled in",
            "This is a developer build (" DES_DEV_MARKER "). The cheats below are for testing. Turning one on "
            "keeps this session offline; if the network is turned on later, they stop until the next start. They "
            "are not saved: each start begins with all of them off.");
        bool_row(leftPane, rightPane, "God mode", "DES_GOD",
            "Your HP, MP and stamina never drop: hits, falls, poison, spells, attacks, rolls, guarding and dashing "
            "cost nothing. Instant deaths still kill: falling out of the level, lethal grabs and scripted deaths."
            "<br/><br/>(DES_GOD)");
        choice_row(leftPane, rightPane, "Attack power", "DES_ATK", {"1", "2", "5", "10", "100", "0"},
            {"Normal", "2x", "5x", "10x", "100x", "No damage"},
            "Multiplies the damage your hits do to others. Any other number can be set with DES_ATK."
            "<br/><br/>(DES_ATK)");
        choice_row(leftPane, rightPane, "Movement speed", "DES_SPEED", {"1", "1.5", "2", "3", "0.5"},
            {"Normal", "1.5x", "2x", "3x", "Half"},
            "Multiplies how far you move: walking, running, dashing, rolling and attack lunges. Animations keep "
            "their speed. Very high values may let you pass through thin walls.<br/><br/>(DES_SPEED)");
        info_row(leftPane, rightPane, "Warp (scripts)", "DES_WARP=x,y,z",
            "For test scripts and the debug console. DES_WARP=x,y,z puts you at x, y, z the way the level scripts "
            "place characters; each new value warps once. DES_WARP_CLEAR=m first sends the enemies within m metres "
            "of that spot to the tutorial's start. A warp counts as a cheat: the session stays offline.");
    });
#endif

    add_tab("About", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("This program");
#if DES_DEV
        info_row(leftPane, rightPane, "Developer build", "Cheats compiled in",
            "This build has the developer cheats (the Developer tab). Never share it as a release.");
#endif
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
