/* Use several soul items at once (docs/47 section 2, approach C).
 *
 * DES_SOULS_AT_ONCE=1 (Settings, Game; off by default): Use on a stack of soul
 * items (Unknown Soldier's Soul to Legendary Hero's Soul, goods 1025-1032) asks
 * how many, in the game's own count picker, the one Drop opens, with its text
 * changed. OK uses them: one crush animation, and the souls of all of them.
 * Boss souls and every other item are left alone.
 *
 * The game's pieces, all reached through vtables, so no hook in the lift:
 * - 0x0081E110, the command menu's choice (Use, Drop, ...). Use sends message
 *   2004 to the inventory and then closes the menus (slots 0x4F and 0x4E to
 *   -2). Drop sets slot 0x95 (0x85 in the other menu mode), sends 2001 and
 *   plays the confirm sound.
 * - 0x008219E0, the inventory's message handler. 2004 is Use: it puts the
 *   item's index at PGD+0x2CC and closes the menus. 2001 is Drop: for goods it
 *   opens the picker (0x007731F0, message 11000), whose OK comes back as 2000
 *   with the number at picker +0x830.
 * - 0x002DD368, the character's use of an item: the item's SpEffect, then
 *   0x008FDB00 takes one off the stack at PGD+0x2CC and sets it to -1. The
 *   souls come from the SpEffect (+0xE8), through AddSoul and the HUD's counter.
 *
 * Here: Use on such a stack takes Drop's steps instead, with Use's question.
 * The picker's OK is never 2000's Drop: it leaves the game's own Use for the
 * menus' next update (des_souls_menu_tick), sent as the command menu sends it.
 * (Inside the OK, the Use closed the menus and the picker's way back to the
 * inventory then brought it back half drawn.) When the character then takes
 * the one item, the rest leave the stack (0x008FD2A8, the game's count change)
 * and their souls go through the character's own soul gain (0x002D8978:
 * AddSoul, and the HUD's counter, which adds them to the one item's). Never
 * past the souls cap: the picker offers no more than fit, and items whose
 * souls would not fit stay in the stack. No picker while a Use still waits
 * for the character (PGD+0x2CC set): a second Use would take its place.
 *
 * DES_SOULS_AT_ONCE_LOG=1 prints the inventory's rows (goods id x count) when
 * they change, and each Use, Drop and OK. */

#include "ps3emu/ppu_lift.h"
#include "des_game_calls.h"
#include "ps3emu/ps3_knob.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#ifndef PS3_GAME_SPLIT   /* DES_SPLIT: the game DLL's (ps3emu/game_split.h) */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#endif
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void ds_malloc(ppu_context* ctx);   /* des_heap.cpp: the guest heap */

