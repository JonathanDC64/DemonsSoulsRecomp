# 101. A half-written cache copy and the load into 1-1 that never ends (2026-10-24)

Issue #171: one player's first trip from the Nexus to Boletarian Palace 1-1 stayed on the loading screen. They sent
four problem reports (0.1.0-dev.580 three times, 0.1.0-dev.790 once), all from the developer route, all offline, and
it hung at the same point every time. Labels: **measured** (a run says so), **read** (seen in the code or a log),
**inferred**.

## Short answer

- **Cause: a short copy in the game's HDD cache** (`/dev_hdd1`). The game copies files it loads into that cache and
  reads the copy on later loads. The copy of a 3.2 MB file used by the 1-1 load (`cache/19/6315a0cc…`) was most likely
  cut short when an earlier session closed during the write. Every later session read it back as good data
  (inferred: their file is not in the reports; our copy cut the same way reproduces their logs, section 2).
- **Why the game trusted it.** The game mounts the cache with an empty cache id. The PS3 clears the cache at every
  such mount, so the game assumes every file there was written in this session. ps3recomp's `cellSysCacheMount`
  answered "cleared" (`CELL_SYSCACHE_RET_OK_CLEARED`) but kept the old files (read).
- **Fix: ps3recomp #119.** The mount now really empties the cache directory. Each start begins with an empty cache,
  as on the PS3, and the half-written copy is gone. This port moves its pin to that commit.
- **Tested:** the trip on the damaged cache hangs with the old runtime and arrives with the fix (measured, section 3).
- **For the player on a build without the fix:** close the game, then delete or rename the folder `cache` in the
  disc folder (`PS3_VFS_ROOT`). The next start writes new copies.

## 1. What the reports showed

- Up to the hang, their load opened the same files as ours, file for file (read). Then theirs went straight to the
  texture packs (`m02_0000.tpf.dcx` to `m02_9999.tpf.dcx`) and never wrote a cache copy. After `m02_9999.tpf.dcx`,
  nothing more was opened.
- The game's loader threads (FileLoader, Decompressor, CacheDeviceHandler) were idle, each waiting for work. No
  thread was blocked in a runtime call, and no SPU event flag wait was reported (read).
- At the hang, a second decompressor stream had just started. The game's decompressor (Edge, 0x00BCB754) keeps up to
  three streams; in all four reports this was the first time the second one was ever used (read).
- File descriptor 14 (the cache copy `cache/19/6315a0cc…`) was never closed; fd 15, opened just after it, was (read:
  the later opens reuse 15 but skip 14).

## 2. How it was reproduced

A scripted, offline trip from the user's save in front of the Boletaria archstone (scratch tooling, not committed):
warp to the stone, press X until the 1-1 load starts, then watch whether the load ends and the arrival cutscene plays.

None of these reproduced it (measured): a warm cache, the reporter's cache state (the 50 files their load missed
taken out), the reporter's settings (200% scale, AA, no blur, no depth of field), movies on, 4 or 12 CPUs, a slow
cache drive (80 MB/s, 12 ms a seek), slowing only the 3.2 MB file.

Then the copy itself was damaged, on the reporter's cache state (measured):

| The copy of `cache/19/6315a0cc…` (3,276,960 bytes) | Result with the old runtime |
|---|---|
| Cut to 1,638,400 bytes | **Hangs exactly as in the reports.** The load stops after `m02_9999.tpf.dcx`, no event flag wait is blocked, a second decompressor stream starts, fd 14 stays open and fd 15 closes. |
| Full size, second half zeroed | Hangs too, but differently: the Decompressor stays blocked in `cellSpursEventFlagWait` (the SPU job never finishes). Not what the reports show. |

So their copy was short, not zero-filled (inferred). That fits how this runtime writes it. The game calls
`cellFsFtruncate` with the full size before writing the copy in pieces. `runtime/ppu/ppu_fs.cpp`, the file layer that
opens the game's files, does not handle `cellFsFtruncate`. The call reaches `libs/filesystem/cellFs.c`, whose own file
table does not have the file, and fails, so the file only grows as pieces are written (read). A session closed
during the copy leaves it short.

What the game then does (read, inferred): the stream for the short file waits for blocks that never arrive. The
next file goes to a new decompressor stream, and the rest of the load is queued behind the stuck one.

## 3. The fix and its test

ps3recomp #119:

- `cellSysCacheMount` calls a hook that the file layer sets at startup.
- The hook removes everything in the `/dev_hdd1/cache` directory: on the host, `PS3_HDD1_ROOT/cache`, or
  `<disc root>/cache` when that is not set. It keeps the directory itself and touches no other path.
- A file still open elsewhere is left in place (a second copy of the game on the same disc folder).
- The boot log says how many files and folders it removed (`[fs] system cache /dev_hdd1/cache/ cleared: …`).

The same trip, same save, on the reporter's cache state with the copy cut to half (measured):

| Runtime | Result |
|---|---|
| Old | Hangs (section 2). |
| #119 | Arrives: 734 cache entries removed at boot, the copy written again in full (3,276,960 bytes), the arrival cutscene played, 41 s after the load started. |
| Old, empty cache (what every start is now) | Arrives in 41 s; 173 copies written during the 1-1 load. |

## 4. What changes for players and developers

- Every start begins with an empty HDD cache, as on the PS3. The first load of each area writes its copies again,
  about 240 for the Nexus and 170 for 1-1 (measured on the trip). A cache had about 1,000 files (340 MB).
- A developer's existing cache folder (`<disc root>/cache`) is emptied at the next start.
- Test scripts that copied a warm cache into `PS3_HDD1_ROOT` now get an empty one at boot. A warm cache was never a
  state the PS3 starts in.

## 5. Left open

- `cellFsFtruncate` is still not handled by `ppu_fs.cpp`. With the cache cleared at every start this no longer
  matters for the hang, but the call still fails.
- `cellSysCacheClear` still keeps the files. Demon's Souls never calls it.
- Every mount clears, for every title. The PS3 keeps a cache when the title mounts it again with the same id and
  the last session ended cleanly (RPCS3 marks a session in progress with a file in the cache). Nothing here records
  either, so a title with its own cache id loses its cache at every start. The mount always told it so.
