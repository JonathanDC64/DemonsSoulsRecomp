# 59. Memory limits and mod support (2026-10-19)

Research only. I read the EBOOT and the lift, the ps3recomp runtime (fork master
`b249b7a`), RPCS3's source, one play log and the disc folder, plus some public web
pages. Nothing was run or built.

Markers:
- **[verified]**: seen in code, the EBOOT, a log or a file on disk.
- **[inferred]**: my conclusion, not tested.
- **[web]**: from a public page, not checked against code.

Naming:
- Lifted functions are `func_<EBOOT address>`.
- ps3recomp line numbers are at `b249b7a`.
- "The play log" is `work/tmp/play.txt`, a 2026-10-19 session of 29,613 lines.
- "The disc folder" is `PS3_VFS_ROOT`, the folder that holds `PS3_GAME`
  (tools/des/deslib.sh:6, :32).

## Summary

- **The Reddit claim is half right.** The limit is not the PC's RAM.
  - The game sets up fixed-size memory pools at boot and never asks for more.
  - When a pool runs out, its allocator reports it, then crashes on purpose. [verified]
- **15 blocks, all at boot.**
  - At boot the game asks the OS for 15 blocks, 163.9 MB in all. Nothing more is
    asked for in the whole session. [verified]
  - 13 of the blocks are Dantelion heaps made by the game's MemMan, 154.6 MB together.
  - The largest is MAIN at 86 MB. The Havok heap is 20 MB. [sizes verified, names inferred]
- **Every heap size is a constant in the EBOOT.**
  - Each heap creator holds three `lis` immediates with its size, 39 in all (table in 1.1).
  - The game reads the OS memory size only to check a request before it makes it.
  - So a bigger heap means changing those constants. [verified]
- **RPCS3's "Debug Console Mode" alone gives the game no bigger heaps.**
  - It adds 192 MB to what the OS will hand out, but changes none of the 39 constants.
  - It only makes room for an EBOOT whose constants were raised. [verified in RPCS3
    source; the effect on DeS inferred]
- **Room in the port today.**
  - The game uses 165 MB of the 256 MB guest window for these blocks.
  - So the heaps could grow by about 90 MB in total with no change to the address map.
  - Past that, the window must grow or move. [verified]
- **Textures live in RSX local memory.**
  - The port reports 256 MB of it; a PS3 has 249 MB. An SPU copy job fills it. [verified]
  - Render surfaces already reach 92 MB into it. [verified]
  - Not known: whether the game's VRAM allocator would use more if more were reported.
- **The /dev_hdd1 cache hides replaced files in the port.**
  - The port reads game files from the same folder layout RPCS3 uses, so it finds
    replaced files. [verified]
  - But the game copies almost every file it loads into its /dev_hdd1 cache, and the
    next boot reads the copy. The copy's name is an MD5 of the file's logical path,
    nothing else. [verified]
  - RPCS3 empties that cache at every boot; the port never does. So in the port, a
    replaced file is ignored until the cache is deleted. [verified chain, not run-tested]
- **Cheapest fixes, in order:**
  1. Clear the cache at mount, as RPCS3 does, or whenever the mod set changes.
  2. A `mods/` overlay in the runtime's path mapping.
  3. For patches: `patch.yml`-style code patches do nothing to lifted code. They need
     the EBOOT patched before the lift, or a port hook. Data patches can be applied
     after load. [mechanisms verified; costs inferred]

## 1. Question 1: the memory limits

### 1.1 Where the budgets come from

**OS memory at boot.** These are all of the session's `sys_memory_allocate` calls,
from play log lines 52-117 [verified]. None is freed, and none comes later.

| # | Size | Pages | Guest address | Owner |
|---|---|---|---|---|
| 1 | 0x5600000 (86 MB) | 1 MB | 0x40000000 | MAIN heap |
| 2 | 0x100000 | 1 MB | 0x45600000 | a 1 MB heap |
| 3 | 0xD00000 (13 MB) | 1 MB | 0x45700000 | HGSYS heap |
| 4 | 0x1400000 (20 MB) | 1 MB | 0x46400000 | HAVOK heap |
| 5 | 0x400000 (4 MB) | 1 MB | 0x47800000 | MO heap (sound) |
| 6 | 0x900000 (9 MB) | 1 MB | 0x47C00000 | FFX heap (effects) |
| 7-9 | 0x200000, 0x100000, 0x100000 | 1 MB | 0x48500000-0x488FFFFF | small heaps |
| 10 | 0x260000 | 64 KB | 0x48900000 | LUA heap |
| 11 | 0xC40000 (12.25 MB) | 64 KB | 0x48B60000 | CORERES heap |
| 12-13 | 0x100000, 0x200000 | 1 MB | 0x49800000, 0x49900000 | small heaps |
| 14 | 0x240000 | 64 KB | 0x49B00000 | not traced |
| 15 | 0x700000 (7 MB) | 1 MB | 0x49E00000 | GCM IO area: `_cellGcmInitBody(cmdSize=0x5FF000, ioSize=0x700000)`, play log line 118 |

