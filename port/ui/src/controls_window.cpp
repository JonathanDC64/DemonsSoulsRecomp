// Adapted from FFCCEOT-Recomp's controls_window.cpp, itself after Dusklight's controller_config.cpp (CC0): the same
// layout (pages on the left, the bindings on the right, a binding is changed by pressing its row and then the button or
// key wanted). Adapted for Demon's Souls: the gamepad page is cellPad's PAD_MAP (the game's button = the pad button
// that presses it), the keyboard page its PAD_KEYS (the game's button = a key), both kept in settings.ini. The pads are
// polled (input.hpp), so a button is caught from the polled state and a key from the UI's key events.
#include "controls_window.hpp"

#include "input.hpp"
#include "modal.hpp"
#include "pane.hpp"
#include "settings.hpp"
#include "ui.hpp"

#include <windows.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ui {
namespace {

constexpr unsigned long long kCaptureTimeoutMs = 8000;
constexpr int kCount = 16;

// cellPad's button names and CELL_PAD_CTRL_* bit order (s_pad_names in cellPad.c), and the labels shown.
constexpr const char* kNames[kCount] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
constexpr const char* kLabels[kCount] = {"Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left",
                                         "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square"};
// The order of the rows: face buttons, shoulders, sticks, Start/Select, D-pad.
constexpr int kRows[kCount] = {14, 13, 15, 12, 10, 11, 8, 9, 1, 2, 3, 0, 4, 6, 7, 5};
// cellPad's keyboard pad without PAD_KEYS (s_pad_keys_default), in kNames order; 0 = no key.
constexpr int kDefaultKeys[kCount] = {VK_TAB, 0, 0, VK_RETURN, VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT,
                                      '1', '2', 'Q', 'W', 'S', 'X', 'Z', 'A'};

using Map = std::array<int, kCount>;

int name_index(const std::string& s) {
    for (int i = 0; i < kCount; i++) {
        if (_stricmp(s.c_str(), kNames[i]) == 0) return i;
    }
    return -1;
}

// "game=value" pairs separated by commas, as cellPad reads them.
template <typename F>
void each_pair(const std::string& list, F&& f) {
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find_first_of(", ;", at);
        if (end == std::string::npos) end = list.size();
        const std::string pair = list.substr(at, end - at);
        if (const size_t eq = pair.find('='); eq != std::string::npos) {
            const int dst = name_index(pair.substr(0, eq));
            if (dst >= 0) f(dst, pair.substr(eq + 1));
        }
        at = end + 1;
    }
}

// The pad button each game button reads (an index into kNames).
Map pad_map() {
    Map m;
    for (int i = 0; i < kCount; i++) m[i] = i;
    each_pair(settings::get("PAD_MAP"), [&](int dst, const std::string& v) {
        if (const int src = name_index(v); src >= 0) m[dst] = src;
    });
    return m;
}

// The key of each game button (a virtual-key code, 0 = none).
Map key_map() {
    Map m;
    for (int i = 0; i < kCount; i++) m[i] = kDefaultKeys[i];
    each_pair(settings::get("PAD_KEYS"), [&](int dst, const std::string& v) {
        if (v.size() == 1 && std::isalnum(static_cast<unsigned char>(v[0]))) {
            m[dst] = std::toupper(static_cast<unsigned char>(v[0]));
        } else {
            m[dst] = int(std::strtol(v.c_str(), nullptr, 0)) & 0xFF;
        }
    });
    return m;
}

void save_pad_map(const Map& m) {
    std::string out;
    for (int i = 0; i < kCount; i++) {
        if (m[i] == i) continue;
        if (!out.empty()) out += ',';
        out += std::string(kNames[i]) + "=" + kNames[m[i]];
    }
    settings::set("PAD_MAP", out);
}

void save_key_map(const Map& m) {
    std::string out;
    for (int i = 0; i < kCount; i++) {
        if (m[i] == kDefaultKeys[i]) continue;
        char value[8];
        std::snprintf(value, sizeof value, "0x%02X", m[i]);
        if (!out.empty()) out += ',';
        out += std::string(kNames[i]) + "=" + value;
    }
    settings::set("PAD_KEYS", out);
}

// Gives game button `button` the pad button or key `value`. A game button that had it takes the old one instead, so
// no pad button or key presses two game buttons.
void assign(Map& m, int button, int value) {
    for (int i = 0; i < kCount; i++) {
        if (i != button && m[i] == value && value != 0) m[i] = m[button];
    }
    m[button] = value;
}

Rml::String key_label(int vk) {
    switch (vk) {
    case 0: return "None";
    case VK_UP: return "Up arrow";
    case VK_DOWN: return "Down arrow";
    case VK_LEFT: return "Left arrow";
    case VK_RIGHT: return "Right arrow";
    case VK_RETURN: return "Enter";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    default: break;
    }
    LONG lp = LONG(MapVirtualKeyA(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE:
    case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
        lp |= 1 << 24;  // extended keys
        break;
    default: break;
    }
    char name[64];
    if (GetKeyNameTextA(lp, name, sizeof name) > 0) return name;
    char code[16];
    std::snprintf(code, sizeof code, "Key 0x%02X", vk);
    return code;
}

// Nothing is held: no key, mouse button or pad button (the press that started the capture would otherwise be taken
// as the choice).
bool input_neutral() {
    if (input::pad_buttons() != 0) return false;
    if (settings::no_user_input()) return true;  // PS3_NO_USER_INPUT: a key held is the user's
    for (int vk = 1; vk < 0xFF; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return false;
    }
    return true;
}

}  // namespace

