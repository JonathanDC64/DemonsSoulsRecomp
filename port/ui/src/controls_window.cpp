// Adapted from FFCCEOT-Recomp's controls_window.cpp, itself after Dusklight's controller_config.cpp (CC0): the same
// layout (pages on the left, the bindings on the right, a binding is changed by pressing its row and then the button or
// key wanted). Adapted for Demon's Souls: the gamepad page is cellPad's PAD_MAP (the game's button = the pad button
// that presses it). The keyboard and mouse page is its PAD_KEYS, kept as changes to the Souls-style layout
// (settings.cpp, kKeysLayout): two rows an action, each a key, a mouse button, a wheel notch or one of those with
// Shift, Ctrl or Alt held (a chord). Both are kept in settings.ini. The pads are polled (input.hpp), so a button is
// caught from the polled state; keys and mouse buttons are caught from the key state (keys.hpp), and wheel notches from
// the UI's events.
#include "controls_window.hpp"

#include "input.hpp"
#include "keys.hpp"
#include "modal.hpp"
#include "pane.hpp"
#include "settings.hpp"
#include "platform.hpp"
#include "portable.hpp"
#include "ui.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ui {
namespace {

constexpr unsigned long long kCaptureTimeoutMs = 8000;
constexpr int kCount = 16;

// cellPad's button names and CELL_PAD_CTRL_* bit order (s_pad_names in cellPad.c), and the labels shown.
constexpr const char* kNames[kCount] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
constexpr const char* kLabels[kCount] = {"Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left",
                                         "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square"};
// The order of the gamepad page's rows: face buttons, shoulders, sticks, Start/Select, D-pad.
constexpr int kRows[kCount] = {14, 13, 15, 12, 10, 11, 8, 9, 1, 2, 3, 0, 4, 6, 7, 5};

using Map = std::array<int, kCount>;

int name_index(const std::string& s) {
    for (int i = 0; i < kCount; i++) {
        if (same_name(s.c_str(), kNames[i])) return i;
    }
    return -1;
}

// "game=value" pairs separated by commas (or spaces or semicolons), as cellPad reads them.
template <typename F>
void each_pair(const std::string& list, F&& f) {
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find_first_of(", ;", at);
        if (end == std::string::npos) end = list.size();
        const std::string pair = list.substr(at, end - at);
        if (const size_t eq = pair.find('='); eq != std::string::npos) f(pair.substr(0, eq), pair.substr(eq + 1));
        at = end + 1;
    }
}

