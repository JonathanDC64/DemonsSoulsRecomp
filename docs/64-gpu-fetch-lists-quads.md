# 64. Triangle lists and quads on the GPU fetch path (static study, 2026-10-19)

Status: static study for docs/56 section 6, row 4 ("triangle lists and quads (and the strips that fall back) on the GPU
fetch path", 0.5-1.3 ms off the FIFO thread, 3-4 days). Nothing was built or run. The code is ps3recomp `fork/master`
at 4a76fc0 (the merge of #71, 10-19 18:47). Line numbers are in `libs/video/rsx_d3d12_backend.c` unless a file is
named. "Inferred" marks what the code or profiles do not show directly.

The FIFO thread study (docs/66, written at b249b7a) has older line numbers. Its Q2 is the starting point here.

## Short answer

- **Lists and quads alone are small at the Palace.** In prof3 (docs/56 section 4), `upload_tris_vp_indexed` is 0.6%
  of the FIFO thread exclusive and 2.4% inclusive, about 0.2 ms a frame. `upload_quads_vp_indexed` is not in prof3's
  FIFO profile at all, whose symbol list runs down to 0.1%. It may be inlined into `d3d12_draw_indexed`, so its cost
  is unknown.
- **The strips that fall back are the bulk of the 0.5-1.3 ms.** `upload_strip_vp_indexed` is 7.0% inclusive and
  `vpj_sync` (the FIFO thread converting vertices itself at sync points) 6.6%: about 1.15 ms a frame. No log says
  which of the seven fallback tests sends them there.
- **So step 0 is a stats run, not code.** The counters exist but merge reasons and ignore lists and quads. A
  30-50 line tooling change and one run at the deep Palace save size every later step.
- **Lists and quads need no shader, root signature or PSO change.** The fetch vertex shader reads by `SV_VertexID`
  and does not care about the primitive (`rsx_vp_decompiler.c:569-610`, `:676-680`). Root parameters 4 (t20, the
  ring) and 5 (b2, the descriptor offset) exist (2043-2051). A list draw is `fetch = 1`, which the VP pass already
  draws as a `TRIANGLELIST` (9550-9554). The work is all on the FIFO side: an index walk per primitive, reuse of the
  strip path's span and descriptor code, the record merge, and a shared carry so fetch and CPU ranges can mix in one
  BEGIN/END.
- **Expected gain:** lists and quads 0.15-0.25 ms a frame off the FIFO thread (inferred), about +2-3% fps where the
  FIFO thread holds the frame. That clears the in-boot knob bar (about 1-1.5%), not the cross-build bars (2.8% profile
  fps, 4.5% gap fps). With the fallback strips fixed too (step 7), up to about 0.6-0.85 ms, capped by the next thread
  to saturate: +5-8% (inferred).
- **Size:** steps 0-6 (stats, lists, quads, check mode, fingerprints) about 300-350 new lines plus 60 moved, 2-2.5
  days. Step 7 (sparse strips) another 150-200 lines, 1.5 days. 3.5-4 days in all: the top of docs/56's estimate.
- **Main risk:** the gain. Lists and quads alone may read inside the noise, and step 7's value depends on the
  stats run. The main correctness risk is triangle phase across index ranges when fetch and CPU ranges mix.

## 1. How a draw flows today

### Entry

- `NV4097_DRAW_ARRAYS` calls the backend's `draw_arrays` (`rsx_commands.c:898-923`). `NV4097_DRAW_INDEX_ARRAY` calls
  `draw_indexed` (`rsx_commands.c:950-957`). Both pass `state->primitive_type`, `first` and `count`.
- `begin_epoch` counts BEGIN/END pairs (`rsx_commands.c:850`). Ranges of one BEGIN/END share it.
- Restart state: `SET_RESTART_INDEX_ENABLE` and `SET_RESTART_INDEX` (`rsx_commands.c:934-941`).
- Primitive numbers: TRIANGLES 5, TRIANGLE_STRIP 6, TRIANGLE_FAN 7, QUADS 8 (`rsx_commands.h:209-212`).

### Indexed draws: `d3d12_draw_indexed` (12393-12576)

| Step | Lines | What |
|---|---|---|
| Strip merge anchor | 12425-12438 | `mpv`: the last record, if it continues this strip or fan range (same BEGIN/END, same FP, indices right after). Strips and fans only. |
| Fetch gate | 12444-12462 | STRIP or FAN and `vf_on()` (11784-11789: `VP_GPU_FETCH`, the ring mapped, `s_vp_fast`, not `late_on()`) -> `upload_strip_vp_fetch`. A range that continues a fetch draw with the same descriptor just extends it (12455-12459). |
| CPU dispatch | 12464-12477 | QUADS -> `upload_quads_vp_indexed`, TRIANGLES -> `upload_tris_vp_indexed`, STRIP/FAN that fell back -> `upload_strip_vp_indexed`. |
| CPU strip merge | 12479-12488 | a CPU range extends a CPU strip record (`s_strip_base`). |
| List batch merge | 12489-12507 | CPU TRIANGLES only: a range right after the last record's vertices (same FP, same BEGIN/END, `index_count == 0`) extends it. This is what keeps lists in phase when a range is not a multiple of 3. |
| Record | 12509-12575 | one `D3D12DrawRecord`. `vs_idx` is `vp_get_vs_fetch` for fetch draws, `vp_get_vs` otherwise (12551-12552). `fetch`/`fdesc` at 12553. Merge anchors at 12571-12573. |

**Strips and fans on the fetch path: `upload_strip_vp_fetch` (11962-12118).**
- Tests in order (each returns -1, the CPU path): attribute 0 off (11966); index room under `count*3+4` (11967); an
  enabled array of type 0 or above 7, or CMP = 6 (11970-11973); the range spans more than `count*4+256` vertices
  (12010); a span over 16 MB (12019); main-memory IO pages not one linear run, or past 4 GB (`vf_resolve`,
  11793-11803, at 12021); the ring full (12070).
- Index walk: a strip goes into the index buffer as a strip (`vf_strip_native`, 11877-11955, SSE2 for 16-bit
  indices), with the restart index as the cut value 0xFFFFFFFF (`VF_CUT`, 11871). A fan is walked into a list
  (11994-12005). The carry across ranges is `s_strip_carry` (declared 11319), read at 11978-11982,
  written at 12104-12106.
- Spans and descriptor (12006-12102): per array, the bytes `[lo, hi]` reach; a range that continues the last
  descriptor reuses it or appends to its last span (12025-12053); otherwise a new 512-byte descriptor (16 records of
  32 bytes) and the merged spans are copied into `vf_buf` (12054-12101).
- Record: `fetch = 2` (strip) or `1` (fan as list), `vb_byte_offset = 0`, the indices in `vp_ib`.
- Stats: `VP_FETCH_STATS=1` prints six counters every 20,000 calls (12109-12117): new, reuse, append, sparse
  (sparse + over 16 MB), type/addr (type + resolve), room (room + ring full).

**Strips that fall back: `upload_strip_vp_indexed` (11640-~11760).**
- `vp_plan_begin` (10763-10776) builds the fetch plan a second time (`upload_strip_vp_fetch` already built it at
  11969).
- Each unique vertex (a 256-entry direct-mapped dedupe, 11711-11717) is converted to 16 float4 (256 bytes): inline
  through `read_vp_vertex` (10778-10797) -> `rsx_fetch_vertex_arrays` (`rsx_vertex_fetch.c:164-200`), or queued to
  the one `vpj_worker` (11700, 11729). `vpj_sync` (10912) drains the queue at every guest-visible sync point, and the
  FIFO thread runs unclaimed jobs itself there.
- Indices are u32 into `vp_vb`; the draw is `DrawIndexedInstanced` with a base vertex.

**Indexed lists: `upload_tris_vp_indexed` (11385-11630).**
- No restart (11402-11404): for every index, `read_guest_index` (11295-11309; it resolves the index array's base
  through `cellGcmResolveLocated` for every index) and `read_vp_vertex_c` (10802-10812: a 256-entry cache, then a
  256-byte `memcpy` into the write-combined `vp_vb`). One 256-byte vertex per index, not per unique vertex.
- Restart (11405-11434): the restart index drops the triangle in progress; a partial triangle carries into the next
  contiguous range of the same BEGIN/END through a function-static `carry` (11418-11433). Only whole triangles are
  written (`count = o`, 11434).
- Without restart there is no carry: a range writes all its `count` vertices, and the batch merge (12489-12507) keeps
  the stream in phase. If that merge fails, the 1-2 leftover vertices are dropped by `DrawInstanced` and the next
  record starts out of phase (inferred from the code; docs/28 line 323 says the record concatenation kept it in phase
  in practice).
- Record: `index_count = 0`, drawn with `DrawInstanced` (9559-9561).

**Indexed quads: `upload_quads_vp_indexed` (11341-11383).**
- `quads = count / 4` (11348); a leftover 1-3 indices are dropped. No restart handling, no carry, no merge (the merge
  at 12492 is TRIANGLES only).
- Per quad: 4 x `read_guest_index` + `read_vp_vertex_c` into a stack copy, then six 256-byte copies in the order
  0,1,2 / 0,2,3 (11359-11360).
- Record: `index_count = 0`, `DrawInstanced`.

### Non-indexed draws: `d3d12_draw_arrays` (12122-~12392)

- QUADS -> `upload_quads_vp` (11178-11212) at 12149-12157: the same 0,1,2 / 0,2,3 split, six 256-byte vertices a quad.
- TRIANGLES -> `upload_tris_vp` (11214-11255); STRIP/FAN -> `upload_strip_vp` (11263-11290), expanded to a list on the
  CPU (12259-12266). The batch merge for lists is at 12276-12290.
- All CPU path; nothing non-indexed takes the fetch path. Neither `d3d12_draw_arrays` nor these three uploads is in
  prof3's FIFO profile, so they are small at the Palace (inferred).

### The GPU side, shared by every fetch draw

- The ring: `vf_buf`, `VP_FETCH_RING_MB` per parity half (default 64, read once), upload heap (2117-2129). State:
  `s_vf_cap`, `s_vf_off`, the last descriptor `s_fx` (606-613).
- Root signature: parameter 4 = root SRV t20 (the ring), parameter 5 = one root constant b2 (`vf_desc`) (2043-2051).
- VS: `vp_get_vs_fetch` (3242) -> `vp_get_vs_mode(st, 1)`; the fetch form is `rsx_vp_decompile_fetch`
  (`rsx_vp_decompiler.c:282-285`). Its prologue (`:569-610`) decodes the 32-byte records: `x` (offset of vertex 0's
  element), stride, `type | size << 4 | divider << 8`, frequency, then the constant register as a float4. Types 1-5
  and 7; S1 is (2x + 1) / 65535. `main` fills `v[16]` from `vf_fetch(k, SV_VertexID)` (`:676-680`). The fetch form is
  precompiled with the plain one (13f5fb3).
- PSO: a fetch VS gets no input layout (4333-4334). VP PSOs use `PrimitiveTopologyType TRIANGLE` and
  `IBStripCutValue 0xFFFFFFFF` (4361-4363).
- VP pass: binds the ring for this parity (9151-9154, again at 9204-9207 after a root signature change), sets b2 per
  fetch draw (9546-9548), switches topology when `fetch == 2` changes (9550-9554), then
  `DrawIndexedInstanced(index_count, 1, ib_first, vb_byte_offset / 256, 0)` (9556-9558). A fetch draw has
  `vb_byte_offset = 0`, so `SV_VertexID` is the guest index.
- End of frame (9696-9721): `vpj_sync`, the parity flip, `s_vf_off = 0`, `s_fx.valid = 0`, the `VP_VSTAT` line, and
  growth of `vp_vb` / `vp_ib` from what the frame asked for (`s_vp_want`, `s_vp_iwant`).

## 2. Where the time goes

From prof3 (docs/56 section 4 and docs/66; `exe/crf`, the deep Palace save, single-counted shares of the FIFO
thread; 1% of the thread is about 0.085 ms a frame):

| Function | Exclusive | Inclusive | Primitive |
|---|---|---|---|
| `upload_strip_vp_indexed` | 6.3% | 7.0% | strips and fans that fell back |
| `vpj_sync` | 1.3% | 6.6% | the same strips: the FIFO thread converting queued vertices at sync points |
| `rsx_fetch_vertex_arrays` | 5.0% | | the per-vertex convert, called from all CPU paths |
| `upload_tris_vp_indexed` | 0.6% | 2.4% | indexed lists |
| `rsx_fetch_plan_init` | 1.1-1.5% | | every VP draw, twice for a fallback strip |
| `upload_quads_vp_indexed` | under 0.1%, or inlined | | indexed quads |
| memcpy of spans into `vf_buf` | | 9.4% (under `d3d12_draw_indexed`) | the fetch path's own cost |

- docs/66 sums the CPU vertex path at about 16% of the thread, roughly 1.3 ms a frame: 7.0 + 6.6 + 2.4.
  `upload_tris_vp_indexed`'s 2.4% is the lists' share: about 0.2 ms. The rest, about 1.15 ms, is strips that
  fell back.
- The `vpj_worker` spends another ~23% of a core converting (prof2 tslack; docs/56 section 4). That is off the FIFO
  thread, but it competes for cores.
- The per-frame counts: 2,254 records and 6,416 draw calls (slope2's `TMP_SLOW_DRAW_NS` line). The deep1 PGO profile
  puts `rsx_fetch_vertex_arrays`'s hottest block at 9.6e9 counts (docs/56 section 6).
- `rsx_fetch_attrib` (the CMP path) is not in the list (docs/66), so CMP arrays are rare at the Palace, and the strip
  fallbacks come from the other tests (inferred, as docs/66 says).

**What no log shows yet.** `VP_VSTAT=<N>` (9703-9715) prints uploads by quad, tri, strip and fan, but it counts a
fetch strip (12452) and a CPU strip (12466) in the same bucket, and it counts ranges, not indices. `VP_FETCH_STATS`
(12109-12117) counts only the strip path's ranges, with three pairs of reasons merged. docs/66 found no log with
either. So the split between "sparse", "type/addr" and "room", and the list and quad volume, are unknown.

Master has moved since prof3: CR fields (8fbe2d4), the FIFO cuts (40ced98, bc48c67) and #71 (0a54361: each draw's
pipeline in its record, texture checksums on worker threads). The vertex path is therefore a larger share of what is
left on the FIFO thread than in prof3 (inferred). In the stack those changes made the FIFO thread the one that holds
the frame (docs/56 stackk1: all FIFO cuts off cost 6.7%). Master is that stack without the bi-switch and with #71
added, so a FIFO cut should show in frames on master (inferred; no A/B on 4a76fc0 yet).

## 3. The edits, in order

Each step is its own commit. Tooling commits stay separate from the change.

### Step 0. Stats that size the rest (tooling; ~40 lines; 1-2 h plus one run)

1. Split `s_vf_stat[6]` (11782) into one counter per test, per primitive, counted in ranges and in indices:
   ```c
   /* VP_FETCH_STATS (live): what each indexed range did, per primitive
    * (0 quad, 1 tri, 2 strip, 3 fan), in ranges and in indices. */
   enum { VF_NEW, VF_REUSE, VF_APPEND, VF_A0, VF_ROOM, VF_TYPE, VF_SPARSE, VF_BIG,
          VF_ADDR, VF_RING, VF_OFF, VF_WOULD, VF_N };
   static u64 s_vf_st[4][VF_N], s_vf_sti[4][VF_N];
   static int s_vf_prim;   /* set by d3d12_draw_indexed before each call */
   #define VF_STAT(r, n) (s_vf_st[s_vf_prim][r]++, s_vf_sti[s_vf_prim][r] += (n))
   ```
   Each `return -1` in `upload_strip_vp_fetch` (11966, 11967, 11972, 12010, 12019, 12021, 12070) gets its own reason.
2. In `d3d12_draw_indexed`, at the CPU dispatch (12468-12469), count QUADS and TRIANGLES as `VF_OFF` with their
   `count`. For them, also run a dry check: `guest_index_open`, lo/hi over the range, and the 12010 rule; count
   `VF_WOULD` when the range would qualify. This prices steps 2-4 before they exist. It costs a pass over the indices,
   so it runs only under the knob.
3. In `upload_strip_vp_indexed`, add the unique vertices it wrote (`nmiss`, 11716) to a per-frame counter: the size of
   the step 7 gather.
4. Print once per `VP_FETCH_STATS` frames at the end of the frame, next to `VP_VSTAT` (9703-9715), as per-frame
   means, then clear.

Run once at the deep Palace save (docs/56's spot) with `VP_FETCH_STATS=600 VP_VSTAT=600`, standing and walking.

Decision table after the run:
- Lists and quads under ~1,000 indices a frame each: skip steps 2-4, go to step 7.
- `VF_SPARSE` dominates the strip fallbacks: step 7 (gather) is the big win.
- `VF_ROOM` or `VF_RING` dominates: a capacity fix (grow the index buffer before the first frame, or a larger ring),
  a few lines.
- `VF_ADDR` dominates: split a span at IO page breaks instead of refusing (`vf_resolve`, 11793-11803).
- `VF_TYPE` dominates: a CMP decode in the fetch shader (the CPU one is `rsx_vertex_fetch.c:104-118`); drop the CMP
  test at 11972 and in `vpj_usable` (10949).

### Step 1. Split the descriptor half out of `upload_strip_vp_fetch` (no behaviour change; ~60 lines moved)

- Move 12006-12102 (spans, reuse/append, new descriptor) into
  `static int vf_desc_for_range(const rsx_state* st, const rsx_fetch_plan* pl, u32 lo, u32 hi, u32 count, const D3D12DrawRecord* ext)`,
  which sets `s_up_fdesc` and returns 0, or returns -1 with its stat reason.
- Make the function-static plan `pl` (11968) file-scope (`s_vf_pl`) so the list walk can share it.
- `upload_strip_vp_fetch` calls it when `ni > 0`. Same tests in the same order, same counters.
- Check: `VP_FETCH_STATS` counts and regress shots unchanged.

### Step 2. One carry for list ranges, shared by both paths (~25 lines)

- Move `upload_tris_vp_indexed`'s function-static `carry` (11418) to file scope as `s_tri_carry` (beside
  `s_strip_carry`, 11319). The restart branch (11405-11434) then reads and writes `s_tri_carry`: no change in what it
  draws.
- While `VP_FETCH_LISTS & 1` is set, the no-restart branch (11402-11404) uses the same walk as the restart branch
  minus the restart test: the carried 1-2 indices first, whole triangles only, the remainder into `s_tri_carry`. With
  the knob clear it stays the straight loop it is today.
- Why: once fetch and CPU ranges mix inside one BEGIN/END (a later range of a particle batch hits the sparse rule or
  a full ring), a fetch record and a CPU record cannot merge, so the record merge can no longer keep the stream in
  phase. The carry can. When the merge does succeed, the carried version draws the same triangles from the same
  vertices at the same places in `vp_vb`: the 1-2 leftover vertices of range k move from the end of range k's output
  to the start of range k+1's, which is the same address. The one difference is at the batch's last range: its 1-2
  trailing indices, which `DrawInstanced` never drew, are no longer written, so a fingerprint's `n=` can be 1-2
  lower there.

### Step 3. The list and quad walk on the fetch path (~110 lines)

A new `upload_list_vp_fetch`, next to `upload_strip_vp_fetch`, built from the same pieces:

```c
/* Indexed TRIANGLES and QUADS on the GPU fetch path: the guest's indices,
 * byte-swapped into the index buffer as a triangle list, and the vertex bytes
 * they reach copied into vf_buf (vf_desc_for_range). Returns the indices
 * written, or -1 for the CPU path. The triangles are the CPU path's: */
/*  - TRIANGLES: in order; with restart, the restart index drops the partial
 *    triangle; a partial triangle carries into the next contiguous range of
 *    the same BEGIN/END (s_tri_carry, upload_tris_vp_indexed's rule).
 *  - QUADS: count / 4 quads, each 0,1,2 / 0,2,3 of its four indices
 *    (upload_quads_vp_indexed); no restart test, no carry. */
static int upload_list_vp_fetch(const rsx_state* st, u32 first, u32 count, int quads,
                                const D3D12DrawRecord* ext)
{
    extern uint8_t* vm_base;
    if (!st || !vm_base || !s_d3d.vp_ib_mapped || !st->vertex_attribs[0].enabled)
        { VF_STAT(VF_A0, count); return -1; }
    const u64 need = quads ? (u64)(count / 4) * 6 : (u64)count + 2;
    if ((u64)s_vp_icap - s_d3d.vp_ib_offset < need) {
        s_vp_iwant += need;   /* the CPU list path never asks for index room: ask here */
        VF_STAT(VF_ROOM, count); return -1;
    }
    rsx_fetch_plan_init(st, &s_vf_pl);
    for (u32 k = 0; k < s_vf_pl.n_on; k++) {
        const u32 t = s_vf_pl.on[k].type;
        if (t < 1 || t > 7 || t == 6) { VF_STAT(VF_TYPE, count); return -1; }
    }
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * s_vp_icap + s_d3d.vp_ib_offset;
    const GuestIdx gi = guest_index_open(st);
    u32 ni = 0, lo = 0xFFFFFFFFu, hi = 0;
    u32 n = 0, tri[3] = {0, 0, 0};          /* the list's carry, committed on success */
    if (quads) {
        const u32 nq = count / 4;
        for (u32 q = 0; q < nq; q++) {
            u32 c[4];
            for (u32 k = 0; k < 4; k++) {
                c[k] = guest_index_at(&gi, first + q * 4 + k);
                lo = c[k] < lo ? c[k] : lo; hi = c[k] > hi ? c[k] : hi;
            }
            ib[ni] = c[0]; ib[ni + 1] = c[1]; ib[ni + 2] = c[2];
            ib[ni + 3] = c[0]; ib[ni + 4] = c[2]; ib[ni + 5] = c[3]; ni += 6;
        }
    } else {
        const int restart = st->restart_index_enable != 0;
        const u32 ridx = st->restart_index;
        if (s_tri_carry.valid && s_tri_carry.epoch == st->begin_epoch && s_tri_carry.next == first)
            { n = s_tri_carry.n; tri[0] = s_tri_carry.tri[0]; tri[1] = s_tri_carry.tri[1]; }
        for (u32 k = 0; k < count; k++) {
            const u32 ix = guest_index_at(&gi, first + k);
            if (restart && ix == ridx) { n = 0; continue; }
            tri[n++] = ix;
            if (n == 3) {
                for (int j = 0; j < 3; j++) {
                    ib[ni + j] = tri[j];
                    lo = tri[j] < lo ? tri[j] : lo; hi = tri[j] > hi ? tri[j] : hi;
                }
                ni += 3; n = 0;
            }
        }
    }
    if (ni && vf_desc_for_range(st, &s_vf_pl, lo, hi, count, ext) < 0) return -1;
    if (!quads) {
        s_tri_carry.valid = 1; s_tri_carry.epoch = st->begin_epoch; s_tri_carry.next = first + count;
        s_tri_carry.n = n; s_tri_carry.tri[0] = tri[0]; s_tri_carry.tri[1] = tri[1];
    }
    VF_STAT(ni ? VF_NEW : VF_OFF, count);   /* or the reuse/append reason from vf_desc_for_range */
    s_vp_iwant += ni;
    s_up_icount = ni; s_up_ifirst = s_d3d.vp_ib_offset;
    s_d3d.vp_ib_offset += ni;
    return (int)ni;
}
```

Notes on the sketch:
- The carry is committed only when the range is accepted, as `upload_strip_vp_fetch` does (12104). A range that
  returns -1 leaves `s_tri_carry` as it found it, and the CPU path (step 2) starts from the same carry.
- A range that fails the room test still adds what it needs to `s_vp_iwant`, so `vp_ib` grows at the end of the frame
  (9720). The strip path does not need this, because its CPU fallback asks for index room itself (11728); the CPU
  list and quad paths never touch `vp_ib`, so without it a frame whose strips fit would leave its lists on the CPU
  path for good.
- The index buffer is an upload heap (write-combined), written in order and never read back (the lesson at
  11888-11890).
- `guest_index_at` (11330-11338) resolves the array base once per range, not per index as `read_guest_index` does.
- A second commit can vectorize the no-restart 16-bit list case like `vf_strip_native_w`'s SSE2 block (11894-11931),
  minus the restart compare. Only if step 0 shows long lists.
- The lo/hi rule is 12010's (`hi - lo + 1 > count * 4 + 256` goes to the CPU path), so the first version takes the
  same decisions as the dry check of step 0. For lists the CPU path writes 256 bytes per index while the fetch path
  copies `(hi - lo + 1) * stride` per array, so a looser rule is likely right. Tune it from the stats, as a separate
  commit.

### Step 4. Wire it into `d3d12_draw_indexed` (~30 lines)

1. Knob and selector at the top (after 12418):
   ```c
   static int fl = -1;
   PS3_KNOB(fl, ps3_env_int("VP_FETCH_LISTS", 0));   /* 1 TRIANGLES, 2 QUADS, 4 step 7 */
   const int lq = (primitive == RSX_PRIMITIVE_TRIANGLES && (fl & 1)) ||
                  (primitive == RSX_PRIMITIVE_QUADS && (fl & 2));
   ```
2. Merge anchor (12425-12438): also accept `lq && s_d3d.merge_list == (int)primitive`, with `pv->fetch == 1` required
   (a CPU list record has `index_count == 0` and keeps using `merge_prev_draw`). Add `int merge_list;` beside
   `merge_strip` (538).
3. Fetch gate (12444-12462): when `lq && vf_on()`, call `upload_list_vp_fetch(state, first, count, primitive == RSX_PRIMITIVE_QUADS, mpv && mpv->fetch == 1 ? mpv : NULL)`.
   On `ni >= 0`: `fetch = 1`, `s_vs_up[primitive == QUADS ? 0 : 1]++`, then the same extend-or-new logic as 12453-12460.
   Set `fetch = 1` directly. Do not reuse 12451's `s_up_strip ? 2 : 1`: `s_up_strip` is whatever the last strip range
   left (11988), and a stale 2 would draw the list as a strip.
4. The CPU list batch merge (12489-12507) is unchanged. It already refuses to merge into a fetch record
   (`pv->index_count == 0`, 12501).
5. Anchors (12571-12573): `merge_list = (fetch == 1 && lq) ? (int)primitive : 0`, and
   `merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES && !fetch)`. Clear `merge_list` wherever `merge_strip` is
   cleared (`d3d12_draw_arrays`, 12354 and 12360).
6. Split `VP_VSTAT`'s counters into fetch and CPU (12452 and 12466), so the frame line shows both.

Nothing else changes: the record's `fetch`/`fdesc` (12553), the VS choice (12551-12552), the VP pass and the PSO are
already generic.

**Caching: nothing new.** What is reused today stays as it is: the descriptor within one BEGIN/END (`s_fx`, reuse or
append, 12025-12053), the fetch-form VS per program (`VPVSEntry.fetch`, 368; precompiled, 13f5fb3), and PSOs by key
(the VS slot is part of the key, 4074). Vertex spans are not kept across frames: that needs a guest-change check, and
skinning and particles rewrite vertex data in part (docs/66 Q4 item 5).

### Step 5. Check mode `VP_FETCH_CHECK` (~80 lines; modelled on `RSX_PSO_IN_REC=2`, 0a54361)

For every list or quad range that took the fetch path, while `VP_FETCH_CHECK=1` (live):
1. **Triangles.** Run the CPU path's index assembly for the same range from the same starting carry, without
   converting anything (the loop of 11423-11431, or 11355-11360 for quads), and compare its triples with the ones
   just written, in order. Read the fetch side from a CPU-side shadow array filled with the same stores, not from
   `vp_ib` (write-combined).
2. **Vertices.** For the first, the last and two other indices of the range, decode each enabled attribute from the
   ring's copy with a C mirror of the shader's `vf_fetch` (`rsx_vp_decompiler.c:592-610`), and compare with
   `rsx_fetch_vertex` (`rsx_vertex_fetch.c:158-162`) on guest memory. Both are CPU code, so the floats must match
   exactly. This checks the descriptor: `x`, stride, the divider and the span offsets, where a bug would put a vertex
   in the wrong place.
3. Print a `[VFCHK]` line every 600 frames (ranges checked, triangle mismatches, vertex mismatches), plus the first
   20 mismatches in detail (primitive, first, count, restart, carry, the two triples).

What it does not check is the GPU's own decode. That is the shader the strips already use in every frame.

### Step 6. Fetch draws in the per-draw fingerprints (tooling; ~30 lines)

- `drawprint_write` (7686) hashes `vb` from every 4th of the record's `vertex_count` vertices in `vp_vb`, from its
  offset (7703-7710). A fetch record has `vb_byte_offset = 0` and `vertex_count` = its index count (12460,
  12511-12513), so its `vb` hashes whatever CPU draws wrote at the start of `vp_vb`, not its own input. This already
  applies to every fetch strip on master.
- For `dr->fetch`, hash the index list (`vp_ib`, this parity, `ib_first`, `index_count`), and per active attribute
  record its format word, stride and frequency plus the copied bytes from its `lo` to `hi` vertex, with offsets taken
  relative to `x` so the ring's placement does not enter. Print it as `vb=` so `capdiff.py`'s `HASHES` pattern
  (`tools/des/capdiff.py:33`) compares it unchanged.
- Then `capdiff.py` checks live against replay for fetch draws too. It cannot compare the fetch and CPU paths with
  each other: their `vb` hashes differ by construction. That comparison is the image check below.

### Step 7. Strips (and lists) that fall back on the sparse rule: gather (~150-200 lines; only if step 0 says so)

- Today a sparse range goes to `upload_strip_vp_indexed`, which already walks the strip, dedupes vertices (256-entry
  direct-mapped, 11711-11717) and writes u32 indices into a compact vertex numbering (`s_vx_out`). Only the vertex
  write is expensive: a 256-byte convert per unique vertex, through the worker queue and `vpj_sync`.
- Gather mode keeps that walk and that numbering, and replaces the vertex write: for each new unique vertex, copy each
  enabled array's element (`lv_elem_bytes(type, n)` bytes, 4-byte aligned) from guest memory into a packed block in
  `vf_buf`. Arrays with a frequency divider are gathered per vertex too (their element index comes from the guest
  index), so the descriptor marks every gathered array as divider 0, stride = the packed stride, `x` = the block base
  + the array's offset inside the packed vertex.
- The draw is a fetch draw: `fetch = 1` (the CPU walk already writes a triangle list), `fdesc` = the gather
  descriptor, `vb_byte_offset = 0`, indices = the compact numbers.
- Ranges of one BEGIN/END extend the block while nothing else was written to the ring after it (the append rule,
  12033: `s_fx.end == s_vf_off`), keeping the dedupe table across ranges as `s_strip_base` does today; otherwise a new
  block and a new record.
- Knob: `VP_FETCH_LISTS & 4`. It takes the sparse ranges of strips, fans and (after step 3) lists.
- What it removes: the 256-byte convert and its write-combined write per unique vertex, the plan built twice, the
  worker queue and the sync spin for those draws. What it adds: a stride-sized copy per unique vertex. A Palace level
  vertex is 32-64 bytes (inferred), against 256.

### Step 8 (optional). Non-indexed quads and lists

`d3d12_draw_arrays` could build indices `first .. first + count - 1` (quads: 0,1,2 / 0,2,3 of each four) and take the
same path. Their span is always dense. Only if step 0's counters (or a `VP_VSTAT` split of the non-indexed path) show
volume at the Palace. docs/28 line 201 lists about 200 six-vertex UI quads (likely text) in one early level frame,
which is little.

### Step 9 (small, independent). The fallback strip's second plan

A strip that falls back builds the fetch plan twice: at 11969 and in `vp_plan_begin` (10769-10772). Pass the first
one through. `rsx_fetch_plan_init` is 1.1-1.5% of the thread in all; this saves the fallbacks' share of it.

## 4. What must stay identical

- **The triangles.** The same index triples, in the same order, with the same vertex order inside each triple. The
  winding, and so the cull result, follow from that. Step 5's triangle check compares exactly this.
- **Quads:** `count / 4` quads, each split 0,1,2 / 0,2,3 (as 11359 and 11197), the leftover dropped. The first
  index of both triangles is the quad's first index. The UV-clamp geometry shader (3931-3985) takes each triangle's
  bounds from all three vertices, and with this split each triangle holds two opposite corners, so both get the
  quad's bounds (the comment at 3915-3916 relies on it).
- **Restart:** for lists, a restart index drops the partial triangle (11425). Quads do not test it, today or after.
  A restart index inside a quad would be read as a vertex index: on the CPU path a vertex far past the array. On the
  fetch path it usually makes a span past the 12010 rule, so the range goes to the CPU path; when it does not (a
  very long range), the fetch path reads the same far vertex. Same picture.
- **Phase across ranges:** a partial triangle carries into the next contiguous range of the same BEGIN/END, with or
  without restart, whichever path each range takes (step 2). With the knob clear, the CPU path is today's code.
- **Provoking vertex:** D3D12 uses a triangle's first vertex. The first index of each triangle is the CPU path's
  first vertex. Today nothing in the VP pipeline reads a provoking vertex (no `SET_SHADE_MODE` support in the
  backend; the only `nointerpolation` values are the UV bounds above, equal on all three vertices), but keep it so a
  later flat-shading change sees the same vertex on both paths.
- **The cut value:** VP PSOs set `IBStripCutValue` to 0xFFFFFFFF (4363), which applies to strip topologies. A list
  draw is `TRIANGLELIST` (`fetch = 1`), and the list walk never writes 0xFFFFFFFF except as a real 32-bit guest
  index, which fails the span rule first (inferred: D3D12 ignores the cut value for list topologies).
- **The vertex decode:** the same fetch VS as the strips, including the S1 rule.
- **When guest memory is read:** the fetch path copies the vertex bytes when the draw is recorded (12046, 12075), the
  same moment the CPU list and quad paths convert them (no worker queue is involved for lists and quads today).
- **`RSX_LATE_CHECK`:** `vf_on()` is false under it (11788), so lists and quads stay on the CPU path where the late
  check records vertex reads (`lv_open`, 10727-10737).
- **Draw records:** the same number or fewer (more merges), each with the same FP, constants and state keys.

Not identical, on purpose and only in already-broken cases: with the knob set, a list batch whose record merge would
have failed now stays in phase (step 2); and a quad range that would have been cut short by a full `vp_vb` (11351) is
drawn whole, because the fetch path is bounded by the index buffer instead.

## 5. Live knobs

| Knob | Meaning |
|---|---|
| `VP_FETCH_LISTS` (live, `PS3_KNOB`) | bit 1: indexed TRIANGLES on the fetch path; bit 2: indexed QUADS; bit 4: the step 7 gather for sparse strips, fans and lists. 0 = today. Default 0 until the A/B; then 1, 3 or 7 by what pays. |
| `VP_GPU_FETCH=0` (existing) | every fetch path off, lists and quads included (`vf_on`, 11786-11788). |
| `VP_FETCH_CHECK=1` (new, live) | step 5's comparisons. |
| `VP_FETCH_STATS=<frames>` (changed, live) | step 0's per-primitive, per-reason counters. |

A switch between two ranges of one BEGIN/END is safe: the carry is shared (step 2), and a fetch record and a CPU
record never merge.

## 6. How to check it

1. **Check mode**, live, at the deep Palace save and in the Tutorial: `VP_FETCH_LISTS=3 VP_FETCH_CHECK=1`, standing,
   walking, a fight with sparks (docs/28 line 323: the particle batches come as ranges (0,32) (32,256) (288,12), with
   restart), fog walls and the aura. Expect 0 mismatches.
2. **Images from one capture.** Take an F9 capture at the Palace spot. Replay it with `rsx_replay <cap> 2` twice,
   once with `VP_FETCH_LISTS=0` and once with `3`, each with `CELLMARK_DUMP_DIR` set (it writes the presented frame
   as a BMP; `libs/video/tests/rsx_replay_main.c:23`), and compare the two with `tools/des/shotdiff.py`. Expect them
   identical: the strip change found its on/off shots identical (f7337e9).
3. **Live against replay** with `capdiff.py` at `VP_FETCH_LISTS=3`, after step 6.
4. **regress.sh** at 30 and 240 fps (with the frame-sort fix, port PR #72 or `sort -V`).
5. **Stats again** (`VP_FETCH_STATS`): ranges by reason, and the ring's use per frame against its 64 MB half.

## 7. Risks

1. **The gain may not show.** Lists and quads are ~0.2 ms by prof3. On a build where HighGraphics holds the frame they
   save CPU only (fifok1, docs/56). The real prize is the fallback strips (step 7), which depend on the stats.
2. **Phase across ranges** (particles, docs/28 line 323; the fluid's ~3,000 batches of 256, 12489-12491): a carry
   bug draws slivers. Step 5's triangle check covers every range.
3. **New PSOs on first use.** `vs_idx` is part of the PSO key (`vp_get_fp_pso`, 4074), and a fetch draw uses the
   fetch-form VS, so every list pipeline is a new PSO the first time the knob is set. A live A/B must flip the knob
   once and wait before its windows, or the first B window carries the compiles (the fetch-form VS itself is
   precompiled, 13f5fb3).
4. **Capacity.** More fetch draws use more of the ring (the strips took 2-3 MB a frame, f7337e9) and of `vp_ib`.
   `vp_ib` grows at the end of a frame (9720); the first frames after the knob is set will count `VF_ROOM`.
5. **GPU time.** The fetch VS does the decode the CPU did, with byte loads for unaligned elements. Lists become
   indexed draws, so the post-transform cache reuses shared vertices that the CPU path's unindexed lists shaded once
   per index (inferred). Watch render_frame's gpu-wait phase under `PERF=1`.
6. **Step 7 is the largest new code** (dedupe across ranges, a packed layout, dividers). Its check is step 5's vertex
   check, extended to the gather descriptor.
7. **Debug knobs go blind for fetch lists.** `IDXDBG`, the `DBG_LOCK` tracker, `RSX_DBG_SCALE`, `RSX_DBG_VTX` and
   `IDX_DBG` read the converted vertices inside `upload_tris_vp_indexed` (11437-11627), as `dbg_vtx_dump` does. They
   see nothing of a list on the fetch path, as they see nothing of a fetch strip today. Either make `vf_on()` false
   for lists while one of them is set, or document `VP_FETCH_LISTS=0` beside them.
8. **Merge conflicts with the render-thread change** (section 9).

## 8. Expected gain

The FIFO thread at the deep Palace save: about 92-93% busy, 1% = ~0.085 ms a frame (prof3). The bars: cross-build
2.8% profile fps and 4.5% gap fps (aa2); in-boot knob about 1-1.5% (`PERF_NULL` line, docs/56 section 1).

| Part | FIFO time saved a frame | Frame rate where the FIFO thread holds the frame | Against the bars |
|---|---|---|---|
| Lists (steps 2-4) | about 0.15 ms (inferred: 70-80% of `upload_tris_vp_indexed`'s 2.4%, which is ~0.2 ms) | about +2% | knob bar likely; cross-build bar no |
| Quads (steps 3-4) | 0-0.1 ms (unknown until step 0) | 0 to +1% | inside the noise alone |
| Gather for sparse strips (step 7) | 0.6-0.85 ms (inferred: most of 7.0% + 6.6%, minus the gather's copies), if "sparse" is the main reason | capped by the next thread: +5-8% | both bars |
| Plan once (step 9) | under 0.05 ms | | noise |

- On a build where HighGraphics holds the frame (the base of docs/56), all of it is CPU time (FIFO busy -2 to -14
  points), as fifok1 found for the FIFO cuts.
- On master 4a76fc0 (CR fields + FIFO cuts + #71), the FIFO thread most likely holds the frame (stackk1: the cuts were
  worth 6.7% there). The cap is the next thread: in the stack, HighGraphics waited in the kernel 27% of the window and
  image 2 was 89% busy (stackab1), so 0.5-0.8 ms is the likely ceiling before image 2 sets the frame (inferred).
- Measure each part with `perfknob.sh` and its knob bit (in-boot, ABBAAB), with a warm-up flip first (risk 3), and
  `knobcpu.py` for the FIFO thread's busy share. Only the gather is likely to clear a cross-build `perfab.sh` batch.

## 9. Overlap with the render-thread change

The render-thread map had not been written when this was written, so this section uses docs/66 Q1 ("Stage 1": the
VP pass, submit and Present on a render thread, `RSX_RF_THREAD`). Where the two meet:

- **The VP pass** (9124-9560): Stage 1 moves it to the render thread. It reads `dr->fetch`, `dr->fdesc`,
  `index_count` and `ib_first`, binds the ring for the parity (9151-9154, 9204-9207) and switches topology
  (9550-9554). This change adds nothing there: a list is `fetch = 1`, already handled. No conflict in code, but the
  render thread must see the records the FIFO thread finished, which Stage 1's two draw lists provide.
- **The parity halves** of `vf_buf` and `vp_ib` (flipped at 9700): this change writes more into them at draw time,
  and Stage 1 reads a half later, on the render thread. The rule is the one strips need today: a half is free once
  the frame two back has finished on the GPU, which `move_to_next_frame` (2228, called at the end of `render_frame`,
  10167) waits for with `FRAME_COUNT` 2 (134). With a render thread, that wait must still come before the FIFO
  thread's first write into a half (inferred; a Stage 1 design point, not this change's).
- **End-of-frame resets** (9696-9721: `s_vf_off`, `s_fx.valid`, the parity flip, `vp_ib_offset`, the growth calls,
  the `VP_VSTAT` line). Stage 1 keeps them on the FIFO thread after the join; step 0 and step 4 add a stats print and
  counter resets in the same block. Expect a textual merge conflict there, nothing more.
- **`drawprint_write`** (7686) reads the current parity's `vp_vb` and, after step 6, `vp_ib` and `vf_buf`. With a render
  thread it must run before the hand-off or on the render thread, consistently.
- **The carry statics** (`s_strip_carry`, `s_tri_carry`) and `s_fx` live on the FIFO thread only; Stage 1 does not
  touch them.
- The two savings add up: Stage 1 moves 1.0-1.1 ms of render_frame (docs/66), this change 0.2-0.85 ms of draw-time
  work. Together they leave HighGraphics or image 2 to set the frame (inferred).

## 10. Open

- Which fallback test sends strips to the CPU path, and how many list and quad indices a Palace frame has: step 0.
- `upload_quads_vp_indexed`'s real cost: not in prof3's sampled list; a profile with `VP_VSTAT` split by path, or
  `__declspec(noinline)` on it in a scratch build, would show it.
- The looser sparse rule for lists (step 3 notes): from the stats.
- The GPU time of the fetch VS on lists: render_frame's gpu-wait phase under `PERF=1`, before and after.
