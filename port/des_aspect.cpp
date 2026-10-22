/* Wider aspect ratios: a 21:9 or 32:9 picture with the HUD kept 16:9 (docs/39).
 *
 * DES_ASPECT=<w:h> | <W>x<H> | <number> | auto (default unset: 16:9, the
 *   game's own) - the shape of the picture. Applied at start, and again when
 *   it changes (the settings window, the console's setenv; "Live" below).
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
 *
 * Live: a change of DES_ASPECT or DES_HUD while the game runs (des_aspect_poll,
 *   at each flip request) gives the same picture as starting with it.
 *   - Cameras. The two default words are written again, for the cameras built
 *     from then on. The cameras built before keep their copy, so the lifter
 *     calls des_cam_ctor after each constructor's store of the aspect
 *     (0x0012F074, 0x0012F0DC, r29 = the camera), and the cameras are listed.
 *     0x000D5980 then gives its camera its own target's shape (0x000D5B24,
 *     r19); des_cam_own takes that one off the list, as starting with the
 *     option never changes it either. A change writes the new aspect into
 *     every listed camera that still holds the old one exactly, with a fov,
 *     near and far around it: a camera freed and its memory reused fails that.
 *     The cameras the game copies each frame (0x002190E0) and the copies in the
 *     frame data follow their listed source by the next frame (seen with the
 *     console: poking only the listed cameras moved every copy).
 *   - The HUD canvas. The record is not replayed: 0x00CD4800, the 0x3A handler,
 *     turns it into the render context's ortho matrix at +0x90 once, and the
 *     list words are reused. des_hud_ctx, after 0x00CD4854 (r5 = the first
 *     word of the command, r3 = the context), notes the context that handles
 *     the HUD's record, and a change writes the matrix's x scale (+0x90) and x
 *     offset (+0xC0) for the new canvas, as the handler would compute them.
 *     Poking that matrix while playing moved the HUD on the next frame.
 *   - RSX_ASPECT follows, unless it was given at start. The window keeps its
 *     size; the picture is fitted into it.
 *   Not followed: a render context's first projection (0x0190433C) in the
 *   contexts built before the change. The game draws through the cameras'
 *   projections, and a 21:9 switch while playing looked the same as a 21:9
 *   start.
 */

#include "ps3emu/ppu_lift.h"   /* ppu_context: the fixed part of the lift's header */
#include "des_game_calls.h"     /* the lifted functions below (the game DLL's with DES_SPLIT) */
#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

constexpr uint32_t kCamFov = 0x50u, kCamAspect = 0x54u, kCamNear = 0x58u, kCamFar = 0x5Cu;   /* a camera's projection */
constexpr uint32_t kCtxOrtho = 0x90u;   /* a render context's ortho matrix, rows of four floats (0x00CD4800) */
constexpr uint32_t kOrthoM11 = 0xBB360B61u;   /* -2 / 720: the canvas's y scale, the same at any width */
constexpr size_t   kMaxCams  = 1024;

float s_hud_l;        /* the canvas's extra width on each side; 0 = the game's canvas */
float s_hud_k = 1.0f; /* (1280 + 2L) / 1280 */
bool  s_widen_plates; /* DES_HUD_FADES=1 */

float s_aspect = kGameAspect;   /* what the default words and the listed cameras hold */
bool  s_off;                    /* the default words were not 16:9 at start: never written */
bool  s_rsx_pinned;             /* RSX_ASPECT given at start: left alone */
std::string s_cur_aspect, s_cur_hud;   /* DES_ASPECT and DES_HUD as last applied */

std::mutex s_cam_mu;            /* s_cams, and s_aspect while it changes */
std::vector<uint32_t> s_cams;   /* cameras built from the defaults (des_cam_ctor) */

uint32_t s_hud_rec;   /* the HUD canvas record's first word (des_hud_canvas); 0 before boot records it */
uint32_t s_hud_ctx;   /* the render context that turned it into its matrix (des_hud_ctx); 0 until then */

