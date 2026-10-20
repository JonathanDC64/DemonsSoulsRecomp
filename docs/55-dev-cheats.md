# Dev cheats: god mode, attack power, movement speed (design 2026-10-18, built 2026-10-19)

Designed on 2026-10-18 and built on 2026-10-19, with the recommended answer to each question in section 14 except question 8, which is still open. Section 15 lists where the build differs from this design and what its first run showed. Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`), the release the port uses. The research used a disassembly of that ELF and Ghidra decompiles. Neither is in the repo.

Two labels are used below:
- **(read)**: I read it in the disassembly, a decompile, or our own source.
- **(inferred)**: my conclusion, not checked in a run yet. Section 11 is one run request that checks the open points.

## Summary

- **Three cheats, three live knobs, all off by default.** `DES_GOD=1` keeps the player's HP, MP and stamina from dropping. `DES_ATK=<x>` multiplies the damage the player's hits do. `DES_SPEED=<x>` multiplies how far the player moves. Test scripts and the console can change them at any time, like the other `DES_*` knobs.
- **12 instruction hooks.** All of them go through one host function, `des_dev_hook`, with the existing `--insn-hooks` list. Every hook checks that the character is the player first, because the same code runs for every enemy. Every store to the player's HP, MP and stamina in the game code was checked against them (section 2.4).
- **Dev builds only.** A CMake option `DES_DEV` (default OFF) compiles in the cheats and a Developer tab in the settings. A release build has an empty `des_dev_hook` and nothing else. One macro guards it all.
- **A marker.** Dev builds carry the string `DES-DEV-BUILD`. A small scanner, `tools/ci/check_release_no_dev.py`, fails a release that contains it or the knob names, or a file it cannot look inside. No CI runs it yet; it is run by hand (section 7).
- **"dev" in the version.** Dev builds are to add `+dev` to the version string. That needs one input in lane 4's new `DesVersion.cmake` (port #62), so it waits for it (section 15).
- **Cheats force offline.** The online state is decided at start. Cheats on at start: the network knobs are cleared before the network modules start. Online at start, or the network turned on later: the cheats stay off until the next start.
- **Effort: about 3 to 4 days**, including lane 4's part and one run (section 12).

## 1. The cheats

| Knob | Default | Values | What it does |
|---|---|---|---|
| `DES_GOD` | 0 | 0, 1 | The player's HP, MP and stamina never drop: hits, falls, poison-type ticks, script damage, spells, attacks, rolls, guarding and the dash cost nothing. Gains still count (healing, regen, level-ups). |
| `DES_ATK` | 1 | 0 to 1000 (a float) | The HP damage of the player's hits on others, times x. 0 means no damage. |
| `DES_SPEED` | 1 | 0.25 to 10 (a float) | How far the player moves each step, times x: walk, run, dash, roll and attack lunges. Animations keep their speed, so the feet slide. |
| `DES_WARP` | unset | `x,y,z` | Already written on the unmerged branch `tools/des-warp` (a teleport for test scripts). It moves behind the same gate (section 10). |
| `DES_DEVLOG` | 0 | 0, 1 | One log line per cheat action, rate-limited. For runs. |

- All are live. `PS3_KNOB` re-reads them when the environment changes, through the console's `setenv` or the settings tab (read, `ps3_knob.h`).
- Out-of-range values are clamped. A value that does not parse counts as the default.
- A knob that is not at its default counts as "a cheat is on" for the offline rule (section 6).
- They are not saved in `settings.ini` (section 9). Every start begins with all cheats off unless the environment sets them.

## 2. Where the game does it

### 2.1 The character fields (read)

| Field | Offset | Notes |
|---|---|---|
| HP, max HP | chr+0x3C4, +0x3C8 | ints |
| MP, max MP | chr+0x3CC, +0x3D0 | ints |
| Stamina, max stamina | chr+0x3D4, +0x3D8 | ints; stamina may go down to -50 |
| Flags | chr+0x310 | 64 bits; 0x4 is the game's own "no damage" flag, 0x10 keeps HP at 1 at least |
| Movement object (mv) | `*(chr+0x20)`, at chr+0xBA0 | mv+4 points back at chr; mv+0x10 is the physics proxy |
| Root-motion step | mv+0xF0 | the vector the animation moves the character this step |
| Speed factor | mv+0x128 | 1.0 normally; 0.5 to 1 while the game thinks the character is held (des_move_post) |
| Wanted speed | mv+0x12C | the step's length over the step time, m/s |

- **The player** is `*(*0x01B3D6E0 + 0x1BC)` (read). Our `player_pos` and `DES_WARP` use it. The game's own Lua binding DisableDamage takes character id 10000 to mean the same pointer (read: 0x00445DA8 calls 0x00425F58, which reads the same global through the TOC).
- **The player check** for every hook: read that pointer (with `guest_ptr_ok` on both reads) and compare. For the movement hook, compare `*(player+0x20)` with the hook's mv.

### 2.2 How the hooks work (read, `ppu_lifter.py`)

- A line `0xADDR=des_dev_hook` in `port/insn_hooks.txt` makes the lifter call `des_dev_hook(ctx, 0xADDR)` right after that instruction. The instruction must not be a branch.
- The hook can change registers. If the instruction it follows is a compare, the hook must set cr7 again after changing the compared register. The lifter's `cmpwi` writes LT 8, GT 4 or EQ 2 into cr7 (`ppu_cr_get(ctx, 7)`; the context keeps one byte per CR field), with no SO bit. Its `fcmpu` does the same, with 1 for unordered.
- A new hook changes `ppu_recomp.h`, so all 14 chunks rebuild once (about 15 to 20 minutes), after a re-lift.
- Several sites can share one host function. `des_dev_hook` switches on the address.

### 2.3 The damage pipeline (read)

A hit goes through the character's damage handler, 0x0028B1D8. It is slot 0x1AC of the character vtables (its descriptor 0x0192D868 sits in four of them, one being the enemy vtable 0x0185AF98 that `DES_WARP` checks). Its arguments are r3 = the target, r4 = the attacker (0 when there is none, for example a trap: the callers at 0x00275484 and 0x002755EC test it for 0 and call its vtable) and r5 = a damage record. The handler calls, in order:

1. **0x00285678, the damage calculation.** It takes the attack's physical, magic and fire values (shorts at rec+0xC, +0xE, +0x10), scales them by 0.01 and the record's rates (rec+0xF4 to +0xFC), and weighs them against the target's defence (vtable+0xC4) and resistances. 0x00223E38 does the formula. 0x00418D70 gives the final HP damage, stored as an int at **rec+0x14C**. A hit at least as big as a threshold sets rec+0x137. The guard's **stamina damage** goes to **rec+0x150**; the guard breaks (rec+0x134) when the target's stamina is not more than that.
2. **0x00280DC8, ApplyDamage.** It is the only place a hit's HP damage reaches the HP. Its one caller is the handler (0x0028B238). It skips HP when the game's flag 0x4 is set or the vtable+0x1E0 check says so, clamps HP to [0, max], and goes to the death branch at 0x00280EB4 when HP reaches 0. It then subtracts the stamina damage at 0x002811F0.
3. 0x00279E98, then 0x0028A5C8 and 0x0027D4E0, then 0x00273248 (the hit reaction and the rest; names not traced).

### 2.4 The hook sites

Each hook runs right after the instruction shown.

| # | Address | Instruction | In | Cheat | What the hook does | Why here |
|---|---|---|---|---|---|---|
| H1 | 0x00280E64 | `lwz r0,0x14C(r28)` | ApplyDamage | god | target r27 is the player, r0 > 0: r0 = 0 | The hit's HP damage, just loaded. `neg`, `cmpwi` and the HP store follow, so HP stays as it was and the rest of the hit (reaction, sound, flags) runs as usual. (read) |
| H2 | 0x00280F88 | `lwz r9,0x150(r28)` | ApplyDamage | god | target r27 is the player, r9 > 0: r9 = 0 | The hit's stamina damage (guarding). With 0 the `ble` after it skips the subtraction. (read) |
| H3 | 0x0028B224 | `or r6,r27,r27` | damage handler | atk | attacker r29 is the player, the record's type rec+0x28 is not 0x40, rec+0x14C > 0: write back rec+0x14C × x (rounded, capped at 10,000,000) | The first instruction after the calculation returns. Attacker, target (r31) and finished damage are all known, and nothing has used the damage yet. The calculation stores rec+0x14C on every path but one: with type 0x40 it returns from 0x00285808 before the store (0x00285AC8), and rec+0x14C keeps its old value, which could be H3's own result from an earlier hit (read, section 3). So H3 leaves type 0x40 alone and only scales a value computed on this hit, which cannot compound. |
| H4 | 0x002818D0 | `lwz r0,0xA0(r1)` | 0x002816D0 | god | chr r31 is the player, r0 > 0: r0 = 0 | The other HP damage path, a damage worked out from a fraction. `neg` and `cmpwi` follow. Its only caller is in the movement code (0x002EBA24), so it is probably the landing damage after a fall (inferred). |
| H5 | 0x00280724 | `cmpwi cr7,r3,0` | 0x002806D8 | god | chr r31 is the player, r3 < 0: r3 = 0, cr7 = EQ | The SpEffect HP change of this update, from 0x00379A88 (read). Poison and toxic go this way (inferred). With EQ the `beq` skips the change. Gains (r3 > 0) stay. |
| H6 | 0x00280C78 | `lwz r9,0x80(r1)` | 0x00280A08 | god | chr r31 is the player, r9 < 0: r9 = 0 | The SpEffect MP change of this update (a percent of max MP plus a flat part, read). `cmpwi` and `beq` follow. Gains stay. |
| H7 | 0x0026FB20 | `cmpwi cr7,r4,0` | AddHP(chr, delta) | god | r3 is the player, r4 < 0: r4 = 0, cr7 = EQ | The first instruction. Script HP changes: the ESD command code (0x00302008) calls it three times, the level-up refresh 0x007E4248 once (read). |
| H8 | 0x0026FC48 | `cmpwi cr7,r4,0` | SetMP(chr, value) | god | r3 is the player, r4 < MP: r4 = MP, cr7 set again against 0 | The first instruction. Every MP loss outside the SpEffect tick goes through SetMP (read: 12 callers, and the store audit below). Spells pay here: a behaviour row adds its MP delta (row+0xC) and stamina delta (row+0x10) through SetMP and SetStamina in 0x00269C98 (read); that this row is the spell's cost is inferred. SetMP still clamps to the max, so a lower max still lowers MP. |
| H9 | 0x0026FC98 | `cmpwi cr7,r4,-50` | SetStamina(chr, value) | god | r3 is the player, r4 < stamina: r4 = stamina, cr7 set again against -50 | The first instruction. Most action costs pay here: the behaviour rows (0x00269C98), the ESD commands (three calls in 0x00302008; the roll's cost is one of them, inferred) and 0x002F8068 (read). Weapon actions pay without SetStamina (H12). |
| H10 | 0x00286614 | `addi r9,r9,-1` | 0x00286020 | god | chr r31 is the player: r9 = stamina (chr+0x3D4) | The dash cost: 1 stamina each time the timer chr+0x144 passes 0.1 s (the constant at 0x019DA2A8, read). The timer adds the frame's step, so this is already rate-independent. |
| H11 | 0x002EDEB0 | `fcmpu cr7,f0,f13` | 0x002EDDA0 | speed | r31 is the player's mv: f0 = f0 × x, cr7 = LT | f0 is the speed factor (mv+0x128) and f13 is 1.0 (read). Only when the factor is below 1 does the code branch to 0x2EE0E0, which multiplies the step vector mv+0xF0 by f0 (read). Forcing LT with the scaled f0 scales every step. mv+0x128 itself is not changed, so des_move_post's stuck test is not disturbed. |
| H12 | 0x0027882C | `lwz r0,0xF4(r1)` | 0x002784C0 | god | chr r28 is the player, r0 > 0: r0 = 0 | A weapon action's stamina cost: a float from the character's vtable+0xAC, made an int just before (read). `subf` takes it off right after, and the result is stored at 0x00278868 without SetStamina (read). With 0 the stamina stays as it was. r26, r27 and r28 all hold the character (read). Callers 0x002AC348, 0x002AC458 and 0x002AF784 (read); that these are attacks and weapon arts is inferred. |

- **ApplyDamage registers (read):** r27 and r26 = target, r20 and r21 = attacker, r28 and r22 = record, r25 = "skip HP" flag (from r7).
- **The handler's registers (read):** r31 = target, r29 = attacker, r28 = record.
- **Threads.** The root-motion step runs from 0x002EE110 (read). The movement worker 0x002A8408 runs this part of the movement update (inferred from des_move_post's comment). The hooks only read guest memory and change the calling thread's registers, so any thread is fine.

**The store audit (read).** I listed every word store to chr+0x3C4 (HP), +0x3CC (MP) and +0x3D4 (stamina) in the game code and sorted each one. No 64-bit store and no store through a computed address writes these fields. This audit found H12, which the first pass missed.

| Kind | Where |
|---|---|
| A hook site, or after one | AddHP (H7), SetMP (H8), SetStamina (H9), ApplyDamage (H1, H2), 0x002806D8 (H5), 0x00280A08 (H6), 0x002816D0 (H4), the dash 0x00286020 (H10), the weapon action 0x002784C0 (H12) |
| Gains | stamina regen (0x002868A0, in 0x00286020; rate × step, never negative as far as I can see, inferred); stamina refilled to max (0x002811A4, in ApplyDamage); the value raised when its maximum rises (0x002802E0, 0x00273620, 0x002737A0, 0x00280500, 0x00280A08) |
| Clamps to a maximum | the same maximum updates; max HP set in 0x0030C870; the stat syncs 0x002C2588, 0x002C3018, 0x002D0258, 0x00447F40, 0x0091B948, 0x0091BB48 and 0x0091BD50, which set the maxima and then clamp |
| HP held at 1 | the flag 0x10 check after each HP change |
| Revive effects | after HP reaches 0, 0x00377BF0 can give HP back (in ApplyDamage, 0x002806D8 and 0x002816D0). Not reached while god mode holds HP. |
| Not hooked on purpose | SetHP 0x0026FAA8, the instant deaths, the event copy and the respawn reset 0x00278020 (section 2.6) |
| Not play | the constructors 0x0028D648 and 0x0028E918; the spawn sync 0x002799E8 (its callers allocate, so spawn is inferred); stack variables; other structures with fields at the same offsets |

So every MP loss goes through SetMP or the SpEffect tick, and every stamina loss through H2, H9, H10 or H12 (read, by this list).

### 2.5 Hook sketch

```cpp
// des_dev.cpp (DES_DEV builds only). One function for every dev hook site.
extern "C" void des_dev_hook(ppu_context* ctx, uint32_t addr)
{
    static int god = 0; static float atk = 1.0f, speed = 1.0f;
    PS3_KNOB(god,   allowed() && env_on("DES_GOD"));
    PS3_KNOB(atk,   allowed() ? knob_f("DES_ATK", 1.0f, 0.0f, 1000.0f) : 1.0f);
    PS3_KNOB(speed, allowed() ? knob_f("DES_SPEED", 1.0f, 0.25f, 10.0f) : 1.0f);
    if (!god && atk == 1.0f && speed == 1.0f) return;
    const uint32_t me = player_chr();            // *(*0x01B3D6E0 + 0x1BC), 0 if none
    if (!me) return;
    switch (addr) {
    case 0x00280E64u: if (god && gpr(27) == me && (int32_t)ctx->gpr[0] > 0) ctx->gpr[0] = 0; break;
    case 0x0026FC98u: if (god && gpr(3) == me && (int32_t)ctx->gpr[4] < (int32_t)vm_read32(me + 0x3D4)) {
                          ctx->gpr[4] = vm_read32(me + 0x3D4); set_cr7(ctx, (int32_t)ctx->gpr[4], -50); }
                      break;
    case 0x002EDEB0u: if (speed != 1.0f && vm_read32(me + 0x20) == gpr(31)) {
                          ctx->fpr[0] = (float)(ctx->fpr[0] * speed); ppu_cr_set(ctx, 7, 8u); }
                      break;
    // ... the other nine sites as in the table
    }
}
```

`allowed()` is the offline rule (section 6). When a knob first leaves its default, the code logs it and sets the session's "cheats used" latch.

### 2.6 What god mode leaves alone

- **Instant deaths.** These functions write HP 0 directly (read): 0x00272C28, 0x00285F40, 0x00286E38, 0x00286FD0, 0x00287120, 0x0027D118, two cases of the event switch 0x002879F8 (at 0x00287B3C and 0x00287BB4) and one case of the event handler 0x002868C8 (at 0x00286C38). 0x00287120 tests 0x001FD3A0 first (inferred: fell out of the level). 0x0027D118 is a grab's kill (inferred); it writes HP 0 only when its last argument is set (read). God mode does not hook them, so a fall into the void, a lethal grab and a scripted death still kill. This keeps scripted deaths from holding up an event. It is a decision for you (section 14).
- **SetHP 0x0026FAA8.** Respawns, loads, Lua and the stat syncs set HP through it (18 callers, read). Not hooked, for the same reason.
- **An event copy.** One case of the character's event handler 0x002868C8 copies four values from chr+0x450 into the 0x3CC to 0x3D8 fields of `*(*(chr+0x20)+0x20)` (read). It looks like a restore after an event (inferred). Not hooked.
- **Lower maxima.** When max stamina drops, 0x00280DBC clamps stamina to it (read). SetMP clamps MP to the max. These follow the maximum, not play.

### 2.7 Rejected approaches

- **Writing HP, MP and stamina every frame.** You found that HP writes don't stick. The per-frame update 0x00280A08 recomputes the maxima each time (0x002802E0, 0x002737A0, vtable+0x9C; read). A refill also comes too late: a big hit takes HP to 0 and goes to the death branch in the same call (0x00280EB4, read).
- **The game's own "no damage" flag (chr+0x310 bit 0x4).** ApplyDamage honours it (read). But scripts own it: Lua DisableDamage sets it through 0x002707D8 (read; that 0x002707D8 sets this bit is inferred). Turning it off could undo a script's setting. It also covers neither MP, stamina nor speed, so a re-lift is needed anyway.
- **Attack power in ApplyDamage's register.** The attacker is known there too (r20), but only the HP store would see the change. Writing rec+0x14C back in the handler (H3) lets everything after the calculation see the same number.
- **Speed through animation speed.** That would also speed up attacks and recovery. A different cheat; not planned.

## 3. The attack damage calculation (read)

For reference, if you want the multiplier earlier in the chain:
- 0x00285678 computes the three element parts as `attack × scale × 0.01 × rate` and hands them with the defence values to 0x00223E38. The result lands at rec+0x15C.
- 0x00418D70 (a global manager at `*(TOC-0x7E94)`, the target and the record) gives the final HP damage, stored at rec+0x14C as an int (0x00285AC8).
- **One early exit.** The calculation keeps the record in r31 (from r5). After 0x00285430 and 0x002851E8 it loads the record's type (rec+0x28, 0x002857FC) and, when it is 0x40, branches from 0x00285808 straight to the epilogue at 0x00285AE4. That path skips the store at 0x00285AC8, so rec+0x14C keeps whatever the record held. Every other path reaches 0x00285950 and the store. Its only other branch to the epilogue (0x00285AD8, the threshold test for rec+0x137) comes after the store. Neither the calculation nor the nine functions it calls directly reads rec+0x14C or stores to +0x28 (read, in the lift; its indirect calls were not followed). The type comes from the record builder's argument; which hits use 0x40 is not known yet (section 15).
- H3 scales the final number. Scaling the attack values instead would interact with defence non-linearly. I recommend the final number.

## 4. The build gate

- **CMake (`port/CMakeLists.txt`).**
  ```cmake
  # Developer builds (docs/55): the dev cheats and the settings window's Developer tab.
  # Never for a release. Build them in a folder of their own, such as port/build_dev.
  option(DES_DEV "Developer build: dev cheats and the Developer settings tab (never in a release)" OFF)
  if(DES_DEV)
      target_sources(des_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/des_dev.cpp")
  endif()
  target_compile_definitions(des_boot PRIVATE DES_DEV=$<BOOL:${DES_DEV}>)
  ```
  `port/ui/CMakeLists.txt` adds the same definition to `des_ui`, with `DES_DEV` OFF when the UI is built alone and the option is not set.
- **One macro.** `DES_DEV` is always defined, as 0 or 1. Code uses `#if DES_DEV`.
- **Files.**
  - `port/des_dev.cpp` (new, dev only): the hooks, the knobs, the offline rule, `DES_DEVLOG`, and later `DES_WARP`.
  - `port/des_dev_marker.h` (new): `#define DES_DEV_MARKER "DES-DEV-BUILD: developer cheats compiled in"`. Used only inside `#if DES_DEV`.
  - `port/des_options.cpp`: in a release build, `extern "C" void des_dev_hook(ppu_context*, uint32_t) {}`. `ps3_title_options_init` calls `des_dev_boot()` inside `#if DES_DEV`.
  - `port/ui/src/settings.cpp`, `settings_window.cpp`: the Developer tab and its keys inside `#if DES_DEV`.
- **One lift for both builds.** The 12 hook lines are always in `port/insn_hooks.txt`. Release builds link the empty `des_dev_hook`. So switching `DES_DEV` recompiles a few files and needs no re-lift. The cost in a release is one empty call per hit and per character step (inferred: not measurable). The alternative, a separate dev hook list, is in section 14.
- **Neutral comments in the hook list.** The release ships `insn_hooks.txt` for the player's own lift (docs/52). Its comments must not name the knobs, so they say "dev hook: hit HP damage" and point here.
- **The marker stays in the binary.** `des_dev_boot()` prints it at start (`[dev] DES-DEV-BUILD: ...`), so the linker keeps it. The UI's Developer tab shows it too.
- **Scripts fail fast.** A test script that sets a cheat knob checks the log for the `[dev]` line and stops with a clear message if the exe is not a dev build. Otherwise the knob would silently do nothing.
- **A folder of their own.** Developers configure a second build folder, `port/build_dev`, once with `-DDES_DEV=ON`; CMake keeps the option in that folder's cache. Never `port/build`: `tools/des/build.sh` and `play.sh` use it, and every later build there would have the cheats in it. The README's build section says how, and how to run such a build (`DES_EXE=port/build_dev/des_boot.exe`).
- **Hard to miss.** A configure with `DES_DEV=ON` prints a CMake warning, and a second one when the folder is `port/build`. A later `cmake --build` does not configure again, so each link of `des_boot` also prints "DEVELOPER BUILD (DES_DEV=ON)". The running game says "(developer build)" after the title in its window caption (`des_dev_boot` sets `PS3_TITLE` before the window opens) and prints the marker at the top of its log.

## 5. "dev" in the version

- **Today (master).** `port/ui/CMakeLists.txt` sets `DES_VERSION` to the short commit (read). A dev build would append `-dev`: `b73d2e1-dev`.
- **With lane 4's `DesVersion.cmake`** (committed on lane 4's branch `feat/version-display` as 2b519b0, not merged; read): a release shows `0.1.0`, an untagged build `0.1.0-dev.<n>+g<commit>`, a build after the tag `0.1.0+<n>.g<commit>`, with `.dirty` for local changes.
  - Proposal: a `DES_DEV` input to `des_version_write()`. When it is ON, append the build-metadata word `dev` by the same rule as `.dirty` (`.dev` when the string already has a `+`, else `+dev`): `0.1.0+dev`, `0.1.0-dev.12+g1a2b3c4d5.dev`, `0.1.0+3.g1a2b3c4d5.dirty.dev`.
  - `des_version.h` gets `#define DES_DEV_BUILD 1`.
  - The exe's version resource gets `VS_FF_PRIVATEBUILD` (0x8) and `PrivateBuild` = the marker text. Explorer then shows it, and the scanner finds it there too (the resource is UTF-16).
  - About and the issue report add a line "Developer build (cheats compiled in)".
