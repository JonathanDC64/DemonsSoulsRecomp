# Game-side features: 8-way rolls, respec, souls, PvP arena (spike, 2026-10-18)

A docs-only spike. Nothing was built or run. Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`), the release the port uses. Game data was read from the game's own files under `PS3_GAME/USRDIR`: `param/gameparam/*.param` with `paramdef/`, the player's script `chr/c0000/c0000.esd`, the player's animations `chr/c0000/c0000_a0x.anibnd`, and `msg/na_english/item.msgbnd`. Nothing from them is copied into the repo beyond the numbers quoted here.

Two labels are used below:
- **(read)**: I read it in the disassembly (`ps3recomp/tools/ppu_disasm.py --va`), in a param file or in the player's script. Constants were read from the ELF.
- **(inferred)**: my conclusion, not yet checked in a run. Section 6 is one batched run request that checks the open points.

> **Run r4 (2026-10-19) did section 6's steps 2 to 4** (section 3.6). Section 3.2's stick-to-slot reading is right. Its slot-to-move columns are wrong for all five slots. chr+0x310's bit 0x800 is not the lock flag. The sprint timer runs while locked on.
>
> **Built (2026-10-25): rolls toward the stick while locked on**, `DES_ROLLDIR`, on by default (section 3.7).
>
> **Built (2026-10-27): several soul items at once**, `DES_SOULS_AT_ONCE`, off by default (section 2.5). It is approach C: Use asks how many, in Drop's own count picker.
>
> **Built (2026-10-28): stat respec at the Nexus statue**, `DES_RESPEC`, off by default (section 1.9). The statue in front of the Nexus spawn opens a shop-style menu; Respecialization puts the class's stats back, refunds the levels' souls and opens the game's level-up screen.

## Summary

| Feature | Verdict | Effort |
|---|---|---|
| Stat respec | **Built** (section 1.9): `DES_RESPEC` makes the statue in front of the Nexus spawn open a menu whose Respecialization resets to the class, refunds the levels and opens the level-up screen. | Done. No on-screen prompt at the statue yet. |
| Use several souls at once | **Built** (section 2.5): `DES_SOULS_AT_ONCE` makes Use on a soul stack ask how many, in Drop's count picker. | Done. |
| 8-direction rolls while locked on | **Built** (section 3.7): `DES_ROLLDIR` plays the roll the game picks and turns the character toward the stick, so a roll goes where the stick points. | Done. True diagonal roll animations: not feasible (new animation content). |
| 8-direction walking while locked on | **Probably nothing to do.** The walk already twists the legs toward the movement (inferred). | 0, unless the run shows otherwise. |
| PvP arena | **Not feasible as a new mode.** The game has no arena. Online duels and invasions come with docs/46 stage 2. | 8-16 days through docs/46, after its stages 0 and 1. A new arena mode: weeks, high risk. |

- **Respec.** Function 0x0090FCD0 resets the eight stats to the class row, recomputes HP, MP and stamina, clears the level-up count and the attuned spells. It does not touch souls or items (read). A respec is: add up the souls the levels cost (0x00226778 per level), call the reset, write the souls back, then call the game's own after-level-up refresh 0x007E4248 for the live character. From SL 620 up, the refund alone is more than the 999,999,999 souls cap, so very high levels need a rule (refuse, or clamp).
- **Several souls.** The eight "<rank> Soldier's/Hero's Soul" items (ids 1025-1032) are worth 200 to 10,000 souls each. One use plays the crush animation, then AddSoul 0x0091B888 adds the amount (read). "Use all" can sum the stacks, remove them and call AddSoul once. Boss souls stay one at a time: they also trade for spells and weapons.
- **8-way rolls.** The stick is quantized to four directions in one place, the player's pad manipulator 0x0030E750. The script then maps five input slots to four roll states and the backstep (read). My reading of which stick direction lands in which slot contradicts the known neutral-tap backstep, so one link is misread (section 3.3). The run settles it. Run r4 did (section 3.6): the stick-to-slot reading is right and the slot-to-move link is the misread one. A diagonal starts the forward or back roll. The plan does not depend on it: let the game pick its roll, then turn the character toward the diagonal for that roll.
- **PvP.** Local split screen is impossible (one local player). An offline duel against an AI phantom is content modding, weeks of work. Online PvP is docs/46 stage 2.
- **All of it opt-in.** Each feature is a knob that is off by default, so the default stays the original game (`DES_RESPEC`, `DES_SOULS_ALL`, `DES_ROLL8` are suggested names). The rolls are the exception: they were built as `DES_ROLLDIR`, on by default by the user's choice (section 3.7).
- **No RPCS3 patch covers any of this.** RPCS3's patch file for BLUS30443 has no lock-on, roll, stat or soul patches (read).

## 1. Stat respec

### 1.1 Where the stats live (read)

The player's game data is `PGD = *(*0x01B4EF9C + 8)`. The character also points at it (chr+0x49C).

| PGD offset | Field |
|---|---|
| +0x0C / +0x10 / +0x14 | HP current / max / base |
| +0x18 / +0x1C / +0x20 | MP current / max / base |
| +0x28 / +0x2C / +0x30 | stamina current / max / base |
| +0x38 / +0x3C | Vitality (two copies, written together by the reset) |
| +0x40 / +0x44 | Intelligence |
| +0x48 / +0x4C | Endurance |
| +0x50 / +0x54 | Strength |
| +0x58 / +0x5C | Dexterity |
| +0x60 / +0x64 | Magic |
| +0x68 / +0x6C | Faith |
| +0x70 / +0x74 | Luck |
| +0x78 | souls held (s32) |
| +0x80 | total souls gained (s64; AddSoul adds every gain) |
| +0x88 | level-ups bought |
| +0xB6 | class (halfword); its CharaInitParam row is 1000 + class |
| +0xD9, +0xDA | soul-penalty flags |
| +0x128 | inventory |
| +0x1F4 | item list (inventory +0xCC) |
| +0x498 | item-weight limit, float. Add-item 0x008FB4F0 checks new items against it, and the level-up screen shows it. |

### 1.2 Soul level and the cost of a level (read)

- **Soul level:** 0x00224B10 returns the sum of the eight stats minus 80, clamped to 0..999. The highest SL is 712 (all stats 99).
- **Cost of one level:** 0x00226778(x), where x is the stat sum after the raise. It reads CalcCorrectGraph row 200 (0.1, 1.0, 0.02, 92) and a constant 2.0. That works out to `cost(x) = R((0.02 * max(0, x - 92) + 0.1) * x^2 + 1)`. R is the float helper 0x0094E508; that it rounds to the nearest soul is inferred from the costs below.
- **Checks (computed):** SL 1 to 2 costs 673, 2 to 3 costs 690, 3 to 4 costs 707, 11 to 12 costs 847. SL 1 to 120 costs 3,506,802 in total. These match the community wiki's table (Fextralife, seen through a web search). For SL 12 and up, the formula above is exactly the wiki's `y = 0.02x^3 + 3.06x^2 + 105.6x - 895`, with x the new level.
- **Single levels (computed):** SL 30 to 31: 5,915. SL 50 to 51: 15,103. SL 100 to 101: 61,592. SL 120 to 121: 92,115. SL 200 to 201: 306,370. SL 711 to 712: 8,844,423.
- **Float, not double.** The game casts the sum to float before rounding (read, in the decompile). A float simulation of it differs from my double numbers at 56 stat sums, the first at 341. A respec must call 0x00226778 for each level instead of re-implementing it.
- **Refund from a SL 6 class (computed in double; the float simulation gives the same numbers up to SL 200 and 54 souls more at SL 712):**

  | Back from SL | Souls |
  |---|---|
  | 50 | 258,958 |
  | 100 | 1,994,386 |
  | 120 | 3,503,267 |
  | 150 | 7,108,989 |
  | 200 | 18,250,267 |
  | 619 | 998,656,971 |
  | 620 | 1,004,664,372 (the first SL whose refund alone passes the cap) |
  | 712 | 1,683,684,272 |

  The souls cap is 999,999,999 (the constant at 0x016FF700). Below SL 620 the refund can still pass the cap when the player already holds many souls. A community estimate of about 1.6 billion souls from the start to SL 712 agrees (CyberPost, seen through a web search).

### 1.3 Classes (read)

CharaInitParam rows 1000-1009, read by the accessor 0x00597108 and the getter 0x00597388.

| Row | Class | VIT | INT | END | STR | DEX | MAG | FAI | LCK | Sum | SL |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1000 | Soldier | 14 | 9 | 12 | 12 | 11 | 8 | 10 | 10 | 86 | 6 |
| 1001 | Knight | 10 | 11 | 11 | 14 | 10 | 10 | 11 | 7 | 84 | 4 |
| 1002 | Hunter | 12 | 10 | 13 | 11 | 12 | 8 | 8 | 12 | 86 | 6 |
| 1003 | Priest | 13 | 11 | 12 | 13 | 8 | 8 | 13 | 8 | 86 | 6 |
| 1004 | Magician | 9 | 15 | 10 | 9 | 11 | 15 | 6 | 11 | 86 | 6 |
| 1005 | Wanderer | 10 | 10 | 11 | 11 | 15 | 9 | 7 | 13 | 86 | 6 |
| 1006 | Barbarian | 15 | 7 | 13 | 15 | 9 | 11 | 8 | 11 | 89 | 9 |
| 1007 | Thief | 10 | 13 | 10 | 9 | 14 | 10 | 8 | 15 | 89 | 9 |
| 1008 | Temple Knight | 11 | 8 | 13 | 14 | 12 | 6 | 13 | 7 | 84 | 4 |
| 1009 | Royalty | 8 | 12 | 8 | 9 | 12 | 13 | 12 | 7 | 81 | 1 |

Row 1010 is a developer row named "strongest character backup" (all stats 99, 100,000 souls). The class values are the respec floor.

### 1.4 The level-up screen at the Maiden in Black (read)

- The screen is 0x007DC418. Its preview 0x007E5810 keeps the pending raises at menu+0xFC, "can raise" at +0x104, the new value at +0x10C, the cost at +0x11C and the souls at +0x120. 0x007E4400 is a helper.
- The commit is 0x007E4A50. A per-stat jump table adds the pending count to the stat. 0x007E4248 refreshes HP, MP and stamina. PGD+0x88 grows by the count. The souls are reduced by the cost and clamped against 0x016FF700. Then message 0x1F58 shows.
- The screen only raises stats (inferred: the pending count starts at 0 and the preview only adds cost).

### 1.5 What a stat change must refresh (read)

- **Base values:** HP from Vitality, 0x002258D8 (CalcCorrectGraph row 100, through 0x00225658), into PGD+0x14. MP from Intelligence, 0x00225890 (row 101), into +0x20. Stamina from Endurance, 0x00226640, into +0x30.
- **The live character:** 0x002802E0 for HP (chr+0x3C4 current, +0x3C8 max, +0x3DC; vtable +0x94; rate from 0x00378940(chr+0x304)). 0x002737A0 for MP (+0x3CC, +0x3D0, +0x3E0; vtable +0x98; 0x003787C8). 0x00273620 for stamina (+0x3D4, +0x3D8; vtable +0x9C; 0x00378620).
- **Spell slots:** 0x008F6680 counts slots from Intelligence at 10, 14, 18, 24, 30 and 40 into inventory+0x11C, with a bonus at +0x120 and a cap of 7. 0x008F65E8 counts a second set from Faith at 10, 16, 24 and 36 into inventory+0x148 and +0x14C (what it gates is unknown).
- **Current from max:** 0x00919D60.
- **One call for the live part:** 0x007E4248 is what the level-up commit runs (read). It takes no arguments and finds PGD and the player character itself. For HP it recomputes the base (0x002258D8), calls 0x002802E0, stores the character's new max in PGD+0x10, moves current HP by the change in max (clamped to the max) and passes the change to the character through 0x0026FB20. MP and stamina work the same way (0x00225890, 0x002737A0; 0x00226640, 0x00273620), except that they pass the new current value (0x0026FC48, 0x0026FC98).
- **Equip load:** players know it grows with Endurance. Where the game keeps the limit was not found.
- **Item-weight limit (PGD+0x498):** the reset, the all-10 function and the new-character setup all write 100,000,000.0 there (read). What sets the real value later was not found.

### 1.6 The game's own reset (read)

0x0090FCD0(r3, pgd) does almost the whole job:
- reads the class row (CharaInitParam 1000 + PGD+0xB6) and writes the eight stats, both copies;
- recomputes base HP, MP and stamina (0x002258D8, 0x00225890, 0x00226640) and sets current and max to the base, without equipment bonuses;
- calls 0x00919D60 (current from max);
- sets PGD+0x88 to 0;
- writes 100,000,000.0 into PGD+0x498;
- unattunes every spell: for each of the 7 entries in two lists (PGD+0x244 and +0x270) it calls 0x009060E0(PGD+0x23C, id);
- recounts the slots (0x008F6680 with Intelligence, 0x008F65E8 with Faith).

It does not touch souls (+0x78), total souls (+0x80) or items. Its only caller is a penalty path at 0x004171F8. Related primitives: one level down 0x0090F508 (callers 0x004159A8, 0x0041A420, 0x002D5308; inferred: the penalty for a losing invader), all stats to 10 0x0090EE78, new-character setup 0x00910BA8, penalty flags clear 0x00919F60.

### 1.7 Approach

**A. A wrapper button while the level-up screen is open (recommended first).**
1. `DES_RESPEC=1` (off by default) shows a "Reset stats" button, or binds a key, only while the level-up screen is open. That is a guest-state gate: the player is in the Nexus, talking to the Maiden, in the one place the game itself changes stats.
2. The work runs on the game thread, from a hook in the screen's update (one new insn-hook site, so one re-lift).
3. Refund = the sum of 0x00226778(x) for x from the class sum + 1 up to the current sum, added in 64 bits.
4. If souls + refund passes 999,999,999, refuse and say why (or offer to clamp and lose the rest).
5. Call 0x0090FCD0(r3, pgd); it does not use r3 (read). Then write PGD+0x78 directly. Do not use AddSoul: it compares the sum in 32 bits, so when souls + amount reaches 2^31 the sum turns negative and is clamped to 0 (read, 0x0091B888). It would also count the refund as souls gained.
6. Call 0x007E4248 for the live character (section 1.5). The reset left current and max at the bare base values, so this call puts the equipment bonuses back (inferred; check HP, MP and stamina after). Then refresh the screen's cached values (inferred need; see section 5).
7. The game autosaves soon after. The button's confirm text should say that the change is permanent.

**B. Lower stats inside the game's level-up screen.** Let the pending count go below 0, down to the class value. Show the refund as a gain, and commit with negative counts. This uses the game's own screen, messages and save path. It needs changes in the preview (0x007E5810), the input handling and the commit (0x007E4A50), plus UI testing.

**C. Same-SL redistribution, as in later games.** Reset, then grant the old number of level-ups as free points. The free points would have to persist across saves, which the save format does not have. Not recommended.

### 1.8 Effort, risks, tests

| Step | Work | Days |
|---|---|---|
| 1 | Hook in the level-up screen's update; queue for wrapper requests | 0.5 |
| 2 | Refund loop over 0x00226778, cap rule | 0.5 |
| 3 | Call 0x0090FCD0, write souls, call 0x007E4248, refresh the screen | 0.5 |
| 4 | Wrapper button with a confirm, `DES_RESPEC` knob | 0.25 |
| 5 | Tests below | 0.5 |
| | **Total (approach A)** | **about 2-2.5** |
| | Approach B instead | 4-6 |

Risks:
- **Save:** the autosave makes it permanent. A wrong refund is permanent too. Test the round trip before shipping.
- **Spells:** the reset unattunes every spell. The player must re-attune at the spell NPCs.
- **Equipment:** weapons may fall under their stat requirements, and the load may go over the limit (MoveParam row 2, overweight). The game handles both, as after any stat change (inferred).
- **Cap:** from SL 620 up the refund does not fit, and below that it may not fit next to the souls already held.
- **Item-weight limit:** the reset leaves PGD+0x498 at 100,000,000.0, as the new-character setup does. Something must set the real value later, since the level-up screen shows it, but I did not find what. Until it runs, the item burden may be unlimited (inferred). Run step 1 reads the field.
- **Online (future, docs/46):** the stats stay consistent with the SL, so a server sees a normal character. No anti-cheat is known (inferred).

Tests:
- Round trip: note stats and souls, respec, then buy back the exact same build at the Maiden. The souls must end exactly where they started.
- Each class floor: a Royalty save resets to SL 1, a Thief save to SL 9.
- Status screen after the reset: SL, HP, MP, stamina, slots and load match a fresh character of that class.
- A SL 620+ save refuses.
- Quit to the title and continue: everything persists.

### 1.9 Built: `DES_RESPEC` (2026-10-28)

Not approach A or B: at the user's choice, the respec is at the statue in the Nexus, in a menu like a shop's, and the points are spent again in the game's own level-up screen. Off by default. Settings > Game > Items > Stat respec at the Nexus statue turns it on (`DES_RESPEC=1`), live.

**What the player sees.** A save that quit in the Nexus starts facing a statue. With the knob on, Cross in front of it opens the shop's command menu (the one with Upgrade Weapon, Repair Equipment, Buy Item, Talk and Leave), titled "Statue", with two rows: Respecialization and Leave. Respecialization asks "Return to your class's starting stats? The souls of every level come back, and every spell must be remembered again." with YES and NO. YES puts the eight stats back to the starting class's, gives back the souls every bought level cost, and opens the level-up screen (the Maiden's) to spend them again. The screen only raises stats, so nothing goes below the class's values. Leave, Circle and NO change nothing. A character still at the class's stats gets the screen alone, with the spells kept.

**No prompt.** The statue shows no "Examine" prompt. The HUD draws the action prompt only for a target that the game's action search found, and the statue is not one (see below).

**How it works** (`port/des_respec.cpp`). No hook in the lift, so no re-lift. `des_respec_tick` runs once a frame from `des_menu_tick` (des_menu_sfx.cpp), just before the menus' update, and calls the game's functions from there.

- **Where.** The map number comes from the level's script archive as it opens (`/script/m01.luabnd...`, through des_options.cpp's file-open hook): m01 is the Nexus. The player is at the statue when alive, on its floor (y within 1.5 of 479.5), within 2.2 of (0, -43) and on the near side (the spawn is at (0, 479.46, -41.69)), facing it within 60°, and with no menu open: slots 9, 0x11 and 0x17 at 0, and the menus' flags 0x00720F80 at 0 under the mask talk command 10 uses (0xDEFFFF7F). A Cross press must begin there; Cross held from before does not count.
- **The menu** is the talk scripts' own, from their commands (0x004CC3A8): 20 ClearTalkListData (0x00723450), 19 AddTalkListData (0x00723498, an entry {text id, row}) for each row, then command 10's 0x00721490(-1) (no talk partner) and 0x007212A8. Slot 9 goes to 1 while it is open and to 4 when it is answered. menus+0xABC holds the row (0 for Circle) once slot 9 is back at 0. The work waits for that and for the flags to clear: a dialog opened earlier was wiped by the closing menu.
- **The rows** are a table, `kRows`: each is a text of ours or a game text id, and the function the row runs (none for Leave). A new row is one line.
- **Texts.** The game has no "Respecialization", so the row's text goes into the talk menu's FMG (category 0x1E): an id of Leave's group (15000005 up) that has no text gets its offset pointed at our UTF-16 string in the guest heap. The lookup is 0x005C2848(fmg, id): groups {offset index, first id, last id} from +0x1C (count at +0x0C), offsets at *(+0x14), each from the FMG's start, modulo 2^32. The title: with no talk partner the menu shows the missing-name text "?NpcName?" (UTF-16 at 0x016EBB98). It is overwritten with "Statue" while the menu is open and put back after. Changing the pointer to it did not work: five tables hold it.
- **The question** is talk command 17's dialog: 0x007216D0(-1, -1), slot 0x11 = 2 (YES/NO; 1 is OK only), slot 0x54 = 0, 0x007216F0(text, YES, NO) with the texts 10020000 and 10020001, slots 0x4E and 0x4F = -1. The answer is at menus+0x82C (1 for YES) once slot 0x11 is back at 0.
- **The respec.** The floor is the class's CharaInitParam row (0x00597108 maps PGD+0xB6 to the row, 0x00597388 finds it). The refund is the sum of 0x00226778(x) for x from the floor's stat sum + 1 to the current sum. Then 0x0090FCD0(0, pgd), the souls written to PGD+0x78, and the character's own updates 0x002802E0, 0x002737A0 and 0x00273620, then 0x007E4248 (section 1.5).
- **The bars.** 0x007E4248 was made for raises. It moves each bar by the change from PGD's old max to the character's new max. After a respec the character's update has already cut the current value to the lower max, so the cut would count twice, and a soul form's HP could reach 0. So before 0x007E4248, each bar's PGD current is set to the character's, and its PGD max to the value that makes the bar end at its old share of the new max (at least 1). Measured: HP 341/341 before, 329/329 after.
- **The screen** is talk command 31's 0x007DC7A0 (slot 0x17 to 1). 0x007DC1E0 only shows the screen's help text.
- **The cap.** If the souls held plus the refund would pass 999,999,999, nothing changes and a dialog with one button (slot 0x11 = 1) says "Not now: with the souls back, you would hold more than 999,999,999." The talk texts have no "OK", so its button is our own text: 0x007216F0 takes the texts as pointers.
- **The log.** `DES_RESPEC_LOG=1` logs the steps. Each respec logs one line: `[des] respec: stat sum 89 to 86 (class 86), 3 levels, souls 47674 + 2326 = 50000`.

**The missing prompt** (tried, not solved). The game's action search 0x004EF840, called from the menus' update at 0x00724B18, shows the nearest target's prompt through 0x00723360(text id, flag, 1): menus+0x80C the text id, +0x814 the flag, +0x818. With no target it takes the prompt down every frame (0x007233E0(-1), 0x00723360(-1, 1, -1)). An instruction hook after that call put ours up the same way. The fields then matched the game's own Talk prompt at Boldwin field for field, but nothing drew. The flag byte made no difference (0 or 1), and neither did the text id: the game's own Talk id (10010200) did not draw at the statue either. The HUD's prompt update (0x007B8F18, through vtable 0x01869D2C) reads +0x80C and +0x800 (another HUD's timed message, set by 0x00721590), slot 0xAD, the menus' flags, slot 9 (at +0x38), and the search object's bytes +0x14 to +0x17 (`*(*(*(TOC - 0x72F0) - 0x7F88))`, the same object the HUD finds through `*(TOC - 0x71D4)`). All of them matched. The one difference seen was the search object's +0x10, the target found (a pointer at Boldwin, 0 at the statue). Showing the prompt would mean giving the statue a real action target in the search's list. The hook was taken out, so the build needs no re-lift.

**Tests** (2026-10-28): copies of a Soldier's save that quit in the Nexus (SL 6, 0 souls), 30 fps, the pad. The souls were set with a test-only knob.

| Test | Result |
|---|---|
| Cross at the statue | The menu, titled "Statue", with Respecialization and Leave |
| Leave; Circle | The menu closes, nothing changes |
| Respecialization, NO | Nothing changes |
| Respecialization, YES, at the class's stats (50,000 souls) | The level-up screen at once; stats and souls as they were |
| Three levels bought there (Vitality 15, Strength 14), then Respecialization, YES | Stats 89 to 86 (14 9 12 12 11 8 10 10), levels 3 to 0, souls 47,674 + 2,326 = 50,000, the exact round trip. HP 329/329. The screen opens with 50,000 souls |
| Quit Game, then continue from that save | Stats 86, souls 50,000, levels 0, HP 329/329 |
| Souls set to 999,999,000 with the console after the three levels, then Respecialization, YES | The OK notice; stats 89, souls 999,999,000 and levels 3 unchanged |
| Knob off, Cross at the statue | Nothing opens |

Not tested: the other classes' floors, a character with attuned spells (the reset unattunes them, section 1.6), a high SL, co-op or invasion (online is not working), and `DES_FPS=unlocked`.

## 2. Use several souls at once

### 2.1 The soul items (read)

From the goods param, the SpEffect param and the English item names:

| Goods id | English name | Souls | SpEffect |
|---|---|---|---|
| 1025 | Unknown Soldier's Soul | 200 | 3100 |
| 1026 | Unknown Hero's Soul | 400 | 3101 |
| 1027 | Renowned Soldier's Soul | 800 | 3102 |
| 1028 | Renowned Hero's Soul | 1,000 | 3103 |
| 1029 | Storied Soldier's Soul | 2,000 | 3104 |
| 1030 | Storied Hero's Soul | 4,000 | 3105 |
| 1031 | Legendary Soldier's Soul | 8,000 | 3106 |
| 1032 | Legendary Hero's Soul | 10,000 | 3107 |

All eight have goodsUseAnim 9 (the crush animation), isConsume 1 and no confirm prompt (opmeMenuType 0). The amount is SpEffect field +0xE8.

Boss souls are ids 7, 15-34 and 45 (SpEffects 3027-3048). They ask yes/no first (opmeMenuType 1, message 10030020). They also trade for spells, miracles and weapons, so "use all" must leave them alone.

### 2.2 How one use works today (read unless marked)

1. The inventory's command menu (0x0081DEC0) offers Use. The usable gate is 0x0081FBD0, with CanUseGoods 0x002284F0.
2. The character plays the crush animation. The item's SpEffect is applied (0x002D8978, 0x002D8B08; 0x00376E68 adds up the souls of the active effects).
3. AddSoul 0x0091B888(pgd, amount) clamps souls to 0..999,999,999 and adds the gain to PGD+0x80. The HUD counter animates through 0x00723E30(amount, delta).
4. One item leaves the stack. The decrement is likely 0x008FD2A8 (inferred; not decompiled).

The item list is at PGD+0x1F4 (inventory+0xCC): +0x08 count, +0x0C capacity, +0x10 entries, +0x14 highest used slot. An entry is 0x20 bytes: +0x00 category (0x40000000 for goods), +0x04 id, +0x08 count, +0x0C link, +0x14 valid flag. Find is 0x00900920, remove 0x00900A40, insert 0x00900B28, add 0x008FB4F0. Other AddSoul callers: 0x002D0860, 0x0053B670, 0x0053E578.

"Use" has no quantity prompt (inferred: one item per animation).

### 2.3 Approach

**A. "Use all soul items" (recommended).** With `DES_SOULS_ALL=1` (off by default), a wrapper button (or a key while the inventory is open) does this on the game thread:
1. For each of ids 1025-1032 in the item list, add count x souls. Read the souls from the SpEffect row through the param accessor rather than hard-coding them.
2. Remove those entries through the game's remove function, so the list stays consistent.
3. Call AddSoul once with the total, and let the HUD show the delta. The total stays far below 2^31: the add function caps stacks at 99 (read; inferred to hold for goods), so eight full stacks are 2,613,600 souls.

Gate it on guest state: the inventory menu open, or the character idle. No animation plays.

**B. One use takes the whole stack.** Hook the soul effect's AddSoul call. Multiply the amount by the stack's count and remove the rest. This keeps the crush animation and uses the game's own menu, but it needs the exact consume path (0x008FD2A8) and a stack-to-effect match (SpEffect 3100 + n is goods 1025 + n).

**C. A quantity prompt like the discard one.** It needs the game's count dialog wired to Use. I did not locate that dialog. Most work of the three.

### 2.4 Effort, risks, tests

| Step | Work | Days |
|---|---|---|
| 1 | Confirm the list layout in a run; signatures of remove 0x00900A40 and 0x008FD2A8 | 0.5 |
| 2 | The sum-remove-add routine on the game thread | 0.5-1 |
| 3 | Trigger (wrapper button or key), `DES_SOULS_ALL` knob | 0.5 |
| 4 | Tests below | 0.5 |
| | **Total (approach A)** | **about 2-2.5** |

Risks:
- **The item list:** writing counts by hand can break the list's links or its highest-slot field. Always go through the game's functions.
- **The cap:** AddSoul clamps at 999,999,999 and the rest is lost. Warn when the total does not fit.
- **Boss souls:** excluded by id.
- **No desync:** souls are local, offline and in co-op alike (inferred).

Tests:
- With N of one soul item: souls grow by N x value, and the entry is gone.
- A mix of several soul items at once.
- Other items and their order unchanged.
- Quit to the title and continue: souls and inventory persist, with no ghost entry.
- Near the cap: the warning shows, and souls stop at 999,999,999.

### 2.5 Built: `DES_SOULS_AT_ONCE` (2026-10-27)

Approach C, at the user's choice: the game's own count picker, the one Drop opens, with its question changed to "Please select the number of items to use." Off by default. Settings > Game > Items > Use several souls at once turns it on (`DES_SOULS_AT_ONCE=1`), live.

**What the player sees.** Use on a stack of two or more soul items (goods 1025-1032) opens the picker at the whole stack, with OK and CANCEL. ▲ ▼ (or a click on the arrows with the mouse in the game's menus) change the number. OK closes the menus as Use does: one crush animation, and the HUD counts up the souls of all of them in one gain. CANCEL goes back to the inventory with nothing used. Every other item, a single soul item and boss souls use as before.

**How it works** (`port/des_souls_use.cpp`). Three game functions are wrapped with `ppu_register_function`. All three are reached through vtables, so there is no re-lift.

- 0x0081E110, the command menu's choice (vtable +0x7C). Use sends message 2004 to the inventory and then writes -2 to menu slots 0x4F and 0x4E (the close). Drop sets slot 0x95 (0x85 in menu mode 1), sends 2001 and plays sound 100. For Use on a soul stack the wrapper takes Drop's steps instead, those of the inventory's 2001 for goods: slots 0x89 and 0x68, then `0x007731F0(11000, 1, 2, n, 1, n, 0xEAC4)` (the picker), then slot 2 = 9. The question's text pointer is then replaced at +0x820 of the menus' shared object, `*(*(*(TOC - 0x72F0) - 0x8000))`. The picker's number is at +0x830 of the same object.
- 0x008219E0, the inventory's message handler (vtable +0x6C). 2000 is the picker's OK, and the handler would drop the items. For our picker the wrapper never lets 2000 through: it only sets 2000's flags +0x140 and +0x141 and keeps the number.
- The Use itself waits for the menus' next update. `des_menu_tick` (des_menu_sfx.cpp, lifted in place of 0x00724E40) runs once a frame just before the menu system's update, and there the inventory gets 2004 as the command menu sends it, followed by the close. The Use cannot run inside the OK. The picker's way back to the inventory comes after the OK and writes the inventory's page (11001) to slots 0x4E and 0x4F. The close was lost, and the Use's page close (0x00723708) had already run, so the inventory came back without its title bar, under the HUD. Measured (menu slots, see below): after the deferred Use the slots match a plain Use's (slot 2 = 0, 0x4E and 0x4F = -1, 0x89 = 0).
- 0x002DD368, the character's use of an item (reached only through its function descriptor, from the character's vtable). It applies the item's effect (0x00278E90) and then 0x008FDB00 takes one item off the stack at the index Use left at PGD+0x2CC and sets that to -1. When that index was ours and is now -1, the wrapper takes the rest off the stack with the game's count change 0x008FD2A8(PGD+0x128, index, -(n-1), 0). Their souls go through the character's own gain 0x002D8978(character, souls, 0, 0): AddSoul, and the HUD's counter 0x00723E30, which adds to the gain already showing.
- The souls of one item come from the params the way 0x00278E90 finds the effect: goods row +0x14 is a behavior id, the behavior row (getter 0x00595B80, param table 10) +0x14 is the SpEffect id, and the SpEffect row's +0xE8 is the souls. The SpEffect table is found by the type name `SP_EFFECT_PARAM_ST` at +0x0C of each table's file (0x005A5920(table, 0) + 0x20). Its rows are the file's {id, data offset, name offset} at +0x30. The getters' table numbers do not follow the param bundle's order: none of the 18 getters tried returned the SpEffect row.
- The cap: the picker offers no more than fit under 999,999,999 souls. At the use the rest is clamped again, keeping room for the one item's own souls, which may come later with its SpEffect. Items that would not fit stay in the stack.
- No picker while a Use still waits for the character (PGD+0x2CC is not -1). A second Use would take its place.

**The menu slots.** `0x00720F50(n, v)` stores v at +0x14 + 4n of the menus' shared object, and 0x00720F20 reads it. A test run dumped them with the debug console (`mem`) around a plain Use and around the picker. Slot 2 is the inventory's sub-mode: 9 with the command menu or a picker open, 2 browsing, 0 closed. 0x4E and 0x4F hold the page shown (23008 the command menu, 11001 the inventory, -1 none). A -2 asks for the close, which the menus honor only in sub-mode 9: in sub-mode 2 they write 11001 back every frame. Forcing slot 2 to 9 there made the slots read closed, but the inventory stayed drawn. The menu script, not code, reads these slots: only two code sites read slot 0x4F.

**Tests** (2026-10-27): copies of the play session's save, in Boletarian Palace, `DES_FPS=unlocked`, the mouse in the game's menus on. Counts were set with the console's `poke32` on the run's own copy.

| Test | Result |
|---|---|
| 2 of 2 Unknown Soldier's Souls, pad | Picker at 02 with Use's question; menus closed, crush, +400 in one gain; souls 18 to 418; the row is gone |
| 7 of 10, pad (▼ three times) | +1,400, souls 1,418, 3 left |
| 3 of 5, mouse (▼ clicked twice, OK clicked) | +600, 2 left |
| CANCEL | Back in the inventory, nothing used |
| Drop after a cancelled Use | Drop's own question, 1 dropped, 9 left |
| Souls at 999,999,000, stack of 10 | Picker at most 04; 4 used, souls 999,999,800 |
| Grass with the knob on | No picker, the game's Use (16 to 15) |
| Knob off | The game's Use, one soul |
| Continue from the save | 3 left and 1,418 souls after the next autosave. The game saves at Use, before the item leaves, so the save made at a Use holds the state before it (as without the knob) |

Not tested: menu mode 1 (0x85), the storage box, and a co-op phantom's use.

## 3. 8-direction movement while locked on

### 3.1 What the game has (read)

**MoveParam rows for the player** (names translated from the row names):

| Row | Use | Walk F/B/L/R | Dash | Rolls F/B/L/R | Backstep |
|---|---|---|---|---|---|
| 0 | standard | 200-203 | 500-503 | 700-703 | 680 |
| 1 | heavy | 300-303 | 710-713 | 720-723 | 690 |
| 2 | overweight | 300-303 | none | 730-733 | 731 |
| 3 | near death | 230-233 | none | 700-703 | 731 |
| 4 | blood swamp | 300-303 | none | 730-733 | 731 |
| 5 | poison swamp | 300-303 | super dash 710 only | 730-733 | 731 |

Field layout: stay +0x00, walk F/B/L/R +0x04..+0x10, dash +0x14..+0x20, super dash +0x24, escape F/B/L/R +0x28/+0x2C/+0x30/+0x34, turns +0x38..+0x44, stepMove (the backstep) +0x48. Only the fly sets (+0x58 on) have diagonal fields, and the player has none. The getter is 0x005A0030 (MoveParam is param index 17).

**Roll animations and their root motion** (read from the last reference-frame sample of each Havok file; forward is -z):

| Anim | Field | Moves | Length |
|---|---|---|---|
| 700 | escape F | 4.18 m forward | 1.77 s |
| 701 | escape B | 3.62 m back | 1.93 s |
| 702 | escape L | 4.5 m along +x | 1.70 s |
| 703 | escape R | 4.5 m along -x | 1.77 s |
| 680 | backstep | 2.0 m back | 1.33 s |
| 720-723 | heavy rolls | 3.77 / 2.65 / 3.63 / 3.63 m | 2.67-2.83 s |
| 690 | heavy backstep | 2.0 m back | |
| 730-733 | overweight rolls | 0.5 m | 2.33-3.0 s |

The walk animations follow the same axes (200 moves -z, 202 moves +x). There is no diagonal roll or walk animation in `c0000_a0x.anibnd`. The bank also holds 705 (3.09 m forward, with a yaw of 9.42 in its last sample) and other four-way sets (7010-7013, 7020-7023). I did not examine those.

**Walking while locked on.** The player's locomotion controller (vtable 0x0185C338, constructor 0x00322428, update 0x003259F8) finds the bones Upper_Root, Master and Neck at init (0x003228B0). Its reset 0x00326B18 turns Master by an angle, Upper_Root by -angle x ratio (+0x1B8) and Neck by -angle x (1 - ratio) (read). That is a lower-body twist. I infer that it points the legs along a diagonal walk while the chest faces the target, so walking is already "8-way". The run checks it.

### 3.2 How a roll is chosen today (read)

1. **Stick.** The pad manipulator's update 0x003103F8 calls 0x0030E750 each frame. That reads the left stick through 0x001B6998 and 0x001B6960 (0x001C3000 with analog ids 17 and 16; radial deadzone 0.37, see docs/41).
2. **Four directions.** Each frame keeps only the larger axis (0x0030E8B0). A 6-frame history (manip+0x1B4 and +0x204, index +0x254; length 6 at 0x019DA308, decay 0.9 per frame) gives the strongest recent value per axis, f30 and f31. Then only the larger of those two survives (0x0030EA34; a tie keeps f30).
3. **Sprint or roll.** While Circle is held, manip+0x1B0 counts up. Past 0.4 s (0x019DA310) it sets manip+0x168, the sprint. The count stays at 0 while chr+0x310 bit 0x800 is set or the SpEffect check 0x00271A68 (state 15) passes. That matches "no sprint while locked on", so bit 0x800 is likely the lock flag (inferred). Run r4: it is not, and the timer runs while locked on too (section 3.6).
4. **Slot.** When bit 6 of the pad's button word +0x122 is set (tested by 0x001B9A10; inferred: Circle released), it sets manip+0x158 and one slot byte. The hold timer in step 3 uses bit 5 (0x001B9D68; inferred: Circle held). The threshold T is the float at 0x01B3FCAC, in .bss. No code stores to it. Besides 0x0030E750, only 0x00312060 loads its address, and it puts that address into a menu item it builds (inferred: a debug tunable). So T is 0.0 in retail (inferred).

   | Condition (read) | Slot | Script states (read) | MoveParam field | Row 0 anim |
   |---|---|---|---|---|
   | f31 > T | 18 (+0x96) | #20, #90 | escape B +0x2C | 701, 3.62 m back |
   | f31 < -T | 17 (+0x95) | #21, #91 | escape R +0x34 | 703, along -x |
   | f30 < -T | 16 (+0x94) | #19, #89 | escape F +0x28 | 700, forward |
   | f30 > T, and the lock flag or the SpEffect check; or f30 > T on a tap released before 0.4 s | 15 (+0x93) | #187, #25, #94, and four more (step 5) | stepMove +0x48 | 680, backstep |
   | none of the above, and the current stick at most 0.45 (0x019DA30C) on each axis | 10 (+0x8E) | #22, #92 | escape L +0x30 | 702, along +x |

   Only 0x0030E750 writes these five slots.

   > Run r4 measured the moves: slot 10 backstep, 15 forward roll, 16 back roll, 17 left roll, 18 right roll. The condition column is right. The last three columns are not (section 3.6).
5. **Script.** The player's script tests the slots. Function 100 (0x002F8E60) is "slot x set and not consumed" (manip+0x84+x and +0xA7+x). Function 108 (0x002F7D68) is "slot x buffered, and its input window open", for x in 9-12 and 15-18 (script wrapper +0x8C+x and +0x3E4+x; 0x002FC9A8 closes the windows). In state #0 (inferred: standing), one transition group holds, in order: slot 15 to #187, 16 to #19, 18 to #20, 17 to #21, 10 to #22. Each also needs function 1001 > 10 (the current stamina, chr+0x3D4) and function 207 == 1 (meaning unknown). Across the whole script, slots 16, 18, 17 and 10 always lead to the same roll states (#19, #20, #21, #22, or the copies #89-#92), apart from one buffered path each back to #0. Slot 15 leads to seven states: #187, #25, #94, #34, #67, #78 and #135. I did not examine the last four.
6. **Animation.** Each roll state plays command (2,3) with the animation from function 110 (0x002EB3E8): #19 passes 0, #20 passes 1, #21 passes 3, #22 passes 2, #25 passes 4. Function 110 takes the move-set row from ctrl+0x194 (row ids at ctrl+0x17C) and calls 0x0059FF98(row, k). That returns escape F, B, L, R or stepMove for k = 0..4 (jump table at 0x0059FFE0). Then 0x002758C8 turns it into a file id. Each roll state also runs command 1001 with -20 at the start (inferred: the stamina cost). State #187 plays command (2,4) with 72 = 0x48, the stepMove offset (inferred: "play the MoveParam field at this offset").

A second copy of the five states (#89-#92, #94) serves another context, perhaps another stance (inferred). Function 110 feeds both copies, so a hook there covers both.

### 3.3 What does not add up

Taken together, the table says a neutral tap gives a side roll (escape L), "f30 > T" gives the backstep, and one stick axis gives both the back roll and the right roll. The game does not play like that. A neutral tap is a hop backwards, and a held direction rolls that way ([controls](https://www.demonssouls.com/Controls)). So one link above is misread.

The most likely place is the stick transform at 0x0030EC44-0x0030F2C0, which I did not decode. It rewrites f30, f31 and the current stick when ctrl+0x139 != 0 and manip+0x294 == 0. It builds rotations from manip+0x268 and 0x002E7420, wraps angles and multiplies 4x4 matrices. If its output is a direction rather than a scaled stick, a neutral stick could come out as a unit vector. Run step 3 in section 6 records which move each input gives, and the gate bytes.

The 8-way plan below does not depend on this table. It only needs "a roll is starting, with this k, and the stick points here".

> Run r4 put the misread link after the slot: in the script's transitions or in function 110's k (section 3.6). The stick transform is not the cause: M+0x139 is 00 while locked on, so it is skipped then.

### 3.4 Approaches

**(a) Turn the character, keep the animation (recommended).** For a diagonal input, let the game start its roll as today (the larger axis wins). At the start of the roll, turn the character by the difference between the stick angle and that roll's direction, snapped to 45 degrees. A forward-left input then plays the forward roll turned 45 degrees left. Hold the turn until the roll ends. Then the locked facing update (0x0030F3C0, called from 0x003103F8) turns the character back to the target at its normal rate. The backstep (k = 4) is not turned.
- One animation plays, so its events stay as they are: invincibility frames, stamina, sounds, cancel windows, the rolling attack.
- Hook points: the slot stores in 0x0030E750 (0x0030EC08, 0x0030F2CC, 0x0030F304, 0x0030F38C, 0x0030F39C) to keep the stick angle that chose the slot, and the two calls of 0x002EB3E8 inside script function 110 (0x002FF8F4, 0x00301C28) to know k at the roll's start. Rolls out of an input window (function 108) start later than the tap, so the angle must be kept with the slot.
- Still to find: the character's yaw field, and whether 0x0030F3C0 pulls the facing back during the roll. If it does, it must skip that while a turned roll runs.
- A possible shortcut: the game already rolls in any direction when not locked on (the character faces the stick first). Letting 0x0030F3C0 take its unlocked branch for a diagonal roll's first frames may give the same result with fewer changes. Worth one test before writing (a).

**(b) Rotate only the root motion.** The character keeps facing the target and slides 45 degrees off the animation's line. It looks wrong for a tumble. Not recommended.

**(c) Blend two rolls.** Blending escape F and escape L at 50/50 gives a diagonal path, but the pose averages a forward tumble with a side roll. The two animations differ in length (1.70-1.93 s, 2.67-2.83 s heavy) and in event timing, so the invincibility and cancel windows would need choosing. Weeks of tuning for a worse look.

**(d) New diagonal animations.** This is what the PS5 remake did: eight directions with new animation work ([TheGamer](https://www.thegamer.com/demons-souls-remake-omni-directional-rolling/), [Sirus Gaming](https://sirusgaming.com/2020/11/02/demons-souls-ps5-roll-system-camera-improvements/)). Not feasible here: it needs animation authoring, and game-derived files cannot ship.

### 3.5 Effort, risks, tests

| Step | Work | Days |
|---|---|---|
| 1 | Run step 3 (section 6): the input-to-move table, the lock flag, the gate bytes | 0.5 |
| 2 | Find the character's yaw and how 0x0030F3C0 drives it; where root motion is applied | 1-1.5 |
| 3 | Hooks (slot stores, function 110), the 45-degree turn, holding it for the roll; one re-lift per hook change | 1-1.5 |
| 4 | `DES_ROLL8` knob and a settings toggle | 0.5 |
| 5 | Tests below | 1 |
| | **Total** | **about 4-5** |

Risks:
- **The facing fights back:** if the lock turn runs during the roll, the roll curves.
- **Move sets:** heavy and overweight rolls must turn the same way. The overweight backstep is 731, a roll-like animation, and must stay unturned.
- **Follow-ups:** a rolling attack after a turned roll swings in the turned direction until the lock turn catches up (may be what players want).
- **Frame rate:** the hold-time and history counters count frames and seconds as the game does today. Check at `DES_FPS=60` and 120.
- **Online (future, docs/46):** a remote player should see the turned roll if facing is part of the synced state (inferred).

Tests:
- Locked on: all eight directions and neutral. Each diagonal rolls along the diagonal; each cardinal and neutral behaves as the original.
- Not locked on: unchanged.
- Invincibility: roll through an attack on a diagonal.
- Stamina per roll unchanged.
- The character re-faces the target after the roll.
- Move-set rows 0-3, rolls from an input window (during an attack's recovery), 30/60/120 fps.

### 3.6 Run r4 (2026-10-19)

Run r4 did section 6's steps 2 to 4 at 30 fps, from the pinned regression save. It was scripted through the console, `PAD_FILE` and `PAD_STICK`, and no real input reached the game, apart from one mouse move noted below. Step 1 was not run. Each locked trial started 6 to 12 m from the target. Angles are from the camera's forward, which points at the target while locked on, and positive is to the left.

**Locked on:**

| Input | Slot set | Move | Distance, direction |
|---|---|---|---|
| neutral | 10 | backstep | 1.82 m at 177 deg |
| up | 15 | forward roll | 3.50 to 3.92 m at 0 to 2 deg |
| down | 16 | back roll | 3.92 m at 180 deg |
| left | 17 | left roll | 3.44 m at +83 deg |
| right | 18 | right roll | 2.89 m at -87 deg |
| up-left | 15 | forward roll | 2.89 to 3.29 m at +4 to +5 deg |
| up-right | 15 | forward roll | 3.80 m at -5 deg |
| down-left | 16 | back roll | 3.87 m at 173 deg |
| down-right | 16 | cut short | 1.38 m at 170 deg, moving 0.42 s instead of a roll's 1.5 s (inferred: blocked behind) |

**Not locked on** (only neutral and down were run): neutral set slot 10 and gave the backstep (1.79 m, moving 0.45 s like the locked one). Down set slot 15 and rolled toward the stick (3.97 m).

Each trial set one slot byte (manip+0x84+x), and its used byte (+0xA7+x) about 0.03 s later. Every roll and backstep cost 20 stamina, which confirms the -20 of command 1001 in section 3.2 step 6.

What this settles:
- **Stick to slot: section 3.2 is right.** The history bytes match the conditions: up gave +1.0 in manip+0x1B4 and slot 15, down -1.0 and slot 16, left -1.0 in +0x204 and slot 17, right +1.0 and slot 18, and neutral neither and slot 10. So +0x1B4 holds f30 and +0x204 holds f31. A diagonal gave 0.707 in +0x1B4 and 0 in +0x204: the tie keeps f30 (step 2), so a diagonal rolls forward or back.
- **Slot to move: wrong for all five.** Measured: slot 10 backstep, 15 forward roll, 16 back roll, 17 left roll, 18 right roll. The table's last three columns give 10 escape L, 15 backstep, 16 escape F, 17 escape R and 18 escape B. So the misread link comes after the slot: in the script's transitions (step 5) or in function 110's k (step 6). Which of the two is still open.
- **The stick transform is not the cause.** It runs only while M+0x139 is not 0 (section 3.3), and M+0x139 read 01 unlocked and 00 locked. So it is skipped while locked on. Unlocked, it fits the roll toward the stick: down set slot 15, the forward slot (inferred: the transform turns the stick into the character's frame).
- **The lock flag is not chr+0x310's bit 0x800.** That bit is bit 0x800 of the word at chr+0x314 (the code loads +0x310 as a doubleword and masks its low word). It was clear both ways: chr+0x310 read FFFFFFFF00190050 unlocked and locked. It selects the second camera, the bow's aim view: setting it opened the aim view in docs/41's mouse-aim test (run aim1, 2026-10-19). M+0x139 changes with the lock. M+0x194 (00000001) and manip+0x294 (00) do not. The camera manager's +0x90 read 0 both ways, and the camera's +0x240 read 0 while locked (the camera's update clears +0x240, so it reads 0 from outside). r4 detected the lock by the look-at point, camera +0x230, which the game rewrites every frame while locked on.
- **The sprint timer runs while locked on.** With Circle held for 1.2 s, manip+0x1B0 reached 1.200 both unlocked and locked, and the sprint flag manip+0x168 went from 00 to 01 both times. So step 3's "no sprint while locked on" reading is wrong. The stick was not held, so whether the character then runs was not checked.
- **Side rolls run straight.** The left and right rolls stayed within 0.11 m of the line from start to end, and did not curve around the target (inferred: the lock turn does not turn the character during a roll, which section 3.4 (a) needs). Facing during the roll was not measured.
- **Diagonal walk.** Locked on, the down-right walk went 3.67 m at -137 degrees for the stick's -135, so the walk follows the diagonal. Whether the legs twist cannot be told from the one screenshot. The up-left walk is void: a real mouse moved during it and switched the target.
- **L3 does nothing, as a game without a jump would.** Demon's Souls has no jump (checked by hand in RPCS3, 2026-10-19: L3 while dashing does nothing). r4 pressed L3 during a 5.34 m/s dash. The game's pad reads saw L3 in 15 samples. The player did not rise. No byte of the slots (manip+0x84 to +0xC3) or of manip+0x150 to +0x25F changed that did not also change in the dash without L3.

For the 8-way plan (section 3.4 (a)): a diagonal starts the forward or back roll today, so that roll is the one to turn 45 degrees. Step 1 of section 3.5 is done.

### 3.7 Built: `DES_ROLLDIR` (2026-10-25)

Approach (a) of section 3.4, on by default: the user chose that, and it is the one game-side change here that is on by default. Settings > Controls > Roll toward the stick turns it off (`DES_ROLLDIR=0`, the original 4-way rolls). It applies live.

**How it works** (`des_roll_dir` in `port/des_options.cpp`, three lines in `port/insn_hooks.txt`, so it needs a re-lift; only `ppu_recomp_006.cpp` changes):

- Script function 110 asks 0x002EB3E8 for the roll's animation once a roll. `DES_ROLLLOG=1` prints each call. Its k, measured: up 0, down 1, left 2, right 3, neutral 4 (the backstep). That is section 3.2 step 6's reading (escape F, B, L, R, stepMove), so the misread link of section 3.6 is in the script's transitions (step 5). On an exact diagonal the game picks right at 45 degrees, back at 135 and -135, and forward at -45.
- The hook after `extsw r4, r4` at 0x002FF8F0 runs just before that call. For the player (M = *(chr+0x20)), locked on (M+0x139 = 0), with a target (*(M+4)+0x100), the stick pushed (over 0.2) and a roll (k 0 to 3), it turns the facing to the target's bearing plus the stick's angle from k's direction. The turn is at most 90 degrees, and none when the stick is along the roll. The facing is the yaw at P+0x04 (P = *(M+0x10)), the bearing + pi; the target's position is at *(M+4)+0x110. The same hook follows `li r4, 0` at 0x00301C24, the call's path for an argument that is not a number; the runs never took it.
- That turn alone is undone (run rd4, below): the locked facing update 0x0030F3C0 aims the character at the target every frame, before the roll moves. So for 0.3 s of game time, the hook after `fadds f1, f1, f0` at 0x0030F734 (that update's yaw toward the target; r26 = M) adds the same angle. If the facing jumps away from the turned one in that time (the lock was dropped and taken again), it stops at once.
- The roll keeps its facing to its end (section 3.6 saw the same). Then the lock turns the character back, as after any roll.
- Unchanged: a stick along a roll's direction, the backstep, rolls when not locked on, and other characters.

**Runs** rd1 and rd3 to rd5: pinned save, scripted pad (`PAD_STICK`, Circle through `PAD_FILE`), the branch's developer build, the player about 7.8 m from the enemy and locked on. The test spot is a corridor with walls about 1.3 m to the right and 2 m to the left, so side rolls end at a wall. The direction below is the median of the roll's 50 ms steps faster than 3.5 m/s in its first 0.8 s; the walk before the press and a slide along a wall are slower. Degrees from the target, + = right; the user's save was unchanged after each run.

Run rd5:

| Stick | `DES_ROLLDIR=0`, 30 / 138 fps | `DES_ROLLDIR=1`, 30 / 138 fps |
|---|---|---|
| 0 | +0.7 / -0.0 | +1.2 / +2.6 |
| 22.5 | +1.0 / -0.4 | +19.5 / +21.3 |
| 45 | +92.4 / +89.5 | +48.5 / +44.9 |
| 67.5 | +81.2 / +87.0 | +63.1 / +66.2 |
| 112.5 | +90.9 / +87.5 | +115.5 / +107.9 |
| 135 | +179.2 / +178.2 | +132.5 / +132.8 |
| 157.5 | -179.1 / +179.6 | +157.2 / +156.3 |
| -157.5 | -179.2 / -177.4 | -155.9 / -153.5 |
| -135 | -175.5 / -178.3 | -129.8 / -130.6 |
| -112.5 | -88.2 / -83.2 | -109.7 / -108.3 |
| -67.5 | -86.4 / -86.3 | -61.7 / -63.4 |
| -45 | +1.3 / +1.6 | -38.3 / -37.7 |
| -22.5 | +3.7 / +4.3 | -19.8 / -20.8 |
| neutral | backstep, -179.1 / +179.4 | backstep, -178.5 / -178.1 |

With `DES_ROLLDIR=1` every diagonal rolls within 7.3 degrees of the stick at both rates (mean 3.2); off, they snap to the four directions as before. 3.2 s after the press the character faced the target within 3 degrees in 43 of the 56 rolls. The rest are mostly forward rolls, in both modes, which end about 4 m closer to the target and were still turning. One of them turned on past the target 1 s after its roll had ended, 2.4 s after the hook's window, which looks like the lock taking another enemy.

Run rd4, the turn without the 0.3 s hold, 30 / 138 fps: 22.5 gave +1.9 / +0.9, 45 gave +92.1 / +88.9, 157.5 gave -178.8 / +178.6, -157.5 gave -179.5 / -176.8 and -45 gave +2.0 / +4.1. Those are the original rolls. With the hold the same session gave +21.9 / +20.5, +46.3 / +47.0, +158.4 / +155.8, -154.7 / -153.7 and -37.1 / -40.6.

Not tested: the heavy and overweight move sets (rows 1 to 5 use the same k), rolls out of an attack's input window, and a remote player's view online.

### 3.8 Built: `DES_ROLLTIE`, diagonal rolls when not locked on (2026-10-26)

Not locked on, a roll toward a diagonal sometimes went 90 degrees off, to the side. It showed with the keyboard (W+D and the other pairs), at 30 fps and above.

**Why.** Not locked on, 0x0030E750 still keeps one stick axis each frame (section 3.2 step 2). It then turns that axis and its history into the frame of the stick's own direction (0x0030EC44 to 0x0030F2C0, run when M+0x139 is 1). The kept axis is at most 45 degrees off the stick, so the turned forward part (f30) is at least the side part (f31): the roll is slot 15, and the character rolls toward the stick. On an exact diagonal the two parts are equal (0.5 and 0.5), and the turn's rounding in the last bit decides between them.

- The game reads an axis byte as (b - 127.5) / 127.5. The pad diagonal (219, 37) fits that exactly: its turned parts were 0.505494 and 0.499970, 44.69 degrees. So the keyboard's corners (0 or 255 on each axis) are always exact diagonals. Of the pad's four diagonals at full tilt, (219, 219) and (37, 37) are exact too.
- In the logged frames, the side part was never larger than the forward part to 6 decimals. Every off roll came from a tie that the rounding broke the other way.
- With the stick and Circle pressed together, the character's facing was still turning, and the parts were the same. So the turn uses the stick's direction, not the facing.

**Fix.** `des_roll_tie` runs after the turn's last load (0x0030F2BC). A near tie (the side part larger by at most 1%) goes forward, as the game's own tie rule means: f31 is set to 0, and f27 too for the current frame's pair (f26, f27). The hook is only on the not-locked path, so locked-on rolls (and `DES_ROLLDIR`) are unchanged. `DES_ROLLTIE=0` is the original. `DES_ROLLLOG=1` prints the first frame of each run of near ties.

**Runs** rk4 (unlocked, about 240 fps) and rk5 (30 fps). Deep Palace save, scripted stick (`PAD_STICK`) and Circle (`PAD_FILE`), the four diagonals in the pad's shape ((219, 37) and so on) and the keyboard's ((255, 0) and so on). Three starts: standing after turning to the stick, running, and the stick and Circle together. A roll more than 30 degrees off the stick counts as off; the off rolls went 70 to 92 degrees off.

| Off rolls, of 4 diagonals | Standing | Running | Stick and Circle together |
|---|---|---|---|
| Unlocked, `DES_ROLLTIE=0`, pad shape | 0 | 1 | 0 |
| Unlocked, `DES_ROLLTIE=0`, keyboard | 2 | 3 | 2 |
| Unlocked, `DES_ROLLTIE=1`, pad shape | 0 | 0 | 0 |
| Unlocked, `DES_ROLLTIE=1`, keyboard | 0 | 0 | 0 |
| 30 fps, `DES_ROLLTIE=0`, pad shape | 1 | 0 | 2 |
| 30 fps, `DES_ROLLTIE=0`, keyboard | 2 | 3 | 1 |
| 30 fps, `DES_ROLLTIE=1`, pad shape | 0 | 0 | 0 |
| 30 fps, `DES_ROLLTIE=1`, keyboard | 0 | 0 | 0 |

The pad shape's off rolls were all on its exact diagonals. In one 30 fps trial with the fix (keyboard, running, 315 degrees) no roll started; in run rk6 that trial rolled toward the stick three times out of three, and the other keyboard diagonals once each.

## 4. PvP arena

### 4.1 What the game has

- **No arena.** There is no arena map or mode (inferred; the game never had one).
- **Online PvP:** invasions and duels. The system goods 9995-9999 (black eye invasion, red eye sign, blue eye sign, kick out, leave session) have disable_offline = 1 (read, goods param). They run on NP Matching2 rooms plus P2P, and the DeS server only relays the room ID (docs/46, read there from DeSSE).
- **The Old Monk fight:** online, a player is summoned as the boss (`summonBlackGhost` in docs/46). This is the closest thing the game has to a set-piece duel. Offline, an AI phantom plays the part.
- **The losing invader drops one soul level** (0x0090F508, inferred).
- **AI phantoms exist.** The engine runs AI-controlled characters on the player model, such as the offline Old Monk and the black phantom NPCs (inferred).

### 4.2 Options

| Option | Verdict |
|---|---|
| Online invasions and duels among port players | docs/46 stage 2, 8-16 days, after stage 0 (4-5 days) and stage 1 (4-7 days). A duel room list on top would be server-side work on a community server. |
| Local two players (split or shared screen) | Not feasible. The engine has one local player, one camera and one controlled character. |
| Offline duel against an AI phantom with a chosen build | Possible in principle, but it needs runtime spawning of an NPC phantom, an arena location and AI tuning. Weeks, high risk. Shipping edited map or event files is not allowed, so it would all have to happen at runtime. |

Verdict: not feasible as a new mode at a sensible cost. PvP arrives with docs/46 stage 2. Save corruption and bans do not apply offline.

## 5. What I could not determine

1. **The stick-to-move table** (section 3.3). The transform at 0x0030EC44-0x0030F2C0 is not decoded. Run r4 measured the table (section 3.6). Still open: whether the script's transitions or function 110's k was misread. The transform runs only unlocked.
2. **The lock flag.** chr+0x310 bit 0x800 stops the sprint timer, which fits "no sprint while locked on", but it is not confirmed. What SpEffect state 15 (0x00271A68) means is unknown; perhaps "cannot sprint". Run r4: bit 0x800 is not the lock flag (it is the aim view's), and M+0x139 changes with the lock (01 unlocked, 00 locked).
3. **The character's yaw field,** and whether the locked facing update 0x0030F3C0 turns the character during a roll. Run r4: side rolls ran straight (inferred: no turn during a roll). The yaw field is still open.
4. **Functions 207 and 225 in the roll conditions,** and why one backstep transition needs function 225(1) == 46.
5. **The item decrement 0x008FD2A8** and the exact signatures of remove 0x00900A40 and find 0x00900920.
6. **Whether the level-up screen re-reads PGD each frame,** which matters for a respec while it is open.
7. **What the second slot count from Faith (0x008F65E8) gates.**
8. **What sets the item-weight limit PGD+0x498** after the reset writes 100,000,000.0 there, and where the equip-load limit lives.
9. **The exact rounding of 0x0094E508.** Round-to-nearest fits every cost I checked. Use the game's function anyway.
10. **Diagonal walking while locked on:** the leg twist is read, but its effect on a diagonal is inferred. Run r4: the down-right walk followed the stick's diagonal. Whether the legs twist was not seen.
11. **The other slot-15 states** (#34, #67, #78, #135), anim 705 and the 7010-7023 sets.

## 6. Run request (one batch)

> Steps 2 to 4 were run as r4 (2026-10-19). The results are in section 3.6. Step 1 was not run.

Run form, through Git Bash: `tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 PS3_DEBUG=<scratch>/dbg.txt PAD_FILE=<scratch>/pad.txt`. From PowerShell, in one line:

`& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 PS3_DEBUG=<scratch>/dbg.txt PAD_FILE=<scratch>/pad.txt"`

The console works as in docs/41 section 6: write one command into `<scratch>/dbg.txt`, wait for `[dbg] done` in `<scratch>/dbg.txt.out`, then write the next. Guest memory is big-endian. `PAD_FILE` takes a button mask, held for N game pad reads: `echo 0x0004 2 > <scratch>/pad.txt` is R3 (lock on), `echo 0x2000 3 > <scratch>/pad.txt` is a Circle tap. `setenv PAD_STICK=<lx>,<ly>,128,128,0,2` holds the left stick for 2 s (128 is centred). Only a new value restarts the window, so alternate the last number (2, then 3) when an input repeats.

Steps 1 and 3 change the save (one level bought, one soul item used). Use the pinned regression save or a backup, as for any test run.

**1. Stats, souls and items (in the Nexus).**
- Do: `mem 01B4EF9C 4` gives G. `mem <G+8> 4` gives P. `mem <P> 256`, then `mem <P+480> 64`.
- Log: both dumps, next to the status screen's stats, souls and SL, and the level-up screen's derived values.
- Expect: the eight stats at +0x38, +0x40, ... +0x70 (and copies 4 bytes later); souls at +0x78; level-ups at +0x88; the class at +0xB6. At +0x498, a float the level-up screen shows as a whole number (not 4CBEBC20, which is 100,000,000.0).
- Then buy one level of Strength at the Maiden and repeat both dumps. Expect: Strength +1 (both copies), +0x88 +1, souls down by cost(new sum) from section 1.2. Note whether +0x498 changed.
- Items: `mem <P+1F4> 32` (count at +0x08, entries pointer at +0x10). Then `find <entries> 8000 4000000000000401` for the Unknown Soldier's Soul; use 402 to 408 for the others. `mem <hit> 32`: the count is at +0x08. If the save has one, use it once and repeat both dumps. Expect: souls +200 (or that item's value), count -1.

**2. Threshold and gate bytes.**
- Do: `mem 01B3FCAC 4` (T). `mem 01B3D6E0 4` gives C. `mem <C+1BC> 4` gives the character. `mem <chr+20> 4` gives the controller M. `mem <M+174> 4` gives the manipulator.
- Then, unlocked and locked: `mem <chr+310> 8`, `mem <M+139> 1`, `mem <M+194> 4`, `mem <manip+294> 1`. While holding Circle unlocked, and while holding it locked: `mem <manip+1B0> 4`.
- Expect: T = 00 00 00 00. Bit 0x800 of chr+0x310's low word set only while locked. manip+0x1B0 counting up only while unlocked. M+0x194 = 0 with a light load.

**3. Which move each input gives** (near an enemy, for example early in the first level).
- Do: lock on. For each input, tap Circle: neutral, up, down, left, right, up-left, up-right, down-left, down-right. By pad, or scripted: `setenv PAD_STICK=<lx>,<ly>,128,128,0,2`, then `echo 0x2000 3 > <scratch>/pad.txt`, with (lx, ly) = neutral 128,128; up 128,0; down 128,255; left 0,128; right 255,128; diagonals 0,0 / 255,0 / 0,255 / 255,255.
- Then unlock and repeat neutral and down.
- During one locked side roll, watch whether the character keeps facing the target.
- Log: for each input, which move played (forward roll, back roll, left roll, right roll, backstep hop, nothing).
- Read against section 3.2: the static reading predicts forward roll for up, backstep for down (locked), back roll and right roll for the two sides, and a left roll for neutral. Known play says neutral is a backstep. The log shows which link is misread.

**4. Diagonal walk while locked on.**
- Do: locked on, walk up-left and down-right for a few seconds each.
- Log: whether the legs turn toward the movement, or the character crab-walks with one of four walk cycles.

## Appendix: addresses

All are read in code unless marked.

| Address | What |
|---|---|
| 0x01B4EF9C | pointer; PGD = `*(*0x01B4EF9C + 8)` |
| 0x00224B10 | soul level from the eight stats |
| 0x00226778 | cost of one level (CalcCorrectGraph row 200, K = 2.0) |
| 0x0094E508 | float helper that rounds the cost (round to nearest, inferred) |
| 0x00596F80 | CalcCorrectGraph access (param index 28) |
| 0x00597108, 0x00597388 | CharaInitParam access and getter (param index 18) |
| 0x002258D8, 0x00225890, 0x00226640 | base HP, MP, stamina from the stats |
| 0x002802E0, 0x002737A0, 0x00273620 | live character HP, MP, stamina refresh |
| 0x008F6680, 0x008F65E8 | slot counts (Intelligence; Faith) |
| 0x009060E0 | unattune one spell (the reset calls it for each entry of two 7-entry lists) |
| 0x00919D60 | current values from max |
| 0x0090FCD0 | reset stats to the class (caller 0x004171F8) |
| 0x0090F508 | one level down (callers 0x004159A8, 0x0041A420, 0x002D5308) |
| 0x0090EE78, 0x00910BA8, 0x00919F60 | all stats 10; new character; penalty flags clear |
| 0x007DC418 | level-up screen |
| 0x007E5810, 0x007E4400 | level-up preview; helper |
| 0x007E4A50, 0x007E4248 | level-up commit; its live refresh of HP, MP and stamina (no arguments) |
| 0x0026FB20, 0x0026FC48, 0x0026FC98 | character: change HP by a delta; set current MP; set current stamina (used by 0x007E4248) |
| 0x016FF700 | souls cap, 999,999,999 |
| 0x0091B888 | AddSoul(pgd, amount): clamp to 0..999,999,999 with 32-bit compares; total-souls update |
| 0x00723E30 | HUD souls counter (amount, delta) |
| 0x002D8978, 0x002D8B08 | SpEffect apply paths that add souls |
| 0x00376E68 | souls summed over active effects |
| 0x002D0860, 0x0053B670, 0x0053E578 | other AddSoul callers |
| 0x002284F0, 0x0081FBD0 | CanUseGoods; usable gate in the menu |
| 0x0081DEC0 | inventory command menu |
| 0x00900920, 0x00900A40, 0x00900B28, 0x008FB4F0 | item list find, remove, insert, add |
| 0x008FD2A8 | item count change (inventory, index, delta, flag); Use's consume (0x008FDB00) passes -1, Drop -n |
| 0x008F50A8 | removes an equipment item (Drop's path for weapons) |
| 0x0081E110 | command menu choice (vtable +0x7C): Use sends 2004, Drop 2001 |
| 0x008219E0 | inventory message handler (vtable +0x6C): 2000 picker OK (drop), 2001 Drop, 2004 Use |
| 0x00821010 | Drop's drop of the picker's number (from 2000) |
| 0x007731F0 | opens the count picker or a yes/no prompt: (message, 1, 2, n, 1 for a count, n, page); Drop passes the stack for both n |
| 0x00720F50, 0x00720F20 | menu slot set and get (+0x14 + 4n of the menus' shared object) |
| 0x00723708 | closes the menu pages (Use's) |
| 0x002DD368 | character: use an item (effect 0x00278E90, then consume 0x008FDB00 at PGD+0x2CC) |
| 0x002D8978 | character: gain souls (AddSoul and the HUD's counter) |
| 0x00595B80 | behavior param getter (table 10) |
| 0x005A5920 | param table by number (+0x20: the file as loaded) |
| 0x0059E3A8 | goods param getter (param index 3) |
| 0x005A0030 | MoveParam getter (param index 17) |
| 0x0059FF98 | escape getter: k 0-3 = F, B, L, R; 4 = stepMove (jump table 0x0059FFE0) |
| 0x002EB3E8 | script function 110: move-set row (ctrl+0x194, ids at ctrl+0x17C) to escape anim |
| 0x002FF5C0 | script function dispatcher (r22 script wrapper, r29 controller, r25 character) |
| 0x00302008 | script command dispatcher; (2,3) play-anim handler 0x00303C84 |
| 0x002F8E60 | script function 100: slot set and not consumed |
| 0x002F7D68 | script function 108: slot buffered and its window open |
| 0x002FC9A8 | closes the input windows (wrapper +0x3E4, 35 bytes) |
| 0x003008C8 | script function 1001: current stamina (chr+0x3D4) |
| 0x002758C8 | animation id resolve (bank x 10000 + id) |
| 0x003103F8 | pad manipulator update; calls 0x0030F3C0 (0x003109EC) and 0x0030E750 (0x00310A60) |
| 0x0030E750 | roll and sprint input: history, four directions, slot bytes |
| 0x0030E8B0 | per-frame larger-axis pick |
| 0x0030EA34 | larger-axis selection on the history (the 4-way quantization) |
| 0x0030EC44-0x0030F2C0 | stick transform (not decoded) |
| 0x0030EC08, 0x0030F2CC, 0x0030F304, 0x0030F38C, 0x0030F39C | stores of slots 17, 18, 15, 10, 16 |
| 0x0030F3C0 | facing update (locked branch aims at the target) |
| 0x0030F734 | `fadds f1, f1, f0` in it: the locked branch's yaw toward the target (r26 = M); `des_roll_dir` hook |
| 0x002FF8F0, 0x00301C24 | in function 110, just before its two calls of 0x002EB3E8 (r3 = M, r4 = k); `des_roll_dir` hooks |
| M+0x04 | X: +0x100 has a target (byte), +0x110 the target's position |
| M+0x10 | P, the transform: +0x00 pitch, +0x04 yaw (the facing's bearing + pi), +0x10 position |
| M+0x139 | 00 locked on, 01 not |
| 0x001C3000 | stick vector from two analog ids |
| 0x001B6960, 0x001B6998 | left-stick getters |
| 0x001B9A10, 0x001B9D68 | button word +0x122 bit 6 and bit 5 (inferred: Circle released, Circle held) |
| 0x01B3FCAC | roll threshold T (.bss; 0.0 in retail, inferred) |
| 0x00312060 | builds a menu item that holds T's address (inferred: a debug tunable) |
| 0x019DA308, 0x019DA30C, 0x019DA310 | history length 6; neutral limit 0.45; sprint hold 0.4 s |
| 0x00271A68 | SpEffect state 15 check |
| 0x01B3D6E0 | `kChrMan`; +0x1BC = player character |
| 0x0185C338, 0x003259F8, 0x00326B18 | player locomotion controller vtable; update; reset with the leg twist |
| 0x00322428, 0x003228B0 | that controller's constructor; init (finds the bones Upper_Root, Master, Neck) |
