# 27 — The full re-lift boots: auto-save notice

The current lifter can now lift Demon's Souls from scratch. That fresh lift boots further than the patched 2026-07-01 lift did: past the loading screen and on to the game's own system-data auto-save notice ("This game is compatible with the system data auto-save feature…" with an OK button). It also creates the threads that the old build never reached and that RPCS3 does create: FXHGDrawEntityTracer, FaceGenMan::Thread and NexusRevolution Surveillance.

`work/recomp` is now the fresh lift. The old lift is kept as `work/recomp_old0701`.

## Why the v0.14.0 lift used to break (docs/24 §8)

The first re-lift (`work/recomp_v0140lift`) broke early, with the `func_00B0AF28` heap-manager singleton reading ASCII garbage. Bisecting turned out to be unnecessary. That lift contains exactly one dropped branch, and it was enough to explain the failure:

```
void func_00010210(ppu_context* ctx) {      /* _init */
    ...
    ctx->lr = 0x00010220; func_00012878(ctx); ...
    /* bl -> non-code 0x00010200 */;        <- dropped
```

- 0x10200 is a linker TOC-switching long-branch stub: it sets r2 to the second TOC, then branches to `func_016B84B0`, which is `__do_global_ctors_aux`. That function walks `__CTOR_LIST__` and calls every C++ static constructor.
- The function list comes from the OPD, so it starts at 0x10210 and the stub is not on it.
- Without `--code-end`, the lifter set its code window to `[first listed function, last end)`. That made the stub's address count as "non-code", and the `bl` became a comment.
- As a result, no static constructor ran, and the first lazily built singleton read whatever was in memory.

The old lifter had no code window, so it promoted the `bl` target to `func_00010200`.

**Fix** (ps3recomp `c99c082`, upstream branch `fix/lifter-code-window-segment`): the default window now starts at the executable PT_LOAD that holds the first function. The upper bound is unchanged; that is where data misread as code actually lives.

## What the re-lift fixes without patch scripts

The current lifter already does what our in-place patch scripts did:

| Patch script | What the current lifter does instead |
|---|---|
| `fix_jumptables.py` | Lifts 703 jump-table dispatchers (7,465 cases) as in-function switches; the old lifter kept 18. |
| `fix_vmx_byteorder.py` | Byte-orders lanes through `ppu_vldf4`/`ppu_vstf4`. |
| `vmrghw` rewrite | Merge handler takes the element size from `mn[5]` (upstream 2070f4d). |
| `vrfin`, `vsumsws`, `vcmpgtuh` lowering | Lowered by the lifter. |
| clz(0) fix | Correct in the lifter. |

It also makes `addi` a full 64-bit add, sets `ctx->lr` at call sites, and adds real fences, PPC NaN semantics and saturating float-to-int conversion.

**Do not run `fix_vmx_byteorder.py` on a new lift.** It would match the new lift's 2,668 word merges. Rewriting them is harmless, because they are pure lane moves, but it is not needed.

`fix_jumptables.py` is not needed either. Only `apply_boot_patches.py` (the NULL-heap shim) still applies. It now takes the lift directory as an argument and finds the right chunk itself.

## Re-lift recipe

```
python ps3recomp/tools/ppu_lifter.py work/game/EBOOT.ELF \
  --functions work/loader/EBOOT.functions.json --hle-stubs work/loader/EBOOT.imports.json \
  --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free,0x273D38=des_move_post" --shadow-stack \
  --output work/recomp -j 16
cp <previous lift>/hle_nid_table.cpp* work/recomp/     # the generated NID table
python scripts/apply_boot_patches.py work/recomp
cmake --build port/build --target des_boot --parallel 4
```

The lift itself takes about 40 s. Recompiling the 14 chunks takes about 15 min with `--parallel 4`.

To build another lift beside the main one, configure a second tree:

```
cmake -S port -B port/build_x -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRECOMP_DIR=<abs lift dir> \
  -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=
```

The empty build type and flags match `port/build`. Without them CMake defaults to Debug (`/Od /RTC1`), which builds and boots but runs slower.

