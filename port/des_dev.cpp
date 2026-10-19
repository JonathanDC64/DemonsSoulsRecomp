/* Demon's Souls: developer cheats (docs/55). Compiled only into developer
 * builds (cmake -DDES_DEV=ON). A release build links the empty des_dev_hook
 * in des_options.cpp instead, and its settings window has no Developer tab.
 *
 * DES_GOD=1 — the player's HP, MP and stamina never drop: hits, falls,
 *   poison-type ticks, script damage, spells, attacks, rolls, guarding and
 *   the dash cost nothing. Gains (healing, regen, level-ups) still count.
 *   Instant deaths (a fall out of the level, lethal grabs, scripted deaths)
 *   and SetHP (respawns, loads, Lua) are left alone, so no event waits on a
 *   death that cannot happen.
 * DES_ATK=<x> — the HP damage of the player's hits on others, times x (0 to
 *   1000; 0 = no damage).
 * DES_SPEED=<x> — how far the player moves each step, times x (0.25 to 10):
 *   walk, run, dash, roll and attack lunges. Animations keep their speed, so
 *   the feet slide.
 * DES_WARP=x,y,z — put the player at x, y, z the way the level scripts'
 *   LoadChrPos does. Each new value warps once. DES_WARP_CLEAR=<m> first
 *   sends the enemies within m metres of the target to the tutorial's start.
 *   Test scripts use it to reach a spot quickly.
 * DES_DEVLOG=1 — a line for each change a cheat makes, at most one a site
 *   every 250 ms (with the count of the ones in between).
 *
 * All live (the console's setenv and the Developer tab change them), all off
 * by default, none kept in settings.ini. Out-of-range values are clamped; a
 * value that does not parse counts as the default.
 *
 * The offline rule: cheats must never reach other players. The session is
 * decided once, at start (des_dev_boot, from ps3_title_options_init, before
 * the network modules read their knobs):
 *   - a cheat set at start: a cheat session. PS3_NET_ONLINE, PSNR_SERVER and
 *     PS3_NET_HOSTS are cleared, so the host sockets are never registered and
 *     cellNetCtl and NP report no connection;
 *   - the network on at start (PS3_NET_ONLINE) and no cheat: an online
 *     session, and the cheats stay off until the next start;
 *   - neither: an offline session. A cheat can be turned on at any time; the
 *     first one sets the "cheats used" latch (des_dev_cheats_used) for the
 *     rest of the session and clears the network knobs again.
 * DES_DEV_SESSION tells the UI which: cheats, online or offline. One gap is
 * left: in an offline session the debug console could set the network knobs
 * again after the latch; cellNetCtl and NP read them live, though the host
 * sockets stay unregistered. A lock in ps3recomp closes it (docs/55 section 6).
 *
 * The hooks: --insn-hooks makes the lifter call des_dev_hook(ctx, addr) right
 * after each instruction in the dev block of port/insn_hooks.txt. The same
 * code runs for every character, so each site first checks that it is the
 * player's. The sites and their registers are in docs/55 section 2.4; each
 * was checked against the instructions around it. */
