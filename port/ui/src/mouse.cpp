// Written for Demon's Souls. The mouse's side of the window (docs/41, sections 1.3 and 3.2): mouse look, and the mouse
// buttons and wheel for the keyboard layout.
//
// The mouse is read as raw input (WM_INPUT): counts as the mouse reports them, without Windows' pointer acceleration
// and without stopping at the edge of the screen, and the buttons as the mouse has them (Windows' swapped buttons do
// not apply). The game has the mouse while all of these hold:
//   - the window is in front and not minimised;
//   - no document of the UI is open (the start screen, the menu, settings, the keyboard, a question, the startup
//     work), and no menu of the game that the mouse can use (game_menu.cpp: it points and clicks there instead);
//   - the window is not being moved or resized, and its system menu is not open;
//   - the game's follow camera or the bow's aim view is running: des_mouse_look (des_options.cpp) takes the counts
//     once a game frame, and after kIdleFrames presented frames without a take (the title screen, a load, a cutscene)
//     the game lets go;
//   - no mouse button is still held from before, such as the click that brought the window to the front or a drag of
//     its title bar: the game takes the mouse once every button is up.
// With mouse look on (DES_MOUSE_LOOK=1, or Settings, Controls), the cursor is then hidden (WM_SETCURSOR) and held on
// the middle of the window (ClipCursor), so it cannot reach the title bar, the frame or another window, and the counts
// turn the camera. Windows can drop the clip by itself (Ctrl+Alt+Del, a UAC prompt), so it is checked again as the
// mouse moves. With mouse look off, the cursor stays shown and free: a button counts only if it goes down over the
// game's picture (the client area, with no other window on top), and so does a wheel notch; letting go always counts.
//
// Either way, the buttons and wheel go to cellPad (cellPad_host_mouse_buttons and _wheel), where the keyboard layout's
// bindings name them (mouse1 = left click is R1, wheelup is Up, and so on; settings.cpp). Anything else lets the mouse
// go: the cursor shown and free, the counts not yet taken dropped, the buttons let go. So a click that brings the
// window to the front, or one on the UI, never presses anything in the game.
//
// Threads: the window's thread owns the state (wndproc). The counts are atomics that the camera hook takes on the
// game's main thread (des_mouse_take). set_ui_open and on_frame (the render thread) and des_mouse_refresh (any thread)
// post WM_DES_MOUSE, and the window's thread then looks again.
#include "mouse.hpp"

#include "des_ui.h"
#include "settings.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void cellPad_host_mouse_buttons(unsigned mask);  // ps3recomp, libs/input/cellPad.h
void cellPad_host_mouse_wheel(int notches);
}

namespace {

// Look again (posted from other threads). The D3D12 backend's own message is WM_APP + 0x52.
constexpr UINT WM_DES_MOUSE = WM_APP + 0x5D;
// The UI's own cursor came or went (set_own_cursor): the cursor over the window is set again.
constexpr UINT WM_DES_OWN_CURSOR = WM_APP + 0x5F;

// Presented frames without a take of the counts after which the game's camera counts as not running. The camera
// takes them once a game frame, and the game presents once a game frame.
constexpr unsigned kIdleFrames = 10;

std::atomic<int> sDx{0}, sDy{0};    // counts gathered while the game has the mouse: x right, y down
std::atomic<unsigned> sTakes{0};    // des_mouse_take calls
std::atomic<bool> sCamera{false};   // the game's camera takes the counts (on_frame)
std::atomic<bool> sHeld{false};     // the cursor is hidden and clipped (mouse look)
std::atomic<bool> sUiOpen{false};   // a document of the UI is open
std::atomic<bool> sGameMenu{false}; // a menu of the game is open (set_game_menu)
std::atomic<bool> sOwnCursor{false};  // the UI draws its own cursor (set_own_cursor): the system's is hidden
std::atomic<HWND> sHwnd{nullptr};   // the game's window, for posting to it

// The window's thread only.
bool sRegistered = false;   // raw mouse input is registered for the window
bool sFree = false;         // the game has the buttons and the wheel, the cursor shown and free (mouse look off)
bool sSizeMove = false;     // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE
bool sMenuLoop = false;     // the window's system menu is open
bool sWaitButtons = false;  // the game would have the mouse, but a button is still held
bool sTracking = false;     // the window is told when the cursor leaves it (TrackMouseEvent), for a game's menu
RECT sClip{};               // the clip set last
bool sAbsValid = false;     // a device that reports positions (remote desktop, tablets): its last one, in pixels
LONG sAbsX = 0, sAbsY = 0;
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
    if (const HWND hwnd = sHwnd.load()) PostMessageW(hwnd, WM_DES_MOUSE, 0, 0);
}

