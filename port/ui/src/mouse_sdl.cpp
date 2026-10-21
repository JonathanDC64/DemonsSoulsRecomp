// Written for Demon's Souls. The mouse's side of the window off Windows (docs/76; mouse.hpp, and mouse.cpp for
// Windows): mouse look and the mouse buttons and wheel for the keyboard layout, with the same rules for when the game
// has the mouse as mouse.cpp, here without the frame's own cases (moving or sizing the window, its system menu).
//
// The runtime's SDL window hands the overlay the messages Windows would (rsx_d3d12_backend.h), and its relative
// motion as WM_RSX_MOUSE_MOTION. With mouse look on, the cursor is held with SDL's relative mouse mode: hidden, kept
// in the window, and the motion without the desktop's pointer acceleration. With mouse look off the cursor stays
// shown and free; SDL sends a click or a wheel notch only over the window, so those over the picture count, and
// letting go always counts.
//
// Threads: the window's thread owns the state (wndproc). The counts are atomics that the camera hook takes on the
// game's main thread (des_mouse_take). set_ui_open, on_frame and des_mouse_refresh post WM_DES_MOUSE, and the window's
// thread then looks again.
#include "mouse.hpp"

#include "des_ui.h"
#include "platform.hpp"
#include "settings.hpp"

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void cellPad_host_mouse_buttons(unsigned mask);  // ps3recomp, libs/input/cellPad.h
void cellPad_host_mouse_wheel(int notches);
void rsx_d3d12_post_message(unsigned msg, unsigned long long wp, long long lp);  // libs/video/rsx_d3d12_backend.h
}

namespace {

constexpr UINT WM_DES_MOUSE = WM_APP + 0x5D;         // look again (as mouse.cpp)
constexpr UINT WM_DES_CURSOR = WM_APP + 0x5E;        // the cursor's shape: wp = an SDL_SystemCursor
constexpr UINT WM_RSX_MOUSE_MOTION = WM_APP + 0x53;  // the runtime's relative motion: wp = x, lp = y (signed)

// Presented frames without a take of the counts after which the game's camera counts as not running (mouse.cpp).
constexpr unsigned kIdleFrames = 10;

std::atomic<int> sDx{0}, sDy{0};    // counts gathered while the game has the mouse: x right, y down
std::atomic<unsigned> sTakes{0};    // des_mouse_take calls
std::atomic<bool> sCamera{false};   // the game's camera takes the counts (on_frame)
std::atomic<bool> sHeld{false};     // relative mouse mode is on (mouse look)
std::atomic<bool> sUiOpen{false};   // a document of the UI is open
std::atomic<bool> sWindowUp{false}; // the window's thread has handed this a message: posts have somewhere to go

// The window's thread only.
SDL_Window* sWindow = nullptr;
bool sFree = false;         // the game has the buttons and the wheel, the cursor shown and free (mouse look off)
bool sMinimised = false;
bool sWaitButtons = false;  // the game would have the mouse, but a button is still held
unsigned sButtons = 0;      // the buttons held as cellPad has them: bit 0 left, 1 right, 2 middle, 3 and 4 the side ones
int sWheel = 0;             // wheel movement short of a whole notch (WHEEL_DELTA)

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

bool log_on() {
    return env_on("DES_MOUSELOG");
}

void post_look_again() {
    if (sWindowUp.load()) rsx_d3d12_post_message(WM_DES_MOUSE, 0, 0);
}

bool buttons_down() {
    return SDL_GetMouseState(nullptr, nullptr) != 0;
}

// Never with PS3_NO_USER_INPUT=1 (a test run): the mouse is the user's.
void hold(const char* why) {
    if (ui::settings::no_user_input()) return;
    if (sHeld.exchange(true)) return;
    SDL_SetRelativeMouseMode(SDL_TRUE);
    sDx.store(0);
    sDy.store(0);
    if (log_on()) std::fprintf(stderr, "[mouse] held (%s)\n", why);
}

// Shows the cursor and frees it, dropping the counts not yet taken and letting go of the buttons.
void let_go(const char* why) {
    sWaitButtons = false;
    if (sButtons) {
        sButtons = 0;
        cellPad_host_mouse_buttons(0);
    }
    sWheel = 0;
    if (sFree) {
        sFree = false;
        if (log_on()) std::fprintf(stderr, "[mouse] let go (%s)\n", why);
    }
    if (!sHeld.exchange(false)) return;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    sDx.store(0);
    sDy.store(0);
    if (log_on()) std::fprintf(stderr, "[mouse] let go (%s)\n", why);
}

// Gives the game the mouse or lets it go, as things are now.
void update(const char* why) {
    const bool look = env_on("DES_MOUSE_LOOK");
    static int sLookSaid = -1;
    if (sLookSaid != int(look)) {
        sLookSaid = int(look);
        std::fprintf(stderr, "[mouse] mouse look %s\n",
                     look ? "on" : "off (mouse buttons and wheel with the cursor free)");
    }
    const char* no = !sWindow || SDL_GetKeyboardFocus() != sWindow ? "window in the background"
                     : sMinimised                                   ? "window minimised"
                     : sUiOpen.load(std::memory_order_acquire)      ? "a menu of the UI is open"
                     : !sCamera.load(std::memory_order_acquire)     ? "the game's camera is not running"
                                                                    : nullptr;
    if (no) {
        let_go(no);
        return;
    }
    if (look ? sHeld.load() : sFree) return;  // the game has the mouse already, the same way
    if (sHeld.load() || sFree) let_go(look ? "mouse look on" : "mouse look off");  // the other way: over again
    sWaitButtons = buttons_down();  // the click that brought the window up ends first
    if (sWaitButtons) return;
    if (look) {
        hold(why);
    } else {
        sFree = true;
        if (log_on()) std::fprintf(stderr, "[mouse] buttons and wheel, cursor free (%s)\n", why);
    }
}

void on_button(int i, bool down) {
    if (sWaitButtons) {
        if (!buttons_down()) update("buttons up");
        return;  // nothing counts before the game has the mouse
    }
    if (!sHeld.load(std::memory_order_relaxed) && !sFree) return;
    unsigned b = sButtons;
    if (down) b |= 1u << i;
    else b &= ~(1u << i);
    if (b != sButtons) {
        sButtons = b;
        cellPad_host_mouse_buttons(b);
    }
}

}  // namespace