- **A naming clash.** Lane 4 already uses "-dev" for untagged builds, so "dev" alone does not tell a cheat build from an ordinary one. The About line and the report line do. A choice for you (section 14).

## 6. The offline rule

Cheats must never reach other players. Today the port is offline unless `PS3_NET_ONLINE` is set (docs/46). Multiplayer is planned (docs/46 stages 0 to 2), so the rule is built in now.

- **Decided at start, then checked live.** `des_dev_boot()` runs at the start of `ps3_title_options_init`. That is before `ppu_hle_init` and `ppu_sysprx_register` (read, `boot_main.cpp` lines 2085 to 2090), so before the network modules look at their knobs. The `--psnr` option is handled earlier (line 1984) and only sets the same environment variables (read).
  - **A cheat is on at start:** clear `PS3_NET_ONLINE`, `PSNR_SERVER` and `PS3_NET_HOSTS` with `ps3_setenv(name, "")`. The getenv cache stores an empty value as unset (read, `cached_setenv`). Then the host sockets are never registered (`ppu_sysprx.cpp` line 902), cellNetCtl reports no connection (`netctl_online`), and NP stays offline (`np_psnr_enabled`), all read. The session is a cheat session.
  - **The network is on at start (`PS3_NET_ONLINE` or `PSNR_SERVER`) and no cheat is:** an online session. Every cheat stays off until the next start. `allowed()` returns false, the knobs are ignored with one log line, and the Developer tab says why.
  - **Neither:** an offline session. A cheat can be turned on at any time. The first one sets the "cheats used" latch for the rest of the session.
