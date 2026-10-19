# docs/29 "Look into first": follow-up work

Date: 2026-10-18.

This picks up the five items at the top of docs/29. The "Follow-up,
2026-10-18" section on master (commit ece0cd9) already checked them against
the game. This pass made the code changes where one applies. Each item says
what was found, what was done, and how it was checked.

**[read]** means seen in code, a log or a file. **[inferred]** means my
conclusion, not tested.

| # | Item | Result |
|---|---|---|
| 1 | cellSysCacheMount | No PR. DeS already reuses a warm cache under the port. |
| 2 | cellSaveData write path | No PR. PR #48 covers every write DeS makes; what is left is listed. |
| 3 | lv2 sync primitives | Two local ps3recomp branches, not pushed. They need a game run. |
| 4 | cellGcmGetLastFlipTime | ps3recomp PR #51, open. DeS ignores the value. |
| 5 | Colour mask, cellSail | Colour mask closed (PR #47). cellSail after Quit Game needs a run. |

## 1. cellSysCacheMount: no PR

Found:
- Ours (`libs/system/cellSysutil.c:422-447`) writes `/dev_hdd1/cache/<id>`
  to getCachePath and returns 0 (OK_CLEARED). It deletes nothing. [read]
- `/dev_hdd1/` maps under the VFS root in both file layers
  (`runtime/ppu/ppu_fs.cpp` host_path, `runtime/syscalls/sys_fs.c`
  sys_fs_translate_path). So the cache sits in `$PS3_VFS_ROOT/cache`
  (docs/45). Only `COLD_CACHE=1` in `tools/des/desrun.sh` wipes it. [read]
- DeS mounts with an empty id (`SysCacheMount(id='')`) and opens
  `/dev_hdd1/cache/<2 hex>/<32 hex>`. On a miss it decompresses from the disc
  and writes the result back (docs/24, section 6). [read]
- The latest play log (`work/tmp/play.txt`): 378 opens under
  `/dev_hdd1/cache`, none failed, and the reads return DCX data. There are
  only 17 `/dev_bdvd` opens, all sound banks. The cache folder holds 900 files
  (329 MB); 770 of them were written on 2026-10-14. [read]
- RPCS3 (`cellSysCache.cpp:178-262`) returns RELAYED only for a non-empty id
  that matches the kept one. An empty id always gets a cleared cache, at the
  fixed path `/dev_hdd1`. So in RPCS3 every DeS boot starts with cache misses
  and reads the disc. [read]

Why no PR:
- RELAYED would change nothing for DeS (empty id), and it is the wrong answer
  for an empty id.
- The boot-time win item 1 hoped for is already ours, by accident. We keep a
  cache that RPCS3 clears, and per RPCS3's comment the console clears it too.
- The guest path differs from RPCS3's (`/dev_hdd1/cache/HH/...` here,
  `/dev_hdd1/HH/...` there). That does not matter while one root serves both.
  [inferred]

Watch:
- The console starts each boot with an empty cache. We do not. A cache file
  left half-written by a crash or a kill would be read back on the next boot.
  [inferred; not seen]
- `COLD_CACHE=1` clears it. A real fix would copy RPCS3's corruption marker:
  write a file at mount, remove it at a clean exit, and clear the cache when
  the marker is still there at mount. Not done: most of our exits are not
  clean, so it could clear the cache on most boots. [inferred]

## 2. cellSaveData write path: no PR

Found:
- PR #48 (e624982, merged) sends a whole-file WRITE at offset 0 to
  `.<name>.tmp`. Once the write is complete, it moves the temp file over the
  old one (MoveFileExA; rename on POSIX). A failed write keeps the old file.
  [read: `libs/system/cellSaveData.c:696-745`]
- That is the only kind of write DeS makes. In the play log, AutoLoad2 reads
  `04USER.DAT` and `USER.DAT` (262144 bytes each). Each AutoSave2 deletes
  `2NNUSER.DAT` and writes `1NNUSER.DAT` (262144 bytes, offset 0): lines
  7509-7510 (`2USER.DAT`, `1USER.DAT`) and 7614-7615 (`204USER.DAT`,
  `104USER.DAT`). [read]
- DeS keeps backup generations of its own. RPCS3's DeS save folder on this PC
  holds `01`-`04USER.DAT`, `USER.DAT`, and `1NN` or `2NN` copies. The two
  autosaves above did not touch the main `NNUSER.DAT`. [read; what the
  generations are for is inferred]

