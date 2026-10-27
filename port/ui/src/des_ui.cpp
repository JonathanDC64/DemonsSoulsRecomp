// The wrapper UI's connection to the port (docs/32): installs the overlay on the ps3recomp D3D12 backend and runs the
// UI from its two hooks. Written for Demon's Souls; the shape follows FFCCEOT-Recomp's main loop (SDL events in, one
// ui::update and draw per frame), with the window procedure and the frame hook in place of SDL.
//
//   wndproc (the window's message thread): F1 is always the UI's (Esc is the game's Start in the keyboard layout,
//     settings.cpp). While a document is up, the keyboard (except F9-F12, the backend's capture keys) and the mouse
//     are queued and kept from the game window. Mouse look (mouse.cpp) sees each message first, holds the cursor in
//     the window while the game has the mouse, and passes the game the mouse's buttons and wheel meanwhile.
//   frame (the render thread, once per presented frame, before Present): drains the queue into RmlUi, reads the
//     gamepads (they drive RmlUi only while a document is up), updates and draws. When nothing is showing (no
//     document, no pop-up) it returns after a couple of loads: no RmlUi work, no pad reads, no GPU work. The
//     gamepad's menu chord is then seen in g_pad0_buttons, the game's own pad 0 read.
//
// The game does not see the pads or the keyboard while a document is up (cellPad_set_host_hold), and gets them back
// only once every key and button is released, so the press that closed the menu does not reach the game.
#include "des_ui.h"

#include "app.hpp"
#include "crash_report.hpp"
#include "game_menu.hpp"
#include "host.hpp"
#include "input.hpp"
#include "keys.hpp"
#include "menu_bar.hpp"
#include "mouse.hpp"
#include "net_notices.hpp"
#include "osk_window.hpp"
#include "overlay.hpp"
#include "platform.hpp"
#include "portable.hpp"
#include "prelaunch.hpp"
#include "prework_modal.hpp"
#include "privacy.hpp"
#include "privacy_window.hpp"
#include "report.hpp"
#include "settings.hpp"
#include "settings_window.hpp"
#include "setup_engine.hpp"
#include "setup_screens.hpp"
#include "setup_window.hpp"
#include "players_window.hpp"
#include "trophies_window.hpp"
#include "trophy_toasts.hpp"
#include "ui.hpp"
#include "updates.hpp"
#include "update_window.hpp"
#include "version.hpp"

#ifdef _WIN32
#include <xinput.h>  // (read_xinput)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ---- What the UI uses of the runtime (declared here: des_ui does not take ps3recomp's include paths) ----
extern "C" {
typedef struct rsx_d3d12_overlay {
    void* user;
    int (*wndproc)(void* user, void* hwnd, unsigned msg, unsigned long long wp, long long lp, long long* result);
    void (*frame)(void* user, void* back_buffer, unsigned index, unsigned width, unsigned height);
    void (*release)(void* user);
} rsx_d3d12_overlay;
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay);
void rsx_d3d12_overlay_repaint(int on);
int rsx_d3d12_hdr(float* ui_scale, int* srgb);  // HDR output: the back buffers are scRGB, the menus' scale
void rsx_d3d12_overlay_wake(void);
long long ps3_ms_since_frame(void);  // boot_main.cpp: since the game's last frame, -1 before the first
unsigned ppu_boot_frames_presented(void);  // boot_main.cpp: the game's frames so far
void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: the debug console's setenv
int rsx_d3d12_borderless(void);
void* rsx_d3d12_device(void);
long rsx_d3d12_device_lost(long long* qpc);  // 0, or why the device was lost (GetDeviceRemovedReason)
void* rsx_d3d12_queue(void);
void* rsx_d3d12_hwnd(void);
void rsx_d3d12_post_message(unsigned msg, unsigned long long wp, long long lp);
void rsx_d3d12_set_borderless(int on);
int rsx_d3d12_borderless(void);

void cellPad_set_host_hold(int hold);
extern volatile unsigned g_pad0_buttons;  // pad 0 as the game last read it (CELL_PAD_CTRL_* layout)

typedef struct {
    unsigned short buttons;    // CELL_PAD_CTRL_*
    unsigned char lx, ly, rx, ry;  // 0-255, centre 128, up = 0
    unsigned char l2, r2;
} PadHidSonyState;
int pad_hid_sony_poll(PadHidSonyState* out, int max);
int cellPad_host_pads_on(void);  // cellPad.h: 0 while the game ignores the pads (PAD_FOCUS_ONLY in the background)

typedef struct CellOskDialogHost {  // cellOskDialog.h: the game's on-screen keyboard, shown by the UI
    void (*open)(const unsigned short* prompt, const unsigned short* text, unsigned limit);
    void (*close)(void);
} CellOskDialogHost;
void cellOskDialog_set_host(const CellOskDialogHost* host);
void cellOskDialog_host_finish(int ok, const unsigned short* text, unsigned n);
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

// The game has stopped (no new frame for this long): the watch thread then reads the gamepad's menu chord itself,
// since the game no longer reads the pad, and a menu key the render thread does not answer opens the hang dialog.
constexpr long long kStoppedMs = 2000;
// After this long without a new frame the overlay says how to report it.
constexpr long long kHangNoticeMs = 15000;
constexpr auto kHangNoticeDuration = std::chrono::seconds(30);
// "Playing offline": a Custom server that was not chosen on this PC (settings::custom_chosen_here).
constexpr auto kCustomServerNoticeDuration = std::chrono::seconds(15);

