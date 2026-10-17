// The wrapper UI's connection to the port (docs/32): installs the overlay on the ps3recomp D3D12 backend and runs the
// UI from its two hooks. Written for Demon's Souls; the shape follows FFCCEOT-Recomp's main loop (SDL events in, one
// ui::update and draw per frame), with the window procedure and the frame hook in place of SDL.
//
//   wndproc (the window's message thread): F1 and Esc are always the UI's. While a document is up, the keyboard
//     (except F9-F12, the backend's capture keys) and the mouse are queued and kept from the game window.
//   frame (the render thread, once per presented frame, before Present): drains the queue into RmlUi, reads the
//     gamepads, updates and draws. When nothing is showing it returns after a couple of loads: no RmlUi work, no
//     pad reads, no GPU work. The gamepad's menu chord is then seen in g_pad0_buttons, the game's own pad 0 read.
//
// The game does not see the pads or the keyboard while a document is up (cellPad_set_host_hold), and gets them back
// only once every key and button is released, so the press that closed the menu does not reach the game.
#include "des_ui.h"

#include "RmlUi_Platform_Win32.h"

#include "app.hpp"
#include "host.hpp"
#include "input.hpp"
#include "menu_bar.hpp"
#include "overlay.hpp"
#include "prelaunch.hpp"
#include "settings.hpp"
#include "settings_window.hpp"
#include "ui.hpp"

#include <windows.h>
#include <windowsx.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---- What the UI uses of the runtime (declared here: des_ui does not take ps3recomp's include paths) ----
extern "C" {
typedef struct rsx_d3d12_overlay {
    void* user;
    int (*wndproc)(void* user, void* hwnd, unsigned msg, unsigned long long wp, long long lp, long long* result);
    void (*frame)(void* user, void* back_buffer, unsigned index, unsigned width, unsigned height);
} rsx_d3d12_overlay;
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay);
void* rsx_d3d12_device(void);
void* rsx_d3d12_queue(void);
void* rsx_d3d12_hwnd(void);

void cellPad_set_host_hold(int hold);
extern volatile unsigned g_pad0_buttons;  // pad 0 as the game last read it (CELL_PAD_CTRL_* layout)

typedef struct {
    unsigned short buttons;    // CELL_PAD_CTRL_*
    unsigned char lx, ly, rx, ry;  // 0-255, centre 128, up = 0
    unsigned char l2, r2;
} PadHidSonyState;
int pad_hid_sony_poll(PadHidSonyState* out, int max);
}

namespace {

using clock_type = std::chrono::steady_clock;

// CELL_PAD_CTRL_*
constexpr unsigned kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3;
constexpr unsigned kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7;
constexpr unsigned kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11;
constexpr unsigned kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15;

// How long the UI keeps drawing after the last document hides: its closing transitions.
constexpr auto kGrace = std::chrono::milliseconds(600);

struct InputEvent {
    enum Type { KeyDown, KeyUp, MouseMove, MouseDown, MouseUp, Wheel, Leave } type;
    int a = 0;  // key (VK) / button / x
    int b = 0;  // y
    int mods = 0;
    float wheel = 0.0f;
};

std::mutex sQueueLock;
std::vector<InputEvent> sQueue;
std::atomic<bool> sQueued{false};
std::atomic<bool> sCapturing{false};  // a document is up: the wndproc takes the keyboard and the mouse
std::atomic<int> sBufferW{1280}, sBufferH{720};

bool sStartScreen = false;
bool sDocumentsPushed = false;
bool sActive = false;
bool sHold = false;
bool sChordWasDown = false;
clock_type::time_point sLastVisible{};
unsigned long long sFrame = 0;
bool sInitFailed = false;
bool sOpenPending = false;     // DES_UI_OPEN, until the documents exist
unsigned sChordMask = kL3 | kR3;  // the menu chord, refreshed while the UI is awake

// DES_UI_SHOT=path[,frame][;path[,frame]...]: write the back buffer after the UI draws, at that presented frame.
struct Shot {
    std::string path;
    unsigned long long frame;
};
std::vector<Shot> sShots;

// DES_UI_KEYS=frame:KEY[,frame:KEY...]: press a key at a presented frame (a test drives the menus with it).
struct ScriptKey {
    unsigned long long frame;
    int vk;
};
std::vector<ScriptKey> sScriptKeys;

void push_event(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(sQueueLock);
    sQueue.push_back(e);
    sQueued.store(true, std::memory_order_release);
}

bool is_menu_key(WPARAM vk) {
    return vk == VK_F1 || vk == VK_ESCAPE;
}

// The back buffer is stretched over the client area: mouse positions are in back buffer pixels.
void map_mouse(HWND hwnd, LPARAM lp, int& x, int& y) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = std::max<int>(1, rc.right - rc.left), ch = std::max<int>(1, rc.bottom - rc.top);
    x = GET_X_LPARAM(lp) * sBufferW.load() / cw;
    y = GET_Y_LPARAM(lp) * sBufferH.load() / ch;
}

