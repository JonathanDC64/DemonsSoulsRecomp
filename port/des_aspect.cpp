/* Wider aspect ratios: a 21:9 or 32:9 picture with the HUD kept 16:9 (docs/39).
 *
 * DES_ASPECT=<w:h> | <W>x<H> | <number> | auto (default unset: 16:9, the
 *   game's own) - the shape of the picture. Read once at start.
 *   The game builds each perspective camera from a default table of fov,
 *   aspect, near and far. The aspect is 16/9 in two places: 0x018CEFBC, which
 *   the game cameras' constructors (0x0012F030, 0x0012F098) copy to camera
 *   +0x54, and 0x0190433C, a render context's first projection (0x00CD4E58,
 *   0x00CD54B8). The projection (0x00CD3CE0) takes x scale = cot(fov/2) /
 *   aspect, so the FOV is vertical: a wider aspect written into both words
 *   before the first camera is built shows more at the sides at the same
 *   height (Hor+), and the frustum (0x0012F100) reads the same camera field,
 *   so culling widens with it. RPCS3's "Aspect Ratio" patch writes the same
 *   two words. The game still draws 1280x720, the wider view squeezed into
 *   it, so this also sets RSX_ASPECT (unless it is set already): the D3D12
 *   backend shows the frame at that shape, the window starts in it, and
 *   fullscreen fills an ultrawide display. A shape narrower than 16:9 (16:10,
 *   4:3) is not supported: the picture stays 16:9 with bars, as without the
 *   option. A shape wider than 4:1 is taken as 4:1. auto is the primary
 *   display's shape.
 *
 * DES_HUD=stretch - the HUD and menus spread across the full width. By default
 *   (when DES_ASPECT is wider than 16:9) they keep their size and shape in a
 *   16:9 band in the middle:
 *   - The canvas. At boot, 0x00718918 records the menu context's ortho canvas
 *     once, command 0x3A [top 0, bottom 720, left 0, right 1280, near -128,
 *     far 128], the words one after another at the list cursor. The lifter
 *     calls des_hud_canvas after the store of the right word (0x00718D84,
 *     r11 = its address, --insn-hooks); it widens the canvas to [-L, 1280 + L]
 *     with L = (720 * A - 1280) / 2, the whole stretched picture, so 0..1280
 *     is the band in the middle. RPCS3's "Aspect ratio (HUD/Menus)" patch
 *     makes the same canvas.
 *   - Markers on things in the world. WorldToScreen (0x0079EE00) maps a point
 *     to x = (ndc_x * 0.5 + 0.5) * 1280, which would pull the lock-on reticle
 *     and the floating health bars towards the middle. des_hud_w2s, after its
 *     store (0x0079EF7C, stvx v31,0,r29), maps x to 640 + (x - 640) * k, with
 *     k = (1280 + 2L) / 1280, the whole canvas.
 *   - On-screen tests. Four HUD pieces hide a marker whose x is outside
 *     0..1280: TargettingSite (0x007B5AB0) and its update (0x007B6178),
 *     FloatingPCGauge (0x008AEA98), and 0x007A61D8 (the +0xBA0 slot).
 *     des_hud_onscreen runs after each one loads x into f13 and maps
 *     [-L, 1280 + L] onto 0..1280, so a marker in a side band still shows.
 *     f13 is used by nothing there but the two comparisons.
 *   - Full-screen plates. The fade (DsMenuDlgFadePlate, vtable 0x0186E1E0)
 *     and the dim behind menus (DsMenuDlgCommonBackPlate, 0x01870C30) are
 *     dialogs with a rect at +0x74 (left, top, right, bottom: 0x00F51180
 *     moves both corners). A rect of the whole 1280x720 canvas covers only
 *     the band on the widened canvas. Their updates (vtable +0x44: the
 *     DsMenuDlg base update 0x0071A6B8 for the fade, 0x008F2A58 for the dim)
 *     are wrapped here (ppu_register_function, which the vtable call goes
 *     through). After each, the plate's rect is logged when it changes (four
 *     times at most). With DES_HUD_FADES=1 a rect that is exactly the canvas
 *     is widened to [-L, 1280 + L]. That is opt-in until a run shows the
 *     plates are drawn from that rect.
 *   Not adjusted: anything drawn on the canvases that 0x00096348 records.
 */

#include "ppu_recomp.h"
#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall GetSystemMetrics(int index);   /* user32 */
#endif