The highest address used is 0x4A500000.

**The 13 MemMan heaps.** Each creator runs once during static init, from the MemMan
init `func_000433F0`, which fills the heap table at 0x01AD62C8.

Each creator does three things:
1. It calls `func_00C35520(size, page flag)` (flag 8 = 1 MB pages, 2 = 64 KB).
2. It builds a DLStaticHeap over the block.
3. It passes the same size again to the heap's strategy init, on two code paths.

So the size appears three times, always as a `lis` immediate. The table lists the
addresses. Sizes and addresses are [verified] by scanning the EBOOT and matching the
lift. The names are [inferred]: they come from pairing the init's table entries with
the heap report's name switch in `func_000303C0`.

| Heap | Size | Creator | Alloc `lis` | Strategy-init `lis` |
|---|---|---|---|---|
| MAIN | 0x5600000 (86 MB) | func_0003FBB8 | 0x0003FBFC | 0x0003FC44, 0x0003FCB0 |
| HAVOK | 0x1400000 (20 MB) | func_000319D0 | 0x00031A14 | 0x00031A5C, 0x00031AC8 |
| HGSYS | 0xD00000 (13 MB) | func_0003D7B8 | 0x0003D7FC | 0x0003D844, 0x0003D8B0 |
| CORERES | 0xC40000 (12.25 MB, 64 KB pages) | func_0003B3B8 | 0x0003B3FC | 0x0003B444, 0x0003B4B0 |
| FFX | 0x900000 (9 MB) | func_0003C5B8 | 0x0003C5FC | 0x0003C644, 0x0003C6B0 |
| MO | 0x400000 (4 MB) | func_00031638 | 0x0003167C | 0x000316C4, 0x00031730 |
| LUA | 0x260000 (2.4 MB, 64 KB pages) | func_00038FB8 | 0x00038FFC | 0x00039044, 0x000390B0 |
| MENU | 0x200000 (2 MB) | func_00041FB8 | 0x00041FFC | 0x00042044, 0x000420B0 |
| DEBUG | 0x200000 (2 MB) | func_0003A1B8 | 0x0003A1FC | 0x0003A244, 0x0003A2B0 |
| INGAME | 0x100000 (1 MB) | func_0003E9B8 | 0x0003E9FC | 0x0003EA44, 0x0003EAB0 |
| SERVER | 0x100000 | func_00040DB8 | 0x00040DFC | 0x00040E44, 0x00040EB0 |
| NETWORK (NRSYS reads the same entry) | 0x100000 | func_00037BF0 | 0x00037C34 | 0x00037C7C, 0x00037CE8 |
| TEMP | 0x100000 | func_00031830 | 0x00031874 | 0x000318BC, 0x00031928 |

**How a size is set.** It is a constant in code. The game reads no file for it, and
does not derive it from the OS. [verified]
- The OS-memory path is `func_00C35520` -> `func_00C36C80`, the DL page allocator.
- `func_00C36C80` calls `sys_memory_get_user_memory_size` (syscall 352). It checks
  "available minus a reserve kept at `this+0x160`" against the request.
- Then it calls `sys_memory_allocate` at 0x00C378B4.
- The reported size only gates a request. It never sizes one.
- The same 86 MB request at 0x00C378B4 shows in an RPCS3 boot log, where RPCS3
  reported 213 MB total (docs/07). The sizes do not depend on the platform. [verified]

**Other budgets** (not sized here):
- DL_RUNTIME and DL_SYSTEM: Dantelion's own heaps, reported through `func_00C33878`.
  - The EBOOT holds "Given system heap size is 0.Maybe an invalid parameter is given
    to DL_SET_SYSTEM_HEAP_SIZE()?", so that size is set in code too. [inferred]
- MO_SYSTEM, MO_WAVE and MO_SOUND: the sound engine's sub-heaps, through
  `func_00DB96C0`. Probably carved from MO. [inferred]
