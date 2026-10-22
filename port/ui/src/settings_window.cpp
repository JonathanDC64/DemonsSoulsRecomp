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
    report::open_url(url);
}

// What heads a row's help: the option is set from outside, or it takes effect at the next start.
Rml::String with_note(const Rml::String& key, Rml::String help) {
    const settings::Def* d = settings::find(key);
    if (settings::pinned(key)) {
        const Rml::String value = settings::pinned_value(key);
        return "<status-badge class=\"warning\">Set to " + escape(value.empty() ? Rml::String("(empty)") : value) +
               " when the game started (" + escape(key) + "), so this row can't change it.</status-badge><br/>" + help;
    }
    if (d && d->apply == settings::Apply::Restart) {
        return "<status-badge class=\"info\">Takes effect the next time you start the game.</status-badge><br/>" + help;
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

// The Online name row never stores an empty or invalid name: online play needs one, and a player who cleared it would
// stay offline without knowing why. It keeps the last good name (with none, a suggested one) and says so here.
std::string sNameNote;

void set_net_name(const std::string& typed) {
    const std::string name = trim_spaces(typed);
    if (settings::net_name_ok(name)) {
        sNameNote.clear();
        settings::set("DES_NET_NAME", name);
        return;
    }
    const std::string old = settings::get("DES_NET_NAME");
    const std::string kept = settings::net_name_ok(old) ? old : settings::suggest_net_name();
    sNameNote = std::string(name.empty() ? "Online play needs a name" : "Names are 3 to 16 letters, digits, - or _") +
                (kept == old ? ", so " + kept + " was kept." : ", so it is " + kept + " for now.");
    settings::set("DES_NET_NAME", kept);
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

        choice_row(leftPane, rightPane, "Display mode", "DES_FULLSCREEN", {"0", "1"}, {"Window", "Fullscreen"},
            "Fullscreen fills the display the window is on. Alt+Enter also switches between the two."
            "<br/><br/>(DES_FULLSCREEN)");
        choice_row(leftPane, rightPane, "Aspect ratio", "DES_ASPECT",
            {"", "2560:1080", "3440:1440", "3840:1600", "32:9", "auto"},
            {"16:9 (original)", "21:9 (2560x1080)", "21:9 (3440x1440)", "21:9 (3840x1600)", "32:9", "Match the display"},
            "The shape of the picture, for ultrawide displays. Wider shows more at the sides. Raise Render "
            "resolution to keep a wide picture sharp.<br/><br/>(DES_ASPECT)");
        choice_row(leftPane, rightPane, "HUD", "DES_HUD", {"", "stretch"}, {"Centred 16:9", "Stretched"},
            "Where the HUD goes on a wide picture. Centred keeps it 16:9 in the middle; Stretched spreads it "
            "across the screen. No effect at 16:9.<br/><br/>(DES_HUD)");
        choice_row(leftPane, rightPane, "Render resolution", "RSX_SCALE", {"100", "150", "200", "300"},
            {"100% (original)", "150%", "200%", "300%"},
            "Draws the game sharper: 150% is 1080p, 200% 1440p, 300% 4K. Higher costs more GPU time."
            "<br/><br/>(RSX_SCALE)");
        choice_row(leftPane, rightPane, "Scaling filter", "RSX_OUTFILTER", {"0", "1"}, {"Bilinear", "Sharp"},
            "How the picture is fitted to the window. Sharp is cleaner, most of all with a high Render resolution."
            "<br/><br/>(RSX_OUTFILTER)");
        choice_row(leftPane, rightPane, "Texture filtering", "RSX_TEXFILTER", {"0", "1", "2", "3"},
            {"Original", "Nearest", "Bilinear", "Trilinear"},
            "How textures are smoothed. Original uses the game's choice. Nearest looks pixelated; Trilinear is the "
            "smoothest.<br/><br/>(RSX_TEXFILTER)");
        choice_row(leftPane, rightPane, "Anisotropic filtering", "RSX_ANISO", {"0", "1", "2", "4", "8", "16"},
            {"Original (4x)", "Off", "2x", "4x", "8x", "16x"},
            "Keeps floors and walls sharp into the distance. Original is the game's own, 4x on most surfaces."
            "<br/><br/>(RSX_ANISO)");
        choice_row(leftPane, rightPane, "Anti-aliasing", "RSX_AA", {"0", "1"}, {"Off (original)", "FXAA"},
            "Smooths jagged edges. FXAA keeps the HUD sharp but softens textures a little.<br/><br/>(RSX_AA)");
        bool_row(leftPane, rightPane, "Depth of field", "DES_DOF",
            "The game's blur on distant scenery. Off keeps it sharp.<br/><br/>(DES_DOF)");
        choice_row(leftPane, rightPane, "Frame rate", "DES_FPS", kFpsValues,
            {"30 fps (original)", "30 fps, real frame time", "60 fps", "120 fps", "144 fps", "165 fps", "240 fps",
                "Unlocked"},
            "The game's frame rate. 30 fps (original) is the PS3's. \"Real frame time\" is also 30 but copes "
            "better when a frame runs late. Unlocked has no cap. Above 60 fps is less tested.<br/><br/>(DES_FPS)",
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
                 "Your own frame rate, 30 to 500. Setting it picks Custom in Frame rate.<br/><br/>(DES_FPS)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });
        choice_row(leftPane, rightPane, "Cutscene frame rate", "DES_CUTFPS", {"0", "1"},
            {"30 fps (original)", "Same as Frame rate"},
            "Cutscenes judder above 30 fps, so by default they play at 30. Same as Frame rate keeps your frame "
            "rate and smooths their camera.<br/><br/>(DES_CUTFPS)");
        bool_row(leftPane, rightPane, "V-sync", "DES_VSYNC",
            "Waits for the display's refresh. Off can tear, but lets you see more fps than the refresh rate."
            "<br/><br/>(DES_VSYNC)");
        bool_row(leftPane, rightPane, "Motion blur", "DES_MOTION_BLUR",
            "The game's blur when the camera moves.<br/><br/>(DES_MOTION_BLUR)");
    });

    add_tab("Game", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Start");
        choice_row(leftPane, rightPane, "Boot logos", "DES_SKIP_INTRO", {"0", "1", "2"},
            {"Show (original)", "Skip", "Last logo only"},
            "The company logos at start. Skip goes straight to the title screen.<br/><br/>(DES_SKIP_INTRO)");
        bool_row(leftPane, rightPane, "Skip opening movie", "DES_SKIP_INTRO_MOVIE",
            "Skips the movie after the logos. Other movies still play.<br/><br/>(DES_SKIP_INTRO_MOVIE)");
        bool_row(leftPane, rightPane, "Continue automatically", "DES_AUTO_CONTINUE",
            "After Play, goes straight into your last save.<br/><br/>(DES_AUTO_CONTINUE)");

        leftPane.add_section("Sound");
        bool_row(leftPane, rightPane, "Sound", "AUDIO_MUTE",
            "The game's sound and music.<br/><br/>(AUDIO_MUTE)",
            true);
    });

    add_tab("Controls", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        const Rml::String chord = input::chord_label(settings::get_int("pad_menu_chord"));
        leftPane.add_section("This menu");
        info_row(leftPane, rightPane, "Open the menu", "F1 / " + chord,
            "F1, or " + escape(chord) + " on a gamepad (change it in Interface). Esc is the game's own menu.");
        info_row(leftPane, rightPane, "In the menus", "Arrows, Enter, Esc",
            "Keyboard: arrows, Enter, Esc; Page Up and Page Down change tabs. The mouse works too.<br/><br/>"
            "Gamepad: D-pad, Cross, Circle; L1 and R1 change tabs.");

        leftPane.add_section("The game");
        leftPane.register_control(
            leftPane.add_group_button({.text = "Change Buttons"}).on_pressed([this] {
                play_sound(kSoundClick);
                push(std::make_unique<ControlsWindow>());
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Change which buttons and keys do what.");
            });
        const input::PadSupport pads = input::pad_support();
        info_row(leftPane, rightPane, "Gamepads", pads.value, pads.help);
        info_row(leftPane, rightPane, "Keyboard and mouse", "WASD, mouse, Space",
            "As in the Souls games on PC:<br/>"
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
            "The mouse buttons and wheel need Mouse look on. Change Buttons changes any of these.");
        leftPane.add_section("Mouse");
        bool_row(leftPane, rightPane, "Mouse look", "DES_MOUSE_LOOK",
            "Turn the camera with the mouse, and use the mouse buttons and wheel in the game. Smoothest at 60 fps "
            "or more.<br/><br/>(DES_MOUSE_LOOK)");
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
                 "How far the camera turns when you move the mouse.<br/><br/>(DES_MOUSE_SENS)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });
        bool_row(leftPane, rightPane, "Invert vertical", "DES_MOUSE_INVERT_Y",
            "On: moving the mouse forward looks down.<br/><br/>(DES_MOUSE_INVERT_Y)");
        bool_row(leftPane, rightPane, "Invert horizontal", "DES_MOUSE_INVERT_X",
            "On: moving the mouse right turns the camera left.<br/><br/>(DES_MOUSE_INVERT_X)");
        choice_row(leftPane, rightPane, "Switch targets", "DES_MOUSE_FLICK", {"0", "8", "15", "30"},
            {"Off", "Short move", "Medium move", "Long move"},
            "While locked on, moving the mouse sideways switches targets. This sets how far you have to move it."
            "<br/><br/>(DES_MOUSE_FLICK)");
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
                help = "<status-badge class=\"warning\">Off this session: a developer cheat is on.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_ONLINE",
                "Plays online on the server below: messages, bloodstains, ghosts, co-op and invasions. Off plays "
                "offline.<br/><br/>(DES_ONLINE)"));
        });

        std::vector<Rml::String> serverValues, serverLabels;
        Rml::String serverHelp = "The game server to connect to. ";
        for (const settings::NetServer& s : settings::kNetServers) {
            serverValues.push_back(s.key);
            serverLabels.push_back(s.name);
        }
        serverHelp += "<br/>";
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
                       Rml::String(v.empty() ? "No address yet" : "Not a valid address") +
                       ", so you stay offline.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_NET_CUSTOM",
                "A server not in the list: an IP address or a host name. Typing one picks Custom.<br/><br/>"
                "(DES_NET_CUSTOM)"));
        });

        auto& name = leftPane.add_child<StringButton>(StringButton::Props{
            .key = "Online name",
            .getValue = [] { return Rml::String(net_value("DES_NET_NAME")); },
            .setValue =
                [](Rml::String value) {
                    if (settings::pinned("DES_NET_NAME")) {
                        return;
                    }
                    set_net_name(value);
                },
            .isDisabled = [] { return settings::pinned("DES_NET_NAME"); },
            .isModified = [] { return !settings::is_default("DES_NET_NAME"); },
            .maxLength = 16,
        });
        leftPane.register_control(name, rightPane, [](Pane& pane) {
            pane.clear();
            Rml::String help;
            const std::string v = net_value("DES_NET_NAME");
            if (!sNameNote.empty() && !settings::pinned("DES_NET_NAME")) {
                help = "<status-badge class=\"info\">" + Rml::String(sNameNote) + "</status-badge><br/>";
            } else if (v.empty()) {
                help = "<status-badge class=\"info\">No name yet, so you stay offline.</status-badge><br/>";
            } else if (!settings::net_name_ok(v)) {
                help = "<status-badge class=\"warning\">Not valid, so you stay offline.</status-badge><br/>";
            }
            pane.add_rml(help + with_note("DES_NET_NAME",
                "Your name online: 3 to 16 letters, digits, - or _. Pick one nobody else uses. A new name makes you "
                "a new player to the server: your old messages stay, but their ratings no longer reach you."
                "<br/><br/>(DES_NET_NAME)"));
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
                 "Summon signs and invasions only between players with the same password. Messages, bloodstains "
                 "and ghosts stay shared. Empty plays with everyone.<br/><br/>(DES_NET_PASSWORD)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });

        choice_row(leftPane, rightPane, "World tendency", "DES_NET_TENDENCY", {"", "own"},
            {"With the server's", "Yours only"},
            "Online, the game mixes other players' world tendency into yours. Yours only changes it by what you "
            "do alone. Applies from the next load.<br/><br/>(DES_NET_TENDENCY)");

        choice_row(leftPane, rightPane, "Message ratings", "DES_NET_RATING_POLL", {"", "30"},
            {"Every 5 minutes", "Every 30 seconds"},
            "How often the game checks whether someone rated your messages. The PS3 checked every 5 minutes."
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
                 "The port for co-op and invasions. Change it only if your router needs another one.<br/><br/>"
                 "(DES_NET_P2P_PORT)")](Pane& pane) {
                pane.clear();
                pane.add_rml(helpText);
            });

        choice_row(leftPane, rightPane, "Your movement updates", "DES_NET_SYNC", {"", "15"},
            {"Original (every 6th frame)", "Smoother (at least 15 a second)"},
            "How often your position goes to other players. Smoother makes you look less laggy on their screens."
            "<br/><br/>(DES_NET_SYNC)");

        leftPane.add_section("In your world");
        bool_row(leftPane, rightPane, "Messages", "DES_NET_MESSAGES",
            "Messages from other players. Off hides all of them, yours too.<br/><br/>(DES_NET_MESSAGES)");
        bool_row(leftPane, rightPane, "Bloodstains", "DES_NET_BLOODSTAINS",
            "Other players' bloodstains. Your own bloodstain with your souls always shows.<br/><br/>"
            "(DES_NET_BLOODSTAINS)");
        bool_row(leftPane, rightPane, "Ghosts", "DES_NET_GHOSTS",
            "Other players' ghosts walking through your world.<br/><br/>(DES_NET_GHOSTS)");

        leftPane.add_section("This session");
        info_row(leftPane, rightPane, "Network", net_cheat_session() ? Rml::String("Off: cheats used") : settings::net_status(),
            net_cheat_session()
                ? Rml::String("A developer cheat is on, so this session is offline.")
                : "This session: " + escape(settings::net_status()) + ".");
        info_row(leftPane, rightPane, "About online play", "Read first",
            "Online play is new and less tested than offline.<br/><br/>"
            "You play with everyone on the same server. Players on other servers, on RPCS3 or on a PS3 are not in "
            "your world.<br/><br/>"
            "Co-op and invasions connect you directly to the other player, so they can see your IP address, as on a "
            "PS3.");
    });

    add_tab("Interface", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        choice_row(leftPane, rightPane, "UI scale", "ui_scale", {"0", "75", "100", "125", "150"},
            {"Auto", "75%", "100%", "125%", "150%"},
            "The size of these menus. Auto follows Windows' display scale.",
            [] { apply_scale(); });
        bool_row(leftPane, rightPane, "Start screen", "show_start_screen",
            "Shows Play, Settings and Quit when the game starts. Off goes straight to the game.");
        bool_row(leftPane, rightPane, "Menu hint", "menu_hint",
            "After Play, a note says how to open this menu.");
        bool_row(leftPane, rightPane, "Trophy pop-ups", "trophy_popups",
            "Shows a note when you earn a trophy. Off only hides the note.");
        choice_row(leftPane, rightPane, "Gamepad menu buttons", "pad_menu_chord", {"0", "1"},
            {"L3 + R3", "Select + Start"},
            "The gamepad buttons that open this menu, pressed together. Select + Start also opens the game's menu.");
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
                "Cheats are off because this session is online. Turn Online off and restart to use them.");
            return;
        }
        info_row(leftPane, rightPane, "Developer build", "Cheats compiled in",
            "Cheats for testing (" DES_DEV_MARKER "). Any cheat keeps the session offline. They reset each start.");
        bool_row(leftPane, rightPane, "God mode", "DES_GOD",
            "Your HP, MP and stamina never drop. Instant deaths still kill.<br/><br/>(DES_GOD)");
        choice_row(leftPane, rightPane, "Attack power", "DES_ATK", {"1", "2", "5", "10", "100", "0"},
            {"Normal", "2x", "5x", "10x", "100x", "No damage"},
            "Multiplies your damage.<br/><br/>(DES_ATK)");
        choice_row(leftPane, rightPane, "Movement speed", "DES_SPEED", {"1", "1.5", "2", "3", "0.5"},
            {"Normal", "1.5x", "2x", "3x", "Half"},
            "Multiplies how fast you move. High values can pass through thin walls.<br/><br/>(DES_SPEED)");
        info_row(leftPane, rightPane, "Warp (scripts)", "DES_WARP=x,y,z",
            "For scripts: DES_WARP=x,y,z moves you there, and DES_WARP_CLEAR=m moves enemies within m metres away. "
            "Counts as a cheat.");
    });