int wndproc(void*, void* hwnd_, unsigned msg, unsigned long long wp, long long lp, long long* result) {
    HWND hwnd = static_cast<HWND>(hwnd_);
    const bool capturing = sCapturing.load(std::memory_order_acquire);
    switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const bool down = msg == WM_KEYDOWN;
        if (is_menu_key(wp) || (capturing && !(wp >= VK_F9 && wp <= VK_F12))) {
            if (!(down && (lp & (1LL << 30)) && is_menu_key(wp) && !capturing)) {  // no auto-repeat opening
                push_event({down ? InputEvent::KeyDown : InputEvent::KeyUp, int(wp), 0, RmlWin32::GetKeyModifierState()});
            }
            *result = 0;
            return 1;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        if (!capturing) return 0;
        int x, y;
        map_mouse(hwnd, lp, x, y);
        InputEvent e{InputEvent::MouseMove, x, y, RmlWin32::GetKeyModifierState()};
        push_event(e);
        if (msg != WM_MOUSEMOVE) {
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 0
                               : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                                                                                : 2;
            push_event({down ? InputEvent::MouseDown : InputEvent::MouseUp, button, 0, e.mods});
            if (down) SetCapture(hwnd);
            else ReleaseCapture();
        }
        *result = 0;
        return 1;
    }
    case WM_MOUSEWHEEL: {
        if (!capturing) return 0;
        InputEvent e{InputEvent::Wheel, 0, 0, RmlWin32::GetKeyModifierState()};
        e.wheel = -float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
        push_event(e);
        *result = 0;
        return 1;
    }
    case WM_MOUSELEAVE:
    case WM_KILLFOCUS:
        if (capturing) push_event({InputEvent::Leave});
        return 0;
    default:
        return 0;
    }
}

// ---- Gamepads: XInput and the Sony HID reader, merged (the same readers the game's pad uses) ----

unsigned xinput_to_cell(WORD b) {
    unsigned c = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP) c |= kUp;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) c |= kDown;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) c |= kLeft;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) c |= kRight;
    if (b & XINPUT_GAMEPAD_START) c |= kStart;
    if (b & XINPUT_GAMEPAD_BACK) c |= kSelect;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) c |= kL3;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) c |= kR3;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) c |= kL1;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) c |= kR1;
    if (b & XINPUT_GAMEPAD_A) c |= kCross;
    if (b & XINPUT_GAMEPAD_B) c |= kCircle;
    if (b & XINPUT_GAMEPAD_X) c |= kSquare;
    if (b & XINPUT_GAMEPAD_Y) c |= kTriangle;
    return c;
}

float pick(float a, float b) {
    return std::abs(a) >= std::abs(b) ? a : b;
}

// Polls XInput slots only while a slot answers: a missing pad's XInputGetState is slow, so empty slots are retried
// once a second.
ui::input::PadState read_pads(bool& neutral) {
    static clock_type::time_point sNextScan[XUSER_MAX_COUNT]{};
    ui::input::PadState pad;
    neutral = true;
    const auto now = clock_type::now();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        if (now < sNextScan[i]) continue;
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) {
            sNextScan[i] = now + std::chrono::seconds(1);
            continue;
        }
        const XINPUT_GAMEPAD& g = st.Gamepad;
        unsigned b = xinput_to_cell(g.wButtons);
        if (g.bLeftTrigger > 100) b |= kL2;
        if (g.bRightTrigger > 100) b |= kR2;
        pad.buttons |= b;
        const float lx = std::clamp(g.sThumbLX / 32767.0f, -1.0f, 1.0f);
        const float ly = std::clamp(-g.sThumbLY / 32767.0f, -1.0f, 1.0f);
        pad.lx = pick(pad.lx, lx);
        pad.ly = pick(pad.ly, ly);
        if (b || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || std::abs(g.sThumbLX) > 8000 || std::abs(g.sThumbLY) > 8000 ||
            std::abs(g.sThumbRX) > 8000 || std::abs(g.sThumbRY) > 8000)
            neutral = false;
    }
    PadHidSonyState hid[4];
    const int n = pad_hid_sony_poll(hid, 4);
    for (int i = 0; i < n; i++) {
        unsigned b = hid[i].buttons;
        if (hid[i].l2 > 100) b |= kL2;
        if (hid[i].r2 > 100) b |= kR2;
        pad.buttons |= b;
        pad.lx = pick(pad.lx, (hid[i].lx - 128) / 127.0f);
        pad.ly = pick(pad.ly, (hid[i].ly - 128) / 127.0f);
        const auto off = [](unsigned char v) { return std::abs(int(v) - 128) > 32; };
        if (b || hid[i].l2 > 30 || hid[i].r2 > 30 || off(hid[i].lx) || off(hid[i].ly) || off(hid[i].rx) || off(hid[i].ry))
            neutral = false;
    }
    return pad;
}

