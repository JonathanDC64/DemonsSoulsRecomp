// Adapted from Dusklight (CC0), src/dusk/ui/input.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: the pads
// are read as a polled state (see input.hpp). The menu keys follow Dusklight's: Cross confirms, Circle cancels, the
// shoulders change tabs, and Start or the menu chord (L3 + R3 by default) is the menu button.
#include "input.hpp"

#include "host.hpp"
#include "ui.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace ui::input {
namespace {

// CELL_PAD_CTRL_* (cellPad.h)
constexpr unsigned kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3;
constexpr unsigned kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7;
constexpr unsigned kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11;
constexpr unsigned kCircle = 1u << 13, kCross = 1u << 14;

constexpr double kRepeatInitialDelay = 0.32;
constexpr double kRepeatStartInterval = 0.12;
constexpr double kRepeatMinInterval = 0.045;
constexpr double kRepeatRampDuration = 1.0;
constexpr float kStickPress = 0.5f, kStickRelease = 0.37f;

// The UI's keys a pad can press.
enum Key { kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyConfirm, kKeyCancel, kKeyNext, kKeyPrev, kKeyMenu, kKeyCount };

constexpr Rml::Input::KeyIdentifier kKeyIds[kKeyCount] = {
    Rml::Input::KI_UP,     Rml::Input::KI_DOWN,   Rml::Input::KI_LEFT, Rml::Input::KI_RIGHT, Rml::Input::KI_RETURN,
    Rml::Input::KI_ESCAPE, Rml::Input::KI_NEXT,   Rml::Input::KI_PRIOR, Rml::Input::KI_F1,
};

struct Repeat {
    bool held = false;
    double pressedAt = 0.0;
    double nextRepeatAt = 0.0;
};

bool sInputBlocked = false;
std::array<Repeat, kKeyCount> sRepeats;
bool sStickX[2] = {}, sStickY[2] = {};  // left/right, up/down held (with hysteresis)
bool sPrimed = false;  // the first state after a reset only records what is held (no presses)
bool sBindingCapture = false;
unsigned sPadButtons = 0;
int sCapturedKey = 0;
int sCapturedWheel = 0;

double now_seconds() noexcept {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool repeatable(int key) noexcept {
    return key == kKeyUp || key == kKeyDown || key == kKeyLeft || key == kKeyRight || key == kKeyNext || key == kKeyPrev;
}

double repeat_interval(double heldFor) noexcept {
    const double ramp = std::clamp(heldFor / kRepeatRampDuration, 0.0, 1.0);
    return kRepeatStartInterval + (kRepeatMinInterval - kRepeatStartInterval) * ramp;
}

void stick_axis(float v, bool (&held)[2]) noexcept {
    held[0] = held[0] ? v < -kStickRelease : v < -kStickPress;
    held[1] = held[1] ? v > kStickRelease : v > kStickPress;
}

}  // namespace

unsigned chord_mask(int chord) noexcept {
    return chord == 1 ? (kSelect | kStart) : (kL3 | kR3);
}

const char* chord_label(int chord) noexcept {
    return chord == 1 ? "Select + Start" : "L3 + R3";
}

bool input_blocked() noexcept {
    return sInputBlocked;
}

void sync_input_block() noexcept {
    sInputBlocked = any_document_visible();
}

void release_input_block() noexcept {
    sInputBlocked = false;
}

void reset_input_state() noexcept {
    auto* context = host::context();
    for (int k = 0; k < kKeyCount; k++) {
        if (sRepeats[k].held && context) context->ProcessKeyUp(kKeyIds[k], 0);
        sRepeats[k] = {};
    }
    sStickX[0] = sStickX[1] = sStickY[0] = sStickY[1] = false;
    sPrimed = false;
}

void set_binding_capture(bool on) noexcept {
    sBindingCapture = on;
    sCapturedKey = 0;
    sCapturedWheel = 0;
    reset_input_state();  // what is held when a capture starts or ends is not a press
}

bool binding_capture() noexcept {
    return sBindingCapture;
}

unsigned pad_buttons() noexcept {
    return sPadButtons;
}

void capture_key(int vk) noexcept {
    sCapturedKey = vk;
}

int take_captured_key() noexcept {
    const int vk = sCapturedKey;
    sCapturedKey = 0;
    return vk;
}

void capture_wheel(int notches) noexcept {
    sCapturedWheel += notches;
}

int take_captured_wheel() noexcept {
    const int n = sCapturedWheel;
    sCapturedWheel = 0;
    return n;
}

void update_pad(const PadState& pad, int chord) noexcept {
    auto* context = host::context();
    if (context == nullptr) return;
    sPadButtons = pad.buttons;
    if (sBindingCapture) {
        sync_input_block();
        return;
    }
    stick_axis(pad.lx, sStickX);
    stick_axis(pad.ly, sStickY);
    const unsigned b = pad.buttons;
    const unsigned mask = chord_mask(chord);
    bool down[kKeyCount] = {};
    down[kKeyUp] = (b & kUp) || sStickY[0];
    down[kKeyDown] = (b & kDown) || sStickY[1];
    down[kKeyLeft] = (b & kLeft) || sStickX[0];
    down[kKeyRight] = (b & kRight) || sStickX[1];
    down[kKeyConfirm] = (b & kCross) != 0;
    down[kKeyCancel] = (b & kCircle) != 0;
    down[kKeyNext] = (b & (kR1 | kR2)) != 0;
    down[kKeyPrev] = (b & (kL1 | kL2)) != 0;
    down[kKeyMenu] = (b & kStart) || (b & mask) == mask;
    const double now = now_seconds();
    for (int k = 0; k < kKeyCount; k++) {
        Repeat& r = sRepeats[k];
        if (down[k] && !r.held) {
            r.held = true;
            r.pressedAt = now;
            r.nextRepeatAt = now + kRepeatInitialDelay;
            if (sPrimed) {
                context->ProcessMouseLeave();
                context->ProcessKeyDown(kKeyIds[k], 0);
            }
        } else if (!down[k] && r.held) {
            r = {};
            if (sPrimed) context->ProcessKeyUp(kKeyIds[k], 0);
        }
    }
    sPrimed = true;
    sync_input_block();
}

void update_input() noexcept {
    auto* context = host::context();
    if (context == nullptr) return;
    const double now = now_seconds();
    for (int k = 0; k < kKeyCount; k++) {
        Repeat& r = sRepeats[k];
        if (!r.held || !repeatable(k) || now < r.nextRepeatAt) continue;
        context->ProcessKeyDown(kKeyIds[k], 0);
        r.nextRepeatAt = now + repeat_interval(now - r.pressedAt);
    }
}

}  // namespace ui::input