- LOCAL (heap ID -100, object 0x01AD6364) and MAPPED (ID -101): named by the report
  function, not traced. LOCAL is likely the VRAM heap. [inferred]
- The game's libc `malloc` and `free`: replaced in the port by `ds_heap`.
  - The lifter's `--override-funcs 0x167AC88=ds_malloc,0x167AC58=ds_free` does it
    (docs/27:50).
  - `ds_heap` is a 1.5 GB free list at 0x60000000-0xC0000000
    (port/des_heap.cpp:33-34). [verified]
  - Its peak is tracked (`g_peak`, des_heap.cpp:43) but printed only on OUT OF MEMORY.
- "Temporary heap setting is too small." (0x01708348): some temp-heap size check.
  The site is not traced.

### 1.2 What happens when a budget runs out

1. **The allocator reports it.** A heap allocation fails in DLStdAllocator
   (`func_00FB7F80`, `DLStdAllocator.inl` line 0x10C). It formats "Out of memory.
   HeapID=%d, MemFree:%d/%d(max:%d), Requested:%d" (string at 0x0173EE90). Then it
   calls DL_PANIC with "Transaction failed. See above message for detail." [verified]
2. **DL_PANIC crashes on purpose.** DL_PANIC is `func_00A9DD58`.
   - It prints "[Dantelion2 Panic] file(line) message".
   - If the flag at 0x01B91BFC is 1, it takes a debugger path.
   - Otherwise it prints "DL_PANIC is firing core dump..." and stores 0x00DEADBA to
     address 0. [verified]
   - On a PS3, and in RPCS3, that store is an access violation: the crash players
     see. [inferred]
3. **A failed heap creation panics too.** If a creator's block or its strategy init
   fails, DL_PANIC prints "DLStaticHeap can not recover from failure of strategy
   initialization." (MemMan.cpp lines 0xD9 and 0xEF). [verified]
4. **In the port, the crash store is dropped.**
   - Stores to the null page are dropped (`vm_null_store_report`,
     ps3recomp/runtime/ppu/ppu_loader.cpp:1443-1470).
   - Only the first 8 are printed, and the play log already has one at line 41.
   - So a panic's crash store is dropped too, and the game runs on with a NULL
     allocation. Whatever happens next is undefined. [verified]
   - The play log has no out-of-memory line and no panic. [verified]
5. **The report may not be printed at all.**
   - TTY output does reach the log (syscall 403, ppu_loader.cpp:3603).
     `TTY_BT=<text>` dumps the call chain for a matching line (:3649).
   - But I did not confirm that a retail build's report call (`func_00C3B2A8`) reaches
     the TTY.
   - A test hook should read the arguments of `func_00FB7F80` instead (1.4).

### 1.3 Can the port lift the limits?

**(a) What the runtime reports and allows today** [verified]

| Item | Today | Where |
|---|---|---|
| `sys_memory_get_user_memory_size` | 256 MB total, 192 MB available, always the same answer | ppu_loader.cpp:3551-3557 (answers before `sys_memory.c`, so the 213 MB at sys_memory.c:21 is not reached for this call) |
| `sys_memory_allocate` window | 0x40000000-0x50000000 (256 MB). Bump allocator; size rounded up to the page size; ENOMEM past the end; free never reuses | sys_memory.c:27-28, :61-95 |
| `sys_mmapper_allocate_address` | Bump from 0x11000000. DeS does not call it | ppu_loader.cpp:3559-3571 |
| RSX local memory | 0xC0000000, 256 MB reported | libs/video/cellGcmSys.c:457-458, `cellGcmGetConfiguration` :589 |
| IO space | The game's own 7 MB at 0x49E00000. `cellGcmMapMainMemory` can map more | cellGcmSys.c:431 (`cellGcmInit`), :3012 |
| Game `malloc` | `ds_heap`, 1.5 GB | port/des_heap.cpp:33-34 |
| `_sys_heap` HLE | 0x50000000-0x58000000 | libs/system/sysPrxForUser.c:862-863 |
| sys_vm region | 0x60000000-0x70000000, inside `ds_heap`'s range. The EBOOT has two `sys_vm_memory_map` callers (`func_00548BF0`, `func_00549810`); none ran in the play log | runtime/syscalls/sys_vm.h:38-39 |
| PPU stacks | 0xD0000000-0xE0000000 | runtime/memory/vm.h:48-49 |