Then run it with `DES_EXE=./port/build_x/des_boot.exe tools/des/desrun.sh ...`.

## Upstream branches (local, not pushed)

- `fix/lifter-code-window-segment`: the code-window fix above.
- `fix/lifter-dead-vmrg-handler`: removes the shadowed second `vmrg*` handler. It still had the `"h" in mn[4:]` bug, which is how the old lift came to emit `vmrghw` as `vmrghh`. Adds `tools/test_ppu_vmrg_element_size.py`.

Both are based on `origin/master` (a2c2010) and pass `test_ppu_lift.py` (1,533 conformance checks), `test_ppu_branches.py`, `test_ppu_fallthrough.py` and `test_raw_prx_toc.py`.

## The hang after the notice: GCM title-ring recycle (fixed)

After the notice, flips stopped at a random count, between about 50 s and never. MAIN waited on cond 40 for HighGraphics, and one SPU host thread sat at 100% CPU.

**The wait chain**
- HighGraphics waits in `func_009F94A8` (poll lr `0x009F9608`) for RSX label 255 (`cellGcmGetLabelAddress(0xFF)`, `0x20000FF0`) to reach a fence value it queued two frames earlier.
- The fence is queued as a `{0x18, 0xFF, value}` entry in a command ring. Its descriptor is at `0x01B51000`: `+0` read, `+0x10`/`+0x14` bounds, `+0x80` write.
- An SPU (img 2) consumes that ring and turns the entries into GCM commands. The 100% CPU thread is this consumer polling the write-pointer line once it has caught up: about 7.8M GETLLAR/s at pc `0x5120` (`SPU_ATOMHIST`). That is a symptom, not the cause.

**What the state at the hang showed**
- The ring was drained: read == write.
- The last fence the consumer took was ahead of label 255: `0x40C` consumed, label at `0x40A`.
- get == put == io `0x8000` in the RSX FIFO.
- Every hung run logged `[cellGcmSys] title-ring recycle ... why=flip`. The run that kept going did not.

**Cause**
- Demon's Souls points its context's `current` past `end` on purpose, because the SPU pushbuffer lives beyond the 32 KB PPU ring.
- The recycle guard took "past end, get != put, nothing moving for 100 ms" as a wedged walker.
- A walker parked at a flip (one flip per drain pass) meets that test whenever a frame takes over 100 ms.
- The recycle then rewound put and get to `begin` and dropped the pushbuffer behind the flip, label releases included.

**Fix** (ps3recomp `cellGcmSys.c`): a pass that stopped at a flip does not count toward the overrun timer. With the fix, the run keeps going past 200 s at about 145 flips/s.

New diagnostics:
- `SPU_ATOMHIST=<sec>`: the busiest atomic (img, cmd, line, pc, lr) sites, with the line contents.
- `COND_SHADOW=1`: the shadow stack of any thread blocked on a cond for more than 3 s.
- `COND_TRACE=<ids>`: waits and signals on those conds.
- `PAD_TRACE=1`: a once-a-second count of cellPad polls.
- `tools/des/dbgsession.sh`: runs console commands after N seconds.
- `tools/des/dbghang.sh`: polls flips and runs commands once they stop.

## After OK: trophy check, then the logos

Pressing CROSS (`PAD_FILE`) dismisses the notice, and the game shows "Checking Trophy Set... Do not turn off the system." (`sceNpTrophyRegisterContext` on `DsTrophyMan::AsyncUpdateThread`).

The game now writes its system save to `gamedata/dev_hdd0/home/00000001/savedata/BLUS30443DEMONSS005`. That is git-ignored, and it changes the next boot.

Later runs with a second press get past the trophy check to the Atlus logo, then the "Sony Computer Entertainment presents" logo. The game stalls on the Sony logo at under 1 FPS.

**RPCS3 difference to keep in mind:** RPCS3's `RegisterContext` blocks while it delivers SETUP (0..3), PROGRESS (0..n), FINALIZE and COMPLETE, one per `cellSysutilCheckCallback`. Ours calls INSTALLED synchronously and queues one COMPLETE. The game's own code after `RegisterContext` sets status 8 (COMPLETE) itself when the call returns 0.