bool buttons_down() {
    static const int kButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (const int vk : kButtons) {
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    }
    return false;
}

bool is_mouse_message(unsigned msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_NCMOUSEMOVE && msg <= WM_NCXBUTTONDBLCLK);
}

// Raw input from the mouse (usage page 1, usage 2) for the window. Without RIDEV_INPUTSINK it arrives only while the
// window is in front; without RIDEV_NOLEGACY the UI still gets its usual mouse messages.
void register_raw(HWND hwnd) {
    if (sRegistered) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.hwndTarget = hwnd;
    if (RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        sRegistered = true;
        return;
    }
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[mouse] RegisterRawInputDevices failed (error %lu): no mouse look, buttons or wheel\n",
                     GetLastError());
    }
}

// The cursor is over the window's client area, with no other window on top there: a click there is on the game.
bool over_picture(HWND hwnd) {
    POINT p{};
    if (!GetCursorPos(&p) || WindowFromPoint(p) != hwnd) return false;
    RECT c{};
    return ScreenToClient(hwnd, &p) && GetClientRect(hwnd, &c) && PtInRect(&c, p);
}

// The middle of the client area, in screen pixels: where the hidden cursor is held.
bool middle_of(HWND hwnd, RECT& r) {
    RECT c{};
    if (!GetClientRect(hwnd, &c) || c.right <= c.left || c.bottom <= c.top) return false;
    POINT p{(c.left + c.right) / 2, (c.top + c.bottom) / 2};
    if (!ClientToScreen(hwnd, &p)) return false;
    r = {p.x, p.y, p.x + 1, p.y + 1};
    return true;
}

// Hides the cursor and holds it on the middle of the window (again, if held already: the window may have moved).
// Never with PS3_NO_USER_INPUT=1 (a test run): the mouse is the user's, and the runtime keeps its raw input from the
// window too.
void hold(HWND hwnd, const char* why) {
    if (ui::settings::no_user_input()) return;
    RECT r;
    if (!middle_of(hwnd, r)) return;
    sClip = r;
    ClipCursor(&r);
    SetCursor(nullptr);
    if (!sHeld.exchange(true)) {
        sDx.store(0);
        sDy.store(0);
        sAbsValid = false;
        if (log_on()) std::fprintf(stderr, "[mouse] held (%s)\n", why);
    }
}

// Shows the cursor and frees it, dropping the counts not yet taken and letting go of the buttons.
void let_go(HWND hwnd, const char* why) {
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
    ClipCursor(nullptr);
    sDx.store(0);
    sDy.store(0);
    // Shown at once, not at its next move: the cursor is over the window, which sets it on WM_SETCURSOR.
    POINT p{};
    if (GetCursorPos(&p) && WindowFromPoint(p) == hwnd) SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (log_on()) std::fprintf(stderr, "[mouse] let go (%s)\n", why);
}

