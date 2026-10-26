# FIFO thread: the next cuts (static analysis)

Written on 2026-10-19 (afternoon) by a static-analysis agent for docs/56 section 6 (the FIFO study). The worktree it read was in that session's scratch dir; it was ps3recomp master of that afternoon (b249b7a). Rows 1 and 3 became #71 (merged), row 2 is the render thread (docs/68), row 4 is docs/64.

Code: a ps3recomp worktree at b249b7a (session scratch, gone). Line numbers are in
`libs/video/rsx_d3d12_backend.c` unless a file is named.

Profile: `lanes/perf/runs/prof3` (exe/crf, the deep Palace save). The FIFO
thread (vblank_ticker, tid 3728) gave 3,715 samples over 6 s. tslack puts it
at 93% busy, with 5.48 cores busy in the whole process. Phase times come from
the frames files (about 2,900 Palace frames). A frame is 8.3-8.6 ms, so 1% of
the thread is about 0.085 ms a frame. prof2 (exe/base) is about 10% higher in
every phase.

Two items in that profile are already fixed in the worktree:
- The c[0] read-back from the write-combined bank (vp_record_cb+0x1639,
  3.1%). RSX_VCB_C00 (5732-5734, 40ced98) reads c00 from the state instead.
- vp_bind_sampler_table (4.0% inclusive), fixed by bc48c67.

## Where the thread goes (prof3)

| Function | Exclusive | Inclusive | Note |
|---|---|---|---|
| d3d12_draw_indexed | 8.4% | 38.4% | plus 9.4% in memcpy it calls (vertex spans into vf_buf) |
| render_frame | 9.9% | 30.0% | 5.8% of it is the inlined tex_csum loop |
| gcm_rsx_process_fifo_unlocked + rsx_process_method(+_run) | 13.0% | | the FIFO parse itself, spread over 90+ addresses |
| upload_strip_vp_indexed | 6.3% | 7.0% | CPU vertex path, strips |
| vpj_sync | 1.3% | 6.6% | the FIFO thread running vertex jobs itself |
| vp_record_cb | 4.0% | 6.2% | 3.1% was the c[0] read-back (fixed) |
| rt_writeback_store | 5.7% | 5.8% | byte-swap row copy into guest memory |
| rsx_fetch_vertex_arrays | 5.0% | | the per-vertex convert |
| vp_get_vs_mode | 3.9% | | byte-wise FNV of the vertex program |
| upload_tris_vp_indexed | 0.6% | 2.4% | CPU vertex path, lists |
| fp_hash_at / vp_get_fp_pso / dr_cube_mask | | 2.0% / 1.9% / 0.9% | per-draw lookups done twice |

render_frame phases (RF, prof3 means): gpu-wait 0.01 ms, store 0.45-0.49,
draw-prepass 1.40-1.48 (tex 0.72-0.77 for about 5,630 lookups; srv including
tex 1.17-1.24; pso 0.12), vp-pass 0.92-0.98, submit+zq 0.09-0.10, post 0.01,
present 0.01. In total 2.95-3.14 ms.

## Q1. render_frame on its own thread

### Guest memory it reads
- Texture bytes:
  - tex_csum (4453-4468), once per unique texture per frame in
    vp_upload_tex_slot (4820-4850). The `used` short-cut is at 4823 and the
    checksum at 4831. A changed texture is converted from guest memory.
  - tex_guest_written (4485-4493) at 8720.
  - The subvp empty scan at 8743.
  - late_tex_hash (5419-5427) when late_check is on.
- Fragment program ucode at its guest address: fp_units_at (8665), and
  vp_get_fp_pso in the prepass (8808) and again in the VP pass (9260). The
  fragment constants themselves are captured at draw time (vp_record_cb,
  5577-5583), so they are safe.
- Render-target checksums: off_rt_drop_rewritten (6759) runs tex_csum over
  each target's guest range (6768).
- current_rsx_state, live rather than a snapshot: compile_vp (8288,
  2565-2709) and the legacy pass (8857-8962). Demon's Souls draws through
  the per-draw VP records, so the legacy pass is idle there.
- guest_fb_present (7099) reads guest memory only with GCM_GUEST_FB set, so
  it is off for this title.