## Logo phase: slow, not hung; CPU spin removed

Runs vary a lot now that a save exists. Some reach the notice within a minute; others sit on the loading screen for minutes. The logos advance (Atlus → Sony → Japan Studio), but sometimes at well under 1 presented FPS. They also have no fade transitions; that is noted for later.

`stat`'s flips count decoded FIFO flip words, not presents. In a slow run it drops from about 146/s to about 5/s.

**What a slow run showed**
- `COND_SHADOW`: MAIN blocked over 3 s at a time on the Havok barrier (cond 13, `func_01328BB8`, called from `0x001D1A80`).
- `COND_TRACE=13,19` (now timestamped): per frame, MAIN waits on cond 13 and tid 11 (Havok) signals it; tid 11 waits on cond 19 and MAIN signals it.
- In a fast run the MAIN side takes under 16 ms. The Havok side has occasional waits of 1–3 s.

**CPU.** The 100% core was the img-2 HighGraphics ring consumer re-reading the ring's write pointer about 8M times/s with GETLLAR.
- After 20k unchanged reads of the same line, the SPU runtime now naps 1 ms per read. That stands in for the lock-line-lost wait hardware would do. `SPU_LLAR_NAP=0` turns it off.
- Since then no host thread is pinned.
- An A/B run showed no slowdown from the nap; run-to-run variance is far larger.

**Follow-up (ps3recomp `d9292c8`): idle pollers sleep for real**
- The GETLLAR back-off now blocks on the SPU context's channel cv until a write drops the reservation (`spu_coh_notify_write` → `spu_ch_wake`), with a 2 ms safety timeout.
- `sys_timer_usleep`: after 64 back-to-back sub-ms sleeps, a thread sleeps on a high-resolution waitable timer instead of the QPC/`SwitchToThread` spin. `USLEEP_SPIN=1` restores the old behaviour.
- `desrun.sh` (and so every debug script) kills its own leftover `des_boot.exe` runs (by pidfile) before a run. It also runs the game at BelowNormal priority on half the logical CPUs: `PS3_PRIORITY`/`PS3_AFFINITY`, read by `boot_main` and set only by the scripts (`tools/des/deslib.sh`), never by the runtime.

Measured with `tools/des/cpusample.sh` (total cores via perf counter, busiest threads mapped to guest tids):
- In the logo phase the game now uses about 0.6 cores.
- MOStreamingThread (tid 18) takes about 40%, mostly in ntdll wait/yield. Its loop at `0x00D64E58` does one streaming step and sleeps 5 ms.
- One SPU thread takes about 18%. HighGraphics is near 0.

**Logos and loading.** In one run the Atlus logo stayed up for more than 107 s, while the game streamed SDATs and wrote its decompressed HDD cache (`Ftruncate`s). The logos look held until a background load finishes. The debug scripts used to wipe that cache before every run, so each run paid the full decompression; they now keep it (`COLD_CACHE=1` wipes). Load throughput is latency-bound (little CPU in use), not compute-bound.

## Loading is not the gate

`FS_TIME=1` (per-fd open/read/close timeline) shows the logo phase's file I/O finishing fast and in the same order as RPCS3. The last open before the logos is `facegen.fgbnd`, as in RPCS3. Slow logos are a rendering/frame-pacing problem, not a loading one.

## Logos froze on Havok: the flip-time ring wrap (fixed)

**Symptom.** The logos ran Atlus → Sony → Japan Studio → FromSoftware → Havok, then the Havok logo stayed up for good. The game kept presenting, but RSX label 255 stopped moving. HighGraphics paces every frame on that label.

**Evidence** (`desprobe` with `EXIT_ON=gcm-wedge`):
```
[cellGcmSys] flip-wrap: JUMP->begin written at io=0x00009B4C (get=0x005FF6A4 end-cur=0x64A0)
[gcm-wedge] label255=0x339 unchanged; get=0x00009B4C put=0x0000D804
```

