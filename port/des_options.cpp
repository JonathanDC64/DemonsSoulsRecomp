/* Demon's Souls — user-facing options (host-side knobs on the game's own state).
 *
 * These are not workarounds: each one drives a state machine the game already
 * has, through the same transitions it takes on its own, just sooner. All are
 * off by default.
 *
 * DES_SKIP_INTRO=1 — shorten the boot logo sequence (DsMenuDlgLogo).
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
 *   With the option on, a logo that is about to start fading in (state 0) is
 *   made the last one, and the hold of state 2 is treated as already over. The
 *   game still plays the last logo's fade-in and fade-out and takes its own exit
 *   path, so nothing downstream sees a state it would not see on hardware.
 *
 * DES_FPS=30 | 60 | 61..240 — the game's own frame-rate mode, with real frame time.
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
 *   once a vblank, so the vblank is the frame limit: DES_FPS=<n> above 60
 *   sets 60 mode and a vblank of n Hz (unless VBLANK_MHZ was set at start)
 *   and so a cap of n. DES_FPS is read at every flip request, so a change
 *   (the settings window's, or the console's setenv) applies at once, and
 *   an empty value or 0 puts back the game's own mode. Presents still wait for the display's
 *   refresh unless DES_VSYNC=0, so with vsync a 144 Hz display shows at most
 *   144 of them a second. Movies keep their speed (cellSail
 *   hands out the frame that is due by the clock), and the game advances by
 *   real time: the regress walk ends in the same place at ~66 fps as at 30.
 *   The in-engine cutscenes (RemoMan) stay at 30: their positions step at
 *   30 Hz while animation follows the frame time, so unlocked they judder
 *   (a 2-frame back-and-forth at 60, a 4-frame sawtooth at 120). While a
 *   RemoMan slot holds a cutscene the game is put back in 30 mode with the
 *   1/30 step and the PS3's vblank, and unlocked again when it ends.
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

bool env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

void logo_update_skip(ppu_context* ctx)
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    if (dlg) {
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
 * mode, else the frame rate. Render thread only (the flip request hook). */
int s_fps = -1;
bool s_vblank_pinned = false;   /* VBLANK_MHZ set at start: it wins over the cap */

int fps_parse(const char* f)
{
    const int fps = f ? atoi(f) : 0;
    if (fps == 60 || fps == 30 || (fps > 60 && fps <= 240)) return fps;
    if (f && *f && strcmp(f, "0") != 0)
        fprintf(stderr, "[des] DES_FPS=%s ignored (30, 60, or 61-240)\n", f);
    return 0;
}

/* Read live: the settings window sets it with ps3_setenv. */
int fps_wanted()
{
    static int fps = 0;
    PS3_KNOB(fps, fps_parse(getenv("DES_FPS")));
    return fps;
}

/* The vblank while unlocked: above 60 the cap, else the PS3's (0 hands the
 * clock back to VBLANK_MHZ, which a user setting it at start keeps). */
int fps_vblank_mhz(int fps)
{
    return fps > 60 && !s_vblank_pinned ? fps * 1000 : 0;
}

bool cutscene_playing()
{
    const uint32_t man = vm_read32(kRemoMan);
    if (!man) return false;
    for (uint32_t off : kRemoSlots)
        if (vm_read32(man + off)) return true;
    return false;
}

void fps_on_flip_request()
{
    using clock = std::chrono::steady_clock;
    static uint8_t orig60, orig_noskip;
    static uint32_t orig_dt;
    static bool in_cutscene = false;
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
            vm_write8(obj + kFlipMode60, want >= 60 ? 1 : 0);
            vm_write8(obj + kFlipNoSkip, 1);
            ps3_vblank_set_mhz(fps_vblank_mhz(want));
            fprintf(stderr, "[des] DES_FPS=%d: FlipMode %s on 0x%08X, real frame time%s\n", want,
                    want >= 60 ? "60FPS(NO SKIP)" : "30FPS(NO SKIP)", obj,
                    fps_vblank_mhz(want) ? ", vblank at the cap" : "");
        } else if (s_fps > 0) {
            vm_write8(obj + kFlipMode60, orig60);
            vm_write8(obj + kFlipNoSkip, orig_noskip);
            vm_write32(kFrameDt, orig_dt);
            ps3_vblank_set_mhz(0);
            fprintf(stderr, "[des] DES_FPS off: the game's own 30 fps mode\n");
        }
        s_fps = want;
        in_cutscene = false;
        last = clock::time_point();
    }
    if (!s_fps) return;
    /* A cutscene plays as the game ships it: 30 mode on the PS3's vblank, so
     * it flips every other vblank, and a fixed 1/30 step. RemoMan steps its
     * positions at 30 Hz while animation follows the frame step, so any
     * faster and a cutscene judders. The vblank goes back to the unlocked
     * rate when the cutscene ends. */
    const bool cut = s_fps >= 60 && cutscene_playing();
    if (cut != in_cutscene) {
        in_cutscene = cut;
        vm_write8(obj + kFlipMode60, cut ? 0 : 1);
        ps3_vblank_set_mhz(cut ? kPs3VblankMhz : fps_vblank_mhz(s_fps));
        fprintf(stderr, "[des] DES_FPS: %s\n",
                cut ? "cutscene, 30 fps" : "cutscene over, unlocked again");
    }
    const clock::time_point now = clock::now();
    float dt = kCutsceneDt;
    if (!cut && last != clock::time_point()) {
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

} // namespace

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

extern "C" void des_menu_sfx_init(void);   /* des_menu_sfx.cpp */

extern "C" void ps3_title_options_init(void)
{
    /* First: settings.ini fills in the options not set in the environment. */
    des_ui_early_init();
    des_menu_sfx_init();   /* the UI's sounds through the game's menu sounds */
    if (env_on("DES_SKIP_INTRO")) {
        ppu_register_function(kLogoUpdate, logo_update_skip);
        fprintf(stderr, "[des] DES_SKIP_INTRO: boot logos shortened\n");
    }
    /* DES_FPS is read at each flip request, so the settings window changes it
     * live; the hook is always on. A VBLANK_MHZ given at start wins over the
     * cap. */
    s_vblank_pinned = getenv("VBLANK_MHZ") != nullptr;
    g_gcm_flip_request_hook = on_flip_request;
    if (const int fps = fps_wanted())
        fprintf(stderr, "[des] DES_FPS=%d: real frame time, no skip frames%s\n", fps,
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
            fprintf(s_framelog, "# t_ms interval_ms step_ms cutscene pad buttons lstick reads empty [x y z stamina want_m/s]; SUM t fps speed m/s game/real; MARK t (F8)\n");
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
