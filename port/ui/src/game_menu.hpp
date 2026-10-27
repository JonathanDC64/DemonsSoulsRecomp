// Written for Demon's Souls (docs/114). The mouse in the game's own menus, the UI's side: the pointer over them, a soft
// glow round the part under it, and the clicks, right clicks and wheel notches, which the game's side
// (des_menu_mouse.cpp) turns into the pad's presses.
#pragma once

namespace ui::game_menu {

// Once a presented frame, first thing (the render thread): whether a menu of the game is up. True while the UI must
// run this frame for it (the pointer shows over the menu, or it has just come or gone).
bool poll();

// In the frame, before the UI is drawn (the render thread): a gamepad in use hides the pointer; while a document of
// the UI is up the pointer is the UI's. Tells the host where to draw the sword.
void frame(bool pads_neutral, bool ui_visible, int width, int height);

// The part of the game's menu under the pointer, in the back buffer's pixels: false if none (the overlay's glow).
bool glow(float box[4]);

// A menu of the game is up (any thread).
bool open();

// The window's thread, while no document of the UI is up. True while a menu of the game takes the mouse: the message
// is then the menu's. Positions in the back buffer's pixels; the wheel in WHEEL_DELTA units, positive away from the
// user, where the pointer is.
bool active();
void on_move(int x, int y);
void on_button(int button, bool down, int x, int y);  // 0 left, 1 right, 2 middle
void on_wheel(int delta);
void on_key();    // a key went down: the pointer hides
void on_leave();  // the pointer left the window

}  // namespace ui::game_menu