- **The UI learns the session** from `DES_DEV_SESSION` (cheats, online or offline), which `des_dev_boot()` sets. No new link between the UI library and the port.
- **The hook point for future network code.** One function, `des_dev_cheats_used()`, true from the first cheat to the end of the session. docs/46's Network section must show Online as off while it is true. Release builds have no cheats, so there it is always false.
- **Checked again where the cheats act.** In an offline or cheat session, the debug console can still set `PS3_NET_ONLINE` and `PSNR_SERVER` again, and cellNetCtl and NP read them live (read: `np_psnr_connect` reads them at each context start). So every cheat hook and `DES_WARP` checks the two knobs before it acts (any value counts, as in ps3recomp), and `des_debug_tick` checks them once a frame after any `setenv`. The first time either is seen set, the session is online until the next start: the cheats do nothing, one log line says so (and whether cheats were used before), and `DES_DEV_SESSION` becomes `online`, so the Developer tab shows the online message.
- **What is left until ps3recomp has a lock.** When the knobs are set late, ps3recomp may still connect: the cheats are off by then, but what they changed stays (souls, items), and a knob set and cleared again between two frames, with no cheat acting meanwhile, is not seen. The host sockets would still be missing, because they are registered only at start. To close it, ps3recomp gets a sticky `ps3_net_lock_offline()`: once called, `netctl_online()`, `np_psnr_enabled()`, the sys_net host sockets and cellHttp's resolver all stay offline. The port calls it at start in a cheat session and at the latch. About half a day, best done with docs/46 stage 0, which touches the same code. `des_dev_cheats_used()` has no caller yet either; the network code of docs/46 is to call it.
- **Saves.** A save made with cheats keeps what was gained (souls, items). The rule protects the session, not the save. Marking such saves is a question for you (section 14).

