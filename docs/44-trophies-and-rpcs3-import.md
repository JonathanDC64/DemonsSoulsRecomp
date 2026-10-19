# Trophies and importing from RPCS3 (spike, 2026-10-18)

A docs-only spike. Nothing was built or run. Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`), the release the port uses. The lift (`work/recomp`) was searched with grep only. Line numbers are for the port's master at 495d5a7 and ps3recomp's fork master at f15a4f0.

Two labels are used below:
- **(read)**: I read it in our source, in the lift, in the RPCS3 source, or in the layout of an RPCS3 install. From the install I took file names, file sizes and config key names only, no values.
- **(inferred)**: my conclusion, not yet checked in a run. The run requests in section 12 check these.

This document has no trophy names, descriptions or icons. They stay on the player's disc.

## Summary

- **Trophies are doable in about 5.5 to 6.5 days.** The game already drives the whole API: 10 sceNpTrophy imports, a manager thread (`DsTrophyMan::AsyncUpdateThread`) and one `UnlockTrophy` call site (0x0005F2A4). Today it boots through `Init`, `CreateContext("NPWR00881")`, `CreateHandle` and `RegisterContext`, in our port and in RPCS3.
- **Our HLE stores unlocks but returns made-up data, in the wrong byte order.** File: `ps3recomp/libs/network/sceNpTrophy.c`.
  - `GetGameInfo` reports 32 trophies. The disc has 38.
  - Struct fields are written host-endian, so the game would read 32 as 536,870,912.
  - The description field is 256 bytes instead of 1,024, so the hidden flag sits at the wrong offset.
  - Error codes are 0x805516xx instead of 0x800229xx.
  - The platinum is never awarded.
- **This matters even without any UI.** Right after an unlock the game calls `GetGameInfo` (0x0005F310) and `GetTrophyInfo` (0x0005F670, and 0x0005F390 for the platinum), and keeps the results. Neither our logs nor the RPCS3 logs I have contain an unlock. So this path has never run in the port (inferred risk).
- **The trophy data comes from the player's disc at run time.** The file is `PS3_GAME/TROPDIR/NPWR00881_00/TROPHY.TRP` (read):
  - 2,578,992 B, version 1, 41 entries: TROPCONF.SFM, TROP.SFM, ICON0.PNG and 38 TROPnnn.PNG.
  - 38 trophies: 1 platinum, 5 gold, 5 silver, 27 bronze. None are hidden. The platinum is id 0 and the other 37 count toward it.
  - The container is plain: no encryption, and no SHA-1 check in version 1. Nothing has to be shipped or extracted.
- **Plan for trophies:**
  1. ps3recomp: read the TRP through the VFS and parse TROP.SFM. Fix the structs, the byte order and the error codes. Add the platinum rule and PS3 tick timestamps. Keep the JSON unlock file (`gamedata/trophies/NPWR00881.json`). Add an unlock hook and a read-only snapshot for the UI.
  2. Port: a trophy toast with its icon on the existing passive overlay (thread-safe queue, wake flag, PNG decoding through WIC). A "Trophy pop-ups" setting on the Interface tab. A "Trophies" window (menu bar tab, 38 rows).
  3. No sync and no network.
- **Import from RPCS3 is doable in about 4.5 to 5.5 days.** RPCS3 uses the same save folder name (`BLUS30443DEMONSS005`) and stores the data files raw, like ours:
  - the data files are the same size in both trees (262,144 B each), and so are ICON0.PNG and PIC1.PNG;
  - only the metadata differs. RPCS3 writes a binary PARAM.SFO and marks secure files inside it (`*<file>` = 1). We write a text PARAM.SFO and list secure files in a text PARAM.PFD;
  - so the import converts two small files and copies the rest.
- **The import has to land before the game touches the save.** The game boots behind the start screen and waits at its auto-save notice. It reads and writes the save only after OK (docs/27). So:
  - apply the import at once if cellSaveData has not been called yet in this session;
  - otherwise stage the files and apply them at the next start, before the game runs;
  - always back up the current save first, and never delete the backup.
- **Settings and patches map in part.** Direct matches: resolution scale, anisotropic filtering, V-sync, the frame limit together with the "Unlock FPS" patch, "Disable Motion Blur", "Skip Intro Videos" and "Show trophy popups". The aspect ratio patches wait for the ultrawide work (docs/39). Input bindings come later.
- **Recommendation:**
  - Do trophies first, starting with the HLE fixes. They protect the first real unlock.
  - Then the import, saves first. Trophies, settings and patches go in the same dialog.
  - The ps3recomp PRs go first; the port PRs depend on them.

## 1. What the game calls

### 1.1 Imports (read)

All 10 are stubs in `work/recomp/ppu_recomp_011.cpp` that call `ps3_hle_call(NID)`. RPCS3's loader log lists the same 10 at the same stub addresses.

| NID | Function | Stub |
|---|---|---|
| 0x39567781 | sceNpTrophyInit | 0x016B8944 |
| 0xE3BF9A28 | sceNpTrophyCreateContext | 0x016B89E4 |
| 0x1C25470D | sceNpTrophyCreateHandle | 0x016B8904 |
| 0x1197B52C | sceNpTrophyRegisterContext | 0x016B88E4 |
| 0x49D18217 | sceNpTrophyGetGameInfo | 0x016B8964 |
| 0xFCE6D30A | sceNpTrophyGetTrophyInfo | 0x016B8A04 |
| 0x8CEEDD21 | sceNpTrophyUnlockTrophy | 0x016B89A4 |
| 0x623CD2DC | sceNpTrophyDestroyHandle | 0x016B8984 |
| 0x3741ECC7 | sceNpTrophyDestroyContext | 0x016B8924 |
| 0xA7FABF4D | sceNpTrophyTerm | 0x016B89C4 |

The game does not import the icon calls (`GetGameIcon`, `GetTrophyIcon`), `GetRequiredDiskSpace`, `GetTrophyUnlockState` or `GetGameProgress`. Only a UI of ours needs icons.

The save-related imports, for section 6: `cellSaveDataAutoLoad2` (0xFBD5C856), `cellSaveDataAutoSave2` (0x8B7ED64B), `cellGameDataCheckCreate2`, `cellUserInfoGetList`, `cellUserInfoGetStat` and `cellSysutilGetSystemParamInt`.

### 1.2 The flow (read)

The game's trophy class is `DsTrophyMan`. RPCS3's log names its thread `DsTrophyMan::AsyncUpdateThread`. The addresses below are the return addresses (LR) of the calls; RPCS3's log shows the same LRs.

1. **Init** (`func_0005E060`, called from 0x00021094):
   - loads sysmodule 0xF035;
   - `Init` (LR 0x0005E160);
   - `CreateContext` (LR 0x0005E1A0), with the communication id at 0x016BC290 ("NPWR00881", number 0) and the signature at 0x016BC1F0;
   - `CreateHandle` (LR 0x0005E254).
   - A second function, `func_0005E290`, makes the same three calls (LR 0x0005E390, 0x0005E3D0, 0x0005E484).
2. **Register**, on the manager thread (`func_0005F180`, message type 0):
   - `RegisterContext` (LR 0x0005F474), with the status callback at 0x0191A630.
   - When the call returns 0, the game writes status 8 (COMPLETE) at +0x4C itself (docs/27).
   - This is the "Checking Trophy Set" screen after the auto-save notice.
3. **Per frame** (`func_0005E888`, called from 0x0001B9B8):
   - it checks for error 0x80022928 (INSUFFICIENT_DISK_SPACE) and then sets +0x64 = 3;
   - in another state it calls `GetGameInfo` (LR 0x0005EB54). It stores numTrophies at +0x7C, and as a float at +0x6C. It allocates a table of numTrophies x 8 bytes at +0x80 and fills it entry by entry.
   - Neither log shows this call, so I don't know when that state is reached.
4. **Unlock** (message type 1):
   - `UnlockTrophy(ctx = [+8], handle = [+0xC], id = payload+4, &platinumId)` (LR 0x0005F2A4). The return value is not checked. Then it sets payload+9 (done).
   - `GetGameInfo` (LR 0x0005F310). The GameDetails buffer is cleared with 0x498 = 1,176 bytes first, RPCS3's size for it. The unlocked count goes to +0x74, and a float ratio to +0x70.
   - If 0 <= id < [+0x7C]: `GetTrophyInfo(id)` (LR 0x0005F670). Then table[id] = {Data.unlocked, Details.trophyGrade}.
   - If platinumId is not -1: `GetTrophyInfo(platinumId)` (LR 0x0005F390), stored the same way.
5. **Refresh** (message type 3): `GetTrophyInfo(i)` for each trophy (LR 0x0005F56C), into the same table.
6. **Exit** (message type 2) and teardown:
   - `DestroyHandle`, `DestroyContext` and `Term` are called from three places (`func_0005DA68`, `func_0005E4C0`, `func_0005EDD8`; the LRs are in the appendix);
   - then sysmodule 0xF035 is unloaded.

What the logs show (read):
- RPCS3, two boots:
  - Init, CreateContext, CreateHandle;
  - RegisterContext, with the callback at status 3 (INSTALLED), about 6 s after Init;
  - DestroyHandle, DestroyContext and Term at exit;
  - no GetGameInfo, GetTrophyInfo or UnlockTrophy.
- Our port (`work/tmp/play.txt`):
  - Init; CreateContext gives context 1; CreateHandle gives handle 1;
  - RegisterContext prints "loaded saved data";
  - no unlock.

### 1.3 When the game unlocks

- (read) Only one place calls `UnlockTrophy`: the manager thread, for a type-1 message. The trophy id is the 32-bit value at offset 4 of the message's payload.
- (not determined) Which game code posts the type-1 messages. The thread waits on a queue (calls between 0x0005F214 and 0x0005F258). I did not trace the producers. Several functions near the class have no direct callers; they are reached through a vtable or a function pointer.
- For the port this matters little: the game decides when, and the HLE only has to answer correctly. It matters for testing: a test of an unlock needs a save close to a trophy (run request R1).

## 2. Our HLE today (read)

File: `ps3recomp/libs/network/sceNpTrophy.c`. It is the same on the fork's master.

- **State:** one JSON file per communication id, `gamedata/trophies/NPWR00881.json` (`s_storage_path`, line 42). The path is relative to the working folder, like the saves. The content is `{"trophies": [{"id": N, "timestamp": T}]}`. It is written at once on each unlock.
- **`CreateContext`** sets 128 trophies (`SCE_NP_TROPHY_MAX_NUM_TROPHIES`, line 233).
- **`RegisterContext`** (line 295) loads the JSON, calls the status callback with INSTALLED at once, and queues PROCESSING_COMPLETE through `cellSysutilQueueGuestCallbackArgs`. That works for this game (docs/27).
- **`GetGameInfo`** (line 376) returns fixed numbers: 32 trophies (1 platinum, 2 gold, 8 silver, 21 bronze) and the title "PS3 Game". It reports every unlock as bronze.
- **`GetTrophyInfo`** (line 423) returns the name "Trophy N", always bronze, never hidden.
- **`UnlockTrophy`** (line 467) marks the trophy, saves, prints a banner, and always returns platinumId = -1.

Problems, measured against RPCS3's `sceNpTrophy.h` and what the game expects:

| Problem | Ours | Needed |
|---|---|---|
| Byte order | struct fields written as host u32/u64 through `GUEST_PTR` | big-endian. Only `platinumId` (through `vm_write32`) and `GetRequiredDiskSpace` are right today |
| SceNpTrophyDetails | description[256]: 396 B, `hidden` at +392 | description[1024]: 1,164 B, `hidden` at +1160 |
| SceNpTrophyGameDetails | 1,172 B | 1,176 B (reserved[4] at the end). The game clears 1,176 |
| Error codes | 0x805516xx | 0x800229xx, for example 0x80022928 (INSUFFICIENT_DISK_SPACE), which the game checks |
| Timestamp | Unix seconds | a PS3 tick (CellRtcTick: microseconds since year 1), as RPCS3 stores it |
| Counts and names | fixed 32 / 128, "Trophy N" | from the trophy set: 38, with names |
| Platinum | never | unlocked when every trophy linked to it (pid) is unlocked; its id goes back in platinumId |
| Errors on unlock | ALREADY_UNLOCKED only | also CANNOT_UNLOCK_PLATINUM (0x80022914); GetTrophyInfo returns HIDDEN (0x80022913) for a hidden, locked trophy |

Every title ps3recomp runs shares this code. The fixes stay generic: no Demon's Souls ids in it.

## 3. The trophy set on the disc (read)

- **Path:** `PS3_GAME/TROPDIR/NPWR00881_00/TROPHY.TRP` under the disc root (`PS3_VFS_ROOT`). In the guest that is `/dev_bdvd/PS3_GAME/TROPDIR/NPWR00881_00/TROPHY.TRP`. The folder name is the communication id, `_`, and its number as two digits, the way RPCS3 builds it.
- **Container** (RPCS3 `Loader/TRP.h`):
  - a 0x40-byte header: magic 0xDCA24D00, version, file size, entry count, entry size, a dev flag, and a SHA-1 that only version 2 and later check;
  - then 64-byte entries: name[32], offset (u64), size (u64), unknown (u32);
  - the files are stored uncompressed.
- **This disc:**
  - version 1, 2,578,992 B, 41 entries;
  - TROPCONF.SFM (2,509 B), TROP.SFM (6,298 B), ICON0.PNG (112,321 B), TROP000.PNG to TROP037.PNG;
  - no TROP_xx.SFM, so the set is English only.
- **SFM files** are XML behind a signature comment. TROP.SFM holds the title and, for each trophy: id, hidden, ttype (P, G, S or B), pid (the platinum it counts toward), name and detail. RPCS3 installs TROP_<lang>.SFM, or else TROP.SFM, under the name TROPCONF.SFM, and reads everything from it.
- **Counts** (read from RPCS3's installed copy of TROP.SFM; tags and attributes only):
  - 38 trophies: P 1, G 5, S 5, B 27;
  - 0 hidden;
  - pid -1 on the platinum and pid 000 on the other 37. So the platinum is id 0, and it unlocks together with the last of the other 37.

## 4. How RPCS3 does it (read)

Files: `rpcs3/Emu/Cell/Modules/sceNpTrophy.cpp`, `rpcs3/Loader/TRP.cpp`, `rpcs3/Loader/TROPUSR.cpp` and `rpcs3/Emu/RSX/Overlays/overlay_trophy_notification.cpp`.

- **`CreateContext`:** the name is `<9-character id>_<number as %02d>`. It checks the signature's magic, 0xB9DDE13B.
- **`RegisterContext`:**
  - finds `TROPDIR/<name>/TROPHY.TRP` in the game folder, or else on /dev_bdvd;
  - installs it to `/dev_hdd0/home/<user>/trophy/<name>/`, through a temporary folder that keeps the existing TROPUSR.DAT;
  - renames TROP_<lang>.SFM, or else TROP.SFM, to TROPCONF.SFM;
  - loads TROPUSR.DAT, or generates it;
  - then sends the status callbacks (SETUP, PROGRESS, FINALIZE, COMPLETE), spread over about 2 s.
- **`GetGameInfo` and `GetTrophyInfo`** parse TROPCONF.SFM. A hidden, locked trophy returns HIDDEN.
- **`UnlockTrophy`:**
  - refuses the platinum (CANNOT_UNLOCK_PLATINUM) and repeats (ALREADY_UNLOCKED);
  - stamps the unlock with `cellRtcGetCurrentTick`;
  - unlocks the platinum when its last linked trophy falls (`GetUnlockedPlatinumID`), and returns its id;
  - saves TROPUSR.DAT;
  - shows a notification for the trophy, and one for the platinum, when `Show trophy popups` is on.
- **TROPUSR.DAT** is big-endian:
  - a 48-byte header (magic 0x818F54AD, table count) and 32-byte table headers (type, entry size, count, offset);
  - RPCS3 uses two tables: type 4 (80-byte entries: id, grade, pid) and type 6 (96-byte entries: id, state 0 or 1, two timestamps);
  - `GetTrophyTimestamp` returns the second timestamp.
- **The notification:**
  - about 5 s on screen, with a 1.5 s slide and a 1 s fade;
  - one at a time, through a queue;
  - the system's trophy sound, the icon (TROPnnn.PNG), and "You have earned the <grade> trophy" with the name.

## 5. Proposed trophy design

### 5.1 HLE (ps3recomp)

- **Find the trophy set** at `RegisterContext`, or earlier when the UI asks:
  1. build the guest path `/dev_bdvd/PS3_GAME/TROPDIR/<id>_<nn>/TROPHY.TRP`. A PSN title keeps TROPDIR in its game folder, so try `/dev_hdd0/game/<title id>/TROPDIR/...` first, as RPCS3 does;
  2. translate the path the way the game's own file reads are translated (`sys_fs_translate_path`, `ps3recomp/runtime/syscalls/sys_fs.c`);
  3. read the entry table, then TROP_<lang>.SFM if it exists, else TROP.SFM;
  4. parse it with a small tag scanner: title-name, and for each trophy its attributes, name and detail. Handle XML entities and UTF-8;
  5. keep the icon offsets, and read the icons on demand.
- **No TRP found:** keep today's fixed set, with the byte order fixed, and log it once.
- **Structs:** RPCS3's sizes. Write fields with `vm_write32` / `vm_write64` and strings as byte arrays. Error codes 0x800229xx. Timestamps as a PS3 tick: Unix microseconds + 62,135,596,800,000,000.
- **`UnlockTrophy`:**
  1. return ALREADY_UNLOCKED (0x80022915) for a repeat, and CANNOT_UNLOCK_PLATINUM (0x80022914) for the platinum itself;
  2. mark the trophy;
  3. if it has a pid >= 0 and every trophy with that pid is now unlocked, unlock the platinum too, and return its id in platinumId;
  4. save once.
- **Status callbacks:** keep them as they are. The game sets COMPLETE itself (docs/27). RPCS3's longer sequence is not needed.
- **Two small C APIs for the port:**
  - `sceNpTrophy_set_unlock_hook(fn)`: called after each unlock, and again for a platinum, with the id, grade, name and detail. It runs on the guest thread, so the hook must only queue.
  - `sceNpTrophy_snapshot(...)`: the title, the counts, and for each trophy its id, grade, hidden flag, name, detail, unlock state and time. Plus `sceNpTrophy_icon(id, &bytes, &size)`. Both take a mutex. They work before `RegisterContext` (they parse the TRP on demand), so the trophy list also works from the start screen.

### 5.2 Storage

- **Keep the JSON** (`gamedata/trophies/NPWR00881.json`). It already exists, it sits beside the saves, and it is generic.
- Write it through a temporary file and a rename, as the save code does (`cellSaveData.c`, `MoveFileEx`).
- Keep Unix seconds in the file, and convert to a PS3 tick when the game asks.
- Later, if wanted: also write an RPCS3-format TROPUSR.DAT, so the folder could go back to RPCS3. The import does not need it; it only reads.

### 5.3 Notification

What exists (read):
- the overlay has a toast queue: `create_toast` (`port/ui/src/overlay.cpp`, line 31) and `Overlay::update` (line 118);
- the overlay document is passive, so it never takes input (`des_ui.cpp`, line 391);
- `ui::push_toast` (`ui.cpp`, line 353) has no callers and is not thread-safe.

What is needed:
1. A mutex-guarded queue, fed by the unlock hook, and an atomic flag that des_ui's fast path checks. Today `frame()` returns early when nothing shows (`des_ui.cpp`, line 512); a waiting toast must count as something.
2. Toast markup: the icon on the left; "Trophy unlocked" with the grade; the name. About 5 s on screen like RPCS3, one at a time. The platinum comes right after the last trophy.
3. The icon. RmlUi's DX11 renderer loads TGA only. Subclass it (`host.cpp`, line 242, creates it) and decode PNG through WIC, which is already linked for the PNG encoder in `report.cpp`. Cache the decoded icons.
4. Placement: top right, where the game has no HUD (inferred).

Sound: RPCS3 plays the firmware's trophy sound, and we ship no firmware. Use one of the game's own sounds (`port/des_menu_sfx.cpp` already plays a few), or none. Not decided.

### 5.4 Trophy list window

- Entry points:
  - a "Trophies" tab in the menu bar (`menu_bar.cpp` has Settings, Report an issue, Quit);
  - a "Trophies" button on the start screen (`prelaunch.cpp` has Play, Settings, Quit).
- Header: "N of 38 (P %)" and the count per grade.
- 38 rows, each with the icon, name, detail, grade, and the unlock date or "Locked".
- A hidden trophy shows "Hidden trophy" until it is unlocked. This game has none, but it costs little.
- Pad and keyboard navigation like the settings window.

### 5.5 Setting

- Interface tab: "Trophy pop-ups" (`trophy_popups`, UI-only, on by default).
- Off hides the toast only: unlocks are still kept and listed.

### 5.6 Sync

None. No PSN, no RPCN, no network. The unlock file is local.

## 6. RPCS3: where things are

### 6.1 Finding the install (read)

- RPCS3 on Windows is a folder the player unpacks anywhere. I found no installer or registry entry in the code that finds its folder.
- The config folder (`fs::get_config_dir`, `Utilities/File.cpp`):
  1. `<exe folder>/portable/`, if it exists;
  2. else `RPCS3_CONFIG_DIR`, if set;
  3. else the exe folder.
  - On Linux it is `$XDG_CONFIG_HOME/rpcs3/` or `~/.config/rpcs3/`, and on macOS `~/Library/Application Support/rpcs3/`. That is for later ports.
- The config files sit in its `config/` subfolder:
  - `config.yml`;
  - `custom_configs/config_BLUS30443.yml` (`system_utils.cpp`, lines 568-580);
  - `patch_config.yml`;
  - `games.yml`: serial to game folder. It has a BLUS30443 line when the game is in the library;
  - `vfs.yml`, which is optional;
  - `input_configs/`.
- `/dev_hdd0` is `$(EmulatorDir)dev_hdd0/` unless `vfs.yml` says otherwise. `$(EmulatorDir)` defaults to the config folder (`Emu/vfs_config.h`). The install I looked at has no vfs.yml.
- Our tools already call this folder `RPCS3_DIR` (`tools/des/local.env.example`, read by `rlog.sh`). And every player has RPCS3: the README uses it to decrypt EBOOT.BIN.

### 6.2 Saves (read)

- RPCS3: `<dev_hdd0>/home/<user>/savedata/BLUS30443DEMONSS005/`. The user is 8 digits, 00000001 by default. RPCS3 can have several users.
- Ours: `gamedata/dev_hdd0/home/00000001/savedata/BLUS30443DEMONSS005/` (`ps3recomp/libs/system/cellSaveData.c`, line 53, relative to the working folder).
- One folder holds the game's system data and all characters: one base data file plus numbered ones (inferred from the names).

Compared by names and sizes only:

| | RPCS3 | Ours |
|---|---|---|
| Data files | raw, 262,144 B each | raw, 262,144 B each |
| ICON0.PNG, PIC1.PNG | 107,203 B and 847,976 B | the same sizes |
| PARAM.SFO | binary PSF, 3,244 B | text, 5 lines, 72 B |
| PARAM.PFD | none | text, one secure file name per line |
| Secure files | `*<file>` = 1 keys in PARAM.SFO | listed in PARAM.PFD |
| File order | `RPCS3_BLIST` key | directory order |

- RPCS3 writes secure files raw; `secureFileId` is only logged (`cellSaveData.cpp`, line 1939). We write them raw too. So the data files should work as they are (inferred: the same game writes the same bytes. I did not compare contents, which would mean reading the saves).
- RPCS3's PSF format (`Loader/PSF.cpp`, `PSF.h`):
  - little-endian;
  - a 20-byte header: magic "\0PSF", version, key table offset, data table offset, count;
  - 16-byte entries: key offset (u16), format (u16: 0x0004 array, 0x0204 string, 0x0404 integer), length, maximum length, data offset;
  - the keys RPCS3 writes for a save (lines 1716-1726): ACCOUNT_ID, ATTRIBUTE, CATEGORY ("SD"), PARAMS, PARAMS2, PARENTAL_LEVEL, DETAIL, SAVEDATA_DIRECTORY, SAVEDATA_LIST_PARAM, SUB_TITLE and TITLE. Then one `*<file>` key per file, and RPCS3_BLIST.
- What ours reads:
  - `read_param_sfo` (line 783) takes only `TITLE=`, `SUB_TITLE=`, `DETAIL=`, `ATTRIBUTE=` and `LIST_PARAM=` lines;
  - `dir_has_save` (line 116) only checks that PARAM.SFO exists;
  - a file missing from PARAM.PFD is reported as NORMALFILE (`savedata_file_type`, line 555).
- So for an RPCS3 folder copied as it is, the save counts as present. But the game's stat callback gets empty title strings and every data file as NORMALFILE. Whether Demon's Souls accepts that is not known (run request R4).
- Converting avoids the question:
  - TITLE, SUB_TITLE, DETAIL, ATTRIBUTE and SAVEDATA_LIST_PARAM go into our text PARAM.SFO;
  - each `*<file>` = 1 key becomes a line in PARAM.PFD.
- Saves from PS3 hardware are a different case. Their secure files are encrypted and their PARAM.PFD is binary. Out of scope.

### 6.3 Trophies (read)

- `<dev_hdd0>/home/<user>/trophy/NPWR00881_00/` holds:
  - ICON0.PNG and the 38 icons;
  - TROPCONF.SFM (6,298 B: the disc's TROP.SFM under another name);
  - TROPUSR.DAT (8,016 B).
- RPCS3's own generator writes 6,800 B for 38 trophies: 48 + 2 x 32 + 38 x 80 + 38 x 96. This file is 1,216 B larger, so it holds more tables, perhaps from another source (inferred). A reader must walk the table headers and take type 6, as RPCS3 does.

### 6.4 Settings (read)

- The per-game file `config/custom_configs/config_BLUS30443.yml` is applied over `config/config.yml`.
- It is YAML with two-space indents. Sections: Audio, Core, Input/Output, Log, Miscellaneous, Net, Savestate, System, VFS, Video. Section 7 lists the keys that matter.
- The file also holds a console ID and the GPU's name. The import must not read, keep or log those.

### 6.5 Patches (read)

- The definitions are in `patches/patch.yml`, RPCS3's public patch file. For Demon's Souls (BLUS30443 01.00) it has:
  - Unlock FPS (version 2.1);
  - Skip Intro Videos;
  - Aspect Ratio, with a value: 32:9, three 21:9 sizes, 16:10, 4:3 or 5:4;
  - four "Aspect ratio (HUD/Menus)" variants: 32:9 and three 21:9 sizes. They need "Aspect Ratio" and RPCS3's "Stretch To Display Area";
  - Disable Motion Blur;
  - Enable FreeCam.
- Which patches are on is in `config/patch_config.yml`. The nesting is `PPU-<hash>` > patch name > game title > serial > version > `Enabled: true`. A patch with a value also gets `Configurable Values:` there (inferred from `Utilities/bin_patch.cpp`).
- Match by serial (BLUS30443) and version (01.00), not by hash.

## 7. Mapping RPCS3 to our settings

Key names are read from the RPCS3 source and config. The rules are proposals.

### 7.1 Settings

| RPCS3 key | Ours | Rule |
|---|---|---|
| Video: Resolution Scale (25-800 %) | Render resolution, `RSX_SCALE` | same meaning, a percentage of 1280x720. The backend takes 50-800; the menu offers 100, 150, 200 and 300, so pick the nearest of those |
| Video: Anisotropic Filter Override (0 = auto) | Anisotropic filtering, `RSX_ANISO` (0 = the game's own) | copy |
| Video: VSync Mode (older builds: VSync) | V-sync, `DES_VSYNC` | off / false gives 0, anything else 1 |
| Video: Frame limit, Second Frame Limit, Vblank Rate, and the "Unlock FPS" patch | Frame rate, `DES_FPS` (and `fps_custom`) | patch off: "" (the game's own 30). Patch on: the lowest of the frame limit, the second limit (if not 0) and the vblank rate. Off or Infinite count as no limit. With the default vblank of 60 that gives 60. A rate the menu lists is used as it is; any other goes through Custom |
| Video: Aspect ratio, Stretch To Display Area, and the "Aspect Ratio" patch | none yet | waits for docs/39 |
| Audio: Master Volume (0-200) | Sound, `AUDIO_MUTE` | 0 mutes. We have no volume setting |
| Miscellaneous: Show trophy popups | `trophy_popups` (new) | copy |
| Miscellaneous: Start games in fullscreen mode | none | skip |
| System: Language, Enter button assignment | none (cellSysutil always reports English (US) and Cross) | skip |
| Video: Resolution, MSAA, Renderer, Shader Mode, output scaling, CAS sharpening | none | skip |
| `input_configs` | `PAD_MAP`, `PAD_KEYS` | later; the names depend on the handler |

### 7.2 Patches

| RPCS3 patch | Ours |
|---|---|
| Unlock FPS | `DES_FPS` (above) |
| Skip Intro Videos | `DES_SKIP_INTRO=1`. Whether the patch also skips the opening movie is not determined, so `DES_SKIP_INTRO_MOVIE` stays as it is |
| Disable Motion Blur | `DES_MOTION_BLUR=0` |
| Aspect Ratio, with its value | the aspect setting from docs/39, once it exists |
| Aspect ratio (HUD/Menus) | HUD centering, docs/39 |
| Enable FreeCam | none |

## 8. The "Import from RPCS3" design

### 8.1 Where

- A button on the start screen. The game has not read the save yet there (inferred from docs/27; the counter in 8.4 makes sure).
- A button row on the Settings window's Game tab.
- In the in-game menu as well, but that path always ends in a restart.

### 8.2 Finding RPCS3

1. `RPCS3_DIR` in the environment;
2. else `rpcs3_dir` from settings.ini (new, UI-only, remembered after the first import);
3. else a folder picker (`IFileOpenDialog` with `FOS_PICKFOLDERS`): "Select the folder that contains rpcs3.exe".

Then:
- if `<folder>/portable/` exists, it is the config folder;
- accept the folder if it has `rpcs3.exe` or `config/config.yml`;
- read `config/vfs.yml` if it exists, for `$(EmulatorDir)` and `/dev_hdd0/`;
- never write anything in the RPCS3 folder.

### 8.3 What the dialog shows

One row per item, each with a checkbox:
- **Save:** each RPCS3 user that has a `BLUS30443DEMONSS005`, with the time of its newest data file and its file count. Ours, the same way. Off by default when ours is newer.
- **Trophies:** "N unlocked in RPCS3, M here, K new".
- **Settings and patches:** one list of changes, such as "Frame rate: 30 to 60". Rows set from the environment are shown but skipped.

Warnings:
- a `.working_BLUS30443DEMONSS005` or `.backup_BLUS30443DEMONSS005` folder next to the RPCS3 save (`cellSaveData.cpp`, line 1500) means RPCS3 is saving, or stopped in the middle of a save. Stop.
- if the newest file time changes during the copy, RPCS3 is running. Stop.

### 8.4 Save: back up, convert, apply

1. **Back up.** Copy the current save folder to `gamedata/save-backups/<date-time>/BLUS30443DEMONSS005/`. Never delete backups automatically. Stop if the copy fails.
2. **Stage.** Build the new folder in `gamedata/import/BLUS30443DEMONSS005/`:
   - copy the data files, ICON0.PNG and PIC1.PNG;
   - write a text PARAM.SFO from the PSF keys;
   - write PARAM.PFD from the `*<file>` = 1 keys;
   - open RPCS3's files read-only. Never move them.
   - Both folders are on the same drive as the saves, so the swap is two renames, and outside `savedata/`, so the game never lists them.
3. **Apply.**
   - If cellSaveData has not been called yet in this session, swap now: rename ours into the backup, then rename the staged folder in. This needs a new call counter in ps3recomp.
   - Otherwise write a marker (`gamedata/import/pending`) and offer "Restart now". The crash dialog's restart (`crash_report.cpp`) already relaunches with the saved command line. At the next start, before the game runs (next to `settings::load_and_apply_env()`), the port finds the marker and swaps. A toast then says "RPCS3 save imported; the old one is in save-backups".
4. **Undo.** A "Restore a backup" button lists the backups and swaps one back the same way.

Conflicts: one folder holds every character. Merging characters would need the game's slot table, which is out of scope. So the choice is all or nothing, and the backup keeps the other side.

Other regions use other folder names. The first version imports BLUS30443 only.

### 8.5 Trophies

- Read TROPUSR.DAT: walk the table headers, take type 6, keep state 1, and turn timestamp 2 from a PS3 tick into Unix seconds. Check that the count matches the TRP (38).
- Merge into our JSON as a union; the earlier time wins. Nothing is lost. Copy the JSON aside first anyway.
- Timing: before `RegisterContext`, merge at once. After it, merge through the HLE under its mutex. The game's own table catches up at its next refresh. The list window reads the HLE, so it is right at once.

### 8.6 Settings and patches

- A small YAML reader is enough for `config.yml`, the custom config and `patch_config.yml`: two-space indents, `key: value`, quoted keys. No new library.
- Read `config.yml`, then the custom config over it, then `patch_config.yml`.
- Apply through `ui::settings::set`. It skips keys set from the environment and saves settings.ini.

## 9. Effort

### 9.1 Trophies

| Step | Work | Where | Days |
|---|---|---|---|
| T1 | Structs, byte order, error codes, PS3 ticks; ALREADY_UNLOCKED, CANNOT_UNLOCK_PLATINUM, HIDDEN; the platinum rule | `ps3recomp/libs/network/sceNpTrophy.c` and `.h` | 1 |
| T2 | TRP reader through the VFS, SFM parser, fallback when there is no TRP; a unit test on a made-up TRP | ps3recomp, a new `trp.c` | 1 to 1.5 |
| T3 | Atomic JSON write; the hook, snapshot and icon APIs | ps3recomp | 0.5 |
| T4 | Toast: thread-safe queue, wake flag, markup and RCSS, PNG through WIC in a renderer subclass, the setting row | `port/ui/src/des_ui.cpp`, `overlay.cpp`, `host.cpp`, `settings.cpp`, `settings_window.cpp` | 1.5 |
| T5 | Trophies window, menu bar tab, start screen button | a new `port/ui/src/trophies_window.cpp`, `menu_bar.cpp`, `prelaunch.cpp` | 1 to 1.5 |
| T6 | A test knob that shows a fake toast (UI only, nothing saved); run requests R1 and R2 | port | 0.5 |

Total: about 5.5 to 6.5 days. T1 to T3 go in a ps3recomp PR first; the port PR depends on it.

### 9.2 Import

| Step | Work | Where | Days |
|---|---|---|---|
| I1 | Find RPCS3: environment, remembered path, folder picker, `portable/`, `vfs.yml`, checks | a new `port/ui/src/rpcs3_import.cpp` | 0.5 to 1 |
| I2 | PSF reader, save conversion, backup, staging, apply now or at the next start, the call counter, the restore list | port, plus a small ps3recomp API | 1.5 to 2 |
| I3 | TROPUSR reader and merge | port (or ps3recomp, next to T2) | 0.5 |
| I4 | YAML subset reader, settings and patch mapping, preview list | port/ui | 1 |
| I5 | Import dialog (checkboxes, found items, warnings, result), restart, start screen and Settings buttons | port/ui | 1 |
| I6 (optional) | cellSaveData also reads a binary PARAM.SFO and its `*<file>` keys, so a hand-copied RPCS3 folder works too | `ps3recomp/libs/system/cellSaveData.c` | 0.5 |
| | Input bindings from RPCS3's input configs | | later, about 1 |

Total: about 4.5 to 5.5 days, plus 0.5 for I6. The counter (and I6) go in a ps3recomp PR first.

Both features together: about 10 to 12 days.

## 10. Risks

- **The first real unlock (inferred).** It runs game code that has never run in the port: the type-1 path, and perhaps the table in `func_0005E888`. With today's HLE the game gets little-endian counts. If `func_0005E888` ever reads 32 as 536,870,912, it sizes its table at about 4 GB. T1 removes this. Until then, any unlock in a long session is a risk, with or without a UI.
- **Testing unlocks.** The producers are unknown, so a test needs a save next to a trophy (R1).
- **Shared HLE.** Every ps3recomp title uses sceNpTrophy. Real error codes and big-endian structs change what other games see. Those games got the same broken values before, but they need a re-run: Virtua Fighter 5, The Simpsons Game, Tornado Outbreak, Tokyo Jungle, Midway Arcade Origins.
- **Disc dumps without TROPDIR.** Some dump tools may leave it out (inferred). Those players get the fallback set: no names and no icons.
- **Threads.** Unlocks arrive on a guest thread and the UI draws on the render thread. The hook only queues.
- **Save import timing.** The game keeps its save in memory and autosaves often. A swap after the game has read the save would be overwritten by the game's next autosave. The counter plus staging covers this. A mistake here loses progress; the backup is the safety net.
- **Save compatibility (inferred).** The data files are raw in both, but nobody has loaded an RPCS3 save in the port yet (R3). A save written by a game update newer than 01.00, if there is one, is unchecked.
- **Partial copies.** RPCS3 running during the import, or a `.working_` folder. Warn and stop.
- **Settings that look alike but are not.** RPCS3's patch measures the frame time at the game's frame update; ours sets it at the flip. Map the number, not the behaviour.
- **Privacy.** RPCS3's config holds a console ID and the GPU's name. The reader skips them and logs key names, never values.
- **Legal.** Names and icons come from the player's disc at run time. Nothing is shipped, the same rule as the rest of the game data.

## 11. What I could not determine

- Which game events post unlock requests (the producers of type-1 messages).
- What the refresh message (type 3) is for, who posts it, and what `loc_0005F6BC` does after it.
- When `func_0005E888` reaches its `GetGameInfo` call. Neither log has it.
- Whether Demon's Souls accepts a save whose files come back as NORMALFILE, with empty title strings (an RPCS3 folder copied by hand).
- Whether RPCS3's "Skip Intro Videos" also skips the opening movie.
- Which sound to play for a trophy.
- Where the extra 1,216 B in RPCS3's TROPUSR.DAT come from.
- Whether a save from a later game update loads in 01.00.

## 12. Run requests

Use the usual run form, through Git Bash: `tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 [knobs]`.

R3 and R4 use a copy of an RPCS3 save, never the player's own save. Play from the copy the way `tools/des/levelcap.sh` does with `LC_SAVE=<dir>`: it puts the current save aside and restores it on exit. `<dir>` is a savedata folder that holds `BLUS30443DEMONSS005`.

**R1. First unlock with today's HLE (no build).**
- Use a save close to any trophy.
- Play until it unlocks.
- Log: the `[sceNpTrophy]` lines (the unlock banner, GetGameInfo, GetTrophyInfo), `gamedata/trophies/NPWR00881.json`, and what the player did at that moment.
- Expect: the banner and one GetGameInfo. No hang. This tells whether the byte-order bug bites today, and pairs one trophy id with one game event.

**R2. First unlock with the reworked HLE (after T1 to T5).**
- The same save and steps as R1.
- Expect:
  - GetGameInfo reports 38;
  - the toast shows with its icon and goes away after about 5 s;
  - the JSON holds the id;
  - the Trophies window lists it as unlocked, with the date.
- Then turn "Trophy pop-ups" off, unlock another trophy, and check that it is kept but not shown.

**R3. RPCS3 save, converted (after I2).**
- Import a copy of an RPCS3 save into a scratch savedata folder, then play from it with `DES_AUTO_CONTINUE=1`.
- Expect: the character loads; 2 minutes of play; the game autosaves without an error. The file sizes stay 262,144 B.

**R4. RPCS3 save, copied as it is (no build).**
- Copy an RPCS3 `BLUS30443DEMONSS005` folder unchanged (binary PARAM.SFO, no PARAM.PFD) into a scratch savedata folder, and play from it.
- Log: what the game shows, whether it loads, calls the save corrupt or offers to overwrite it, and the `[cellSaveData]` lines.
- This decides whether I6 is needed.

## Appendix: addresses

| Address | What |
|---|---|
| 0x016B88E4 to 0x016B8A04 | the 10 import stubs (table in 1.1) |
| 0x00021094 | the call to the init function `func_0005E060` |
| `func_0005E060` | init: sysmodule 0xF035; `Init` LR 0x0005E160; `CreateContext` LR 0x0005E1A0; `CreateHandle` LR 0x0005E254 |
| `func_0005E290` | the second init: LR 0x0005E390, 0x0005E3D0, 0x0005E484 |
| `func_0005DA68`, `func_0005E4C0`, `func_0005EDD8` | teardown: `DestroyHandle` LR 0x0005DDA0 / 0x0005E7F8 / 0x0005F0F0; `DestroyContext` LR 0x0005DDC0 / 0x0005E818 / 0x0005F110; `Term` LR 0x0005DDDC / 0x0005E834 / 0x0005F12C |
| 0x0001B9B8 | the call to the per-frame update `func_0005E888` |
| `func_0005E888` | per frame: the 0x80022928 check (+0x64 = 3); `GetGameInfo` LR 0x0005EB54; fills +0x7C, +0x6C and +0x80 |
| `func_0005F180` | the `DsTrophyMan::AsyncUpdateThread` loop: queue wait 0x0005F214 to 0x0005F258; `RegisterContext` LR 0x0005F474; `UnlockTrophy` LR 0x0005F2A4; `GetGameInfo` LR 0x0005F310; `GetTrophyInfo` LR 0x0005F670 (the unlocked id), 0x0005F390 (the platinum), 0x0005F56C (the refresh loop) |
| `loc_0005F6BC` | follows the refresh; not examined |
| 0x016BC290 | SceNpCommunicationId: "NPWR00881", number 0 |
| 0x016BC1F0 | SceNpCommunicationSignature |
| 0x0191A630 | the status callback (OPD) passed to `RegisterContext` |

`DsTrophyMan` fields:

| Offset | What |
|---|---|
| +0x08, +0x0C | the context and the handle |
| +0x4C | status; 8 = COMPLETE after registration |
| +0x64 | set to 3 on error 0x80022928 |
| +0x6C | numTrophies, as a float |
| +0x70 | unlocked ratio, as a float |
| +0x74 | unlocked count |
| +0x7C | numTrophies |
| +0x80 | the table: numTrophies x 8 bytes; byte 0 = unlocked, byte 1 = grade |

Message: the payload pointer is at +0x2C. In the payload, +4 is the trophy id and +9 is the done flag.