### Guest memory it writes
- rt_writeback_store (6663-6752). With RSX_WRITE_COLOR_DEFER=1 (the default,
  6383) it runs at the start of the next render_frame (8269). Otherwise it
  runs right after submit (9811-9817).
- zq_finish writes the reports through cellGcm_report_store (9819). Before
  that, wait_for_gpu runs when the frame issued queries (`s_zq_n`, 9818).
  In the Palace frames submit+zq is 0.1 ms, so those frames do not wait
  there.
- Nothing else. Flip status, labels and semaphores are written by
  cellGcmSys.c (1842-1850, 1940-1955) on the FIFO thread.

### State it shares with the FIFO thread
- `s_d3d.draws[]` and `draw_count`. The FIFO thread fills them at draw time
  (12350-12416), and render_frame reads them and resets them (9540).
- The parity heaps: vp_vb, vp_ib, vf_buf, vp_cb and vp_fpcb. Draws write
  into half `vp_parity`. render_frame flips it (9541) and resets `s_vf_off`,
  `s_fx` and `s_vcb_last` (9542-9562).
- Query events: `s_zq_ev`, `s_zq_nev` and `s_zq_on`, written by zq_hook at
  FIFO time (7285-7297).
- `s_composite_src`, set at flip and read and cleared by composite_present
  (7042-7045).
- vpj jobs. vpj_sync runs at 8244 and 9537, and also at FIFO sync points:
  rsx_commands.c:459-460 and cellGcmSys.c:1841 and 1940-1955.
- GPU objects that only render_frame touches: the command list, off_rt[],
  zeta, vp_tex[], the SRV and sampler heaps, and the readback buffers. Every
  command-list user is render_frame or one of its helpers: vp_upload_tex_slot,
  off_rt_get, zeta_get, off_rt_snapshot, subvp_capture, rt_writeback_record,
  composite_present and aa_*. No FIFO-time callback records GPU commands, so
  the list can be handed over cleanly.

### Ordering that already exists
- The flip word runs render_frame synchronously. cellGcmSys.c:1841 does
  GCM_RSX_SYNC (vpj_sync), 1842-1850 sets the flip status, 1851 calls the
  hook (d3d12_fifo_flip, 10222-10242), and 1861-1863 breaks the drain.
- Until render_frame returns, the FIFO thread processes no label, semaphore,
  reference or `get` past the flip. So by the time the guest can observe the
  flip, the frame has been submitted, the previous frame's write-back stored,
  and this frame's queries stored.
- wait_for_gpu at the start of the next frame (8266) is what frees the other
  parity half. Frame N-1 has finished on the GPU before frame N+1's draws
  write into its half.

### Hazards for a hand-off at flip
1. **Draw list reuse.** The FIFO thread starts filling `draws[]` for frame
   N+1 while a render thread reads frame N. This needs two draw lists indexed
   by parity, or a copy.
2. **Parity halves.** They are already double-buffered. The render thread
   must have submitted frame N before the FIFO thread's GPU wait at the next
   flip. Join first, then wait; the existing wait at 8266 then covers it.
3. **Guest-visible writes.** The zq report stores and the write-back store
   land today before the FIFO thread passes the flip. The guest may read a
   report straight after. These need a join before zq_finish, or the frame
   runs inline when `s_zq_n > 0`. The deferred write-back store already runs
   at the next flip, so it stays on the FIFO thread after the join.
4. **Guest reads at flip time** (textures, FP ucode, target checksums). They
   already race the PPU a little. On a lagging render thread the window
   grows. Stage 1 keeps them on the FIFO thread.
5. **Live state reads** (compile_vp, the legacy pass). Stage 1 keeps them on
   the FIFO thread.
6. **`s_composite_src` and the end-of-frame resets** (9537-9562). Snapshot
   the first at the hand-off. Run the resets on the FIFO thread.
7. **Present and the swap chain.** The resize path (window/resize) and
   d3d12_present (10105) must not race a Present on the render thread. Check
   the locking there before coding.

### Staged plan
**Stage 1, the first safe stage.**
- The FIFO thread keeps gpu-wait+store, setup, rt-prepass and draw-prepass.
- The prepass saves each draw's PSO in its record. Both calls (8808 and 9260)
  take the same record fields.
