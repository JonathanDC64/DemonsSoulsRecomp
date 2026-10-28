/* Demon's Souls — user-facing options (host-side knobs on the game's own state).
 *
 * These are not workarounds: each one drives a state machine the game already
 * has, through the same transitions it takes on its own, just sooner. All are
 * off by default.
 *
 * DES_SKIP_INTRO=1 | 2 — skip or shorten the boot logos (DsMenuDlgLogo).
 *   The dialog's per-frame update (vtable method at 0x008EF430) runs a
 *   per-logo state machine on the dialog object:
 *     +0x0F8 m_elapseTime   (float, hold time accumulated in state 2)
 *     +0x0FC m_curLogType   (0..4; the five logos, in order)
 *     +0x100 m_animState    (0 start fade-in, 1 wait fade-in, 2 hold,
 *                            3 wait fade-out -> next logo, or leave)
 *     +0x104 pre-delay done (byte), +0x108 pre-delay, +0x10C pre-delay elapsed
 *   The hold is 3 s for logo 0 and 5 s for the others; each fade is 1 s, so the
 *   sequence takes about 33 s. When the fade-out of logo 4 finishes, the dialog
 *   requests the next menu state itself (0x3C, the title).
 *   1 does what RPCS3's "Skip Intro" patch for BLUS30443 does (li r0,3;
 *   stw r0,0x100(r28) over the state tests at 0x008EEF78): every update takes
 *   state 3's branch, which moves to the next logo without waiting for a fade,
 *   and after the last one requests the title itself. No logo is drawn, and the
 *   title comes up about five frames after the dialog opens.
 *   2 (the first version of the option) makes a logo that is about to start
 *   fading in (state 0) the last one, and treats the hold of state 2 as already
 *   over: the last logo's fade-in and fade-out still play, about 2 s.
 *   Either way the game takes its own exit path, so nothing downstream sees a
 *   state it would not see on hardware. Read at each update, so a change in
 *   the settings window applies to the logo on screen (or the next start).
 *
 * DES_SKIP_INTRO_MOVIE=1 — skip the opening movie (movie/ds_advertise.pam, the
 *   130 s advert after the logos), and only that play of it: the title screen
 *   replays the same advert after each 10 s loop of its own movie, and those
 *   plays stay. It sets PS3_SAIL_SKIP_FIRST, so cellSail ends that movie the
 *   first time it starts, as START does on hardware; the title screen's own
 *   movie, the one after character creation and the endings still play. The
 *   settings window changes it live (from the next movie; once the advert has
 *   played, there is nothing left to skip).
 *
 * DES_FPS=30 | 60 | 31..500 | unlocked — the game's own frame-rate mode, with real frame time.
 *   The engine object at *0x01AD6298 (built by 0x00025DD8) holds a flip mode
 *   its debug menu shows as text (0x000290A8): +0x6ED 0 is "FlipMode 30FPS"
 *   (the default: when a frame runs late it draws a near-empty one, the skip
 *   frames), +0x6ED 1 with +0x6EC 0 is "30FPS(NO SKIP)", and both 1 is
 *   "60FPS(NO SKIP)". The frame update 0x0001B8D0 advances the game by the
 *   float at 0x018C8DB4, a constant 1/30, and nothing else reads it.
 *   With the option on, the mode is set once the object exists and that float
 *   is rewritten every frame with the time since the previous flip request,
 *   capped at 50 ms so a long frame slows the game instead of jumping it.
 *   RPCS3's "Unlock FPS" patch (Gibbed, Whatcookie) changes the same two
 *   things in the code; here the code is left alone. 60 mode flips at most
 *   once a vblank, so the vblank is the frame limit: any other DES_FPS=<n>
 *   from 31 to 500 sets 60 mode and a vblank of n Hz (unless VBLANK_MHZ was
 *   set at start) and so a cap of n, and DES_FPS=unlocked a vblank of 1 kHz,
 *   so a frame waits at most 1 ms for its flip and the rate is whatever the
 *   machine reaches. DES_FPS is read at every flip request, so a change
 *   (the settings window's, or the console's setenv) applies at once, and
 *   an empty value or 0 puts back the game's own mode. Presents still wait for the display's
 *   refresh unless DES_VSYNC=0, so with vsync a 144 Hz display shows at most
 *   144 of them a second. Movies keep their speed (cellSail
 *   hands out the frame that is due by the clock), and the game advances by
 *   real time: the regress walk ends in the same place at ~66 fps as at 30.
 *   The in-engine cutscenes (RemoMan) stay at 30: their camera takes the
 *   nearest of keys 1/30 s apart while the characters follow the frame
 *   time, so unlocked they judder (a 2-frame back-and-forth at 60, a
 *   4-frame sawtooth at 120). While a RemoMan slot holds a cutscene the
 *   game is put back in 30 mode with the 1/30 step and the PS3's vblank,
 *   and unlocked again when it ends. DES_CUTFPS=1 leaves cutscenes
 *   unlocked and blends the camera's keys instead (des_cut_cam).
 *   One movement test counts in 30 Hz steps too. After physics, 0x002EC2B8
 *   asks whether a character that wants to move fast (its animation asks
 *   for more than 5 m/s) moved less than 1/30 m this step, i.e. under 1 m/s
 *   at 30 fps. If so it is held by something, and the factor its root
 *   motion is scaled by (movement object +0x128) goes down 0.8x a step to
 *   0.5; if not, it goes back up 1.2x a step to 1. At 120 fps a step is a
 *   quarter as long, so the test fires under 4 m/s: a dash that starts from
 *   a slow run (stamina still coming back) drops to half speed, 3.2 m/s,
 *   and stays there. des_move_post redoes the test with the step's real
 *   length and makes the rates per 30 Hz step as well.
 *   The loading screens' two soul icons step once a frame as well, and so
 *   does the boot screen's fade; load_souls_rate (boot) and
 *   nowloading_souls_rate (level loads) move them in 30 Hz steps. Menu
 *   text too long for its box (the inventory's key guide) scrolls 1.5 px a
 *   frame; scroll_msg_rate makes that 1.5 px a 30 Hz step. On the HUD, the
 *   soul counter's roll after a kill (get_soul_rate), the bars' fill after
 *   damage or a heal (gauge_fill_rate) and their length when the max
 *   changes (gauge_len_rate) step once a frame too, and go in 30 Hz steps.
 *   So do the character creation screen's previews, which turn a fixed
 *   angle a frame with the right stick (des_viewer_rate).
 *
 * DES_VSYNC=1 | 0 | even | vrr — how frames meet the display (default 1).
 *   Read at each flip request, so the settings window changes it live; it
 *   sets RSX_VSYNC (each frame held that many refreshes; 0 = at once, with
 *   tearing) unless RSX_VSYNC was set at start, and the vblank clock.
 *   1 presents at the display's refresh. 0 presents each frame as soon as it
 *   is drawn, with tearing, where the system supports it (the D3D12 backend
 *   asks DXGI; without it, it presents with vsync and says so once in the
 *   log). Above the display's refresh (DES_FPS over 60 on a 60 Hz display)
 *   only 0 shows every frame.
 *   even: every frame stays on screen the same number of refreshes. A frame
 *   rate that does not divide the refresh cannot do that: 60 fps on a 144 Hz
 *   display shows frames for 2, 3, 2, 2, 3 refreshes, a judder however
 *   evenly the game makes them (docs/94). So with DES_FPS set, the rate goes
 *   to the nearest the display divides (the refresh over n: 60 becomes 72 on
 *   a 144 Hz display, 30 becomes 28.8), the vblank clock ticks on the
 *   display's own vblanks every n refreshes (ps3_vblank_lock_display), each
 *   frame is held n refreshes (RSX_VSYNC=n), and the game's step is n
 *   refreshes long. The game's own 30 fps mode and the cutscenes keep the
 *   PS3's 59.94 Hz vblank, so they are even only where the display runs at
 *   a multiple of it (60, 120, 240 Hz): there the clock goes to the
 *   display's vblanks too (59.94 to 60 Hz, 0.1% faster); elsewhere they
 *   present as with 1.
 *   vrr: for a G-Sync or FreeSync display, which shows each frame as it
 *   comes. The rate is capped a little under the display mode's refresh
 *   (the refresh x (1 - refresh / 3600): 138 fps at 144 Hz), so the frames
 *   stay inside the display's range, and frames present at the refresh
 *   (as 1). On such a display that shows a frame at once, and one that
 *   comes a little sooner than a refresh after the last (the clock's timer
 *   wakes up to half a millisecond off) waits for the last to finish
 *   drawing instead of tearing through it. Presenting at once, with tearing,
 *   tore a fifth of the frames at 138 fps (docs/94). The display and its
 *   driver have to have it on; the log says whether the display follows
 *   the frames (vrr_note).
 *
 * DES_AUTO_CONTINUE=1 — press through the boot menus into the last save.
 *   From the first pad poll it taps CROSS, CROSS, START in a 600 ms cycle:
 *   that clears the notices, the title's "press start", "load game" and the
 *   first save in the list. It never presses CIRCLE or the d-pad, so it only
 *   goes forwards and never moves a cursor off its default. It lets go for
 *   good when the level's script archive (script/m*.luabnd) opens. Read at
 *   each pad poll, so the settings window turns it on or off live until then;
 *   after the first level there is nothing left for it to do.
 *
 * The dev cheats and the warp are in des_dev.cpp, compiled into developer
 *   builds only (cmake -DDES_DEV=ON; docs/55).
 */
#include "ps3emu/ppu_lift.h"   /* ppu_context: the fixed part of the lift's header */
#include "des_game_calls.h"     /* the lifted functions below (the game DLL's with DES_SPLIT) */
#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include "ui/include/des_ui.h"  /* the wrapper UI: settings.ini and the overlay (docs/32) */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void (*g_gcm_flip_request_hook)(void);
extern "C" unsigned (*g_pad_title_hook)(void);
extern "C" void (*g_fs_open_hook)(const char* guest_path);
extern "C" void ps3_vblank_set_mhz(int mhz);   /* boot_main.cpp; 0 = VBLANK_MHZ */
extern "C" void ps3_vblank_lock_display(int n);   /* boot_main.cpp; a tick every n refreshes, 0 = the timer */
extern "C" int rsx_d3d12_display_refresh_mhz(void);   /* the window's display, 0 = not known */
extern "C" int rsx_d3d12_display_mode_mhz(void);      /* its mode alone, in whole Hz */
extern "C" int rsx_d3d12_display_shown_mhz(void);     /* how often it refreshed, by the swap chain */
extern "C" void des_aspect_init(void);         /* des_aspect.cpp: DES_ASPECT, DES_HUD */
extern "C" void des_aspect_poll(void);         /* des_aspect.cpp: their live changes */
extern "C" void des_hud_init(void);            /* des_hud.cpp: DES_HUD_SIZE, the corners, hidden groups */
extern "C" volatile unsigned g_pad0_reads, g_pad0_empty_reads, g_pad0_buttons, g_pad0_lstick;   /* cellPad.c */
extern "C" void cellPad_host_stick(int stick, unsigned char x, unsigned char y, unsigned reads);   /* cellPad.c */
#ifdef _WIN32
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vkey);   /* user32 */
constexpr int VK_F8 = 0x77;
#endif

namespace {

constexpr uint32_t kLogoUpdate   = 0x008EF430u;
constexpr uint32_t kElapseTime   = 0x0F8u;
constexpr uint32_t kCurLogType   = 0x0FCu;
constexpr uint32_t kAnimState    = 0x100u;
constexpr uint32_t kLastLogType  = 4u;
constexpr const char* kOpeningMovie = "ds_advertise.pam";   /* PS3_SAIL_SKIP_FIRST for DES_SKIP_INTRO_MOVIE */

bool env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* DES_SKIP_INTRO as it is now: 0, 1 or 2. The settings window changes it
 * live; the logos only play at start, so a change shows there or at the
 * next start. */
int skip_intro()
{
    static int mode = 0;
    PS3_KNOB(mode, env_on("DES_SKIP_INTRO") ? (atoi(getenv("DES_SKIP_INTRO")) == 2 ? 2 : 1) : 0);
    return mode;
}

void logo_update_skip(ppu_context* ctx)
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    const int mode = skip_intro();
    if (dlg && mode == 1) {
        vm_write32(dlg + kAnimState, 3);   /* to the next logo, or out after the last */
    } else if (dlg && mode == 2) {
        const uint32_t state = vm_read32(dlg + kAnimState);
        if (state == 0 && vm_read32(dlg + kCurLogType) < kLastLogType) {
            static bool said = false;
            if (!said) {
                said = true;
                fprintf(stderr, "[des] DES_SKIP_INTRO: logo %u -> last logo\n",
                        vm_read32(dlg + kCurLogType));
            }
            vm_write32(dlg + kCurLogType, kLastLogType);
        }
        if (state == 2) {
            const float done = 1.0e9f;   /* past any hold threshold */
            uint32_t bits;
            memcpy(&bits, &done, 4);
            vm_write32(dlg + kElapseTime, bits);
        }
    }
    func_008EF430(ctx);   /* the game's own update does the rest */
}

constexpr uint32_t kFlipModeObj  = 0x01AD6298u;   /* -> engine object */
constexpr uint32_t kFlipMode60   = 0x6ECu;
constexpr uint32_t kFlipNoSkip   = 0x6EDu;
constexpr uint32_t kFrameDt      = 0x018C8DB4u;   /* float, seconds */
constexpr float    kMaxFrameDt   = 0.05f;
constexpr float    kCutsceneDt   = 1.0f / 30.0f;
constexpr int      kPs3VblankMhz = 59940;

/* RemoMan, the in-engine cutscene player: *0x01B3A28C. Its request
 * (0x001A1AF0, reached from the Lua RequestRemo) takes the first free of four
 * slots at +0x28, +0x40, +0x58 and +0x70 (0x18 bytes each) and stores the
 * cutscene object in the slot's first word; the word is cleared when the
 * cutscene ends. A cutscene requested during a level load holds its slot
 * from the load on. */
constexpr uint32_t kRemoMan      = 0x01B3A28Cu;
constexpr uint32_t kRemoSlots[]  = {0x28u, 0x40u, 0x58u, 0x70u};

/* DES_FPS as applied: -1 before the engine object exists, 0 the game's own
 * mode, else the frame rate (kFpsUnlocked for unlocked). Render thread only
 * (the flip request hook). */
int s_fps = -1;
bool s_vblank_pinned = false;   /* VBLANK_MHZ set at start: it wins over the cap */

constexpr int kFpsMax      = 500;
constexpr int kFpsUnlocked = 1000;   /* the cap of DES_FPS=unlocked: a 1 kHz vblank */

int fps_parse(const char* f)
{
    if (f && strcmp(f, "unlocked") == 0) return kFpsUnlocked;
    const int fps = f ? atoi(f) : 0;
    if (fps >= 30 && fps <= kFpsMax) return fps;
    if (f && *f && strcmp(f, "0") != 0)
        fprintf(stderr, "[des] DES_FPS=%s ignored (30-%d, or unlocked)\n", f, kFpsMax);
    return 0;
}

/* For the log: "unlocked", or the rate. */
std::string fps_name(int fps)
{
    return fps == kFpsUnlocked ? "unlocked" : std::to_string(fps);
}

/* Read live: the settings window sets it with ps3_setenv. */
int fps_wanted()
{
    static int fps = 0;
    PS3_KNOB(fps, fps_parse(getenv("DES_FPS")));
    return fps;
}

/* The vblank while unlocked: the cap, except at 30 and 60, which run on the
 * PS3's (0 hands the clock back to VBLANK_MHZ, which a user setting it at
 * start keeps). */
int fps_vblank_mhz(int fps)
{
    return fps > 30 && fps != 60 && !s_vblank_pinned ? fps * 1000 : 0;
}

bool cutscene_playing()
{
    const uint32_t man = vm_read32(kRemoMan);
    if (!man) return false;
    for (uint32_t off : kRemoSlots)
        if (vm_read32(man + off)) return true;
    return false;
}

/* DES_CUTFPS=1: cutscenes at the DES_FPS rate, without the 30 fps lock
 * below; des_cut_cam blends the camera's keys. Read live. */
bool cutfps_on()
{
    static int on = 0;
    PS3_KNOB(on, env_on("DES_CUTFPS"));
    return on != 0;
}

/* DES_VSYNC (the header): how frames meet the display. Read live. */
enum class Sync { Off, On, Even, Vrr };

Sync sync_parse(const char* v)
{
    if (!v || !*v) return Sync::On;
    if (strcmp(v, "even") == 0) return Sync::Even;
    if (strcmp(v, "vrr") == 0) return Sync::Vrr;
    return atoi(v) ? Sync::On : Sync::Off;
}

Sync sync_wanted()
{
    static int s = int(Sync::On);
    PS3_KNOB(s, int(sync_parse(getenv("DES_VSYNC"))));
    return Sync(s);
}

bool s_vsync_pinned = false;   /* RSX_VSYNC set at start: it wins over DES_VSYNC */

/* What the frame clock and the presents do in the mode the game is in. */
struct Pacing {
    int mode60 = -1;      /* the flip mode's 60 byte; -1 = the game's own */
    int vblank_mhz = 0;   /* ps3_vblank_set_mhz; 0 = VBLANK_MHZ */
    int lock = 0;         /* ps3_vblank_lock_display */
    int interval = 1;     /* RSX_VSYNC */
    float step = 0.0f;    /* even frames: the frame step, n refreshes in seconds (0 = real time only) */
};

/* fps: s_fps (0 or less: the game's own mode); cut: as in fps_on_flip_request.
 * The refreshes a frame is held under even frames stay the same while the
 * frame rate is within 0.6 of them, so a display at 150 Hz with 60 fps (2.5)
 * does not switch at every reading of its rate. */
Pacing pacing_for(int fps, int cut, Sync sync)
{
    Pacing p;
    /* 30 mode on the PS3's vblank: the game's own mode, a cutscene, and 30 fps
     * but under even frames, which runs it like any other rate. */
    const bool ps3_30 = fps <= 0 || cut == 1 || (fps == 30 && sync != Sync::Even);
    p.mode60 = fps <= 0 ? -1 : ps3_30 ? 0 : 1;
    p.vblank_mhz = fps <= 0 ? 0 : cut == 1 ? kPs3VblankMhz : fps_vblank_mhz(fps);
    p.interval = sync == Sync::Off ? 0 : 1;
    const int r = rsx_d3d12_display_refresh_mhz();
    if (r <= 0) return p;
    if (sync == Sync::Vrr && !ps3_30) {
        /* From the mode's rate: under a variable refresh the swap chain's
         * statistics are the frames' own times, and a reading of them that
         * came near the mode's moved the cap from 138 to 136 and back. */
        const int mode = rsx_d3d12_display_mode_mhz();
        const int top = mode > 0 ? mode : r;
        const int cap = int(top / 1000.0 * (1.0 - top / 3600000.0));
        if (fps > cap) p.vblank_mhz = fps_vblank_mhz(cap);
    } else if (sync == Sync::Even && !s_vblank_pinned) {
        if (ps3_30) {
            const int m = (r + kPs3VblankMhz / 2) / kPs3VblankMhz;
            if (m >= 1 && std::abs(r - m * kPs3VblankMhz) * 1000LL <= m * kPs3VblankMhz * 6LL) {   /* 0.6% */
                p.lock = m;
                p.interval = std::min(2 * m, 4);
                p.vblank_mhz = r / m;
            }
        } else {
            static int held = 0;
            const double x = double(r) / (double(fps) * 1000.0);
            if (!held || std::fabs(x - held) >= 0.6) held = std::max(1, int(x + 0.5));
            p.lock = held;
            p.interval = std::min(held, 4);
            p.vblank_mhz = r / held;
            p.step = float(held * 1000.0 / r);
        }
    }
    return p;
}

Pacing s_paced;   /* as applied (the backend's and the clock's defaults to start) */

void pacing_apply(const Pacing& p, uint32_t obj, Sync sync)
{
    if (p.mode60 >= 0 && p.mode60 != s_paced.mode60) vm_write8(obj + kFlipMode60, uint8_t(p.mode60));
    const bool say = p.lock != s_paced.lock || p.interval != s_paced.interval ||
                     std::abs(p.vblank_mhz - s_paced.vblank_mhz) * 200LL > std::max(p.vblank_mhz, 1);
    if (p.lock != s_paced.lock) ps3_vblank_lock_display(p.lock);
    if (p.vblank_mhz != s_paced.vblank_mhz) ps3_vblank_set_mhz(p.vblank_mhz);
    if (p.interval != s_paced.interval && !s_vsync_pinned)
        ps3_setenv("RSX_VSYNC", std::to_string(p.interval).c_str());
    if (say) {
        const char* name = sync == Sync::Off ? "0" : sync == Sync::Even ? "even" : sync == Sync::Vrr ? "vrr" : "1";
        const int r = rsx_d3d12_display_refresh_mhz();
        if (p.lock)
            fprintf(stderr, "[des] DES_VSYNC=%s: the vblank on every %d of %.2f Hz, a frame held %d refreshes\n",
                    name, p.lock, r / 1000.0, p.interval);
        else
            fprintf(stderr, "[des] DES_VSYNC=%s: the vblank at %.2f Hz, presents %s%s\n", name,
                    (p.vblank_mhz ? p.vblank_mhz : kPs3VblankMhz) / 1000.0,
                    p.interval ? "at the refresh" : "at once",
                    sync == Sync::Even ? " (this rate does not fit the display: as V-sync)"
                    : sync == Sync::Vrr ? " (shown at once where the display follows the frames)" : "");
    }
    s_paced = p;
}

