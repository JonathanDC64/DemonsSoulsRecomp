/* The HUD's size, its corners on a wide picture, and parts of it hidden (docs/102).
 *
 * DES_HUD_SIZE=<percent> (default 100; 25 to 200, the settings window offers
 *   50 to 150) - the HUD's size. Each corner group grows or shrinks about its
 *   own corner of the screen, the boss's health bar about the bottom middle.
 * DES_HUD="" (default) | centre | stretch - des_aspect.cpp makes the canvas
 *   for a wide picture. With "" (Corners) the four corner groups also move out
 *   to the picture's corners; with centre they stay in the 16:9 band in the
 *   middle, as before; stretch spreads the whole canvas.
 * DES_HUD_BARS, DES_HUD_AREA, DES_HUD_EQUIP, DES_HUD_SOULS = 0 - hide a group:
 *   health, magic and stamina (top left, with the eye emblem and the status
 *   icons), the area name (top right, with its emblem), the equipped items and
 *   spells (bottom left), the souls (bottom right). 1 (default) shows it.
 * All are read again after any setenv (the settings window, the console), so
 * they change while playing.
 *
 * How (read in the EBOOT and in heap dumps of a run, docs/102):
 * - The in-game HUD is one EzMenu dialog, DsMenuDlgPCGauge (vtable
 *   0x018698A0), the size of the 1280x720 canvas. Its children (array at +0x0C,
 *   count at +0x10) are layout objects, 86 of them. Each object's record (+0x1C)
 *   starts with a pointer to its name in the layout, UTF-16 ("plate_hp",
 *   "map_name", "soul_param", "F20-00_item"...). An object that hosts a dialog
 *   draws it (the four equipment slots are DsMenuDlgFeShortcut dialogs, the
 *   souls a DsMenuDlgFEGetSoul).
 * - Every dialog is drawn through 0x00F60BD0 (vtable +0x4C), which tail-calls
 *   the draw, 0x00F5EAC0 (0x00F5F878 when +0x9C is set). The draw maps the
 *   dialog's layout rect (+0x84: left, top, right, bottom) onto its current rect
 *   (+0x74), scale and offset, and draws each child through that: moving the
 *   current rect moved and scaled the whole HUD on the next frame. Text moved
 *   with it but kept its size (below). An object is drawn only while bit
 *   0x40000000 of its +0x18 is set (its draw, 0x00F62A88).
 * - So 0x00F60BD0 is wrapped (ppu_register_function: the vtable call goes
 *   through it). For PCGauge, each group whose transform differs gets a draw
 *   of its own: the current rect is set to the group's transform, and only
 *   that group's objects keep their bit. Then the bits and the rect are put
 *   back, so the game never sees them changed. Objects the game itself hides
 *   stay hidden. The dialog's own shape draws nothing (0x018840E8, 0x00F9CCD0),
 *   and nothing else in the draw lasts past it (it restores the renderer's
 *   matrix and colour), so drawing it more than once draws each object once.
 * - Not in a group, drawn as before: the lock-on marker and the enemies'
 *   health bars (they follow the world), the status plate in the middle, and
 *   any object with a name not listed here.
 * - The loading screen (DsMenuDlgNowLoading, vtable 0x0186FBC0) is drawn over
 *   the HUD, and covers only the 16:9 band on a wide picture. While it is
 *   drawn, the groups moved out to the corners are not drawn. Its own soul_0
 *   to soul_99, parked right of the canvas, are not drawn on a widened one.
 * - "HP icon", "SP icon" and "MP icon" sit at x = -168, left of the 16:9
 *   canvas, where the PS3 never shows them. A widened canvas showed them in
 *   the left band (docs/39, section 8): while it is widened they are not drawn.
 * - Text kept its size because its glyphs are sized by its font, not by the
 *   dialog's rect: the text shape's "set scale" (font +0x28) does nothing for
 *   the menu font. Each text begins with a font number (font +0x08,
 *   0x0071B728); the drawer queues it as a command, and its playback
 *   (0x000B15C8) sets the glyph scale (text state +0x38, +0x3C) and the line
 *   spacing (+0x94) from that font's entry in a table of 13. The HUD uses
 *   fonts 5 to 8. So while a group is drawn at another size, the number is
 *   queued with the size in its top bits, and the playback's wrapper gives the
 *   game the plain number and then multiplies the scale and spacing by the
 *   size. The text shape then sets the spacing again (font +0x38, 0x0071B5A0,
 *   0 for one line; played back by 0x000B14C8): a scaled one is passed exactly
 *   as 1/64ths with the top bit set, and that playback's wrapper writes it.
 */