// The keys the game reads from the keyboard (cellPad's keyboard pad), plus the menu keys: the game gets its input back
// only when none of them is held.
bool keyboard_neutral() {
    static const int kKeys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'A', 'S', 'Q', 'W', '1', '2',
                                VK_RETURN, VK_TAB, VK_F1, VK_ESCAPE, VK_LBUTTON, VK_RBUTTON};
    for (int k : kKeys)
        if (GetAsyncKeyState(k) & 0x8000) return false;
    return true;
}

ui::MenuBar* menu_bar() {
    return static_cast<ui::MenuBar*>(ui::find_document(ui::DocumentScope::MenuBar));
}

void open_menu_bar() {
    if (auto* bar = menu_bar(); bar && !bar->visible()) {
        ui::play_sound(ui::kSoundMenuOpen);
        bar->show();
    }
}

void push_documents() {
    if (sDocumentsPushed) return;
    sDocumentsPushed = true;
    ui::push_document(std::make_unique<ui::Overlay>(), true, true);
    ui::push_document(std::make_unique<ui::MenuBar>(), false);
    if (sStartScreen) ui::push_document(std::make_unique<ui::Prelaunch>());

    // DES_UI_OPEN=menu | settings: open one at startup (for screenshots and tests).
    if (const char* open = std::getenv("DES_UI_OPEN"); open && *open) {
        if (!std::strcmp(open, "menu")) open_menu_bar();
        else if (!std::strcmp(open, "settings")) ui::push_document(std::make_unique<ui::SettingsWindow>());
    }
}

bool ensure_init(int width, int height) {
    if (ui::host::is_initialized()) return true;
    if (sInitFailed) return false;
    if (!ui::host::init(rsx_d3d12_device(), rsx_d3d12_queue(), rsx_d3d12_hwnd(), width, height) || !ui::initialize()) {
        std::fprintf(stderr, "[ui] the UI could not start; it stays off for this run\n");
        sInitFailed = true;
        return false;
    }
    push_documents();
    return true;
}

void process_queue() {
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lock(sQueueLock);
        events.swap(sQueue);
        sQueued.store(false, std::memory_order_relaxed);
    }
    Rml::Context* ctx = ui::host::context();
    for (const InputEvent& e : events) {
        switch (e.type) {
        case InputEvent::KeyDown:
            if (is_menu_key(WPARAM(e.a)) && !ui::any_document_visible()) {
                open_menu_bar();
                break;
            }
            ctx->ProcessMouseLeave();
            ctx->ProcessKeyDown(RmlWin32::ConvertKey(e.a), e.mods);
            break;
        case InputEvent::KeyUp:
            ctx->ProcessKeyUp(RmlWin32::ConvertKey(e.a), e.mods);
            break;
        case InputEvent::MouseMove:
            ctx->ProcessMouseMove(e.a, e.b, e.mods);
            break;
        case InputEvent::MouseDown:
            ctx->ProcessMouseButtonDown(e.a, e.mods);
            break;
        case InputEvent::MouseUp:
            ctx->ProcessMouseButtonUp(e.a, e.mods);
            break;
        case InputEvent::Wheel:
            ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, e.wheel), e.mods);
            break;
        case InputEvent::Leave:
            ctx->ProcessMouseLeave();
            ui::input::reset_input_state();
            break;
        }
    }
}

const char* shot_for_frame(unsigned long long frame) {
    for (const Shot& s : sShots)
        if (s.frame == frame) return s.path.c_str();
    return nullptr;
}

void script_keys_for_frame(unsigned long long frame) {
    for (const ScriptKey& k : sScriptKeys) {
        if (k.frame != frame) continue;
        push_event({InputEvent::KeyDown, k.vk, 0, 0});
        push_event({InputEvent::KeyUp, k.vk, 0, 0});
    }
}