- One render thread, woken by an event (no spinning), runs:
  - the VP pass (8967-9431) and post;
  - rt_writeback_record and ResolveQueryData (9795-9796);
  - Close and Execute (9804-9806);
  - Present (9980-10004) and move_to_next_frame (10008).
- At the next flip, the FIFO thread waits on the render thread's done event,
  then runs wait_for_gpu, the store and zq_finish. Frames with queries run
  inline.
- Moves vp-pass 0.92-0.98 ms, submit 0.09-0.10 ms and post/present 0.02 ms:
  1.0-1.1 ms of 2.95-3.14 ms. That is 34-36% of render_frame and 12-13% of
  the thread.
- The render thread needs a free core. tslack shows room (5.48 cores busy).
- Knob: `RSX_RF_THREAD=0/1`, read at flip. 0 is today's inline path.

**Stage 2.** Move the texture work (checksums, conversion, SRV writes) to
the render thread too. Take a snapshot of the guest texture bytes at the
flip, or run the checksums in a parallel pre-pass (candidate 2). That moves
up to 1.2 ms more. The risk is reading guest textures later than today.

**Stage 3.** The write-back store, 0.45-0.49 ms (docs/56 §3 #9).

## Q2. The CPU vertex path

A draw takes the GPU fetch path only if it is a TRIANGLE_STRIP or
TRIANGLE_FAN (after the merge at 12266-12279) and vf_on() holds (11625-11630:
VP_GPU_FETCH, vf_mapped, s_vf_cap, s_vp_fast and !late_on()). See
12286-12303. Otherwise:

- QUADS go to upload_quads_vp_indexed (12309; 11182-11224).
- TRIANGLES go to upload_tris_vp_indexed (12310; 11226-11471).

A strip or fan still falls back to upload_strip_vp_indexed (12311-12312;
11481-11607) when upload_strip_vp_fetch (11803-11960, inlined) returns -1:
- attribute 0 is off (11807);
- there is less index room than count*3+4 (11808, stat "room");
- an attribute type is outside 1..7, or is 6 = CMP (11811-11814, stat
  "type/addr");
- the range is sparse: hi-lo+1 > count*4+256 (11851, stat "sparse");
- a span is over 16 MB (11860);
- the IO pages are not one linear run (vf_resolve, 11634-11643), or the span
  passes 4 GB (11862);
- the vf ring is full (11911).

### CPU work per draw on those paths
- A 256-byte vertex (16 float4) per vertex, built by rsx_fetch_vertex_arrays
  (rsx_vertex_fetch.c:164-200) and written to write-combined memory.
  Main-memory attributes pay a cellGcmResolveLocated per vertex per
  attribute (rsx_vertex_fetch.c:171-172).
- rsx_fetch_plan_init per draw (rsx_vertex_fetch.c:133-156), 1.2-1.5%.
- Lists are unindexed: one 256-byte vertex per index, not per unique vertex.
  Each index also goes through read_guest_index, which resolves it on its own
  (11136-11150). Quads write 6 vertices each.
- CPU strips go to the worker only when vpj_usable() and vpj_room() pass.
  vpj_usable rejects CMP (10798). Otherwise the strip converts inline.
  vpj_sync (10753-10765) runs any unclaimed jobs on the FIFO thread at every
  sync point and spins until they finish.

The total on the FIFO thread is about 16%, roughly 1.3 ms a frame
(upload_strip 7.0% + vpj_sync 6.6% + tris 2.4%, inclusive). The worker
spends another 22.8% of a core. rsx_fetch_attrib is not in the top list, so
CMP draws are rare in the Palace: the strip fallbacks come from the other
tests.

The code does not say which test dominates. `VP_FETCH_STATS=1` prints the
counts behind each reason (11951-11958). `VP_VSTAT=60` prints vertices and
uploads by quad, tri, strip and fan per frame (9544-9556). I found no log
with either. One run with both answers the split.

### Sending lists and quads to the GPU path
- Build u32 indices from the guest's. Lists need no cut index. Quads become
  0,1,2 / 0,2,3, which keeps the CPU path's split and provoking vertex.
- Reuse the span copy and the 512-byte descriptor (11880-11937), and set the
  list topology in the record.
- Keep upload_tris_vp_indexed's restart and carry rules.
- Make the triangles batch merge (12333-12348) fetch-aware.
- Sparse strips: rather than bailing at 11851, gather only the referenced
  vertices by stride and remap the indices. That is a stride-sized copy per
  unique vertex instead of a 256-byte convert.
- CMP: add a decode to the fetch shader (rsx_vertex_fetch.c:104-118 has
  it) and drop the type-6 tests at 11813 and 10798. Low value in the Palace.

What this removes: the 256-byte convert and its write-combined write for
each vertex (and for lists, for each index), the per-vertex resolve, the
plan init, the job queue and the sync spin. The GPU does the decode
instead, so measure GPU time as well.

## Q3. The constant-bank copy

- **Size.** 512 registers × 16 bytes = 8,192 bytes (rsx_commands.h:170 and
  :361). Not 7.5 KB. The slot is VP_CB_STRIDE = 8,448 bytes (158) with
  posscale and posoffset. The copy is at 5649.
- **When it runs.** For every draw (vp_record_cb from 12409), unless
  VP_CB_REUSE (5631-5646) hits. A hit needs the same rsx_state pointer, the
  same vertex_constants_gen and the same viewport z as the last bank written
  this frame. Only the previous bank is checked.
- **FP slot.** It is written for every draw: texscale and alpha test
  (5539-5570), and fp_k (5577-5583).
- **Cost now.** memcpy under vp_record_cb is 1.5% (about 0.13 ms). The 3.1%
  hot spot was the c[0] read-back, which is fixed. The comment at 5633-5636
  says the copies were about 50 MB a frame before reuse. The copy is now a
  small cost, not the large one docs/56 expected.
- **Copying only the registers the program reads.** The decompiler knows
  them:
  - emit_src writes `vp_c[N]` for a direct read (rsx_vp_decompiler.c:99) and
    `vp_c[(N + aX.c) & 511u]` for an indexed read (:96). There is one
    constant index per instruction (:369-385).
  - Record a 512-bit read mask per vertex shader at decompile time, and keep
    it in `s_d3d.vp_vs[]` beside `uses_c03` (2576-2577, 5725-5726).
  - Any indexed read sets every bit. The `& 511` wrap can reach any register,
    so skinned programs keep the full copy.
  - Keep c[0..3] in the mask for the projection fallback and RSX_VCB_C00
    (5684-5690, 5725-5742).
- **Correctness risk.**
  - A reused bank must hold every register the new draw reads. Keep the
    written mask with `s_vcb_last` and reuse only when the new mask is a
    subset of it.
  - A register the mask misses reads whatever an older frame left in that
    slot. That gives a wrong vertex, not a crash.
- **Verdict.** It saves at most about 0.13 ms. Do it after the others.

## Q4. Other large cuts in the top list

1. **tex_csum** (4453-4468), inlined in render_frame.
   - render_frame+0x7B87 and +0x7B8D (the FNV loop's load stall) are 5.8% of
     the thread, about 0.49 ms. That is two thirds of the tex phase.
   - It reads about 1,024 sparse words per unique texture per frame (step =
     nbytes / 1024), and each read misses the cache.
   - Cheap cut A: run the frame's checksums in a parallel pre-pass. Compute
     them over the unique texture keys in the draw records, on 2-3 workers
     or the vpj worker. This keeps today's rule (every texture, every frame).
   - Cheap cut B: an age rule. A texture unchanged for K frames is checked
     only every M frames. TEX_VERIFY (4832-4848) catches stale hits.
2. **vp_get_vs_mode** (3231-3405), 3.7-3.9% exclusive, about 0.3 ms.
   - All of it is the byte-at-a-time FNV of the program (vp_hash_ucode,
     3201-3205, inlined; one imul per byte).
   - It runs when the 2-way memo per program start misses (RSX_VP_MEMO=2,
     3249-3290). So the title switches more than two programs per start.
   - Cut: more ways (4-8). They are still exact, because each way memcmps a
     copy of the program. Then a word-wise hash. Check first whether the hash
     keys anything on disk.
3. **Twice-per-draw lookups.**
   - vp_get_fp_pso at 8808 and 9260 has the same arguments; keep the
     prepass result in the record.
   - dr_cube_mask runs at 8655 and 9265.
   - fp_hash_at (3849-3871) runs, then fp_units_at memcmps again
     (3877-3886).
   - Together about 0.15 ms.
4. **rt_writeback_store**, 5.8%. Its hot spot (+0x13A6, 3.4%) is the
   pshufb byte-swap row copy from the readback buffer into guest memory.
   rt_writeback_record (6591-6634) copies every used target every frame.
   Writing back only targets the guest reads is docs/56 #9. The risk is high:
   a missed target gives a live-only wrong image.
5. **memcpy of vertex spans into vf_buf** (11887, 11916), 9.4%, about 0.8 ms.
   This is the GPU fetch path's own cost. Keeping spans across frames needs
   a guest-change check, as textures have. Vertex data rewritten in part
   (skinning, particles) makes a sparse check risky. Not cheap; listed for
   completeness.
6. **read_guest_index per index** in the tris and quads paths (11136-11150).
   guest_index_open/at (11161-11179) resolves the base once. About 10 lines;
   a small share of the 2.4%.
7. **Not cheap.** The FIFO parse (13% over 90+ addresses) has no single hot
   spot. The per-frame getenv calls in render_frame (9569-9585, 9595, 9952)
   are about 0.3%.

## Ranked candidates

The ranking is by saving per day of work.

| # | Candidate | FIFO saving a frame | Cost | Risk | Measure and knob |
|---|---|---|---|---|---|
| 1 | VS memo with more ways, then a word-wise hash (Q4.2) | 0.25-0.3 ms (3-4%) | 20-40 lines, half a day | Low: memcmp keeps it exact | `RSX_VP_MEMO=N` ways (0, 1 and 2 keep today's meaning). Count memo misses under PERF=1. |
| 2 | tex_csum in a parallel pre-pass (A), or the age rule (B) (Q4.1) | 0.3-0.45 ms (4-5%) | 40-80 lines, 0.5-1 day | A low. B medium: a stale animated texture for up to M-1 frames. | `TEX_CSUM_THREADS=0/N`, `TEX_CSUM_EVERY=1/M`; `TEX_VERIFY=1` for stale hits; RF tex phase. |
| 3 | render_frame stage 1: VP pass, submit and present on a render thread (Q1) | 1.0-1.1 ms (12-13%) | about 300 lines, 2-3 days | Medium: races show as flicker or missing draws | `RSX_RF_THREAD=0/1` at flip; RF phases with PERF=1, tslack busy %. |
| 4 | Lists and quads (and sparse strips) on the GPU fetch path (Q2) | 0.5-1.0 ms (up to about 1.3) | 250-350 lines, 3-4 days | Medium: restart and carry rules, index width, provoking vertex | First a `VP_FETCH_STATS=1 VP_VSTAT=60` run. Then `VP_FETCH_LISTS=0/1`; `VP_GPU_FETCH=0` turns off the whole path. |
| 5 | PSO in the record, and the FP and cube memos once per record (Q4.3) | 0.1-0.2 ms | 40-60 lines, half a day | Low: same arguments | `RSX_PSO_IN_REC=0/1`. A debug mode compares both PSOs for one frame. |
| 6 | Write back only the targets the guest reads (Q4.4, docs/56 #9) | up to 0.45 ms | 1-2 days | High: live-only wrong images | The existing RSX_WRITE_COLOR knobs; capdiff on F9 captures. |
| 7 | Constant copy limited to the registers read (Q3) | 0.13 ms or less | 80-120 lines, 1 day | Medium: a bank reused with a smaller mask | `VP_CB_MASK=0/1`. Check reuse against the mask under RSX_DBG_CB. |
| 8 | read_guest_index changed to guest_index_at (Q4.6) | under 0.1 ms | about 10 lines | Low | A/B with the new knob only |

For every row, measure with an in-boot ABBA run (perfknob.sh) at
VBLANK_MHZ=240000 on the Palace save, with RF phases under PERF=1. Use a
profrip run of the FIFO thread for attribution. Rows 1, 2 and 5 together
take about 0.7-0.9 ms off the thread in two days of work. Row 3 takes about
1 ms more, and row 4 about the same again once the stats run has sized it.
