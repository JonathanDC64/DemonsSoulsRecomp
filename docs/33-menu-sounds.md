# 33. The game's menu sounds, for the wrapper UI

Found by static analysis (EBOOT BLUS30443, the lift in `work/recomp`, the
disc's sound banks), then built: `port/des_menu_sfx.cpp` is the hook below,
lifted in place of 0x00724E40, and the wrapper UI's sounds go through it.

## What was built and checked (2026-10-17)

- `des_menu_tick` (des_menu_sfx.cpp) as sketched under "Host side": a
  16-entry queue filled by the UI on the render thread, drained on the main
  thread before the menu update; the six ids only; sounds older than 300 ms
  dropped; skipped while `*0x01B4B150` is 0. `des_menu_sfx_init()` (from
  `ps3_title_options_init`) points `g_des_ui_sound` at it.
- The UI's sounds map to: cursor and tab change 0; click, Play and binding
  changed 100; menu or window closed 200; menu or window opened and a
  warning question 300; a value changed or toggled 400. 999999999 is not used.
- Lift: `0x724E40=des_menu_tick` added to the docs/27 recipe. A lift without
  it still builds; the UI is then silent.
- `DES_UI_SOUND=0` keeps the UI silent. `DES_MENU_SFX_TEST=1` plays the six
  cues in turn every 2 s and logs each slot before and after.
- Run (pinned regression save, boot into the Nexus): the manager is
  `0x48501020` from early boot on. Each slot is a 16-byte object: a vtable
  pointer (0x01852728), the cue handle, a second vtable (0x01852738), 0.
  After each play the handle word holds a new, increasing value (1, 2, 4, 5,
  0x20008, 0x3000C, ...), so MagicOrchestra found and started every cue,
  999999999 included, at the boot logos as well as in game. A start-screen
  walk with scripted keys queued 0, 300, 0 and 400 from the UI, and all were
  played.
- Not checked by ear yet (the test machine was locked; the game's music
  covers the cues in a recording). What 300 and 999999999 sound like is still
  open (question 3 below).

The analysis as it was written follows.

The wrapper UI (docs/32) wants to play the game's own cursor, confirm, cancel,
tab and window sounds. Short version:

- The menus play every UI sound through one guest call, **`0x0072D970(id)`**:
  "play menu SE `id`". It builds the cue name `"f%09d"` and starts it as a 2D
  MagicOrchestra cue.
- The menu sounds are `f000000000`, `f000000100`, `f000000200`,
  `f000000300`, `f000000400` and `f999999999`. The resident bank
  `sound/ds_se_main.mosb` holds exactly these six `f` cues and no other.
- Only eight IDs are safe to pass. Any other ID makes the game write through a
  wild pointer (see "The play function").
- Hook point: `0x00724E40`, the menu system's per-frame update. The main
  frame function `0x0001B8D0` calls it on the main thread every frame, right
  after the game's sound update `0x003DBD00`. It is called with `bl`, so the hook needs
  `--override-funcs "...,0x724E40=des_menu_tick"` and a re-lift, as
  `des_move_post` did.

## The play function

### Call chain

```
menu code ──bl──> 0x0072D970 (id)                  play menu SE, type 2 = 'f'
                     r3 = *(u32*)0x01B4B150         the menu-sound manager
                     r4 = id, r5 = 2
                     tail call (g_trampoline_fn) ──> 0x0072D538 (mgr, id, type)
0x0072D538:
  if (mgr == 0 || id == -1) return;
  slot = IdToSlot(id)                              inlined switch, 8 slots
  0x003DBA18(&mgr->slot[slot].handle)              stop what that slot plays
  name = "%c%09d" % ("acfopsmvxbg"[type], id)      type > 10 gives '?'
  0x00D68218(&str, name)                           build the name string
  0x003DBBB0(&h, &str, -1)                         play a 2D cue by name
                     -> 0x00DA1100(&h, &str, -1, 0) MagicOrchestra
  0x00D67F80(&str)                                 free the string
  0x00DBA298(&mgr->slot[slot].handle, &h)          keep the new handle
```

- Calling convention (PPC64 ELFv1, one TOC for the whole EBOOT, r2 =
  0x019B5040): `0x0072D970` takes r3 = the ID (s32) and returns nothing. It
  loads the manager from the TOC itself (`r30 = [r2-0x72E8]` = 0x018F8834,
  `[r30-0x8000]` = 0x01B4B150).
- `0x0072D538` takes r3 = manager, r4 = ID, r5 = type index into
  `"acfopsmvxbg"` (the string at 0x016D4580). Dark Souls uses the same letters:
  a environment, c character, f menu, o object, p, s SFX, m BGM, v voice,
  x/b material, g. The digits come from divide-by-constant code that writes
  bytes +0x71..+0x79 of a stack buffer, highest digit first, so the name is
  `printf("%c%09d")`: ID 100 becomes `f000000100`.
- Sibling entry points that share the same body:

  | Entry | Does | Callers |
  |---|---|---|
  | `0x0072D970(id)` | play, type 2 `f` (menu SE) | 247 direct `bl` call sites in the menu code (0x71xxxx-0x8Fxxxx): 208 with a constant ID, 39 with a variable one (35 of them one-line wrappers) |
  | `0x0072D950(id)` | play, type 6 `m` (BGM) | 0x180AF0, 0x1844A8, always id 11111 (`m000011111` is in `ds_bgm_other.mosb`) |
  | `0x0072D930(id)` | play, type 7 `v` (voice) | 0x8E1D10, 0x8E2760, ID from a table at 0x016FD524 (888888888 / 999999999) |
  | `0x0072D990(&id, stop)` | stop==0: play type 6; else stop that slot | not called directly |
  | `0x0072D200(id)` / `0x0072D0B0(mgr, id)` | stop the slot's sound | 0x180630, 0x182D68, 0x1845A8 (id 11111) |

- The manager is an object of 0x88 bytes made by `0x0072E5F0` during boot init
  (`0x00020DD8` -> `0x00729F28`), stored at `*0x01B4B150`, and destroyed by
  `0x0072D2E0`. It holds 8 slots of 16 bytes at +0x08; the first word of each
  is the cue handle. One slot per ID, so a new play of an ID stops the
  previous one first. Rapid repeats cut each other off, as in the game.

### The eight IDs, and the wild write

`IdToSlot` (standalone at `0x0072CEF8`, inlined into 0x72D538, 0x72D0B0 and
0x72D200) is a `switch`:

| id | slot | cue |
|---|---|---|
| 0 | 0 | f000000000 |
| 100 | 1 | f000000100 |
| 200 | 2 | f000000200 |
| 300 | 3 | f000000300 |
| 999999999 | 4 | f999999999 (bank has it; the menus never pass it as a constant) |
| 11111 | 5 | used with type 6: m000011111 |
| 888888888 | 6 | voice table only |
| 400 | 7 | f000000400 |

The default case is broken. In `0x0072D538`, at 0x72D5F8, the code compares
**r9** with -1. At that point r9 holds the address of the manager's global
(0x01B4B150), not a slot number, so the comparison never matches and the code
falls through with slot = 0x01B4B150. It then stops a handle at
`mgr + 8 + 0x1B4B150*16` and later stores the new handle there: a wild write.
(The standalone `0x72CEF8` returns 7 for unknown IDs.) **The host must only
pass IDs from the table**; for the wrapper UI that is {0, 100, 200, 300, 400,
999999999}. ID -1 is a safe no-op. The menus pass -1 when there is "no sound"
(the DsMenuDlgInfo plates at 0x888040/0x888088).

## What each ID means

Evidence: the input handlers that call `0x0072D970` with a constant. The
handlers read menu actions through `0x00F55A18(pad, action)`. The action
codes, worked out from how the handlers use them: 0x78/0x79 and 0x80/0x81 up/down,
0x7A/0x7B and 0x82/0x83 left/right, 0x88 decide, 0x89 cancel, 0x8C
close-menu (Start), 0x8F/0x90 previous/next page (L1/R1), 0x8E/0x92/0x94/0x95
the extra face buttons. Call-site counts: id 0 ×65, 100 ×43, 200 ×50, 300 ×6,
400 ×40, -1 ×4, a variable ID ×39 (35 of those are one-line `r3=r4; bl 0x72D970`
wrappers).

| Wrapper UI sound | ID | Cue | Evidence | Confidence |
|---|---|---|---|---|
| cursor move / focus | **0** | f000000000 | Options `0x007CC7E0`: up/down (0x80/0x81, 0x78/0x79) moves the cursor, then `72D970(0)` at 0x7CCB3C/0x7CCB68. Brightness `0x008EC7B8`: moving between its two rows plays 0. | high |
| confirm / decide | **100** | f000000100 | Options: 0x88 runs 0x7C8C10, then `72D970(100)` at 0x7CCCDC. Talk yes/no box `0x00791D30`: 0x88 plays 100 (0x791FB0). Brightness: 0x88 plays 100. | high |
| cancel / back | **200** | f000000200 | Options: 0x89 runs 0x7C89C0, then 200 (0x7CCD20). Talk box: 0x89 plays 200 (0x791E78). Status `0x0082AA08`: 0x89 plays 200. Brightness: 0x89 plays 200. | high |
| window / sub-menu opens | **300** | f000000300 | Status `0x0082AA08`: 0x8E opens a sub-window (message 0x3EE), then 300 (0x82ABE8). Equipment `0x00734080`: 0x94 opens 0x731BD0, then 300. The DsMenuDlgInfo popups (0x8880D8, 0x888A50, 0x88AC10) play 300 when they appear. | medium |
| slider / value change | **400** | f000000400 | Brightness `0x008EC7B8`: left/right (0x7A/0x7B) changes the 0..10 value at +0xFC, then 400 (0x8EC94C). Character creation sliders (0x855B30, 0x863E80, 0x867748, 0x8679D8): runs of 400. | high |
| tab / page switch | **0** | f000000000 | The pause menu's L1/R1 page switch `0x007234E0(page)` (called on 0x8F/0x90 from Options, Status, Equipment, Inventory, ...) changes the page, then plays `72D970(0)` at 0x723628. The game uses the cursor sound for pages. | high |
| menu open (Start) | (300?) | | No sound call found on the open/close helpers (0x723708, 0x721B48, 0x721B68, 0x721E08, 0x81FA40, 0x841A90, 0x8ABCE8, 0x8ABD18). It may be played through a virtual call. Use 300 until checked. | low |
| error / buzzer | (999999999?) | f999999999 | No buzzer call found. `f999999999` is the only other menu cue in the bank and the slot table accepts its ID; what it sounds like is unknown. Fall back to 200. | unknown |

## Hook design

### Why 0x00724E40

- The main loop is `0x00015220`: `do {} while (!0x0001B8D0());`. The frame
  function `0x0001B8D0` runs on the main PPU thread, the thread from entry
  0x124C0 -> 0x125F0 -> 0x15220. In a straight line, with no branch between
  them, it calls:
  - `0x003DBD00(f1 = dt)` at 0x1B9A4: the game sound manager's per-frame
    update (it calls the MagicOrchestra listener/update functions
    0xD9F6E8, 0xDA0418, 0xDA0960, ...);
  - `0x00724E40(f1 = dt)` at 0x1B9C4: the menu system's per-frame update. Its
    whole body is `r3 = *(u32*)0x01B4B114; tail-call 0x00724AD0`.
- 0x724E40 runs every frame at the title and in game, on the thread whose frame
  also runs the menus. Starting the queued sounds there is the same as a menu
  calling `0x72D970` itself, with no race on the manager's slots.
- Both functions are reached by `bl` only (one caller each), so
  `ppu_register_function()` cannot hook them. That table is used only by
  `ps3_indirect_call` (bctrl), which is how `logo_update_skip` works on the
  virtual 0x8EF430. The hook has to be a lift-time override, like
  `des_move_post` (`0x273D38`). Changing `--override-funcs` touches
  `ppu_recomp.h`, so all 14 chunks rebuild (about 15 min).
- Do not start sounds from `g_gcm_flip_request_hook` or the cellPad hook. They
  run inside an HLE call, on the thread that flips or polls, which is not
  necessarily the menu thread. That would race the manager's slot handles
  against the menus.

### Lift change

docs/27 re-lift recipe, with one more override:

```
--override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post,0x724E40=des_menu_tick"
```

The lifter then emits `void func_00724E40(ppu_context* ctx) { des_menu_tick(ctx); return; }`.
The original body is gone, so the hook redoes it. The caller (0x1B8D0)
already drains `g_trampoline_fn` after the call.

### Host side (sketch, for port/)

A single-producer queue like FFCCEOT's `cfie::queue_sound` /
`cfie_sound_update` (`FFCCEOT-Recomp/game/src/cfie_hooks.cpp`): the UI thread
pushes, and the main guest thread drains.

```cpp
// port/des_menu_sfx.cpp  (sketch, not built)
#include "ppu_recomp.h"
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {
constexpr uint32_t kMenuSeMgr  = 0x01B4B150u;   // *this = menu-sound manager (0 until boot init)
constexpr uint32_t kMenuSystem = 0x01B4B114u;   // *this = r3 for 0x00724AD0
struct Req { std::atomic<int32_t> id; std::atomic<int64_t> at_ms; };
constexpr uint32_t kQ = 16; constexpr int64_t kMaxAgeMs = 300;
Req q[kQ]; std::atomic<uint32_t> head{0}, tail{0};

bool allowed(int32_t id) {   // ONLY ids in IdToSlot's table: anything else is a wild write
    return id == 0 || id == 100 || id == 200 || id == 300 || id == 400 || id == 999999999;
}
}

extern "C" void des_queue_menu_sound(int32_t id)   // UI thread
{
    if (!allowed(id)) return;
    const uint32_t h = head.load(std::memory_order_relaxed);
    if (h - tail.load(std::memory_order_acquire) >= kQ) return;   // full: drop
    q[h % kQ].id.store(id, std::memory_order_relaxed);
    q[h % kQ].at_ms.store(now_ms(), std::memory_order_relaxed);
    head.store(h + 1, std::memory_order_release);
}

/* Lifted in place of 0x00724E40 (--override-funcs). Main thread, once a frame,
 * f1 = frame time. */
extern "C" void des_menu_tick(ppu_context* ctx)
{
    uint32_t t = tail.load(std::memory_order_relaxed);
    if (t != head.load(std::memory_order_acquire)) {
        // 0x72D538 is a normal ABI function: r1, r2, r13-r31, f14-f31 survive.
        // Keep what this entry still needs (f1 = dt) plus everything volatile.
        uint64_t g[13]; memcpy(g, ctx->gpr, sizeof g);              // r0..r12
        double   f[14]; memcpy(f, ctx->fpr, sizeof f);              // f0..f13
        const uint64_t lr = ctx->lr, ctr = ctx->ctr;
        const uint32_t cr = ppu_cr_pack(ctx);
        const int64_t now = now_ms();
        while (t != head.load(std::memory_order_acquire)) {
            const int32_t id = q[t % kQ].id.load(std::memory_order_relaxed);
            const bool fresh = now - q[t % kQ].at_ms.load(std::memory_order_relaxed) <= kMaxAgeMs;
            tail.store(++t, std::memory_order_release);
            if (!fresh || !allowed(id) || vm_read32(kMenuSeMgr) == 0) continue;
            ctx->gpr[3] = (uint32_t)id;
            ctx->lr = 0x00724E40u;
            func_0072D970(ctx);                      // sets g_trampoline_fn = func_0072D538
            while (g_trampoline_fn) { auto tf = g_trampoline_fn; g_trampoline_fn = nullptr; tf(ctx); }
        }
        memcpy(ctx->gpr, g, sizeof g); memcpy(ctx->fpr, f, sizeof f);
        ctx->lr = lr; ctx->ctr = ctr; ppu_cr_unpack(ctx, cr);
    }
    // The original 0x00724E40: r3 = *0x01B4B114; tail-call 0x00724AD0 (f1 untouched).
    ctx->gpr[3] = vm_read32(kMenuSystem);
    g_trampoline_fn = (void (*)(void*))func_00724AD0;
}
```

Notes:

- `ppu_context` fields to set: only `gpr[3]` (the ID) and `lr` (any guest
  address; the lifted code does not return through it). r1 is the caller's SP,
  which is valid: 0x72D538 makes its own 0x150-byte frame and writes its LR
  save into the caller's header slot (r1+0x10), which the ABI allows and which
  0x724AD0 does as well. r2 is already the EBOOT TOC (0x019B5040) because the
  caller is EBOOT code. r13 is the thread's own TLS pointer.
- To call guest code from host code that does **not** run on a guest thread,
  the runtime has `ppu_guest_call(opd, a0..a7)` and
  `ppu_guest_call_ct(code, toc, a0..a7)` (ps3recomp
  `runtime/ppu/ppu_loader.cpp`). They build a scratch `ppu_context` on a
  shared callback stack (0xCFFE0000 unless `ppu_guest_call_set_stack`). They
  are the wrong tool here: they would run 0x72D970 on the UI thread, at the
  same time as the menus. The OPD of 0x72D970 is 0x0194C570, if one is ever
  needed.
- The drain runs before the menu update, so a sound queued in frame N starts in
  frame N. The game stops a slot before reusing it, so pressing the same sound
  twice quickly restarts it, as the game's own menus do.
- To play other cues (any `f`/`s` name in a resident bank) without the slot
  table, copy 0x72D538's tail: `0x00D68218(&str, "f000000100")`,
  `0x003DBBB0(&h, &str, -1)`, `0x00D67F80(&str)`, then stop or keep `h`. The
  six `f` cues above are all the menu bank has, so this is not needed for the UI.

### Alternative without a re-lift (to look for at runtime)

`ppu_register_function` does hook a per-frame **virtual** method on the main
thread, for example a Step's update. There are `Step<TitleMenuStep>`,
`Step<InGameMenuStep>`, `Step<CommonMenuStep>` and others. To find one that
runs every frame both at the title and in game, run with
`PS3_ICALL_TRACE=<main thread id>` (every bctrl target on that thread, in
order) and pick a target that appears once per frame in both places. This is
less robust than 0x724E40, because steps change between title, loading and
in game.

## Open questions, with the runtime check for each

The game is needed for all of these. Run each through `desprobe.sh` with a
`CMDS` file for the debug console.

1. **The manager exists at the title and in game, and the slots move when
   the menus play.** No rebuild needed. Debug console: `mem 1b4b150 4` gives
   the manager M (non-zero after boot). In a menu, run `mem <M+8> 128` before
   and after one cursor move, one confirm and one cancel. Slot 0 (M+0x08),
   slot 1 (M+0x18) and slot 2 (M+0x28) should change their first word each
   time. If they do, IDs 0/100/200 are confirmed without a hook.
2. **Which thread calls 0x72D970.** Expected: the main thread (the one that
   runs 0x1B8D0). With the override in, log `ctx->thread_id` once in
   `des_menu_tick`. For the menus' own calls, a temporary
   `0x72D970=des_menu_se_log` override that logs `r3` and `thread_id` and then
   redoes the body (`r4=r3; r5=2; r3=*0x1B4B150; g_trampoline_fn=func_0072D538`).
   That logger also lists every ID the menus play while you click through them;
   it would show 999999999 if the variable-ID wrappers ever pass it.
3. **What each cue sounds like**, mainly `f000000300` (window open?) and
   `f999999999` (buzzer?). With the hook in, add a test knob
   (e.g. `DES_MENU_SFX_TEST=1`) that queues 0, 100, 200, 300, 400, 999999999
   in turn every 2 s at the title, and listen. Also confirm that
   `ds_se_main` is loaded at the title, before any level loads.
4. **The menu-open sound.** Open the pause menu with Start in game, with the
   item 2 logger on, and see which ID (if any) goes through 0x72D970.
5. **Loading screens.** Check that 0x1B8D0, and so 0x724E40, keeps running
   while a level loads. The FFCCEOT-style 300 ms age limit drops sounds queued
   during a stall in either case.
6. **Action codes.** The 0x78..0x95 meanings above come from how the handlers
   use them. If they are needed, a log of `0x00F55A18(r3, r4)` with a
   non-zero return while pressing each button pins them down (that needs an
   override as well, since it is direct-called).

## Addresses at a glance

| What | Address |
|---|---|
| play menu SE (id) | 0x0072D970 (OPD 0x0194C570) |
| play (mgr, id, type) | 0x0072D538 |
| play BGM / voice / stop | 0x0072D950 / 0x0072D930 / 0x0072D200, 0x0072D0B0 |
| id -> slot | 0x0072CEF8 |
| menu-sound manager pointer | 0x01B4B150 (create 0x0072E5F0, destroy 0x0072D2E0) |
| type letters `"acfopsmvxbg"` | 0x016D4580 |
| 2D cue play by name (game wrapper) | 0x003DBBB0 -> MagicOrchestra 0x00DA1100 |
| 3D cue play by name (positional) | 0x003DBB60 -> 0x00DA1040 |
| stop handle | 0x003DBA18 -> 0x00DBAEF8 |
| name string ctor / dtor | 0x00D68218 / 0x00D67F80 |
| main loop / frame | 0x00015220 / 0x0001B8D0 |
| game sound update (per frame) | 0x003DBD00 (from 0x1B9A4) |
| **menu system update (hook)** | **0x00724E40** (from 0x1B9C4); body: r3 = *0x01B4B114, tail 0x00724AD0 |
| menu action query | 0x00F55A18(pad, action) |
| resident menu bank | `sound/ds_se_main.mosb`: f000000000/100/200/300/400, f999999999 |

How the addresses were found: Python over the ELF (TOC-relative references
resolved through the r2 table, data-pointer reverse map) and over the lifted C
in `work/recomp`. `0x72D970` stood out as the most-called constant-argument
function in the menu code whose body ends in MagicOrchestra. The DsMenuDlg*
class registrations (0x71AF00..0x8F4xxx) give the class names used above;
they are a rough "which class is this" guide, not exact ownership.