#endif

    add_tab("About", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("This program");
#if DES_DEV
        info_row(leftPane, rightPane, "Developer build", "Cheats compiled in",
            "This build has cheats (the Developer tab). Never share it as a release.");
#endif
        info_row(leftPane, rightPane, "Version", kVersion,
            "Demon's Souls PC port, a recompilation of the PS3 game.<br/><br/>Version " +
                escape(kVersion) + ".",
            [](Pane& pane) {
                pane.add_button("Open the project page").on_pressed([] {
                    play_sound(kSoundClick);
                    open_url("https://github.com/JonathanDC64/DemonsSoulsRecomp");
                });
            });
        info_row(leftPane, rightPane, "ps3recomp", kPs3recompVersion,
            "The recompiler this build uses.<br/><br/>" + escape(kPs3recompVersion) + ".");
        {
            const Rml::String sha = kDumpSha256;
            Rml::String grouped;  // (in fours of 16, so the help pane can wrap it)
            for (size_t i = 0; i < sha.size(); i += 16) grouped += (i ? " " : "") + sha.substr(i, 16);
            info_row(leftPane, rightPane, "Game dump", sha.empty() ? Rml::String("unknown") : sha.substr(0, 12),
                "The SHA-256 of the game file this build was made from (EBOOT.BIN, decrypted).<br/><br/>" +
                    (sha.empty() ? Rml::String("Not known: the build did not find work/game/EBOOT.ELF.") : grouped));
        }
        info_row(leftPane, rightPane, "Licence", "MIT",
            "The port's code is MIT. You need your own copy of Demon's Souls.<br/><br/>Demon's Souls is a trademark "
            "of Sony Interactive "
            "Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment, "
            "FromSoftware or Atlus.");
        info_row(leftPane, rightPane, "Settings file", settings::path().empty() ? "(not kept)" : "settings.ini",
            settings::path().empty()
                ? Rml::String("This run does not keep its settings (a test script started it).")
                : "Your settings are saved in<br/>" + escape(settings::path()) +
                      (report::data_dir().empty()
                           ? Rml::String()
                           : "<br/><br/>Logs, crash reports and issue reports are in the same folder. Your saves are "
                             "elsewhere."));

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