What is left:
- **No per-call commit.** RPCS3 builds the whole save in memory and writes it
  to `.working_<dir>`. Then it swaps the directories through
  `.backup_<dir>`. So one save call lands whole or not at all
  (`cellSaveData.cpp:2145-2208`). Ours applies each step at once: a DELETE
  runs immediately (`:747-748`), and PARAM.SFO is written before the file
  steps (`:918-925`). [read]
  - Suppose a crash comes after DeS's DELETE of `2NN` and before its WRITE of
    `1NN` finishes. That slot is then left with no backup copy, but the main
    file stays. [inferred]
  - A brand-new save cut short leaves PARAM.SFO without data files.
    PARAM.SFO is what marks a save as present (`dir_has_save`, `:116`), so the
    next load is told the save exists. That is the Virtua Fighter 5
    ERR_BROKEN case that function's comment describes. [read; the outcome is
    inferred]
- PARAM.SFO is rewritten in place (`"wb"`, `:769`), and the PFD list is
  appended to (`"ab"`, `:551`). A crash during those small writes leaves a
  short file. [read; the outcome is inferred]
- WRITE_NOTRUNC and offset writes go in place (`"r+b"`, `:711`). DeS sends
  none. [read]
- No flush. That covers a process crash but not a power cut, because the
  rename can reach the disk before the data. [inferred] A flush on the game
  thread is what caused RPCS3's DeS autosave stutter (#9857). Leave it out,
  or do it off the game thread.
- A DELETE of a missing file returns 0 here. RPCS3 fails the call with
  CELL_SAVEDATA_ERROR_FAILURE (`cellSaveData.cpp:2074-2081`). DeS deletes
  only files it has listed. [inferred]