// Gives the game the mouse or lets it go, as things are now.
void update(HWND hwnd, const char* why) {
    register_raw(hwnd);
    const bool look = env_on("DES_MOUSE_LOOK");
    static int sLookSaid = -1;
    if (sLookSaid != int(look)) {
        sLookSaid = int(look);
        std::fprintf(stderr, "[mouse] mouse look %s\n",
                     look ? "on" : "off (mouse buttons and wheel with the cursor free)");
    }
    const char* no = !sRegistered                                       ? "no raw mouse input"
                     : GetForegroundWindow() != hwnd || IsIconic(hwnd) ? "window in the background"
                     : sUiOpen.load(std::memory_order_acquire)          ? "a menu of the UI is open"
                     : sGameMenu.load(std::memory_order_acquire)        ? "a menu of the game is open"
                     : sSizeMove                                        ? "window moving or resizing"
                     : sMenuLoop                                        ? "system menu"
                     : !sCamera.load(std::memory_order_acquire)         ? "the game's camera is not running"
                                                                        : nullptr;
    if (no) {
        let_go(hwnd, no);
        return;
    }
    if (look ? sHeld.load() : sFree) {  // the game has the mouse already, the same way
        if (look) hold(hwnd, why);       // the clip again
        return;
    }
    if (sHeld.load() || sFree) let_go(hwnd, look ? "mouse look on" : "mouse look off");  // the other way: over again
    sWaitButtons = buttons_down();  // the click that brought the window up, or a drag of its title bar, ends first
    if (sWaitButtons) return;
    if (look) {
        hold(hwnd, why);
    } else {
        sFree = true;
        if (log_on()) std::fprintf(stderr, "[mouse] buttons and wheel, cursor free (%s)\n", why);
    }
}