**Cause.**
- `gcm_flip_into_fifo` (ps3recomp `cellGcmSys.c`) wraps the title's ring at a flip: it writes JUMP->begin and sets `current = begin` once the walker is a margin past `begin`.
- That test also held when the walker was in a different buffer. Demon's Souls' PPU ring is 32 KB (io `0x8000`–`0x10000`). Each frame continues into an SPU-built pushbuffer higher in IO space (`get` around `0x5FF6A4`).
- The wrap rewound the PPU ring under the title. The walker then replayed `begin`..JUMP, and the label releases in the SPU pushbuffer never ran again.

**Fix** (ps3recomp `5b6e6bb`): the flip-time wrap also requires the walker to be in this ring, at or behind the write position (`io_begin + margin <= get <= io(current)`). With the fix there is no flip-wrap, label 255 keeps advancing, and the game gets past the logos to cellSail.

Only Demon's Souls was tested. VF5, The Simpsons Arcade Game, Tornado Outbreak and Tokyo Jungle, which the surrounding ring code was written for, were not re-run.

**Still open.** The title-ring recycle still treats "past `end`, nothing moved for 100 ms" as a wedge. That is a timing rule. It doesn't fire for Demon's Souls since the flip exclusion (above). A guest-state replacement needs either the title's callback result (hooking a title-installed callback) or a walker-past-end test that also handles Demon's Souls' deliberate run past `end`, and VF5 to test it on.

## After the logos: cellSail, and a full taskset

The game loads the PAMF and SAIL modules and builds a cellSail player for `movie/ds_advertise.pam`. It calls `PlayerInitialize2`, the adapters, `PlayerBoot`, `CreateDescriptor`, `AddDescriptor`, then `PlayerOpenStream`. Ours fires the same completion events RPCS3 does (RPCS3's `OpenStream` is an empty stub). The game never calls `PlayerStart`. The screen goes black, and presents drop to about 1/s while label 255 keeps moving slowly. `gstacks` 20 s later: HighGraphics (tid 4) and MAIN (tid 1) are still in their usual per-frame waits.

**Task ids ran out (fixed, ps3recomp `0320c58`).** In every run, `cellSpursCreateTask` failed with "no free task id (all 128 enabled)" on two tasksets: several hundred to about 2,000 times each. The one-time dump at the first failure (`efae23c`) told the two apart:
```
taskset 0x48194800 bits: running=0 ready=1 pending=127 enabled=128 ...
  elf=0x0179CC00 holds 64 ids / elf=0x017ACD00 holds 63 ids
taskset 0x477ED800 bits: ready=128 pending=0 enabled=128 ...
  elf=0x017BE200 holds 128 ids; 1009 task threads alive, 0 parked
```
- **Tasks created by SPU code never started.** SPU images 3 and 4 add tasks themselves. `SPU_ATOM_EA` showed one PUTLLC setting the enabled bit (pc `0x079EC`/`0x0794C`) and a second setting pending_ready (pc `0x07C2C`/`0x07BB4`). That is what RPCS3's PPU-side create plus `task_start` do. On hardware the taskset policy module then schedules them. Ours only started pending tasks when another task of the same taskset exited. Now a committed SPU lock-line write to a known taskset's header starts its pending tasks.
- **PPU-created tasks could start twice.** A task was marked running only once its thread started. An exit in between started it again, and under load this fed itself (Havok: 1009 threads for 128 ids, under 1 frame/s). It is now marked running at dispatch.

With both fixes, no taskset runs out of IDs, and the game reaches cellSail after the logos at about 15 presents/s. One of two runs sat on the pre-notice loading screen for 6 minutes instead (it never reached `cellSaveDataAutoLoad2`). That variance existed before.

After `PlayerOpenStream`, three threads were sampled once with `gstacks`. HighGraphics (tid 4) was in `00B0A5A8 ← 00B0A428 ← 00A164B8 ← 00ACA470 ← 009F81F0`, and tid 23 was in `00B0A5A8 ← 00B0A428 ← 00EC1090`. `00B0A5A8` calls a reporting routine (`func_00A9DD58`: prints, then writes `0xDEADBA` to address 0) only on its error path, so it is probably a lock wrapper. A single sample doesn't show whether it blocks there.

## The movies, and the title screen (fixed)

The game drives its movies through cellSail, and our HLE answered a different protocol. Traced from the game's handlers (sail callback OPD `0x0198A1C8` -> `0x00FD9120`; after boot, `0x0198A170` -> `0x00FD8378`; both post each event to the game's own event port, drained one per frame by `0x00FDBB10`, jump table at `0x00FDBC54`):