/* Under vrr, every 2 s: does the display follow the frames? A fixed
 * refresh counts its refreshes at the mode's rate whatever the frames do;
 * one that follows them (G-Sync, FreeSync) refreshes as often as they come,
 * and the cap keeps them 4% or more under the mode. The log says when the
 * answer changes, after two readings agree. */
void vrr_note(Sync sync)
{
    using clock = std::chrono::steady_clock;
    static clock::time_point at;
    static int said = -1, last = -1;
    if (sync != Sync::Vrr) {
        said = last = -1;
        return;
    }
    const clock::time_point now = clock::now();
    if (now - at < std::chrono::seconds(2)) return;
    at = now;
    const int shown = rsx_d3d12_display_shown_mhz(), mode = rsx_d3d12_display_mode_mhz();
    if (shown <= 0 || mode <= 0) return;
    const int follows = shown * 100LL < mode * 98LL ? 1 : 0;
    if (follows == last && follows != said) {
        if (follows)
            fprintf(stderr, "[des] DES_VSYNC=vrr: the display follows the frames (%.1f Hz): G-Sync / FreeSync is on\n",
                    shown / 1000.0);
        else
            fprintf(stderr, "[des] DES_VSYNC=vrr: the display stays at %.2f Hz: G-Sync / FreeSync is not on "
                    "for this window (the driver's setting, the display's own menu, the window in front)\n",
                    shown / 1000.0);
        said = follows;
    }
    last = follows;
}

void fps_on_flip_request()
{
    using clock = std::chrono::steady_clock;
    static uint8_t orig60, orig_noskip;
    static uint32_t orig_dt;
    static int in_cutscene = 0;   /* 0 none, 1 at 30 (the lock), 2 unlocked (DES_CUTFPS) */
    static clock::time_point last;
    const Sync sync = sync_wanted();
    const uint32_t obj = vm_read32(kFlipModeObj);
    if (!obj) {
        pacing_apply(pacing_for(0, 0, sync), obj, sync);
        return;
    }
    /* A change of DES_FPS takes effect at the next flip request: the mode
     * bytes, the step and the vblank are set as at the start, and the game's
     * own mode puts back what it found. */
    const int want = fps_wanted();
    if (want != s_fps) {
        if (s_fps < 0) {
            orig60 = vm_read8(obj + kFlipMode60);
            orig_noskip = vm_read8(obj + kFlipNoSkip);
            orig_dt = vm_read32(kFrameDt);
        }
        if (want) {
            vm_write8(obj + kFlipNoSkip, 1);
            fprintf(stderr, "[des] DES_FPS=%s: FlipMode %s on 0x%08X, real frame time%s\n",
                    fps_name(want).c_str(), want > 30 ? "60FPS(NO SKIP)" : "30FPS(NO SKIP)", obj,
                    fps_vblank_mhz(want) ? ", vblank at the cap" : "");
        } else if (s_fps > 0) {
            vm_write8(obj + kFlipMode60, orig60);
            vm_write8(obj + kFlipNoSkip, orig_noskip);
            vm_write32(kFrameDt, orig_dt);
            s_paced.mode60 = -1;
            fprintf(stderr, "[des] DES_FPS off: the game's own 30 fps mode\n");
        }
        s_fps = want;
        in_cutscene = 0;
        last = clock::time_point();
    }
    /* A cutscene plays as the game ships it: 30 mode on the PS3's vblank, so
     * it flips every other vblank, and a fixed 1/30 step. Its camera takes
     * the nearest of keys 1/30 s apart while everything else follows the
     * frame step, so any faster and a cutscene judders. The vblank goes back
     * to the unlocked rate when the cutscene ends. With DES_CUTFPS=1 a
     * cutscene stays unlocked and des_cut_cam blends the camera's keys. 30 fps
     * under even frames runs at the display's rate (pacing_for), so its
     * cutscenes go to 30 too. */
    const bool unlocked = s_fps > 30 || (s_fps > 0 && sync == Sync::Even);
    const int cut = unlocked && cutscene_playing() ? (cutfps_on() ? 2 : 1) : 0;
    if (cut != in_cutscene) {
        fprintf(stderr, "[des] DES_FPS: %s\n",
                cut == 1 ? "cutscene, 30 fps" : cut == 2 ? "cutscene, unlocked (DES_CUTFPS)"
                : in_cutscene == 1 ? "cutscene over, unlocked again" : "cutscene over");
        in_cutscene = cut;
    }
    const Pacing p = pacing_for(s_fps, cut, sync);
    pacing_apply(p, obj, sync);
    vrr_note(sync);
    if (!s_fps) return;
    /* Under even frames a frame is shown n refreshes after the last, or 2n
     * after a late one: the step is that many whole frames when the time
     * since the last flip request is within a quarter frame of it, so the
     * game moves by the time each frame is on screen and not by when the
     * request came. */
    const clock::time_point now = clock::now();
    float dt = kCutsceneDt;
    if (cut != 1 && last != clock::time_point()) {
        dt = std::chrono::duration<float>(now - last).count();
        if (p.step > 0.0f) {
            const float k = std::round(dt / p.step);
            if (k >= 1.0f && std::fabs(dt - k * p.step) < p.step * 0.25f) dt = k * p.step;
        }
        if (dt > kMaxFrameDt) dt = kMaxFrameDt;
    }
    uint32_t bits;
    memcpy(&bits, &dt, 4);
    vm_write32(kFrameDt, bits);
    last = now;
}

/* The player (character 10000) through the lookup Lua's SaveChrPos uses
 * (0x004142A8): *(*0x01B3D6E0 + 0x1BC) is the character, and its position is
 * the float x, y, z at *(*(chr + 0x20) + 0x10) + 0x10. Any link can be 0 or
 * stale during a load; every one is range-checked. */
constexpr uint32_t kChrMan       = 0x01B3D6E0u;
/* Main memory and the lv2 areas below 0x50000000, or the stack area
 * (0xD0000000-0xDFFFFFFF), where lv2 puts every thread's stack. The main
 * thread's is there too with PS3_MAIN_STACK_LV2=1, and hooks such as
 * des_body_xform write to its locals. */
bool guest_ptr_ok(uint32_t a)
{
    return ((a >= 0x00010000u && a < 0x50000000u) || (a >= 0xD0000000u && a < 0xE0000000u)) && !(a & 3u);
}

/* The character also holds its stamina (int, +0x3D4) and the move speed it
 * asks for (float m/s, +0xCCC): found by dumping it while dashing. */
bool player_pos(float* x, float* y, float* z, int* stamina, float* speed)
{
    uint32_t p = vm_read32(kChrMan);
    if (!guest_ptr_ok(p) || !guest_ptr_ok(p = vm_read32(p + 0x1BC))) return false;
    const uint32_t chr = p, sp = vm_read32(chr + 0xCCC);
    *stamina = (int)vm_read32(chr + 0x3D4);
    memcpy(speed, &sp, 4);
    if (!guest_ptr_ok(p = vm_read32(p + 0x20)) || !guest_ptr_ok(p = vm_read32(p + 0x10))) return false;
    uint32_t v[3] = {vm_read32(p + 0x10), vm_read32(p + 0x14), vm_read32(p + 0x18)};
    memcpy(x, &v[0], 4); memcpy(y, &v[1], 4); memcpy(z, &v[2], 4);
    return true;
}

/* DES_FRAMELOG=<file>: one line per flip request (time, interval, the step the
 * game was handed, cutscene, player position) and every half second a summary
 * of the player's ground speed and the sum of steps against real time. With
 * the developer keys on (RSX_DEV_HOTKEYS=1: a developer build, or a tools/des
 * run), F8 writes a MARK line, to tag the moment something looks wrong. */
FILE* s_framelog = nullptr;
char s_framelog_path[260];

/* Snapshot the player's movement state for a frame log: the movement object
 * (*(chr + 0x20)), its animator (*(mv + 8): a count, then an array of 0x58-byte
 * animation controls), and one level of heap pointers out of each. Records are
 * [u32 guest address][u32 length][bytes], big-endian as in guest memory. */
void movement_dump(const char* kind, double ms)
{
    static int n = 0;
    if (n >= 40) return;
    uint32_t chr = vm_read32(kChrMan);
    if (!guest_ptr_ok(chr) || !guest_ptr_ok(chr = vm_read32(chr + 0x1BC))) return;
    const uint32_t mv = vm_read32(chr + 0x20);
    if (!guest_ptr_ok(mv)) return;
    const uint32_t anim = vm_read32(mv + 8);
    uint32_t regions[96][2]; int nr = 0;
    auto add = [&](uint32_t a, uint32_t len) {
        if (!guest_ptr_ok(a) || nr >= 96) return;
        for (int i = 0; i < nr; i++) if (regions[i][0] == a) return;
        regions[nr][0] = a; regions[nr][1] = len; nr++;
    };
    add(chr, 0x1000); add(mv, 0x400);
    uint32_t count = 0, ctrls = 0;
    if (guest_ptr_ok(anim)) {
        add(anim, 0x100);
        count = vm_read32(anim); ctrls = vm_read32(anim + 4);
        if (count > 32) count = 32;
        if (count) add(ctrls, count * 0x58);
    }
    const int first = nr;   /* follow pointers out of mv, the animator and the controls */
    for (int r = 1; r < first; r++)
        for (uint32_t o = 0; o < regions[r][1]; o += 4) {
            const uint32_t q = vm_read32(regions[r][0] + o);
            if (q >= 0x30000000u) add(q & ~0xFu, 0x100);
        }
    char path[300];
    snprintf(path, sizeof path, "%s.dump%02d_%s.bin", s_framelog_path, n, kind);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    auto put = [&](uint32_t v) {
        const unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                                    (unsigned char)(v >> 8), (unsigned char)v};
        fwrite(b, 1, 4, f);
    };
    for (int i = 0; i < nr; i++) {
        put(regions[i][0]); put(regions[i][1]);
        for (uint32_t o = 0; o < regions[i][1]; o += 4) put(vm_read32(regions[i][0] + o));
    }
    fclose(f);
    fprintf(s_framelog, "DUMP %d %s %.1f chr %08X mv %08X anim %08X controls %u at %08X\n",
            n, kind, ms, chr, mv, anim, count, ctrls);
    n++;
}

void frame_log()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    static double last_ms = 0, win_ms = 0, win_dt = 0, win_dist = 0;
    static int win_n = 0;
    static bool have_pos = false, f8_down = false;
    static float px, pz;
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    uint32_t bits = vm_read32(kFrameDt);
    float dt; memcpy(&dt, &bits, 4);
    float x = 0, y = 0, z = 0, want = 0;
    int stamina = 0;
    const bool pos = player_pos(&x, &y, &z, &stamina, &want);
    const bool cut = cutscene_playing();
    fprintf(s_framelog, "%.1f %.2f %.2f %d", ms, ms - last_ms, dt * 1000.0f, cut ? 1 : 0);
    static unsigned last_reads, last_empty;
    const unsigned reads = g_pad0_reads, empty = g_pad0_empty_reads;
    fprintf(s_framelog, " pad %04X %04X %u %u", g_pad0_buttons, g_pad0_lstick,
            reads - last_reads, empty - last_empty);
    last_reads = reads; last_empty = empty;
    if (pos) fprintf(s_framelog, " %.3f %.3f %.3f %d %.2f", x, y, z, stamina, want);
    if (pos) {   /* the movement object's root-motion factor (see des_move_post), then HP and MP */
        const uint32_t chr = vm_read32(vm_read32(kChrMan) + 0x1BC);
        const uint32_t fb = vm_read32(vm_read32(chr + 0x20) + 0x128);
        float f; memcpy(&f, &fb, 4);
        fprintf(s_framelog, " %.3f %d %d", f, (int)vm_read32(chr + 0x3C4), (int)vm_read32(chr + 0x3CC));
    }
    fputc('\n', s_framelog);
    if (pos && have_pos) {
        const float d = std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        if (d < 5.0f) win_dist += d;   /* a warp or respawn is not running */
    }
    /* A dash that covers half the ground the game asks for (the 120 fps run
     * speed bug) and one that covers all of it: snapshot each once per dash. */
    static int slow_run = 0, full_run = 0;
    static bool slow_done = false, full_done = false;
    if (pos && have_pos && want > 5.0f && dt > 0.0f) {
        const float d = std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        const float r = d / (want * dt);
        slow_run = (r > 0.4f && r < 0.6f) ? slow_run + 1 : 0;
        full_run = (r > 0.9f && r < 1.1f) ? full_run + 1 : 0;
        if (slow_run == 60 && !slow_done) { slow_done = true; movement_dump("slow", ms); }
        static int full_dumps = 0;   /* leave room for the slow ones */
        if (full_run == 60 && !full_done && full_dumps < 8) { full_done = true; full_dumps++; movement_dump("full", ms); }
    } else if (want < 4.0f) {
        slow_run = full_run = 0; slow_done = full_done = false;
    }
    have_pos = pos; px = x; pz = z;
    win_dt += dt; win_n++;
#ifdef _WIN32
    const bool f8 = !ps3_env_int("PS3_NO_USER_INPUT", 0) && ps3_env_int("RSX_DEV_HOTKEYS", 0) &&
                    (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
#else
    const bool f8 = false;   /* (the F8 mark is Windows-only for now) */
#endif
    if (f8 && !f8_down) { fprintf(s_framelog, "MARK %.1f\n", ms); movement_dump("mark", ms); }
    f8_down = f8;
    if (ms - win_ms >= 500.0) {
        const double s = (ms - win_ms) / 1000.0;
        fprintf(s_framelog, "SUM %.1f fps %.1f speed %.2f m/s game/real %.3f\n", ms,
                win_n / s, win_dist / s, win_dt / s);
        fflush(s_framelog);
        win_ms = ms; win_dt = 0; win_dist = 0; win_n = 0;
    }
    last_ms = ms;
}

/* Flip requests so far (any thread): des_mouse_look tells a gap in the
 * camera's updates by them. */
std::atomic<unsigned> s_flips{0};

void on_flip_request()
{
    des_report_flip_hook();   /* (a test knob: DES_HANG_AT_FRAME) */
    des_aspect_poll();        /* DES_ASPECT, DES_HUD changed while playing */
    s_flips.fetch_add(1, std::memory_order_relaxed);
    fps_on_flip_request();
    if (s_framelog) frame_log();
}

constexpr unsigned kPadCross = 0x4000u, kPadStart = 0x0008u;
volatile bool s_auto_done = false;

extern "C" int des_ui_start_screen_up(void);   /* port/ui/src/des_ui.cpp */

bool auto_continue_on()   /* live: the settings window can turn it on or off before the first level */
{
    static bool on = false;
    PS3_KNOB(on, env_on("DES_AUTO_CONTINUE"));
    return on;
}

unsigned auto_continue_buttons()
{
    /* From settings.ini the start screen still shows: press nothing until Play. */
    if (s_auto_done || !auto_continue_on() || des_ui_start_screen_up()) return 0;
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() % 600;
    if (ms < 100 || (ms >= 200 && ms < 300)) return kPadCross;
    if (ms >= 400 && ms < 500) return kPadStart;
    return 0;
}

extern "C" unsigned des_mm_pad_buttons(void);   /* des_menu_mouse.cpp: the clicks in the game's menus */

unsigned title_buttons()
{
    return auto_continue_buttons() | des_mm_pad_buttons();
}

void auto_continue_on_open(const char* path)
{
    if (!s_auto_done && strstr(path, "/script/m") && strstr(path, ".luabnd")) {
        s_auto_done = true;   /* on or not: from the first level on there is nothing left to press through */
        if (auto_continue_on()) fprintf(stderr, "[des] DES_AUTO_CONTINUE: level loading (%s), pad released\n", path);
    }
}

/* The movement object's speed factor and its physics proxy (positions this
 * step and last, x y z). See des_move_post. */
constexpr uint32_t kMoveSpeedFactor = 0x128u;   /* float, scales root motion */
constexpr uint32_t kMoveProxy       = 0x10u;
constexpr uint32_t kProxyPos        = 0x10u, kProxyPrevPos = 0x20u;
constexpr float    kStuckSpeed      = 1.0f;     /* m/s */
constexpr float    kStuckFloor      = 0.5f, kStuckRate = 0.8f, kFreeRate = 1.2f;

float vm_readf(uint32_t ea)
{
    const uint32_t bits = vm_read32(ea);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void vm_writef(uint32_t ea, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    vm_write32(ea, bits);
}

/* Redo the game's stuck test with the step's real length. f0 is the factor
 * before 0x002EC2B8 ran; the factor it left tells which branch it took. */
void move_stuck_real_time(uint32_t mv, float f0, float dt)
{
    const float f1 = vm_readf(mv + kMoveSpeedFactor);
    const bool took_stuck = f0 > kStuckFloor ? f1 == f0 * kStuckRate : f1 == kStuckFloor;
    const bool took_free  = f0 < 1.0f && f1 == f0 * kFreeRate;
    if (!(took_stuck || took_free) || !(dt > 0.0f)) return;
    const uint32_t proxy = vm_read32(mv + kMoveProxy);
    if (!proxy) return;
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 3; i++) {
        const float c = vm_readf(proxy + kProxyPos + 4 * i) - vm_readf(proxy + kProxyPrevPos + 4 * i);
        d2 += c * c;
    }
    const float d = std::sqrt(d2);
    if (took_stuck && d * 30.0f >= 1.0f) return;   /* not the game's stuck test after all */
    const bool stuck = took_stuck && d < kStuckSpeed * dt;
    const float k = dt * 30.0f;   /* the rates are per 30 Hz step */
    float f;
    if (stuck) f = f0 > kStuckFloor ? f0 * std::pow(kStuckRate, k) : kStuckFloor;
    else       f = f0 < 1.0f ? f0 * std::pow(kFreeRate, k) : 1.0f;
    vm_writef(mv + kMoveSpeedFactor, f);
}

/* The loading screens' two soul icons by the small logo, bottom left. Each
 * soul is a flipbook of 100 cells on a 16 x 8 sheet, the two 50 apart, and
 * the game moves each one cell a frame without looking at the frame's step,
 * so above 30 fps they turn fps/30 times as fast. With DES_FPS set the hooks
 * below count the frame in 30 Hz steps, take whole steps, rounded, carry the
 * rest to the next frame, and set the cells back before the game's own +1 so
 * that it lands where that many steps would. At 30 fps that is one step a
 * frame, as on the PS3. DES_UIFIX=0 turns them off live. */
constexpr uint32_t kSoulCells = 100;

bool ui_fix_on()
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_UIFIX", 1) != 0);
    return on && s_fps > 0;
}

/* dt seconds in whole 30 Hz steps, the rest kept on carry; -1 out of range. */
int souls_steps(float dt, float& carry)
{
    const float steps = dt * 30.0f;
    if (!(steps >= 0.0f && steps < 30.0f)) return -1;
    carry += steps;
    const int n = (int)carry;
    carry -= (float)n;
    return n;
}

/* The cell from which the game's +1 (0 after 99) lands n steps past cell. */
uint32_t soul_cell_before(uint32_t cell, int n)
{
    return (cell + (uint32_t)n) % kSoulCells - 1u;
}

/* Boot: the frame update calls 0x0015AE10 once a frame while the boot
 * loading screen is up. It moves the screen's fade (+0x1C) one step a call,
 * in or out by a flag, clamped to [0, max], and calls 0x0015AA00, which
 * steps the souls' cells (+0x20, +0x24) and draws the logo and the souls.
 * The fade is set back the same way. */
constexpr uint32_t kLoadSouls  = 0x0015AE10u;
constexpr uint32_t kSoulFade   = 0x1Cu;          /* float */
constexpr uint32_t kSoulCell[] = {0x20u, 0x24u};