void on_input(HWND hwnd, WPARAM wp, LPARAM lp) {
    if (sWaitButtons) {
        if (!buttons_down()) update(hwnd, "buttons up");
        return;  // nothing counts before the game has the mouse
    }
    const bool held = sHeld.load(std::memory_order_relaxed);
    if ((!held && !sFree) || GET_RAWINPUT_CODE_WPARAM(wp) != RIM_INPUT) return;
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        ri.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const RAWMOUSE& m = ri.data.mouse;
    if (held) {  // the counts, for the camera
        LONG dx = m.lLastX, dy = m.lLastY;
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {  // 0..65535 across the screen (or the virtual desktop): pixels moved
            const bool desk = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const LONG x = MulDiv(m.lLastX, GetSystemMetrics(desk ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535);
            const LONG y = MulDiv(m.lLastY, GetSystemMetrics(desk ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535);
            dx = sAbsValid ? x - sAbsX : 0;
            dy = sAbsValid ? y - sAbsY : 0;
            sAbsX = x;
            sAbsY = y;
            sAbsValid = true;
        }
        if (dx) sDx.fetch_add(int(dx), std::memory_order_relaxed);
        if (dy) sDy.fetch_add(int(dy), std::memory_order_relaxed);
    }
    if (const USHORT f = m.usButtonFlags) {  // the buttons and the wheel, for cellPad
        static const USHORT kDown[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
                                        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        static const USHORT kUp[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
                                      RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
        const bool over = held || over_picture(hwnd);  // (a free cursor: presses and notches over the picture only)
        unsigned b = sButtons;
        for (int i = 0; i < 5; i++) {
            if ((f & kDown[i]) && over) b |= 1u << i;
            if (f & kUp[i]) b &= ~(1u << i);
        }
        if (b != sButtons) {
            sButtons = b;
            cellPad_host_mouse_buttons(b);
        }
        if ((f & RI_MOUSE_WHEEL) && over) {  // usButtonData: signed, WHEEL_DELTA a notch, positive away from the user
            sWheel += short(m.usButtonData);
            if (const int notches = sWheel / WHEEL_DELTA) {
                sWheel -= notches * WHEEL_DELTA;
                cellPad_host_mouse_wheel(notches);
            }
        }
    }
    if (!held) return;
    RECT clip{};
    if (GetClipCursor(&clip) && !EqualRect(&clip, &sClip)) hold(hwnd, "clip lost");  // Windows dropped it
}

}  // namespace

namespace ui::mouse {

bool wndproc(void* hwnd_, unsigned msg, unsigned long long wp, long long lp, long long* result) {
    const HWND hwnd = static_cast<HWND>(hwnd_);
    if (sHwnd.load(std::memory_order_relaxed) != hwnd) sHwnd.store(hwnd);
    switch (msg) {
    case WM_INPUT:
        on_input(hwnd, WPARAM(wp), LPARAM(lp));
        return false;  // DefWindowProc frees the input
    case WM_SETCURSOR:
        if (LOWORD(lp) != HTCLIENT ||
            !(sHeld.load(std::memory_order_relaxed) || sOwnCursor.load(std::memory_order_relaxed)))
            return false;
        SetCursor(nullptr);
        *result = TRUE;
        return true;
    case WM_DES_OWN_CURSOR: {  // set again now, not at the next move, if the cursor is over the window
        POINT p{};
        if (GetCursorPos(&p) && WindowFromPoint(p) == hwnd) {
            const LRESULT hit = SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
            SendMessageW(hwnd, WM_SETCURSOR, WPARAM(hwnd), MAKELPARAM(LOWORD(hit), WM_MOUSEMOVE));
        }
        *result = 0;
        return true;
    }
    case WM_DES_MOUSE:
        update(hwnd, "menu closed, camera running or setting changed");
        *result = 0;
        return true;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            let_go(hwnd, "window in the background");
        } else {
            update(hwnd, "window in front");
        }
        return false;
    case WM_ACTIVATEAPP:
        if (!wp) let_go(hwnd, "another program in front");
        return false;
    case WM_SETFOCUS:
        update(hwnd, "focus");
        return false;
    case WM_KILLFOCUS:
        let_go(hwnd, "focus lost");
        return false;
    case WM_ENTERSIZEMOVE:
        sSizeMove = true;
        let_go(hwnd, "window moving or resizing");
        return false;
    case WM_EXITSIZEMOVE:
        sSizeMove = false;
        update(hwnd, "window moved or resized");
        return false;
    case WM_ENTERMENULOOP:
        sMenuLoop = true;
        let_go(hwnd, "system menu");
        return false;
    case WM_EXITMENULOOP:
        sMenuLoop = false;
        update(hwnd, "system menu closed");
        return false;
    case WM_SIZE:  // (minimised, restored, Alt+Enter, a display of another scale)
    case WM_MOVE:
    case WM_DISPLAYCHANGE:
    case WM_CAPTURECHANGED:
        update(hwnd, "window changed");  // the clip again, or let go (minimised), or take the mouse (restored)
        return false;
    case WM_WTSSESSION_CHANGE:  // (the backend sees it too)
        if (wp == WTS_SESSION_LOCK) {
            let_go(hwnd, "session locked");
        } else if (wp == WTS_SESSION_UNLOCK) {
            update(hwnd, "session unlocked");
        }
        return false;
    case WM_DESTROY:
        let_go(hwnd, "window closing");
        return false;
    case WM_MOUSEMOVE:  // over a menu of the game: WM_MOUSELEAVE when the cursor leaves (game_menu.cpp hides it)
        if (sGameMenu.load(std::memory_order_relaxed) && !sTracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
            sTracking = TrackMouseEvent(&t) != FALSE;
        }
        if (sWaitButtons && !buttons_down()) update(hwnd, "buttons up");
        return false;
    case WM_MOUSELEAVE:
        sTracking = false;
        return false;
    default:
        if (sWaitButtons && is_mouse_message(msg) && !buttons_down()) update(hwnd, "buttons up");
        return false;
    }
}

void set_ui_open(bool open) {
    if (sUiOpen.exchange(open) != open) post_look_again();
}

void set_game_menu(bool open) {
    if (sGameMenu.exchange(open) != open) post_look_again();
}

void set_own_cursor(bool drawn) {
    if (ui::settings::no_user_input()) drawn = false;  // a test run's: the mouse over the window is the user's
    const HWND hwnd = sHwnd.load();  // (none yet: the next frame tells it again)
    if (hwnd && sOwnCursor.exchange(drawn) != drawn) PostMessageW(hwnd, WM_DES_OWN_CURSOR, 0, 0);
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
    if (sHeld.exchange(false)) ClipCursor(nullptr);
}

void client_size(void* hwnd, int& width, int& height) {
    RECT rc{};
    GetClientRect(static_cast<HWND>(hwnd), &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
}

void hold_buttons(void* hwnd, bool down) {
    if (down) SetCapture(static_cast<HWND>(hwnd));
    else ReleaseCapture();
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