namespace {

constexpr uint32_t kCameraFov     = 0x018CEFB4u;   /* the game cameras' default table: fov, aspect, near, far */
constexpr uint32_t kCameraAspect  = 0x018CEFBCu;
constexpr uint32_t kContextAspect = 0x0190433Cu;   /* a render context's first projection */
constexpr float    kGameAspect    = 16.0f / 9.0f;
constexpr float    kMaxAspect     = 4.0f;
constexpr float    kCanvasW       = 1280.0f, kCanvasH = 720.0f;

constexpr uint32_t kDlgBaseUpdate   = 0x0071A6B8u;   /* DsMenuDlg's update, the fade plate's (vtable +0x44) */
constexpr uint32_t kBackPlateUpdate = 0x008F2A58u;   /* DsMenuDlgCommonBackPlate's update (vtable +0x44) */
constexpr uint32_t kFadePlateVt     = 0x0186E1E0u;
constexpr uint32_t kBackPlateVt     = 0x01870C30u;
constexpr uint32_t kDlgRect         = 0x74u;         /* left, top, right, bottom */

float s_hud_l;        /* the canvas's extra width on each side; 0 = the game's canvas */
float s_hud_k = 1.0f; /* (1280 + 2L) / 1280 */
bool  s_widen_plates; /* DES_HUD_FADES=1 */

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

/* "21:9", "2560:1080", "3440x1440", "2.37": width over height, 0 if it does not read as one. */
float parse_aspect(const char* s)
{
    char* end = nullptr;
    double a = strtod(s, &end);
    if (end == s) return 0.0f;
    if (*end == ':' || *end == 'x' || *end == 'X' || *end == '/') {
        const char* h = end + 1;
        const double b = strtod(h, &end);
        if (end == h || !(b > 0.0)) return 0.0f;
        a /= b;
    }
    return *end == '\0' && a > 0.0 && std::isfinite(a) ? (float)a : 0.0f;
}

/* The primary display's shape (the window opens there), 0 if unknown. */
float display_aspect()
{
#ifdef _WIN32
    const int w = GetSystemMetrics(0), h = GetSystemMetrics(1);   /* SM_CXSCREEN, SM_CYSCREEN */
    return w > 0 && h > 0 ? (float)w / (float)h : 0.0f;
#else
    return 0.0f;
#endif
}

/* A full-screen plate after its update: its rect logged when it changes (four
 * times at most), and with DES_HUD_FADES=1 widened when it is exactly the
 * canvas. Main thread. */
struct PlateLog {
    const char* name;
    float last[4];
    int said;
};
PlateLog s_fade_log = {"FadePlate", {}, 0};
PlateLog s_back_log = {"CommonBackPlate", {}, 0};

void plate_after_update(uint32_t dlg, PlateLog& log)
{
    const uint32_t r = dlg + kDlgRect;
    const float rc[4] = {vm_readf(r), vm_readf(r + 4), vm_readf(r + 8), vm_readf(r + 12)};
    const bool full = rc[0] == 0.0f && rc[1] == 0.0f && rc[2] == kCanvasW && rc[3] == kCanvasH;
    if (log.said < 4 && memcmp(rc, log.last, sizeof rc) != 0) {
        memcpy(log.last, rc, sizeof rc);
        log.said++;
        const char* note = !full ? "" : s_widen_plates ? ": the whole canvas, widened"
                                                       : ": the whole canvas (DES_HUD_FADES=1 widens it)";
        fprintf(stderr, "[des] HUD: %s 0x%08X rect x %g to %g, y %g to %g%s\n", log.name, dlg, rc[0], rc[2], rc[1],
                rc[3], note);
    }
    if (full && s_widen_plates) {
        vm_writef(r, -s_hud_l);
        vm_writef(r + 8, kCanvasW + s_hud_l);
    }
}

void fade_plate_update(ppu_context* ctx)   /* for 0x0071A6B8 */
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    func_0071A6B8(ctx);
    if (dlg && vm_read32(dlg) == kFadePlateVt) plate_after_update(dlg, s_fade_log);
}

void back_plate_update(ppu_context* ctx)   /* for 0x008F2A58 */
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    func_008F2A58(ctx);
    if (dlg && vm_read32(dlg) == kBackPlateVt) plate_after_update(dlg, s_back_log);
}

} // namespace

/* After 0x00718D84, stfs f13,0(r11): the menu canvas's right word. The words
 * before it are left (r11 - 4), bottom (- 8) and top (- 12). */
extern "C" void des_hud_canvas(ppu_context* ctx, uint32_t addr)
{
    if (s_hud_l <= 0.0f) return;
    const uint32_t right = (uint32_t)ctx->gpr[11];
    if (vm_readf(right) != kCanvasW || vm_readf(right - 4) != 0.0f || vm_readf(right - 8) != kCanvasH ||
        vm_readf(right - 12) != 0.0f)
        return;
    vm_writef(right - 4, -s_hud_l);
    vm_writef(right, kCanvasW + s_hud_l);
    static bool said;
    if (!said) {
        said = true;
        fprintf(stderr, "[des] HUD canvas: x %.2f to %.2f, y 0 to 720 (0x%08X, list word 0x%08X)\n", -s_hud_l,
                kCanvasW + s_hud_l, addr, right);
    }
}