// The pad button each game button reads (an index into kNames).
Map pad_map() {
    Map m;
    for (int i = 0; i < kCount; i++) m[i] = i;
    each_pair(settings::get("PAD_MAP"), [&](const std::string& game, const std::string& v) {
        const int dst = name_index(game), src = name_index(v);
        if (dst >= 0 && src >= 0) m[dst] = src;
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

// Gives game button `button` the pad button `value`. A game button that had it takes the old one instead, so no pad
// button presses two game buttons.
void assign(Map& m, int button, int value) {
    for (int i = 0; i < kCount; i++) {
        if (i != button && m[i] == value) m[i] = m[button];
    }
    m[button] = value;
}

// ---- The keyboard and mouse page ----

// An input as cellPad codes it (libs/input/cellPad.c): a Windows virtual-key code, or a mouse button or wheel notch.
constexpr int kInMouse = 0x100;  // mouse1 (left) to mouse5: 0x100 to 0x104
constexpr int kInWheelUp = 0x105, kInWheelDown = 0x106;

struct Binding {
    int in = 0;
    int mod = 0;        // a chord's other input (0 = none): Shift, Ctrl or Alt from this page
    bool menu = false;  // "menu:", only while the game says a menu is open: not in the layout, kept as written
    bool operator==(const Binding&) const = default;
};
using Bindings = std::vector<Binding>;

// The actions: cellPad's PAD_KEYS targets, in the page's order.
struct Target {
    const char* name;
    const char* label;
    const char* section;  // the heading this row starts, or nullptr
};
constexpr Target kTargets[] = {
    {"ls_up", "Move forward", "Movement"},
    {"ls_down", "Move back", nullptr},
    {"ls_left", "Move left", nullptr},
    {"ls_right", "Move right", nullptr},
    {"walk", "Walk (held)", nullptr},
    {"circle", "Roll, backstep, dash (Circle)", "Actions"},
    {"l3", "Left stick press (L3)", nullptr},
    {"r1", "Attack (R1)", nullptr},
    {"r2", "Strong attack (R2)", nullptr},
    {"l1", "Left hand, guard (L1)", nullptr},
    {"l2", "Left hand, parry (L2)", nullptr},
    {"r3", "Lock on (R3)", nullptr},
    {"cross", "Action (Cross)", nullptr},
    {"square", "Use item (Square)", nullptr},
    {"triangle", "Two-hand (Triangle)", nullptr},
    {"up", "Switch spell (Up)", "Equipment"},
    {"down", "Switch item (Down)", nullptr},
    {"left", "Switch left hand (Left)", nullptr},
    {"right", "Switch right hand (Right)", nullptr},
    {"start", "Menu (Start)", "Menus"},
    {"select", "Select", nullptr},
    {"rs_up", "Camera up", "Camera keys"},
    {"rs_down", "Camera down", nullptr},
    {"rs_left", "Camera left", nullptr},
    {"rs_right", "Camera right", nullptr},
};
constexpr int kTargetCount = int(sizeof kTargets / sizeof kTargets[0]);
constexpr int kSlots = 2;      // rows an action (cellPad takes up to 4: more from a hand-written PAD_KEYS are kept)
constexpr int kBindMax = 4;    // cellPad's PAD_BIND_MAX
using KeyMap = std::array<Bindings, kTargetCount>;

// cellPad's input names (s_key_names in cellPad.c, and f1 to f24). Letters and digits stand for themselves, and any
// other key is written as its virtual-key code ("0xBA").
struct InName {
    const char* name;
    int code;
};
constexpr InName kInNames[] = {
    {"space", VK_SPACE},     {"enter", VK_RETURN},     {"tab", VK_TAB},          {"esc", VK_ESCAPE},
    {"backspace", VK_BACK},  {"shift", VK_SHIFT},      {"lshift", VK_LSHIFT},    {"rshift", VK_RSHIFT},
    {"ctrl", VK_CONTROL},    {"lctrl", VK_LCONTROL},   {"rctrl", VK_RCONTROL},   {"alt", VK_MENU},
    {"lalt", VK_LMENU},      {"ralt", VK_RMENU},       {"up", VK_UP},            {"down", VK_DOWN},
    {"left", VK_LEFT},       {"right", VK_RIGHT},      {"insert", VK_INSERT},    {"delete", VK_DELETE},
    {"home", VK_HOME},       {"end", VK_END},          {"pageup", VK_PRIOR},     {"pagedown", VK_NEXT},
    {"capslock", VK_CAPITAL}, {"mouse1", kInMouse},    {"mouse2", kInMouse + 1}, {"mouse3", kInMouse + 2},
    {"mouse4", kInMouse + 3}, {"mouse5", kInMouse + 4}, {"wheelup", kInWheelUp}, {"wheeldown", kInWheelDown},
    {"return", VK_RETURN},   {"escape", VK_ESCAPE},  // (other spellings cellPad reads; the first name is written)
};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One input's code, as cellPad's pad_parse_input reads it (0 = not an input).
int parse_input(const std::string& text) {
    const std::string s = lower(text);
    const auto digit = [&](size_t i) { return i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); };
    if (s.size() == 1 && std::isalnum(static_cast<unsigned char>(s[0]))) {
        return std::toupper(static_cast<unsigned char>(s[0]));
    }
    for (const InName& n : kInNames) {
        if (s == n.name) return n.code;
    }
    if ((s.size() == 2 || s.size() == 3) && s[0] == 'f' && digit(1) && s[1] != '0' && (s.size() == 2 || digit(2))) {
        const int f = std::atoi(s.c_str() + 1);
        return f >= 1 && f <= 24 ? VK_F1 + f - 1 : 0;
    }
    if (digit(0) && s.size() < 16) return int(std::strtol(s.c_str(), nullptr, 0)) & 0xFF;
    return 0;
}

std::string input_name(int code) {
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) {
        return std::string(1, char(std::tolower(code)));
    }
    for (const InName& n : kInNames) {
        if (n.code == code) return n.name;
    }
    if (code >= VK_F1 && code <= VK_F24) return "f" + std::to_string(code - VK_F1 + 1);
    char text[8];
    std::snprintf(text, sizeof text, "0x%02X", code & 0xFF);
    return text;
}

// A target's value: bindings separated by '|', each "[menu:][modifier+]input"; "none" or nothing is no binding. The
// ones cellPad would skip are left out, as it leaves them out.
Bindings parse_bindings(const std::string& value) {
    Bindings out;
    size_t at = 0;
    while (at <= value.size()) {
        size_t end = value.find('|', at);
        if (end == std::string::npos) end = value.size();
        std::string item = value.substr(at, end - at);
        at = end + 1;
        Binding b;
        if (item.size() > 5 && lower(item.substr(0, 5)) == "menu:") {
            b.menu = true;
            item = item.substr(5);
        }
        const size_t plus = item.find('+');
        if (plus == std::string::npos) {
            b.in = parse_input(item);
        } else {
            b.mod = parse_input(item.substr(0, plus));
            b.in = parse_input(item.substr(plus + 1));
        }
        const bool ok = b.in != 0 && (plus == std::string::npos || (b.mod != 0 && b.mod < kInWheelUp));
        if (ok && int(out.size()) < kBindMax) out.push_back(b);
    }
    return out;
}

std::string bindings_text(const Bindings& bindings) {
    if (bindings.empty()) return "none";
    std::string out;
    for (const Binding& b : bindings) {
        if (!out.empty()) out += '|';
        if (b.menu) out += "menu:";
        if (b.mod) out += input_name(b.mod) + "+";
        out += input_name(b.in);
    }
    return out;
}

int target_index(const std::string& name) {
    for (int t = 0; t < kTargetCount; t++) {
        if (same_name(name.c_str(), kTargets[t].name)) return t;
    }
    return -1;
}

// Applies "target=bindings" pairs in order: a target named again replaces what it had, as in cellPad.
void apply_pairs(KeyMap& m, const std::string& list) {
    each_pair(list, [&](const std::string& target, const std::string& value) {
        if (const int t = target_index(target); t >= 0) m[t] = parse_bindings(value);
    });
}

const KeyMap& layout_map() {
    static const KeyMap m = [] {
        KeyMap k;
        apply_pairs(k, settings::kKeysLayout);
        return k;
    }();
    return m;
}

// The bindings the PAD_KEYS setting makes: the layout, then its changes. (Each row asks once a frame: parsed again
// only when the setting changes.)
const KeyMap& key_map() {
    static std::string sValue("\x01");
    static KeyMap sMap;
    const std::string value = settings::get("PAD_KEYS");
    if (value != sValue) {
        sValue = value;
        sMap = layout_map();
        apply_pairs(sMap, value);
    }
    return sMap;
}

// Keeps the actions that differ from the layout as the PAD_KEYS setting ("" when none do).
void save_key_map(const KeyMap& m) {
    const KeyMap& base = layout_map();
    std::string out;
    for (int t = 0; t < kTargetCount; t++) {
        if (m[t] == base[t]) continue;
        if (!out.empty()) out += ',';
        out += std::string(kTargets[t].name) + "=" + bindings_text(m[t]);
    }
    settings::set("PAD_KEYS", out);
}

// Gives action t's row `slot` the input `in` (with modifier `mod`), or clears the row (in = 0). The same input leaves
// any other row first, so one input never works two actions.
void assign_key(KeyMap& m, int t, int slot, int in, int mod) {
    Bindings& own = m[t];
    if (in == 0) {
        if (slot < int(own.size())) own.erase(own.begin() + slot);
        return;
    }
    for (int u = 0; u < kTargetCount; u++) {
        Bindings& bs = m[u];
        for (int j = 0; j < int(bs.size());) {
            if (bs[j].in == in && bs[j].mod == mod && !(u == t && j == slot)) {
                bs.erase(bs.begin() + j);
                if (u == t && j < slot) slot--;
            } else {
                j++;
            }
        }
    }
    const Binding b{in, mod, false};
    if (slot < int(own.size())) {
        own[slot] = b;
    } else if (int(own.size()) < kBindMax) {
        own.push_back(b);
    }
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
    case VK_LSHIFT: return "Left Shift";
    case VK_RSHIFT: return "Right Shift";
    case VK_LCONTROL: return "Left Ctrl";
    case VK_RCONTROL: return "Right Ctrl";
    case VK_LMENU: return "Left Alt";
    case VK_RMENU: return "Right Alt";
    case kInMouse: return "Left button";
    case kInMouse + 1: return "Right button";
    case kInMouse + 2: return "Middle button";
    case kInMouse + 3: return "Mouse button 4";
    case kInMouse + 4: return "Mouse button 5";
    case kInWheelUp: return "Wheel up";
    case kInWheelDown: return "Wheel down";
    default: break;
    }
    if (Rml::String name = keys::key_name(vk); !name.empty()) return name;
    char code[16];
    std::snprintf(code, sizeof code, "Key 0x%02X", vk);
    return code;
}