- Saves complete at once. RPCS3 adds about 0.5 s (PR #13492), and per that
  PR's tests DeS is fine either way. [read: docs/29]

Why no PR:
- DeS's only writes are already safe, and its own backup copies cover the gap
  between its DELETE and its WRITE.
- A per-call commit would change save behaviour for every title. It needs a
  save/load run, and the payoff for DeS is small.

If wanted later, a design:
- Defer DELETEs, and the renames of whole-file writes, to the end of the
  call. Do the renames first, then the deletes.
- Write PARAM.SFO last, through a temp file.
- Drop the pending steps when the call ends in an error.
- Commit the pending steps before a READ or an in-place write in the same
  call.
- `libs/system/tests/test_savedata_io.c` already drives savedata_execute with
  fake guest callbacks, so it can test this.

## 3. lv2 sync primitives audit: two local branches

DeS's static syscall sites. These are counted in the EBOOT lift as lines
that load r11 with the syscall number; the lifted libsre has none. [read;
approximate]

| Syscall | Sites |
|---|---|
| sys_mutex_create (100) | 25 |
| sys_mutex_destroy (101) | 29 |
| sys_mutex_lock (102) | 44 |
| sys_mutex_trylock (103) | 1 |
| sys_mutex_unlock (104) | 58 |
| sys_cond_create (105) | 12 |
| sys_cond_destroy (106) | 21 |
| sys_cond_wait (107) | 10 |
| sys_cond_signal (108) | 11 |
| sys_cond_signal_all (109) | 11 |
| sys_cond_signal_to (110) | 0 |
| sys_spu_thread_write_spu_mb (190) | 10, in 9 functions. Never called in the play, snag or regress logs. |

### Branch fix/lv2-sync-audit (3 commits on master f15a4f0, not pushed)

ae60f8a "lv2: sys_mutex_destroy and trylock answer as lv2 does":
- sys_mutex_destroy used to free the mutex whatever its state. It deleted the
  CRITICAL_SECTION under its holder and its waiters, and left conds bound to
  a dead id. Now it answers EBUSY while the mutex has an owner, then EPERM
  while a cond is bound to it, in RPCS3's order. A cond waiter that has left
  its cond but has not yet relocked counts as an owner (`relock_pending`).
- The CRITICAL_SECTION is made once per slot and kept. So a thread that read
  the id just before a destroy still enters a valid lock (RPCS3 #5475). lock
  and trylock answer ESRCH when they find the slot destroyed.
- sys_cond_create checks the mutex and counts itself on it in one step,
  under the mutex table lock. This closes the create/destroy race of RPCS3
  #8340. sys_cond_destroy gives the count back.
- trylock by the owner of a non-recursive mutex answers EDEADLK. It used to
  return CELL_OK and take a second count on Windows, and EBUSY on POSIX.

30408ff "lv2: sys_cond_signal takes the woken thread off the cond":
- Signal and signal_all now unlink the waiters they wake, as lv2 does. So a
  sys_cond_destroy right after the last signal no longer gets a spurious
  EBUSY (RPCS3 #8340).
- A waiter unlinks itself only on a timeout or a failure.
- A woken waiter's cond slot may be reused, so the wait keeps its own copy of
  the cond and mutex ids for the sync history.

792ed9c "lv2: log the first refused sys_mutex_destroy and sys_cond_destroy":
- Prints the first 16 refusals of each call with the caller's lr, e.g.
  `[SYNC] sys_mutex_destroy(12) -> EBUSY: owner tid 3, 0 relocking, 0 conds, lr=0x...`.
  Nothing is printed otherwise. A run then shows whether DeS ever relied on
  the old answers.

Checked:
- New test `tests/sync_stress/test_destroy_semantics` (Windows), with modes
  mutex_busy, mutex_conds, trylock, relock, signal and timeout. relock and
  signal fail against the first commit's signal code (seeded check).
- relock, signal, timeout and mutex_busy passed 40 runs each. sync_stress
  passed with seeds 0, 1, 7 and 12345. All sync ctests pass, and the runtime
  library builds.
- Not compiled for POSIX: there is no POSIX toolchain here. The Linux and
  macOS CI jobs already fail on master (see item 4), so CI will not catch it
  either.

### Branch fix/lwcond-layout (1 commit on master f15a4f0, not pushed)

293bb52 "sysPrxForUser: sys_lwcond_t is 8 bytes (lwmutex be32, id be32)":
- sys_lwcond_create wrote the lwmutex EA as a be64 at +0 and the id at +8:
  12 bytes into an 8-byte struct. RPCS3's `sys_lwcond.h` has the lwmutex at
  +0 and the queue id at +4, and so does our own `sysPrxForUser.h`.
- In DeS the lwcond is libsre's CellSpurs cond at +0xDC8, so the id landed on
  CellSpurs+0xDD0. No lifted libsre code uses 0xDD0 as an offset, so no
  visible change is expected. [read: grep; the outcome is inferred]
- sys_lwcond_wait now reads the EA as a be32 at +0. That is the same value
  it read before.
- Checked: `ppu_sysprx.cpp` compiles with the port's flags, and the object
  code writes at +0 and +4.

### Integration branch test/l2-sync-lwcond (local, for the run only)

master f15a4f0 plus both branches (commit 0383460). The runtime library
builds, and all 13 ctests of that build pass. Do not open a PR from it.

### Automated runs on the integration branch (2026-10-18)

des_boot from test/l2-sync-lwcond (0383460) with master's port tree, then
`tools/des/regress.sh` with the pinned save. [measured]

| Run | Result |
|---|---|
| 30 fps (18:13) | s1 to s3 ok. s4_on flagged (41.5% changed): the walk is timed by the clock and ended elsewhere, as it sometimes does on other builds. |
| 120 fps (18:15) | s1 and s2 flagged. The game ran slowly: 3766 flips in 190 s, and s1 caught the arrival animation (taken at 110 s; usually about 89 s). |
| 120 fps again (21:42) | All four shots ok. 9513 flips in 111 s, s1 at 89 s, as on master builds. |

- No run printed a `[SYNC]` line, so the game never hit a refused
  sys_mutex_destroy or sys_cond_destroy on this route.
- The same exe was normal in the re-run, so the slow 18:15 run was most
  likely load from other programs on the PC at the time. [inferred]
- The run request below still needs its manual steps: warp, autosave,
  Quit Game to the title, restart.

### RPCS3 PRs against our code

| RPCS3 PR | What it fixed | Ours |
|---|---|---|
| #8340 | cond_create vs mutex_destroy race; spurious EBUSY in cond_destroy; SPU inbound mailbox written under the group mutex; IPC id counts | The first two are fixed on fix/lv2-sync-audit. Mailbox: our write_spu_mb checks the single slot, then writes it without a lock, so two PPU threads writing to one SPU at once could lose a word. DeS never calls it (0 log lines; `nrawspu=0`). IPC: we have no process-shared objects. |
| #5475 | mutex destroyed while an unlock is running | Fixed: the lock is kept per slot, and destroy refuses while the mutex is owned. |
| #5667 | lwcond wait return code overwritten in a race | Does not apply. Our sys_lwcond_wait releases the lwmutex, sleeps 1 ms, relocks and returns 0. Signals are no-ops, and the guest re-checks its predicate. There is no wake or error code to race. |
| #5314 | lwmutex flag, scheduler queueing, lwmutex relock after an lwcond timeout | Our lwmutex (`ppu_sysprx.cpp`, lwm_wait / lwm_release) counts a sleeper under its lock before the last try, and release wakes under the same lock, so no wake is lost. lwcond always relocks. |
| #12496, #12482 | scheduler flag races, lwcond wake under the mutex, yield, timers | Do not apply: we have no lv2 scheduler (host locks), and lwcond polls. |
| #6604 | signal flag left set after a cond_wait ETIMEDOUT | Does not apply. Our waiter record lives on the waiting thread's stack for one wait. A signal that picks a timed-out waiter wins, as in lv2, and the wait returns CELL_OK. |
| #5310 | group join returns before the threads stop | Ours waits on finish_event, which is set after exit_status and `running = 0` (`lv2_register.c:729-750`). |

Left as they are (also listed in the PR text):
- Cond timeouts use GetTickCount64, which moves in 15.6 ms steps.
- Waiters wake in FIFO order whatever the protocol.
- A woken waiter competes for the mutex instead of being handed it.
- Ids are reused without a generation count.
- POSIX re-creates the pthread mutex on each create.

## 4. cellGcmGetLastFlipTime: PR #51 (open)

ps3recomp fork PR #51 (branch fix/gcm-last-flip-time-us, commit b94caca).

Changes:
- New `lv2_system_time_usec()` in `runtime/syscalls/sys_timer.c`. It counts
  microseconds from the first call, and a compare-exchange sets that origin.
  Before, two threads making the first call together could get one huge
  reading.
- `sys_time_get_system_time` and the cellGcmSys flip time (at init and at
  each flip) use it.
- The NID comment above cellGcmGetLastFlipTime now says 0x63387071.
- New test `tests/lv2_time`.

Not modelled: RPCS3 stamps the flip with host time minus one second
(`RSXThread.cpp`, `get_system_time() - 1000000`). Ours uses the same origin
as sys_time_get_system_time.

DeS:
- One call (lr 0x00FE0378). The value goes to
  cellSailGraphicsAdapterUpdateAvSync, which ignores it. [read]
- DeS's first sys_time_get_system_time call comes before `_cellGcmInitBody`
  in every logged run, so the values it sees do not change.

Checked: the host test (a seeded nanosecond version fails it), a full
runtime build, and `ppu_sysprx.cpp` compiled with the port's flags. Not run
in the game.

CI: the Windows and both Vulkan jobs pass. Linux and macOS fail on master
f15a4f0 too: `_mm_pause` is undeclared at `spu_channels.c:1408`, and the SPU
helper tests and sync_stress fail to link on `g_spu_ls_watch_n` /
`spu_ls_watch_slow`. The PR did not cause these failures.

This goes further than the docs/29 follow-up, which left the units alone
because the change was untested. It is now tested on the host, and it is
inert for DeS.

## 5. Colour mask and cellSail

- **Colour mask: closed.** PR #47 (07234b5, merge 67f3bbb) ignores a
  COLOR_MASK write with any bit outside 0x01010101 and keeps the old mask.
  This applies in the dispatch and the D3D12 path, and the first few such
  writes are logged. DeS sent none in a 120 s level run. [read]
- **cellSail re-init works within one boot.** The play log has two full
  cycles on the same player (0x42B4BC88): ds_advertise.pam (lines 3120-3139)
  and ds_title.pam (lines 3150-3169). Each goes MemAllocatorInitialize,
  PlayerInitialize2 ... PlayerFinalize. [read]
- **Not checked:** Quit Game back to the title (RPCS3 #3476's flicker) and
  the end credits (#4302's white screen). The ending movies are
  `ds_goodending.pam` and `ds_badending.pam` [read: disc]. Reaching them
  needs an end-game save.

## Run request (one run covers items 3 and 5)

Build des_boot against ps3recomp branch test/l2-sync-lwcond (0383460):
- The port build compiles that branch's `ppu_sysprx.cpp`, so PS3RECOMP_DIR
  must point at that tree. It also needs the untracked `work_spu/` and
  `fw_spu/`.
- `build/ps3recomp_runtime.lib` must come from that branch. It uses the same
  Release clang-cl Ninja config as the main checkout.

Steps:
1. Set `PS3_SAIL_MOVIES=title`, so the title movie plays and the others end
   at once. Boot, Continue, and play into the game.
2. Warp from the Nexus to a level and back once. Trigger an autosave (pick
   up an item).
3. Quit Game to the title. Expect a third `[cellSail] PlayerInitialize2` and
   `CreateDescriptor ... ds_title.pam`, a title menu without flicker, and no
   hang. Continue again.
4. Restart, and check that the save loads.

Check the log for freezes, and for any `[SYNC] sys_mutex_destroy` or
`[SYNC] sys_cond_destroy` lines. None are expected. If there are some, the lr
in each line shows which guest code relied on the old answers.