#include "ps3emu/ppu_lift.h"   /* ppu_context, vm_read32 */
#include "des_game_calls.h"     /* func_00F60BD0... (the game DLL's with DES_SPLIT) */
#include "ps3emu/ps3_knob.h"   /* ps3_env_gen */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" float des_hud_side_width(void);   /* des_aspect.cpp: the canvas's extra width on each side */
#ifndef PS3_GAME_SPLIT   /* DES_SPLIT: the game DLL's (ps3emu/game_split.h) */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#endif

namespace {

constexpr uint32_t kDialogDraw  = 0x00F60BD0u;   /* every EzMenu dialog's draw (vtable +0x4C) */
constexpr uint32_t kHudDialogVt = 0x018698A0u;   /* DsMenuDlgPCGauge */
constexpr uint32_t kLoadingVt   = 0x0186FBC0u;   /* DsMenuDlgNowLoading, the loading screen */
constexpr uint32_t kDlgChildren = 0x0Cu, kDlgChildCount = 0x10u;
constexpr uint32_t kDlgRect     = 0x74u;         /* current rect: left, top, right, bottom */
constexpr uint32_t kDlgVisible  = 0x94u;         /* byte; 0 = the dialog draws nothing */
constexpr uint32_t kObjFlags    = 0x18u, kObjRecord = 0x1Cu;
constexpr uint32_t kObjDrawn    = 0x40000000u;   /* in kObjFlags: the object is drawn */
constexpr float    kCanvasW     = 1280.0f, kCanvasH = 720.0f;
constexpr uint32_t kMaxChildren = 256;
constexpr uint32_t kFontBegin   = 0x0071B728u;   /* the menu font's "begin" with a font number (vtable 0x01865E10 +0x08) */
constexpr uint32_t kFontPlay    = 0x000B15C8u;   /* plays the number back into the text state */
constexpr uint32_t kFontSpacing = 0x0071B5A0u;   /* the menu font's line spacing (vtable +0x38) */
constexpr uint32_t kSpacingPlay = 0x000B14C8u;   /* plays the spacing back into the text state */
constexpr uint32_t kGlyphScale  = 0x38u;         /* in the text state: the glyph scale, x then y (floats) */
constexpr uint32_t kLineSpacing = 0x94u;         /* in the text state: the line spacing (float) */
constexpr uint32_t kFontScaled  = 0x80000000u;   /* in the font number: a scale in 1/1024ths at bit 16 */
constexpr uint32_t kSpacingExact = 0x8000u;      /* in the spacing: 1/64ths of a pixel follow */
constexpr float    kSpacingFrac = 64.0f;
constexpr float    kScaleFrac   = 1024.0f;

enum Group : uint8_t { kOther, kBars, kArea, kEquip, kSouls, kBoss, kOffCanvas, kGroups };

/* Where a group grows from, and which way it moves on a wide picture with the
 * HUD in the corners (-1 left, 1 right, 0 stays). kOther never changes. */
struct Anchor {
    float x, y;
    int side;
};
constexpr Anchor kAnchor[kGroups] = {
    {0.0f, 0.0f, 0},                // kOther
    {0.0f, 0.0f, -1},               // kBars
    {kCanvasW, 0.0f, 1},            // kArea
    {0.0f, kCanvasH, -1},           // kEquip
    {kCanvasW, kCanvasH, 1},        // kSouls
    {kCanvasW * 0.5f, kCanvasH, 0}, // kBoss
    {0.0f, 0.0f, -1},               // kOffCanvas: as kBars
};

/* PCGauge's objects by layout name. The first that matches wins, so the names
 * in full come before the prefixes that would also match them. */
struct Piece {
    const char* name;
    Group group;
    bool prefix;
};
constexpr Piece kPieces[] = {
    {"icon_eye_normal1", kArea, false},  // the area's emblem, beside its name
    {"icon_eye_phantom1", kArea, false},
    {"icon_eye_ghost1", kArea, false},
    {"eye_BG_s", kArea, false},
    {"map_name", kArea, false},
    {"icon_eye_", kBars, true},          // the eye emblem left of the bars
    {"piece17", kBars, false},
    {"plate_", kBars, true},             // plate_hp, plate_mp, plate_sp, plate_hp1
    {"bar_", kBars, true},
    {"outline_", kBars, true},
    {"HP_endbar", kBars, false},
    {"MP_endbar", kBars, false},
    {"SP_endbar", kBars, false},
    {"icon_state", kBars, true},         // the status icons under the bars
    {"icon_broke_", kBars, true},
    {"HP icon", kOffCanvas, false},
    {"SP icon", kOffCanvas, false},
    {"MP icon", kOffCanvas, false},
    {"F20-00_", kEquip, true},           // the four slots (one name ends in a full-width letter)
    {"text_magic_name", kEquip, false},
    {"text_item_name", kEquip, false},
    {"soul_param", kSouls, false},
    {"F20_boss_HP", kBoss, true},
};

/* What the knobs ask for, read again after any setenv. */
struct Wanted {
    float size = 1.0f;
    bool corners = true;
    bool shown[kGroups] = {true, true, true, true, true, true, true};
};
Wanted s_want;
unsigned s_want_gen;
PPU_THREAD_LOCAL float s_text_scale = 1.0f;   /* the size of the group being drawn (dialog_draw) */
uint32_t s_hud_draws = 1;   /* draws of the HUD dialog, from 1 */
uint32_t s_loading_at;      /* s_hud_draws at the loading screen's last draw; 0 = never */

/* A pointer read from the game's objects, checked before it is followed. */
bool guest_ok(uint32_t a, uint32_t align)
{
    return a >= 0x00010000u && a < 0xD0000000u && (a & (align - 1u)) == 0;
}

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

bool knob_on(const char* name)
{
    const char* v = getenv(name);
    return !v || !*v || strcmp(v, "0") != 0;
}

void want_read()
{
    Wanted w;
    const int pct = ps3_env_int("DES_HUD_SIZE", 100);
    w.size = (float)(pct < 25 ? 25 : pct > 200 ? 200 : pct) / 100.0f;
    const char* hud = getenv("DES_HUD");
    w.corners = !hud || strcmp(hud, "centre") != 0;
    w.shown[kBars] = w.shown[kOffCanvas] = knob_on("DES_HUD_BARS");
    w.shown[kArea] = knob_on("DES_HUD_AREA");
    w.shown[kEquip] = knob_on("DES_HUD_EQUIP");
    w.shown[kSouls] = knob_on("DES_HUD_SOULS");
    const bool changed = w.size != s_want.size || w.corners != s_want.corners ||
                         memcmp(w.shown, s_want.shown, sizeof w.shown) != 0;
    s_want = w;
    if (!changed) return;
    char hidden[64] = "";
    const struct { Group g; const char* name; } groups[] = {
        {kBars, "bars"}, {kArea, "area name"}, {kEquip, "equipment"}, {kSouls, "souls"}};
    for (const auto& g : groups)
        if (!w.shown[g.g]) {
            if (hidden[0]) strncat(hidden, ", ", sizeof hidden - strlen(hidden) - 1);
            strncat(hidden, g.name, sizeof hidden - strlen(hidden) - 1);
        }
    fprintf(stderr, "[des] HUD: size %d%%, %s on a wide picture%s%s\n", (int)(w.size * 100.0f + 0.5f),
            w.corners ? "corners" : "centred", hidden[0] ? ", hidden: " : "", hidden);
}

/* The object's layout name against a piece's: UTF-16 code units against ASCII. */
bool name_matches(uint32_t name, const Piece& p)
{
    size_t i = 0;
    for (; p.name[i]; i++)
        if (vm_read16(name + 2 * (uint32_t)i) != (uint8_t)p.name[i]) return false;
    return p.prefix || vm_read16(name + 2 * (uint32_t)i) == 0;
}

Group group_of_name(uint32_t rec)
{
    if (!guest_ok(rec, 4)) return kOther;
    const uint32_t name = vm_read32(rec);
    if (!guest_ok(name, 2)) return kOther;
    for (const Piece& p : kPieces)
        if (name_matches(name, p)) return p.group;
    return kOther;
}

/* The group of child i, kept while the same object holds the same record. */
uint32_t s_seen_obj[kMaxChildren], s_seen_rec[kMaxChildren];
uint8_t s_seen_group[kMaxChildren];

Group group_of(uint32_t i, uint32_t obj)
{
    const uint32_t rec = vm_read32(obj + kObjRecord);
    if (s_seen_obj[i] != obj || s_seen_rec[i] != rec) {
        s_seen_obj[i] = obj;
        s_seen_rec[i] = rec;
        s_seen_group[i] = (uint8_t)group_of_name(rec);
    }
    return (Group)s_seen_group[i];
}

/* The group's rect: the dialog's rect r grown by size about the anchor, and
 * moved by shift. Identity at size 1 and no shift. */
void group_rect(const float r[4], const Anchor& a, float size, float shift, float out[4])
{
    out[0] = a.x * (1.0f - size) + shift + size * r[0];
    out[1] = a.y * (1.0f - size) + size * r[1];
    out[2] = out[0] + size * (r[2] - r[0]);
    out[3] = out[1] + size * (r[3] - r[1]);
}

/* One whole draw of the dialog. 0x00F60BD0 only picks the draw (0x00F5EAC0,
 * or 0x00F5F878 when +0x9C is set) and leaves it on the trampoline, a tail
 * call that would run after this wrapper returns: it runs here instead. */
void draw_once(ppu_context* ctx, uint32_t dlg, uint32_t renderer, uint64_t lr)
{
    ctx->gpr[3] = dlg;
    ctx->gpr[4] = renderer;
    ctx->lr = lr;
    func_00F60BD0(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
}

/* A dialog's children; false if it has none to go through. */
bool hud_children(uint32_t dlg, uint32_t& arr, uint32_t& n)
{
    arr = vm_read32(dlg + kDlgChildren);
    n = vm_read32(dlg + kDlgChildCount);
    if (n > kMaxChildren) n = kMaxChildren;
    return guest_ok(arr, 4) && n > 0;
}

/* Each HUD dialog's groups, said once, at its first draw. */
void hud_said_once(uint32_t dlg)
{
    static uint32_t said_for;
    uint32_t arr, n;
    if (said_for == dlg || !hud_children(dlg, arr, n)) return;
    said_for = dlg;
    unsigned count[kGroups] = {};
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t obj = vm_read32(arr + 4 * i);
        count[guest_ok(obj, 4) ? group_of(i, obj) : kOther]++;
    }
    fprintf(stderr, "[des] HUD: dialog 0x%08X, %u pieces: bars %u, area %u, equipment %u, souls %u, boss %u, "
            "off the canvas %u, others %u\n", dlg, n, count[kBars], count[kArea], count[kEquip], count[kSouls],
            count[kBoss], count[kOffCanvas], count[kOther]);
}

/* soul_0 to soul_99: a digit after "soul_". */
bool soul_cell(uint32_t obj)
{
    const uint32_t rec = vm_read32(obj + kObjRecord);
    const uint32_t name = guest_ok(rec, 4) ? vm_read32(rec) : 0u;
    if (!guest_ok(name, 2)) return false;
    for (uint32_t i = 0; i < 5; i++)
        if (vm_read16(name + 2 * i) != (uint8_t)"soul_"[i]) return false;
    const uint16_t c = vm_read16(name + 10);
    return c >= '0' && c <= '9';
}

/* The loading screen. Its soul_0 to soul_99 hold the frames of its two soul
 * icons (des_options.cpp), parked right of the canvas, where a 16:9 screen
 * never shows them. A widened canvas showed them as a column in the right
 * band: while it is widened they are not drawn. */
void loading_draw(ppu_context* ctx, uint32_t dlg)
{
    s_loading_at = s_hud_draws;
    uint32_t arr, n;
    if (des_hud_side_width() <= 0.0f || !hud_children(dlg, arr, n)) {
        func_00F60BD0(ctx);
        return;
    }
    uint32_t hid[kMaxChildren], nh = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t obj = vm_read32(arr + 4 * i);
        if (!guest_ok(obj, 4)) continue;
        const uint32_t f = vm_read32(obj + kObjFlags);
        if (!(f & kObjDrawn) || !soul_cell(obj)) continue;
        vm_write32(obj + kObjFlags, f & ~kObjDrawn);
        hid[nh++] = obj;
    }
    if (!nh) {
        func_00F60BD0(ctx);
        return;
    }
    const uint64_t lr = ctx->lr;
    draw_once(ctx, dlg, (uint32_t)ctx->gpr[4], lr);
    ctx->lr = lr;
    for (uint32_t k = 0; k < nh; k++) vm_write32(hid[k] + kObjFlags, vm_read32(hid[k] + kObjFlags) | kObjDrawn);
}