## 7. The CI check

- **What it looks for.** In every file of the release: `DES-DEV-BUILD`, `DES_GOD`, `DES_ATK`, `DES_SPEED` and `DES_WARP`, each in ASCII and in UTF-16LE. Zip, tar, gzip, bzip2 and xz files are opened, nested up to 8 deep, zip self-extractors included. Text documents (`.md`, `.txt`, `.html`) are not searched, because docs may name the knobs. None of these strings appear in master's sources today (read).
- **It fails closed.** A file it cannot look inside fails the check (exit 3, each file listed) instead of passing: other archives (7z, RAR, CAB, MSI, zstd, lz4, ISO, DMG, xar, squashfs, AppImage and the like, by their first bytes or their name), installers (NSIS, Inno Setup, WiX bundles, an executable with a 7z, RAR, CAB or MSI inside), executables packed with UPX and similar packers (by their section names), archives nested deeper than 8, encrypted or damaged archive members, and files it cannot read. Such a file's bytes are still searched. After checking one another way (unpacked), `--skip PATTERN` leaves it out; each skipped file is listed.
- **Where it runs.** Nowhere by itself yet: there is no release workflow (section 8). Run it by hand on any build before sharing it: `python tools/ci/check_release_no_dev.py <folder or zip>`. Exit 0 means nothing found, 1 dev build code found (each hit with its file and offset), 3 files not checked, 2 a usage error.
- **A self-test.** CI builds `des_ui` alone with `-DDES_DEV=ON` (it needs no game files) and runs the scanner with `--expect-dev`, which must find the marker. The same library built with OFF must pass. That proves the scanner works and the gate holds. Not set up yet.
- **Tested** on 30 small sample files (each format above, zips nested 8 and 9 deep, a zip self-extractor, an encrypted member, `--skip`, `--expect-dev`) and on the developer and release `des_boot.exe` of this branch (section 15).

