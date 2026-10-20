// Written for Demon's Souls. The mouse's side of the window (docs/41, sections 1.3 and 3.2): raw mouse input on the
// window's thread. While the game has the mouse, its buttons and wheel go to cellPad for the keyboard layout, and with
// mouse look on the cursor is hidden and held in the window. The camera's side is des_mouse_look in des_options.cpp,
// which takes the counts with des_mouse_take (des_ui.h).
#pragma once

namespace ui::mouse {

// Called first in the overlay's window procedure, on the window's thread, for every message the overlay sees. True
// when the message is consumed (*result is then the window procedure's return value): only WM_SETCURSOR while the
// cursor is held, and this module's own posted message.
bool wndproc(void* hwnd, unsigned msg, unsigned long long wp, long long lp, long long* result);

// A document of the UI opened or closed (the render thread, from frame; or at startup). While one is open the cursor
// is the UI's.
void set_ui_open(bool open);

// Once a presented frame (the render thread, first thing in frame): notices whether the game's camera is taking the
// counts, so the mouse is the game's only while the camera runs.
void on_frame();

// The game crashed: the cursor is let go at once (the crash handler's thread).
void release_now();

}  // namespace ui::mouse