| Event (major) | What the game does |
|---|---|
| CALL_COMPLETED (2), minor BOOT | `cellSailFutureSet(arg1, arg0)`, then `ReplaceEventHandler`. arg1 must be the userParam (a CellSailFuture). |
| STATE_CHANGED (3) | stores arg0 at player+0x718; Start is only called once it reads OPENED (4) |
| STREAM_OPENED (4) | arg0 = descriptor: `GetStreamType`, `CreateDatabase` (a PAMF reader), `OpenEsVideo` |
| STREAM_CLOSED (5) | destroys the descriptor, sets "closed"; the close loop also waits for CLOSED (2) |
| SOURCE_EOS (8) | sets an "ended" flag |
| ES_OPENED (9) | arg0 = ES type. VIDEO: pick the audio track with cellPamfReader calls, `OpenEsAudio`. AUDIO: movie ready |

Our player sent only state changes with arg1 = 0, and the futures, `ReplaceEventHandler`, `OpenEsVideo/Audio` were unregistered (each silently returned CELL_OK; the "unresolved NID" log had a 40-line cap that one polled NP import used up, fixed in `b6a0d00`). The cellPamf reader also parsed an invented header layout. Fixes: cellPamf `ce105e6` (real PAMF layout, all 25 exports), cellSail `d149e20` (the event protocol above, real futures, ABI fixes).

Result: `ds_advertise.pam` (M2V 1280x720 + 6-ch LPCM, 130 s) and then `ds_title.pam` (2-ch, 10 s) both go Boot -> Open -> ES open -> Start -> Close -> Finalize, and **the title screen renders**: the Demon's Souls logo and "PRESS START BUTTON", at about 28 presents/s. Nothing decodes yet, so each movie "ends" the moment it starts: the intro is skipped, and the title logo comes from the game's own fallback drawing after `ds_title.pam` ends.

![title](../work/tmp/shots/f029.png) (local only, not committed)

## New tools (this round)

| Knob / tool | What it does |
|---|---|
| `EXIT_ON=<tag,...\|any>` | Exit when a tagged report fires (`gcm-wedge`, `task-ids`). `EXIT_DELAY_MS` lets the console reply finish. |
| `EXIT_ON_HLE=<nid>`, `EXIT_AFTER_MS=<ms>` | Exit on the first call of an import, or after a time. |
| `HLE_FIRST=1` | Log the first call of each import (tid, lr). |
| `[gcm-wedge]` | Label 255 frozen while flips keep coming (`GCM_WEDGE_FLIPS`, default 64). Prints the walker's last steps. |
| `GCM_RATES=1` | Presents/s, FIFO flip words/s and label 255, once a second. |
| `FS_TIME=1` | Per-fd file timeline. |
| `CELLMARK_DUMP_CHANGED=1` | Keep a screenshot only when the image changed. |
| console `gstacks [n]` | Shadow stacks of every guest thread. |
| `PS3_VERBOSE=0` | Quiet per-call logs (the scripts' default; the logging lock perturbed timing). |
| `tools/des/deslib.sh` | Shared script defaults. Paths come from env, with this machine's paths only as fallbacks. Affinity is half the logical CPUs. A pidfile means scripts kill only their own runs. |
| `tools/des/desprobe.sh` | One event-driven run: presses CROSS until a pattern, runs console commands on `WHEN=<regex>`, keeps changed shots, and returns when the game exits. |
| `COLD_CACHE=1` | Wipe the HDD cache first (the default now keeps it). |

## Next

- Press START on the title screen and follow what comes next (main menu, then the save load).
- Real movie playback: demux the PAMF (M2V + LPCM) and decode on the host, deliver frames through the graphics adapter, and end the movie on real end of stream instead of at once.
- The pre-notice loading-screen stall (boot variance).