float vm_readf(uint32_t ea)
{
    const uint32_t bits = vm_read32(ea);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t float_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

void vm_writef(uint32_t ea, float f)
{
    vm_write32(ea, float_bits(f));
}

/* A listed camera that still holds the aspect it was given, exactly, with a
 * fov, near and far around it. Memory reused for anything else fails this. */
bool camera_holds(uint32_t cam, uint32_t aspect_bits)
{
    if (vm_read32(cam + kCamAspect) != aspect_bits) return false;
    const float fov = vm_readf(cam + kCamFov), n = vm_readf(cam + kCamNear), f = vm_readf(cam + kCamFar);
    return fov > 0.01f && fov < 3.1f && n > 0.0f && f > n;
}

/* The list is full: drop the cameras that no longer hold the current aspect,
 * then the older half if that was not enough. Under s_cam_mu. */
void cams_prune()
{
    const uint32_t bits = float_bits(s_aspect);
    std::vector<uint32_t> keep;
    for (uint32_t c : s_cams)
        if (camera_holds(c, bits)) keep.push_back(c);
    if (keep.size() > kMaxCams / 2) keep.erase(keep.begin(), keep.begin() + (long)(keep.size() - kMaxCams / 2));
    s_cams.swap(keep);
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
    if (s_hud_l <= 0.0f) return;   /* (wrapped from the start, for a live change to a centred HUD) */
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
    const uint32_t right = (uint32_t)ctx->gpr[11];
    if (vm_readf(right) != kCanvasW || vm_readf(right - 4) != 0.0f || vm_readf(right - 8) != kCanvasH ||
        vm_readf(right - 12) != 0.0f)
        return;
    s_hud_rec = right - 12;   /* for des_hud_ctx, whatever the canvas is now */
    s_hud_ctx = 0;
    if (s_hud_l <= 0.0f) return;
    vm_writef(right - 4, -s_hud_l);
    vm_writef(right, kCanvasW + s_hud_l);
    static bool said;
    if (!said) {
        said = true;
        fprintf(stderr, "[des] HUD canvas: x %.2f to %.2f, y 0 to 720 (0x%08X, list word 0x%08X)\n", -s_hud_l,
                kCanvasW + s_hud_l, addr, right);
    }
}

/* After 0x00CD4854 in the 0x3A handler: r5 = the command's first word, r3 =
 * the render context. The one that handles the HUD's record keeps its matrix
 * at +0x90 from then on. */
extern "C" void des_hud_ctx(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    if (!s_hud_rec || s_hud_ctx || (uint32_t)ctx->gpr[5] != s_hud_rec) return;
    s_hud_ctx = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[des] HUD canvas: matrix at 0x%08X (render context 0x%08X)\n", s_hud_ctx + kCtxOrtho, s_hud_ctx);
}

/* After a camera constructor's store of the default aspect (0x0012F074,
 * 0x0012F0DC, stfs f12,0x54(r29)). Any thread that builds one. */
extern "C" void des_cam_ctor(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    const uint32_t cam = (uint32_t)ctx->gpr[29];
    std::lock_guard<std::mutex> lk(s_cam_mu);
    for (uint32_t c : s_cams)
        if (c == cam) return;
    if (s_cams.size() >= kMaxCams) cams_prune();
    s_cams.push_back(cam);
}

/* After 0x000D5B24, stfs f0,0x54(r19): this camera takes its target's shape. */
extern "C" void des_cam_own(ppu_context* ctx, uint32_t addr)
{
    (void)addr;
    const uint32_t cam = (uint32_t)ctx->gpr[19];
    std::lock_guard<std::mutex> lk(s_cam_mu);
    for (size_t i = 0; i < s_cams.size(); i++)
        if (s_cams[i] == cam) { s_cams.erase(s_cams.begin() + (long)i); return; }
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

namespace {

/* DES_ASPECT -> the picture's shape; kGameAspect when it is unset, 16:9 or not
 * supported (said in the log). */
float aspect_wanted(const char* v)
{
    if (!v || !*v) return kGameAspect;
    const bool from_display = strcmp(v, "auto") == 0;
    float a = from_display ? display_aspect() : parse_aspect(v);
    if (!(a > 0.0f)) {
        fprintf(stderr, "[des] DES_ASPECT=%s: %s; the picture stays 16:9\n", v,
                from_display ? "the display's shape is unknown" : "not a shape (w:h, WxH or a number)");
        return kGameAspect;
    }
    if (a < kGameAspect * 0.999f) {
        fprintf(stderr, "[des] DES_ASPECT=%s (%.4f:1) is narrower than 16:9, which is not supported yet: "
                "the picture stays 16:9, with bars\n", v, a);
        return kGameAspect;
    }
    if (a <= kGameAspect * 1.001f) {
        if (from_display) fprintf(stderr, "[des] DES_ASPECT=auto: the display is 16:9, the game's own shape\n");
        return kGameAspect;
    }
    if (a > kMaxAspect) {
        fprintf(stderr, "[des] DES_ASPECT=%s (%.4f:1): wider than 4:1 is not supported; the picture is 4:1\n", v, a);
        a = kMaxAspect;
    }
    return a;
}

/* The HUD context's x scale and x offset for a canvas l wider on each side,
 * as 0x00CD4800 computes them from [-l, 1280 + l]. */
void hud_matrix(float l, float& m00, float& m30)
{
    const float left = -l, right = kCanvasW + l, k = 1.0f / (right - left);
    m00 = k + k;
    m30 = k * -(left + right);
}

bool near_value(float got, float want)
{
    return std::fabs(got - want) <= std::fabs(want) * 1e-5f;
}

/* A live change of the HUD canvas, into the HUD context's matrix while it
 * still holds the one for the old canvas. */
bool hud_rewrite(float was_l, float l)
{
    if (!s_hud_ctx) return false;
    const uint32_t m = s_hud_ctx + kCtxOrtho;
    float was00, was30, m00, m30;
    hud_matrix(was_l, was00, was30);
    hud_matrix(l, m00, m30);
    if (vm_read32(m + 0x14) != kOrthoM11 || !near_value(vm_readf(m), was00) || !near_value(vm_readf(m + 0x30), was30))
        return false;
    vm_writef(m, m00);
    vm_writef(m + 0x30, m30);
    return true;
}

/* DES_ASPECT and DES_HUD -> the default words, the listed cameras, RSX_ASPECT
 * and the HUD. At start (live false) before the guest runs; live from the
 * flip request, on the game's render thread. */
void aspect_apply(const char* v, const char* hud, bool live)
{
    s_cur_aspect = v ? v : "";
    s_cur_hud = hud ? hud : "";
    const float a = aspect_wanted(v);
    if (s_off) {
        if (a != kGameAspect)
            fprintf(stderr, "[des] DES_ASPECT: the default aspect words hold %g and %g, not 16:9 (another release "
                    "of the game?): left alone, the picture stays 16:9\n", vm_readf(kCameraAspect),
                    vm_readf(kContextAspect));
        return;
    }
    int changed = 0;
    size_t listed = 0;
    {
        std::lock_guard<std::mutex> lk(s_cam_mu);
        const uint32_t was = float_bits(s_aspect), now = float_bits(a);
        listed = s_cams.size();
        if (was != now) {
            vm_writef(kCameraAspect, a);
            vm_writef(kContextAspect, a);
            for (uint32_t c : s_cams)
                if (camera_holds(c, was)) {
                    vm_write32(c + kCamAspect, now);
                    changed++;
                }
            s_aspect = a;
        }
    }
    const char* shown = getenv("RSX_ASPECT");
    if (!s_rsx_pinned) {
        char buf[32] = "";
        if (a > kGameAspect) snprintf(buf, sizeof buf, "%.6f", a);
        if (strcmp(shown ? shown : "", buf) != 0) ps3_setenv("RSX_ASPECT", buf);   /* ("" unsets it) */
    }
    const bool stretch = hud && strcmp(hud, "stretch") == 0;
    const float was_l = s_hud_l, l = !stretch && a > kGameAspect ? (kCanvasH * a - kCanvasW) * 0.5f : 0.0f;
    s_hud_l = l;
    s_hud_k = (kCanvasW + 2.0f * l) / kCanvasW;
    s_widen_plates = l > 0.0f && ps3_env_int("DES_HUD_FADES", 0) != 0;
    const bool hud_moved = live && l != was_l && hud_rewrite(was_l, l);

    if (!live && a == kGameAspect) return;   /* (the game's own picture: nothing to say at start) */
    const char* name = v && *v ? v : "(unset)";
    if (a == kGameAspect) {
        fprintf(stderr, "[des] DES_ASPECT=%s, live: the picture is 16:9", name);
    } else {
        /* The horizontal FOV at the default (vertical) one, in degrees. */
        const double half = std::tan(vm_readf(kCameraFov) * 0.5), deg = 360.0 / 3.14159265358979;
        fprintf(stderr, "[des] DES_ASPECT=%s%s: the picture is %.4f:1, %.0f degrees across at the default FOV (16:9: %.0f)",
                name, live ? ", live" : "", a, std::atan(half * a) * deg, std::atan(half * kGameAspect) * deg);
    }
    if (s_rsx_pinned) fprintf(stderr, ", shown at RSX_ASPECT=%s", shown);
    if (live) fprintf(stderr, ", %d of %zu listed cameras changed", changed, listed);
    if (a == kGameAspect) fprintf(stderr, "; HUD at the game's canvas");
    else if (stretch) fprintf(stderr, "; HUD stretched across (DES_HUD=stretch)");
    else fprintf(stderr, "; HUD centred at 16:9, canvas x %.2f to %.2f%s", -s_hud_l, kCanvasW + s_hud_l,
                 s_widen_plates ? ", full-screen plates widened (DES_HUD_FADES=1)" : "");
    if (live && l != was_l) fputs(hud_moved ? " (moved)" : " (not moved: no HUD matrix found)", stderr);
    fprintf(stderr, "\n");
}

} // namespace

/* From ps3_title_options_init, after settings.ini is read and before the guest
 * runs, so before the first camera is built. */
extern "C" void des_aspect_init(void)
{
    const char* shown = getenv("RSX_ASPECT");
    s_rsx_pinned = shown && *shown;
    const float was_camera = vm_readf(kCameraAspect), was_context = vm_readf(kContextAspect);
    s_off = std::fabs(was_camera - kGameAspect) > 1e-5f || std::fabs(was_context - kGameAspect) > 1e-5f;
    /* Wrapped whatever the HUD is at start: a live change can centre it. */
    ppu_register_function(kDlgBaseUpdate, fade_plate_update);
    ppu_register_function(kBackPlateUpdate, back_plate_update);
    aspect_apply(getenv("DES_ASPECT"), getenv("DES_HUD"), false);
}

/* From des_options.cpp's flip request hook, each frame: a changed DES_ASPECT
 * or DES_HUD (the settings window, the console's setenv) is applied live. */
extern "C" void des_aspect_poll(void)
{
    static unsigned gen;
    const unsigned now = ps3_env_gen;
    if (gen == now) return;
    gen = now;
    const char* v = getenv("DES_ASPECT");
    const char* hud = getenv("DES_HUD");
    if (s_cur_aspect == (v ? v : "") && s_cur_hud == (hud ? hud : "")) return;
    aspect_apply(v, hud, true);
}