struct InputEvent {
    enum Type { KeyDown, KeyUp, Char, MouseMove, MouseDown, MouseUp, Wheel, Leave } type;
    int a = 0;  // key (VK) / character / button / x
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
bool sSetupOnly = false;  // setup's screens alone, no game (des_ui_launch)
std::atomic<bool> sStartUp{false};
// The window's close button (and Alt+F4) asks first, as Quit does; a test run's window closes at once.
bool sCloseAsks = false;
std::atomic<bool> sCloseRequested{false};  // set on the window thread, the question opened in frame
std::atomic<bool> sQuitConfirmed{false};   // Quit Game was chosen: the next WM_CLOSE goes through  // the start screen is to show or showing (des_ui_start_screen_up, any thread)
bool sDocumentsPushed = false;
bool sActive = false;
bool sPadIdle = false;  // awake with no document up: the pads are not fed to RmlUi (frame)
bool sHold = false;
bool sChordWasDown = false;
clock_type::time_point sLastVisible{};
bool sRepaint = false;  // rsx_d3d12_overlay_repaint is on
unsigned long long sFrame = 0;
// For the watch thread (watch_thread): frame's calls, and the last menu key or chord (when, and frame's count then).
std::atomic<unsigned long long> sFramesRun{0};
std::atomic<unsigned long long> sWakeAt{0}, sWakeMark{0};
std::atomic<bool> sWakeByUser{false};       // that wake was the player's (not the hang notice's)
std::atomic<bool> sHangNotice{false};      // the watch thread asks frame for the hang notice
std::atomic<bool> sCustomServerNotice{false};  // "Playing offline" is due (des_ui_early_init); frame shows it
clock_type::time_point sHangNoticeUntil{};  // repaints stay on while it shows (the game draws no frames)
int sReportCountdown = 0;     // "Report an issue": frames until the game's picture is taken (report.cpp)
bool sInitFailed = false;
bool sOpenPending = false;     // DES_UI_OPEN, until the documents exist
std::atomic<unsigned> sChordMask{kL3 | kR3};  // the menu chord, refreshed while the UI is awake

// DES_UI_SHOT=path[,frame][;path[,frame]...]: write the back buffer after the UI draws, at that presented frame.
struct Shot {
    std::string path;
    unsigned long long frame;
};
std::vector<Shot> sShots;
// DES_UI_SHOT_NOW=path (a test, with the debug console's setenv): the next frame, likewise, then the variable is
// cleared. For a test that clicks first and looks after, and at the lock screen, where a capture of the window comes out
// blank. The watch thread takes it; the frame writes it.
std::mutex sShotNowLock;
std::string sShotNow;

// DES_UI_KEYS=frame:KEY[,frame:KEY...]: press a key at a presented frame (a test drives the menus with it). The key
// OSK opens the keyboard as the game would, and a digit types it (a number field).
constexpr int kScriptOsk = -1;
constexpr int kScriptChar = 0x10000;  // + the character
struct ScriptKey {
    unsigned long long frame;
    int vk;
};
std::vector<ScriptKey> sScriptKeys;
int vk_from_name(const std::string& name);
std::vector<std::string> split(const std::string& s, char sep);

// The game's on-screen keyboard (cellOskDialog_set_host). The game opens and closes it on its own threads; frame
// shows and closes the window.
struct OskRequest {
    bool open = false;   // show it with these
    bool close = false;  // the game stopped it first
    std::u16string prompt, text;
    unsigned limit = 0;
};
std::mutex sOskLock;
OskRequest sOsk;
std::atomic<bool> sOskPending{false};

std::u16string osk_string(const unsigned short* s) {
    std::u16string out;
    for (; s && *s; ++s) out.push_back(char16_t(*s));
    return out;
}

void osk_open(const unsigned short* prompt, const unsigned short* text, unsigned limit) {
    std::lock_guard<std::mutex> lock(sOskLock);
    sOsk = {true, false, osk_string(prompt), osk_string(text), limit ? limit : 1};
    sOskPending.store(true, std::memory_order_release);
}

void osk_close() {
    std::lock_guard<std::mutex> lock(sOskLock);
    sOsk.open = false;
    sOsk.close = true;
    sOskPending.store(true, std::memory_order_release);
}

std::string osk_utf8(const std::u16string& s) {
    return ui::utf16_to_utf8(s);
}

// In frame: show or close the keyboard as the game asked. Without the UI (it failed to start) the game gets its
// initial text back rather than waiting for good.
void apply_osk_request(bool ui_up) {
    OskRequest r;
    {
        std::lock_guard<std::mutex> lock(sOskLock);
        r = std::exchange(sOsk, {});
    }
    if (r.close) {
        if (ui::OskWindow* osk = ui::active_osk()) osk->close_silently();
    }
    if (!r.open) return;
    if (!ui_up) {
        cellOskDialog_host_finish(1, reinterpret_cast<const unsigned short*>(r.text.c_str()), unsigned(r.text.size()));
        return;
    }
    if (ui::OskWindow* osk = ui::active_osk()) osk->close_silently();
    ui::push_document(std::make_unique<ui::OskWindow>(ui::OskWindow::Props{
        .prompt = osk_utf8(r.prompt),
        .text = r.text,
        .limit = r.limit,
        .onDone = [](bool ok, const std::u16string& text) {
            std::fprintf(stderr, "[ui] keyboard: %s, %zu characters\n", ok ? "Done" : "Cancel", text.size());
            cellOskDialog_host_finish(ok ? 1 : 0, reinterpret_cast<const unsigned short*>(text.c_str()),
                                      unsigned(text.size()));
        },
    }));
}

void push_event(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(sQueueLock);
    sQueue.push_back(e);
    sQueued.store(true, std::memory_order_release);
}

// A menu key or the chord: frame runs soon even when the game has stopped presenting (the runtime draws it over the
// last frame shown), and the watch thread can tell whether the render thread answered.
void wake_overlay(bool user = true) {
    sWakeByUser.store(user);
    sWakeMark.store(sFramesRun.load());
    sWakeAt.store(ui::now_ms());
    rsx_d3d12_overlay_wake();
}

// Opens the menu over the game. Not Esc: in play it is the game's Start (the keyboard layout, settings.cpp), and it
// reaches the UI only while a document is up, where it goes back.
bool is_menu_key(WPARAM vk) {
    return vk == VK_F1;
}

// The back buffer is stretched over the client area: mouse positions are in back buffer pixels.
void map_mouse(HWND hwnd, LPARAM lp, int& x, int& y) {
    int w = 1, h = 1;
    ui::mouse::client_size(hwnd, w, h);
    const int cw = std::max(1, w), ch = std::max(1, h);
    x = GET_X_LPARAM(lp) * sBufferW.load() / cw;
    y = GET_Y_LPARAM(lp) * sBufferH.load() / ch;
}

int wndproc(void*, void* hwnd_, unsigned msg, unsigned long long wp, long long lp, long long* result) {
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (ui::mouse::wndproc(hwnd_, msg, wp, lp, result)) return 1;
    const bool capturing = sCapturing.load(std::memory_order_acquire);
    switch (msg) {
    case WM_CLOSE:
        if (ui::updates::close_held()) {  // (the update is swapping the program files: a second)
            *result = 0;
            return 1;
        }
        if (!sCloseAsks || sQuitConfirmed.load()) return 0;
        sCloseRequested.store(true);
        *result = 0;
        return 1;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const bool down = msg == WM_KEYDOWN;
        if (is_menu_key(wp) || (capturing && !(wp >= VK_F9 && wp <= VK_F12))) {
            if (!(down && (lp & (1LL << 30)) && is_menu_key(wp) && !capturing)) {  // no auto-repeat opening
                push_event({down ? InputEvent::KeyDown : InputEvent::KeyUp, int(wp), 0, ui::keys::modifiers()});
                if (down && is_menu_key(wp) && !capturing) wake_overlay();
            }
            *result = 0;
            return 1;
        }
        if (down && ui::game_menu::active()) ui::game_menu::on_key();  // (the game's: the pointer hides)
        return 0;
    }
    case WM_CHAR:  // typing, for the on-screen keyboard
        if (!capturing) return 0;
        push_event({InputEvent::Char, int(wp)});
        *result = 0;
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        if (!capturing && ui::game_menu::active()) {  // a menu of the game: the pointer and its clicks (docs/114)
            int x, y;
            map_mouse(hwnd, lp, x, y);
            if (msg == WM_MOUSEMOVE) {
                ui::game_menu::on_move(x, y);
                return 0;
            }
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            ui::game_menu::on_button((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 0
                                     : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                                                                                      : 2,
                                     down, x, y);
            *result = 0;
            return 1;
        }
        if (!capturing) return 0;
        int x, y;
        map_mouse(hwnd, lp, x, y);
        InputEvent e{InputEvent::MouseMove, x, y, ui::keys::modifiers()};
        push_event(e);
        if (msg != WM_MOUSEMOVE) {
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 0
                               : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                                                                                : 2;
            push_event({down ? InputEvent::MouseDown : InputEvent::MouseUp, button, 0, e.mods});
            ui::mouse::hold_buttons(hwnd, down);
        }
        *result = 0;
        return 1;
    }
    case WM_MOUSEWHEEL: {
        if (!capturing && ui::game_menu::active()) {  // (its position is the screen's: the pointer's last one counts)
            ui::game_menu::on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
            *result = 0;
            return 1;
        }
        if (!capturing) return 0;
        InputEvent e{InputEvent::Wheel, 0, 0, ui::keys::modifiers()};
        e.wheel = -float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
        push_event(e);
        *result = 0;
        return 1;
    }
    case WM_MOUSELEAVE:
    case WM_KILLFOCUS:
        if (capturing) push_event({InputEvent::Leave});
        else if (ui::game_menu::active()) ui::game_menu::on_leave();
        return 0;
    default:
        return 0;
    }
}

