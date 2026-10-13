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

**Fix** (ps3recomp `9610a37`, upstream branch `fix/lifter-code-window-segment`): the default window now starts at the executable PT_LOAD that holds the first function. The upper bound is unchanged; that is where data misread as code actually lives.

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
  --override-funcs "0x167AC88=ds_malloc,0x167AC58=ds_free" --shadow-stack \
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

## Next

Catch a slow logo run and find what the Havok thread (tid 11) waits on during its multi-second gaps. Expected suspects are SPU task completion delivery (image 7 tasks, evq 5) and the image-7 task spawning (one host thread per task).