namespace {

constexpr uint32_t kCmdChoice  = 0x0081E110u;   /* the command menu's choice (command dialog) */
constexpr uint32_t kInvMessage = 0x008219E0u;   /* the inventory's messages (inventory dialog, message, argument) */
constexpr uint32_t kUseGoods   = 0x002DD368u;   /* the character uses an item (character, goods id, ...) */

/* The command dialog's row (0 Use, 1 Drop) and its Use-greyed byte; the menus' state (*(TOC - 0x7134)): -0x8000
 * holds the inventory dialog, -0x7FFC the menu mode. */
constexpr uint32_t kCmdRow = 0xF4u, kCmdUseOff = 0x10Au;

constexpr int32_t kMsgPicked = 2000;   /* the picker's OK */
constexpr int32_t kMsgDrop   = 2001;
constexpr int32_t kMsgUse    = 2004;

constexpr uint32_t kToc       = 0x019B5040u;
constexpr uint32_t kPgdRoot   = 0x01B4EF9Cu;   /* PGD = *(*this + 8) */
constexpr uint32_t kPgdSouls  = 0x78u;
constexpr uint32_t kPgdInv    = 0x128u;        /* the inventory, the argument of its functions */
constexpr uint32_t kPgdItems  = 0x1F4u;        /* +0x0C slots, +0x10 entries of 0x20 bytes */
constexpr uint32_t kPgdUsing  = 0x2CCu;        /* the index Use left for the character, -1 once taken */
constexpr int32_t  kSoulsCap  = 999999999;     /* AddSoul's */

constexpr uint32_t kGoods = 0x40000000u;       /* an entry's category */
constexpr uint32_t kSoulFirst = 1025u, kSoulLast = 1032u;

/* The dialog's selection: the row (+0x134) to the list's order (+0x13C) to the item's index (+0x138). */
constexpr uint32_t kDlgRow = 0x134u, kDlgItems = 0x138u, kDlgOrder = 0x13Cu;

/* The menus' shared object (*(*(*(TOC - 0x72F0) - 0x8000))): the picker's texts and number. (Its slots, which
 * 0x00720F50 sets, are at +0x14 + 4n.) */
constexpr uint32_t kPickText = 0x820u, kPickNumber = 0x830u;

bool guest_ok(uint32_t a) { return a >= 0x00010000u && a < 0xD0000000u && !(a & 3u); }

struct Entry { uint32_t cat = 0, id = 0; int32_t count = 0; };

uint32_t pgd()
{
    const uint32_t root = vm_read32(kPgdRoot);
    return guest_ok(root) && guest_ok(vm_read32(root + 8)) ? vm_read32(root + 8) : 0;
}

Entry entry(uint32_t p, int32_t idx)
{
    Entry e;
    const uint32_t list = p + kPgdItems, at = vm_read32(list + 0x10);
    if (idx < 0 || idx >= (int32_t)vm_read32(list + 0x0C) || !guest_ok(at)) return e;
    const uint32_t q = at + 0x20u * (uint32_t)idx;
    e.cat = vm_read32(q);
    e.id = vm_read32(q + 4);
    e.count = (int32_t)vm_read32(q + 8);
    return e;
}

bool soul_item(const Entry& e)
{
    return e.cat == kGoods && e.id >= kSoulFirst && e.id <= kSoulLast;
}

int32_t selected(uint32_t dlg)
{
    const int32_t row = (int32_t)vm_read32(dlg + kDlgRow);
    const uint32_t order = vm_read32(dlg + kDlgOrder), items = vm_read32(dlg + kDlgItems);
    if (row < 0 || !guest_ok(order) || !guest_ok(items)) return -1;
    const uint32_t ov = vm_read32(order + 4), iv = vm_read32(items + 4);
    if (!guest_ok(ov) || !guest_ok(iv)) return -1;
    const int32_t m = (int32_t)vm_read32(ov + 4u * (uint32_t)row);
    return m < 0 ? -1 : (int32_t)vm_read32(iv + 4u * (uint32_t)m);
}

uint32_t menu_state()
{
    const uint32_t t = vm_read32(kToc - 0x72F0u);
    const uint32_t holder = guest_ok(t) ? vm_read32(t - 0x8000u) : 0;
    return guest_ok(holder) ? vm_read32(holder) : 0;
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
 * are put back after, with the EBOOT's TOC and a stack below the caller's 288-byte red zone (des_dev.cpp's).
 * An address goes in as a uint32_t (zero-extended: the stack is above 0x80000000), a count as an int32_t. */
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

/* The SpEffect param's row of id, 0 if none. The game's param tables (0x005A5920(table, 0) + 0x20) hold each file
 * as loaded: its type name at +0x0C, its row count at +0x0A, rows {id, data offset, name offset} from +0x30. The
 * SpEffect table is found by its name; the getters' table numbers are not the param bundle's order. */
uint32_t speffect_row(ppu_context* ctx, int32_t id)
{
    static const char kType[] = "SP_EFFECT_PARAM_ST";
    static int s_table = -1;
    const int first = s_table < 0 ? 0 : s_table, last = s_table < 0 ? 0x1D : s_table;
    for (int t = first; t <= last; t++) {
        const uint32_t holder = call_game(ctx, func_005A5920, {t, 0});
        const uint32_t file = guest_ok(holder) ? vm_read32(holder + 0x20) : 0;
        if (!guest_ok(file)) continue;
        bool named = true;
        for (uint32_t i = 0; i < sizeof kType - 1 && named; i++) named = vm_read8(file + 0x0C + i) == (uint8_t)kType[i];
        if (!named) continue;
        s_table = t;
        const uint32_t n = vm_read16(file + 0x0A);
        for (uint32_t k = 0; k < n; k++)
            if ((int32_t)vm_read32(file + 0x30 + 12 * k) == id) return file + vm_read32(file + 0x30 + 12 * k + 4);
        return 0;
    }
    return 0;
}

/* The souls one of these items gives, as the game finds its effect (0x00278E90): the goods row's +0x14 is a
 * behavior, the behavior row's +0x14 its SpEffect, and the SpEffect's +0xE8 the souls (what 0x00376E68 adds up).
 * 0 if the params say none. */
int32_t soul_value(ppu_context* ctx, uint32_t goods)
{
    static std::mutex s_lk;
    static uint32_t s_out = 0;   /* the getters' {id, row} */
    std::lock_guard<std::mutex> lk(s_lk);
    if (!s_out && !(s_out = guest_alloc(ctx, 16))) return 0;
    const uint32_t out = s_out;
    vm_write32(out, 0xFFFFFFFFu); vm_write32(out + 4, 0);
    call_game(ctx, func_0059E3A8, {out, goods});
    const uint32_t row = vm_read32(out + 4);
    if (!guest_ok(row)) return 0;
    vm_write32(out, 0xFFFFFFFFu); vm_write32(out + 4, 0);
    call_game(ctx, func_00595B80, {out, (int32_t)vm_read32(row + 0x14)});
    const uint32_t behavior = vm_read32(out + 4);
    const uint32_t eff = guest_ok(behavior) ? speffect_row(ctx, (int32_t)vm_read32(behavior + 0x14)) : 0;
    return eff ? std::max<int32_t>((int32_t)vm_read32(eff + 0xE8), 0) : 0;
}

/* The picker's question, once, in the guest heap (the picker keeps the pointer). */
uint32_t use_text(ppu_context* ctx)
{
    static uint32_t s_text = 0;
    if (s_text) return s_text;
    static const char kText[] = "Please select the number of items to use.";
    const uint32_t t = guest_alloc(ctx, 2 * sizeof kText);
    if (!t) return 0;
    for (size_t i = 0; i < sizeof kText; i++) vm_write16(t + 2u * (uint32_t)i, (uint16_t)(uint8_t)kText[i]);
    return s_text = t;
}

/* The command menu's message argument for Use ({*(menus - 0x7F6C), 0, 2}, 0x0081E110's), once, in the guest heap. */
uint32_t use_arg(ppu_context* ctx)
{
    static uint32_t s_arg = 0;
    if (!s_arg && !(s_arg = guest_alloc(ctx, 16))) return 0;
    const uint32_t menus = vm_read32(kToc - 0x7134u);
    vm_write32(s_arg, guest_ok(menus) ? vm_read32(menus - 0x7F6Cu) : 0);
    vm_write32(s_arg + 4, 0);
    vm_write32(s_arg + 8, 2);
    return s_arg;
}

/* The inventory dialog the menus hold now (*(*(menus - 0x8000)), 0x0081E110's), 0 if none. */
uint32_t inventory()
{
    const uint32_t menus = vm_read32(kToc - 0x7134u);
    const uint32_t holder = guest_ok(menus) ? vm_read32(menus - 0x8000u) : 0;
    const uint32_t inv = guest_ok(holder) ? vm_read32(holder) : 0;
    return guest_ok(inv) ? inv : 0;
}

/* The picker Use opened (inventory dialog, item index, goods id), the Use its OK left for the menus' next update
 * (n items), and the use that began. Game threads. */
std::mutex s_mtx;
struct Pick { uint32_t dlg = 0; int32_t idx = -1; uint32_t id = 0; int32_t n = 0; } s_pick, s_later;
struct Use { int32_t idx = -1; uint32_t id = 0; int32_t n = 0; } s_use;
std::atomic<bool> s_later_set{false};

bool on()
{
    static int s_on = 0;
    PS3_KNOB(s_on, ps3_env_int("DES_SOULS_AT_ONCE", 0));
    return s_on != 0;
}

bool logging()
{
    static int s_log = 0;
    PS3_KNOB(s_log, ps3_env_int("DES_SOULS_AT_ONCE_LOG", 0));
    return s_log != 0;
}

void log_rows(uint32_t dlg, int32_t msg)
{
    const uint32_t p = pgd(), order = vm_read32(dlg + kDlgOrder), items = vm_read32(dlg + kDlgItems);
    if (!p || !guest_ok(order) || !guest_ok(items)) return;
    const uint32_t ov = vm_read32(order + 4), oe = vm_read32(order + 8), iv = vm_read32(items + 4);
    if (!guest_ok(ov) || !guest_ok(iv) || oe < ov) return;
    char line[2048];
    int k = snprintf(line, sizeof line, "[des] souls at once: rows");
    for (uint32_t r = 0; r < (oe - ov) / 4u && r < 64u && k < (int)sizeof line - 32; r++) {
        const int32_t m = (int32_t)vm_read32(ov + 4u * r);
        const Entry e = m < 0 ? Entry{} : entry(p, (int32_t)vm_read32(iv + 4u * (uint32_t)m));
        k += snprintf(line + k, sizeof line - k, " %u:%u%s%d", r, e.id, e.cat == kGoods ? "x" : "w", e.count);
    }
    if (msg >= kMsgPicked && msg <= kMsgUse)
        fprintf(stderr, "[des] souls at once: message %d, row %d, index %d\n", msg, (int32_t)vm_read32(dlg + kDlgRow),
                selected(dlg));
    static std::string s_last;
    if (s_last == line) return;
    s_last = line;
    fprintf(stderr, "%s\n", line);
}

/* Use on a stack of soul items: Drop's steps (0x0081E110's for Drop, then 0x008219E0's 2001 for goods), with Use's
 * question. False (the game's own Use) for any other item, one item, souls near the cap, or while a Use still waits
 * for the character (PGD+0x2CC set). */
bool offer(ppu_context* ctx, uint32_t dlg, uint32_t mode)
{
    const uint32_t p = pgd();
    const int32_t idx = selected(dlg);
    const Entry e = p ? entry(p, idx) : Entry{};
    if (!soul_item(e) || e.count < 2 || (int32_t)vm_read32(p + kPgdUsing) != -1) return false;
    const int32_t value = soul_value(ctx, e.id);
    if (value <= 0) {
        if (logging()) fprintf(stderr, "[des] souls at once: goods %u gives no souls in the params\n", e.id);
        return false;
    }
    const int32_t room = kSoulsCap - std::clamp((int32_t)vm_read32(p + kPgdSouls), 0, kSoulsCap);
    const int32_t most = std::min(e.count, room / value);
    const uint32_t m = menu_state(), text = use_text(ctx);
    if (logging())
        fprintf(stderr, "[des] souls at once: Use on %d of goods %u, %d souls each, at most %d\n", e.count, e.id, value,
                most);
    if (most < 2 || !guest_ok(m) || !text) return false;
    call_game(ctx, func_00720F50, {mode == 0 ? 0x95 : mode == 1 ? 0x85 : 0, 1});   /* (Drop's, by the menu mode) */
    call_game(ctx, func_00720F50, {0x89, 1});
    call_game(ctx, func_00720F50, {0x68, 1});
    call_game(ctx, func_00720F50, {0x89, 1});
    call_game(ctx, func_007731F0, {11000, 1, 2, most, 1, most, 0xEAC4});
    call_game(ctx, func_00720F50, {2, 9});
    call_game(ctx, func_00720F50, {0x68, 1});
    vm_write32(m + kPickText, text);
    call_game(ctx, func_0072D970, {100});   /* Drop's confirm sound */
    std::lock_guard<std::mutex> lk(s_mtx);
    s_pick = {dlg, idx, e.id, 0};
    return true;
}

void cmd_choice(ppu_context* ctx)   /* for 0x0081E110 */
{
    const uint32_t cmd = (uint32_t)ctx->gpr[3];
    if (on() && vm_read32(cmd + kCmdRow) == 0 && vm_read8(cmd + kCmdUseOff) == 0) {
        const uint32_t menus = vm_read32(kToc - 0x7134u);
        const uint32_t mode = guest_ok(menus) && guest_ok(vm_read32(menus - 0x7FFCu)) ? vm_read32(vm_read32(menus - 0x7FFCu)) : 2;
        const uint32_t inv = inventory();
        if (inv && offer(ctx, inv, mode)) return;
    }
    func_0081E110(ctx);
    drain(ctx);
}

void inv_message(ppu_context* ctx)   /* for 0x008219E0 */
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    const int32_t msg = (int32_t)ctx->gpr[4];
    if (logging()) log_rows(dlg, msg);
    Pick pk;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (msg == kMsgPicked || msg == kMsgDrop || msg == kMsgUse) {
            pk = s_pick;
            s_pick = {};
        }
    }
    if (msg != kMsgPicked || pk.dlg != dlg) {
        func_008219E0(ctx);
        drain(ctx);
        return;
    }
    /* The picker's OK on Use's, never 2000's Drop: 2000's flags (+0x140, +0x141), and the Use at the menus' next
     * update, once the picker's way back to the inventory is done. (Run here, inside it, the Use closed the menus
     * and then the way back brought the inventory back half drawn.) */
    const uint32_t m = menu_state();
    pk.n = guest_ok(m) ? (int32_t)vm_read32(m + kPickNumber) : 1;
    vm_write8(dlg + 0x140, 1);
    vm_write8(dlg + 0x141, 1);
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_later = pk;
    }
    s_later_set.store(true);
    if (logging()) fprintf(stderr, "[des] souls at once: OK, %d of goods %u (index %d)\n", pk.n, pk.id, pk.idx);
}