// ---- Gamepads: XInput and the Sony HID reader, merged (the same readers the game's pad uses; off Windows the reader
// gives SDL's pads) ----

float pick(float a, float b) {
    return std::abs(a) >= std::abs(b) ? a : b;
}

#ifdef _WIN32
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

// XInput's pads, into `pad`. Polls XInput slots only while a slot answers: a missing pad's XInputGetState is
// slow, so empty slots are retried once a second.
void read_xinput(ui::input::PadState& pad, bool& neutral) {
    static clock_type::time_point sNextScan[XUSER_MAX_COUNT]{};
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
}
#else
void read_xinput(ui::input::PadState&, bool&) {}  // (off Windows SDL's pads come through the HID reader)
#endif

// The pads: XInput's and the HID reader's, merged.
ui::input::PadState read_pads(bool& neutral) {
    ui::input::PadState pad;
    neutral = true;
    if (ui::settings::no_user_input()) return pad;  // PS3_NO_USER_INPUT: no pad of the user's is read
    if (!cellPad_host_pads_on()) return pad;         // PAD_FOCUS_ONLY, with the window in the background
    read_xinput(pad, neutral);
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

// No key or mouse button is held (the keyboard pad's keys can be any, PAD_KEYS): the game gets its input back only
// then.
bool keyboard_neutral() {
    if (ui::settings::no_user_input()) return true;  // PS3_NO_USER_INPUT: a key held is the user's
    for (int vk = 1; vk < 0xFF; vk++)
        if (ui::keys::key_down(vk)) return false;
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

// The keyboard as the game opens it (cellOskDialog_set_host), for DES_UI_OPEN=osk and the OSK test key.
void open_test_osk() {
    static const unsigned short kPrompt[] = {'T', 'e', 's', 't', 0}, kText[] = {'A', 'l', 'd', 'i', 'a', 0};
    osk_open(kPrompt, kText, 16);
}

// Play on setup alone: the game in a new process of this program (crash_report::start_again: with the environment
// this one started with, so the game reads what setup stored in settings.ini), then this one closes.
void play_from_setup() {
    if (!ui::crash_report::start_again()) std::fprintf(stderr, "[setup] play: the game could not be started\n");
    else std::fprintf(stderr, "[setup] play: the game starts in a new process\n");
    ui::app().quit();
}

// Setup's screens: the start screen, or setup-run's build at once. On setup alone their Play starts the game; over a
// running game (a runtime without the launch hook) it only closes them.
void push_setup(const char* open) {
    const std::function<void()> onPlay = sSetupOnly ? std::function<void()>(play_from_setup) : nullptr;
    if (open && !std::strcmp(open, "setup-run")) {
        ui::push_document(std::make_unique<ui::SetupWindow>(
            ui::SetupWindow::Props{.request = ui::setup::request_from_env(), .onPlay = onPlay}));
        return;
    }
    ui::push_document(std::make_unique<ui::SetupStartWindow>(
        ui::SetupStartWindow::Props{.request = ui::setup::request_from_env(), .onPlay = onPlay}));
    // The update, or going back, that the game's start screen asked for (updates.hpp's restart_into), over setup's
    // start screen, which shows once it closes.
    const bool update = open && !std::strcmp(open, "update-run"), back = open && !std::strcmp(open, "update-back");
    if (update || back) {
        const char* to = std::getenv("DES_UPDATE_TO");
        ui::updates::Release release;
        release.version = to ? to : "";
        ui::push_document(std::make_unique<ui::UpdateWindow>(ui::UpdateWindow::Props{
            .open = back ? ui::UpdateWindow::Open::Back : ui::UpdateWindow::Open::Update, .release = release}));
    }
}

// DES_UI_OPEN's values that open setup's screens alone (des_ui_launch): no game below them.
bool setup_only_open(const char* open) {
    return open && (!std::strcmp(open, "setup") || !std::strcmp(open, "setup-run") ||
                    !std::strcmp(open, "update-run") || !std::strcmp(open, "update-back"));
}

void push_documents() {
    if (sDocumentsPushed) return;
    sDocumentsPushed = true;
    ui::push_document(std::make_unique<ui::Overlay>(), true, true);
    // The update check (updates.hpp), off this thread: what it finds shows on the start screens and in Settings, About.
    ui::updates::set_over_game(!sSetupOnly);
    if (const char* open = std::getenv("DES_UI_OPEN");
        ui::updates::wanted() && !(open && !std::strncmp(open, "update-", 7)))
        ui::updates::start_check();
    if (sSetupOnly) {  // (no game: no menu and none of the game's screens)
        push_setup(std::getenv("DES_UI_OPEN"));
        return;
    }
    ui::push_document(std::make_unique<ui::MenuBar>(), false);
    if (sStartScreen) ui::push_document(std::make_unique<ui::Prelaunch>());

    // DES_UI_OPEN=menu | settings | trophies | players | privacy | osk | setup | setup-run: open one at startup (for
    // screenshots and tests; privacy is the note as Settings > Network shows it).
    // setup is setup's start screen over the game (docs/88: its own window comes with the split host), with
    // DES_SETUP_GAME's folders to begin with; setup-run runs the setup engine at once, on those folders.
    if (const char* open = std::getenv("DES_UI_OPEN"); open && *open) {
        if (!std::strcmp(open, "menu")) open_menu_bar();
        else if (!std::strcmp(open, "settings")) ui::push_document(std::make_unique<ui::SettingsWindow>());
        else if (!std::strcmp(open, "trophies")) ui::push_document(std::make_unique<ui::TrophiesWindow>());
        else if (!std::strcmp(open, "players")) ui::push_document(std::make_unique<ui::PlayersWindow>());
        else if (!std::strcmp(open, "privacy"))
            ui::push_document(std::make_unique<ui::PrivacyWindow>(ui::PrivacyWindow::Mode::Read));
        else if (!std::strcmp(open, "osk")) open_test_osk();
        else if (!std::strcmp(open, "setup") || !std::strcmp(open, "setup-run")) push_setup(open);
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
        if (ui::OskWindow* osk = ui::active_osk()) {  // the on-screen keyboard is up: typing goes into it
            if (e.type == InputEvent::Char) {
                osk->on_char(unsigned(e.a));
                continue;
            }
            if (e.type == InputEvent::KeyDown && osk->on_key(e.a)) continue;
        }
        switch (e.type) {
        case InputEvent::KeyDown:
            if (ui::input::binding_capture()) {  // a key for a binding (controls_window.cpp)
                ui::input::capture_key(e.a);
                break;
            }
            if (is_menu_key(WPARAM(e.a)) && !ui::any_document_visible()) {
                open_menu_bar();
                break;
            }
            ctx->ProcessMouseLeave();
            ctx->ProcessKeyDown(ui::keys::to_rml(e.a), e.mods);
            break;
        case InputEvent::KeyUp:
            if (ui::input::binding_capture()) break;
            ctx->ProcessKeyUp(ui::keys::to_rml(e.a), e.mods);
            break;
        case InputEvent::Char:
            // Typing into a text field (Custom frame rate's): printable characters only, as RmlUi's own Win32
            // backend passes them; Enter, Backspace and Escape arrive as key presses.
            if (e.a >= 0x20 && e.a != 0x7F && !(e.a >= 0xD800 && e.a <= 0xDFFF)) {
                ctx->ProcessTextInput(Rml::Character(e.a));
            }
            break;
        case InputEvent::MouseMove:
            ctx->ProcessMouseMove(e.a, e.b, e.mods);
            ui::host::set_pointer(e.a, e.b, true);  // (the sword cursor's place)
            break;
        case InputEvent::MouseDown:
            if (ui::input::binding_capture()) break;  // a mouse button for a binding (controls_window.cpp reads it)
            ctx->ProcessMouseButtonDown(e.a, e.mods);
            break;
        case InputEvent::MouseUp:
            if (ui::input::binding_capture()) break;
            ctx->ProcessMouseButtonUp(e.a, e.mods);
            break;
        case InputEvent::Wheel:
            if (ui::input::binding_capture()) {  // a wheel notch for a binding
                ui::input::capture_wheel(e.wheel < 0.0f ? 1 : -1);
                break;
            }
            ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, e.wheel), e.mods);
            break;
        case InputEvent::Leave:
            ctx->ProcessMouseLeave();
            ui::input::reset_input_state();
            ui::host::set_pointer(0, 0, false);
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
        if (k.vk == kScriptOsk) {
            open_test_osk();
            continue;
        }
        if (k.vk >= kScriptChar) {
            push_event({InputEvent::Char, k.vk - kScriptChar});
            continue;
        }
        push_event({InputEvent::KeyDown, k.vk, 0, 0});
        push_event({InputEvent::KeyUp, k.vk, 0, 0});
    }
}

// "The game is not responding" (the watch thread asks for it): a toast, with repaints on while it shows, since the
// game draws no frames to show it on.
void push_hang_notice() {
    const long long ms = std::max(0LL, ps3_ms_since_frame());
    ui::push_toast({
        .type = "warning",
        .title = "The game is not responding",
        .content = "No new frame for " + std::to_string(ms / 1000) + " s. Press F1 or " +
                   ui::input::chord_label(ui::settings::get_int("pad_menu_chord")) +
                   " to open the menu, then choose Report an issue.",
        .duration = kHangNoticeDuration,
    });
    sHangNoticeUntil = clock_type::now() + kHangNoticeDuration + std::chrono::seconds(1);
    std::fprintf(stderr, "[ui] no new frame for %lld s: the hang notice is up\n", ms / 1000);
}

// "Playing offline": the settings name a Custom server that was not chosen on this PC (settings::custom_chosen_here).
void push_custom_server_notice() {
    ui::push_toast({
        .type = "warning",
        .title = "Playing offline",
        .content = "The settings name a custom server (" + ui::settings::get("DES_NET_CUSTOM") +
                   ") that was not chosen on this PC. To play on it, press Custom again in Settings > Network > "
                   "Server, then restart the game.",
        .duration = kCustomServerNoticeDuration,
    });
    std::fprintf(stderr, "[ui] the custom server notice is up\n");
}

// The game's frames each second, the last minute, oldest first (the watch thread counts them), for reports.
std::mutex sRateLock;
std::vector<unsigned> sRates;

const auto sStarted = std::chrono::steady_clock::now();  // (static initialisation: the start of the process)

// What a report says about this run (report.cpp's game.txt). The render thread, in frame.
std::string session_text(unsigned width, unsigned height) {
    std::string s;
    char line[256];
    {
        const unsigned long long sec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - sStarted).count();
        std::snprintf(line, sizeof line, "Running for: %llu h %02llu min %02llu s\n", sec / 3600, sec / 60 % 60, sec % 60);
        s += line;
    }
    const long long since = ps3_ms_since_frame();
    std::snprintf(line, sizeof line, "Game frames: %u", ppu_boot_frames_presented());
    s += line;
    if (since < 0) s += " (none yet)\n";
    else if (since >= kStoppedMs) {
        std::snprintf(line, sizeof line, " (no new frame for %.1f s: the game had stopped)\n", since / 1000.0);
        s += line;
    } else s += "\n";
    {
        std::lock_guard<std::mutex> lock(sRateLock);
        if (!sRates.empty()) {
            s += "Game frames each second, the last " + std::to_string(sRates.size()) + " s (oldest first):";
            for (unsigned r : sRates) s += " " + std::to_string(r);
            s += "\n";
        }
    }
    std::snprintf(line, sizeof line, "Overlay frames: %llu\n", sFrame);
    s += line;
    const unsigned dpi = ui::host::window_dpi();
    std::snprintf(line, sizeof line, "Window: %s, %ux%u, %u dpi (%u%%)\n", rsx_d3d12_borderless() ? "fullscreen" : "window",
                  width, height, dpi, dpi ? dpi * 100 / 96 : 0);
    s += line;
    return s;
}