#include "ppu_recomp.h"
#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include "des_dev_marker.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !DES_DEV
#error "des_dev.cpp is only for developer builds (cmake -DDES_DEV=ON)"
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {

constexpr uint32_t kChrMan      = 0x01B3D6E0u;   /* *(*kChrMan + 0x1BC) = the player, as player_pos in des_options.cpp */
constexpr uint32_t kChrMp       = 0x3CCu;        /* ints */
constexpr uint32_t kChrStamina  = 0x3D4u;
constexpr uint32_t kChrMove     = 0x20u;         /* the movement object */
constexpr uint32_t kRecHpDamage = 0x14Cu;        /* a damage record's final HP damage, an int */
constexpr float    kAtkMax      = 1000.0f;
constexpr float    kSpeedMin    = 0.25f, kSpeedMax = 10.0f;
constexpr double   kDamageCap   = 10000000.0;

bool guest_ptr_ok(uint32_t a) { return a >= 0x00010000u && a < 0x50000000u && !(a & 3u); }   /* as in des_options.cpp */

float vm_readf(uint32_t ea)
{
    const uint32_t bits = vm_read32(ea);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t player_chr()
{
    uint32_t p = vm_read32(kChrMan);
    if (!guest_ptr_ok(p) || !guest_ptr_ok(p = vm_read32(p + 0x1BC))) return 0;
    return p;
}

bool env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* A float knob: def when unset or not a number, else clamped to [lo, hi]. */
float knob_f(const char* name, float def, float lo, float hi)
{
    const char* v = getenv(name);
    if (!v || !*v) return def;
    char* end = nullptr;
    const float f = strtof(v, &end);
    if (end == v || !std::isfinite(f)) return def;
    return f < lo ? lo : f > hi ? hi : f;
}

int   god_knob()   { return env_on("DES_GOD") ? 1 : 0; }
float atk_knob()   { return knob_f("DES_ATK", 1.0f, 0.0f, kAtkMax); }
float speed_knob() { return knob_f("DES_SPEED", 1.0f, kSpeedMin, kSpeedMax); }

enum Session { kOffline, kCheats, kOnline };
Session s_session = kOffline;   /* set once by des_dev_boot, before any game thread runs */
std::atomic<bool> s_used{false};

const char* session_name(Session s) { return s == kCheats ? "cheats" : s == kOnline ? "online" : "offline"; }

void clear_network()
{
    ps3_setenv("PS3_NET_ONLINE", "");   /* an empty value reads as unset (cached_setenv) */
    ps3_setenv("PSNR_SERVER", "");
    ps3_setenv("PS3_NET_HOSTS", "");
}

/* The first cheat of the session: it stays offline from here on. */
void mark_used(const char* what)
{
    if (s_used.exchange(true)) return;
    clear_network();
    fprintf(stderr, "[dev] cheat in use (%s): this session stays offline\n", what);
}

/* An online session ignores the cheats, and says so once. */
bool refused(const char* what)
{
    if (s_session != kOnline) return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        fprintf(stderr, "[dev] %s ignored: this session is online (PS3_NET_ONLINE at start); start the game "
                "without it to use the cheats\n", what);
    return true;
}

/* cr7 as the lifter's cmpwi leaves it: LT 8, GT 4 or EQ 2 in the low nibble, no SO. */
void set_cr7(ppu_context* ctx, int32_t a, int32_t b)
{
    ctx->cr = (ctx->cr & ~0xFu) | (a < b ? 8u : a > b ? 4u : 2u);
}

uint32_t gpr32(const ppu_context* ctx, int r) { return (uint32_t)ctx->gpr[r]; }
int32_t  sgpr32(const ppu_context* ctx, int r) { return (int32_t)(uint32_t)ctx->gpr[r]; }

/* DES_DEVLOG: at most one line a site every 250 ms. */
void dev_log(int site, const char* what, double before, double after, uint32_t chr)
{
    static double last[16];
    static bool seen[16];
    static unsigned between[16];
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    if (seen[site] && ms - last[site] < 250.0) {
        between[site]++;
        return;
    }
    fprintf(stderr, "[dev] H%d %s: %g -> %g (chr %08X)", site, what, before, after, chr);
    if (between[site]) fprintf(stderr, ", %u more since the last line", between[site]);
    fputc('\n', stderr);
    seen[site] = true;
    last[site] = ms;
    between[site] = 0;
}

} // namespace

/* Every dev hook site (insn_hooks.txt) comes here; the switch picks the site. */
extern "C" void des_dev_hook(ppu_context* ctx, uint32_t addr)
{
    static int god = 0, devlog = 0;
    static float atk = 1.0f, speed = 1.0f;
    PS3_KNOB(god, god_knob());
    PS3_KNOB(atk, atk_knob());
    PS3_KNOB(speed, speed_knob());
    PS3_KNOB(devlog, env_on("DES_DEVLOG") ? 1 : 0);
    if (!god && atk == 1.0f && speed == 1.0f) return;
    if (refused(god ? "DES_GOD" : atk != 1.0f ? "DES_ATK" : "DES_SPEED")) return;
    if (!s_used.load(std::memory_order_relaxed)) mark_used(god ? "DES_GOD" : atk != 1.0f ? "DES_ATK" : "DES_SPEED");
    const uint32_t me = player_chr();
    if (!me) return;
    switch (addr) {
    case 0x00280E64u:   /* H1, ApplyDamage 0x00280DC8: r0 = the hit's HP damage (target r27); neg and cmpwi follow */
        if (god && gpr32(ctx, 27) == me && sgpr32(ctx, 0) > 0) {
            if (devlog) dev_log(1, "hit HP damage", sgpr32(ctx, 0), 0, me);
            ctx->gpr[0] = 0;
        }
        break;
    case 0x00280F88u:   /* H2, ApplyDamage: r9 = the hit's stamina damage (guarding); cmpwi and ble follow */
        if (god && gpr32(ctx, 27) == me && sgpr32(ctx, 9) > 0) {
            if (devlog) dev_log(2, "hit stamina damage", sgpr32(ctx, 9), 0, me);
            ctx->gpr[9] = 0;
        }
        break;
    case 0x0028B224u: {   /* H3, the damage handler 0x0028B1D8 after the calculation: attacker r29, record r28 */
        if (atk == 1.0f || gpr32(ctx, 29) != me) break;
        const uint32_t rec = gpr32(ctx, 28);
        if (!guest_ptr_ok(rec)) break;
        const int32_t d = (int32_t)vm_read32(rec + kRecHpDamage);
        if (d <= 0) break;   /* the calculation rewrites it on every hit, so this never compounds */
        const double x = std::nearbyint((double)d * atk);
        const int32_t nd = (int32_t)(x > kDamageCap ? kDamageCap : x);
        vm_write32(rec + kRecHpDamage, (uint32_t)nd);
        if (devlog) dev_log(3, "hit damage dealt", d, nd, gpr32(ctx, 31));
        break;
    }
    case 0x002818D0u:   /* H4, 0x002816D0: r0 = a damage worked out from a fraction (falls, inferred), chr r31 */
        if (god && gpr32(ctx, 31) == me && sgpr32(ctx, 0) > 0) {
            if (devlog) dev_log(4, "fraction damage", sgpr32(ctx, 0), 0, me);
            ctx->gpr[0] = 0;
        }
        break;
    case 0x00280724u:   /* H5, 0x002806D8 after cmpwi cr7,r3,0: r3 = this update's SpEffect HP change, chr r31 */
        if (god && gpr32(ctx, 31) == me && sgpr32(ctx, 3) < 0) {
            if (devlog) dev_log(5, "SpEffect HP change", sgpr32(ctx, 3), 0, me);
            ctx->gpr[3] = 0;
            set_cr7(ctx, 0, 0);   /* EQ: the beq skips the change */
        }
        break;
    case 0x00280C78u:   /* H6, 0x00280A08: r9 = this update's SpEffect MP change, chr r31; cmpwi follows */
        if (god && gpr32(ctx, 31) == me && sgpr32(ctx, 9) < 0) {
            if (devlog) dev_log(6, "SpEffect MP change", sgpr32(ctx, 9), 0, me);
            ctx->gpr[9] = 0;
        }
        break;
    case 0x0026FB20u:   /* H7, AddHP(chr r3, delta r4), after cmpwi cr7,r4,0 */
        if (god && gpr32(ctx, 3) == me && sgpr32(ctx, 4) < 0) {
            if (devlog) dev_log(7, "AddHP", sgpr32(ctx, 4), 0, me);
            ctx->gpr[4] = 0;
            set_cr7(ctx, 0, 0);
        }
        break;
    case 0x0026FC48u: {   /* H8, SetMP(chr r3, value r4), after cmpwi cr7,r4,0 */
        if (!god || gpr32(ctx, 3) != me) break;
        const int32_t mp = (int32_t)vm_read32(me + kChrMp), v = sgpr32(ctx, 4);
        if (v >= mp) break;   /* a gain, or no change */
        if (devlog) dev_log(8, "SetMP", v, mp, me);
        ctx->gpr[4] = (uint32_t)mp;
        set_cr7(ctx, mp, 0);
        break;
    }
    case 0x0026FC98u: {   /* H9, SetStamina(chr r3, value r4), after cmpwi cr7,r4,-50 */
        if (!god || gpr32(ctx, 3) != me) break;
        const int32_t st = (int32_t)vm_read32(me + kChrStamina), v = sgpr32(ctx, 4);
        if (v >= st) break;
        if (devlog) dev_log(9, "SetStamina", v, st, me);
        ctx->gpr[4] = (uint32_t)st;
        set_cr7(ctx, st, -50);
        break;
    }
    case 0x00286614u: {   /* H10, the dash cost in 0x00286020: r9 = stamina - 1 (chr r31); the clamp follows */
        if (!god || gpr32(ctx, 31) != me) break;
        const uint32_t st = vm_read32(me + kChrStamina);
        if (devlog) dev_log(10, "dash cost", sgpr32(ctx, 9), (int32_t)st, me);
        ctx->gpr[9] = st;
        break;
    }
    case 0x002EDEB0u:   /* H11, the root-motion step 0x002EDDA0, fcmpu cr7,f0,f13: f0 = the speed factor (mv r31), f13 = 1 */
        if (speed == 1.0f || vm_read32(me + kChrMove) != gpr32(ctx, 31)) break;
        if (devlog) dev_log(11, "step factor", ctx->fpr[0], ctx->fpr[0] * speed, me);
        ctx->fpr[0] = (float)(ctx->fpr[0] * speed);
        ctx->cr = (ctx->cr & ~0xFu) | 8u;   /* LT: the branch that scales the step (mv+0xF0) by f0 */
        break;
    case 0x0027882Cu:   /* H12, 0x002784C0: r0 = a weapon action's stamina cost (chr r28); subf follows */
        if (god && gpr32(ctx, 28) == me && sgpr32(ctx, 0) > 0) {
            if (devlog) dev_log(12, "weapon action cost", sgpr32(ctx, 0), 0, me);
            ctx->gpr[0] = 0;
        }
        break;
    default:
        break;
    }
}

/* Called once from ps3_title_options_init, after settings.ini and before the
 * network modules start: prints the marker and decides the session. */
extern "C" void des_dev_boot(void)
{
    fprintf(stderr, "[dev] %s\n", DES_DEV_MARKER);
    const char* warp = getenv("DES_WARP");
    const char* net = getenv("PS3_NET_ONLINE");
    const bool cheat = god_knob() || atk_knob() != 1.0f || speed_knob() != 1.0f || (warp && *warp);
    if (cheat) {
        s_session = kCheats;
        mark_used("set at start");
    } else if (net && *net) {
        s_session = kOnline;
    }
    ps3_setenv("DES_DEV_SESSION", session_name(s_session));
    fprintf(stderr, "[dev] session: %s%s\n", session_name(s_session),
            s_session == kCheats   ? " (a cheat was set at start: the network knobs are cleared)"
            : s_session == kOnline ? " (the network is on: the cheats stay off until the next start)"
                                   : " (the first cheat turned on keeps it offline)");
}

/* True from the first cheat to the end of the session: future network code
 * must stay offline while it is (docs/46, docs/55 section 6). */
extern "C" int des_dev_cheats_used(void)
{
    return s_used.load() ? 1 : 0;
}

/* DES_WARP (see the top). Lua's LoadChrPos is 0x004428F0 -> 0x00273BE0, r3 =
 * the character, vr2 = x y z 0. Enemies (vtable 0x0185AF98) are in the array
 * at *(ChrMan + 0xC). Main thread: des_menu_tick calls des_debug_tick once a
 * frame. */
static void warp_chr(ppu_context* ctx, uint32_t chr, const float* p)
{
    uint64_t g[32]; double f[32]; uint8_t v[32][16];
    memcpy(g, ctx->gpr, sizeof g); memcpy(f, ctx->fpr, sizeof f); memcpy(v, ctx->vr, sizeof v);
    const uint64_t lr = ctx->lr, ctr = ctx->ctr;
    const uint32_t cr = ctx->cr, xer = ctx->xer;
    uint8_t be[16] = {};
    for (int i = 0; i < 3; i++) {
        uint32_t b; memcpy(&b, &p[i], 4);
        be[4 * i] = (uint8_t)(b >> 24); be[4 * i + 1] = (uint8_t)(b >> 16);
        be[4 * i + 2] = (uint8_t)(b >> 8); be[4 * i + 3] = (uint8_t)b;
    }
    memcpy(&ctx->vr[2], be, 16);
    ctx->gpr[3] = chr;
    ctx->lr = 0x00724E40u;
    func_00273BE0(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
    memcpy(ctx->gpr, g, sizeof g); memcpy(ctx->fpr, f, sizeof f); memcpy(ctx->vr, v, sizeof v);
    ctx->lr = lr; ctx->ctr = ctr; ctx->cr = cr; ctx->xer = xer;
}

static bool chr_pos(uint32_t chr, float* p)
{
    uint32_t q = vm_read32(chr + 0x20);
    if (!guest_ptr_ok(q) || !guest_ptr_ok(q = vm_read32(q + 0x10))) return false;
    for (int i = 0; i < 3; i++) p[i] = vm_readf(q + 0x10 + 4 * i);
    return true;
}

extern "C" void des_debug_tick(ppu_context* ctx)
{
    static int gen = 0;
    static char last[96] = "";
    const int before = gen;
    PS3_KNOB(gen, gen + 1);
    if (gen == before) return;
    const char* e = getenv("DES_WARP");
    if (!e || !strcmp(e, last)) return;
    snprintf(last, sizeof last, "%s", e);
    float p[3];
    if (sscanf(e, "%f,%f,%f", &p[0], &p[1], &p[2]) != 3) return;
    if (refused("DES_WARP")) return;
    const uint32_t man = vm_read32(kChrMan);
    uint32_t chr;
    if (!guest_ptr_ok(man) || !guest_ptr_ok(chr = vm_read32(man + 0x1BC))) return;
    mark_used("DES_WARP");
    const char* c = getenv("DES_WARP_CLEAR");
    const float r = c ? (float)atof(c) : 0.0f;
    const uint32_t list = vm_read32(man + 0xC);
    if (r > 0.0f && guest_ptr_ok(list)) {
        int moved = 0;
        for (uint32_t i = 0; i < 64; i++) {
            const uint32_t q = vm_read32(list + 4 * i);
            float qp[3];
            if (!guest_ptr_ok(q) || q == chr || vm_read32(q) != 0x0185AF98u || !chr_pos(q, qp)) continue;
            const float d = std::sqrt((qp[0] - p[0]) * (qp[0] - p[0]) + (qp[1] - p[1]) * (qp[1] - p[1]) +
                                      (qp[2] - p[2]) * (qp[2] - p[2]));
            if (d > r) continue;
            const float away[3] = {201.0f + 1.5f * moved, -1.1f, -96.2f};   /* the tutorial's start */
            warp_chr(ctx, q, away);
            fprintf(stderr, "[des] DES_WARP_CLEAR: enemy %08X (%.1f m) sent away\n", q, d);
            moved++;
        }
    }
    warp_chr(ctx, chr, p);
    fprintf(stderr, "[des] DES_WARP %.3f %.3f %.3f (chr %08X)\n", p[0], p[1], p[2], chr);
}