## 8. What lane 4 must do

1. **Release workflow:** configure the host with `-DDES_DEV=OFF` explicitly, never from a cache. Print the value in the log, and fail if the cache says ON.
2. **Player setup (docs/52):** the player's PC compiles only the lift. Its compile driver never defines `DES_DEV=1`. The lift's `des_dev_hook` calls resolve to the prebuilt host's empty function. If port sources are ever compiled on the player's PC, the driver passes `-DDES_DEV=0`.
3. **Add the scanner** as the last step before upload (section 7).
4. **Add the self-test job** (section 7).
5. **`DesVersion.cmake`:** take a `DES_DEV` input. Append the `dev` build metadata, define `DES_DEV_BUILD`, and set `PrivateBuild` in the version resource (section 5).
6. **Second guard:** a release's version string must not carry the dev metadata. A tagged release is plain `x.y.z`, so the check is simple.
7. **Merge order:** whichever PR merges second (lane 4's version PR or the cheats PR) wires `DES_DEV` into `DesVersion.cmake`. Both stay based on master.

## 9. Settings: a Developer tab

Only in dev builds (`#if DES_DEV` in `settings.cpp` and `settings_window.cpp`). It goes between Interface and About.

| Row | Kind | Key | Choices | Help text |
|---|---|---|---|---|
| Developer build | info | none | "Cheats compiled in" | "This is a developer build (DES-DEV-BUILD). The cheats below are for testing. While one is on, this session stays offline. They are not saved: each start begins with all of them off." |
| God mode | on/off | `DES_GOD` | Off, On | "Your HP, MP and stamina never drop: hits, falls, poison, spells, attacks, rolls, guarding and dashing cost nothing. Instant deaths still kill: falling out of the level, lethal grabs and scripted deaths." |
| Attack power | choice | `DES_ATK` | Normal (1), 2x, 5x, 10x, 100x, No damage (0) | "Multiplies the damage your hits do to others. Any other number can be set with DES_ATK." |
| Movement speed | choice | `DES_SPEED` | Normal (1), 1.5x, 2x, 3x, Half (0.5) | "Multiplies how far you move: walking, running, dashing, rolling and attack lunges. Animations keep their speed. Very high values may let you pass through thin walls." |
| Warp (scripts) | info | none | "DES_WARP=x,y,z" | What `DES_WARP` and `DES_WARP_CLEAR` do (section 10). |

- **Not saved.** A new `Apply::Session` kind: live like `Apply::Live`, but `load_and_apply_env` skips it when reading `settings.ini` and `save()` does not write it. `describe()` still lists it, so an issue report from a dev build shows which cheats were on.
- **The environment wins**, as for every row: a test script's `DES_GOD=1` shows the row as set from outside and disabled (existing `pinned` logic, read).
- **Online session.** When `DES_DEV_SESSION` is `online` (from the start, or since the network was turned on), the tab shows only one info row: "This session is online (PS3_NET_ONLINE or PSNR_SERVER is or was set), so the cheats are off until the next start. Start the game without them to use the cheats." The tab is built again each time it is shown, so it follows the session.
- **Rows use the existing helpers** `bool_row`, `choice_row` and `info_row` (read). The tab's strings carry the marker, which the CI self-test relies on.

## 10. DES_WARP behind the gate

`tools/des-warp` (unmerged, read) adds `des_debug_tick` to `des_options.cpp` and a call in `des_menu_sfx.cpp`. Whichever of the two PRs merges second moves `des_debug_tick` and its helpers into `des_dev.cpp`. The call goes inside `#if DES_DEV`. A warp also counts as a cheat for the offline rule. The ledge, camera and turn test scripts that use `DES_WARP` then need a dev build, and check for the `[dev]` log line.

## 11. The run request

One play session, after the cheats PR builds a dev exe. The cheats and the frame rate are switched live in the settings.

Command (the exe path is set when the PR exists):
`& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 DES_EXE=<dev build>/des_boot.exe DES_DEVLOG=1 DES_FRAMELOG=work/tmp/cheats_framelog.txt"`

Do this at Frame rate 30, then again at 240. Press F8 at the start of each step (it writes a MARK line).

1. **All off.** Take one hit, roll twice, attack twice, block one hit, dash for 5 s, cast one spell if the character has one, and kill one enemy.
2. **God mode on.** The same actions. The HP, MP and stamina bars must not move.
3. **God off, Attack power 10x.** Hit an enemy. A soldier should die in one hit. Then **No damage**: hits do nothing. Then Normal.
4. **Movement speed 2x.** Dash 5 s on flat ground. Then Normal.

What I check afterwards:
- `[dev]` lines in `work/tmp/play.txt`: the session line at start, then one line per blocked loss or scaled hit, with the site.
- The framelog: the stamina column flat under god mode; the `SUM` speed about twice step 1's dash (about 6.4 m/s, so about 12.8) at both 30 and 240 fps. The PR also adds HP and MP columns at the end of each framelog line, so god mode can be checked there too.
- That step 1's numbers look the same as without the dev build (cheats off really is off).

Open points this run settles: whether spell hits and arrows pass the player as the attacker (H3); whether 0x002816D0 is fall damage (H4); that the roll, attack and spell costs really stop (H9, H12, H8; the store audit says they must). If a bar still drops under god mode, the `[dev]` lines show which sites ran, and the missing path is the next thing to find.

## 12. Effort

| Step | Days |
|---|---|
| `des_dev.cpp`: 12 sites, knobs, latch, `DES_DEVLOG`, framelog HP and MP | 0.5 |
| Gate: CMake option, macro, release stub, marker header, version suffix on master's scheme | 0.25-0.5 |
| Settings: `Apply::Session`, the Developer tab, the online message | 0.5 |
| Re-lift, a full dev build and a release build, the scanner on both | 0.25 (about 20 minutes a build) |
| The run and fixes from it | 0.5-1 |
| `DES_WARP` behind the gate | 0.1 |
| Lane 4: `DesVersion.cmake` input, scanner step, self-test job | 0.5-0.75 |
| ps3recomp `ps3_net_lock_offline()` (with docs/46 stage 0) | 0.5 |
| **Total** | **about 3-4** |

## 13. Risks

- **Enemies run the same code.** Every hook compares with the player first. Without that, god mode would make enemies immortal too.
- **Scripted deaths.** God mode does not touch the instant-death functions or SetHP. If a script ever kills through AddHP, god mode would stop it and could hold up that event (inferred: unlikely). The pinned save cannot show this. A note for anyone testing the tutorial's end with god mode on.
- **Lethal grabs and falls into the void still kill** (section 2.6). By design, but you may expect otherwise.
- **Spells and arrows.** If the damage handler gets no attacker for projectile hits, `DES_ATK` does not apply to them (inferred). The run checks it.
- **Speed.** The feet slide. At x10 and 30 fps a dash step is about 2.1 m, so the physics proxy may pass through thin walls (inferred). Below about x0.2 a dash drops under the game's own 1 m/s stuck test, and the factor halves on top (read, des_move_post). Hence the 0.25 to 10 range.
- **The lift changes.** 12 new hook calls, one full rebuild. Release builds call an empty function at those places.
- **What is left of the console gap** in the offline rule, until the ps3recomp lock exists: the cheats stop when the network knobs are set late, but ps3recomp may still connect (section 6).
- **Saves.** Gains made with cheats stay in the save (section 6).
- **False alarms in CI.** The knob names must not appear in release binaries or in the shipped hook list. Comments in `insn_hooks.txt` stay neutral (section 4).

## 14. Decisions for you

The build took the recommended answer to questions 1 to 7 and 9. Question 8 is still open.

1. **Instant deaths under god mode:** leave them alone (recommended: falls into the void, lethal grabs and scripted deaths still kill), or block them too (risk: stuck events).
2. **Save the cheats in `settings.ini`?** Recommended: no, they are per session.
3. **One hook list with an empty release function** (recommended: one lift for both builds, switching is quick) **or a separate dev hook list** (a release lift without the dev calls, but every switch needs a re-lift, about 20 minutes).
4. **The version word.** Lane 4's untagged builds already say "-dev". Options: (a) keep that and add `dev` build metadata plus the About and report lines (recommended); (b) lane 4 uses "-pre.N" for untagged builds, and "dev" means a cheat build only; (c) a different word for cheat builds, for example "devtools".
5. **Attack power on the final damage** (recommended) or on the attack values before defence.
6. **Speed through root motion only** (recommended) or also faster animations.
7. **`DES_WARP` in the tab:** script-only for now (recommended), or "save spot" and "return to spot" buttons later.
8. **Mark saves touched by cheats** (a small list next to the saves) so future online play can refuse them?
9. **Online session:** cheats refused until the next start (recommended), or the connection dropped when a cheat is turned on.

## 15. As built (2026-10-19)

The build follows sections 1 to 11, with these differences:

- **The `DES_DEV` define is a source property** of the three port files that use it (`des_options.cpp`,
  `des_menu_sfx.cpp`, `des_dev.cpp`), not a definition on the whole `des_boot` target as section 4 shows. A target-wide
  definition is part of every lifted chunk's command line, so switching it would rebuild all 14 chunks (15 to 20
  minutes). As a source property, a switch rebuilds those files and `des_ui`, which takes the definition target-wide (it
  is small).
