# Image 2: a cheaper DMA path and the native ring loop

Date: 2026-10-21 (overnight). Status: merged 2026-10-21 as two ps3recomp PRs, with the port pinned at the fork's merge commit 3f88c24:
- #90: DMA fast path, ring fetch and /GS checks;
- #91: native loop for commands 7 and 8.

Both are measured, and the loop was checked against the lifted code. Bases: ps3recomp `fork/master` = 19c8c97, port `origin/master` = 29aefa5. ps3recomp #89 (the usleep yield, docs/71) merged with them.

Labels as in docs/48: **measured** (from a run, source given), **read** (seen in the code or the lift), **inferred**.

## Short answer

- **Image 2 still sets the frame** in the deep Palace (measured, same-build slope with a scratch `TMP_SLOW_RING_NS` knob):
  - on master, 20 ns added to every ring command cost 15% of the frame rate (147 -> 124 fps), and 50 ns cost 24%;
  - with both PRs, 10 ns cost 8.7% and 30 ns cost 21.6%.
  - The spin rounds up to about one `pause` (~35 ns), so the added time shows in the frame almost one for one.
- **#90**, the DMA and fetch trims: +4.6% to +8.8% a round against master. A further +1.8% comes from removing /GS cookie checks.
- **#91**, the native loop: +4.2% against master on its own.
- **Both together: 146.3 -> 162.2 fps (+10.9%)**, every round. 1% lows rise from about 93 to about 108 fps.
- **With #89 as well** (the merged state, last A/B of the night): 146.7 -> 163.1 fps (+11.2%), every round; 1% lows 88-96 -> 107-122 fps.
- The two PRs are both based on master, touch different hunks of `spu_ring_hook.c`, and merge cleanly in either order.

## Measurements

All runs: deep Palace save (`work/tmp/lc_deep/save` copy), `RSX_VSYNC=0 DES_VSYNC=0 DES_FPS=unlocked VBLANK_MHZ=960000`, `RSX_PRESENT=0` (session locked). exeab and knobab, 3 rounds; B beat A in every round unless noted.

| A/B | A fps | B fps |
|---|---|---|
| #90's helper changes vs master (exeab) | 141.2 | 150.3 |
| forcing those helpers inline (exeab) | 153.2 | 152.6 (dropped) |
| the loop knob, same build (knobab) | 150.5 | 154.8 |
| fetch registers in locals (exeab, loop on) | 155.8 | 162.0 |
| no /GS cookie checks (exeab, loop on) | 160.5 | 163.4 |
| #91 alone vs master (exeab) | 146.2 | 152.3 |
| **#90 + #91 vs master (exeab)** | **146.3** | **162.2** |
| loop v2 (also 10-13, 17, 18) vs v1 (exeab) | 162.4 | 162.8 (dropped) |
| the coherent PPU store as one call (exeab, on top of both) | 161.9 | 162.9 (B won 2 rounds of 3; dropped) |
| **#89 + #90 + #91 vs master (exeab)** | **146.7** | **163.1** |
| loop on vs off, same build, 9 windows each (knobab) | lows 99-122 | lows 90-108; one round of three favoured off on the mean |

## What #90 changes (read)

About half of image 2's samples were small runtime helpers, and most of that was function entries: the cost was calls and lookups, not work.
- `mfc_for`: the TLS slot check is inline, and the search and claim are out of line.
- `spu_wrch` runs `mfc_fast_xfer` itself for a plain GET or PUT.
- `mfc_repeat_check` keeps its count in the MFC engine rather than three TLS variables.
- Quadword copies of up to 256 bytes are done inline.
- The coherence check per 128-byte line is inline.
- **Store forwarding.** The ring fetch wrote SPU registers into `ctx->gpr` a word at a time and read them back as 16 bytes, which the store buffer cannot forward. Those were three of the four hottest addresses in the fetch. The fetch now computes in locals and only stores.
- **/GS cookies.** clang-cl adds a stack-cookie check to any function that copies a `u128` argument onto its stack, takes a local's address, or has a byte array. The fixes:
  - the LS write probe now takes the stored bytes, which removed the cookie from every lifted region; the exe is 490 KB smaller;
  - `mfc_fast_xfer` returns its result instead of writing it through a pointer;
  - the hooks use `spu_wrch_word`;
  - the overlay dump is out of line.

## What #91 changes (read)

`SPU_RING_HIST` in the deep Palace (measured): command 8 is 68% of the ring, 7 is 12%, and nothing else is above 3.2%. At about 4.9 million commands a second, that is roughly 32,000 a frame at 150 fps.

With `SPU_NATIVE_LOOP=1` (default), `spu_hook_ring_fetch` runs a fetched 7 or 8 to the end itself, then fetches the next command:
- the 0xC358 bracket and `spu_hook_put_wait`;
- the handler, as the lifted instructions with the dead volatile registers in locals;
- `spu_hook_ring_post`;
- `ctx->steps`.

Every way off the fast path is checked before the command's first side effect, and any of them leaves that command to the lifted code. The full list is in the code comment and the PR.