**(b) RPCS3 Debug Console Mode** [verified in RPCS3 source]
- **More OS memory.** The default memory container is 0xD500000 (213 MB) for this
  SDK. Debug Console Mode adds 0xC000000 (192 MB), for 405 MB
  (rpcs3/Emu/Cell/PPUModule.cpp:2556, :2579-2582). That changes what
  `sys_memory_get_user_memory_size` reports and how much `sys_memory_allocate` may
  take (Cell/lv2/sys_memory.cpp:324-358).
- **Local memory stays 0xF900000** (249 MB) either way (Cell/Modules/cellGcmSys.cpp:404).
- **IO space** reaches 512 MB only in two cases:
  - "DECR memory layout" together with Debug Console Mode (Cell/lv2/sys_rsx.cpp:329-333);
  - or a game that asks for `CELL_GCM_SYSTEM_MODE_IOMAP_512MB` (cellGcmSys.cpp:414).
- The settings are at system_config.h:75 and :175.
- **For DeS** the setting changes nothing by itself, because the heap sizes are
  constants. [inferred from 1.1]
- **Headroom on RPCS3.** In RPCS3's boot log, 182 MB of the 213 MB was still free just
  before the game's first block (docs/07). The game then takes about 165 MB: these 15
  blocks plus a 1 MB libc block that the port skips. That leaves about 17 MB for an
  EBOOT with bigger constants, or about 209 MB with Debug Console Mode. [inferred,
  untested]

**(c) A reported size or patched constants?** Patched constants. [verified]
- **Which constants:** the 39 `lis` immediates in 1.1. All three in a creator must
  match.
- **Granularity:** 1 MB for 1 MB-page heaps and 64 KB for the other two. A `lis`
  holds the high 16 bits only.
- **Single-request cap:** the port's 352 answer caps one request at about 192 MB minus
  the reserve. A request is checked on its own, not against the running total.
- **Unknown:** whether the heap code or its callers assume the original sizes anywhere
  else. Test T3 in 1.4 is meant to find out.

**(d) Hard limits**
- **32-bit pointers.**
  - Everything the game uses must fit in the 4 GB guest space (vm.h:41).
  - The space is not full. Fixed windows are the problem.
  - To grow the sys_memory window, take room from `ds_heap` (first print its peak) or
    from the `_sys_heap` range. [verified map, inferred fix]
- **Hard-coded window ranges.** Syscall 14
  (`sys_process_is_spu_lock_line_reservation_address`, lv2_register.c:2409-2433)
  hard-codes 0x40000000-0x50000000 and 0xC0000000-0xD0000000 as reservation-capable.
  SPURS calls it during init. A moved window must update it. [verified]
- **RSX offsets.**
  - Local offsets count from 0xC0000000 and stop at the reported local size.
  - Local memory cannot grow in place: the PPU stacks start at 0xD0000000.
  - IO offsets cover only what the game maps (7 MB). [verified]
- **SPU DMA.** SPU jobs address guest memory by 32-bit address, so a new window is
  reachable once it is committed. [inferred]
- **Size checks.** These are known:
  - the pre-check in `func_00C36C80`;
  - ENOMEM at the window end;
  - the DLStaticHeap strategy-init panic;
  - the temp-heap check.

  Other limits are not known: entity counts, MSB part counts, param row counts. They
  are not memory, but they could also make an added enemy fail. [open]

**(e) Textures**
- **Destination.** An SPU job (EBOOT 0x0177B480) PUTs texels from a main-memory buffer
  to the texture's VRAM address (docs/26 §1). [verified]
- **Render surfaces.** The tiled surfaces sit in local memory (`SetTile ... loc=0`).
  They span offsets 0x10000-0x5C10000, about 92 MB, per the play log. [verified]
- **How the game learns the VRAM size.**
  - The GCM init wrapper `func_009F6D40` calls `cellGcmGetConfiguration` (stub
    `func_016B8EA4`).
  - It stores localAddress and localSize at the HG device's +0xAE4 and +0xAE8.
    [verified]
  - Next, `func_00D1EAD0` passes localAddress to a VRAM manager method. [verified]
  - Not traced: whether that manager's capacity comes from localSize or a constant.
- **Who allocates.** The game owns VRAM allocation (likely the LOCAL heap). The port's
  backend only translates offsets. [inferred]
- **The tightest budget for HD texture mods** is probably the ~150 MB of local memory
  left after surfaces, plus whichever main-memory heap stages the TPF before upload.
  That heap is not traced. [inferred]

### 1.4 How to test