Rml::String binding_label(const Binding& b) {
    Rml::String out = b.menu ? "In menus: " : "";
    if (b.mod) out += key_label(b.mod) + " + ";
    return out + key_label(b.in);
}

// A modifier's either-side code, for a chord.
int either_side(int vk) {
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return vk;
    }
}

// Nothing is held: no key, mouse button or pad button (the press that started the capture would otherwise be taken
// as the choice).
bool input_neutral() {
    if (input::pad_buttons() != 0) return false;
    if (settings::no_user_input()) return true;  // PS3_NO_USER_INPUT: a key held is the user's
    for (int vk = 1; vk < 0xFF; vk++) {
        if (keys::key_down(vk)) return false;
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
    leftPane.register_control(leftPane.add_group_button({.text = "Keyboard and Mouse"}), rightPane,
                              [this](Pane& pane) {
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
                .bodyText = "Every gamepad button goes back to the game's own layout, and the keys and mouse buttons "
                            "to the Souls layout this port starts with.",
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
            pane.add_text("Puts all buttons and keys back to the defaults.");
        });
    render_page(rightPane, mPage);
}

void ControlsWindow::render_page(Pane& pane, Page page) {
    pane.clear();
    if (page == Page::Gamepad) {
        render_pad_page(pane);
    } else {
        render_key_page(pane);
    }
}

void ControlsWindow::render_pad_page(Pane& pane) {
    if (settings::pinned("PAD_MAP")) {
        pane.add_rml("<status-badge class=\"warning\">Set to " + escape(settings::pinned_value("PAD_MAP")) +
                     " when the game started (PAD_MAP), so these can't change it.</status-badge><br/>");
    }
    for (const int i : kRows) {
        pane.add_select_button({
                                   .key = kLabels[i],
                                   .getValue =
                                       [this, i]() -> Rml::String {
                                           if (mPendingButton == i && mPendingPad) return pending_label();
                                           return kLabels[pad_map()[i]];
                                       },
                                   .isModified = [i] { return pad_map()[i] != i; },
                               })
            .on_pressed([this, i] {
                play_sound(kSoundClick);
                start_capture(i, true, 0);
            });
    }
    pane.add_rml("<br/>Choose a row, then press the button for it. Esc cancels.<br/><br/>(PAD_MAP)");
}

void ControlsWindow::render_key_page(Pane& pane) {
    if (settings::pinned("PAD_KEYS")) {
        pane.add_rml("<status-badge class=\"warning\">Set to " + escape(settings::pinned_value("PAD_KEYS")) +
                     " when the game started (PAD_KEYS), so these can't change it.</status-badge><br/>");
    }
    for (int t = 0; t < kTargetCount; t++) {
        if (kTargets[t].section) pane.add_section(kTargets[t].section);
        for (int slot = 0; slot < kSlots; slot++) {
            pane.add_select_button({
                                       .key = slot == 0 ? Rml::String(kTargets[t].label)
                                                        : Rml::String(kTargets[t].label) + ", 2nd",
                                       .getValue =
                                           [this, t, slot]() -> Rml::String {
                                               if (!mPendingPad && mPendingButton == t && mPendingSlot == slot) {
                                                   return pending_label();
                                               }
                                               const Bindings& bs = key_map()[t];
                                               return slot < int(bs.size()) ? binding_label(bs[slot]) : "None";
                                           },
                                       .isModified =
                                           [t, slot] {
                                               const Bindings& now = key_map()[t];
                                               const Bindings& base = layout_map()[t];
                                               const bool a = slot < int(now.size()), b = slot < int(base.size());
                                               return a != b || (a && !(now[slot] == base[slot]));
                                           },
                                   })
                .on_pressed([this, t, slot] {
                    play_sound(kSoundClick);
                    start_capture(t, false, slot);
                });
        }
    }
    pane.add_rml("<br/>Choose a row, then press a key, mouse button or wheel. Hold Shift, Ctrl or Alt for a "
                 "combination. Delete clears the row; Esc cancels.<br/><br/>(PAD_KEYS)");
}

Rml::String ControlsWindow::pending_label() const {
    if (!mArmed) return "...";
    return mPendingPad ? "Press a button" : "Press a key or button";
}

void ControlsWindow::start_capture(int button, bool pad, int slot) {
    mPendingButton = button;
    mPendingPad = pad;
    mPendingSlot = slot;
    mArmed = false;
    mModDown = 0;
    mPendingStartedAt = now_ms();
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
        if (mPendingPad) {
            update_pad_capture();
        } else {
            update_key_capture();
        }
        if (capture_active() && now_ms() - mPendingStartedAt > kCaptureTimeoutMs) cancel_capture();
    }
    Window::update();
}