void dialog_draw(ppu_context* ctx)   /* for 0x00F60BD0 */
{
    const uint32_t dlg = (uint32_t)ctx->gpr[3];
    const uint32_t vt = guest_ok(dlg, 4) ? vm_read32(dlg) : 0u;
    if (vt == kLoadingVt && vm_read8(dlg + kDlgVisible) != 0) {
        loading_draw(ctx, dlg);
        return;
    }
    if (vt != kHudDialogVt || vm_read8(dlg + kDlgVisible) == 0) {
        func_00F60BD0(ctx);
        return;
    }
    hud_said_once(dlg);
    /* Up to now or in the frame before: the loading screen draws before or
     * after the HUD. */
    const uint32_t draw_n = ++s_hud_draws;
    const bool loading = s_loading_at != 0 && draw_n - s_loading_at <= 1u;
    if (s_want_gen != ps3_env_gen) {
        s_want_gen = ps3_env_gen;
        want_read();
    }
    const Wanted w = s_want;
    const float side = des_hud_side_width();
    bool all_shown = true;
    for (bool s : w.shown) all_shown &= s;
    uint32_t arr, n;
    if ((w.size == 1.0f && all_shown && side <= 0.0f) || !hud_children(dlg, arr, n)) {   /* (the game's own HUD) */
        func_00F60BD0(ctx);
        return;
    }

    /* Each group's rect, and which groups share one draw (the first group with
     * the same rect). Hidden groups get none. */
    float r[4];
    for (int k = 0; k < 4; k++) r[k] = vm_readf(dlg + kDlgRect + 4 * (uint32_t)k);
    float rect[kGroups][4];
    int pass_of[kGroups];
    bool shown[kGroups];
    for (int g = 0; g < kGroups; g++) {
        const Anchor& a = kAnchor[g];
        const float size = g == kOther ? 1.0f : w.size;
        const float shift = w.corners ? (float)a.side * side : 0.0f;
        group_rect(r, a, size, shift, rect[g]);
        shown[g] = w.shown[g] && !(g == kOffCanvas && side > 0.0f) && !(loading && shift != 0.0f);
        pass_of[g] = g;
        for (int e = 0; e < g; e++)
            if (memcmp(rect[e], rect[g], sizeof rect[g]) == 0) {
                pass_of[g] = pass_of[e];
                break;
            }
    }

    uint32_t obj[kMaxChildren];
    uint8_t group[kMaxChildren];
    bool drawn[kMaxChildren];
    bool any_change = false;
    for (uint32_t i = 0; i < n; i++) {
        obj[i] = vm_read32(arr + 4 * i);
        group[i] = kOther;
        drawn[i] = false;
        if (!guest_ok(obj[i], 4)) {
            obj[i] = 0;
            continue;
        }
        group[i] = (uint8_t)group_of(i, obj[i]);
        drawn[i] = (vm_read32(obj[i] + kObjFlags) & kObjDrawn) != 0;
        if (drawn[i] && (pass_of[group[i]] != pass_of[kOther] || !shown[group[i]])) any_change = true;
    }
    if (!any_change) {   /* (every drawn object in kOther's draw: the game's own) */
        func_00F60BD0(ctx);
        return;
    }

    const uint32_t renderer = (uint32_t)ctx->gpr[4];
    const uint64_t lr = ctx->lr;
    for (int p = 0; p < kGroups; p++) {
        if (pass_of[p] != p) continue;
        bool any = false;
        for (uint32_t i = 0; i < n; i++) {
            if (!obj[i] || !drawn[i]) continue;
            const bool want = pass_of[group[i]] == p && shown[group[i]];
            const uint32_t f = vm_read32(obj[i] + kObjFlags);
            const uint32_t nf = want ? f | kObjDrawn : f & ~kObjDrawn;
            if (nf != f) vm_write32(obj[i] + kObjFlags, nf);
            any |= want;
        }
        if (!any) continue;
        for (int k = 0; k < 4; k++) vm_writef(dlg + kDlgRect + 4 * (uint32_t)k, rect[p][k]);
        s_text_scale = p == kOther ? 1.0f : w.size;
        draw_once(ctx, dlg, renderer, lr);
    }
    s_text_scale = 1.0f;
    ctx->lr = lr;
    for (int k = 0; k < 4; k++) vm_writef(dlg + kDlgRect + 4 * (uint32_t)k, r[k]);
    for (uint32_t i = 0; i < n; i++) {
        if (!obj[i] || !drawn[i]) continue;
        const uint32_t f = vm_read32(obj[i] + kObjFlags);
        if (!(f & kObjDrawn)) vm_write32(obj[i] + kObjFlags, f | kObjDrawn);
    }
}