- **T1, a memory statistics knob** (proposed `DES_MEMSTAT=1`). At each area load and
  at exit, log:
  - the sys_memory high-water mark (the bump pointer in sys_memory.c);
  - `ds_heap` `g_cur` and `g_peak`;
  - each MemMan heap's use, by calling the game's own report line `func_000303C0`
    for each heap ID. It formats "%s> FREE:%d/%d[%0.1f%%]  MAX:%d  NUM:%d" (wide
    string at 0x016BD600). Its output sink is not traced yet;
  - for VRAM, the highest local offset that a texture upload or surface touched.
- **T2, a loud out-of-memory report.**
  - Add an instruction hook at the entry of `func_00FB7F80` that prints HeapID, free,
    total, max and requested.
  - Make DL_PANIC (`func_00A9DD58`) stop the game with a crash dialog that names the
    heap, instead of dropping its null store.
- **T3, a shrink-then-grow test** with the size knob from stage M2 (section 3).
  - First halve FFX (9 to 4 MB) and load Boletaria 1-1. T2 should fire with FFX's
    HeapID. That proves the detection and the knob.
  - Then raise MAIN to 128 MB. `regress.sh` frames should not change, because the
    extra memory should go unused.
- **T4, the cache test.**
  - Copy one disc file, changed by one byte, over the original in a copy of the disc
    folder.
  - Boot with the cache kept: the `[fs] open` lines should show only the cache file.
  - Delete that cache file and boot again: they should show the `/dev_bdvd` file and
    a fresh copy being written.
  - No game content goes in the log.

## 2. Question 2: mod support

### 2.1 How the port finds game files [verified]

- **Three path layers.** Each one ends in the same `PS3_GAME` fallback,
  `ps3_vfs_ps3game_fallback` (runtime/syscalls/sys_fs.c:92-123).
  - `host_path` (runtime/ppu/ppu_fs.cpp:95-155) serves the five `cellFs` handlers in
    that file: Open :193, SdataOpen :301, Stat :611, Opendir :658, Mkdir :729. DeS
    uses this layer: every file line in the play log is `[fs]`, none is `[sys_fs]` or
    `[cellFs]`.
  - `sys_fs_translate_path` (sys_fs.c:125-262) is the raw-syscall twin.
  - `translate_path` (libs/filesystem/cellFs.c:122-184) is a mapping table. Its
    defaults are `gamedata/dev_*` (cellFs.c:65-77), with no `/dev_hdd1` entry.
- **How `host_path` maps a path.**
  - `/dev_hdd0/` goes to `PS3_HDD0_ROOT` when that is set (:98-110), and
    `/dev_flash/` to `PS3_DEV_FLASH` (:112-124).
  - Otherwise it strips `/dev_bdvd/`, `/app_home/`, `/dev_hdd0/`, `/dev_hdd1/` and the
    other mounts, and appends the rest to the VFS root (:126-152).
  - So `/dev_bdvd/PS3_GAME/USRDIR/x` reads `<disc folder>/PS3_GAME/USRDIR/x`.
  - And `/dev_hdd1/cache/...` reads `<disc folder>/cache/...`. There is no override for
    `/dev_hdd1` yet (docs/45).
- **Encrypted `.sdat` files.**
  - The three `menu/*.sdat` files on this disc are NPD-encrypted.
  - `edat_resolve` (libs/filesystem/edat.c:635-652) decrypts each to `<file>.dec` beside
    it. It decrypts again when the source is newer.
  - A file without the NPD header is opened as it is. So a plaintext replacement works
    in the port.
- **Unlink and rmdir do nothing.** `cellFsUnlink` and `cellFsRmdir` return OK without
  doing anything (ppu_fs.cpp:738-739).
- **The EBOOT.** At run time it is read only for its data segments (`ppu_load_elf`,
  ppu_loader.cpp:3809). Its code is the compiled lift.

### 2.2 The game's /dev_hdd1 cache serves stale copies in the port [verified]

- **The game reads nearly everything through this cache.** In the play log there are
  1,733 opens of `/dev_hdd1/cache/...` and only 76 of `/dev_bdvd`. Of those 76, 72 are
  `.mowb` sound streams; the encrypted `.sdat` files open on their own path.
- **How a cache name is built.**
  - The name is the MD5 of the file's logical path in UTF-16BE, for example
    `other:/systex_tex.tpf.dcx`. There is no terminator, size or timestamp in it.
  - The folder is the first digest byte mod 37: `/dev_hdd1/cache/<00-24>/<md5>`.
  - All 7 names from the boot sequence match this rule.
  - The code is `func_00955218`, called from the cache device handler `func_00BFB900`.
    It runs MD5 init `func_00CCDE78` (the standard constants), an update over `wcslen*2`
    bytes, and the final step. Then it computes `digest[0] % 37`.
