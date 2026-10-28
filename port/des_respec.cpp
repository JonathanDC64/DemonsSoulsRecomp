/* Stat respec at the Nexus statue (docs/47 section 1).
 *
 * DES_RESPEC=1 (Settings, Game; off by default): Cross in front of the statue
 * that a save which quit in the Nexus starts facing opens the shop's command
 * menu (the one with Upgrade Weapon ... Leave) with the rows in kRows:
 * Respecialization and Leave. No prompt shows: the HUD draws the action
 * prompt only for a target the game's action search found (docs/47 1.9).
 * Respecialization asks first, then puts the stats back to the starting
 * class's, gives back the souls the levels cost, and opens the game's level-up
 * screen to spend them again. The screen only raises stats, so nothing goes
 * below the class's values.
 *
 * The game's pieces, all called from here (no hook in the lift):
 * - The talk scripts' commands (0x004CC3A8, ids as the scripts number them):
 *   20 ClearTalkListData (0x00723450), 19 AddTalkListData (0x00723498, an
 *   entry {text id, row}), 10 the menu (0x00721490 the talk partner, then
 *   0x007212A8: slot 9 to 1, the menus open page 17400), 17 the yes/no dialog
 *   (0x007216D0, slot 0x11 the kind, 0x007216F0 its texts), 31 the level-up
 *   screen (0x007DC7A0: slot 0x17 to 1). The menu's answer: slot 9 goes to 4
 *   and menus+0xABC holds the row (0 for Circle); the dialog's: slot 0x11 back
 *   to 0 and menus+0x82C 1 for YES.
 * - 0x00226778(x), the cost of the level the screen sells to stat sum x.
 * - 0x0090FCD0(r3, pgd), the game's reset (a penalty path's): the class's
 *   stats (CharaInitParam 1000 + PGD+0xB6), base HP, MP and stamina, PGD+0x88
 *   (levels bought) to 0, every spell unattuned. Souls and items stay.
 * - 0x007E4248(), what the level-up screen runs after a raise: the live
 *   character's HP, MP and stamina from the new stats.
 *
 * The texts the game does not have go in its own tables: a row's into an
 * unused id of the talk menu's FMG (category 0x1E), the menu's title in place
 * of the missing-name text ("?NpcName?", while the menu is open). A respec
 * whose souls would pass the cap is refused, and nothing changes. */

#include "ps3emu/ppu_lift.h"
#include "des_game_calls.h"
#include "ps3emu/ps3_knob.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#ifndef PS3_GAME_SPLIT   /* DES_SPLIT: the game DLL's (ps3emu/game_split.h) */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#endif
extern "C" void ds_malloc(ppu_context* ctx);              /* des_heap.cpp: the guest heap */
extern "C" volatile unsigned g_pad0_buttons;              /* cellPad.c: the buttons the game last read */