/* The text state the drawer's commands play back into: +0x24 in the object at
 * +0x38 of the list's owner (+0x14). */
uint32_t text_state(uint32_t list)
{
    return vm_read32(vm_read32(list + 0x14) + 0x38) + 0x24u;
}

/* A text begins with its font's number (r4). Inside a group drawn at another
 * size, the group's size goes with it in the number's top bits. */
void font_begin(ppu_context* ctx)   /* for 0x0071B728 */
{
    const float s = s_text_scale;
    const uint32_t font = (uint32_t)ctx->gpr[4];
    if (s != 1.0f && font < 0x10000u) {
        int q = (int)(s * kScaleFrac + 0.5f);
        q = q < 1 ? 1 : q > 0x7FFF ? 0x7FFF : q;
        ctx->gpr[4] = kFontScaled | (uint32_t)q << 16 | font;
    }
    func_0071B728(ctx);
}

/* Its playback: r3 = the drawer's list, the number at the next 4-byte boundary
 * of the read position (+0x08). The playback sets the glyph scale and the line
 * spacing from the font's entry, so it is given the number as the game gave
 * it, and then both are multiplied by the size. */
void font_play(ppu_context* ctx)   /* for 0x000B15C8 */
{
    const uint32_t list = (uint32_t)ctx->gpr[3];
    const uint32_t at = (vm_read32(list + 0x08) + 3u) & ~3u;
    const uint32_t end = vm_read32(list + 0x0C) + vm_read32(list + 0x10);
    const uint32_t font = end > at + 4u ? vm_read32(at) : 0u;   /* (the playback's own bounds check) */
    if (!(font & kFontScaled)) {
        func_000B15C8(ctx);
        return;
    }
    vm_write32(at, font & 0xFFFFu);
    func_000B15C8(ctx);
    vm_write32(at, font);
    const float s = (float)((font >> 16) & 0x7FFFu) / kScaleFrac;
    const uint32_t state = text_state(list);
    const uint32_t scaled[] = {kGlyphScale, kGlyphScale + 4u, kLineSpacing};
    for (uint32_t off : scaled) vm_writef(state + off, vm_readf(state + off) * s);
}

