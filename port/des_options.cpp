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
 *   state it would not see on hardware.
 *
 * DES_SKIP_INTRO_MOVIE=1 — skip the opening movie (movie/ds_advertise.pam, the
 *   130 s advert after the logos; the title screen also replays it when left
 *   idle). It sets PS3_SAIL_SKIP, so cellSail ends that movie as soon as it
 *   starts, as START does on hardware; the title screen's own movie, the one
 *   after character creation and the endings still play. The settings window
 *   changes it live (from the next movie).
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
 * DES_VSYNC=0 | 1 — whether presents wait for the display's refresh (default
 *   1). 0 presents each frame as soon as it is drawn, with tearing, where the
 *   system supports it (the D3D12 backend asks DXGI; without it, it presents
 *   with vsync and says so once in the log). Sets RSX_VSYNC unless that is
 *   set already. Above the display's refresh (DES_FPS over 60 on a 60 or
 *   144 Hz display) only DES_VSYNC=0 shows every frame.
 *
 * DES_AUTO_CONTINUE=1 — press through the boot menus into the last save.
 *   From the first pad poll it taps CROSS, CROSS, START in a 600 ms cycle:
 *   that clears the notices, the title's "press start", "load game" and the
 *   first save in the list. It never presses CIRCLE or the d-pad, so it only
 *   goes forwards and never moves a cursor off its default. It lets go for
 *   good when the level's script archive (script/m*.luabnd) opens.
 */
#include "ppu_recomp.h"
#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include "ui/include/des_ui.h"  /* the wrapper UI: settings.ini and the overlay (docs/32) */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void (*g_gcm_flip_request_hook)(void);
extern "C" unsigned (*g_pad_title_hook)(void);
extern "C" void (*g_fs_open_hook)(const char* guest_path);
extern "C" void ps3_vblank_set_mhz(int mhz);   /* boot_main.cpp; 0 = VBLANK_MHZ */
extern "C" volatile unsigned g_pad0_reads, g_pad0_empty_reads, g_pad0_buttons, g_pad0_lstick;   /* cellPad.c */
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vkey);   /* user32 */
constexpr int VK_F8 = 0x77;