/* After 0x0079EF7C, stvx v31,0,r29: WorldToScreen's result, x first. */
extern "C" void des_hud_w2s(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    if (s_hud_l <= 0.0f) return;
    const uint32_t p = (uint32_t)ctx->gpr[29] & ~0xFu;   /* stvx ignores the low four bits */
    vm_writef(p, kCanvasW * 0.5f + (vm_readf(p) - kCanvasW * 0.5f) * s_hud_k);
}

/* After an on-screen test's lfs f13 of a marker's x (0x007B5AE8, 0x007B61D4,
 * 0x008AEB68, 0x007A6264). */
extern "C" void des_hud_onscreen(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    if (s_hud_l <= 0.0f) return;
    ctx->fpr[13] = (float)((ctx->fpr[13] + s_hud_l) / s_hud_k);
}

/* From ps3_title_options_init, after settings.ini is read and before the guest
 * runs, so before the first camera is built. */
extern "C" void des_aspect_init(void)
{
    const char* v = getenv("DES_ASPECT");
    if (!v || !*v) return;
    const bool from_display = strcmp(v, "auto") == 0;
    float a = from_display ? display_aspect() : parse_aspect(v);
    if (!(a > 0.0f)) {
        fprintf(stderr, "[des] DES_ASPECT=%s: %s; the picture stays 16:9\n", v,
                from_display ? "the display's shape is unknown" : "not a shape (w:h, WxH or a number)");
        return;
    }
    if (a < kGameAspect * 0.999f) {
        fprintf(stderr, "[des] DES_ASPECT=%s (%.4f:1) is narrower than 16:9, which is not supported yet: "
                "the picture stays 16:9, with bars\n", v, a);
        return;
    }
    if (a <= kGameAspect * 1.001f) {
        if (from_display) fprintf(stderr, "[des] DES_ASPECT=auto: the display is 16:9, the game's own shape\n");
        return;
    }
    if (a > kMaxAspect) {
        fprintf(stderr, "[des] DES_ASPECT=%s (%.4f:1): wider than 4:1 is not supported; the picture is 4:1\n", v, a);
        a = kMaxAspect;
    }
    const float was_camera = vm_readf(kCameraAspect), was_context = vm_readf(kContextAspect);
    if (std::fabs(was_camera - kGameAspect) > 1e-5f || std::fabs(was_context - kGameAspect) > 1e-5f) {
        fprintf(stderr, "[des] DES_ASPECT: the default aspect words hold %g and %g, not 16:9 (another release "
                "of the game?): left alone, the picture stays 16:9\n", was_camera, was_context);
        return;
    }
    vm_writef(kCameraAspect, a);
    vm_writef(kContextAspect, a);
    const char* shown = getenv("RSX_ASPECT");
    if (!shown || !*shown) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.6f", a);
        ps3_setenv("RSX_ASPECT", buf);
    }
    const char* hud = getenv("DES_HUD");
    const bool stretch = hud && strcmp(hud, "stretch") == 0;
    if (!stretch) {
        s_hud_l = (kCanvasH * a - kCanvasW) * 0.5f;
        s_hud_k = kCanvasH * a / kCanvasW;
        s_widen_plates = ps3_env_int("DES_HUD_FADES", 0) != 0;
        ppu_register_function(kDlgBaseUpdate, fade_plate_update);
        ppu_register_function(kBackPlateUpdate, back_plate_update);
    }
    /* The horizontal FOV at the default (vertical) one, in degrees. */
    const double half = std::tan(vm_readf(kCameraFov) * 0.5), deg = 360.0 / 3.14159265358979;
    fprintf(stderr, "[des] DES_ASPECT=%s: the picture is %.4f:1, %.0f degrees across at the default FOV (16:9: %.0f)",
            v, a, std::atan(half * a) * deg, std::atan(half * kGameAspect) * deg);
    if (shown && *shown) fprintf(stderr, ", shown at RSX_ASPECT=%s", shown);
    if (stretch) fprintf(stderr, "; HUD stretched across (DES_HUD=stretch)\n");
    else fprintf(stderr, "; HUD centred at 16:9, canvas x %.2f to %.2f%s\n", -s_hud_l, kCanvasW + s_hud_l,
                 s_widen_plates ? ", full-screen plates widened (DES_HUD_FADES=1)" : "");
}
