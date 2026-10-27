// Written for Demon's Souls (docs/114). The mouse in the game's own menus, the UI's side (game_menu.hpp).
//
// The pointer shows while a menu of the game is up and the mouse moves over the window, as the sword when Settings,
// Interface, Sword cursor is on (host.cpp draws it), else the system's arrow. A key or a gamepad button hides it again,
// and so does the menu closing; while it is hidden the system's cursor is hidden too. Meanwhile mouse.cpp leaves the
// mouse free: no mouse look, and the buttons are not the keyboard layout's (left click is not R1 in a menu).
//
// Where the pointer is over the game's menu canvas: the backend shows the picture in the window's middle at its shape
// (hud_editor.hpp, picture_rect_of), and the picture shows the canvas the game gives (des_menu_mouse_view: 1280x720,
// wider on a wider picture). The overlay (overlay.cpp) draws the glow round the part the game's side finds there.
//
// Off with DES_MENU_MOUSE=0 (Settings, Controls): the game's side then reports no menu. In a test run
// (PS3_NO_USER_INPUT=1) the mouse over the window is the user's and does nothing here; DES_UI_MOUSE (des_ui.cpp) moves
// and clicks instead.
//
// Threads: the window's thread (active, on_move, on_button, on_wheel, on_key, on_leave) and the render thread (poll,
// frame, glow) share the atomics below.
#include "game_menu.hpp"

#include "host.hpp"
#include "hud_editor.hpp"
#include "mouse.hpp"
#include "settings.hpp"

#include <atomic>

extern "C" {  // des_menu_mouse.cpp
int des_menu_mouse_open(void);
void des_menu_mouse_view(float view[4]);
int des_menu_mouse_hit(float x, float y, float box[4]);
void des_menu_mouse_click(float x, float y);
void des_menu_mouse_release(void);
void des_menu_mouse_back(void);
void des_menu_mouse_drag(float x, float y);
void des_menu_mouse_wheel(int notches, float x, float y);
}

namespace {

constexpr int kWheelDelta = 120;  // WHEEL_DELTA: a notch

std::atomic<bool> sOpen{false};   // a menu of the game is up (poll)
std::atomic<bool> sShown{false};  // the pointer shows
std::atomic<bool> sLeft{false};   // the left button is down (a slider's thumb follows the pointer)
std::atomic<int> sX{0}, sY{0};    // where, in the back buffer's pixels
std::atomic<int> sW{0}, sH{0};    // the back buffer's size (frame)
int sMode = 0;                    // what frame told the host last (the render thread): host::GamePointer
int sWheel = 0;                   // wheel movement short of a notch (the window's thread)

// Back buffer pixels to the menu canvas, and back.
bool mapping(float pic[4], float view[4]) {
    const int w = sW.load(std::memory_order_relaxed), h = sH.load(std::memory_order_relaxed);
    if (w <= 0 || h <= 0) return false;
    ui::picture_rect_of(float(w), float(h), pic);
    des_menu_mouse_view(view);
    return pic[2] > pic[0] && pic[3] > pic[1] && view[2] > view[0] && view[3] > view[1];
}

bool to_canvas(int x, int y, float& cx, float& cy) {
    float pic[4], view[4];
    if (!mapping(pic, view)) return false;
    cx = view[0] + (float(x) - pic[0]) * (view[2] - view[0]) / (pic[2] - pic[0]);
    cy = view[1] + (float(y) - pic[1]) * (view[3] - view[1]) / (pic[3] - pic[1]);
    return true;
}

bool to_pixels(float cx, float cy, float& x, float& y) {
    float pic[4], view[4];
    if (!mapping(pic, view)) return false;
    x = pic[0] + (cx - view[0]) * (pic[2] - pic[0]) / (view[2] - view[0]);
    y = pic[1] + (cy - view[1]) * (pic[3] - pic[1]) / (view[3] - view[1]);
    return true;
}

}  // namespace

namespace ui::game_menu {

bool poll() {
    const bool open = des_menu_mouse_open() != 0;
    if (sOpen.exchange(open) != open) {
        ui::mouse::set_game_menu(open);
        if (!open && !ui::settings::no_user_input()) sShown.store(false);
    }
    const int mode = !open ? 0 : sShown.load() ? 1 : 2;
    return mode == 1 || mode != sMode;
}

void frame(bool pads_neutral, bool ui_visible, int width, int height) {
    sW.store(width, std::memory_order_relaxed);
    sH.store(height, std::memory_order_relaxed);
    if (!pads_neutral && !ui::settings::no_user_input()) sShown.store(false);
    const int mode = ui_visible || !sOpen.load() ? 0 : sShown.load() ? 1 : 2;
    sMode = mode;
    ui::host::set_game_pointer(mode, sX.load(), sY.load());
}

bool glow(float box[4]) {
    if (sMode != 1) return false;
    float cx, cy, b[4];
    if (!to_canvas(sX.load(), sY.load(), cx, cy) || !des_menu_mouse_hit(cx, cy, b)) return false;
    return to_pixels(b[0], b[1], box[0], box[1]) && to_pixels(b[2], b[3], box[2], box[3]);
}

bool open() {
    return sOpen.load(std::memory_order_acquire);
}

bool active() {
    return sOpen.load(std::memory_order_acquire) && !ui::settings::no_user_input();
}

void on_move(int x, int y) {
    sX.store(x);
    sY.store(y);
    sShown.store(true);
    float cx, cy;
    if (sLeft.load() && to_canvas(x, y, cx, cy)) des_menu_mouse_drag(cx, cy);
}

void on_button(int button, bool down, int x, int y) {
    const bool was_shown = sShown.load();
    on_move(x, y);
    if (button == 0) sLeft.store(down);
    if (!down) {
        if (button == 0) des_menu_mouse_release();  // (an arrow held down lets go)
        return;
    }
    float cx, cy;
    if (button == 0 && was_shown && to_canvas(x, y, cx, cy)) des_menu_mouse_click(cx, cy);  // (hidden: it only shows)
    if (button == 1) des_menu_mouse_back();
}

void on_wheel(int delta) {
    sShown.store(true);
    sWheel += delta;
    const int notches = sWheel / kWheelDelta;
    if (!notches) return;
    sWheel -= notches * kWheelDelta;
    float cx, cy;
    if (to_canvas(sX.load(), sY.load(), cx, cy)) des_menu_mouse_wheel(notches, cx, cy);
}

void on_key() {
    sShown.store(false);
}

void on_leave() {
    sShown.store(false);
    sLeft.store(false);
    des_menu_mouse_release();
}

}  // namespace ui::game_menu