namespace {

constexpr uint32_t kLogoUpdate   = 0x008EF430u;
constexpr uint32_t kElapseTime   = 0x0F8u;
constexpr uint32_t kCurLogType   = 0x0FCu;
constexpr uint32_t kAnimState    = 0x100u;
constexpr uint32_t kLastLogType  = 4u;
constexpr const char* kOpeningMovie = "ds_advertise.pam";   /* PS3_SAIL_SKIP for DES_SKIP_INTRO_MOVIE */

bool env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

int s_skip_intro = 0;

void logo_update_skip(ppu_context* ctx)
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    if (dlg && s_skip_intro == 1) {
        vm_write32(dlg + kAnimState, 3);   /* to the next logo, or out after the last */
    } else if (dlg) {
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

void fps_on_flip_request()
{
    using clock = std::chrono::steady_clock;
    static uint8_t orig60, orig_noskip;
    static uint32_t orig_dt;
    static int in_cutscene = 0;   /* 0 none, 1 at 30 (the lock), 2 unlocked (DES_CUTFPS) */
    static clock::time_point last;
    const uint32_t obj = vm_read32(kFlipModeObj);
    if (!obj) return;
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
            vm_write8(obj + kFlipMode60, want > 30 ? 1 : 0);
            vm_write8(obj + kFlipNoSkip, 1);
            ps3_vblank_set_mhz(fps_vblank_mhz(want));
            fprintf(stderr, "[des] DES_FPS=%s: FlipMode %s on 0x%08X, real frame time%s\n",
                    fps_name(want).c_str(), want > 30 ? "60FPS(NO SKIP)" : "30FPS(NO SKIP)", obj,
                    fps_vblank_mhz(want) ? ", vblank at the cap" : "");
        } else if (s_fps > 0) {
            vm_write8(obj + kFlipMode60, orig60);
            vm_write8(obj + kFlipNoSkip, orig_noskip);
            vm_write32(kFrameDt, orig_dt);
            ps3_vblank_set_mhz(0);
            fprintf(stderr, "[des] DES_FPS off: the game's own 30 fps mode\n");
        }
        s_fps = want;
        in_cutscene = 0;
        last = clock::time_point();
    }
    if (!s_fps) return;
    /* A cutscene plays as the game ships it: 30 mode on the PS3's vblank, so
     * it flips every other vblank, and a fixed 1/30 step. Its camera takes
     * the nearest of keys 1/30 s apart while everything else follows the
     * frame step, so any faster and a cutscene judders. The vblank goes back
     * to the unlocked rate when the cutscene ends. With DES_CUTFPS=1 a
     * cutscene stays unlocked and des_cut_cam blends the camera's keys. */
    const int cut = s_fps > 30 && cutscene_playing() ? (cutfps_on() ? 2 : 1) : 0;
    if (cut != in_cutscene) {
        if ((cut == 1) != (in_cutscene == 1)) {
            vm_write8(obj + kFlipMode60, cut == 1 ? 0 : 1);
            ps3_vblank_set_mhz(cut == 1 ? kPs3VblankMhz : fps_vblank_mhz(s_fps));
        }
        fprintf(stderr, "[des] DES_FPS: %s\n",
                cut == 1 ? "cutscene, 30 fps" : cut == 2 ? "cutscene, unlocked (DES_CUTFPS)"
                : in_cutscene == 1 ? "cutscene over, unlocked again" : "cutscene over");
        in_cutscene = cut;
    }
    const clock::time_point now = clock::now();
    float dt = kCutsceneDt;
    if (cut != 1 && last != clock::time_point()) {
        dt = std::chrono::duration<float>(now - last).count();
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
bool guest_ptr_ok(uint32_t a) { return a >= 0x00010000u && a < 0x50000000u && !(a & 3u); }

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
 * of the player's ground speed and the sum of steps against real time. F8
 * writes a MARK line, so a player can tag the moment something looks wrong. */
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
    if (pos) {   /* the movement object's root-motion factor (see des_move_post) */
        const uint32_t chr = vm_read32(vm_read32(kChrMan) + 0x1BC);
        const uint32_t fb = vm_read32(vm_read32(chr + 0x20) + 0x128);
        float f; memcpy(&f, &fb, 4);
        fprintf(s_framelog, " %.3f", f);
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
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
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

void on_flip_request()
{
    fps_on_flip_request();
    if (s_framelog) frame_log();
}

constexpr unsigned kPadCross = 0x4000u, kPadStart = 0x0008u;
volatile bool s_auto_done = false;

extern "C" int des_ui_start_screen_up(void);   /* port/ui/src/des_ui.cpp */

unsigned auto_continue_buttons()
{
    /* From settings.ini the start screen still shows: press nothing until Play. */
    if (s_auto_done || des_ui_start_screen_up()) return 0;
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() % 600;
    if (ms < 100 || (ms >= 200 && ms < 300)) return kPadCross;
    if (ms >= 400 && ms < 500) return kPadStart;
    return 0;
}

void auto_continue_on_open(const char* path)
{
    if (!s_auto_done && strstr(path, "/script/m") && strstr(path, ".luabnd")) {
        s_auto_done = true;
        fprintf(stderr, "[des] DES_AUTO_CONTINUE: level loading (%s), pad released\n", path);
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

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

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
    if (!on) ctx->cr ^= 2u;   /* cr7 is the low field; EQ = 2 */
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

namespace {

constexpr uint32_t kCutCamKeys  = 0x005F7254u;   /* lwz r9: s, the gap and the six key indices are set */
constexpr uint32_t kCutCamPos   = 0x005F72ECu;   /* lvx v4: earlier key at r1+0xA0, later at r1+0xB0 */
constexpr uint32_t kCutCamRot   = 0x005F7394u;   /* lvx v12: earlier r1+0xD0, later r1+0xA0 */
constexpr uint32_t kCutCamScale = 0x005F743Cu;   /* lvx v2: earlier r1+0x70, later r1+0xA0 */
constexpr float    kCutMaxMove  = 2.0f;          /* m in one 1/30 s step */
constexpr float    kCutMaxTurn  = 0.5f;          /* rad, any angle, in one step */
constexpr float    kTwoPi       = 6.28318531f;

/* The choice for the call of 0x005F71D0 running on this thread, made at
 * kCutCamKeys before its three loads: blend them, or leave them alone. */
PPU_THREAD_LOCAL bool s_cut_blend;

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
 * code after the load wraps them again. A register that holds neither key
 * is left alone. DES_CUTLOG=1 counts what it did. Main thread. */
extern "C" void des_cut_cam(ppu_context* ctx, uint32_t addr)
{
    static int log = 0;
    PS3_KNOB(log, env_on("DES_CUTLOG"));
    static unsigned n_calls, n_blend, n_apart, n_jump, n_odd, n_neither;
    static double widest;   /* the widest gap between keys, frames */
    if (addr == kCutCamKeys) {
        s_cut_blend = false;
        if (!cutfps_on() || s_fps <= 0) return;
        const double s = ctx->fpr[10], gap = ctx->fpr[11];
        if (!(s > 0.0 && s < 1.0)) return;   /* t on a key: the loads are exact */
        const uint32_t h = (uint32_t)ctx->gpr[7];
        float p0[3], p1[3], r0[3], r1[3], k0[3], k1[3];
        const int pos   = cut_keys(h, 0x2C, (int32_t)ctx->gpr[6], (int32_t)ctx->gpr[4], p0, p1);
        const int rot   = cut_keys(h, 0x2F, (int32_t)ctx->gpr[29], (int32_t)ctx->gpr[31], r0, r1);
        const int scale = cut_keys(h, 0x32, (int32_t)ctx->gpr[12], (int32_t)ctx->gpr[28], k0, k1);
        float move = 0.0f, turn = 0.0f;
        for (int c = 0; c < 3; c++) {
            if (pos > 0) move += (p1[c] - p0[c]) * (p1[c] - p0[c]);
            if (rot > 0) turn = std::fmax(turn, std::fabs(angle_delta(r0[c], r1[c])));
        }
        move = std::sqrt(move);
        const bool apart = std::fabs(gap * 30.0 - 1.0) > 0.01;
        const bool jump  = !apart && (move > kCutMaxMove || turn > kCutMaxTurn);
        const bool odd   = pos < 0 || rot < 0 || scale < 0;
        s_cut_blend = !apart && !jump && !odd;
        if (log) {
            n_calls++;
            if (s_cut_blend) n_blend++;
            if (apart) { n_apart++; if (gap * 30.0 > widest) widest = gap * 30.0; }
            if (jump) n_jump++;
            if (odd) n_odd++;
            static uint32_t jump_h;
            static double jump_t0 = -1.0;
            if (jump && !(h == jump_h && ctx->fpr[12] == jump_t0)) {   /* f12: the earlier key's time */
                jump_h = h;
                jump_t0 = ctx->fpr[12];
                fprintf(stderr, "[cutlog] t=%.3f s: a jump (move %.2f m, turn %.2f rad), nearest key kept\n",
                        ctx->fpr[1], move, turn);
            }
            if (n_calls % 1000 == 0)
                fprintf(stderr, "[cutlog] calls between keys %u: blended %u, keys apart %u (widest %.1f frames),"
                        " jumps %u, one index unset %u; loads holding neither key %u\n",
                        n_calls, n_blend, n_apart, widest, n_jump, n_odd, n_neither);
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
        const float d = i == 3 ? 0.0f : ch == 1 ? angle_delta(x[i], y[i]) : y[i] - x[i];
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

extern "C" void des_menu_sfx_init(void);   /* des_menu_sfx.cpp */

extern "C" void ps3_title_options_init(void)
{
    /* First: settings.ini fills in the options not set in the environment. */
    des_ui_early_init();
    des_menu_sfx_init();   /* the UI's sounds through the game's menu sounds */
    if (env_on("DES_SKIP_INTRO")) {
        s_skip_intro = atoi(getenv("DES_SKIP_INTRO")) == 2 ? 2 : 1;
        ppu_register_function(kLogoUpdate, logo_update_skip);
        fprintf(stderr, "[des] DES_SKIP_INTRO=%d: boot logos %s\n", s_skip_intro,
                s_skip_intro == 1 ? "skipped" : "shortened");
    }
    if (env_on("DES_SKIP_INTRO_MOVIE") && !getenv("PS3_SAIL_SKIP")) {
        ps3_setenv("PS3_SAIL_SKIP", kOpeningMovie);
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
    if (const char* v = getenv("DES_VSYNC")) {
        if (*v && !getenv("RSX_VSYNC")) {
            const bool on = atoi(v) != 0;
            ps3_setenv("RSX_VSYNC", on ? "1" : "0");
            if (!on) fprintf(stderr, "[des] DES_VSYNC=0: presents do not wait for the display\n");
        }
    }
    if (const char* f = getenv("DES_FRAMELOG")) {
        if (*f && (s_framelog = fopen(f, "w")) != nullptr) {
            snprintf(s_framelog_path, sizeof s_framelog_path, "%s", f);
            fprintf(s_framelog, "# t_ms interval_ms step_ms cutscene pad buttons lstick reads empty [x y z stamina want_m/s speed_factor]; SUM t fps speed m/s game/real; MARK t (F8)\n");
            g_gcm_flip_request_hook = on_flip_request;
            fprintf(stderr, "[des] DES_FRAMELOG: %s (F8 marks)\n", f);
        } else if (*f) {
            fprintf(stderr, "[des] DES_FRAMELOG: cannot open %s\n", f);
        }
    }
    if (env_on("DES_AUTO_CONTINUE")) {
        g_pad_title_hook = auto_continue_buttons;
        g_fs_open_hook = auto_continue_on_open;
        fprintf(stderr, "[des] DES_AUTO_CONTINUE: pressing through the menus to the last save\n");
    }
}