namespace ui::mouse {

bool wndproc(void* hwnd, unsigned msg, unsigned long long wp, long long lp, long long* result) {
    sWindow = static_cast<SDL_Window*>(hwnd);
    if (!sWindowUp.load(std::memory_order_relaxed)) sWindowUp.store(true);
    switch (msg) {
    case WM_RSX_MOUSE_MOTION:
        if (sHeld.load(std::memory_order_relaxed)) {  // the counts, for the camera
            sDx.fetch_add(int(static_cast<long long>(wp)), std::memory_order_relaxed);
            sDy.fetch_add(int(lp), std::memory_order_relaxed);
        }
        return false;
    case WM_LBUTTONDOWN: on_button(0, true); return false;
    case WM_LBUTTONUP: on_button(0, false); return false;
    case WM_RBUTTONDOWN: on_button(1, true); return false;
    case WM_RBUTTONUP: on_button(1, false); return false;
    case WM_MBUTTONDOWN: on_button(2, true); return false;
    case WM_MBUTTONUP: on_button(2, false); return false;
    case WM_XBUTTONDOWN: on_button(GET_XBUTTON_WPARAM(wp) == 2 ? 4 : 3, true); return false;
    case WM_XBUTTONUP: on_button(GET_XBUTTON_WPARAM(wp) == 2 ? 4 : 3, false); return false;
    case WM_MOUSEWHEEL:  // WHEEL_DELTA a notch, positive away from the user
        if (sWaitButtons || (!sHeld.load(std::memory_order_relaxed) && !sFree)) return false;
        sWheel += GET_WHEEL_DELTA_WPARAM(wp);
        if (const int notches = sWheel / WHEEL_DELTA) {
            sWheel -= notches * WHEEL_DELTA;
            cellPad_host_mouse_wheel(notches);
        }
        return false;
    case WM_DES_MOUSE:
        update("menu closed, camera running or setting changed");
        *result = 0;
        return true;
    case WM_DES_CURSOR: {
        static SDL_Cursor* sCursors[SDL_NUM_SYSTEM_CURSORS] = {};
        const auto shape = static_cast<SDL_SystemCursor>(wp < SDL_NUM_SYSTEM_CURSORS ? wp : 0);
        if (!sCursors[shape]) sCursors[shape] = SDL_CreateSystemCursor(shape);
        if (sCursors[shape]) SDL_SetCursor(sCursors[shape]);
        *result = 0;
        return true;
    }
    case WM_SETFOCUS:
        update("focus");
        return false;
    case WM_KILLFOCUS:
        let_go("focus lost");
        return false;
    case WM_SIZE:  // (minimised, restored, Alt+Enter)
        sMinimised = wp == SIZE_MINIMIZED;
        update("window changed");
        return false;
    case WM_MOUSEMOVE:
        if (sWaitButtons && !buttons_down()) update("buttons up");
        return false;
    default:
        return false;
    }
}

void set_ui_open(bool open) {
    if (sUiOpen.exchange(open) != open) post_look_again();
}

void on_frame() {
    static unsigned sLastTakes = 0, sIdle = kIdleFrames;
    const unsigned takes = sTakes.load(std::memory_order_relaxed);
    if (takes != sLastTakes) {
        sLastTakes = takes;
        sIdle = 0;
    } else if (sIdle < kIdleFrames) {
        ++sIdle;
    }
    const bool camera = sIdle < kIdleFrames;
    if (sCamera.load(std::memory_order_relaxed) != camera) {
        sCamera.store(camera, std::memory_order_release);
        post_look_again();
    }
}

void release_now() {
    if (sHeld.exchange(false)) SDL_SetRelativeMouseMode(SDL_FALSE);
}

void set_cursor(int shape) {
    static int sShape = SDL_SYSTEM_CURSOR_ARROW;
    if (shape == sShape) return;
    sShape = shape;
    if (sWindowUp.load()) rsx_d3d12_post_message(WM_DES_CURSOR, (unsigned long long)shape, 0);
}

}  // namespace ui::mouse

extern "C" void des_mouse_take(int* dx, int* dy) {
    sTakes.fetch_add(1, std::memory_order_relaxed);
    *dx = sDx.exchange(0, std::memory_order_relaxed);
    *dy = sDy.exchange(0, std::memory_order_relaxed);
}

extern "C" void des_mouse_refresh(void) {
    post_look_again();
}
