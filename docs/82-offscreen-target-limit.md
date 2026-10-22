# 82. The 16 offscreen targets: what raising the limit would take (study, not built)

docs/37 left one render-scale item open: "all 16 offscreen targets in use". This note says where the limit is, when Demon's Souls reaches it, and what a change would need. Nothing here is built. The code read is fork master, `libs/video/rsx_d3d12_backend.c`.

## 1. The limit

- **The table.** Each guest colour surface the title renders to gets an `OffRT` entry: the D3D12 texture, its views, an upload buffer, a readback buffer, a sampling snapshot, and at a render scale above 100% a guest-size copy for the write-back. The table has `MAX_OFF_RTS` = 16 entries (line 315), named after the demosaic pass chain it was sized for.
- **Taking an entry** (`off_rt_get`, ~6804):
  1. a free entry;
  2. else the first entry not used this frame, which is evicted, so it loses its content (`[RT] evicted ...`);
  3. else none. The surface is not created, its draws are dropped, and anything that samples it reads nothing (`[RT] all 16 offscreen targets in use this frame`).
- **"Used"** is set when a frame binds or re-creates the entry. It is cleared for every entry at the end of each frame (~9199).

## 2. When the game reaches it

| Where | Evictions | "All in use" |
|---|---|---|
| Deep Palace, the A/B runs and checks of 10-21 night (`lc_diff_k1`, `lc_us_k1`, `lc_chk2`) | 1 per run: a 128x128 target for a 160x90 one | 0 |
| The regress run of the live-settings build | 1 | 0 |
| Menus, at 1x and at a scale (docs/37) | often | yes |

So in play the table is full but holds. In menus it overflows. docs/37 traced the coloured noise under the Profile List's footer at 200% to this: targets re-created every frame, each filled from guest memory.

## 3. What a change would need

### 3.1 More entries

- **The constant.** `MAX_OFF_RTS` sizes the table, the RTV heap (~2461), the write-back views (`s_down_srv`, 2 per entry, ~7123) and the shadow arrays (~7332). All of those follow the constant.
- **The SRV heap does not follow it.** Each entry's shader view sits at heap slot `RT_SRV_BASE + i`, that is slots 5-20 (line 316).
  - Slot 21 is the AA pass's source (`AA_SRV_SLOT`, ~8617).
  - Slots 22-31 are spare, and the per-draw windows start at `DRAW_SRV_BASE` = 32 (line 319).
  - So up to 26 entries fit as the heap is laid out: slots 5-20 plus 22-31, which needs a two-part mapping from entry to slot. 32 or more needs the target views moved to the end of the heap (after `VP_TEX_SRV_BASE + VP_TEX_SLOTS`, with `SRV_HEAP_SIZE` grown by `MAX_OFF_RTS`). Draws copy those views into their windows by index, so nothing else depends on where they sit (inferred; to check at each `RT_SRV_BASE` use).
- **Lookups.** `off_rt_find` scans the table linearly. There are 13 call sites, among them the per-draw texture match (about 5,800 lookups a frame at the Palace, docs/56). At 32 entries that is about 185,000 compares a frame, on the order of 0.05 ms on the FIFO thread (inferred). A small open-addressed hash by guest offset would make the size free.

### 3.2 Memory

- **Size per entry.** An entry's resources scale with the render scale. A 1280x720 FP16 target at 200% is 2560x1440x8 B = 29.5 MB for the texture, the same again for the snapshot, plus a guest-size copy, upload and readback (7.4 MB each). That is about 80 MB for one large entry (inferred from the formats in `off_rt_get`).
- **Most entries are small.** They are the exposure chain, shadow maps and blur steps. Today's 16 already include all the large ones, so 32 entries would add mostly small targets (inferred).
- **What to change.** Evict the least recently used entry (a frame number per entry) instead of the first unused one. Then cap the table by bytes as well as by count, so a long menu session at 400% cannot grow without bound.

### 3.3 The real fix for menus

An entry is evicted when the frame did not touch it. In menus the same targets come back every few frames, so recreating them is what costs: each comes back filled from guest memory. With LRU eviction and about 32 entries the menu set should stay resident. Check that by counting `[RT] evicted` lines in a menu session, before and after.

## 4. Size and order

1. A counter first (tooling, ~20 lines): per frame, the number of entries in use and the evictions, behind a knob. Then run one menu session and one Palace session at 100% and 200%, which says how many entries the game needs.
2. LRU eviction (~30 lines): no behaviour change while the table has room.
3. `MAX_OFF_RTS` = 32 with the target views at the end of the SRV heap (~40 lines, every `RT_SRV_BASE` use checked), plus a byte cap.
4. Check:
   - regress;
   - the menu screenshots at 200% (the Profile List footer);
   - a Palace knob A/B for the lookup cost;
   - the per-draw fingerprints of a menu frame (F9 capture, `tools/des/capdiff.py`) before and after, which should differ only where a target used to come back empty.

About one day in all. Nothing in it touches the guest-facing behaviour except that targets stop losing their content.

## 5. The occlusion counts at a render scale (measured, 2026-10-22)

The other open render-scale item (docs/37): `ZPASS` reports at a scale above 100%. A scaled target covers more samples, so `zq_finish` divides each query's count by the target's area ratio, rounding up, before it stores the report.

One boot each at 100% and at 200% (`RSX_SCALE`), `ZPASS_LOG=200000` (every report printed), fork master plus the `VP_FETCH_STATS` patch, not presenting:

| Save | Reports at 100% | At 200% | Values |
|---|---|---|---|
| Deep Palace (`lc_deep`) | 0 | 0 | the spot issues no occlusion queries |
| The regress save (Tutorial, `m08`) | 407 | 407 | every one is report 1024 = 576, at both scales |

- At 200% the query covers 4 x 576 = 2,304 samples (inferred: the raw count is not printed), and the division gives 576 again, the same value the game reads at 100% (measured).
- Every query at that spot is fully visible, so a partly hidden query (where the rounding up matters) was not exercised. A spot with a flare behind geometry would test that.