void load_souls_rate(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    static float carry = 0.5f;   /* so the cells round: one a frame at 30 fps */
    const float dt = vm_readf(kFrameDt);
    const int n = ui_fix_on() && obj ? souls_steps(dt, carry) : -1;
    if (n >= 0) {
        for (const uint32_t off : kSoulCell) {
            const uint32_t cell = vm_read32(obj + off);
            if (cell < kSoulCells) vm_write32(obj + off, soul_cell_before(cell, n));
        }
        /* 0x0015AE10's constants: the fade's step and the fade-out flag */
        const uint32_t consts = vm_read32((uint32_t)ctx->gpr[2] - 0x7C90u);
        const float more = vm_readf(consts - 0x7EE4u) * (dt * 30.0f - 1.0f);
        const bool out = vm_read8(vm_read32(consts - 0x7F7Cu)) != 0;
        vm_writef(obj + kSoulFade, vm_readf(obj + kSoulFade) + (out ? -more : more));
    }
    func_0015AE10(ctx);
}

/* Level loads: the NowLoading dialog draws the souls as layout parts.
 * soul_0 to soul_99 hold the cells, and icon_soul1 and icon_soul2 take the
 * images of the cells at +0xF8 and +0xFC (0x008CCB08). The dialog's update,
 * 0x008CD078 (vtable 0x0186FC04, dt in f1), steps both cells once a call
 * while the dialog is up, and sets them to 0 and 50 while it is hidden; its
 * fade goes by dt already. The cells go back to what they were when the
 * update skipped them. */
constexpr uint32_t kNowLoadingUpdate = 0x008CD078u;
constexpr uint32_t kNowLoadingCell[] = {0xF8u, 0xFCu};

void nowloading_souls_rate(ppu_context* ctx)
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    static float carry = 0.5f;
    const int n = ui_fix_on() && dlg ? souls_steps((float)ctx->fpr[1], carry) : -1;
    uint32_t was[2] = {}, set[2] = {};
    bool moved[2] = {false, false};
    for (int i = 0; i < 2 && n >= 0; ++i) {
        was[i] = vm_read32(dlg + kNowLoadingCell[i]);
        if (was[i] < kSoulCells) {
            set[i] = soul_cell_before(was[i], n);
            vm_write32(dlg + kNowLoadingCell[i], set[i]);
            moved[i] = true;
        }
    }
    func_008CD078(ctx);
    for (int i = 0; i < 2; ++i)
        if (moved[i] && vm_read32(dlg + kNowLoadingCell[i]) == set[i]) vm_write32(dlg + kNowLoadingCell[i], was[i]);
}

/* Menu text too long for its box scrolls sideways, such as the key guide
 * along the bottom of the inventory. ObjScrollMsg's update, 0x00798CD8
 * (vtable 0x018690F0, dt in f1), moves the text's offset (+0x20) 1.5 px to
 * the left a call while the message (+0x28) is the one it had at the last
 * call (+0x2C), and sets the offset to 0 when the message changes. Its slot
 * 5, 0x00798AC8, places the text by the offset when it is drawn: 90 px of
 * hold at the start, the overflow, 90 px of hold at the end, then the offset
 * starts over at 0. It compares ranges, so a part step is fine. The hook
 * makes the step 1.5 px per 30 Hz step of the frame (dt in f1, capped at
 * 50 ms like the game's step). DES_UIFIX=0 turns it off as well. */
constexpr uint32_t kScrollMsgUpdate = 0x00798CD8u;
constexpr uint32_t kScrollOffset    = 0x20u;   /* float, px */
constexpr uint32_t kScrollMsgLast   = 0x2Cu;   /* the message at the last call; -1 for none */

void scroll_msg_rate(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const float steps = (float)ctx->fpr[1] * 30.0f;
    const float was = obj ? vm_readf(obj + kScrollOffset) : 0.0f;
    const uint32_t last = obj ? vm_read32(obj + kScrollMsgLast) : 0u;
    /* 0x00798CD8's constants: the step */
    const float step = vm_readf(vm_read32((uint32_t)ctx->gpr[2] - 0x7208u) - 0x7FA0u);
    func_00798CD8(ctx);
    if (!obj || !ui_fix_on() || !(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    /* stepped: the same message as at the last call, and nothing else moved the offset */
    const uint32_t msg = vm_read32(obj + kScrollMsgLast);
    if (msg != 0xFFFFFFFFu && msg == last && vm_readf(obj + kScrollOffset) == was - step)
        vm_writef(obj + kScrollOffset, was - step * steps);
}

/* The HUD's soul counter after a kill. DsMenuDlgFEGetSoul's update,
 * 0x007A8C60 (vtable 0x018696C0, dt in f1), adds new souls to the gain to
 * show (+0x100), then moves what it shows of it (+0xFC) one step a call
 * towards the gain, the step by how much is left: 1 up to 10, 2 up to 100,
 * 12 up to 1,000, 112 up to 10,000 and so on (immediates, 0x007A8D24 to
 * 0x007A8D74 and 0x007A9078 to 0x007A9290), and stops at the gain. When it
 * gets there it holds by dt and clears. So +1,000 souls roll in 130 calls,
 * 4.3 s at 30 fps. The hook counts the frame in 30 Hz steps and takes that
 * many steps instead of the game's one, from where the counter was. The
 * number on screen was already set from the game's step, so while it rolls
 * it can be a step off the count; it ends on the total. DES_UIFIX=0 turns
 * it off. */
constexpr uint32_t kGetSoulUpdate = 0x007A8C60u;
constexpr uint32_t kSoulShown     = 0xFCu;    /* int: how much of the gain is shown */
constexpr uint32_t kSoulGain      = 0x100u;   /* int: the gain to show */

int32_t soul_step(int32_t left)
{
    const uint32_t a = left < 0 ? 0u - (uint32_t)left : (uint32_t)left;
    const int32_t step = a > 100000000u ? 11111112 : a > 10000000u ? 1111112 : a > 1000000u ? 111112
                       : a > 100000u ? 11112 : a > 10000u ? 1112 : a > 1000u ? 112 : a > 100u ? 12
                       : a > 10u ? 2 : 1;
    return left < 0 ? -step : step;
}

/* One call's move of the shown souls, as 0x007A8C60 makes it. */
int32_t soul_shown_after(int32_t shown, int32_t gain)
{
    if (shown == gain) return shown;
    const uint32_t left = (uint32_t)gain - (uint32_t)shown;
    const int32_t s = (int32_t)((uint32_t)shown + (uint32_t)soul_step((int32_t)left));
    if (gain > 0) return s < gain ? s : gain;
    return gain < s ? s : gain;
}

void get_soul_rate(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    static float carry = 0.5f;
    const int n = ui_fix_on() && obj ? souls_steps((float)ctx->fpr[1], carry) : -1;
    const int32_t was = obj ? (int32_t)vm_read32(obj + kSoulShown) : 0;
    func_007A8C60(ctx);
    if (n < 0 || n == 1) return;
    const int32_t gain = (int32_t)vm_read32(obj + kSoulGain);
    /* stepped: something to show, and the counter moved by the game's one step */
    if (gain == 0 || was == gain || (int32_t)vm_read32(obj + kSoulShown) != soul_shown_after(was, gain)) return;
    int32_t shown = was;
    for (int i = 0; i < n && shown != gain; ++i) shown = soul_shown_after(shown, gain);
    vm_write32(obj + kSoulShown, (uint32_t)shown);
}

/* The HP, MP and stamina bars, and the boss's and enemies' HP bars:
 * DsMenuDlgObjSmoothGauge (vtable 0x01868C28) moves the fill it shows
 * (+0x3C) towards the target (+0x20) by a fixed step a call, +0x44 down
 * (0.012: the trail after damage) and +0x4C up (0.038 or 0.048: a heal; 10
 * is at once), snapping to the target once it is within a step. That
 * happens in its draw callbacks, 0x0078EDC8, 0x0078EB60 and 0x0078F020
 * (slots 5 to 7), which get no dt; each bar on the HUD takes two calls a
 * frame, so the HP trail drains 0.72 bar/s at 30 fps. The hooks make a step
 * they see the step times the frame's 30 Hz steps (the frame step,
 * kFrameDt), not going past the target. DES_UIFIX=0 turns them off. */
constexpr uint32_t kGaugeDraw[]   = {0x0078EDC8u, 0x0078EB60u, 0x0078F020u};
constexpr uint32_t kGaugeTarget   = 0x20u;   /* float, 0 to 1 */
constexpr uint32_t kGaugeFill     = 0x3Cu;   /* float, 0 to 1 */
constexpr uint32_t kGaugeStepDown = 0x44u;   /* float, per call */
constexpr uint32_t kGaugeStepUp   = 0x4Cu;   /* float, per call */

template <void (*Draw)(ppu_context*)>
void gauge_fill_rate(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const float was = obj ? vm_readf(obj + kGaugeFill) : 0.0f;
    Draw(ctx);
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!obj || !ui_fix_on() || !(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    const float now = vm_readf(obj + kGaugeFill), target = vm_readf(obj + kGaugeTarget);
    const float down = vm_readf(obj + kGaugeStepDown), up = vm_readf(obj + kGaugeStepUp);
    if (now == was - down && target < now) {
        const float f = was - down * steps;
        vm_writef(obj + kGaugeFill, f > target ? f : target);
    } else if (now == was + up && target > now) {
        const float f = was + up * steps;
        vm_writef(obj + kGaugeFill, f < target ? f : target);
    }
}

/* A bar's length follows its max (the HP bar is longer with more max HP).
 * DsMenuDlgObjGauge's update, 0x00789BC0 (vtable 0x018688A8, dt in f1),
 * moves the length shown (+0x30) towards the target length (+0x2C) by half
 * the gap a call while it is over 2 px (0.5 at 0x018F2A04), then by 1 px,
 * then snaps. ObjSmoothGauge's update, 0x0078E2E8, runs it with a direct
 * call (0x0078E340), so both are hooked. The hooks make the half-gap step
 * 0.5 to the power of the frame's 30 Hz steps and the 1 px step that many
 * px. DES_UIFIX=0 turns them off. */
constexpr uint32_t kGaugeUpdate       = 0x00789BC0u;
constexpr uint32_t kSmoothGaugeUpdate = 0x0078E2E8u;
constexpr uint32_t kGaugeLenTarget    = 0x2Cu;   /* float, px */
constexpr uint32_t kGaugeLen          = 0x30u;   /* float, px */

template <void (*Update)(ppu_context*)>
void gauge_len_rate(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const float steps = (float)ctx->fpr[1] * 30.0f;
    const float was = obj ? vm_readf(obj + kGaugeLen) : 0.0f;
    Update(ctx);
    if (!obj || !ui_fix_on() || !(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    const float now = vm_readf(obj + kGaugeLen), target = vm_readf(obj + kGaugeLenTarget);
    const float gap = std::fabs(target - was), moved = std::fabs(now - was);
    float len;
    if (gap > 2.0f && std::fabs(moved - gap * 0.5f) <= 1e-3f)
        len = target + (was - target) * std::pow(0.5f, steps);
    else if (gap > 1.0f && gap <= 2.0f && std::fabs(moved - 1.0f) <= 1e-4f)
        len = now > was ? was + steps : was - steps;
    else
        return;
    if ((target - len) * (target - was) < 0.0f) len = target;   /* not past it */
    vm_writef(obj + kGaugeLen, len);
}

} // namespace

/* DES_DOF=0: no depth of field. The image-process update 0x00218428 blends
 * the area's two DOF_BANK rows (0x005EAF18) into a buffer on its stack at
 * r1 + 0xD0: far begin, far end, far strength (+0x08), near begin, near end,
 * near strength (+0x14), dispersion. The strengths are the rows' 0..100
 * bytes as floats, scaled by 0.01 on their way to the DoF pass (0x003E7290).
 * The lifter calls this after the first instruction past the blend
 * (0x002184DC, --insn-hooks); with both strengths 0 the pass blurs nothing.
 * Live. */
extern "C" void des_dof(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_DOF", 1) != 0);
    if (on) return;
    const uint32_t blend = (uint32_t)ctx->gpr[1] + 0xD0u;
    vm_writef(blend + 0x08, 0.0f);
    vm_writef(blend + 0x14, 0.0f);
}

#ifndef PS3_GAME_SPLIT   /* DES_SPLIT: the game DLL's (ps3emu/game_split.h) */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#endif

/* Lifted in place of 0x00273D38 (--override-funcs), which is only
 * `0x002EC2B8(*(r3 + 0x20), f1 = step)`: the movement object's step after
 * physics. Called from the movement worker (0x002A8408) and 0x00296970. */
extern "C" void des_move_post(ppu_context* ctx)
{
    const uint64_t lr = ctx->lr;
    const uint32_t mv = vm_read32((uint32_t)ctx->gpr[3] + 0x20);
    const float dt = (float)ctx->fpr[1];
    const float f0 = vm_readf(mv + kMoveSpeedFactor);
    ctx->gpr[3] = mv;
    ctx->lr = 0x00273D4Cu;
    func_002EC2B8(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
    ctx->lr = lr;
    if (s_fps > 0) move_stuck_real_time(mv, f0, dt);
}

/* DES_MOTION_BLUR=0: no camera motion blur. The blur stage (0x00217E68)
 * tests a byte of its object (+0x1D4) and, when it is set, branches to the
 * blur at 0x00217F50 (bne cr7); otherwise it hands the stage a fixed vector
 * through 0x003E6ED8. RPCS3's "Disable Motion Blur" patch turns that bne
 * into a beq. The lifter calls this after the cmpwi at 0x00217E70
 * (--insn-hooks); flipping cr7's EQ bit there does the same. Live. */
extern "C" void des_motion_blur(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_MOTION_BLUR", 1) != 0);
    if (!on) ppu_cr_set(ctx, 7, (uint8_t)(ppu_cr_get(ctx, 7) ^ 2u));   /* cr7's EQ = 2 */
}

/* The cameras ease towards their targets by a fixed fraction a frame,
 * cur += (target - cur) * k, with k tuned for 30 fps (the follow camera's
 * chase is k = 0.1 at 0x0011C2BC). At 240 fps that is eight steps where the
 * game takes one, so the camera catches up eight times as fast. The lifter
 * calls this after each such load of k (--insn-hooks, see docs/28); it puts
 * k' = 1 - (1 - k)^(dt * 30) in the register, the fraction that leaves the
 * same remainder per second as k does at 30 fps. Main thread. DES_CAMFIX=0
 * turns it off live; DES_CAMLOG=1 counts the sites. */
extern "C" void des_cam_rate(ppu_context* ctx, uint32_t addr)
{
    static int on = 1, log = 0;
    PS3_KNOB(on, ps3_env_int("DES_CAMFIX", 1) != 0);
    PS3_KNOB(log, env_on("DES_CAMLOG"));
    if (log) {
        static uint32_t sites[32];
        static unsigned counts[32], total;
        for (int i = 0; i < 32; i++)
            if (sites[i] == addr || !sites[i]) { sites[i] = addr; counts[i]++; break; }
        if (++total % 20000 == 0) {
            fprintf(stderr, "[camlog] dt=%.4f", vm_readf(kFrameDt));
            for (int i = 0; i < 32 && sites[i]; i++) fprintf(stderr, " %X:%u", sites[i], counts[i]);
            fputc('\n', stderr);
        }
    }
    if (!on || s_fps <= 0) return;
    const uint32_t insn = vm_read32(addr);
    if ((insn >> 26) != 48) return;   /* lfs only */
    const int fd = (insn >> 21) & 31;
    const double k = ctx->fpr[fd];
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!(k > 0.0 && k < 1.0) || !(steps > 0.0f) || steps == 1.0f) return;
    ctx->fpr[fd] = (float)(1.0 - std::pow(1.0 - k, (double)steps));
}

/* The foot IK (issue #140, docs/92). A character's movement object places its
 * feet once an update: the driver 0x002F5B08 calls Havok's
 * hkaFootPlacementIkSolver::doFootPlacement (0x00FF4DC0) for each leg, then
 * moves the pelvis. Its smoothing is a fixed amount an update, tuned for 30 fps:
 * the pelvis offset takes 0.2 of the lower foot's error (or keeps 0.6 of itself
 * when neither foot hits the ground), the solver's weight eases by its onOffGain
 * (0.8), and a foot's height by its gains' product (0.64). At 95 fps that
 * settles three times as fast, so the hips and feet follow every bump. The
 * lifter calls this after each of those loads (--insn-hooks): an ease k becomes
 * 1 - (1 - k)^(30 dt) and a keep r becomes r^(30 dt), with dt the driver's own
 * (kept at its start, 0x002F5B14, for the solver's sites inside the same call),
 * so a second goes as at 30 fps. DES_IKFIX=0 turns it off live; DES_IKLOG=<file>
 * writes the player's pelvis offset at each update. */
static PPU_THREAD_LOCAL float s_ik_dt;
static void ik_log(ppu_context* ctx, uint32_t addr)
{
    static int open = 0;
    static FILE* f = nullptr;
    if (!open) {
        open = 1;
        const char* path = getenv("DES_IKLOG");
        if (path && *path && strcmp(path, "0") != 0 && (f = fopen(path, "w")) != nullptr)
            fprintf(f, "# ms dt offset error (air: neither foot hits)\n");
    }
    if (!f) return;
    uint32_t chr = vm_read32(kChrMan);
    if (!guest_ptr_ok(chr) || !guest_ptr_ok(chr = vm_read32(chr + 0x1BC)) || vm_read32(chr + 0x20) != ctx->gpr[31])
        return;   /* r31 is the movement object at both sites; only the player's */
    static const auto t0 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (addr == 0x002F5DB8u)   /* f1 = the offset; r9 points at the lower foot's error (0x002F5DBC loads it) */
        fprintf(f, "%.1f %.4f %.4f %.4f\n", ms, s_ik_dt, ctx->fpr[1], vm_readf((uint32_t)ctx->gpr[9]));
    else
        fprintf(f, "%.1f %.4f %.4f air\n", ms, s_ik_dt, ctx->fpr[1]);
    fflush(f);   /* a test driver lines its phases up with the last line */
}

/* The ankle's tilt (docs/92). After its two rays the solver turns each ankle to
 * the ground's normal under the foot: the normal of whichever surface is higher,
 * the ankle ray's (r1+0xC0) or the toe ray's (r1+0x80), picked fresh at every
 * update with nothing carried over (0x00FF5BAC). A ray that grazes a step edge,
 * a rubble block or a wall returns that face's normal, up to 90 degrees from up,
 * and the foot turns nearly on its side for an update or three: the twitching
 * foot on uneven ground. The original game does this at 30 fps too.
 * DES_FOOTFIX=1 (default 0, the original): a normal steeper than 45 degrees is
 * not used (the solver keeps the last update's, or straight up), and the rest
 * eases from the last update's toward the new one, keeping half the difference
 * per 30 fps update (0.5^(30 dt) at other rates). DES_FOOTTILT=<degrees> and
 * DES_FOOTSMOOTH=<share kept> set the two parts (0 turns one off). It runs at
 * 0x00FF54D4, on the solver's path with a hit and its weight above 0, after the
 * normal is picked into r1+0xA0 and before the ankle is turned (0x00FF59BC);
 * the solver stores the result at +0xA0 itself (0x00FF56C0), where the next
 * update finds it. Every character with foot IK gets it. DES_FOOTLOG=<file>
 * writes the player's normals at each update. */
static PPU_THREAD_LOCAL bool s_ik_player;
static FILE* foot_log_file()
{
    static int open = 0;
    static FILE* f = nullptr;
    if (!open) {
        open = 1;
        const char* path = getenv("DES_FOOTLOG");
        if (path && *path && strcmp(path, "0") != 0 && (f = fopen(path, "w")) != nullptr)
            fprintf(f, "# ms solver dt weight picked(x y z) used(x y z) toe(x y z) ankle(x y z) player(x y z)\n");
    }
    return f;
}
struct FootFix { float cos_tilt; float keep; };   /* cos_tilt -2: no limit; keep 0: no easing */
static FootFix foot_fix()
{
    const bool on = ps3_env_int("DES_FOOTFIX", 0) != 0;
    const char* t = getenv("DES_FOOTTILT");
    const char* s = getenv("DES_FOOTSMOOTH");
    float tilt = (t && *t) ? (float)atof(t) : (on ? 45.0f : 0.0f);
    float keep = (s && *s) ? (float)atof(s) : (on ? 0.5f : 0.0f);
    if (!(tilt > 0.0f && tilt < 90.0f)) tilt = 0.0f;
    if (!(keep > 0.0f && keep < 1.0f)) keep = 0.0f;
    return {tilt > 0.0f ? std::cos(tilt * 3.14159265f / 180.0f) : -2.0f, keep};
}
static void foot_normal(ppu_context* ctx)
{
    static FootFix fix = {-2.0f, 0.0f};
    PS3_KNOB(fix, foot_fix());
    FILE* log = s_ik_player ? foot_log_file() : nullptr;
    if (fix.cos_tilt < -1.5f && fix.keep <= 0.0f && !log) return;
    const uint32_t solver = (uint32_t)ctx->gpr[30], sp = (uint32_t)ctx->gpr[1], n_ea = (sp + 0xA0u) & ~0xFu;
    float h[3], p[3], n[3], hl = 0.0f, pl = 0.0f;
    for (int i = 0; i < 3; i++) {
        h[i] = n[i] = vm_readf(n_ea + 4 * i);
        p[i] = vm_readf(solver + 0xA0 + 4 * i);
        hl += h[i] * h[i]; pl += p[i] * p[i];
    }
    const bool h_ok = hl > 0.25f && hl < 4.0f, p_ok = pl > 0.25f && pl < 4.0f;   /* +Y is up */
    const bool p_flat = p_ok && p[1] >= fix.cos_tilt * std::sqrt(pl);
    bool changed = false;
    if (h_ok && n[1] < fix.cos_tilt * std::sqrt(hl)) {   /* too steep to stand on */
        for (int i = 0; i < 3; i++) n[i] = p_flat ? p[i] : (i == 1 ? 1.0f : 0.0f);
        changed = true;
    }
    const float steps = s_ik_dt * 30.0f;
    if (fix.keep > 0.0f && steps > 0.0f && h_ok && p_flat) {
        const float a = 1.0f - std::pow(fix.keep, steps);   /* the share of the difference taken this update */
        for (int i = 0; i < 3; i++) n[i] = p[i] + (n[i] - p[i]) * a;
        changed = true;
    }
    if (changed) {
        const float m = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (m > 1e-6f) {
            const float r = 1.0f / std::sqrt(m);
            for (int i = 0; i < 3; i++) { n[i] *= r; vm_writef(n_ea + 4 * i, n[i]); }
        } else {
            for (int i = 0; i < 3; i++) n[i] = h[i];
        }
    }
    if (log) {
        static const auto t0 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        float x = 0, y = 0, z = 0, speed = 0;
        int stamina = 0;
        player_pos(&x, &y, &z, &stamina, &speed);
        fprintf(log, "%.1f %08X %.4f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.2f %.2f %.2f\n",
                ms, solver, s_ik_dt, vm_readf(solver + 0x90), h[0], h[1], h[2], n[0], n[1], n[2], vm_readf(sp + 0x80),
                vm_readf(sp + 0x84), vm_readf(sp + 0x88), vm_readf(sp + 0xC0), vm_readf(sp + 0xC4), vm_readf(sp + 0xC8),
                x, y, z);
        fflush(log);
    }
}

extern "C" void des_ik_rate(ppu_context* ctx, uint32_t addr)
{
    if (addr == 0x002F5B14u) {   /* fmr f30, f1: the driver's dt; r3 is still the movement object */
        s_ik_dt = (float)ctx->fpr[30];
        if (foot_log_file()) {
            uint32_t chr = vm_read32(kChrMan);
            s_ik_player = guest_ptr_ok(chr) && guest_ptr_ok(chr = vm_read32(chr + 0x1BC)) &&
                          vm_read32(chr + 0x20) == (uint32_t)ctx->gpr[3];
        }
        return;
    }
    if (addr == 0x00FF54D4u) foot_normal(ctx);
    if (addr == 0x002F5DB8u || addr == 0x002F5CCCu) ik_log(ctx, addr);
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_IKFIX", 1) != 0);
    if (!on || s_fps <= 0) return;
    const int fd = (vm_read32(addr) >> 21) & 31;   /* the lfs's or the fmuls's target */
    const double k = ctx->fpr[fd];
    const float steps = s_ik_dt * 30.0f;
    if (!(k > 0.0 && k < 1.0) || !(steps > 0.0f) || steps == 1.0f) return;
    ctx->fpr[fd] = (float)(addr == 0x002F5CCCu ? std::pow(k, (double)steps) : 1.0 - std::pow(1.0 - k, (double)steps));
}

/* The lower body's turn against the upper body (docs/28, "Locked-on direction
 * changes above 30 fps"). Moving locked on, the locomotion controller (vtable
 * 0x0185C338) plays one of four direction clips and turns the Master,
 * Upper_Root and Neck bones by the angle between the move and the clip. That
 * angle jumps when the clip changes (about 85 degrees on a diagonal), and the
 * bone turner (0x003D3BC0) eases each bone toward it by a fixed share an update:
 * cur += (target - cur) * k, with k = 0.35 (the controller's +0x154, stored to
 * each bone at 0x00322AB0). At 30 fps that settles in about 0.18 s, alongside the
 * clips' 0.2 s blend; at 240 fps in about 20 ms, so the body snaps round while
 * the legs still blend. The lifter calls this after the load of k (0x003D42BC):
 * k becomes 1 - (1 - k)^(30 dt), with dt the game's frame time, so a second
 * eases as at 30 fps. The turner's other user, a two-bone controller
 * (0x0031B56C), gets the same. DES_TWISTFIX=0 turns it off live. */
extern "C" void des_twist_rate(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_TWISTFIX", 1) != 0);
    if (!on || s_fps <= 0) return;
    const int fd = (vm_read32(addr) >> 21) & 31;   /* the lfs's target */
    const double k = ctx->fpr[fd];
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!(k > 0.0 && k < 1.0) || !(steps > 0.0f) || steps == 1.0f) return;
    ctx->fpr[fd] = (float)(1.0 - std::pow(1.0 - k, (double)steps));
}

/* DES_PHYS_HZ=<n> (default 30; 0 = step every frame): at most n Havok steps
 * a second. The physics update 0x001D1E30 steps the world once a frame with
 * the frame's dt (f31). Havok counts its solver passes and its sleep checks
 * in steps, so at 400 fps a ragdoll gets 13 times the passes and 1/13 of the
 * time to settle: corpses twitch after death and sink through the floor when
 * touched. The hook runs after 0x001D1E64 (lbz r28, 0x14(r31), the update's
 * own skip byte). Frames shorter than 1/n add up; the world steps 1/n each
 * time they reach it and skips the step (r28 = 1) otherwise. A frame of 1/n
 * or longer steps its own dt, so 30 fps is the game as it was. */
static int phys_hz()
{
    static int hz = 30;
    PS3_KNOB(hz, ps3_env_int("DES_PHYS_HZ", 30));
    return hz;
}
static std::atomic<double> s_phys_clock{0.0};   /* game seconds, summed frame dts */
static std::atomic<bool> s_phys_capped{false};  /* this frame was shorter than the step */
static std::atomic<bool> s_phys_ride{false};    /* this frame steps uncapped: the player rides a moving body */
static std::atomic<double> s_phys_world_now{0.0}; /* under the cap: the world's time that matches the game's */

/* The cap and moving bodies. An elevator, a lift, a paternoster or a turning
 * bridge is a keyframed body, moved by the world step, so under the cap it
 * moves in 1/n steps and the player standing on it with it: on the Palace 1-4
 * elevator at 200 fps, a 6 cm jump every 33 ms with still frames between. So
 * while the player is in or on a moving keyframed body, and for 0.25 s of game
 * time after, the world steps every frame, as with DES_PHYS_HZ=0.
 * The player's character proxy does not list the floor the player stands on
 * (its contacts stay empty on level ground and on a platform's floor; only a
 * side contact shows), so the test is by position: the world
 * (*(*(r31 + 0x10) + 4) in the physics update, vtable 0x018B4F98) lists its
 * active simulation islands at +0x28 (data, size), and an island its bodies
 * at +0x48. A moving keyframed body (vtable 0x018B4810) has inverse mass 0
 * (+0x19C) and a linear (+0x1A0) or angular (+0x1B0) velocity. Characters'
 * own bodies are keyframed too, on collision layer 8 (the filter word +0x2C,
 * low 5 bits), and layer 6 once the character is dead; both are left out.
 * The lifts and paternosters seen are on layer 12. The body's bounding
 * box is held as 32-bit broadphase integers (+0x30 min, +0x40 max); the
 * broadphase (*(world + 0x54)) maps them back with its offset (+0x70) and
 * scale (+0x90). The player is on the body when the player's position
 * (P + 0x10, P = *(*(chr + 0x20) + 0x10)) is in the box widened by 0.3 m.
 * DES_PHYS_RIDE=0 keeps the cap there too; DES_PHYS_RIDE_LOG=1 prints each
 * start and end. */
static uint32_t player_moving_body(uint32_t world)
{
    uint32_t p = vm_read32(kChrMan);
    if (!guest_ptr_ok(p) || !guest_ptr_ok(p = vm_read32(p + 0x1BC)) || !guest_ptr_ok(p = vm_read32(p + 0x20)) ||
        !guest_ptr_ok(p = vm_read32(p + 0x10)))
        return 0;
    const float pos[3] = {vm_readf(p + 0x10), vm_readf(p + 0x14), vm_readf(p + 0x18)};
    if (!guest_ptr_ok(world) || vm_read32(world) != 0x018B4F98u) return 0;
    const uint32_t bp = vm_read32(world + 0x54);
    if (!guest_ptr_ok(bp)) return 0;
    double off[3], scale[3];
    for (int k = 0; k < 3; k++) {
        off[k] = vm_readf(bp + 0x70 + 4 * k); scale[k] = vm_readf(bp + 0x90 + 4 * k);
        if (!(scale[k] > 0.0)) return 0;
    }
    const uint32_t islands = vm_read32(world + 0x28), ni = vm_read32(world + 0x2C);
    if (!guest_ptr_ok(islands) || ni > 1024) return 0;
    for (uint32_t i = 0; i < ni; i++) {
        const uint32_t isl = vm_read32(islands + 4 * i);
        if (!guest_ptr_ok(isl)) continue;
        const uint32_t bodies = vm_read32(isl + 0x48), n = vm_read32(isl + 0x4C);
        if (!guest_ptr_ok(bodies) || n > 4096) continue;
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t b = vm_read32(bodies + 4 * j);
            if (!guest_ptr_ok(b) || vm_read32(b) != 0x018B4810u || vm_readf(b + 0x19C) != 0.0f) continue;
            const uint32_t layer = vm_read32(b + 0x2C) & 31;
            if (layer == 8 || layer == 6) continue;
            float v = 0.0f, w = 0.0f;
            for (int k = 0; k < 3; k++) {
                const float a = vm_readf(b + 0x1A0 + 4 * k), r = vm_readf(b + 0x1B0 + 4 * k);
                v += a * a; w += r * r;
            }
            if (!(v > 1e-4f || w > 1e-4f)) continue;
            bool in = true;
            for (int k = 0; k < 3 && in; k++) {
                const double lo = vm_read32(b + 0x30 + 4 * k) / scale[k] - off[k];
                const double hi = vm_read32(b + 0x40 + 4 * k) / scale[k] - off[k];
                in = pos[k] >= lo - 0.3 && pos[k] <= hi + 0.3;
            }
            if (in) return b;
        }
    }
    return 0;
}

extern "C" void des_phys_rate(ppu_context* ctx, uint32_t)
{
    static int ride_on = 1, ride_log = 0;
    PS3_KNOB(ride_on, ps3_env_int("DES_PHYS_RIDE", 1));
    PS3_KNOB(ride_log, ps3_env_int("DES_PHYS_RIDE_LOG", 0));
    int hz = phys_hz();
    static double acc = 0.0, ride_until = 0.0;
    const double dt = ctx->fpr[31];
    if (dt > 0.0) s_phys_clock.store(s_phys_clock.load() + dt);
    uint32_t world = vm_read32((uint32_t)ctx->gpr[31] + 0x10);
    world = guest_ptr_ok(world) ? vm_read32(world + 4) : 0;
    static uint32_t world_seen = 0;
    if (ride_log && world != world_seen) {
        world_seen = world;
        fprintf(stderr, "[physride] world %08X (vtable %08X)\n", world, guest_ptr_ok(world) ? vm_read32(world) : 0u);
    }
    const uint32_t body = hz > 0 && ride_on && dt > 0.0 && dt < 1.0 / hz ? player_moving_body(world) : 0;
    if (body) ride_until = s_phys_clock.load() + 0.25;
    const bool ride = hz > 0 && ride_on && s_phys_clock.load() < ride_until;
    if (ride_log && ride != s_phys_ride.load())
        fprintf(stderr, "[physride] %s at %.2f s (body %08X, filter %08X)\n", ride ? "every frame" : "capped again",
                s_phys_clock.load(), body, body ? vm_read32(body + 0x2C) : 0u);
    s_phys_ride.store(ride);
    if (ride) hz = 0;
    const double h = hz > 0 ? 1.0 / hz : 0.0;
    s_phys_capped.store(hz > 0 && dt > 0.0 && dt < h);
    if (hz <= 0 || ctx->gpr[28] != 0) { acc = 0.0; return; }
    if (!(dt > 0.0) || dt >= h) { acc = 0.0; return; }
    /* for DES_PARTINTERP: the world's time before this frame's step (the
     * world *(*(r31 + 0x10) + 4), vtable 0x018B4F98; its simulation, +8;
     * the time, +0x14) */
    uint32_t pw = vm_read32((uint32_t)ctx->gpr[31] + 0x10);
    pw = guest_ptr_ok(pw) ? vm_read32(pw + 4) : 0;
    const uint32_t sim = guest_ptr_ok(pw) && vm_read32(pw) == 0x018B4F98u ? vm_read32(pw + 8) : 0;
    const double wt = guest_ptr_ok(sim) ? vm_readf(sim + 0x14) : 0.0;
    acc += dt;
    if (acc < h) {
        ctx->gpr[28] = 1;              /* skip this frame's step */
        s_phys_world_now.store(wt + acc);
        return;
    }
    ctx->fpr[31] = (float)h;
    acc -= h;
    if (acc >= h) acc = 0.0;
    s_phys_world_now.store(wt + h + acc);
}

/* With DES_PHYS_HZ: the passes that drive a character's bodies to its
 * animation pose (0x001EF670, 0x001DF7C0, through Havok's hard keyframe
 * 0x0131BAF8) set each body's velocity to the distance to the pose times
 * 1/dt. Under the cap the world steps 1/n, so a frame's 1/dt would carry the
 * body n*dt times too far. The hook runs after their fdivs f31, f0, f1 and
 * makes 1/dt n when the frame is shorter than 1/n (not while the world steps
 * every frame for a ride). */
extern "C" void des_phys_kf(ppu_context* ctx, uint32_t)
{
    const int hz = s_phys_ride.load() ? 0 : phys_hz();
    if (hz <= 0 || !(ctx->fpr[1] > 0.0) || ctx->fpr[1] >= 1.0 / hz) return;
    ctx->fpr[31] = (float)(ctx->fpr[0] * hz);
}

/* DES_RAGINTERP (default 1; 0 = off): with DES_PHYS_HZ capping the steps,
 * physics objects would be drawn at 30 Hz. A rigid body's swept transform
 * holds its pose at the start and the end of its last step (centre of mass 0
 * at +0x120, whose w is the step's start time, centre of mass 1 at +0x130,
 * whose w is 1 / the step length, rotations at +0x140 and +0x150, local
 * centre of mass at +0x160). Two hooks draw the pose between the two instead,
 * blended by the game time since the body last got a new step over the step
 * length, so the drawn pose runs one step behind:
 *  - ragdolls: Havok's pose readers (0x012D85E8, 0x012D9288) turn each bone's
 *    body transform into a position and rotation (0x0109FB30, through
 *    0x011801C8); the hook runs after that call;
 *  - everything else the game draws from a body (breakables, debris, loose
 *    objects): the game reads the transform through 0x0109F250, which copies
 *    an hkTransform into a 4x4 matrix; the hook runs after its last store.
 * Only moving dynamic bodies are touched: a body that sleeps or stops
 * stepping shows its own transform, keyframed bodies (inverse mass 0) are
 * left to the game, and frames of a step or longer are left alone. */
namespace {
struct RagSeen { float t0; double at; };
std::mutex s_rag_mu;
std::unordered_map<uint32_t, RagSeen> s_rag_seen;
void vm_read4f(uint32_t ea, float* v) { for (int i = 0; i < 4; i++) v[i] = vm_readf(ea + 4 * i); }
/* v rotated by the quaternion q (x, y, z, w) */
void quat_rotate(const float* q, const float* v, float* r)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    r[0] = v[0] + w * tx + (y * tz - z * ty);
    r[1] = v[1] + w * ty + (z * tx - x * tz);
    r[2] = v[2] + w * tz + (x * ty - y * tx);
}
}
static bool rag_interp_on()
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_RAGINTERP", 1));
    return on && s_phys_capped.load();
}
/* Game seconds since the body got the step that starts at t0 (its swept
 * transform's start time). */
