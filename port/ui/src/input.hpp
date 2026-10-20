// Adapted from Dusklight (CC0), src/dusk/ui/input.hpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: the
// gamepad comes from the port's own pad readers (XInput and the Sony HID reader) as a polled state, not as events.
#pragma once

namespace ui::input {

// The gamepads, merged: CELL_PAD_CTRL_* buttons and the left stick (-1..1, y down).
struct PadState {
    unsigned buttons = 0;
    float lx = 0.0f, ly = 0.0f;
};

// Once a frame while the UI is up: turns presses into the UI's keys (D-pad/left stick: arrows, Cross: Enter,
// Circle: Escape, R1/R2 and L1/L2: next and previous tab, Start or the menu chord: F1), with key repeat.
void update_pad(const PadState& pad, int chord) noexcept;
void update_input() noexcept;
void reset_input_state() noexcept;
void sync_input_block() noexcept;
void release_input_block() noexcept;

// The chord that opens the menu over the game: 0 = L3 + R3, 1 = Select + Start (CELL_PAD_CTRL_* masks).
unsigned chord_mask(int chord) noexcept;
const char* chord_label(int chord) noexcept;

// Whether the UI is taking the input (a document is showing): the game then gets none.
bool input_blocked() noexcept;

// A button or key is being chosen for a binding (controls_window.cpp): the pads, keys and mouse buttons then work
// nothing in the menus. The pads' buttons as last read (CELL_PAD_CTRL_*, the pads' own: PAD_MAP does not apply), the
// last key pressed during a capture (a virtual-key code, 0 = none) and the wheel's notches since the last read
// (up positive); reading either clears it.
void set_binding_capture(bool on) noexcept;
bool binding_capture() noexcept;
unsigned pad_buttons() noexcept;
void capture_key(int vk) noexcept;
int take_captured_key() noexcept;
void capture_wheel(int notches) noexcept;
int take_captured_wheel() noexcept;

}  // namespace ui::input