void use_goods(ppu_context* ctx)   /* for 0x002DD368 */
{
    Use u;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        u = s_use;
    }
    const uint32_t chr = (uint32_t)ctx->gpr[3], goods = (uint32_t)ctx->gpr[4];
    const uint32_t p = u.n ? pgd() : 0;
    const bool mine = p && vm_read32(chr + 0x49C) == p && goods == u.id && (int32_t)vm_read32(p + kPgdUsing) == u.idx;
    const int32_t before = mine ? (int32_t)vm_read32(p + kPgdSouls) : 0;
    func_002DD368(ctx);
    drain(ctx);
    if (!u.n || !p) return;
    const int32_t using_now = (int32_t)vm_read32(p + kPgdUsing);
    if (mine ? using_now != -1 : using_now == u.idx) return;   /* not taken yet */
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_use = {};
    }
    if (!mine) return;   /* Use's item went another way */
    const Entry e = entry(p, u.idx);
    const int32_t value = e.id == u.id && e.cat == kGoods ? soul_value(ctx, goods) : 0;
    if (value <= 0) return;
    /* The one item's souls may come with its SpEffect, later: leave room for them too. */
    const int32_t souls = std::clamp((int32_t)vm_read32(p + kPgdSouls), 0, kSoulsCap);
    const int32_t own = souls == before ? value : 0;
    const int32_t fit = std::max((kSoulsCap - souls - own) / value, 0);
    const int32_t more = std::min({u.n - 1, e.count, fit});
    if (more <= 0) return;
    call_game(ctx, func_008FD2A8, {p + kPgdInv, u.idx, -more, 0});
    call_game(ctx, func_002D8978, {chr, more * value, 0, 0});
    fprintf(stderr, "[des] souls at once: %d of goods %u, +%d souls (souls %d, before the use %d)\n", more + 1, goods,
            (more + 1) * value, (int32_t)vm_read32(p + kPgdSouls), before);
}

} // namespace