static double body_since(uint32_t body, float t0)
{
    const double now = s_phys_clock.load();
    std::lock_guard<std::mutex> lk(s_rag_mu);
    if (s_rag_seen.size() > 8192) s_rag_seen.clear();
    RagSeen& s = s_rag_seen[body];
    if (s.t0 != t0) { s.t0 = t0; s.at = now; }
    return now - s.at;
}
/* The body's pose between the start and the end of its last step: the
 * position of its origin in pos, its rotation (x, y, z, w) in q. False when
 * it shows its own transform. */
static bool body_pose_between(uint32_t body, float* pos, float* q)
{
    float c0[4], c1[4], q0[4], q1[4], cl[4];
    vm_read4f(body + 0x120, c0); vm_read4f(body + 0x130, c1);
    vm_read4f(body + 0x140, q0); vm_read4f(body + 0x150, q1);
    vm_read4f(body + 0x160, cl);
    if (!(c1[3] > 0.0f)) return false;
    const double span = 1.0 / c1[3], since = body_since(body, c0[3]);
    if (since >= span) return false;
    static int log = 0; static std::atomic<int> nlog{0};
    PS3_KNOB(log, ps3_env_int("DES_RAGINTERP_LOG", 0));
    if (log && (nlog++ < 40 || nlog.load() % 4000 == 0)) {
        /* the swept end against the body's transform: origin and first column */
        float e[3], col[3], ex[3] = {1.0f, 0.0f, 0.0f};
        quat_rotate(q1, cl, e); quat_rotate(q1, ex, col);
        fprintf(stderr, "[raginterp] body %08X span %.4f since %.4f end %.3f %.3f %.3f transform %.3f %.3f %.3f"
                " col0 %.3f %.3f %.3f transform %.3f %.3f %.3f\n", body, span, since,
                c1[0] - e[0], c1[1] - e[1], c1[2] - e[2], vm_readf(body + 0x110), vm_readf(body + 0x114), vm_readf(body + 0x118),
                col[0], col[1], col[2], vm_readf(body + 0xE0), vm_readf(body + 0xE4), vm_readf(body + 0xE8));
    }
    const float a = (float)(since / span);
    const float d = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    const float s1 = d < 0.0f ? -a : a;
    float n = 0.0f;
    for (int i = 0; i < 4; i++) { q[i] = q0[i] * (1.0f - a) + q1[i] * s1; n += q[i] * q[i]; }
    if (!(n > 1e-12f)) return false;
    n = 1.0f / std::sqrt(n);
    for (int i = 0; i < 4; i++) q[i] *= n;
    float r[3];
    quat_rotate(q, cl, r);   /* origin = centre of mass - q * local centre of mass */
    for (int i = 0; i < 3; i++) pos[i] = c0[i] + (c1[i] - c0[i]) * a - r[i];
    return true;
}
static void rag_interp(uint32_t inst, uint32_t out, uint32_t idx)
{
    if (!rag_interp_on()) return;
    uint32_t map, bodies, bi, body;
    if (!guest_ptr_ok(inst) || !guest_ptr_ok(out) || !guest_ptr_ok(map = vm_read32(inst + 0x20)) ||
        !guest_ptr_ok(bodies = vm_read32(inst + 8))) return;
    if ((int32_t)(bi = vm_read32(map + 4 * idx)) < 0 || !guest_ptr_ok(body = vm_read32(bodies + 4 * bi))) return;
    float pos[3], q[4], qo[4];
    if (!body_pose_between(body, pos, q)) return;
    vm_read4f(out + 0x10, qo);
    if (q[0] * qo[0] + q[1] * qo[1] + q[2] * qo[2] + q[3] * qo[3] < 0.0f)
        for (int i = 0; i < 4; i++) q[i] = -q[i];      /* same sign as the converter's */
    for (int i = 0; i < 3; i++) vm_writef(out + 4 * i, pos[i]);
    for (int i = 0; i < 4; i++) vm_writef(out + 0x10 + 4 * i, q[i]);
}
/* After 0x012D86C4: ragdoll instance in r30, pose slot in r29, bone in r28. */
extern "C" void des_rag_pose_a(ppu_context* ctx, uint32_t)
{
    rag_interp((uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[28]);
}
/* After 0x012D9354: ragdoll instance in r29, pose slot in r30, bone in r28. */
extern "C" void des_rag_pose_b(ppu_context* ctx, uint32_t)
{
    rag_interp((uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[28]);
}
/* After 0x0109F2C4, the last store of the transform-to-matrix copy: r3 and r4
 * are the source and the destination plus 0x30. A source at +0xE0 of a moving
 * dynamic rigid body (vtable 0x018B4810) gets the in-between pose. */
extern "C" void des_body_xform(ppu_context* ctx, uint32_t)
{
    if (!rag_interp_on()) return;
    const uint32_t src = (uint32_t)ctx->gpr[3] - 0x30, dst = (uint32_t)ctx->gpr[4] - 0x30, body = src - 0xE0;
    if (!guest_ptr_ok(body) || !guest_ptr_ok(dst) || vm_read32(body) != 0x018B4810u || !(vm_readf(body + 0x19C) > 0.0f)) return;
    float pos[3], q[4];
    if (!body_pose_between(body, pos, q)) return;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float col[3][3] = {
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y)},
        {2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)},
        {2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)}};
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) vm_writef(dst + 0x10 * c + 4 * i, col[c][i]);
    for (int i = 0; i < 3; i++) vm_writef(dst + 0x30 + 4 * i, pos[i]);
}

