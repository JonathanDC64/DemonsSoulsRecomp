// Written for Demon's Souls. Mouse look's side of the window (docs/41, section 1.3).
//
// With mouse look on (DES_MOUSE_LOOK=1, or Settings, Controls), the mouse is read as raw input (WM_INPUT): counts as
// the mouse reports them, without Windows' pointer acceleration and without stopping at the edge of the screen. The
// game has the mouse while all of these hold:
//   - mouse look is on;
//   - the window is in front and not minimised;
//   - no document of the UI is open (the start screen, the menu, settings, the keyboard, a question, the startup
//     work);
//   - the window is not being moved or resized, and its system menu is not open;
//   - the game's follow camera is running: des_mouse_look (des_options.cpp) takes the counts once a game frame, and
//     after kIdleFrames presented frames without a take (the title screen, a load, a cutscene) the game lets go;
//   - no mouse button is still held from before, such as the click that brought the window to the front or a drag of
//     its title bar: the game takes the mouse once every button is up.
// While the game has the mouse, the cursor is hidden (WM_SETCURSOR) and held on the middle of the window (ClipCursor),
// so it cannot reach the title bar, the frame or another window. Anything else lets it go, shown and free, and drops
// the counts not yet taken. Windows can drop the clip by itself (Ctrl+Alt+Del, a UAC prompt), so it is checked again
// as the mouse moves.
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

namespace {

// Look again (posted from other threads). The D3D12 backend's own message is WM_APP + 0x52.
constexpr UINT WM_DES_MOUSE = WM_APP + 0x5D;

// Presented frames without a take of the counts after which the game's camera counts as not running. The camera
// takes them once a game frame, and the game presents once a game frame.
constexpr unsigned kIdleFrames = 10;

std::atomic<int> sDx{0}, sDy{0};    // counts gathered while the game has the mouse: x right, y down
std::atomic<unsigned> sTakes{0};    // des_mouse_take calls
std::atomic<bool> sCamera{false};   // the game's camera takes the counts (on_frame)
std::atomic<bool> sHeld{false};     // the cursor is hidden and clipped
std::atomic<bool> sUiOpen{false};   // a document of the UI is open
std::atomic<HWND> sHwnd{nullptr};   // the game's window, for posting to it

// The window's thread only.
bool sRegistered = false;   // raw mouse input is registered for the window
bool sSizeMove = false;     // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE
bool sMenuLoop = false;     // the window's system menu is open
bool sWaitButtons = false;  // the game would have the mouse, but a button is still held
RECT sClip{};               // the clip set last
bool sAbsValid = false;     // a device that reports positions (remote desktop, tablets): its last one, in pixels
LONG sAbsX = 0, sAbsY = 0;

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

// Raw input from the mouse (usage page 1, usage 2) for the window, while mouse look is on. Without RIDEV_INPUTSINK it
// arrives only while the window is in front; without RIDEV_NOLEGACY the UI still gets its usual mouse messages.
void register_raw(HWND hwnd, bool on) {
    if (on == sRegistered) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = on ? 0 : RIDEV_REMOVE;
    rid.hwndTarget = on ? hwnd : nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        sRegistered = on;
        std::fprintf(stderr, "[mouse] mouse look %s\n", on ? "on (raw mouse input)" : "off");
        return;
    }
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[mouse] RegisterRawInputDevices failed (error %lu): no mouse look\n", GetLastError());
    }
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

// Shows the cursor and frees it, dropping the counts not yet taken.
void let_go(HWND hwnd, const char* why) {
    sWaitButtons = false;
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
    const bool on = env_on("DES_MOUSE_LOOK");
    register_raw(hwnd, on);
    const char* no = !on                                                ? "mouse look off"
                     : !sRegistered                                     ? "no raw mouse input"
                     : GetForegroundWindow() != hwnd || IsIconic(hwnd) ? "window in the background"
                     : sUiOpen.load(std::memory_order_acquire)          ? "a menu of the UI is open"
                     : sSizeMove                                        ? "window moving or resizing"
                     : sMenuLoop                                        ? "system menu"
                     : !sCamera.load(std::memory_order_acquire)         ? "the game's camera is not running"
                                                                        : nullptr;
    if (no) {
        let_go(hwnd, no);
        return;
    }
    if (sHeld.load()) {
        hold(hwnd, why);  // the clip again
        return;
    }
    sWaitButtons = buttons_down();  // the click that brought the window up, or a drag of its title bar, ends first
    if (!sWaitButtons) hold(hwnd, why);
}

void on_input(HWND hwnd, WPARAM wp, LPARAM lp) {
    if (sWaitButtons) {
        if (!buttons_down()) update(hwnd, "buttons up");
        return;  // nothing counts before the game has the mouse
    }
    if (!sHeld.load(std::memory_order_relaxed) || GET_RAWINPUT_CODE_WPARAM(wp) != RIM_INPUT) return;
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        ri.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const RAWMOUSE& m = ri.data.mouse;
    LONG dx = m.lLastX, dy = m.lLastY;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {  // 0..65535 across the screen (or the virtual desktop): a change in pixels
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
        if (!sHeld.load(std::memory_order_relaxed) || LOWORD(lp) != HTCLIENT) return false;
        SetCursor(nullptr);
        *result = TRUE;
        return true;
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
    default:
        if (sWaitButtons && is_mouse_message(msg) && !buttons_down()) update(hwnd, "buttons up");
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
    if (sHeld.exchange(false)) ClipCursor(nullptr);
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