/* A text shape then sets its line spacing (r4, pixels; 0 for one line). Inside
 * a group drawn at another size, the spacing is scaled with it. */
void line_spacing(ppu_context* ctx)   /* for 0x0071B5A0 */
{
    const float s = s_text_scale;
    const int px = (int16_t)ctx->gpr[4];
    if (s != 1.0f && px > 0) {
        const float want = (float)px * s;
        if (want * kSpacingFrac < (float)(kSpacingExact - 1u))
            ctx->gpr[4] = kSpacingExact | (uint32_t)(want * kSpacingFrac + 0.5f);
        else
            ctx->gpr[4] = (uint32_t)(int)(want + 0.5f);
    }
    func_0071B5A0(ctx);
}

/* Its playback: the 16-bit spacing at the next 2-byte boundary of the read
 * position. A scaled one is written exactly. */
void spacing_play(ppu_context* ctx)   /* for 0x000B14C8 */
{
    const uint32_t list = (uint32_t)ctx->gpr[3];
    const uint32_t raw = vm_read16((vm_read32(list + 0x08) + 1u) & ~1u);
    func_000B14C8(ctx);
    if (raw & kSpacingExact)
        vm_writef(text_state(list) + kLineSpacing, (float)(raw & (kSpacingExact - 1u)) / kSpacingFrac);
}

} // namespace

/* From ps3_title_options_init, before the guest runs. Wrapped whatever the
 * knobs say at start, so the settings window can change them while playing. */
extern "C" void des_hud_init(void)
{
    ppu_register_function(kDialogDraw, dialog_draw);
    ppu_register_function(kFontBegin, font_begin);
    ppu_register_function(kFontPlay, font_play);
    ppu_register_function(kFontSpacing, line_spacing);
    ppu_register_function(kSpacingPlay, spacing_play);
    s_want_gen = ps3_env_gen;
    want_read();
}