/* DES_PARTINTERP (default 1; 0 = off): with DES_PHYS_HZ capping the steps.
 * The game reads where a character's part is (body i of the character's body
 * lists) through four getters (0x001DFB68, 0x001E1610, 0x001EFA20,
 * 0x001EFED0). An arrow stuck in a character follows one (0x00262DB0,
 * through 0x002E86D8 and 0x001EFA20), and so does an effect tied to a part.
 * Each samples the body's swept transform at the world's time (0x001E4228)
 * plus a frame, through Havok's approxTransformAt (0x011B1D50, by the stub
 * 0x000112E0, which nothing else calls). The world's time stands still
 * between steps, so these moved in 30 Hz steps while the character's model
 * moves every frame. The hook in the stub (r3 = the body's motion, body +
 * 0xD0; f1 = the time) moves the time on:
 *  - a keyframed body (inverse mass 0: a living character's parts, driven to
 *    the animation pose) by the game time the world has not stepped yet
 *    (des_phys_rate's world time for now, less the body's world's time:
 *    world at +8, its simulation at +8, the time at +0x14), so it carries on
 *    its last step and keeps up with the model. Not after a jump of more
 *    than 1 m in one step, or two steps or more behind;
 *  - a dynamic body (a ragdoll's parts) to the pose between the start and the
 *    end of its last step, where DES_RAGINTERP draws it.
 * DES_PARTINTERP_LOG=1 prints the first samples it moves. */
extern "C" void des_body_sample(ppu_context* ctx, uint32_t)
{
    static int on = 1, log = 0;
    PS3_KNOB(on, ps3_env_int("DES_PARTINTERP", 1));
    PS3_KNOB(log, ps3_env_int("DES_PARTINTERP_LOG", 0));
    if (!on || !s_phys_capped.load()) return;
    const uint32_t body = (uint32_t)ctx->gpr[3] - 0xD0;
    if (!guest_ptr_ok(body) || vm_read32(body) != 0x018B4810u) return;
    float c0[4], c1[4];
    vm_read4f(body + 0x120, c0); vm_read4f(body + 0x130, c1);
    if (!(c1[3] > 0.0f)) return;
    const double span = 1.0 / c1[3];
    const bool keyframed = !(vm_readf(body + 0x19C) > 0.0f);
    double add;
    if (keyframed) {
        const uint32_t world = vm_read32(body + 8);
        const uint32_t sim = guest_ptr_ok(world) && vm_read32(world) == 0x018B4F98u ? vm_read32(world + 8) : 0;
        if (!guest_ptr_ok(sim)) return;
        const float dx = c1[0] - c0[0], dy = c1[1] - c0[1], dz = c1[2] - c0[2];
        add = s_phys_world_now.load() - vm_readf(sim + 0x14);
        if (!(add > 0.0) || add >= 2.0 * span || dx * dx + dy * dy + dz * dz > 1.0f) return;
    } else {
        const double since = body_since(body, c0[3]);
        if (since >= span) return;
        add = since - span;
    }
    static std::atomic<int> nlog{0};
    if (log && nlog++ < 60)
        fprintf(stderr, "[partinterp] body %08X %s t %.4f step %.4f..%.4f -> %.4f\n", body, keyframed ? "kf" : "dyn",
                ctx->fpr[1], c0[3], c0[3] + span, ctx->fpr[1] + add);
    ctx->fpr[1] = (float)(ctx->fpr[1] + add);
}

/* DES_NETLOG=1: seconds since the first line, for des_net_sync's log. */
static double net_log_time()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/* What des_net_sync keeps about each remote player, by its NetworkManipulator
 * (eight at most; a new one takes the slot used longest ago). Main thread. */
struct NetRemote {
    uint32_t m;
    float x, z;          /* the target at the last packet */
    float acc;           /* seconds since it */
    float speed, gap;    /* the sender's speed, and the last gap between packets */
    float gap_avg;       /* those gaps averaged, 0 after a long one */
    float dir_x, dir_z;  /* the direction of the last move that went somewhere */
    unsigned last_use;
};
static NetRemote s_net_rem[8];

/* A vector register's word as the guest's float bits, and back (the register
 * holds guest bytes). */
static uint32_t net_swap(uint32_t b)
{
    return (b >> 24) | ((b >> 8) & 0xFF00u) | ((b << 8) & 0xFF0000u) | (b << 24);
}

static NetRemote* net_remote(uint32_t m)
{
    static unsigned use;
    NetRemote* r = nullptr;
    for (NetRemote& e : s_net_rem)
        if (e.m == m) { r = &e; break; }
    if (!r) {
        r = &s_net_rem[0];
        for (NetRemote& e : s_net_rem)
            if (e.last_use < r->last_use) r = &e;
        *r = NetRemote{m, vm_readf(m + 0x1D0), vm_readf(m + 0x1D8), 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0};
    }
    r->last_use = ++use;
    return r;
}

/* Once a frame, from SetMoveVector's entry: the sender's speed. The game
 * picks idle / walk / run / dash from the frame's chase move (speed below 0.01,
 * 2 and 6 m/s), and the chase restarts at each packet at distance / (1.2 *
 * the last gap) and stops dead when it reaches the last position early. With
 * packets 200 ms apart (30 fps, the game's rate) that holds steady; 40-60 ms
 * apart (a sender above 30 fps, or DES_NET_SYNC) the gaps jitter by a frame
 * or two, the chase arrives early on a fifth of the frames, and each arrival
 * shows idle for at least 0.2 s (the gait's hold): a walk that flickers into
 * idle, and a walk that reads as a run. The sender's speed is the distance
 * between its last two positions over their gap, averaged with the one
 * before: the speed the game's chase settles to (its 1.2 lead sets only how
 * far behind it trails). */
static void net_remote_frame(NetRemote* r, float dt)
{
    const uint32_t m = r->m;
    r->acc += dt;
    if (vm_readf(m + 0x21C) != 0.0f) return;   /* the update resets its gap timer on a packet */
    const float x = vm_readf(m + 0x1D0), z = vm_readf(m + 0x1D8);
    const float d = std::sqrt((x - r->x) * (x - r->x) + (z - r->z) * (z - r->z));
    const float gap = r->acc;
    r->x = x; r->z = z; r->acc = 0.0f;
    if (gap > 0.0f) {   /* (a second packet in the same frame: no gap to measure) */
        r->gap = gap;
        const float v = d / gap;
        r->speed = (d < 0.005f || v > 20.0f) ? 0.0f : (r->speed > 0.0f ? 0.5f * (r->speed + v) : v);
    }
}

/* The sender's speed while packets come under 150 ms apart and it moves; -1
 * for a packet with the same position (it stopped), a gap over 150 ms, or a
 * jump over 20 m/s (a warp): then the game's own value stands. */
static float net_remote_gait(const NetRemote* r)
{
    if (r->gap > 0.15f || r->acc > 0.15f || r->speed <= 0.0f) return -1.0f;
    return r->speed;
}

/* Remote players (docs/69, "Remote players"). The local player's position
 * goes to the other players in a type-1 packet every 6 game frames:
 * PadManipulator counts +0x198 down once a frame, sends at 0, and reloads it
 * with `li r0, 5` at 0x00313B24, so 5 packets a second at 30 fps. The
 * packet holds the position and nothing about time or speed. The receiving
 * NetworkManipulator (OnPacket 0x0030C090) chases the last position at
 * distance / (1.2 * the gap since the previous packet), so after a stop the
 * other copy shows the walk dying out over about 0.7 s, as on a PS3.
 *
 * DES_NET_SYNC=<n>: send n packets a second (every second frame at most, as
 * a reload of 0 would never send); 15 at 30 fps is every other frame, and the
 * tail on the other copies shrinks with the gap. It changes what this copy
 * sends, so it smooths this player on the others' screens. Unset = the
 * game's 6 frames, and never less often than those (above 30 fps they are
 * more than 5 a second). DES_NETLOG=1 logs each reload and, each frame, each
 * remote's chase (move, gait, timers, velocity, target). Live.
 *
 * SetMoveVector (0x0030C378) picks the remote's walk, run or dash from the
 * frame's move times 30 (the fmuls at 0x0030C46C): a speed only at 30 fps,
 * so above it a running remote walked. While packets come under 150 ms apart
 * the gait comes from the sender's speed (net_remote_gait); otherwise, with
 * DES_FPS set, the product is scaled by 1/(30 dt).
 *
 * With packets that close, the chase also jumps at each one: the gap the game
 * divides by (+0x21C, 0x0030C330) is counted in the receiver's frames and is
 * off by up to one, 20-30% of 45 ms, so the remote surges and slows 20 times
 * a second. That gap is replaced with the average of the recent ones (an
 * exponential average, a fifth of each new gap). And a chase that reaches the
 * last position before the next packet (the game then moves nothing until
 * it) handed SetMoveVector a zero move, which sets the remote's stick to
 * zero: a frame of letting go mid-walk. While the sender moves, such a frame
 * gets a 0.1 mm move along the last direction instead, so the stick holds.
 * In a replay of a 110 fps log, the jumps at packets halved (p90 1.6 to 0.6
 * m/s) and the zero frames went from 29 to none, with the same lag.
 *
 * At 30 fps with the game's packet rate none of this applies. DES_NETFIX=0
 * turns it all off. Main thread. */
extern "C" void des_net_sync(ppu_context* ctx, uint32_t addr)
{
    const float dt = vm_readf(kFrameDt);
    static int log = 0;
    PS3_KNOB(log, env_on("DES_NETLOG"));
    if (addr == 0x00313B24u) {   /* li r0, 5: the send counter's reload */
        static int hz = 0;
        PS3_KNOB(hz, ps3_env_int("DES_NET_SYNC", 0));
        if (hz > 0 && dt > 0.0f) {
            /* never less often than the game's 6 frames: above 30 fps those
             * are already more than 5 a second */
            int frames = (int)(1.0f / ((float)hz * dt) + 0.5f);
            if (frames < 2) frames = 2;
            if (frames > 6) frames = 6;
            ctx->gpr[0] = (uint64_t)(frames - 1);
        }
        if (log) fprintf(stderr, "[netlog] %.3f send reload %u dt=%.4f\n", net_log_time(), (unsigned)ctx->gpr[0], dt);
        return;
    }
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_NETFIX", 1) != 0);
    const bool fix = on && dt > 0.0f && dt <= kMaxFrameDt;
    if (addr == 0x0030C330u) {   /* fmuls f0, f12, f0: OnPacket's 1.2 * gap; r31 = the manipulator */
        if (!fix) return;
        NetRemote* r = net_remote((uint32_t)ctx->gpr[31]);
        const float gap = (float)ctx->fpr[12];
        if (!(gap > 0.0f && gap < 0.15f)) {
            r->gap_avg = 0.0f;
            return;
        }
        r->gap_avg = r->gap_avg > 0.0f ? r->gap_avg + 0.2f * (gap - r->gap_avg) : gap;
        ctx->fpr[0] = (float)(ctx->fpr[0] * (r->gap_avg / gap));
        return;
    }
    if (addr == 0x0030C378u) {   /* SetMoveVector's entry: v2 = this frame's move, r3 = the manipulator */
        if (!fix) return;
        NetRemote* r = net_remote((uint32_t)ctx->gpr[3]);
        net_remote_frame(r, dt);
        float mv[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t b = net_swap(ctx->vr[2].w[i]);
            memcpy(&mv[i], &b, 4);
        }
        const float len = std::sqrt(mv[0] * mv[0] + mv[2] * mv[2]);
        if (len > 1e-5f) {
            r->dir_x = mv[0] / len;
            r->dir_z = mv[2] / len;
        } else if (len < 1e-6f && net_remote_gait(r) >= 0.0f && (r->dir_x != 0.0f || r->dir_z != 0.0f)) {
            const float hold[2] = {r->dir_x * 1e-4f, r->dir_z * 1e-4f};
            for (int i = 0; i < 2; i++) {
                uint32_t b;
                memcpy(&b, &hold[i], 4);
                ctx->vr[2].w[2 * i] = net_swap(b);
            }
            if (log) fprintf(stderr, "[netlog] %.3f m=%08X stick held\n", net_log_time(), r->m);
        }
        return;
    }
    /* 0x0030C46C, fmuls f12, f0, f12: |move| * 30; r3 = the NetworkManipulator */
    if (log) {
        const uint32_t m = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[netlog] %.3f m=%08X mv=%.5f dt=%.4f cat=%u tmr=%.3f since=%.4f snap=%u "
                        "vel=%.3f,%.3f,%.3f tgt=%.3f,%.3f,%.3f\n",
                net_log_time(), m, ctx->fpr[0], dt, vm_read8(m + 0x224), vm_readf(m + 0x220), vm_readf(m + 0x21C),
                vm_read8(m + 0x1CC), vm_readf(m + 0x1F0), vm_readf(m + 0x1F4), vm_readf(m + 0x1F8),
                vm_readf(m + 0x1D0), vm_readf(m + 0x1D4), vm_readf(m + 0x1D8));
    }
    if (!fix) return;
    const float gait = net_remote_gait(net_remote((uint32_t)ctx->gpr[3]));
    if (gait >= 0.0f) {
        ctx->fpr[12] = gait;
        return;
    }
    if (s_fps > 0) ctx->fpr[12] = (float)(ctx->fpr[12] / (30.0 * dt));
}

/* DES_NET_TENDENCY=own: the server stops changing this player's world
 * tendency (docs/69, "World tendency"). The game's QWC object (*(*0x01B4EF9C
 * + 0x24)) keeps the player's own value per area at +0x48 + 8 * (area - 1),
 * two floats, white/black then left/right. Online, two functions read the
 * server's per-area values from the getQWCData reply through 0x00567E98
 * (out = r4, area 1-7 = r5): 0x009204A0, once per load, sets the area and
 * own values to (1 - 0.5) * own + 0.5 * server, and 0x00920FC8 (a new
 * character, and after the login) sets them to the server's. Offline both
 * use the own values alone. Called after the mflr at 0x00567E9C, before r4
 * is tested: it writes the own values to out and clears r4, so the reader
 * skips the reply, both functions see server = own, and the result is the
 * offline one. The player's own values still go to the server (addQWCData).
 * Blends saved before stay. Live: from the next load. Main thread. */
extern "C" void des_qwc_local(ppu_context* ctx, uint32_t)
{
    static int own = 0;
    PS3_KNOB(own, [] {
        const char* e = std::getenv("DES_NET_TENDENCY");
        return e && std::strcmp(e, "own") == 0 ? 1 : 0;
    }());
    if (!own) return;
    const uint32_t out = (uint32_t)ctx->gpr[4], area = (uint32_t)ctx->gpr[5];
    if (!guest_ptr_ok(out) || area < 1 || area > 7) return;
    uint32_t q = vm_read32(0x01B4EF9Cu);
    if (!guest_ptr_ok(q) || !guest_ptr_ok(q = vm_read32(q + 0x24))) return;
    const uint32_t own_at = q + 0x48 + 8 * (area - 1);
    vm_write32(out, vm_read32(own_at));
    vm_write32(out + 4, vm_read32(own_at + 4));
    ctx->gpr[4] = 0;
    if (area == 1) {
        float bw, lr;
        const uint32_t b0 = vm_read32(own_at), b1 = vm_read32(own_at + 4);
        std::memcpy(&bw, &b0, 4);
        std::memcpy(&lr, &b1, 4);
        std::fprintf(stderr, "[des] world tendency: yours only (the server's skipped; area 1 %.0f / %.0f)\n", bw, lr);
    }
}

/* DES_NET_RATING_POLL=<seconds>, 30-299: how often the game asks the server
 * whether the player's messages were rated (docs/69, "Message ratings"). The
 * game asks for the player's total rating (getBloodMessageGrade) every 300
 * s: its network scheduler 0x0054E7E8 tests the seconds since the last ask
 * against 0x12C at 0x0054E938. When the total has gone up, a step that runs
 * each frame (0x0049BFF8) heals the player and shows that a message was
 * rated. So a rating showed up to 5 minutes late, as on the PS3; the server
 * cannot push it. Called after the subf at 0x0054E934 (r0 = the seconds
 * since the last ask): from the knob's seconds on, r0 becomes 301. "" (the
 * default) leaves the game's 300 s. Main thread. */
extern "C" void des_net_grade_poll(ppu_context* ctx, uint32_t)
{
    static int every = 0;
    PS3_KNOB(every, ps3_env_int("DES_NET_RATING_POLL", 0));
    if (every < 30 || every >= 300) return;
    if (ctx->gpr[0] >= (uint64_t)every) ctx->gpr[0] = 0x12D;
}

/* DES_NET_MESSAGES / DES_NET_BLOODSTAINS / DES_NET_GHOSTS = 0: hide what
 * other players put in your world through the server (docs/69, "Showing
 * messages, bloodstains and ghosts"). Each is on (1) by default, the game as
 * it is. The requests and uploads go on as before; only the lists are taken
 * as empty:
 * - messages: the getBloodMessage parser 0x00562A98 reads the entry count
 *   into r21 (0x00562BC4). 0 takes the game's own empty-reply path: the
 *   messages shown are removed, the player's own too. An empty list is
 *   cached under the key -1, so the real list comes back when switched on.
 * - bloodstains: the getReplayList parser 0x005607D0, the count in r18
 *   (0x00560904), the same way. The soul bloodstain is the game's own and
 *   stays.
 * - ghosts: the ghost tick 0x003A5808 sets r29 = 1 to start the next ghost
 *   (0x003A5CA8); 0 starts none, and those walking finish their path.
 * A switch applies at once: the network scheduler 0x0054E7E8 loads each
 * list's last time (bloodstains +0xC8 at 0x0054E8C0, messages +0xD0 at
 * 0x0054E8F8), and a 0 there makes it parse the list again, from its cache.
 * Main thread.
 *
 * The same sites, and the ghost list's 0x00558884, hold each count to the
 * room its array has: the parsers write that many records from the server's
 * reply with no bound (docs/69, "Replays"). Each list's array is allocated
 * with its element count just before it (array - 4; array - 8 is the
 * allocation): messages *(list + 0x144), 0x3C bytes each; bloodstains
 * *(list + 0x140), 0x40 each; the ghost reply's two lists, counts at
 * +0x18C and +0x190, *(list + 0x150) and *(list + 0x14C), 12 bytes each.
 * Messages and bloodstains also copy the last list's ids into a stack array
 * of list + 0x4 and list + 0x0 entries, so their counts stay within those
 * too. A count held back is logged, the first few times. */
static void list_fit(ppu_context* ctx, int reg, const char* what, uint32_t list, uint32_t array_at, int room_at)
{
    const uint32_t n = (uint32_t)ctx->gpr[reg];
    if (!n) return;
    uint32_t room = 0;
    if (guest_ptr_ok(list)) {
        const uint32_t a = vm_read32(list + array_at);
        if (guest_ptr_ok(a) && guest_ptr_ok(a - 8)) room = vm_read32(a - 4);
        if (room_at >= 0 && vm_read32(list + (uint32_t)room_at) < room) room = vm_read32(list + (uint32_t)room_at);
    }
    if (n <= room) return;
    static int said = 0;
    if (said < 8) {
        said++;
        fprintf(stderr, "[des] the server's %s list has %u entries; %u fit, the rest are left out\n", what, n, room);
    }
    ctx->gpr[reg] = room;
}