- **`des_dev_boot()` runs right after `des_ui_early_init()`**, not at the very start of `ps3_title_options_init`. By then
  `settings.ini` has been applied, so the decision sees the final network knobs. It is still before `ppu_hle_init`, so
  before the network modules start.
- **No version suffix yet.** Lane 4's version PR (port #62) replaces how the version is made (`port/cmake/DesVersion.cmake`
  writes `des_version.h`), so a `+dev` set in `port/ui/CMakeLists.txt` would conflict with it and be lost. The suffix
  waits for #62; then `DesVersion.cmake` gets a `DES_DEV` input (section 8, step 7). Until then a developer build shows
  itself by the marker: the first row of About, the first line of the issue report's system section, and the start log.
- **The release stub** in `des_options.cpp` also defines `des_dev_cheats_used()` (always 0), so future network code can
  call it in any build.
- **`DES_WARP`** was merged as port #59 before this build. Its code moved into `des_dev.cpp` unchanged, apart from the
  offline rule: a warp is refused in an online session and sets the latch otherwise. Its call in `des_menu_tick` is inside
  `#if DES_DEV`. A release build ignores `DES_WARP`. No script in the repo uses it.
- **The Developer tab's help texts** end with the knob name in brackets, as the other rows do.
- **The frame log** (`DES_FRAMELOG`) gained HP and MP columns at the end of each line, in a separate tooling commit.
- **The scanner** is `tools/ci/check_release_no_dev.py`. No workflow runs it yet (lane 4, section 8).
- **Not done:** ps3recomp's `ps3_net_lock_offline()` (what is left of section 6's gap stays until then), the `+dev`
  version suffix (above), the version resource's `PrivateBuild` field, the CI self-test job, and lane 4's steps.