void ControlsWindow::update_pad_capture() {
    const int vk = input::take_captured_key();
    if (vk == VK_ESCAPE) {
        play_sound(kSoundWindowClose);
        cancel_capture();
    } else if (!mArmed) {
        mArmed = input_neutral();
    } else if (const unsigned b = input::pad_buttons(); b != 0) {
        int host = 0;
        while (!(b & (1u << host))) host++;
        Map m = pad_map();
        assign(m, mPendingButton, host);
        save_pad_map(m);
        play_sound(kSoundBindingChanged);
        cancel_capture();
    }
}

// Keys and mouse buttons are read from the key state (Alt and F10 never reach the UI as key presses), wheel notches
// from the UI's events. A Shift, Ctrl or Alt held when another input goes down makes a chord with it; let go with
// nothing else pressed, it is the choice itself.
void ControlsWindow::update_key_capture() {
    (void)input::take_captured_key();  // (the UI's key events only keep the keys from the menus here)
    int wheel = input::take_captured_wheel();
    if (settings::no_user_input()) wheel = 0;  // (Windows sends the wheel to the window under the cursor, focused or not)
    if (!mArmed) {
        if (input_neutral()) {
            mArmed = true;
            mDown.fill(false);
            mModDown = 0;
        }
        return;
    }
    std::array<bool, 256> now{};
    if (!settings::no_user_input()) {  // PS3_NO_USER_INPUT: a key held is the user's, so a capture sees none
        for (int vk = 1; vk < 0xFF; vk++) now[vk] = keys::key_down(vk);
    }
    // The modifier first, so one pressed in the same frame as the other input still makes the chord.
    for (const int vk : {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU}) {
        if (!mModDown && now[vk] && !mDown[vk]) mModDown = vk;
    }
    const int mod = mModDown && now[mModDown] ? either_side(mModDown) : 0;
    for (int vk = 1; vk < 0xFF; vk++) {
        const bool pressed = now[vk] && !mDown[vk];
        mDown[vk] = now[vk];
        if (pressed && key_pressed(vk, mod)) return;
    }
    if (wheel != 0 && key_pressed(wheel > 0 ? kInWheelUp : kInWheelDown, mod)) return;
    if (mModDown && !now[mModDown]) bind_key(mModDown, 0);
}