extern "C" void des_net_show(ppu_context* ctx, uint32_t addr)
{
    static int msgs = 1, stains = 1, ghosts = 1;
    PS3_KNOB(msgs, ps3_env_int("DES_NET_MESSAGES", 1) != 0);
    PS3_KNOB(stains, ps3_env_int("DES_NET_BLOODSTAINS", 1) != 0);
    PS3_KNOB(ghosts, ps3_env_int("DES_NET_GHOSTS", 1) != 0);
    static int msgs_seen = 1, stains_seen = 1;
    switch (addr) {
    case 0x00562BC4u:   /* mr r21, r3: the message count */
        list_fit(ctx, 21, "message", (uint32_t)ctx->gpr[23], 0x144, 0x4);
        if (!msgs && ctx->gpr[21]) {
            fprintf(stderr, "[des] messages hidden (%u from the server)\n", (unsigned)ctx->gpr[21]);
            ctx->gpr[21] = 0;
        }
        break;
    case 0x00558884u: {   /* lwz r0, 0x18c(r31): the ghost list's first count; the loops read both from r31 */
        const uint32_t list = (uint32_t)ctx->gpr[31];
        if (!guest_ptr_ok(list)) break;
        for (const uint32_t off : { 0x18Cu, 0x190u }) {
            ctx->gpr[0] = vm_read32(list + off);
            list_fit(ctx, 0, "ghost", list, off == 0x18Cu ? 0x150 : 0x14C, -1);
            vm_write32(list + off, (uint32_t)ctx->gpr[0]);
        }
        ctx->gpr[0] = vm_read32(list + 0x18C);
        break;
    }
    case 0x00560904u:   /* mr r18, r3: the bloodstain count */
        list_fit(ctx, 18, "bloodstain", (uint32_t)ctx->gpr[24], 0x140, 0x0);
        if (!stains && ctx->gpr[18]) {
            fprintf(stderr, "[des] bloodstains hidden (%u from the server)\n", (unsigned)ctx->gpr[18]);
            ctx->gpr[18] = 0;
        }
        break;
    case 0x003A5CA8u:   /* li r29, 1: start a ghost */
        if (!ghosts) ctx->gpr[29] = 0;
        break;
    case 0x0054E8C0u:   /* ld r0, 0xc8(r31): the bloodstain list's last time */
        if (stains != stains_seen) {
            stains_seen = stains;
            ctx->gpr[0] = 0;
        }
        break;
    case 0x0054E8F8u:   /* ld r0, 0xd0(r31): the message list's last time */
        if (msgs != msgs_seen) {
            msgs_seen = msgs;
            ctx->gpr[0] = 0;
        }
        break;
    }
}

/* Other players' ghost and bloodstain replays come from the server, and the
 * game trusts their frame count. The reader 0x003957B8 (r3 the replay, r4
 * the unpacked data) takes the count at the data's start, allocates
 * count * 32 + 8 bytes and copies count * 32 from offset 12, then reads
 * 0x50 + 0x22 bytes after the frames: 12 + count * 32 + 0x72 in all. Nothing
 * checks that against the data's length (docs/69, "Replays"). These hooks
 * drop a replay whose count does not fit, through the game's own path for
 * one that did not unpack:
 * - a bloodstain's: 0x00391818, the cmpwi of r3 after the unpack 0x005685E8,
 *   which put the length at 0x74(r1). r3 = 0 and cr7 EQ, as if it had failed;
 * - a wandering ghost's: 0x003A596C and 0x003A5EA0, the cmplwi of length - 1
 *   against 0xFFF after each of its two unpacks (r3 the data, r5 the
 *   length). cr7 GT, the game's own "length out of range";
 * - any other: 0x003957B8, the reader's cmpwi of r4. Both unpacks write
 *   0x1000-byte buffers, so a count past that is dropped there: r4 = 0 and
 *   cr7 EQ, the reader's own "no data".
 * DES_NET_REPLAY_CHECK=0 turns the checks off. Main thread. */
constexpr uint32_t kReplayAround = 12 + 0x50 + 0x22;   /* the bytes before and after the frames */
constexpr uint32_t kReplayMax = 0x1000;                 /* the unpack buffers */

static bool replay_fits(uint32_t data, uint32_t len)
{
    if (!guest_ptr_ok(data) || len > kReplayMax) return false;
    const int32_t n = (int32_t)vm_read32(data);
    return n > 0 && (uint64_t)n * 32 + kReplayAround <= len;
}

static void replay_dropped(const char* what, uint32_t data, uint32_t len)
{
    static int said = 0;
    if (said >= 8) return;
    said++;
    fprintf(stderr, "[des] a %s replay from the server was dropped: %d frames do not fit its %u bytes\n", what,
            guest_ptr_ok(data) ? (int)vm_read32(data) : 0, len);
}

extern "C" void des_net_replay(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_NET_REPLAY_CHECK", 1) != 0);
    if (!on) return;
    switch (addr) {
    case 0x00391818u: {   /* cmpwi cr7, r3, 0: the bloodstain's unpacked data */
        const uint32_t data = (uint32_t)ctx->gpr[3];
        const uint32_t len = vm_read32((uint32_t)ctx->gpr[1] + 0x74);
        if (!data || replay_fits(data, len)) break;
        replay_dropped("bloodstain", data, len);
        ctx->gpr[3] = 0;
        ppu_cr_set(ctx, 7, 2u);   /* EQ: the beq takes the no-data exit */
        break;
    }
    case 0x003A596Cu:
    case 0x003A5EA0u: {   /* cmplwi cr7, r0, 0xFFF: the ghost's length - 1 */
        const uint32_t data = (uint32_t)ctx->gpr[3], len = (uint32_t)ctx->gpr[5];
        if (len - 1u > 0xFFFu || replay_fits(data, len)) break;   /* (out of range: the game refuses it itself) */
        replay_dropped("ghost", data, len);
        ppu_cr_set(ctx, 7, 4u);   /* GT: the bgt takes the out-of-range path */
        break;
    }
    case 0x003957B8u: {   /* cmpwi cr7, r4, 0: the reader's entry */
        const uint32_t data = (uint32_t)ctx->gpr[4];
        if (!data || !guest_ptr_ok(data)) break;
        const int32_t n = (int32_t)vm_read32(data);
        if (n >= 0 && (uint64_t)n * 32 + kReplayAround <= kReplayMax) break;
        replay_dropped("", data, kReplayMax);
        ctx->gpr[4] = 0;
        ppu_cr_set(ctx, 7, 2u);   /* EQ: the beq takes the reader's no-data exit */
        break;
    }
    }
}

/* A step a camera adds once a frame, cur += input * step, tuned for 30 fps:
 * the bow's aim view zooms by 0.1 of its range a frame (ChrAimCam 0x00110608,
 * +0x138 += input * +0x13C, then clamped to [0, 1]), so at 120 fps it zoomed
 * four times as fast. Called after the lfs of the step (--insn-hooks); puts
 * step * dt * 30 in the register. Main thread. DES_CAMFIX=0 turns it off with
 * the camera eases; nothing at 30 fps or without DES_FPS. */
extern "C" void des_cam_step(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_CAMFIX", 1) != 0);
    if (!on || s_fps <= 0) return;
    const uint32_t insn = vm_read32(addr);
    if ((insn >> 26) != 48) return;   /* lfs only */
    const int fd = (insn >> 21) & 31;
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    ctx->fpr[fd] = (float)(ctx->fpr[fd] * steps);
}

/* A factor kept once a frame, cur = base + (cur - base) * r (or cur *= r),
 * tuned for 30 fps: at 240 fps the remainder shrinks eight times as fast. Called
 * after the lfs of r (--insn-hooks); puts r' = r^(dt * 30) in the register, the
 * factor that keeps the same part per second as r does at 30 fps. Nothing at
 * 30 fps or without DES_FPS. */