// Why the graphics device was lost (rsx_d3d12_device_lost), for the player.
std::string lost_text(long reason) {
    switch (static_cast<unsigned long>(reason)) {
    case 0x887A0006ul: return "The graphics card stopped responding, and Windows reset its driver";
    case 0x887A0005ul: return "The graphics card was removed, or its driver was updated or restarted";
    case 0x887A0007ul: return "The graphics driver was reset after a bad command";
    case 0x887A0020ul: return "The graphics driver had an internal error";
    case 0x8007000Eul: return "The graphics card ran out of memory";
    default: {
        char text[64];
        std::snprintf(text, sizeof text, "The graphics device was lost (0x%08lX)", static_cast<unsigned long>(reason));
        return text;
    }
    }
}

// Every quarter second, on a thread of its own: what the render thread cannot do once the game has stopped. The game
// no longer reads the pad, so the menu chord is read here. A menu key or chord that frame has not answered within
// kStoppedMs means the render thread is stuck too: the hang dialog (a second process) takes over. After kHangNoticeMs
// without a new frame the overlay is asked to say how to report it (and if it cannot, the dialog opens). It also
// counts the game's frames each second for reports. A lost graphics device (a driver reset) opens the dialog in its
// lost form: the game runs on, so no frame stops, but nothing can be drawn, the menu included (issue #205).
void watch_thread() {
    bool chordWasDown = true;  // (a chord held from before is not a press)
    bool noticed = false;
    bool dialogShown = false;  // the hang dialog opened in this stop: again only when the player asks
    bool lostShown = false;
    unsigned lastFrames = ppu_boot_frames_presented(), ticks = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!lostShown) {
            if (const long reason = rsx_d3d12_device_lost(nullptr)) {
                lostShown = true;
                std::fprintf(stderr, "[ui] the graphics device was lost (0x%08lX): the dialog\n", (unsigned long)reason);
                ui::crash_report::report_graphics_lost(lost_text(reason));
            }
        }
        if (++ticks % 4 == 0) {
            const unsigned frames = ppu_boot_frames_presented();
            std::lock_guard<std::mutex> lock(sRateLock);
            sRates.push_back(frames - lastFrames);
            if (sRates.size() > 60) sRates.erase(sRates.begin());
            lastFrames = frames;
        }
        const long long since = ps3_ms_since_frame();
        const bool stopped = since >= kStoppedMs;
        if (!stopped) noticed = dialogShown = false;
        if (stopped && !sCapturing.load()) {  // (the menu is not up: the chord opens it)
            bool neutral = true;
            const unsigned mask = sChordMask.load();
            const bool down = (read_pads(neutral).buttons & mask) == mask;
            if (down && !chordWasDown) {
                std::fprintf(stderr, "[ui] the menu chord, with no new frame for %lld s\n", since / 1000);
                push_event({InputEvent::KeyDown, VK_F1});
                push_event({InputEvent::KeyUp, VK_F1});
                wake_overlay();
            }
            chordWasDown = down;
        } else {
            chordWasDown = true;
        }
        // DES_UI_PRESS=KEY (a test, with the debug console's setenv): the key is pressed as the wndproc presses it,
        // in a running game or a stopped one, and the variable is cleared. (PS3_NO_USER_INPUT keeps the keys a test
        // posts to the window from the overlay.)
        if (const char* v = std::getenv("DES_UI_PRESS"); v && *v) {
            const int vk = vk_from_name(v);
            ps3_setenv("DES_UI_PRESS", "");
            if (vk > 0) {
                push_event({InputEvent::KeyDown, vk});
                push_event({InputEvent::KeyUp, vk});
                if (is_menu_key(WPARAM(vk)) && !sCapturing.load()) wake_overlay();
            }
        }
        if (const char* v = std::getenv("DES_UI_SHOT_NOW"); v && *v) {
            std::lock_guard<std::mutex> lock(sShotNowLock);
            sShotNow = v;
            ps3_setenv("DES_UI_SHOT_NOW", "");
        }
        // DES_UI_MOUSE=x,y[,down|up][;x,y[,down|up]...] (a test, likewise): the mouse moves to each point (back
        // buffer pixels) in turn, the left button going down or up there; one setenv is one drag. Over a menu of the
        // game (no document up) it is that menu's, as the window's messages would be, with rdown and rup for the
        // right button and wheelup and wheeldown for a notch.
        if (const char* v = std::getenv("DES_UI_MOUSE"); v && *v) {
            const std::string steps = v;
            ps3_setenv("DES_UI_MOUSE", "");
            const bool game = !sCapturing.load() && ui::game_menu::open();
            for (const std::string& step : split(steps, ';')) {
                int x = 0, y = 0;
                char act[12] = {};
                const int got = std::sscanf(step.c_str(), "%d,%d,%11s", &x, &y, act);
                if (got < 2) continue;
                const std::string a = got == 3 ? act : "";
                if (game) {
                    ui::game_menu::on_move(x, y);
                    if (a == "down" || a == "up") ui::game_menu::on_button(0, a == "down", x, y);
                    if (a == "rdown" || a == "rup") ui::game_menu::on_button(1, a == "rdown", x, y);
                    if (a == "wheelup" || a == "wheeldown") ui::game_menu::on_wheel(a == "wheelup" ? 120 : -120);
                    continue;
                }
                push_event({InputEvent::MouseMove, x, y});
                if (a == "down") push_event({InputEvent::MouseDown, 0});
                if (a == "up") push_event({InputEvent::MouseUp, 0});
            }
        }
        if (const unsigned long long at = sWakeAt.load()) {
            if (sFramesRun.load() != sWakeMark.load()) {
                sWakeAt.store(0);
            } else if (ui::now_ms() - at >= unsigned(kStoppedMs)) {
                sWakeAt.store(0);
                if (stopped && (sWakeByUser.load() || !dialogShown) && !ui::crash_report::hang_dialog_open()) {
                    std::fprintf(stderr, "[ui] no new frame for %lld s, and the menu cannot open: the hang dialog\n",
                                 since / 1000);
                    dialogShown = true;
                    ui::crash_report::report_hang(since);
                }
            }
        }
        if (stopped && since >= kHangNoticeMs && !noticed && !sCapturing.load()) {
            noticed = true;
            sHangNotice.store(true);
            wake_overlay(false);
        }
    }
}