void frame(void*, void* back_buffer, unsigned index, unsigned width, unsigned height) {
    ++sFrame;
    const bool script = !sScriptKeys.empty();
    const char* shot = sShots.empty() ? nullptr : shot_for_frame(sFrame);
    if (script) script_keys_for_frame(sFrame);

    // Fast path: nothing showing, nothing queued, no chord pressed.
    const unsigned mask = sChordMask;
    const bool chordDown = (g_pad0_buttons & mask) == mask;
    const bool chordEdge = chordDown && !sChordWasDown;
    sChordWasDown = chordDown;
    if (!sActive && !sStartScreen && !chordEdge && !shot && !sQueued.load(std::memory_order_acquire) &&
        !sOpenPending) {
        return;
    }

    sBufferW.store(int(width));
    sBufferH.store(int(height));
    if (!ensure_init(int(width), int(height))) {
        sStartScreen = false;
        sActive = false;
        std::lock_guard<std::mutex> lock(sQueueLock);
        sQueue.clear();
        sQueued.store(false);
        return;
    }
    sStartScreen = false;  // pushed now; from here on it is an ordinary document
    sOpenPending = false;
    const int chord = ui::settings::get_int("pad_menu_chord");
    sChordMask = ui::input::chord_mask(chord);

    if (!sActive) {
        // Waking: what is held now is not a press (the chord, or the key that opened the menu, is handled here).
        sActive = true;
        ui::input::reset_input_state();
        if (chordEdge) open_menu_bar();
    }

    process_queue();
    bool padNeutral = true;
    const ui::input::PadState pad = read_pads(padNeutral);
    ui::input::update_pad(pad, chord);
    ui::update();

    const bool visible = ui::any_document_visible();
    const auto now = clock_type::now();
    if (visible) sLastVisible = now;
    sCapturing.store(visible, std::memory_order_release);

    if (visible && !sHold) {
        sHold = true;
        cellPad_set_host_hold(1);
    } else if (!visible && sHold && padNeutral && keyboard_neutral()) {
        sHold = false;
        cellPad_set_host_hold(0);
    }

    ui::host::render(back_buffer, index, int(width), int(height), shot);

    auto* overlay = static_cast<ui::Overlay*>(ui::find_document(ui::DocumentScope::Overlay));
    const bool busy = overlay && overlay->busy();
    if (!visible && !sHold && !busy && now - sLastVisible > kGrace) {
        // Asleep until F1 / Esc or the chord. The chord may still read as held from before: only a new press counts.
        sActive = false;
        ui::input::reset_input_state();
        sChordWasDown = (g_pad0_buttons & sChordMask) == sChordMask;
    }
}

int vk_from_name(const std::string& name) {
    static const struct {
        const char* name;
        int vk;
    } kNames[] = {{"F1", VK_F1},       {"ESC", VK_ESCAPE}, {"UP", VK_UP},         {"DOWN", VK_DOWN},
                  {"LEFT", VK_LEFT},   {"RIGHT", VK_RIGHT}, {"ENTER", VK_RETURN}, {"PGUP", VK_PRIOR},
                  {"PGDN", VK_NEXT},   {"TAB", VK_TAB},    {"SPACE", VK_SPACE}};
    for (const auto& n : kNames)
        if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == std::string::npos) next = s.size();
        if (next > pos) out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

void parse_debug_knobs() {
    if (const char* v = std::getenv("DES_UI_SHOT"); v && *v) {
        for (const std::string& item : split(v, ';')) {
            const size_t comma = item.rfind(',');
            Shot s{item, 300};
            if (comma != std::string::npos) {
                s.path = item.substr(0, comma);
                s.frame = std::strtoull(item.c_str() + comma + 1, nullptr, 10);
            }
            if (!s.path.empty() && s.frame > 0) sShots.push_back(s);
        }
    }
    if (const char* v = std::getenv("DES_UI_KEYS"); v && *v) {
        for (const std::string& item : split(v, ',')) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            const int vk = vk_from_name(item.substr(colon + 1));
            const unsigned long long f = std::strtoull(item.c_str(), nullptr, 10);
            if (vk && f) sScriptKeys.push_back({f, vk});
        }
    }
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

namespace ui {
App& app() {
    static App s;
    return s;
}
}  // namespace ui

extern "C" void des_ui_early_init(void) {
    if (const char* v = std::getenv("DES_UI"); v && !std::strcmp(v, "0")) {
        return;
    }
    ui::settings::load_and_apply_env();
    parse_debug_knobs();
    sOpenPending = env_on("DES_UI_OPEN");
    sChordMask = ui::input::chord_mask(ui::settings::get_int("pad_menu_chord"));

    sStartScreen = env_on("DES_UI_START") ||
                   (ui::settings::get_int("show_start_screen") != 0 && !env_on("DES_SKIP_UI") && !ui::settings::scripted_run());

    ui::app().quit = [] {
        if (HWND hwnd = static_cast<HWND>(rsx_d3d12_hwnd())) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    };

    static const rsx_d3d12_overlay kOverlay = {nullptr, wndproc, frame};
    rsx_d3d12_set_overlay(&kOverlay);
}