static void keep_rate(ppu_context* ctx, uint32_t addr)
{
    const uint32_t insn = vm_read32(addr);
    if ((insn >> 26) != 48) return;   /* lfs only */
    const int fd = (insn >> 21) & 31;
    const double r = ctx->fpr[fd];
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!(r > 0.0 && r < 1.0) || !(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    ctx->fpr[fd] = (float)std::pow(r, (double)steps);
}

/* The follow camera's pitch return after a scripted angle (0x00119670):
 * pitch = +0x140 + (pitch - +0x140) * r, r = +0x18C (0.3) loaded at 0x0011C15C.
 * Main thread. DES_CAMFIX=0 turns it off with the camera eases. */
extern "C" void des_cam_keep(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_CAMFIX", 1) != 0);
    if (on && s_fps > 0) keep_rate(ctx, addr);
}

/* Sound events: a position, a radius (0.5 to 13 m) and a life (0.5 or 2 s),
 * in three lists that the game's frame step updates through 0x006A3970. Each
 * frame the radius is multiplied by 0.99 (loaded at 0x006A3990; 0x006A2E8C in
 * the single-entry twin 0x006A2E78) while the life counts down by dt. So at
 * 240 fps a radius falls to 8% of its size in a second instead of 74%, and
 * (inferred) enemies hear the player less. Main thread. DES_NOISEFIX=0 turns
 * it off live. */
extern "C" void des_noise_rate(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_NOISEFIX", 1) != 0);
    if (on && s_fps > 0) keep_rate(ctx, addr);
}

/* Character fades. A character's effect list (+0x3BC, docs/42) multiplies the
 * model's alpha (chr+0x368) by each node's fade (+0x10, 0 to 1) every frame.
 * Two node types move their fade by a fixed step a call, without dt: type 16
 * (0x0025EB20, a fade-in, or a fade by distance from the player) by 0.05,
 * loaded into f0 at 0x0025EB88 (in) and 0x0025EC14 (out); type 14
 * (0x002654E8, a model fade with sounds) by 1/30, loaded into f13 at
 * 0x00265C88 (in) and 0x0026558C (out). So at 240 fps a fade of 0.67 s or
 * 1 s takes 83 or 125 ms. The lifter calls this after each of those lfs
 * (--insn-hooks); it scales the step by the frame's 30 Hz steps (kFrameDt).
 * The clamps to 0 and 1 come after the step, so a scaled step never
 * overshoots. Nothing at 30 fps or without DES_FPS. DES_FADEFIX=0 turns it
 * off live; DES_FADELOG=1 prints every step (node r31). */
extern "C" void des_fade_rate(ppu_context* ctx, uint32_t addr)
{
    static int on = 1, log = 0;
    PS3_KNOB(on, ps3_env_int("DES_FADEFIX", 1) != 0);
    PS3_KNOB(log, env_on("DES_FADELOG"));
    const uint32_t insn = vm_read32(addr);
    if ((insn >> 26) != 48) return;   /* lfs only */
    const int fd = (insn >> 21) & 31;
    const float dt = vm_readf(kFrameDt), steps = dt * 30.0f;
    const bool scale = on && s_fps > 0 && steps > 0.0f && steps < 30.0f && steps != 1.0f;
    if (log) {
        using clock = std::chrono::steady_clock;
        static const clock::time_point t0 = clock::now();
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        const uint32_t node = (uint32_t)ctx->gpr[31];
        fprintf(stderr, "[fadelog] %.1f ms %08X node %08X fade %.4f step %.5f dt %.5f%s\n", ms, addr, node,
                vm_readf(node + 0x10), ctx->fpr[fd], dt, scale ? " scaled" : "");
    }
    if (scale) ctx->fpr[fd] = (float)(ctx->fpr[fd] * steps);
}

/* A character proxy's slide velocity (wrapper + 0x100), in its velocity
 * update 0x001FE778 (r26 = the step, dt at +0x20). On a frame whose ground
 * probe finds a slope too steep to stand on (+0x33), the game adds gravity
 * along the slope, gain 1.0 (0x019D9C3C) a frame; on any other frame it
 * multiplies the slide by 0.65 (0x019D9C40), and zeroes it under 0.1 m/s.
 * Neither uses dt, so at 240 fps a narrow steep lip that a 30 fps step clears
 * in one frame gets several frames of slide, enough to push the character
 * back down: a snag that never ends. The lifter calls this after the two lfs
 * (--insn-hooks): 0x001FED50 loads the gain, scaled here by dt * 30, and
 * 0x001FEAF4 loads the decay, raised here to dt * 30.
 *
 * With that, the slide matches 30 fps: at a slow walk the game snags on such
 * a lip at 30 fps too (half stick: 20 of 30 pushes at 30 fps, 21 of 30 at 240).
 * DES_SLIDEFIX=2 is kinder than the game: a slide starts only once the ground
 * has been too steep for 1/30 s without a break (12 of 30), and a real steep
 * slope slides at most 1/30 s later. The run is kept per proxy (r29 = the
 * wrapper) in a small table per thread. All characters, any thread.
 * DES_SLIDEFIX=0 turns it off live. */
extern "C" void des_slide_rate(ppu_context* ctx, uint32_t addr)
{
    static int mode = 1;
    PS3_KNOB(mode, ps3_env_int("DES_SLIDEFIX", 1));
    if (!mode || s_fps <= 0) return;
    const float dt = vm_readf((uint32_t)ctx->gpr[26] + 0x20);
    const float steps = dt * 30.0f;
    if (!(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    struct Run { uint32_t wrapper; float steep_s; };
    static PPU_THREAD_LOCAL Run runs[64];
    const uint32_t wrapper = (uint32_t)ctx->gpr[29];
    Run& r = runs[((wrapper >> 4) * 2654435761u) >> 26];
    const double v = ctx->fpr[0];   /* both sites are lfs f0 */
    if (addr == 0x001FED50u) {      /* a steep step */
        if (r.wrapper != wrapper) r = {wrapper, 0.0f};
        r.steep_s += dt;
        const bool wait = mode == 2 && r.steep_s < 1.0f / 30.0f - 1e-4f;
        ctx->fpr[0] = wait ? 0.0 : (float)(v * steps);
    } else if (addr == 0x001FEAF4u) {   /* any other step */
        if (r.wrapper == wrapper) r.steep_s = 0.0f;
        if (v > 0.0 && v < 1.0) ctx->fpr[0] = (float)std::pow(v, (double)steps);
    }
}

/* DES_ROLLDIR=1 (the default; 0 is the original): a roll while locked on goes
 * where the stick points. Locked on, the game rolls only forward, back, left
 * or right of the target (docs/47, section 3): the player's script asks its
 * function 110 (0x002EB3E8, r3 = the character's controller, r4 = k) once a
 * roll for the move set's roll animation k (0 forward, 1 back, 2 left, 3
 * right; 4 is the backstep), and the animation moves the character along its
 * facing. The lifter calls this just before that call (0x002FF8F0; 0x00301C24
 * is the call's other path). For the player's roll it turns the facing (the
 * yaw at *(controller + 0x10) + 4, the bearing + pi) to the target's bearing
 * plus the stick's angle from k's direction, at most 90 degrees, so the roll
 * runs along the stick. On its own that turn is undone: the locked facing
 * update turns the character back to the target before the roll moves. So for
 * the first 0.3 s of game time this adds the same angle to the yaw that update
 * aims at (0x0030F734, r26 = the controller). The roll keeps its facing to its
 * end, and the lock then turns the character back as usual. Live.
 * DES_ROLLLOG=1 prints each of the player's calls. */
static uint32_t player_controller()
{
    uint32_t p = vm_read32(kChrMan);
    if (!guest_ptr_ok(p) || !guest_ptr_ok(p = vm_read32(p + 0x1BC))) return 0;
    p = vm_read32(p + 0x20);
    return guest_ptr_ok(p) ? p : 0;
}

extern "C" void des_roll_dir(ppu_context* ctx, uint32_t addr)
{
    constexpr float kPi = 3.14159265f, kHold = 0.3f;
    static const float kDir[4] = {0.0f, kPi, -kPi / 2, kPi / 2};   /* roll k's direction from the facing; + = right */
    static struct { uint32_t ctrl; float turn, yaw, left; } s;
    static int on = 1, log = 0;
    PS3_KNOB(on, ps3_env_int("DES_ROLLDIR", 1) != 0);
    PS3_KNOB(log, env_on("DES_ROLLLOG"));
    if (addr == 0x0030F734u) {   /* fadds f1, f1, f0: f1 = the yaw the locked facing update aims at */
        if (!(s.left > 0.0f) || (uint32_t)ctx->gpr[26] != s.ctrl) return;
        const uint32_t xf = vm_read32(s.ctrl + 0x10);
        /* Off, or the facing jumped away from the turned roll's (the lock was dropped in between): done. */
        if (!on || !guest_ptr_ok(xf) || std::fabs(std::remainder(vm_readf(xf + 4) - s.yaw, 2 * kPi)) > 0.25f) {
            s.left = 0.0f;
            return;
        }
        s.yaw = vm_readf(xf + 4);
        ctx->fpr[1] = (float)(ctx->fpr[1] + s.turn);
        const float dt = vm_readf(kFrameDt);
        s.left -= dt > 0.0f && dt < 0.25f ? dt : 1.0f / 30.0f;
        return;
    }
    /* 0x002FF8F0 (extsw r4, r4) or 0x00301C24 (li r4, 0) */
    const uint32_t ctrl = (uint32_t)ctx->gpr[3];
    if (ctrl != player_controller()) return;
    const int k = (int)(int32_t)ctx->gpr[4];
    const unsigned st = g_pad0_lstick;
    const float x = ((int)(st >> 8) - 128) / 127.0f, up = (128 - (int)(st & 0xFFu)) / 127.0f;
    const float stick = std::atan2(x, up);   /* from up; + = right */
    float turn = 0.0f;
    uint32_t xf = 0, lock = 0;
    const char* why = nullptr;
    if (!on) why = "off";
    else if (vm_read8(ctrl + 0x139)) why = "not locked on";
    else if (k < 0 || k > 3) why = "not a roll";
    else if (x * x + up * up < 0.2f * 0.2f) why = "stick centred";
    else if (std::fabs(turn = std::remainder(stick - kDir[k], 2 * kPi)) > kPi / 2) why = "stick too far from the roll";
    else if (std::fabs(turn) < 0.01f) why = "stick along the roll";
    else if (!guest_ptr_ok(xf = vm_read32(ctrl + 0x10)) || !guest_ptr_ok(lock = vm_read32(ctrl + 4)) ||
             !vm_read8(lock + 0x100))
        why = "no target";
    else {
        /* The target's bearing, as the locked facing update takes it: its position is at +0x110. */
        const float dx = vm_readf(lock + 0x110) - vm_readf(xf + 0x10);
        const float dz = vm_readf(lock + 0x118) - vm_readf(xf + 0x18);
        const float yaw = vm_readf(xf + 4);
        s = {ctrl, turn, yaw + std::remainder(std::atan2(dx, dz) + kPi + turn - yaw, 2 * kPi), kHold};
        vm_writef(xf + 4, s.yaw);
    }
    if (log)
        fprintf(stderr, "[rolllog] %08X k=%d stick %+.2f %+.2f (%+.0f deg), turn %+.0f deg: %s\n", addr, k, x, up,
                stick * 180.0f / kPi, turn * 180.0f / kPi, why ? why : "turned");
}

/* DES_ROLLTIE=1 (the default; 0 is the original): a roll toward a diagonal
 * when not locked on. Each frame 0x0030E750 keeps one axis of the stick, the
 * larger (up/down on a tie, 0x0030E8B0). Not locked on, it then turns that
 * axis and its 6-frame history into the frame of the stick's own direction
 * (0x0030EC44 to 0x0030F2C0). The kept axis is at most 45 degrees off the
 * stick, so the turned forward part (f30) beats the side part (f31), a tie
 * going forward (0x0030EA34), and the roll goes toward the stick. On an exact
 * diagonal the two parts are equal (0.5 and 0.5). The game reads an axis as
 * (byte - 127.5) / 127.5, so the keys' corners (0 or 255 on each axis) are
 * exact diagonals, and the turn's rounding in the last bit picked the side
 * roll, 90 degrees off, in about half of those rolls. The lifter calls this
 * after the turn's last load (0x0030F2BC). A near tie (the side part larger
 * by at most 1%) goes forward, as the game means: f31, and f27 of the current
 * frame's pair (f26, f27), set to 0. Live. DES_ROLLLOG=1 prints the first
 * frame of each run of near ties. */
static bool roll_near_tie(double fwd, double side)
{
    const double a = std::fabs(fwd), b = std::fabs(side);
    return b > a && b - a <= 0.01 * b;
}
extern "C" void des_roll_tie(ppu_context* ctx, uint32_t)
{
    static int on = 1, log = 0;
    static bool was = false;
    PS3_KNOB(on, ps3_env_int("DES_ROLLTIE", 1) != 0);
    PS3_KNOB(log, env_on("DES_ROLLLOG"));
    const bool hist = roll_near_tie(ctx->fpr[30], ctx->fpr[31]), cur = roll_near_tie(ctx->fpr[26], ctx->fpr[27]);
    if (log && (hist || cur) && !was)
        fprintf(stderr, "[rolllog] 0030F2BC near tie: forward %+.7f side %+.7f, now %+.7f %+.7f: %s\n", ctx->fpr[30],
                ctx->fpr[31], ctx->fpr[26], ctx->fpr[27], on ? "forward" : "left to the game");
    was = hist || cur;
    if (!on) return;
    if (hist) ctx->fpr[31] = 0.0;
    if (cur) ctx->fpr[27] = 0.0;
}

/* The character creation screen's previews turn with the right stick: the
 * whole body (0x008DB170, from Create Profile on) and the face in Edit
 * Appearance (0x008DA0E0). Each step gets r3 = the preview and f1, f2 = the
 * stick's x and y, which PadMan's getters 0x001B3F58 and 0x001B3F90 scale by
 * pi/120, and adds 1.5 times the stick to the yaw (+0x154, wrapped to pi)
 * and the pitch (+0x158, clamped) once a frame: 2.25 degrees a frame at full
 * tilt, 67.5 degrees a second at 30 fps and eight times that at 240.
 * The lifter calls this after their first instruction (--insn-hooks); it
 * scales the stick by the frame's 30 Hz steps (kFrameDt). DES_UIFIX=0 turns
 * it off. */
extern "C" void des_viewer_rate(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    const float steps = vm_readf(kFrameDt) * 30.0f;
    if (!ui_fix_on() || !(steps > 0.0f && steps < 30.0f) || steps == 1.0f) return;
    ctx->fpr[1] = (float)(ctx->fpr[1] * steps);
    ctx->fpr[2] = (float)(ctx->fpr[2] * steps);
}

/* Particle effects (FFX). Each effect node runs a small state machine in the
 * scene update 0x013B74B8 (main thread, once a frame from the effect manager's
 * step 0x00515070; r31 = the node, f31 = dt). Entering a state runs its entry
 * messages, and an emitter's messages emit a fixed number of particles. An
 * emitter repeats by re-entering its own state once the state's time (+0x94,
 * set to 0 on entry, stepped by dt after the test) reaches an interval; with
 * an interval of a frame or less that is every update: 30 times a second at 30
 * fps, 240 at 240. So the boss fog walls churn 2, 4 or 8 times as fast. The
 * lifter calls this after each instruction that picks the next state (r0, 0
 * for none; --insn-hooks): 0x013B75A8 for the state's own transitions, the
 * other four on the paths of templates 0x7E4, 0x7E7, 0x835 and 0x849. A state
 * that would re-enter itself before it has lasted 1/30 s stays as it is this
 * update. When it does re-enter, 0x013B75D4 (the store that sets its time to
 * 0) starts the new time with the part of a frame past 1/30 s, so the loop
 * runs 30 times a second at any frame rate. Other transitions, and loops
 * slower than 1/30 s, are left alone. Nothing at 30 fps or without DES_FPS.
 * DES_FXFIX=0 turns it off live. */
extern "C" void des_fx_state(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_FXFIX", 1) != 0);
    static PPU_THREAD_LOCAL uint32_t carry_node;
    static PPU_THREAD_LOCAL float carry;
    const uint32_t node = (uint32_t)ctx->gpr[31];
    if (addr == 0x013B75D4u) {   /* the new state's time, just set to 0 */
        if (carry_node == node) vm_writef(node + 0x94, carry);
        carry_node = 0;
        return;
    }
    carry_node = 0;
    const uint32_t next = (uint32_t)ctx->gpr[0];
    if (!on || s_fps <= 0 || !next || next != vm_read32(node + 0x80)) return;
    const float tick = 1.0f / 30.0f, dt = (float)ctx->fpr[31];
    if (!(dt > 0.0f && dt < tick - 1e-4f)) return;
    const float t = vm_readf(node + 0x94);
    if (t < tick - 1e-5f) {
        ctx->gpr[0] = 0;   /* too soon: no re-entry this update */
        return;
    }
    if (t > tick && t - tick < dt) {
        carry_node = node;
        carry = t - tick;
    }
}

/* Wandering ghosts (other players' phantoms walking through your world)
 * play back through ReplayManipulator (vtable 0x0185C128): a recorded path of
 * up to 27 samples, a third of a second apart. Its Update (0x00314C80) moves
 * to the next sample every 10 frames (the li r9, 10 at 0x00315030), and the
 * sample's move and turn are split into 10 per-frame steps (the 10.0 at
 * 0x003147EC). Frames, not time: at 144 fps a ghost walked its path 4.8 times
 * as fast. SetMoveVector (0x00314ABC) picks walk or run from the step times
 * 30, a speed only at 30 fps. With DES_FPS set, the hooks put N = 1/(3 dt)
 * frames a sample, N as the divisor and 3 N as the factor: 10, 10 and 30 at
 * 30 fps, where nothing changes. Recording already runs on dt (a sample each
 * 1/3 s, 0x002E4C68), so what is uploaded stays as on the PS3. Bloodstain
 * replays most likely play through the same code. DES_GHOSTFIX=0 turns it
 * off. Main thread. */
extern "C" void des_ghost_rate(ppu_context* ctx, uint32_t addr)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("DES_GHOSTFIX", 1) != 0);
    const float dt = vm_readf(kFrameDt);
    if (!on || s_fps <= 0 || !(dt > 0.0f && dt <= kMaxFrameDt)) return;
    int n = (int)(1.0f / (3.0f * dt) + 0.5f);
    if (n < 1) n = 1;
    if (n == 10) return;
    if (addr == 0x00315030u) ctx->gpr[9] = (uint64_t)n;         /* li r9, 10: frames to the next sample */
    else if (addr == 0x003147ECu) ctx->fpr[12] = (float)n;      /* lfs f12: the move and turn divisor */
    else ctx->fpr[12] = 3.0f * (float)n;                        /* 0x00314ABC, lfs f12: the step's speed factor */
}

#if !DES_DEV
/* Release builds: the dev hook lines in insn_hooks.txt (docs/55) call this
 * empty function, and no cheat is ever used. Developer builds (cmake
 * -DDES_DEV=ON) link des_dev.cpp's instead. */
extern "C" void des_dev_hook(ppu_context* ctx, uint32_t addr) { (void)ctx; (void)addr; }
extern "C" int des_dev_cheats_used(void) { return 0; }
#else
extern "C" void des_dev_boot(void);   /* des_dev.cpp */
#endif

namespace {

constexpr uint32_t kCutCamKeys  = 0x005F7254u;   /* lwz r9: s, the gap and the six key indices are set */
constexpr uint32_t kCutCamPos   = 0x005F72ECu;   /* lvx v4: earlier key at r1+0xA0, later at r1+0xB0 */
constexpr uint32_t kCutCamRot   = 0x005F7394u;   /* lvx v12: earlier r1+0xD0, later r1+0xA0 */
constexpr uint32_t kCutCamScale = 0x005F743Cu;   /* lvx v2: earlier r1+0x70, later r1+0xA0 */
constexpr float    kCutMaxMove  = 2.0f;          /* m in one 1/30 s step */
constexpr float    kCutMaxTurn  = 0.5f;          /* rad, any angle, in one step */
constexpr float    kPi          = 3.14159265f;
constexpr float    kTwoPi       = 6.28318531f;

/* The choice for the call of 0x005F71D0 running on this thread, made at
 * kCutCamKeys before its three loads: blend them, or leave them alone; and
 * whether the rotation blends to the later key's other Euler angles. */
PPU_THREAD_LOCAL bool s_cut_blend;
PPU_THREAD_LOCAL bool s_cut_flip;

uint32_t bswap32(uint32_t b)
{
    return (b >> 24) | ((b >> 8) & 0xFF00u) | ((b << 8) & 0xFF0000u) | (b << 24);
}

/* b - a for two angles, the short way round. */
float angle_delta(float a, float b)
{
    const float d = b - a;
    return d - kTwoPi * std::nearbyint(d / kTwoPi);
}

/* Angle c of the other Euler angles for the rotation e. 0x00054D48 builds
 * the camera's rotation from (x, y, z) as Rx(x) Rz(z) Ry(y), row vectors, so
 * z is the middle turn, and Rx(x + pi) Rz(pi - z) Ry(y + pi) is the same
 * matrix (docs/62). Angles 2 pi apart need nothing: angle_delta takes the
 * short way round. */
float euler_other(const float e[3], int c)
{
    return c == 2 ? kPi - e[2] : e[c] + kPi;
}

/* One channel's two keys, read from the track as 0x005F71D0 reads them. i0
 * and i1 are the keys' indices; the game skips the channel when i0 is -1.
 * The byte at h + zero makes both keys read as zero, and the word at
 * h + 0x14 points at the 12-byte x, y, z entries the indices select.
 * Returns 1 with the keys, 0 for a channel skipped or read as zero, and -1
 * when only i1 is -1 (the game would read the entry before the table). */
int cut_keys(uint32_t h, uint32_t zero, int32_t i0, int32_t i1, float k0[3], float k1[3])
{
    if (i0 < 0 || vm_read8(h + zero)) return 0;
    if (i1 < 0) return -1;
    const uint32_t table = vm_read32(h + 0x14);
    for (uint32_t c = 0; c < 3; c++) {
        k0[c] = vm_readf(table + 12u * (uint32_t)i0 + 4 * c);
        k1[c] = vm_readf(table + 12u * (uint32_t)i1 + 4 * c);
    }
    return 1;
}

} // namespace

/* DES_CUTFPS=1: the cutscene camera between its keys. A remo cut's camera
 * track (sibcam) keys position, rotation and scale at whole frames (time =
 * frame / 30). 0x005F71D0 finds the keys either side of the cut time t (f1),
 * puts how far t is between them in f10 (s, 0 to 1) and their gap in f11
 * (seconds), then for each channel copies both keys' vectors to its stack
 * and loads one: the earlier when s < 0.5, else the later. So the camera
 * moves 30 times a second at any frame rate, while the characters, posed at
 * the same t, move every frame: above 30 they creep against a still
 * background and snap back when the camera catches up (docs/40).
 *
 * The lifter calls this after four instructions (--insn-hooks). At
 * kCutCamKeys, once a call and before any load, it decides: with
 * DES_CUTFPS=1 and DES_FPS set, blend when t is strictly between keys one
 * frame apart. Keys further apart keep the nearest key: those are holds or
 * jumps made on purpose. So does a step that moves the camera over 2 m or
 * turns it over 0.5 rad, a shot change inside one track. After each of the
 * three loads it then puts the two keys blended by s in the register.
 * Rotations are Euler angles in radians and blend the short way round; the
 * code after the load wraps them again. A step whose angles turn over
 * 0.5 rad while the later key's other angles for the same rotation
 * (euler_other) are within 0.5 rad of the earlier key is not a turn: the
 * track wrote the same rotation the other way (a flip). The rotation then
 * blends to those other angles; keeping the nearest key there held the
 * camera for a step (docs/62). DES_CUTEULER=0 keeps the nearest key at a
 * flip, as before, live. A register that holds neither key is left alone.
 * DES_CUTLOG=1 counts what it did. Main thread. */
extern "C" void des_cut_cam(ppu_context* ctx, uint32_t addr)
{
    static int log = 0, euler = 1;
    PS3_KNOB(log, env_on("DES_CUTLOG"));
    PS3_KNOB(euler, ps3_env_int("DES_CUTEULER", 1) != 0);
    static unsigned n_calls, n_blend, n_flip, n_apart, n_jump, n_odd, n_neither;
    static double widest;   /* the widest gap between keys, frames */
    if (addr == kCutCamKeys) {
        s_cut_blend = false;
        s_cut_flip = false;
        if (!cutfps_on() || s_fps <= 0) return;
        const double s = ctx->fpr[10], gap = ctx->fpr[11];
        if (!(s > 0.0 && s < 1.0)) return;   /* t on a key: the loads are exact */
        const uint32_t h = (uint32_t)ctx->gpr[7];
        float p0[3], p1[3], r0[3] = {}, r1[3] = {}, k0[3], k1[3];
        const int pos   = cut_keys(h, 0x2C, (int32_t)ctx->gpr[6], (int32_t)ctx->gpr[4], p0, p1);
        const int rot   = cut_keys(h, 0x2F, (int32_t)ctx->gpr[29], (int32_t)ctx->gpr[31], r0, r1);
        const int scale = cut_keys(h, 0x32, (int32_t)ctx->gpr[12], (int32_t)ctx->gpr[28], k0, k1);
        float move = 0.0f, turn = 0.0f, turn_e = 0.0f;   /* turn_e: to the later key's other angles */
        for (int c = 0; c < 3; c++) {
            if (pos > 0) move += (p1[c] - p0[c]) * (p1[c] - p0[c]);
            if (rot > 0) {
                turn   = std::fmax(turn, std::fabs(angle_delta(r0[c], r1[c])));
                turn_e = std::fmax(turn_e, std::fabs(angle_delta(r0[c], euler_other(r1, c))));
            }
        }
        move = std::sqrt(move);
        const bool flip  = euler && turn > kCutMaxTurn && turn_e <= kCutMaxTurn;
        const bool apart = std::fabs(gap * 30.0 - 1.0) > 0.01;
        const bool jump  = !apart && (move > kCutMaxMove || (flip ? turn_e : turn) > kCutMaxTurn);
        const bool odd   = pos < 0 || rot < 0 || scale < 0;
        s_cut_blend = !apart && !jump && !odd;
        s_cut_flip  = s_cut_blend && flip;
        if (log) {
            n_calls++;
            if (s_cut_blend) n_blend++;
            if (s_cut_flip) n_flip++;
            if (apart) { n_apart++; if (gap * 30.0 > widest) widest = gap * 30.0; }
            if (jump) n_jump++;
            if (odd) n_odd++;
            static uint32_t said_h;
            static double said_t0 = -1.0;
            if ((jump || s_cut_flip) && !(h == said_h && ctx->fpr[12] == said_t0)) {   /* f12: the earlier key's time */
                said_h = h;
                said_t0 = ctx->fpr[12];
                float e[3];   /* the later key's other angles, wrapped to [-pi, pi] */
                for (int c = 0; c < 3; c++) e[c] = angle_delta(0.0f, euler_other(r1, c));
                fprintf(stderr, "[cutlog] t=%.3f s: %s (move %.2f m, turn %.2f rad, %.2f to the other angles), %s;"
                        " rotation keys (%.4f %.4f %.4f) to (%.4f %.4f %.4f), other angles (%.4f %.4f %.4f)\n",
                        ctx->fpr[1], jump ? "a jump" : "an Euler flip", move, turn, turn_e,
                        jump ? "nearest key kept" : "blended to the other angles",
                        r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], e[0], e[1], e[2]);
            }
            if (n_calls % 1000 == 0)
                fprintf(stderr, "[cutlog] calls between keys %u: blended %u (Euler flips %u), keys apart %u"
                        " (widest %.1f frames), jumps %u, one index unset %u; loads holding neither key %u\n",
                        n_calls, n_blend, n_flip, n_apart, widest, n_jump, n_odd, n_neither);
        }
        return;
    }
    if (!s_cut_blend) return;
    const double s = ctx->fpr[10];
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    int ch, v;       /* the channel (0 position, 1 rotation, 2 scale) and its register */
    uint32_t a, b;   /* the earlier and the later key's vector */
    switch (addr) {
    case kCutCamPos:   ch = 0; v = 4;  a = sp + 0xA0; b = sp + 0xB0; break;
    case kCutCamRot:   ch = 1; v = 12; a = sp + 0xD0; b = sp + 0xA0; break;
    case kCutCamScale: ch = 2; v = 2;  a = sp + 0x70; b = sp + 0xA0; break;
    default: return;
    }
    a &= ~0xFu;   /* as lvx and stvx address them */
    b &= ~0xFu;
    float x[4], y[4];
    bool is_x = true, is_y = true;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t xb = vm_read32(a + 4 * i), yb = vm_read32(b + 4 * i);
        const uint32_t rb = bswap32(ctx->vr[v].w[i]);   /* the register holds guest bytes */
        is_x = is_x && rb == xb;
        is_y = is_y && rb == yb;
        memcpy(&x[i], &xb, 4);
        memcpy(&y[i], &yb, 4);
    }
    if (!is_x && !is_y) {
        if (log) n_neither++;
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        /* At a flip the rotation blends to the later key's other angles. */
        const float yi = ch == 1 && s_cut_flip && i < 3 ? euler_other(y, (int)i) : y[i];
        const float d = i == 3 ? 0.0f : ch == 1 ? angle_delta(x[i], yi) : yi - x[i];
        const float f = x[i] + d * (float)s;
        uint32_t fb;
        memcpy(&fb, &f, 4);
        ctx->vr[v].w[i] = bswap32(fb);
    }
    static bool said = false;
    if (!said) {
        said = true;
        fprintf(stderr, "[des] DES_CUTFPS: cutscene camera keys blended (first at t=%.3f s)\n", ctx->fpr[1]);
    }
}

/* Mouse look (docs/41): DES_MOUSE_LOOK=1, or Settings, Controls, Mouse look.
 * The mouse turns the follow camera (ChrExFollowCam, the usual third-person
 * camera) and the bow's aim view (ChrAimCam, below) the way the right stick
 * does, by its counts times DES_MOUSE_SENS degrees, with no limit on the turn
 * speed. The counts are gathered on the window's thread
 * (port/ui/src/mouse.cpp).
 *
 * The camera's update (0x00119670) keeps no angles from one frame to the
 * next. Each frame it works yaw (+0x134) and pitch (+0x130) out again from
 * the look direction, eye to target (+0xD0): with the eye it placed last
 * frame (+0xE0, at 0x0011C96C), or on a frame where the stick moved, with the
 * eye as drawn (+0x100, at 0x0011D4CC). From 0x0011C758 on it adds the stick's
 * turn (input * rate * dt), clamps pitch to [+0x1D4, +0x1D0], wraps yaw and
 * stores both. Then it places the eye again from the angles (0x0011AA48:
 * target - direction * distance), and the next frame's angles come from
 * there. A frame where the stick moved also sets the pitch the camera holds
 * (+0x140; on the other frames 0x0011C118 pulls pitch back to it) and puts
 * the drawn eye on the placed one at once (+0x120 = 1) instead of easing it
 * there; after the stick is let go the camera keeps its direction for a while
 * before it swings round behind the player again (+0x148, +0x144). So angles
 * written at the update's entry are thrown away: a turn has to go in after
 * the rebuild and before the stick's turn, on a frame that counts as one
 * where the stick moved.
 *
 * Four hooks (--insn-hooks, port/insn_hooks.txt), all on the main thread:
 * - 0x00119670, the entry (r3 = the camera): once a game frame, an active
 *   camera (+0x298) takes the counts and keeps their turn for this update.
 *   They are dropped while the camera has a set-angle request (+0x12C), a
 *   look-at target (+0x240, lock-on, inferred) or an auto-turn running
 *   (+0x243, the R3 reset), where the update does not take the stick's turn
 *   either, and on the first update after a gap of more than three flips, so
 *   moves made while the camera was not running do not turn it all at once.
 * - 0x0011C910, the stick test (f0 = the stick's pitch input, compared with 0
 *   next): with a turn to make, f0 is made nonzero, so the update takes its
 *   stick-moved path (0x0011CCD8). The stick's input itself is left alone.
 * - 0x0011C758, after the rebuild (r31 = the camera): adds the turn to yaw and
 *   pitch, pitch kept inside its limits. The update adds the stick's turn on
 *   top, and places and draws the camera from the new angles in this frame.
 * - 0x00110608, the aim camera's entry (r3 = the camera, r5 = the character):
 *   the aim view, below.
 *
 * The bow's aim view is another camera (ChrAimCam). The camera manager
 * (0x00115370) runs one camera a game frame, by its mode (+0x74): the follow
 * camera in modes 0 and 3 (3 is the blend out of the aim view), the aim
 * camera (update 0x00110608) in modes 1 (the blend in) and 2. Unlike the
 * follow camera it keeps pitch (+0x144) and yaw (+0x148) from frame to frame:
 * it adds the stick's turn to them (input * rate * (+0x50 / +0x134) * dt, the
 * field of view over the widest, so zoomed in it turns finer), wraps yaw,
 * clamps pitch and stores both. So at its entry, while its input is on
 * (+0x184, as for the stick), the counts are taken and their turn goes onto
 * the kept angles at once, times the same ratio. Not on its first frame
 * (+0x140), whose angles the update sets from the follow camera's, and not
 * without a character (r5 = 0: the update then skips the wrap and the clamp).
 * Leaving the aim view hands its angles to the follow camera (0x00115E08), so
 * a turn made while aiming stays. One of the two entries takes the counts each
 * game frame and they share the gap check, so a switch between the cameras
 * loses no counts and turns nothing twice.
 *
 * Lock-on switching: with a look-at target the update takes no turn, and on a
 * pad a flick of the right stick switches targets. So there the counts to the
 * left and right add up instead, and a move of DES_MOUSE_FLICK degrees (the
 * counts times DES_MOUSE_SENS; default 15, 0 = off) flicks the right stick
 * that way for kFlickReads packets (cellPad_host_stick, counted in packets
 * the game reads, so the same at any frame rate). The next flick waits
 * kFlickRest updates, the stick back at rest in between, and a move ends when
 * it turns back or stops for kFlickIdle updates. Up and down do nothing there.
 *
 * Signs (from the code, and seen in runs): pitch grows as the view looks down
 * (0x00114120 works it out as -atan2(y, horizontal length) of the look
 * direction), and yaw grows as the view turns right (atan2(x, z) of it; the
 * camera is left-handed, 0x0011BA44). The right stick agrees: with the game's
 * default camera options (RotLR = RotUD = 1, set at 0x00918090), stick right
 * raises yaw and stick up lowers pitch (getters 0x001B6A80 and 0x001B6B40; the
 * pad's Y is up-positive, 0x00C46410). So mouse right raises yaw and turns the
 * view right, and mouse forward (dy < 0) lowers pitch and looks up. The aim
 * camera adds the stick the same way (0x00110854), so the same signs turn it.
 * DES_MOUSE_INVERT_X=1 and DES_MOUSE_INVERT_Y=1 turn either around;
 * the game's own camera options don't apply to it. DES_MOUSE_SENS is in
 * degrees per count (0.09: a full turn per 4000), the same at any frame rate.
 * All live. The camera turns once a game frame, so at 30 fps it moves in 30 Hz
 * steps. DES_MOUSELOG=1 prints what happened to the counts every 60 updates
 * (and mouse.cpp when it takes and lets go of the mouse). */