// Display mode (DES_FULLSCREEN) and the window follow each other. A change of the setting asks the window thread
// for it; a change of the window that nobody here asked for (Alt+Enter, handled on the window thread) becomes the
// setting, and is saved with it. Borderless fullscreen, not exclusive: the overlay and Alt+Tab work as windowed.
void sync_fullscreen() {
    static int sSeen = -1;   // the window's state as last seen here
    static int sWant = -1;   // the setting as last acted on
    static int sAsked = -1;  // what was asked of the window, until it shows it
    static int sWaited = 0;  // frames since: a window that cannot switch (no monitor information) is given up on
    const int now = rsx_d3d12_borderless();
    if (sAsked >= 0) {
        if (now != sAsked && ++sWaited < 240) return;  // (the window thread has not got to it yet)
        sAsked = -1;
        sSeen = now;
    }
    const bool pinned = ui::settings::pinned("DES_FULLSCREEN");
    const std::string v = pinned ? ui::settings::pinned_value("DES_FULLSCREEN") : ui::settings::get("DES_FULLSCREEN");
    const int want = !v.empty() && v != "0";
    if (sSeen >= 0 && now != sSeen) {  // Alt+Enter
        sSeen = now;
        std::fprintf(stderr, "[ui] display mode: %s (Alt+Enter)%s\n", now ? "fullscreen" : "window",
                     pinned ? "; DES_FULLSCREEN is set in the environment, so the setting stays" : "");
        if (pinned) return;  // (set on the command line: Alt+Enter still switches, the setting stays)
        sWant = now;
        if (now != want) ui::settings::set("DES_FULLSCREEN", now ? "1" : "0");
        return;
    }
    sSeen = now;
    if (want != sWant) {
        sWant = want;
        if (now != want) {
            sAsked = want;
            sWaited = 0;
            std::fprintf(stderr, "[ui] display mode: %s (setting)\n", want ? "fullscreen" : "window");
            rsx_d3d12_set_borderless(want);
        }
    }
}

