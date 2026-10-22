/* The wrapper UI's sounds, played with the game's own menu sounds (docs/33).
 *
 * The game plays every menu sound through 0x0072D970(id): "play menu SE id",
 * cue "f%09d" from the resident bank sound/ds_se_main.mosb. The UI asks for a
 * sound on the render thread (g_des_ui_sound, des_ui.h); the request goes into
 * a small queue that the game's main thread empties in des_menu_tick, which
 * is lifted in place of the menu system's per-frame update 0x00724E40
 * (--override-funcs, docs/27). So the sound starts on the thread whose frame
 * runs the menus, exactly as if a menu had called 0x72D970 itself.
 *
 * Only the ids in the game's id-to-slot table may be passed: for any other id
 * 0x0072D538 writes through a wild pointer (docs/33, "The eight IDs").
 * DES_UI_SOUND=0 keeps the UI silent. */

#include "ps3emu/ppu_lift.h"   /* ppu_context: the fixed part of the lift's header */
#include "des_game_calls.h"     /* the lifted functions below (the game DLL's with DES_SPLIT) */
#include "ui/include/des_ui.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef PS3_GAME_SPLIT   /* DES_SPLIT: the game DLL's (ps3emu/game_split.h) */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#endif

namespace {

constexpr uint32_t kMenuSeMgr = 0x01B4B150u;   /* *this = the menu-sound manager, 0 until boot init */

/* The game's menu sound ids (cue f000000<id>). */
constexpr int32_t kSeCursor  = 0;     /* cursor move, page switch */
constexpr int32_t kSeConfirm = 100;
constexpr int32_t kSeCancel  = 200;
constexpr int32_t kSeWindow  = 300;   /* a sub-window opens */
constexpr int32_t kSeValue   = 400;   /* a slider or value changes */

bool allowed(int32_t id)
{
    return id == kSeCursor || id == kSeConfirm || id == kSeCancel || id == kSeWindow ||
           id == kSeValue || id == 999999999;
}

/* The game's id-to-slot table (0x0072CEF8): each id has its own slot of 16
 * bytes at manager + 8, the cue handle first. */
uint32_t slot_of(int32_t id)
{
    switch (id) {
    case 0: return 0;
    case 100: return 1;
    case 200: return 2;
    case 300: return 3;
    case 999999999: return 4;
    default: return 7;   /* 400 */
    }
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* One producer (the render thread), one consumer (the game's main thread). A
 * sound older than kMaxAgeMs when the main thread gets to it (a loading
 * stall) is dropped rather than played late. */
struct Req {
    std::atomic<int32_t> id;
    std::atomic<int64_t> at_ms;
};
constexpr uint32_t kQ = 16;
constexpr int64_t kMaxAgeMs = 300;
Req s_q[kQ];
std::atomic<uint32_t> s_head{0}, s_tail{0};

void queue_menu_sound(int32_t id)
{
    if (!allowed(id)) return;
    const uint32_t h = s_head.load(std::memory_order_relaxed);
    if (h - s_tail.load(std::memory_order_acquire) >= kQ) return;   /* full: drop */
    s_q[h % kQ].id.store(id, std::memory_order_relaxed);
    s_q[h % kQ].at_ms.store(now_ms(), std::memory_order_relaxed);
    s_head.store(h + 1, std::memory_order_release);
}

/* The UI's sounds (DES_UI_SOUND_*) to the game's. */
void ui_sound(int ui)
{
    static const int32_t map[] = {
        kSeConfirm,   /* CLICK */
        kSeConfirm,   /* PLAY */
        kSeConfirm,   /* BINDING_CHANGED */
        kSeWindow,    /* MENU_OPEN */
        kSeCancel,    /* MENU_CLOSE */
        kSeWindow,    /* WINDOW_OPEN */
        kSeCancel,    /* WINDOW_CLOSE */
        kSeCursor,    /* TAB_CHANGED: the pause menu's L1/R1 pages play the cursor sound */
        kSeCursor,    /* ITEM_FOCUS */
        kSeValue,     /* ITEM_CHANGE */
        kSeValue,     /* ITEM_ENABLE */
        kSeValue,     /* ITEM_DISABLE */
        kSeWindow,    /* WARNING: a question opens */
    };
    if (ui >= 0 && ui < (int)(sizeof map / sizeof map[0])) queue_menu_sound(map[ui]);
}

/* DES_MENU_SFX_TEST=1: play the six menu cues in turn, one every 2 s, to hear
 * which is which (docs/33, open question 3). */
bool s_test;
int64_t s_test_next;
int s_test_i;

} // namespace

/* Lifted in place of 0x00724E40 (--override-funcs). Called with bl from the
 * frame function 0x0001B8D0 on the main thread, once a frame, f1 = frame
 * time, right after the game's sound update. Its own body is only
 * `r3 = *0x01B4B114; tail-call 0x00724AD0`, redone at the end. */
#if DES_DEV
extern "C" void des_debug_tick(ppu_context* ctx);   /* des_dev.cpp: the warp (developer builds) */
#endif

extern "C" void des_menu_tick(ppu_context* ctx)
{
#if DES_DEV
    des_debug_tick(ctx);
#endif
    if (s_test) {
        const int64_t now = now_ms();
        if (now >= s_test_next) {
            static const int32_t ids[] = {0, 100, 200, 300, 400, 999999999};
            const int32_t id = ids[s_test_i++ % 6];
            fprintf(stderr, "[des] menu sound test: f%09d\n", (int)id);
            queue_menu_sound(id);
            s_test_next = now + 2000;
        }
    }
    uint32_t t = s_tail.load(std::memory_order_relaxed);
    if (t != s_head.load(std::memory_order_acquire)) {
        /* 0x72D538 is a normal ABI function: r1, r2, r13-r31, f14-f31 and
         * CR2-4 survive it. Keep the volatile ones too, f1 = dt above all,
         * which the menu update below still needs. */
        uint64_t g[13];
        double f[14];
        memcpy(g, ctx->gpr, sizeof g);
        memcpy(f, ctx->fpr, sizeof f);
        const uint64_t lr = ctx->lr, ctr = ctx->ctr;
        const uint32_t cr = ppu_cr_pack(ctx), xer = ctx->xer;
        const int64_t now = now_ms();
        while (t != s_head.load(std::memory_order_acquire)) {
            const int32_t id = s_q[t % kQ].id.load(std::memory_order_relaxed);
            const bool fresh = now - s_q[t % kQ].at_ms.load(std::memory_order_relaxed) <= kMaxAgeMs;
            s_tail.store(++t, std::memory_order_release);
            if (!fresh || !allowed(id) || vm_read32(kMenuSeMgr) == 0) continue;
            const uint32_t slot_ea = vm_read32(kMenuSeMgr) + 8 + 16 * slot_of(id);
            uint32_t before[4] = {};
            if (s_test)
                for (int i = 0; i < 4; i++) before[i] = vm_read32(slot_ea + 4 * i);
            ctx->gpr[3] = (uint32_t)id;
            ctx->lr = 0x00724E40u;
            func_0072D970(ctx);   /* tail-calls 0x72D538(mgr, id, 2) */
            while (g_trampoline_fn) {
                void (*tf)(void*) = g_trampoline_fn;
                g_trampoline_fn = nullptr;
                tf(ctx);
            }
            if (s_test)   /* the slot keeps the new cue's handle */
                fprintf(stderr, "[des] menu sound f%09d: slot %08x %08x %08x %08x -> %08x %08x %08x %08x\n",
                        (int)id, before[0], before[1], before[2], before[3], vm_read32(slot_ea),
                        vm_read32(slot_ea + 4), vm_read32(slot_ea + 8), vm_read32(slot_ea + 12));
        }
        memcpy(ctx->gpr, g, sizeof g);
        memcpy(ctx->fpr, f, sizeof f);
        ctx->lr = lr;
        ctx->ctr = ctr;
        ppu_cr_unpack(ctx, cr);
        ctx->xer = xer;
    }
    /* The original 0x00724E40: r9 = the menu system's global (0x01B4B114,
     * through the TOC), r3 = *r9, tail-call 0x00724AD0 with f1 untouched. */
    ctx->gpr[9] = vm_read32(vm_read32((uint32_t)ctx->gpr[2] - 0x72F0u) - 0x8000u);
    ctx->gpr[3] = vm_read32((uint32_t)ctx->gpr[9]);
    g_trampoline_fn = (void (*)(void*))func_00724AD0;
}

/* Called from ps3_title_options_init, after des_ui_early_init. */
extern "C" void des_menu_sfx_init(void)
{
    const char* v = getenv("DES_UI_SOUND");
    if (!v || strcmp(v, "0") != 0) g_des_ui_sound = ui_sound;
    v = getenv("DES_MENU_SFX_TEST");
    s_test = v && strcmp(v, "0") != 0;
}