### First run (2026-10-19)

Built from master `d408341` with this branch, without `/Zi`: once with `-DDES_DEV=ON`, then with `OFF` in the same build
folder. The switch to `OFF` rebuilt 37 files (the three port files and `des_ui`) in 9 seconds, and both linked. The
scanner passed the release exe and found the developer exe (the marker and the four knob names in the exe).

Four boots on the pinned regression save (the Tutorial), 30 fps, the cheats switched with the debug console's `setenv`:

| Boot | Exe and knobs | What it did | Result |
|---|---|---|---|
| m | developer, `DES_DEVLOG=1` | two backsteps and two R1 attacks with everything off, then the same with God mode on; Movement speed 1 and 2 on a straight walk; a dash with God mode on, then off | all off: stamina 95, lowest 75. God mode: stamina stayed at 95, the latch line and the hook sites H9 and H12 logged. Speed: the step ratio over 44 frames was 1.001 at 1 and 2.003 at 2 (2.000 times the first). Dash with God mode: stamina stayed at 95; off again: 95 down to 83 |
| s | developer, `DES_GOD=1` at start | the same actions | stamina stayed at 95 from the first frame; the log called it a cheat session with the network knobs cleared |
| r | release, `DES_GOD=1` | the same actions | stamina 95, lowest 75 (the knob is ignored), and no `[dev]` lines |
| u | developer, at the title | the Developer tab and About by keys, God mode switched on in the tab | 7 of 7 screenshots; `settings.ini`, written after the switch, holds no cheat key |