namespace {
constexpr uint32_t kMouseAtEntry     = 0x00119670u;   /* li r0, 4080 */
constexpr uint32_t kMouseAtStickTest = 0x0011C910u;   /* lfs f0, 0xF0(r1) */
constexpr uint32_t kMouseAtTurn      = 0x0011C758u;   /* lwz r9, -0x7F84(r30) */
constexpr uint32_t kMouseAtAimEntry  = 0x00110608u;   /* li r0, 1776 */
constexpr uint32_t kCamPitch     = 0x130u;   /* float, radians */
constexpr uint32_t kCamYaw       = 0x134u;   /* float, radians, kept in [-pi, pi] by the update */
constexpr uint32_t kCamPitchMax  = 0x1D0u;
constexpr uint32_t kCamPitchMin  = 0x1D4u;
constexpr uint32_t kCamSetAngle  = 0x12Cu;   /* bytes */
constexpr uint32_t kCamLookAt    = 0x240u;
constexpr uint32_t kCamAutoTurn  = 0x243u;
constexpr uint32_t kCamActive    = 0x298u;
constexpr uint32_t kAimFov       = 0x50u;    /* float: this frame's field of view (the aim camera) */
constexpr uint32_t kAimPitchMax  = 0xF0u;    /* float, degrees */
constexpr uint32_t kAimPitchMin  = 0xF4u;
constexpr uint32_t kAimFovMax    = 0x134u;   /* float: the widest field of view */
constexpr uint32_t kAimFirst     = 0x140u;   /* byte */
constexpr uint32_t kAimPitch     = 0x144u;   /* float, radians, kept from frame to frame */
constexpr uint32_t kAimYaw       = 0x148u;   /* float, radians, wrapped by the update */
constexpr uint32_t kAimInput     = 0x184u;   /* byte: the update reads the stick */
constexpr float    kMouseSensDefault = 0.09f;   /* degrees per count; settings.cpp's default too */
constexpr unsigned kMouseStaleFlips  = 3;
constexpr float    kFlickDefault     = 15.0f;   /* degrees of mouse movement; settings.cpp's default too */
constexpr unsigned kFlickReads       = 3;       /* packets the right stick is held over */
constexpr unsigned kFlickRest        = 8;       /* camera updates from one flick to the next */
constexpr unsigned kFlickIdle        = 4;       /* updates without a count to the side that end a move */

/* The turn the entry kept for this update of camera s_turn_cam (0: none), in
 * radians, and its counts (|dx| + |dy|) for the log. */
uint32_t s_turn_cam = 0;
float s_turn_yaw = 0.0f, s_turn_pitch = 0.0f;
int s_turn_counts = 0;

/* What happened to the counts, for DES_MOUSELOG: turned the camera, kept at
 * the entry but the update did not reach the turn (it took another path),
 * or dropped at the entry and why. */
enum { kTurned, kLost, kStale, kSetAngle, kLookAt, kAutoTurn, kReasons };
long long s_mouse_counts[kReasons];
long long s_flicks;   /* lock-on switching's flicks, for DES_MOUSELOG */

/* Lock-on switching: this move's counts to the side (left negative), the
 * updates left before the next flick can start, and the updates since the
 * last count to the side. A lock starts settling: counts to the side wait for
 * an update without one, so the turn towards the target, still going on as
 * the lock starts, does not switch to the next target at once. */
int s_flick_x = 0;
unsigned s_flick_rest = 0, s_flick_idle = 0;
bool s_flick_settle = true;

float mouse_sens()
{
    const char* v = getenv("DES_MOUSE_SENS");
    const float s = (v && *v) ? (float)atof(v) : kMouseSensDefault;
    return (s > 0.0f && s <= 10.0f) ? s : kMouseSensDefault;
}

float flick_degrees()
{
    const char* v = getenv("DES_MOUSE_FLICK");
    const float d = (v && *v) ? (float)atof(v) : kFlickDefault;
    return (d >= 0.0f && d <= 360.0f) ? d : kFlickDefault;
}

/* With a look-at target: an update's counts to the side (dx, inverted with
 * DES_MOUSE_INVERT_X) towards a flick of the right stick. */
void mouse_flick(int dx, float sens, float deg)
{
    if (s_flick_settle) {
        if (!dx) s_flick_settle = false;
        return;
    }
    if (s_flick_rest) {
        s_flick_rest--;
        s_flick_x = 0;
        return;
    }
    if (!dx) {
        if (++s_flick_idle >= kFlickIdle) s_flick_x = 0;
        return;
    }
    s_flick_idle = 0;
    if (s_flick_x && (dx > 0) != (s_flick_x > 0)) s_flick_x = 0;   /* turned back: a new move */
    s_flick_x += dx;
    if ((float)std::abs(s_flick_x) * sens < deg) return;
    cellPad_host_stick(1, s_flick_x > 0 ? 255 : 0, 128, kFlickReads);
    s_flicks++;
    s_flick_x = 0;
    s_flick_rest = kFlickRest;
}

/* 0x0011C758: the kept turn onto the angles the update just rebuilt. */
void mouse_turn(uint32_t cam)
{
    s_turn_cam = 0;
    s_mouse_counts[kTurned] += s_turn_counts;
    const float yaw0 = vm_readf(cam + kCamYaw), pitch0 = vm_readf(cam + kCamPitch);
    const float lo = vm_readf(cam + kCamPitchMin), hi = vm_readf(cam + kCamPitchMax);
    const float yaw = (float)std::remainder((double)yaw0 + s_turn_yaw, 6.283185307179586);
    float pitch = pitch0 + s_turn_pitch;
    if (lo < hi) {   /* inside the limits, without pulling back a pitch already past one */
        if (s_turn_pitch > 0.0f && pitch > hi) pitch = pitch0 > hi ? pitch0 : hi;
        if (s_turn_pitch < 0.0f && pitch < lo) pitch = pitch0 < lo ? pitch0 : lo;
    }
    if (std::isfinite(yaw) && std::isfinite(pitch)) {
        vm_writef(cam + kCamYaw, yaw);
        vm_writef(cam + kCamPitch, pitch);
    }
}

/* 0x00110608: the aim camera's turn, onto its kept angles at once (the update
 * wraps yaw and clamps pitch next). Returns kTurned, or why it did not turn. */
int mouse_aim(uint32_t cam, uint32_t chr, float yaw_turn, float pitch_turn)
{
    if (!chr || vm_read8(cam + kAimFirst)) return kSetAngle;
    float f = vm_readf(cam + kAimFov) / vm_readf(cam + kAimFovMax);   /* zoomed in turns finer, as the stick does */
    if (!(f > 0.0f && f <= 1.0f)) f = 1.0f;
    const float yaw = vm_readf(cam + kAimYaw) + yaw_turn * f, pitch = vm_readf(cam + kAimPitch) + pitch_turn * f;
    if (std::isfinite(yaw) && std::isfinite(pitch)) {
        vm_writef(cam + kAimYaw, yaw);
        vm_writef(cam + kAimPitch, pitch);
    }
    return kTurned;
}
} // namespace

extern "C" unsigned des_flip_count(void) { return s_flips.load(std::memory_order_relaxed); }

extern "C" void des_mouse_look(ppu_context* ctx, uint32_t addr)
{
    if (addr == kMouseAtStickTest) {
        if (s_turn_cam && (uint32_t)ctx->gpr[31] == s_turn_cam && ctx->fpr[0] == 0.0) ctx->fpr[0] = 1.0;
        return;
    }
    if (addr == kMouseAtTurn) {
        if (s_turn_cam && (uint32_t)ctx->gpr[31] == s_turn_cam) mouse_turn(s_turn_cam);
        return;
    }
    const bool aim = addr == kMouseAtAimEntry;
    if (!aim && addr != kMouseAtEntry) return;
    static int on = 0, inv_x = 0, inv_y = 0, log = 0;
    static float sens = kMouseSensDefault, flick = kFlickDefault;
    PS3_KNOB(on, env_on("DES_MOUSE_LOOK"));
    PS3_KNOB(inv_x, env_on("DES_MOUSE_INVERT_X"));
    PS3_KNOB(inv_y, env_on("DES_MOUSE_INVERT_Y"));
    PS3_KNOB(sens, mouse_sens());
    PS3_KNOB(flick, flick_degrees());
    PS3_KNOB(log, env_on("DES_MOUSELOG"));
    static int was_on = 0;
    if (on != was_on) {   /* turned on or off live: the window takes or lets go of the mouse */
        was_on = on;
        des_mouse_refresh();
    }
    if (s_turn_cam) {   /* last update's turn never reached 0x0011C758 */
        s_turn_cam = 0;
        s_mouse_counts[kLost] += s_turn_counts;
    }
    const uint32_t cam = (uint32_t)ctx->gpr[3];
    if (!guest_ptr_ok(cam) || !vm_read8(cam + (aim ? kAimInput : kCamActive))) return;
    int dx = 0, dy = 0;
    des_mouse_take(&dx, &dy);   /* (with mouse look off too: the game has the mouse's buttons while the camera runs) */
    if (!on) return;
    static unsigned last_flip = 0;   /* (both entries: one of the two cameras runs a game frame) */
    const unsigned flip = s_flips.load(std::memory_order_relaxed);
    const bool stale = flip - last_flip > kMouseStaleFlips;
    last_flip = flip;
    const float k = sens * 0.0174532925f;   /* radians per count */
    const float yaw_turn = (inv_x ? -1.0f : 1.0f) * (float)dx * k, pitch_turn = (inv_y ? -1.0f : 1.0f) * (float)dy * k;
    const int why = aim ? (stale ? kStale : (dx || dy) ? mouse_aim(cam, (uint32_t)ctx->gpr[5], yaw_turn, pitch_turn)
                                                       : kTurned)
                    : stale                        ? kStale
                    : vm_read8(cam + kCamSetAngle) ? kSetAngle
                    : vm_read8(cam + kCamLookAt)   ? kLookAt
                    : vm_read8(cam + kCamAutoTurn) ? kAutoTurn
                                                   : kTurned;
    if (why == kLookAt && flick > 0.0f) {
        mouse_flick(inv_x ? -dx : dx, sens, flick);
    } else {
        s_flick_x = 0;
        s_flick_rest = s_flick_idle = 0;
        s_flick_settle = true;
    }
    if (why != kTurned || aim) {   /* (the aim camera turned already; the follow camera's turn is counted when made) */
        s_mouse_counts[why] += std::abs(dx) + std::abs(dy);
    } else if (dx || dy) {
        s_turn_yaw = yaw_turn;
        s_turn_pitch = pitch_turn;
        s_turn_counts = std::abs(dx) + std::abs(dy);
        s_turn_cam = cam;
    }
    static unsigned updates;
    if (log && ++updates % 60 == 0) {
        const long long* c = s_mouse_counts;
        const float d2r = 0.0174532925f;   /* the aim camera's pitch limits are in degrees */
        fprintf(stderr, "[mouse] %s %08X counts turned %lld, lost %lld, dropped: gap %lld set-angle %lld look-at %lld "
                "auto-turn %lld; flicks %lld; yaw %.3f pitch %.3f in [%.3f, %.3f], %.3f deg/count\n", aim ? "aim" : "cam",
                cam, c[kTurned], c[kLost], c[kStale], c[kSetAngle], c[kLookAt], c[kAutoTurn], s_flicks,
                vm_readf(cam + (aim ? kAimYaw : kCamYaw)), vm_readf(cam + (aim ? kAimPitch : kCamPitch)),
                aim ? vm_readf(cam + kAimPitchMin) * d2r : vm_readf(cam + kCamPitchMin),
                aim ? vm_readf(cam + kAimPitchMax) * d2r : vm_readf(cam + kCamPitchMax), sens);
        memset(s_mouse_counts, 0, sizeof s_mouse_counts);
        s_flicks = 0;
    }
}

extern "C" void des_menu_sfx_init(void);   /* des_menu_sfx.cpp */
extern "C" void des_chrins_init(void);     /* des_chrins.cpp */
extern "C" void des_souls_use_init(void);  /* des_souls_use.cpp */

/* The run defaults every start needs, from setup's Play, Explorer or a
 * shortcut as much as from tools/des (docs/45 1.3). Without them a player's
 * start hung on the loading screen: libsre was never loaded, so every SPURS
 * call stayed on HLE, LFQueue included (measured 10-22, docs/88 section 11). A value
 * already in the environment wins, so tools/des/deslib.sh (des_env_defaults)
 * and tools/linux/appimage.sh, which set the same, still decide for their runs. */
static void run_defaults()
{
    static const struct { const char* name; const char* value; } kDefaults[] = {
        /* The known-good boot (docs/25): load libsre (read by the generated
         * libsre glue, scripts/gen_libsre_exports.py), these families (LFQueue,
         * SpursQueue) on Sony's code, the rest on HLE, and no round notify. */
        {"LIBSRE_LLE", "1"},
        {"LIBSRE_LLE_ONLY", "011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667"},
        {"SPURS_NO_ROUND_NOTIFY", "1"},
        /* The level's busiest frames need ~850k vertices; growing the buffer
         * during play stalled a frame for 100-400 ms (docs/28). */
        {"VP_VB_VERTS", "1572864"},
        /* No per-event log lines: they go through one lock and fill des.log. */
        {"PS3_VERBOSE", "0"},
        /* The main thread's stack where lv2 puts it, in the stack region
         * (0xD0000000 up): the lifter's stack stores take their direct path
         * only there (docs/98). */
        {"PS3_MAIN_STACK_LV2", "1"},
        /* HDR output's highlights (RSX_HDR): the composite stores the scene at
         * half brightness and clamps it to white once doubled; with HDR on it
         * clamps at the target's own limit, twice white (docs/103). */
        {"RSX_HDR_SCENE_FP", "B42C01:2"},
    };
    for (const auto& d : kDefaults)
        if (!getenv(d.name)) ps3_setenv(d.name, d.value);
}

extern "C" int des_prework_shaders(const char* game, const char* eboot);   /* des_prework.cpp */
extern "C" void des_prework_elf(const char* path);

/* main's ELF, as boot_main reads its arguments: the first that is not a
 * --username or --psnr flag (or its value). */
static const char* main_elf(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (std::strncmp(a, "--username", 10) && std::strncmp(a, "--psnr", 6)) return a;
        if (!std::strchr(a, '=')) i++;
    }
    return nullptr;
}

/* First thing in main (ps3recomp's launch hook, docs/88 section 3): setup's
 * shaders phase (`--shaders <game folder> [<EBOOT.elf>]`: the shader cache
 * filled with no window, then the exit), or the run defaults, then setup's
 * screens alone, or the game (des_ui_launch), whose ELF the shader pass at
 * the start reads too (des_prework_elf). */
extern "C" int ps3_title_launch(int* argc, char*** argv)
{
    if ((*argc == 3 || *argc == 4) && std::strcmp((*argv)[1], "--shaders") == 0)
        return des_prework_shaders((*argv)[2], *argc == 4 ? (*argv)[3] : nullptr);
    run_defaults();
    const int launch = des_ui_launch(argc, argv);
    if (launch == -1) des_prework_elf(main_elf(*argc, *argv));   /* (-1: PS3_LAUNCH_BOOT) */
    return launch;
}

/* Setup's screens alone, in place of ps3_title_options_init: no game, so none
 * of the game's options, hooks or menu sounds. */
extern "C" void ps3_title_window_only_init(void)
{
    des_ui_setup_only_init();
}

extern "C" void ps3_title_options_init(void)
{
    /* First: settings.ini fills in the options not set in the environment. */
    des_ui_early_init();
#if DES_DEV
    des_dev_boot();        /* developer build: the marker (log, window title) and the cheats' offline rule (docs/55) */
#endif
    des_aspect_init();     /* DES_ASPECT, DES_HUD: before the first camera is built */
    des_hud_init();        /* the HUD's size, corners and hidden groups */
    des_menu_sfx_init();   /* the UI's sounds through the game's menu sounds */
    des_chrins_init();     /* the character step's handshake without its lost-kick race */
    des_souls_use_init();  /* DES_SOULS_AT_ONCE: Use several soul items at once */
    /* DES_SKIP_INTRO and DES_AUTO_CONTINUE are read as they are used, so the
     * settings window changes them live; their hooks are always on. */
    ppu_register_function(kLogoUpdate, logo_update_skip);
    if (const int mode = skip_intro())
        fprintf(stderr, "[des] DES_SKIP_INTRO=%d: boot logos %s\n", mode, mode == 1 ? "skipped" : "shortened");
    if (env_on("DES_SKIP_INTRO_MOVIE") && !getenv("PS3_SAIL_SKIP_FIRST")) {
        ps3_setenv("PS3_SAIL_SKIP_FIRST", kOpeningMovie);
        fprintf(stderr, "[des] DES_SKIP_INTRO_MOVIE: the opening movie is skipped\n");
    }
    /* DES_FPS is read at each flip request, so the settings window changes it
     * live; the hook is always on. A VBLANK_MHZ given at start wins over the
     * cap. */
    s_vblank_pinned = getenv("VBLANK_MHZ") != nullptr;
    g_gcm_flip_request_hook = on_flip_request;
    ppu_register_function(kLoadSouls, load_souls_rate);   /* the loading screens' souls, in 30 Hz steps */
    ppu_register_function(kNowLoadingUpdate, nowloading_souls_rate);
    ppu_register_function(kScrollMsgUpdate, scroll_msg_rate);   /* menu text scroll, 1.5 px per 30 Hz step */
    ppu_register_function(kGetSoulUpdate, get_soul_rate);   /* the HUD's soul counter, a step per 30 Hz step */
    ppu_register_function(kGaugeDraw[0], gauge_fill_rate<func_0078EDC8>);   /* HUD bars' fill */
    ppu_register_function(kGaugeDraw[1], gauge_fill_rate<func_0078EB60>);
    ppu_register_function(kGaugeDraw[2], gauge_fill_rate<func_0078F020>);
    ppu_register_function(kGaugeUpdate, gauge_len_rate<func_00789BC0>);   /* HUD bars' length */
    ppu_register_function(kSmoothGaugeUpdate, gauge_len_rate<func_0078E2E8>);
    if (const int fps = fps_wanted())
        fprintf(stderr, "[des] DES_FPS=%s: real frame time, no skip frames%s\n", fps_name(fps).c_str(),
                fps_vblank_mhz(fps) ? ", vblank at the cap" : "");
    /* DES_VSYNC is read at each flip request (pacing_apply); an RSX_VSYNC
     * given at start wins over it. */
    s_vsync_pinned = getenv("RSX_VSYNC") != nullptr;
    if (const char* f = getenv("DES_FRAMELOG")) {
        if (*f && (s_framelog = fopen(f, "w")) != nullptr) {
            snprintf(s_framelog_path, sizeof s_framelog_path, "%s", f);
            fprintf(s_framelog, "# t_ms interval_ms step_ms cutscene pad buttons lstick reads empty [x y z stamina want_m/s speed_factor hp mp]; SUM t fps speed m/s game/real; MARK t (F8)\n");
            g_gcm_flip_request_hook = on_flip_request;
            fprintf(stderr, "[des] DES_FRAMELOG: %s%s\n", f, ps3_env_int("RSX_DEV_HOTKEYS", 0) ? " (F8 marks)" : "");
        } else if (*f) {
            fprintf(stderr, "[des] DES_FRAMELOG: cannot open %s\n", f);
        }
    }
    g_pad_title_hook = title_buttons;
    g_fs_open_hook = auto_continue_on_open;
    if (auto_continue_on())
        fprintf(stderr, "[des] DES_AUTO_CONTINUE: pressing through the menus to the last save\n");
}