**Check mode** (`SPU_NATIVE_LOOP=2`): every Nth command runs lifted against a prediction made on a copy of local store, and the next fetch compares all of local store outside the frame.
- 0 mismatches in about 2.4 million comparisons, over three saves.
- A planted fault was caught on the first sampled command.

## Tried and dropped

- **Forcing the DMA helpers inline**: no gain.
- **Loop v2**: adds commands 10-13, 17 and 18, with the GETs through an emulated 0xC4D8 main case. Check mode passed 2.1 million comparisons, and a planted GET fault was caught. But there was no measurable gain over v1 (162.4 vs 162.8), so it is not in #91. The code is kept locally in `work/tmp/scratch_20261021/perf/loop_v2.py` if image 2 becomes the limit again.
- **SSSE3 LS accessors** (one load or store plus `pshufb`): clang already turns the four swaps into a `pshufb` in the lifted regions (1,782 shuffles and 8 scalar swaps in region 0x8CB8). Not measured, reverted.
- **The coherent PPU store as one call.** A PPU store into a reserved line made four calls: take the stripe, store, notify, release. A scratch `spu_coh_store` did them in one, with one stripe for a store inside one line. +0.7% over three rounds, but A won one of them, so it is inside the noise. The patch is kept locally in `work/tmp/scratch_20261021/perf/coh_store.py`, and the slope knobs in `causal.patch` next to it. The lock itself is the cost (next candidates, item 1), not the calls around it.

## Where the frame is now (measured, combined build, `cpu 4`)

- **HighGraphics**: 98% of a core, and only about 12% of that is in usleep (it was half before).
  - About 12% of the thread is the lock-line path: `spu_lockline_unlock_ea` 4.5%, `spu_ll_acquire` 3.2%, `spu_lockline_lock_ea` 3.0%, `spu_coh_notify_write` 1.5%.
  - `PPU_COH_HIST` shows the cause: about 5.3 million stores a second to line 0x01B51080, the ring's put pointer, which image 2 holds a reservation on. That is one store per command.
- **FIFO thread** (`vblank_ticker`): 92%.
  - memcpy 11.8% (vertex spans into `vf_buf`), `d3d12_draw_indexed` 10.2%, `upload_strip_vp_indexed` 6.9%, the FIFO parse 6.5%, `rt_writeback_store` 6.2%.
- **Image 2**: 92%, about 7% of it napping.
  - `ring_loop` 12.8%, region 0x8CB8 9.8%, `mfc_fast_xfer` 9.4%, `spu_wrch_word` 6.8%.
  - Commands 31-34 (regions 0x8CB8, 0x7140, 0x59C0, 0xCA40, 0xAB30) together are about a quarter of image 2, for about 2.5% of the commands.

All three are close to a full core, so the next gain on any one of them will be smaller than its share suggests (inferred).

Same-build slopes on the combined build at about 161 fps (measured, scratch `TMP_SLOW_*` knobs, 6 interleaved pairs each). A slope is the share of the added time that shows up in the frame:

| Busy wait added | fps A -> B | Frame time added | Slope |
|---|---|---|---|
| 300 us per flip, HighGraphics (`TMP_SLOW_FLIP_US`) | 161.2 -> 155.5 | +0.23 ms | 0.75 |
| 300 us per present, FIFO thread (`TMP_SLOW_PRESENT_US`) | 161.0 -> 154.0 | +0.28 ms | 0.94 |
| 30 ns per coherent PPU store, 39,000 a frame (`TMP_SLOW_COH_NS`) | 161.4 -> 144.9 | +0.71 ms of 1.18 | 0.60 |
| 10-30 ns per ring command, image 2 (`TMP_SLOW_RING_NS`) | see the short answer | | about 1 |

All four sit on the frame's path. No single thread is the limit any more.

## Next candidates (inferred, for the decisions list)

1. **The put-pointer store's lock** (HighGraphics, about 12%). Every PPU store to a reserved line takes the stripe lock, notifies and unlocks. *Update 2026-10-21: built behind `SPU_COH_FAST` and measured, +0.4% (inside the noise). HighGraphics waits on image 2, so the time it saves does not reach the frame. Parked; see docs/78.*
   - An exact cheaper path needs a reservation design that does not lock plain stores. RPCS3 instead compares data at PUTLLC time.
   - That is a design change on the path that froze the game before (docs/48 risks, the lwarx and PUTLLC notes). It needs its own plan.
2. **Commands 31-34 natively** (docs/48 Stage 4). About a quarter of image 2. Region 0x8CB8 alone is about 1,950 SPU instructions, so this is a project of its own.
3. **The FIFO thread**: the render thread (docs/68) and GPU fetch for lists and quads (docs/64) are still the big items there. docs/66 rows 1, 2, 5 and 8 are already in master.
4. **Havok's image 8** (added 2026-10-21, docs/73). It is not on the deep Palace frame. But after a break it sets the length of the physics step, and with it the frame rate for 2-4 s: about 10% lower, with step frames of 7-20 ms. It needs the same kind of work as image 2, after a profile of image 8 on its own.