void frame(void*, void* back_buffer, unsigned index, unsigned width, unsigned height) {
    ++sFrame;
    sFramesRun.fetch_add(1, std::memory_order_relaxed);
    sync_fullscreen();
    ui::mouse::on_frame();  // (mouse look: is the game's camera taking the mouse?)
    if (ui::report::frame_hook(sFrame)) sReportCountdown = 4;  // DES_REPORT_AT_FRAME (and the crash test knobs)
    const bool script = !sScriptKeys.empty();
    std::string shotNow;  // DES_UI_SHOT_NOW's, for this frame
    {
        std::lock_guard<std::mutex> lock(sShotNowLock);
        shotNow.swap(sShotNow);
    }
    const char* shot = !shotNow.empty() ? shotNow.c_str() : sShots.empty() ? nullptr : shot_for_frame(sFrame);
    if (script) script_keys_for_frame(sFrame);
    ui::trophies::poll_test_knob();  // DES_TROPHY_TEST
    ui::net_notices::poll();          // what the online sign-in has to tell the player

    // A menu of the game with the pointer over it (game_menu.cpp) keeps the UI running too.
    const bool gameMenu = ui::game_menu::poll();
    // Fast path: nothing showing, nothing queued (input, a trophy pop-up), no chord pressed.
    const unsigned mask = sChordMask;
    const bool chordDown = (g_pad0_buttons & mask) == mask;
    const bool chordEdge = chordDown && !sChordWasDown;
    sChordWasDown = chordDown;
    // The startup work (des_prework.cpp) runs before the game, with its window over the blank frames. So does the
    // privacy note's question (des_ui_privacy_wait), first.
    static bool sPreworkShown = false;
    static bool sPrivacyShown = false;
    const bool prework = ui::prework_running();
    const bool privacy = ui::privacy::asking() && !sPrivacyShown;
    if (!sActive && !sStartScreen && !gameMenu && !chordEdge && !shot && !sQueued.load(std::memory_order_acquire) &&
        !sOpenPending && !sReportCountdown && !(prework && !sPreworkShown) && !privacy && !sCloseRequested.load() &&
        !sOskPending.load(std::memory_order_acquire) && !ui::trophies::pending() && !ui::net_notices::pending() &&
        !sHangNotice.load(std::memory_order_relaxed) &&
        !sCustomServerNotice.load(std::memory_order_relaxed)) {
        return;
    }

    sBufferW.store(int(width));
    sBufferH.store(int(height));
    if (!ensure_init(int(width), int(height))) {
        sStartScreen = false;
        sStartUp.store(false);
        ui::mouse::set_ui_open(false);
        if (sCloseRequested.exchange(false) && ui::app().quit) ui::app().quit();  // nothing to ask with
        if (sOskPending.exchange(false)) apply_osk_request(false);
        ui::trophies::discard();  // (the unlocks are saved; only the pop-ups are lost)
        sHangNotice.store(false);
        sCustomServerNotice.store(false);
        if (ui::privacy::asking()) ui::privacy::answer(false);  // (no way to ask: offline, and the start goes on)
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
    if (sHangNotice.exchange(false)) push_hang_notice();
    if (sCustomServerNotice.exchange(false)) push_custom_server_notice();

    if (privacy) {
        sPrivacyShown = true;
        ui::push_document(std::make_unique<ui::PrivacyWindow>(ui::PrivacyWindow::Mode::Ask));
    }
    if (prework && !sPreworkShown) {
        sPreworkShown = true;
        ui::push_document(std::make_unique<ui::PreworkModal>());
    }
    if (sCloseRequested.exchange(false)) ui::request_quit_confirm();
    if (sOskPending.exchange(false)) apply_osk_request(true);
    process_queue();
    bool padNeutral = true;
    const ui::input::PadState pad = read_pads(padNeutral);
    // The pads drive the UI only while a document is up. With just the overlay (a trophy pop-up, the menu hint) the
    // UI stays awake to draw it, but the pads stay the game's as when it sleeps: Start is the game's, and the chord
    // opens the menu.
    if (ui::any_document_visible()) {
        sPadIdle = false;
        ui::input::update_pad(pad, chord);
    } else {
        if (!sPadIdle) {
            sPadIdle = true;
            ui::input::reset_input_state();  // what is held now is not a press when a document opens
        }
        if (chordEdge) open_menu_bar();
    }
    if (ui::OskWindow* osk = ui::active_osk()) osk->on_pad(pad.buttons);
    ui::trophies::drain();
    ui::net_notices::drain();
    ui::update();
    sStartUp.store(ui::is_prelaunch_open());

    const bool visible = ui::any_document_visible();
    const auto now = clock_type::now();
    if (visible) sLastVisible = now;
    sCapturing.store(visible, std::memory_order_release);
    // While a document is up (and through its closing fade), the hang notice shows or a report waits for its
    // picture, the UI moves at the display's refresh rate: between the game's frames (or once it has stopped) the
    // runtime shows the last one again and calls this hook for it. Not in scripted runs, whose keys and screenshots
    // count frames.
    const bool repaint =
        (now - sLastVisible <= kGrace || now < sHangNoticeUntil || sReportCountdown) && !script && sShots.empty();
    if (repaint != sRepaint) {
        sRepaint = repaint;
        rsx_d3d12_overlay_repaint(repaint ? 1 : 0);
    }
    ui::mouse::set_ui_open(visible);  // (the cursor is the UI's while a document is up)

    if (visible && !sHold) {
        sHold = true;
        cellPad_set_host_hold(1);
    } else if (!visible && sHold && padNeutral && keyboard_neutral()) {
        sHold = false;
        cellPad_set_host_hold(0);
    }

    if (sReportCountdown > 0 && !visible && --sReportCountdown == 0)  // (the game's picture, before the UI is drawn)
        ui::report::send_issue_report(back_buffer, index, int(width), int(height), session_text(width, height));
    {  // HDR output (RSX_HDR): the menus at the game's paper white
        float scale = 1.0f;
        int srgb = 0;
        rsx_d3d12_hdr(&scale, &srgb);
        ui::host::set_hdr(scale, srgb != 0);
    }
    ui::host::set_cursor_wanted(visible);
    ui::game_menu::frame(padNeutral, visible, int(width), int(height));
    ui::host::render(back_buffer, index, int(width), int(height), shot);

    auto* overlay = static_cast<ui::Overlay*>(ui::find_document(ui::DocumentScope::Overlay));
    const bool busy = overlay && overlay->busy();
    if (!visible && !sHold && !busy && !sReportCountdown && !gameMenu && now - sLastVisible > kGrace) {
        // Asleep until F1 or the chord. The chord may still read as held from before: only a new press counts.
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
                  {"PGDN", VK_NEXT},   {"TAB", VK_TAB},    {"SPACE", VK_SPACE}, {"OSK", kScriptOsk}};
    for (const auto& n : kNames)
        if (ui::same_name(n.name, name.c_str())) return n.vk;
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return kScriptChar + name[0];
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

// For des_options.cpp: the start screen is up (or about to be), so DES_AUTO_CONTINUE waits for Play.
extern "C" int des_ui_start_screen_up(void) { return sStartUp.load() ? 1 : 0; }

// des_ui.h. The game's main thread, before the game's code: the frame hook shows the note while this waits (and wakes
// the overlay until it does: with no document up it sleeps). With RSX_PRESENT=0 nothing is drawn, so nothing can ask.
extern "C" void des_ui_privacy_wait(void) {
    if (!ui::privacy::unanswered()) return;
    if (!std::getenv("PS3_NET_ONLINE") || !*std::getenv("PS3_NET_ONLINE")) return;  // (a cheat session cleared it)
    if (const char* v = std::getenv("RSX_PRESENT"); v && !std::strcmp(v, "0")) {
        std::fprintf(stderr, "[ui] the privacy note cannot be shown (RSX_PRESENT=0): this run is offline\n");
        ui::privacy::answer(false);
        return;
    }
    std::fprintf(stderr, "[ui] the privacy note (version %d) waits for an answer\n", ui::privacy::version());
    ui::privacy::begin_asking();
    ui::privacy::wait_answer([] { rsx_d3d12_overlay_wake(); });
}

namespace {

// An ELF on the command line, as main reads it: an argument that is not one of its flags (--username and --psnr, with
// their value after = or as the next argument).
bool elf_given(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--username", 10) && std::strncmp(a, "--psnr", 6)) return true;
        if (!std::strchr(a, '=')) ++i;
    }
    return false;
}

// Setup alone: each pass of the window's thread presents, so V-sync paces it; the last build's pipeline library stays
// shut (its threads would compete with the compile); and the title bar has the game's name (no PARAM.SFO is read).
void setup_only_env() {
    ps3_setenv("RSX_VSYNC", "1");
    ps3_setenv("PS3_PSO_LIBRARY", "0");
    if (!env_on("PS3_TITLE")) ps3_setenv("PS3_TITLE", "Demon's Souls");
    // No game runs in this process, so none of the backend's vertex-program buffers: 1.2 GB more for the compile
    // beside the screens (docs/53 section 16). Play starts the game in a new process, with the environment this one
    // started with.
    if (!std::getenv("RSX_VP_BUFFERS")) ps3_setenv("RSX_VP_BUFFERS", "0");
}

// A player's start keeps the saves where a double-click puts them. They are the PS3 file system's gamedata/dev_hdd0
// under the working folder (cellFs.c, cellSaveData.c), so a shortcut's "Start in", or a start from a terminal in
// another folder, would read and write saves there. So the game works in the exe's folder (docs/90 section 4, item
// 3). Saves in the folder it was started in are named in the log, never moved. DES_KEEP_CWD=1 (a test run with its
// own copy of a save in its own folder) leaves the working folder as it is.
void work_in_exe_folder() {
    if (env_on("DES_KEEP_CWD")) return;
    const std::filesystem::path exe = ui::report::program_dir();
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (exe.empty() || ec || std::filesystem::equivalent(cwd, exe, ec)) return;
    std::filesystem::directory_iterator user(cwd / "gamedata" / "dev_hdd0" / "home", ec), end;
    for (; !ec && user != end; user.increment(ec)) {   // (the forms that throw would end the start)
        std::error_code ec2;
        const auto saves = user->path() / "savedata";
        if (std::filesystem::is_directory(saves, ec2) && !std::filesystem::is_empty(saves, ec2))
            std::fprintf(stderr, "[setup] launch: %s has saves; this start uses the ones beside the game, in %s\n",
                         saves.string().c_str(), (exe / "gamedata").string().c_str());
    }
    std::filesystem::current_path(exe, ec);
    if (ec) std::fprintf(stderr, "[setup] launch: cannot work in %s, so the saves are in %s\n", exe.string().c_str(),
                         (cwd / "gamedata").string().c_str());
}

}  // namespace

extern "C" int des_ui_launch(int* argc, char*** argv) {
    if (const char* v = std::getenv("DES_UI"); v && !std::strcmp(v, "0")) return -1;
    // An update's swap cut short (updates.hpp): undone, then the program that is in place now starts.
    if (ui::updates::recover_swap() && ui::crash_report::start_again({}, false)) return 0;
    if (setup_only_open(std::getenv("DES_UI_OPEN"))) return -2;
    if (elf_given(*argc, *argv)) return -1;
    const ui::setup::Launch launch = ui::setup::launch_state();
    if (launch.state != ui::setup::State::Ready) {
        std::fprintf(stderr, "[setup] launch: the game is not set up yet, setup opens\n");
        return -2;
    }
    work_in_exe_folder();
    // The game setup built (docs/88 section 7): its module, the game's folder and its EBOOT.elf as main's ELF.
    if (!std::getenv("PS3_GAME_MODULE")) ps3_setenv("PS3_GAME_MODULE", launch.module.string().c_str());
    ps3_setenv("PS3_VFS_ROOT", launch.game.string().c_str());
    // The game's folder is only read (docs/90 section 4): its .sdat files are opened as the decrypted copies setup
    // staged, and the runtime's own decryption, if a file has no copy, and the game's HDD cache (/dev_hdd1) go to
    // setup's data folder.
    if (const auto& data = ui::setup::data_dir(); !data.empty()) {
        if (!std::getenv("PS3_EDAT_PLAIN")) ps3_setenv("PS3_EDAT_PLAIN", (data / "inputs").string().c_str());
        if (!std::getenv("PS3_EDAT_CACHE")) ps3_setenv("PS3_EDAT_CACHE", (data / "edat").string().c_str());
        if (!std::getenv("PS3_HDD1_ROOT")) {
            std::error_code ec;
            std::filesystem::create_directories(data / "hdd1", ec);
            ps3_setenv("PS3_HDD1_ROOT", (data / "hdd1").string().c_str());
        }
    }
    static std::string eboot;
    static std::vector<char*> args;
    eboot = launch.eboot.string();
    args.assign(*argv, *argv + *argc);
    args.insert(args.begin() + 1, eboot.data());
    args.push_back(nullptr);
    *argc += 1;
    *argv = args.data();
    std::fprintf(stderr, "[setup] launch: the game setup built, %s\n", eboot.c_str());
    return -1;
}

extern "C" void des_ui_setup_only_init(void) {
    sSetupOnly = true;
    des_ui_early_init();
}

extern "C" void des_ui_early_init(void) {
    std::fprintf(stderr, "[des] DemonsSoulsRecomp %s, ps3recomp %s, game dump %.12s\n", ui::kVersion,
                 ui::kPs3recompVersion, *ui::kDumpSha256 ? ui::kDumpSha256 : "unknown");
    ui::crash_report::install();  // (with DES_UI=0 too; nothing in a test run)
    if (const char* v = std::getenv("DES_UI"); v && !std::strcmp(v, "0")) {
        return;
    }
    ui::settings::load_and_apply_env();
    if (sSetupOnly) setup_only_env();
    if (!sSetupOnly && ui::settings::net_custom_held()) sCustomServerNotice.store(true);
    parse_debug_knobs();
    sOpenPending = sSetupOnly || env_on("DES_UI_OPEN");
    sChordMask = ui::input::chord_mask(ui::settings::get_int("pad_menu_chord"));

    sStartScreen = !sSetupOnly && (env_on("DES_UI_START") || (ui::settings::get_int("show_start_screen") != 0 &&
                                                              !env_on("DES_SKIP_UI") && !ui::settings::scripted_run()));
    sStartUp.store(sStartScreen);
    ui::mouse::set_ui_open(sStartScreen);  // (until the first frame pushes it)

    sCloseAsks = !sSetupOnly && !ui::settings::unattended_run();  // (setup alone: nothing to lose, it carries on next time)
    ui::app().quit = [] {
        sQuitConfirmed.store(true);
        rsx_d3d12_post_message(WM_CLOSE, 0, 0);
    };
    ui::app().report_issue = [] { sReportCountdown = 4; };  // (the render thread: the menu's callbacks run in frame)

    static const rsx_d3d12_overlay kOverlay = {nullptr, wndproc, frame, [](void*) { ui::host::release_buffers(); }};
    rsx_d3d12_set_overlay(&kOverlay);
    static const CellOskDialogHost kOsk = {osk_open, osk_close};
    cellOskDialog_set_host(&kOsk);
    if (sSetupOnly) return;  // (no game: no trophies, and no frames to watch)
    ui::trophies::install();
    std::thread(watch_thread).detach();
}