- **The copies are exact.** Five cache files I compared are byte-identical to their disc
  files.
- **Size.** The cache on the dev PC holds 951 files, 336 MB, next to an 8,078 MB
  USRDIR.
- **No disc check on a hit.** When the copy exists, the play log shows no open of the
  disc file. The port's `cellFsStat` returns mtime 0 anyway (ppu_fs.cpp:599-609).
- **The port never clears it.**
  - The game mounts the cache with an empty ID: `SysCacheMount(id='')`, play log line
    103. RPCS3's cache folder for the game is `BLUS30443_`, so it gets the empty ID
    there too.
  - The port's `cellSysCacheMount` (libs/system/cellSysutil.c:422-447) writes the path
    and returns 0, `CELL_SYSCACHE_RET_OK_CLEARED`, without clearing anything.
  - The game does not import `cellSysCacheClear`: its import stub is
    `func_016B9284` (for the mount), and it has none for the clear.
  - The test scripts keep the cache unless `COLD_CACHE=1` (tools/des/desrun.sh:11, :25).
- **RPCS3 clears it on every boot.**
  - An empty ID never reuses a cache. At mount, RPCS3 deletes the old one and makes it
    again (rpcs3/Emu/Cell/Modules/cellSysCache.cpp:199-260).
  - On shutdown it deletes the folder (:112-134), except across an `exitspawn`
    (Cell/lv2/sys_process.cpp:345-356, :502).
- **Result:** in RPCS3, a replaced file takes effect at the next boot. In the port, a
  replaced file that was loaded once before is ignored until its cache copy is
  deleted. [inferred from the verified steps; run test T4 to confirm]
- **Community guides** for RPCS3 still say to empty `dev_hdd1/caches` after
  installing mods [web]. That step is harmless, and may date from older builds.
  [inferred]
- **Content checks.** I saw no size or hash check on file contents. I did not search
  for one everywhere. [inferred]
  - Two other MD5 users, `func_009A68E8` and `func_009A6BC0`, are reached through
    virtual calls. They were not traced. Nothing ties them to file loading.

### 2.3 Do RPCS3 file-replacement mods work unchanged?

Mostly yes, with one fix and three gaps:

- **Same layout.** RPCS3 mods replace files in the game's `PS3_GAME/USRDIR` tree. The
  port reads that same tree. [verified, path mapping]
- **The fix.** Clear `<disc folder>/cache` after changing files until stage F1 lands.
  [verified chain]
- **Gap 1, patched EBOOTs.** Mods that ship a patched `EBOOT.BIN`, or RPCS3 patches,
  do not carry over. One example is a community debug-menu patch. See 2.5. [inferred]
- **Gap 2, menu `.sdat` files.** The port opens a plaintext or an NPD `.sdat`. A real
  PS3 would want NPD. [verified for the port]
- **Gap 3, emulator settings.** Mods that need RPCS3 settings ("Write Color Buffers",
  resolution scale) need the port's own equivalents instead. docs/44 §7 has a mapping.
- **Not tested end to end.**

### 2.4 A mods/ overlay

**Design** [inferred]:
- **Where.** Add one helper, say `ps3_vfs_mod_overlay(path, cap, rel)`. Call it from
  the same three places that call `ps3_vfs_ps3game_fallback`:
  - `host_path` (ppu_fs.cpp:154);
  - `sys_fs_translate_path` (sys_fs.c:262);
  - `translate_path` (cellFs.c:182).
- **What it does.**
  - For a `/dev_bdvd` or `/app_home` path, it looks for the same relative path under
    each enabled mod folder, in load order.
  - The first hit wins. It never touches `/dev_hdd0` (saves) or `/dev_hdd1` (the
    cache).
- **Layout.** For example `mods/<name>/PS3_GAME/USRDIR/...`, next to the disc folder or
  in the settings folder. The disc folder stays untouched.
- **Directory listings.** `cellFsOpendir` would merge them. Whether DeS lists
  directories at all is not known; `PS3_FSLOG=1` shows it.
- **The cache.** Clear `<disc folder>/cache` whenever the enabled mod set changes. Keep
  a hash of the mod files' paths, sizes and times. Or give each mod set its own cache
  folder, so vanilla keeps a warm cache.
- **Not this way.** Do not try to delete single cache entries. That needs the logical
  alias for every folder (`map:` and the others), which is not mapped.