// A new press during a key capture, with modifier `mod` held (0 = none): true when it ends the capture.
bool ControlsWindow::key_pressed(int code, int mod) {
    switch (code) {
    case VK_ESCAPE:
        play_sound(kSoundWindowClose);
        cancel_capture();
        return true;
    case VK_DELETE:
    case VK_BACK:
        if (mod) break;
        bind_key(0, 0);
        return true;
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:  // (each also comes as its left or right key)
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_CANCEL: case VK_LWIN: case VK_RWIN:
    case VK_F1: case VK_F9: case VK_F10: case VK_F11: case VK_F12:  // this menu's key and the backend's capture keys
        return false;
    default:
        break;
    }
    int in = code;
    switch (code) {
    case VK_LBUTTON: in = kInMouse; break;
    case VK_RBUTTON: in = kInMouse + 1; break;
    case VK_MBUTTON: in = kInMouse + 2; break;
    case VK_XBUTTON1: in = kInMouse + 3; break;
    case VK_XBUTTON2: in = kInMouse + 4; break;
    default: break;
    }
    bind_key(in, mod);
    return true;
}

void ControlsWindow::bind_key(int in, int mod) {
    KeyMap m = key_map();
    assign_key(m, mPendingButton, mPendingSlot, in, mod);
    save_key_map(m);
    play_sound(kSoundBindingChanged);
    cancel_capture();
}

void ControlsWindow::hide(bool close) {
    cancel_capture();
    settings::save();
    Window::hide(close);
}

}  // namespace ui