Not run yet: Attack power in a fight; God mode against hits, falls, poison and spells; the online refusal with a real
connection (unattended runs stay offline); a pad or keyboard in the Developer tab (the session was locked). Section 11's
run request covers them.

### Review fixes (2026-10-19)

A review of the PR found four things, fixed in separate commits:

- **H3 could compound.** Its comment said the calculation rewrites rec+0x14C on every hit. It does not: a record of type
  0x40 skips the store (section 3), so with `DES_ATK=10` such a record could have its earlier, already scaled damage
  scaled again on each pass. H3 now leaves type 0x40 alone, so it only ever scales a value the calculation stored on
  this hit. With `DES_DEVLOG=1` a skipped record logs "H3 hit damage left as it was (record type 0x40)".
- **The offline rule only looked at start.** It now also checks the network knobs where the cheats act and after each
  `setenv` (section 6).
- **A cached `DES_DEV=ON`.** The README sent developers to configure `port/build` itself with `-DDES_DEV=ON`; CMake
  caches the option, and `tools/des/build.sh` only builds `port/build`, so every later build there would have had the
  cheats. Developer builds now go in `port/build_dev`, and a developer build says so at configure, at each link and in
  its window title (section 4). The CMake comment and the README no longer mention a CI check, a release workflow or a
  player setup, which do not exist yet.
- **The scanner passed what it could not open.** It now fails closed and takes `--skip` (section 7).

Built without `/Zi` in the lane's build folder: `-DDES_DEV=ON` (38 files rebuilt; the configure printed the new warning,
the link printed the developer-build line), then `OFF` (37 files; neither printed). Both linked with no compiler
warnings. The scanner passed the release exe and found the developer exe. The release exe has none of the new strings.

Not run yet: whether the player's hits ever use type 0x40 records (the `DES_DEVLOG` line above shows it); the live
refusal (set `PS3_NET_ONLINE=1` with the debug console while God mode is on: one `[dev] PS3_NET_ONLINE is set` line,
then God mode stops, and the Developer tab shows the online message); the window title.

## Appendix A. The proposed hook lines

```
# Dev hooks (docs/55). Every line calls des_dev_hook, which is empty in release builds.
0x00280E64=des_dev_hook  # dev hook: hit HP damage (ApplyDamage 0x00280DC8)
0x00280F88=des_dev_hook  # dev hook: hit stamina damage (ApplyDamage)
0x0028B224=des_dev_hook  # dev hook: hit damage after the calculation (handler 0x0028B1D8)
0x002818D0=des_dev_hook  # dev hook: fraction damage (0x002816D0)
0x00280724=des_dev_hook  # dev hook: SpEffect HP change (0x002806D8)
0x00280C78=des_dev_hook  # dev hook: SpEffect MP change (0x00280A08)
0x0026FB20=des_dev_hook  # dev hook: AddHP
0x0026FC48=des_dev_hook  # dev hook: SetMP
0x0026FC98=des_dev_hook  # dev hook: SetStamina
0x00286614=des_dev_hook  # dev hook: dash cost (0x00286020)
0x002EDEB0=des_dev_hook  # dev hook: root-motion factor (0x002EDDA0)
0x0027882C=des_dev_hook  # dev hook: weapon action stamina cost (0x002784C0)
```

None of these addresses is in master's hook list today (read).

## Appendix B. Key disassembly (read)

ApplyDamage, the HP part:
```
00280E64  lwz    r0,0x14C(r28)    ; H1: the hit's HP damage
00280E68  neg    r31,r0
00280E6C  cmpwi  cr7,r31,0
00280E70  blt    cr7,0x2810B0     ; damage: flag 0x4 and vtable+0x1E0 checks first
00280E74  lwz    r0,0x3C4(r27)    ; HP
00280E80  add    r11,r31,r0       ; then clamped to [0, max HP]
00280EB0  stw    r9,0x3C4(r27)
00280EB4  ble    cr7,0x281520     ; HP 0: the death branch
...
00280F88  lwz    r9,0x150(r28)    ; H2: the hit's stamina damage
00280F8C  cmpwi  cr7,r9,0
00280F90  ble    cr7,0x280FB8
```

The damage handler:
```
0028B220  bl     0x285678         ; the damage calculation
0028B224  or     r6,r27,r27       ; H3
0028B238  bl     0x280DC8         ; ApplyDamage
```

The root-motion step:
```
002EDEA8  lfs    f13,-0x8000(r30) ; 1.0
002EDEAC  lfs    f0,0x128(r31)    ; the speed factor
002EDEB0  fcmpu  cr7,f0,f13       ; H11
002EDEB4  blt    cr7,0x2EE0E0     ; below 1: mv+0xF0 *= f0, then back to 0x2EDEB8
```

The dash cost:
```
002860EC  lfs    f13,0x144(r31)   ; the timer, plus the step
00286100  fcmpu  cr7,f13,f0       ; 0.1 s
00286608  lwz    r9,0x3D4(r31)    ; stamina
00286614  addi   r9,r9,-1         ; H10
00286658  stw    r0,0x3D4(r31)    ; after the clamp to [-50, max]
```

The weapon action cost:
```
00278804  bctrl                   ; vtable+0xAC: the cost, a float
00278810  fctiwz f1,f1            ; made an int, stored at 0xF4(r1)
00278818  lwz    r9,0x3D4(r28)    ; stamina
0027882C  lwz    r0,0xF4(r1)      ; H12: the cost
00278830  subf   r9,r0,r9
00278868  stw    r0,0x3D4(r28)    ; after the clamp to [-50, max]
```