- **The launcher.** Docs/45's launcher would list `mods/`, with enable and order.

**Cost:**
- runtime, about 100-150 lines and 1-2 days with tests;
- launcher UI, 1-2 days.

Risk is low: a missing overlay file falls through to today's path.

### 2.5 Code and data patches

- **No patch engine.** Neither the port nor ps3recomp has a `patch.yml` engine; no code
  mentions it. [verified]
  - docs/44 §6.5 plans to map RPCS3's DeS patches to port settings instead.
  - Several already have native knobs: `DES_SKIP_INTRO`, `DES_FPS`, aspect and HUD
    (docs/39), `DES_DOF`.
- **Data patches** (`.data` and `.rodata`) can work at run time. The exe loads the ELF's
  segments into guest memory (ppu_load_elf). A patch written after load, before the
  guest runs, is what the lifted code reads. [verified load path, inferred effect]
  - Cost: a small `patch.yml` reader for be16/be32/float/byte types, which applies a
    patch only when its address is in a data segment. About 2-3 days.
- **Code patches do nothing at run time.** They change instruction words, and the lift
  never reads those. RPCS3's "Skip Intro Videos" patch, for example, writes two be32
  words at 0x008EEF78, in `.text`. [inferred, near certain]
- **Two routes for code patches:**
  - **Before the lift.** Patch the decrypted EBOOT, then lift and build.
    - docs/45's setup already lifts on the player's PC, and rebuilds when the "lift key"
      changes. The patched inputs would be part of that key.
    - A rebuild takes about 10 minutes on 4 workers (docs/53), and less with the
      incremental lifter output.
    - Risk: a patch that touches a hooked function can break the hook. Check patch
      addresses against `port/insn_hooks.txt` and the `--override-funcs` list.
  - **A port hook.**
    - `port/insn_hooks.txt` calls a host function after a given instruction; one
      example is `des_cam_rate`.
    - `--override-funcs` replaces a whole function, for example
      `0x167AC88=ds_malloc` (docs/27:50).
    - Both are lift-time lists, so a new hook means a re-lift. The values the hooks use
      can stay run-time settings.
- **Heap sizes, by either route.** An instruction hook at each of the 39 `lis` sites
  can overwrite `rD` with a size from settings. Then one re-lift makes all sizes
  run-time knobs. [inferred]

### 2.6 Community tools and formats

**On this disc** [verified, headers only]:
- **Loose files.** The disc is a tree of loose files, with no big archives. The
  folders are chr, facegen, font, item, map, menu, movie, msg, mtd, obj, other, param,
  paramdef, parts, remo, script, sfx, shader, sound, testdata and tropdir.
- **DCX compression.** Most `.dcx` files are DCX with EDGE (chunked deflate)
  compression. That covers 1,564 of 1,787 under `map/` and 221 of 233 under `chr/`.
  The rest use a DCP header with DFLT.
- **Other containers.** `param/` and `msg/` hold BND3 files and DCX-wrapped BND3
  files, and `menu/*.sdat` are NPD-encrypted.

**Tools** [web]:
- **SoulsFormats** is the base .NET library. It is archived (read-only since
  2025-05-26).
  - Its DCX code calls DCX_EDGE the "most common compression format for Demon's Souls",
    and DCP_DFLT "used in DeS test maps". It can write both.
  - Its FORMATS.md lists for DeS: FLVER0 (models), PARAM and PARAMDEF, NVM, MCG and
    MCP (navigation), and MSB.
  - MSB for DeS has "extremely basic support" and cannot be written.
  - BND3 and BXF3 are listed for the games before DS2.
- **WitchyBND**, the successor of Yabber, unpacks and repacks BND, DCX and TPF.
- **Smithbox**, the successor of DSMapStudio, lists Demon's Souls among its games. Its
  README does not say which of its editors (map, param, model and others) work for DeS.
- **DSParamStudio** was tested on the US version. It says to "disable the RPCS3 file
  cache to test changes".
- **Paramdex** holds DeS paramdefs. DeS is the only game that shipped paramtdfs.
- **Mod pages on Nexus Mods** (graphics, params, a debug-menu EBOOT patch) tell RPCS3
  users to empty `dev_hdd1/caches`. They were read through search summaries, because
  the pages refused a direct fetch.
- **RPCS3 issue #9082** ("does not function properly with mods on NA/EU") came down to
  one mod's loading-screen files. It was not memory (docs/29).
