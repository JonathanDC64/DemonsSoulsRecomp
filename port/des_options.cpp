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
 *   sets 60 mode and a vblank of n Hz (VBLANK_MHZ, unless that is set
 *   already), a cap of n, and presents that do not wait for the display's
 *   refresh (RSX_VSYNC=0, unless set). Movies keep their speed (cellSail
 *   hands out the frame that is due by the clock), and the game advances by
 *   real time: the regress walk ends in the same place at ~66 fps as at 30.
 *   Not the in-engine cutscenes (RemoMan): their positions step at 30 Hz
 *   while animation follows the frame time, so above 30 a cutscene judders,
 *   a 2-frame back-and-forth at 60 and a 4-frame sawtooth at 120 (the
 *   tutorial arrival; the default mode is smooth).
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void (*g_gcm_flip_request_hook)(void);
extern "C" unsigned (*g_pad_title_hook)(void);
extern "C" void (*g_fs_open_hook)(const char* guest_path);

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

bool s_fps60 = false;

void fps_on_flip_request()
{
    static bool mode_set = false;
    if (!mode_set) {
        const uint32_t obj = vm_read32(kFlipModeObj);
        if (obj) {
            vm_write8(obj + kFlipMode60, s_fps60 ? 1 : 0);
            vm_write8(obj + kFlipNoSkip, 1);
            mode_set = true;
            fprintf(stderr, "[des] DES_FPS: FlipMode %s set on 0x%08X\n",
                    s_fps60 ? "60FPS(NO SKIP)" : "30FPS(NO SKIP)", obj);
        }
    }
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    const clock::time_point now = clock::now();
    if (last != clock::time_point()) {
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt > kMaxFrameDt) dt = kMaxFrameDt;
        uint32_t bits;
        memcpy(&bits, &dt, 4);
        vm_write32(kFrameDt, bits);
    }
    last = now;
}

constexpr unsigned kPadCross = 0x4000u, kPadStart = 0x0008u;
volatile bool s_auto_done = false;

unsigned auto_continue_buttons()
{
    if (s_auto_done) return 0;
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

} // namespace

extern "C" void ps3_title_options_init(void)
{
    if (env_on("DES_SKIP_INTRO")) {
        ppu_register_function(kLogoUpdate, logo_update_skip);
        fprintf(stderr, "[des] DES_SKIP_INTRO: boot logos shortened\n");
    }
    if (const char* f = getenv("DES_FPS")) {
        const int fps = atoi(f);
        if (fps == 60 || fps == 30 || (fps > 60 && fps <= 240)) {
            s_fps60 = fps >= 60;
            g_gcm_flip_request_hook = fps_on_flip_request;
            /* Before the vblank clock starts: boot_main reads the rate once.
             * Presents stop waiting for the display's refresh too (RSX_VSYNC),
             * or a 60 Hz desktop holds the game to 60 whatever the vblank. */
            if (fps > 60) {
                char v[16];
                snprintf(v, sizeof v, "%d", fps * 1000);
                if (!getenv("VBLANK_MHZ")) ps3_setenv("VBLANK_MHZ", v);
                if (!getenv("RSX_VSYNC"))  ps3_setenv("RSX_VSYNC", "0");
            }
            fprintf(stderr, "[des] DES_FPS=%d: real frame time, no skip frames%s\n", fps,
                    fps > 60 ? ", vblank at the cap" : "");
        } else if (*f) {
            fprintf(stderr, "[des] DES_FPS=%s ignored (30, 60, or 61-240)\n", f);
        }
    }
    if (env_on("DES_AUTO_CONTINUE")) {
        g_pad_title_hook = auto_continue_buttons;
        g_fs_open_hook = auto_continue_on_open;
        fprintf(stderr, "[des] DES_AUTO_CONTINUE: pressing through the menus to the last save\n");
    }
}