/* From ps3_title_options_init, before the guest runs. Always wrapped: the knob is read at each Use. */
extern "C" void des_souls_use_init(void)
{
    ppu_register_function(kCmdChoice, cmd_choice);
    ppu_register_function(kInvMessage, inv_message);
    ppu_register_function(kUseGoods, use_goods);
}

/* From des_menu_tick (des_menu_sfx.cpp), on the main thread just before the menus' update (0x00724AD0): the Use
 * the picker's OK left, as the command menu sends it (0x0081E110: 2004 to the inventory, then slots 0x4F and 0x4E
 * to -2, the close). The rest of the n leave once the character takes the one (use_goods). */
extern "C" void des_souls_menu_tick(ppu_context* ctx)
{
    if (!s_later_set.load(std::memory_order_relaxed) || !s_later_set.exchange(false)) return;
    Pick pk;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        pk = s_later;
        s_later = {};
    }
    const uint32_t p = pgd(), arg = use_arg(ctx);
    if (!p || !pk.dlg || pk.dlg != inventory() || !arg || selected(pk.dlg) != pk.idx) return;
    const Entry e = entry(p, pk.idx);
    if (e.id != pk.id || e.count < 1) return;
    const int32_t n = std::clamp(pk.n, 1, e.count);
    call_game(ctx, func_008219E0, {pk.dlg, kMsgUse, arg});
    call_game(ctx, func_00720F50, {0x4F, -2});
    call_game(ctx, func_00720F50, {0x4E, -2});
    const int32_t using_now = (int32_t)vm_read32(p + kPgdUsing);
    if (logging()) fprintf(stderr, "[des] souls at once: Use, %d of goods %u, Use left index %d\n", n, pk.id, using_now);
    if (n > 1 && using_now == pk.idx) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_use = {pk.idx, pk.id, n};
    }
}