- **An RPCS3-based "remaster" graphics mod** was in early work in July 2026. Its
  coverage says nothing about memory limits.

## 3. Staged plan

| Stage | What | Where | Cost | Risk |
|---|---|---|---|---|
| F1 | Clear the game cache at mount, as RPCS3 does for an empty ID, or when the mod set changes. Keep a dev knob for a warm cache | ps3recomp `cellSysCacheMount` (cellSysutil.c:422-447); desrun.sh `COLD_CACHE` | 0.5 day | Low: first loads copy files again, as on RPCS3. Measure the cold boot once |
| M1 | Diagnostics: T1 (`DES_MEMSTAT`) and T2 (loud out-of-memory report, fatal DL_PANIC) | port hooks at `func_00FB7F80`, `func_00A9DD58`; sys_memory.c; des_heap.cpp | 1-2 days | Low |
| F2 | `mods/` overlay with load order, plus launcher list | the three path layers (2.4); docs/45 launcher | 2-4 days | Low |
| M2 | Heap-size knobs: instruction hooks at the 39 `lis` sites; sizes from settings; checked against the window | `port/insn_hooks.txt`, a `des_heap_size` hook | 1 day plus a re-lift and T3 | Medium: other code may assume the old sizes |
| M3 | Bigger sys_memory window: take room from `ds_heap` after reading its peak; update syscall 14's ranges; move the sys_vm range out of `ds_heap` | sys_memory.c:27-28, lv2_register.c:2409-2433, sys_vm.h:38-39, des_heap.cpp:33-34 | 1-2 days | Medium: address-map couplings |
| F3 | `patch.yml` data patches applied after load. Code patches refused, with a message | runtime loader, after `ppu_load_elf` | 2-3 days | Low |
| F4 | Code patches by pre-lift EBOOT patching in setup | docs/45 setup pipeline, lift key | 2-4 days on top of docs/45 stage A | Medium: hook address clashes |
| M4 | VRAM above 256 MB: trace the game's VRAM allocator first. If it follows localSize, move local memory, and the stacks or the 0xD0000000 region with it | cellGcmSys.c, the D3D12 backend, vm.h | 1-3 weeks | High |
| M5 | Non-memory content limits (entity, part and param counts), as mods hit them | the game | open | open |

F1 and M1 come first: they are cheap and make every later test readable.

## 4. Open questions

1. Do heap internals or callers assume the original sizes anywhere other than the 39
   immediates? Test T3 answers it.
2. Which heap runs out when a modder adds enemies or map parts? The HeapID in a real
   failure would say.
   - It might reach RPCS3's `TTY.log`, if retail builds print the report at all
     (`func_00C3B2A8`, not traced).
3. Does the VRAM allocator's capacity follow the reported localSize (device+0xAE8), or
   a constant? Trace `func_00D1EAD0` and its VRAM manager.
4. Which main-memory heap stages TPF textures before the SPU upload? Is that copy freed
   afterwards?
5. Who owns the 0x240000 block (64 KB pages)? And where do DL_RUNTIME and DL_SYSTEM
   get their memory?
6. Does DeS list directories, so that the overlay must merge listings? And does it
   ever call `cellFsUnlink` on cache files, which the port ignores today?
7. How much slower is a cold-cache boot in the port? This decides whether F1 clears
   at every boot or only on a mod change.
8. What is `ds_heap`'s real peak? This decides how much room M3 can take.
9. Why do RPCS3 guides still say to empty `dev_hdd1/caches`, when current RPCS3 clears
   it itself? Are older builds the reason?
10. What do the two untraced MD5 users (`func_009A68E8`, `func_009A6BC0`) hash?

## 5. Web sources

Repositories that live under a personal account are named, not linked, so that no
person's handle goes in this file.

- RPCS3 issue #9082: https://github.com/RPCS3/rpcs3/issues/9082
- The DSParamStudio README on GitHub.
- SoulsFormats on GitHub: `FORMATS.md` and `SoulsFormats/Formats/DCX.cs`.
- The Smithbox README on GitHub.
- Paramdex: https://github.com/soulsmods/Paramdex
- WitchyBND: https://www.nexusmods.com/eldenring/mods/3862
- Nexus Mods, Demon's Souls tutorial and mod pages, through search results only:
  https://www.nexusmods.com/demonssouls/mods/21
- Muy Computer, 2026-07-14, on an RPCS3-based remaster mod:
  https://www.muycomputer.com/2026/07/14/un-modder-trabaja-en-demons-souls-remaster-para-pc/