ControlsWindow::ControlsWindow() {
    add_tab("Buttons", [this](Rml::Element* content) { build_tab(content); });
}

void ControlsWindow::build_tab(Rml::Element* content) {
    cancel_capture();
    auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

    leftPane.register_control(leftPane.add_group_button({.text = "Gamepad"}), rightPane, [this](Pane& pane) {
        mPage = Page::Gamepad;
        render_page(pane, Page::Gamepad);
    });
    leftPane.register_control(leftPane.add_group_button({.text = "Keyboard"}), rightPane, [this](Pane& pane) {
        mPage = Page::Keyboard;
        render_page(pane, Page::Keyboard);
    });

    leftPane.add_section("Options");
    leftPane.register_control(
        leftPane.add_button("Restore Default Buttons").on_pressed([this] {
            play_sound(kSoundClick);
            const auto dismiss = [](Modal& modal) { modal.pop(); };
            push(std::make_unique<Modal>(Modal::Props{
                .title = "Restore Default Buttons",
                .bodyText = "Every gamepad button and key goes back to the game's own layout.",
                .actions =
                    {
                        ModalAction{
                            .label = "Cancel",
                            .onPressed =
                                [dismiss](Modal& modal) {
                                    play_sound(kSoundWindowClose);
                                    dismiss(modal);
                                },
                        },
                        ModalAction{
                            .label = "Restore",
                            .onPressed =
                                [dismiss](Modal& modal) {
                                    play_sound(kSoundClick);
                                    settings::set("PAD_MAP", "");
                                    settings::set("PAD_KEYS", "");
                                    dismiss(modal);
                                },
                        },
                    },
                .onDismiss = dismiss,
                .icon = "question-mark",
            }));
        }),
        rightPane, [](Pane& pane) {
            pane.clear();
            pane.add_text("Puts every gamepad button and key back to the game's own layout.");
        });
    render_page(rightPane, mPage);
}

void ControlsWindow::render_page(Pane& pane, Page page) {
    pane.clear();
    const bool isPad = page == Page::Gamepad;
    const char* key = isPad ? "PAD_MAP" : "PAD_KEYS";
    if (settings::pinned(key)) {
        pane.add_rml("<status-badge class=\"warning\">Set to " + escape(settings::pinned_value(key)) +
                     " in the environment when the game started (" + key +
                     "): that value is used, and wins over these.</status-badge><br/>");
    }
    for (const int i : kRows) {
        pane.add_select_button({
                                   .key = kLabels[i],
                                   .getValue =
                                       [this, i, isPad]() -> Rml::String {
                                           if (mPendingButton == i && mPendingPad == isPad) return pending_label();
                                           return isPad ? Rml::String(kLabels[pad_map()[i]]) : key_label(key_map()[i]);
                                       },
                                   .isModified =
                                       [i, isPad] {
                                           return isPad ? pad_map()[i] != i : key_map()[i] != kDefaultKeys[i];
                                       },
                               })
            .on_pressed([this, i, isPad] {
                play_sound(kSoundClick);
                start_capture(i, isPad);
            });
    }
    pane.add_rml(isPad ? "<br/>Press a row, then the gamepad button that should press it. The button it had moves to "
                         "the row that had yours. Esc cancels.<br/><br/>(PAD_MAP)"
                       : "<br/>Press a row, then the key that should press it. Delete clears it; Esc cancels. Without a "
                         "gamepad the arrow keys also work the left stick.<br/><br/>(PAD_KEYS)");
}

Rml::String ControlsWindow::pending_label() const {
    if (!mArmed) return "...";
    return mPendingPad ? "Press a button" : "Press a key";
}

void ControlsWindow::start_capture(int button, bool pad) {
    mPendingButton = button;
    mPendingPad = pad;
    mArmed = false;
    mPendingStartedAt = GetTickCount64();
    input::set_binding_capture(true);
}

void ControlsWindow::cancel_capture() {
    if (mPendingButton < 0) return;
    mPendingButton = -1;
    mArmed = false;
    input::set_binding_capture(false);
}

void ControlsWindow::update() {
    if (capture_active()) {
        const int vk = input::take_captured_key();
        if (vk == VK_ESCAPE) {
            play_sound(kSoundWindowClose);
            cancel_capture();
        } else if (!mArmed) {
            mArmed = input_neutral();
        } else if (mPendingPad) {
            const unsigned b = input::pad_buttons();
            if (b != 0) {
                int host = 0;
                while (!(b & (1u << host))) host++;
                Map m = pad_map();
                assign(m, mPendingButton, host);
                save_pad_map(m);
                play_sound(kSoundBindingChanged);
                cancel_capture();
            }
        } else if (vk != 0) {
            Map m = key_map();
            if (vk == VK_DELETE || vk == VK_BACK) {
                m[mPendingButton] = 0;
            } else {
                assign(m, mPendingButton, vk);
            }
            save_key_map(m);
            play_sound(kSoundBindingChanged);
            cancel_capture();
        }
        if (capture_active() && GetTickCount64() - mPendingStartedAt > kCaptureTimeoutMs) cancel_capture();
    }
    Window::update();
}

void ControlsWindow::hide(bool close) {
    cancel_capture();
    settings::save();
    Window::hide(close);
}

}  // namespace ui