namespace {

constexpr uint32_t kToc       = 0x019B5040u;
constexpr uint32_t kPgdRoot   = 0x01B4EF9Cu;   /* PGD = *(*this + 8) */
constexpr uint32_t kPgdStats  = 0x38u;         /* eight stats, 8 bytes apart (two copies each) */
constexpr uint32_t kPgdSouls  = 0x78u;
constexpr uint32_t kPgdLevels = 0x88u;         /* levels bought */
constexpr uint32_t kPgdClass  = 0xB6u;         /* halfword */
constexpr int64_t  kSoulsCap  = 999999999;     /* the constant at 0x016FF700 */
constexpr uint32_t kChrMan    = 0x01B3D6E0u;   /* *this + 0x1BC: the player's character */

/* PGD's current and max HP, MP and stamina; the character's current values (its max 4 bytes on). */
constexpr uint32_t kPgdBars[3][2] = {{0x0Cu, 0x10u}, {0x18u, 0x1Cu}, {0x28u, 0x2Cu}};
constexpr uint32_t kChrBars[3]    = {0x3C4u, 0x3CCu, 0x3D4u};

/* The menus' object (*(*(*(TOC - 0x72F0) - 0x8000))): its slots at +0x14 + 4n (0x00720F50 sets them). */
constexpr uint32_t kSlotTalk = 9, kSlotDialog = 0x11, kSlotLevelUp = 0x17;
constexpr uint32_t kMenuRow = 0xABCu, kDialogAnswer = 0x82Cu;
constexpr int32_t  kTalkAnswered = 4, kDialogYesNo = 2, kDialogOk = 1;

/* The texts in the talk menu's FMG (category 0x1E). */
constexpr int32_t kFmgTalk = 0x1E;
constexpr int32_t kTextYes = 10020000, kTextNo = 10020001, kTextLeave = 15000005;

/* The statue, in the Nexus (map m01): a save that quit there starts at (0, 479.46, -41.69), facing it (-z); the
 * player stops against it at z -41.8. Cross from in front of it, within kReach of (kStatueX, kStatueZ). */
constexpr int   kNexus   = 1;
constexpr float kStatueX = 0.0f, kStatueY = 479.5f, kStatueZ = -43.0f, kReach = 2.2f;

bool guest_ok(uint32_t a) { return a >= 0x00010000u && a < 0xD0000000u && !(a & 3u); }

uint32_t pgd()
{
    const uint32_t root = vm_read32(kPgdRoot);
    return guest_ok(root) && guest_ok(vm_read32(root + 8)) ? vm_read32(root + 8) : 0;
}

uint32_t player_chr()
{
    const uint32_t man = vm_read32(kChrMan);
    const uint32_t c = guest_ok(man) ? vm_read32(man + 0x1BC) : 0;
    return guest_ok(c) ? c : 0;
}

uint32_t menus()
{
    const uint32_t t = vm_read32(kToc - 0x72F0u);
    const uint32_t h = guest_ok(t) ? vm_read32(t - 0x8000u) : 0;
    const uint32_t m = guest_ok(h) ? vm_read32(h) : 0;
    return guest_ok(m) ? m : 0;
}

int32_t slot(uint32_t m, uint32_t n) { return (int32_t)vm_read32(m + 0x14 + 4 * n); }

float readf(uint32_t a)
{
    const uint32_t w = vm_read32(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
}

/* Calls a game function with up to eight arguments (r3-r10) and returns r3, on the thread's own registers, which
 * are put back after (des_souls_use.cpp's). */
uint32_t call_game(ppu_context* ctx, void (*fn)(ppu_context*), std::initializer_list<int64_t> a)
{
    uint64_t g[32]; double f[32]; uint8_t v[32][16];
    memcpy(g, ctx->gpr, sizeof g); memcpy(f, ctx->fpr, sizeof f); memcpy(v, ctx->vr, sizeof v);
    const uint64_t lr = ctx->lr, ctr = ctx->ctr;
    const uint32_t cr = ppu_cr_pack(ctx), xer = ctx->xer;
    ctx->gpr[1] -= 0x1000; ctx->gpr[2] = kToc;
    int i = 0;
    for (int64_t x : a) if (i < 8) ctx->gpr[3 + i++] = (uint64_t)x;
    ctx->lr = 0x00724E40u;
    fn(ctx);
    drain(ctx);
    const uint32_t r3 = (uint32_t)ctx->gpr[3];
    memcpy(ctx->gpr, g, sizeof g); memcpy(ctx->fpr, f, sizeof f); memcpy(ctx->vr, v, sizeof v);
    ctx->lr = lr; ctx->ctr = ctr; ppu_cr_unpack(ctx, cr); ctx->xer = xer;
    return r3;
}

/* size bytes of the guest heap, for good (des_heap.cpp's ds_malloc); 0 if none. */
uint32_t guest_alloc(ppu_context* ctx, uint32_t size)
{
    const uint64_t r3 = ctx->gpr[3];
    ctx->gpr[3] = size;
    ds_malloc(ctx);
    const uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = r3;
    return a;
}

/* text as the game's UTF-16 (big-endian) in the guest heap, once per call site's static; 0 if none. */
uint32_t guest_text(ppu_context* ctx, const char* text)
{
    const uint32_t n = (uint32_t)strlen(text) + 1;
    const uint32_t t = guest_alloc(ctx, 2 * n);
    for (uint32_t i = 0; t && i < n; i++) vm_write16(t + 2 * i, (uint16_t)(uint8_t)text[i]);
    return t;
}

bool on()
{
    static int s_on = 0;
    PS3_KNOB(s_on, ps3_env_int("DES_RESPEC", 0));
    return s_on != 0;
}

bool logging()
{
    static int s_log = 0;
    PS3_KNOB(s_log, ps3_env_int("DES_RESPEC_LOG", 0));
    return s_log != 0;
}

std::atomic<int> s_map{-1};   /* the map whose script archive opened last (des_respec_on_open) */

int32_t stat_sum(uint32_t p)
{
    int32_t s = 0;
    for (uint32_t i = 0; i < 8; i++) s += (int32_t)vm_read32(p + kPgdStats + 8 * i);
    return s;
}

/* The class's CharaInitParam row (0x0090FCD0's: 0x00597108 maps the class to a row, 0x00597388 finds it), whose
 * first eight words are its stats in PGD order; 0 if none. */
uint32_t class_row(ppu_context* ctx, uint32_t p)
{
    static uint32_t s_out = 0;   /* the getter's {id, row} */
    if (!s_out && !(s_out = guest_alloc(ctx, 16))) return 0;
    const int32_t row = (int32_t)call_game(ctx, func_00597108, {(int16_t)vm_read16(p + kPgdClass)});
    vm_write32(s_out, 0xFFFFFFFFu); vm_write32(s_out + 4, 0);
    call_game(ctx, func_00597388, {s_out, row});
    const uint32_t r = vm_read32(s_out + 4);
    return guest_ok(r) ? r : 0;
}

/* The souls the levels from stat sum lo up to hi cost, as the screen sells them: the level to sum x costs cost(x). */
int64_t refund(ppu_context* ctx, int32_t lo, int32_t hi)
{
    int64_t s = 0;
    for (int32_t x = lo + 1; x <= hi; x++) s += call_game(ctx, func_00226778, {x});
    return s;
}

/* The game's text id in the talk menu's FMG for an own text: an id of the group that holds Leave that has no text
 * (offset 0), its offset set to the text (offsets are from the FMG's start, modulo 2^32, as 0x005C2848 adds them).
 * k picks the k-th such id, so each own text keeps its own. -1 if none. The lookup (0x005C2848): groups of
 * {offset index, first id, last id} from +0x1C (count at +0x0C), the offsets at *(+0x14). */
int32_t own_text_id(ppu_context* ctx, int k, uint32_t text)
{
    const uint32_t fmg = call_game(ctx, func_005C2810, {kFmgTalk});
    if (!guest_ok(fmg) || !text) return -1;
    const uint32_t groups = vm_read32(fmg + 0x0C), offs = vm_read32(fmg + 0x14);
    if (!guest_ok(offs)) return -1;
    for (uint32_t g = 0; g < groups && g < 0x10000u; g++) {
        const uint32_t gi = fmg + 0x1C + 12 * g;
        const uint32_t oi = vm_read32(gi), first = vm_read32(gi + 4), last = vm_read32(gi + 8);
        if ((uint32_t)kTextLeave < first || (uint32_t)kTextLeave > last) continue;
        for (uint32_t id = last; id > (uint32_t)kTextLeave; id--) {
            const uint32_t at = offs + 4 * (oi + id - first), off = vm_read32(at);
            if (off == text - fmg) { if (k-- == 0) return (int32_t)id; continue; }
            if (off) continue;
            if (k-- == 0) {
                vm_write32(at, text - fmg);
                return (int32_t)id;
            }
        }
    }
    return -1;
}

/* ---- The menu ---- */

void ask_respec(ppu_context* ctx);

/* The statue's rows, in order (the talk list's row number is the position + 1). own: a text of ours (own_text_id),
 * else game: the game's text id. choose: what the row does, nullptr to close (Leave). */
struct Row { const char* own; int32_t game; void (*choose)(ppu_context*); };
constexpr Row kRows[] = {
    {"Respecialization", 0, ask_respec},
    {nullptr, kTextLeave, nullptr},
};
constexpr int kRowCount = (int)(sizeof kRows / sizeof kRows[0]);

const char kTitle[] = "Statue";
const char kAsk[] = "Return to your class's starting stats?\nThe souls of every level come back,\n"
                    "and every spell must be remembered again.";
const char kCapped[] = "Not now: with the souls back,\nyou would hold more than 999,999,999.";

enum class Phase { idle, menu, confirm, notice };
Phase s_phase = Phase::idle;
bool s_here = false;              /* the player was at the statue last frame (for the log) */
unsigned s_cross_prev = 0x4000u;  /* Cross as last seen, held at the start: a press must begin near the statue */
int s_chosen = 0;                /* the row the menu answered, run once the menu has closed */

/* The menu's title with no talk partner: the game's missing-name text ("?NpcName?", 0x016EBB98, ten UTF-16 units
 * with its end, which five pointers in the game's tables give the name lookups), ours while the menu is open. */
constexpr uint32_t kNoName = 0x016EBB98u, kNoNameUnits = 10;
uint16_t s_no_name[kNoNameUnits];
bool s_title = false;

void title_on()
{
    static_assert(sizeof kTitle <= kNoNameUnits, "the title must fit the missing-name text");
    if (s_title || vm_read16(kNoName) != (uint16_t)'?') return;
    for (uint32_t i = 0; i < kNoNameUnits; i++) s_no_name[i] = vm_read16(kNoName + 2 * i);
    for (uint32_t i = 0; i < sizeof kTitle; i++) vm_write16(kNoName + 2 * i, (uint16_t)(uint8_t)kTitle[i]);
    s_title = true;
}

void title_off()
{
    if (!s_title) return;
    for (uint32_t i = 0; i < kNoNameUnits; i++) vm_write16(kNoName + 2 * i, s_no_name[i]);
    s_title = false;
}

/* The player stands in front of the statue, in the Nexus, facing it, alive, with no menu or dialog open (talk
 * command 10's test, 0x00720F80, masked as it masks it). */
bool at_statue(ppu_context* ctx, uint32_t m)
{
    if (s_map.load(std::memory_order_relaxed) != kNexus) return false;
    const uint32_t chr = player_chr();
    if (!chr || (int32_t)vm_read32(chr + 0x3C4) <= 0) return false;
    uint32_t xf = vm_read32(chr + 0x20);
    if (!guest_ok(xf) || !guest_ok(xf = vm_read32(xf + 0x10))) return false;
    const float x = readf(xf + 0x10), y = readf(xf + 0x14), z = readf(xf + 0x18);
    const float dx = kStatueX - x, dz = kStatueZ - z;
    if (std::fabs(y - kStatueY) > 1.5f || dx * dx + dz * dz > kReach * kReach || z < kStatueZ) return false;
    /* The facing: the yaw at +4, the bearing + pi (des_options.cpp's roll turn), so it faces (-sin, -cos). */
    const float yaw = readf(xf + 4), fx = -std::sin(yaw), fz = -std::cos(yaw);
    if (fx * dx + fz * dz < 0.5f * std::sqrt(dx * dx + dz * dz)) return false;   /* within 60 degrees */
    if (slot(m, kSlotTalk) || slot(m, kSlotDialog) || slot(m, kSlotLevelUp)) return false;
    return (call_game(ctx, func_00720F80, {}) & 0xDEFFFF7Fu) == 0;
}

void open_menu(ppu_context* ctx)
{
    static uint32_t s_entry = 0, s_texts[kRowCount];
    if (!s_entry && !(s_entry = guest_alloc(ctx, 32))) return;
    call_game(ctx, func_00723450, {});
    for (int i = 0, own = 0; i < kRowCount; i++) {
        int32_t id = kRows[i].game;
        if (kRows[i].own) {
            if (!s_texts[i]) s_texts[i] = guest_text(ctx, kRows[i].own);
            id = own_text_id(ctx, own++, s_texts[i]);
        }
        if (id < 0) continue;
        vm_write32(s_entry, (uint32_t)id);
        vm_write32(s_entry + 4, (uint32_t)(i + 1));
        call_game(ctx, func_00723498, {s_entry});
    }
    title_on();
    call_game(ctx, func_00721490, {-1});             /* no talk partner: the title is the missing name's */
    vm_write8(s_entry + 8, 0);
    vm_write32(s_entry + 12, 0xFFFFFFFFu);
    vm_write32(s_entry + 16, 0);
    call_game(ctx, func_007212A8, {s_entry + 8});    /* talk command 10's */
    s_phase = Phase::menu;
}

void dialog(ppu_context* ctx, int32_t kind, const char* text, Phase next)
{
    static uint32_t s_ask = 0, s_capped = 0, s_ok = 0;
    uint32_t& t = text == kAsk ? s_ask : s_capped;
    if (!t) t = guest_text(ctx, text);
    const uint32_t fmg = call_game(ctx, func_005C2810, {kFmgTalk});
    if (!t || !guest_ok(fmg)) return;
    uint32_t yes = call_game(ctx, func_005C2848, {fmg, kTextYes});
    const uint32_t no = call_game(ctx, func_005C2848, {fmg, kTextNo});
    if (kind == kDialogOk && (s_ok || (s_ok = guest_text(ctx, "OK")))) yes = s_ok;   /* the talk texts have no OK */
    call_game(ctx, func_007216D0, {-1, -1});
    call_game(ctx, func_00720F50, {(int64_t)kSlotDialog, kind});
    call_game(ctx, func_00720F50, {0x54, 0});
    call_game(ctx, func_007216F0, {t, yes, no});    /* talk command 17's */
    call_game(ctx, func_00720F50, {0x4E, -1});
    call_game(ctx, func_00720F50, {0x4F, -1});
    s_phase = next;
    if (logging()) fprintf(stderr, "[des] respec: dialog %d\n", kind);
}

void ask_respec(ppu_context* ctx) { dialog(ctx, kDialogYesNo, kAsk, Phase::confirm); }

/* ---- The respec ---- */

/* false (and nothing changed) if the souls would pass the cap or the game's state is not there. */
bool respec(ppu_context* ctx, bool* capped)
{
    *capped = false;
    const uint32_t p = pgd();
    const uint32_t row = p ? class_row(ctx, p) : 0;
    const uint32_t chr = player_chr();
    if (!row || !chr || vm_read32(chr + 0x49C) != p) return false;
    int32_t floor_sum = 0;
    for (uint32_t i = 0; i < 8; i++) floor_sum += (int32_t)vm_read32(row + 4 * i);
    const int32_t sum = stat_sum(p), souls = (int32_t)vm_read32(p + kPgdSouls), levels = (int32_t)vm_read32(p + kPgdLevels);
    const int64_t back = sum > floor_sum ? refund(ctx, floor_sum, sum) : 0;
    if (souls < 0 || souls + back > kSoulsCap) {
        fprintf(stderr, "[des] respec: refused, %d souls and %lld back would pass the cap\n", souls, (long long)back);
        *capped = true;
        return false;
    }
    if (sum <= floor_sum) return true;   /* nothing to give back: the screen alone, the spells kept */
    double share[3];
    for (int b = 0; b < 3; b++) {
        const int32_t cur = (int32_t)vm_read32(chr + kChrBars[b]), max = (int32_t)vm_read32(chr + kChrBars[b] + 4);
        share[b] = max > 0 ? std::clamp((double)cur / max, 0.0, 1.0) : 1.0;
    }
    call_game(ctx, func_0090FCD0, {0, p});
    vm_write32(p + kPgdSouls, (uint32_t)(souls + back));
    /* 0x007E4248, made for raises, moves each bar by the change from PGD's old max to the character's new one, after
     * the character's own update (0x002802E0, 0x002737A0, 0x00273620) has already cut its current value to a lower
     * max: after a respec the cut would count twice, and from the reset's bare base values a soul form's HP could
     * reach 0. So the character's update runs first, and PGD's old values are set so that each bar ends at its
     * old share of the new max (at least 1). */
    call_game(ctx, func_002802E0, {chr});
    call_game(ctx, func_002737A0, {chr});
    call_game(ctx, func_00273620, {chr});
    for (int b = 0; b < 3; b++) {
        const int32_t cur = (int32_t)vm_read32(chr + kChrBars[b]), max = (int32_t)vm_read32(chr + kChrBars[b] + 4);
        const int32_t want = std::clamp((int32_t)(share[b] * max + 0.5), std::min(1, max), max);
        vm_write32(p + kPgdBars[b][0], (uint32_t)cur);
        vm_write32(p + kPgdBars[b][1], (uint32_t)(max - want + cur));
    }
    call_game(ctx, func_007E4248, {});
    fprintf(stderr, "[des] respec: stat sum %d to %d (class %d), %d levels, souls %d + %lld = %d\n", sum, stat_sum(p),
            floor_sum, levels, souls, (long long)back, (int32_t)vm_read32(p + kPgdSouls));
    return true;
}

} // namespace

/* From des_options.cpp's file-open hook (the file thread): a level's script archive, /script/mNN.luabnd..., names
 * the map that is loading. */
extern "C" void des_respec_on_open(const char* path)
{
    const char* s = strstr(path, "/script/m");
    if (!s || !strstr(s, ".luabnd") || s[9] < '0' || s[9] > '9' || s[10] < '0' || s[10] > '9') return;
    s_map.store((s[9] - '0') * 10 + (s[10] - '0'), std::memory_order_relaxed);
}

/* From des_menu_tick (des_menu_sfx.cpp), on the main thread just before the menus' update (0x00724AD0). */
extern "C" void des_respec_tick(ppu_context* ctx)
{
    const uint32_t m = menus();
    if (!m) return;
    const unsigned cross = g_pad0_buttons & 0x4000u, pressed = cross & ~s_cross_prev;
    s_cross_prev = cross;
    switch (s_phase) {
    case Phase::idle: {
        const bool here = on() && at_statue(ctx, m);
        if (here != s_here && logging()) fprintf(stderr, "[des] respec: %s the statue\n", here ? "at" : "away from");
        s_here = here;
        if (here && pressed) {
            open_menu(ctx);
            if (logging()) fprintf(stderr, "[des] respec: menu\n");
        }
        break;
    }
    case Phase::menu: {   /* the answer (slot 9 at 4, the row at +0xABC), then the menu closes (slot 9 to 0) */
        const int32_t talk = slot(m, kSlotTalk);
        if (talk == kTalkAnswered) s_chosen = 1;
        if (talk || (call_game(ctx, func_00720F80, {}) & 0xDEFFFF7Fu)) break;
        const int row = s_chosen ? (int32_t)vm_read32(m + kMenuRow) : 0;
        call_game(ctx, func_00723450, {});   /* as the scripts do once the menu answers */
        title_off();
        s_phase = Phase::idle;
        s_chosen = 0;
        if (logging()) fprintf(stderr, "[des] respec: menu row %d\n", row);
        if (row >= 1 && row <= kRowCount && kRows[row - 1].choose) kRows[row - 1].choose(ctx);
        break;
    }
    case Phase::confirm: {
        if (slot(m, kSlotDialog)) break;
        s_phase = Phase::idle;
        bool capped = false;
        const int32_t answer = (int32_t)vm_read32(m + kDialogAnswer);
        if (logging()) fprintf(stderr, "[des] respec: answer %d\n", answer);
        if (answer != 1) break;
        if (respec(ctx, &capped)) call_game(ctx, func_007DC7A0, {});   /* talk command 31: the level-up screen */
        else if (capped) dialog(ctx, kDialogOk, kCapped, Phase::notice);
        break;
    }
    case Phase::notice:
        if (!slot(m, kSlotDialog)) s_phase = Phase::idle;
        break;
    }
}
