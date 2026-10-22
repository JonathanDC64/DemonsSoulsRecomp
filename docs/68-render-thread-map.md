# render_frame's second half on a render thread: edit plan

This is a design only: nothing was built or run, because a performance A/B was using the PC. It was written on 2026-10-19 so the work can start on 10-20. The design inputs are docs/56:1153-1169 and docs/66:105-145.

**Source.** The stack worktree of that evening (a scratch worktree, gone with the session): ps3recomp master (4a76fc0) plus the memo fix (#72), the bi-switch (#73) and FIFO_PUT_WAKE (later #79). Its `rsx_d3d12_backend.c` is master's plus the memo fix (3 lines at 3288) and a log block 2 lines shorter near 9412, so on master the anchors from 3291 to about 9425 sit 3 lines earlier, and later ones 1 line earlier. References use these short forms:

| Form | File |
|---|---|
| bare number | `libs/video/rsx_d3d12_backend.c` |
| `G:n` | `libs/video/cellGcmSys.c` |
| `C:n` | `libs/video/rsx_commands.c` |
| `H:n` | `libs/video/rsx_d3d12_backend.h` |
| `BM:n` | `runtime/ppu/tests/boot_main.cpp` |
| `UI:n` | `port/ui/src/des_ui.cpp` (main repo) |

**Line numbers can drift.**
- The backend anchors were re-checked at the end of the session.
- cellGcmSys.c moved by about 48 lines during the session, because another lane edits that worktree. If an anchor no longer matches, find it by the quoted text.
- "(guess)" marks anything not confirmed in the code.

## Short answers

| Question | Answer |
|---|---|
| Gain | About 1.0-1.1 ms off the FIFO thread per frame, of the 2.95-3.14 ms that render_frame costs it (docs/56:1110, 1117). Only frames without occlusion queries get the saving (§4.1). Step 0 measures how many those are. |
| Size | All in rsx_d3d12_backend.c: about 200 new lines, 40 edited and 300 moved verbatim (rf_handoff 251, rf_titlebar 48). Diffstat about +550/-350 (guess). About 2 days: 1 for the refactor and its regress runs, 0.5-1 for the thread and its tests, 0.5 for the A/B (guess). Add a day if occlusion-query frames need Stage 1a or 1b. |
| Knob | `RSX_RF_THREAD`, read with PS3_KNOB at every flip in `d3d12_fifo_flip` (10394), so the console can switch it live. 0 is the default and today's path; 1 is threaded; 2 is reserved for Stage 1b. |
| Main risk | Occlusion-query frames run inline, which could cancel the gain (§5.3 #1). The next risk is a missed global read, which would show as flicker or missing draws (#2). |
| Swap-chain lock | **None needed** (§3.2). See below. |

Why the swap chain needs no lock:
- **All swap-chain calls stay together, on the render thread.** They are Present (10159), `ResizeBuffers` (through `swap_resize`, 1313; its only caller is 10169, right after Present), `GetCurrentBackBufferIndex` (1327, 2233) and `GetFrameStatistics` (8008).
- **The join orders them.** Every render_frame entry runs `rf_join` first. That waits on the manual-reset done event, so the next frame cannot touch the swap chain while the previous one is still on it.
- **The window thread never calls DXGI.**
  - For a resize, it only stores the wanted size with `InterlockedExchange(&s_win_req)` (845).
  - Borderless and Alt+Enter only move the window (811-830, 864-868). The `WM_SIZE` that follows reaches `swap_resize` through `s_win_req`.
- **No second Present path can overlap.** Every other path goes through render_frame and so joins first:
  - `d3d12_present`: 10288.
  - `d3d12_clear_display`: 10499.
  - the ticker present: 13090. It also returns early at 13018-13019 once framed presents have started (10396).
- **The overlay hook submits to the free-threaded queue** (H:63-69), and only one thread runs it at a time.
- **Shutdown joins before releasing the swap chain** (12914, 12952).

## 0. Summary

**Where render_frame runs today.**
- render_frame (8376-10221) runs whole on the FIFO thread. That thread is the vblank ticker: it drains at BM:670 and BM:684.
- The path is: flip word, then the G:1899 hook, then d3d12_fifo_flip (10382-10402), then render_frame.
- The drain stops at the flip (G:1909-1911) until render_frame returns.

**Where to cut.** At `RF_CP(5)` (9103).
- The FIFO thread keeps everything before it: gpu-wait+store, setup, rt-prepass, draw-prepass and the legacy pass.
- It then runs a hand-off, which does three things:
  - snapshots the batch;
  - runs the end-of-frame resets and the bookkeeping that reads FIFO-time state (9594-9745, 9832-9895);
  - wakes one render thread.
- The render thread runs:
  - the VP pass (9103-9591);
  - RTT_SAVERT and the post tail (9747-9830, 9896-9952);
  - submit (9954-9979);
  - dumps (9981-10136);
  - Present, `move_to_next_frame`, `swap_resize` and `frame_count++` (10138-10171).
- The next render_frame first waits for the render thread (`rf_join`), before `vpj_sync` (8378). `wait_for_gpu` (8401) and the deferred write-back store (8404) then run as they do today.
- Knob: `RSX_RF_THREAD`, read at the flip. 0, the default, is today's path.

**Expected saving.** 1.0-1.1 ms of the FIFO thread's 2.95-3.14 ms in render_frame (docs/56:1110, 1117).

What this study found, beyond docs/56 and docs/66:

1. **Resize and Present need no lock.** Every swap-chain call is in render_frame's tail, or in init or shutdown. They all move together, and the join orders them (§3.2).
2. **The per-parity draw lists change what `s_d3d.draws` means after the hand-off, inline frames included.** Three readers after the hand-off must take the job's list:
   - the VP pass (9108-9197);
   - `fpwatch_batch` (7629-7630);
   - `rsx_capture_frame` (2500, 2531, 2535).
3. **The render thread must never call `vp_get_fp_pso`.**
   - It reads guest fragment ucode (4037-4069, 4145-4156).
   - It reads `s_d3d.vp_vs` (4043-4044), which `vp_vs_insert` reallocates (3224-3228).
   - The VP pass's fallback lookup (9408-9415) therefore moves into the hand-off, and threaded frames skip it.
4. **Two blocks leave the tail and stay on the FIFO side.**
   - `pso_lib_tick` (10163), because its counters are FIFO-time state.
   - The titlebar and GCM_RATES block (10173-10220). It reads the guest's label 255 (10198-10199) and calls `SetWindowTextA` (10188).
5. **Occlusion-query frames may be rare in the Palace (inference).** docs/66:67-70 gives submit+zq as 0.1 ms in the Palace frames. That suggests those frames issue no queries: a `wait_for_gpu` after Execute would cost the GPU's whole frame. If that holds, the rule "zq frames run inline" keeps the gain. Step 0 checks it without a build.

## 1. render_frame by phase

The phase names are RF-SLOW's (7800-7801). "Thread" is where each phase runs after the split. "Hand-off" means the FIFO thread, inside `rf_handoff`.

| Phase | Lines | Thread | Reads | Writes |
|---|---|---|---|---|
| entry | 8378-8391 | FIFO | `vpj_sync` (8378), `s_dbg_last_draws` (8382), `s_d3d.frame_index` (8384), RSX_FRAME_SLEEP (8389-8391) | `s_rf_entry_qpc` (8380), `s_rf_serial++` (8381), `s_rf_cp` memset (8383) |
| gpu-wait+store | 8399-8411 | FIFO | fence, `frame_index` (2196), `s_wb_pending` (8404) | `fence_values` (2197). Guest RAM through `rt_writeback_store` (8404 → 6797+). `s_fl_wait_ms` and `s_fl_store_ms` (8406-8407). `rsx_scale_apply` (8408 → 8027-8052): off_rt, zeta, subvp, screen copy, readback, render targets, depth, host size. `s_rt_grave` emptied (8409). Texture staging (8410). |
| setup | 8411-8673 | FIFO | captured VP ucode, `dump_frames_left` (8429) | base VP pipeline (`vp_ready` at 2701), `tex_ready` (8527), `readback_buf` (8429-8442), draw fix-ups (8544-8575), allocator/list Reset (8446-8447) |
| rt-prepass | 8673-8700 | FIFO | draws. Guest RAM through `off_rt_drop_rewritten` (8679 → 6893-6910). | `used` flags (8677-8678). Slots, grave and DSV clear through `zeta_get`/`off_rt_get` (8683, 8687; 6247-6284, 6318-6343). `gfmt`/`gpitch`/`gdma` (8688-8690). |
| draw-prepass | 8700-8957 | FIFO | Guest RAM: `late_check` (8701), texture uploads (8903 → 4907+), FP ucode through `vp_get_fp_pso` (8945). `s_screen_copy`, `s_subvp`, off_rt, zeta (8823-8895). | SRV heap (`srv_write` 8808-8938), texture cache, record PSO (8944-8952), drawprint (8956), `s_rf_pp` (8957) |
| legacy-pass | 8959-9101 | FIFO | `clear_color` (8983, written at draw time at 10406), the live `current_rsx_state` (9014) | backbuffer barrier to RENDER_TARGET (8960-8966); `rtv_handle`/`dsv_handle` (8969-8975) |
| vp-pass | 9103-9591 | **render** | `vp_ready`, `draw_count`, `draws` (9106-9109, 9196-9197). `tex_ready` and the VP pipelines (9112-9115). Root sig, `vp_cb`, heaps (9128-9139). `vp_vb`, `vp_ib`, `vf_buf`, `vp_parity`, `s_vp_cap`, `s_vp_icap` (9140-9157, 9207-9210, 9343-9346, 9498, 9502). Host and guest sizes (9166-9167, 9231-9236). `s_present_this_frame` (9191). off_rt and zeta (9246-9302). `RSX_PSO_IN_REC` and `s_rf_serial` (9403-9407). zq state (9539-9540). | Command list. `s_vp_topo_strip` (9131, 9212, 9552). SRV heap (9469, 9487). Sampler tables (9136, 9494 → 6030-6128, which may `wait_for_gpu` at 6080). `s_last_drawn_*` (9250). subvp and screen copy (9233-9239). `s_zq_rec`, `s_zq_area`, `s_zq_n` (9541-9543). `s_perf_gpu` (9580). fpwatch (9581). AA (9193-9194, 9203-9224). |
| post: FIFO-time state | 9594-9745 | **hand-off** | Guest RAM: MEMPEEK (9597-9611), VRAMSCAN (9615-9636). `s_req_*` (9641-9648). `s_perf_*` (9649-9680). `s_lock_*` (9681-9695). | `s_dbg_last_draws` (9696). `vb_offset`, `vp_vb_offset`, `draw_count` (9698-9700). `vp_parity` (9701). `s_vf_off`, `s_fx` (9702-9703). VSTAT (9704-9716). Grows (9717, 9721): `vp_vb`, `vp_ib`, caps, grave. `vp_ib_offset`, `s_vcb_last` (9719-9720). `vp_gen` (9740). |
| post: RTT_SAVERT | 9747-9830 | render | off_rt | its statics (9751-9754), a copy on the list (9808-9820) |
| post: dump arming | 9832-9895 | **hand-off** | `s_dbg_last_draws` (9842). `g_rcap_live_arm`/`_name` (9853-9859; G:4068-4069, written at G:4118-4119 from F9 at 893-896). `s_hotshot_arm` (9869, set at 905). CELLMARK_DUMP_NOW (9891-9895). | `dump_skip_left`/`dump_frames_left` (9843, 9862, 9887, 9895), `s_hotshot_*` (9858-9886), `s_dp_prefix`/`left`/`n` (9865-9866) |
| post: tail | 9896-9952 | render | guest FB (9896, guest RAM at 7261-7264 only with GCM_GUEST_FB), `s_composite_src` (9897 → 7178-7179), `s_present_this_frame` (9900), `dump_frames_left` and `readback_buf` (9906) | `dump_skip_left` (9919), copy to readback (9921-9943), `screen_copy_capture` (9947), barrier to PRESENT (9948-9951) |
| submit+zq | 9954-9979 | render (threaded: no store, no `zq_finish`) | `s_zq_n` (9956, 9978), `s_present_this_frame` (9961) | `rt_writeback_record` (9955 → 6725-6768). Resolve (9957). Blit (9961). Close and Execute (9964-9966). `s_wb_pending=1` (9970), or wait + guest RAM (9973-9976). `zq_finish` writes guest RAM (9979 → G:3290-3295). |
| dumps | 9981-10136 | render | D3D12_IQ (9987-10010), `s_sc_dump_pending` (10012), RTT_SAVERT write (10025-10106), RSX_CAP (10112-10136 → 2479-2573: reads `draws` and the mapped constants) | files. Waits at 10014, 10020 and 10030. `dump_frames_left--` (10022). |
| present | 10138-10166 | render | vsync and tearing (10148-10154). `s_overlay` (10155). `knob_present`, `s_session_locked` (10158). | overlay `frame` (10156), Present (10159), `frame_log` (10162 → 7986-8020, `GetFrameStatistics` at 8008), `pso_lib_tick` (10163, **moves to the hand-off**), `prof_record` (10166) |
| next frame | 10168-10171 | render | `s_win_req` (1303) | `frame_index` and `fence_values` (2228-2248, 1327-1330). Swap buffers and `win_w`/`win_h` (1308-1318). Overlay `release` (1307). `frame_count` (10171). |
| titlebar | 10173-10220 | **FIFO, after the hand-off** | `s_dbg_last_draws`, size, `g_gcm_flip_words`, guest label 255 (10198-10199) | `SetWindowTextA` (10188), GCM_RATES line (10207-10212) |

Draw-time code (10403-12835) calls none of `cmd_list`, `cmd_queue`, `wait_for_gpu`, `srv_heap->`, `off_rt_*`, `zeta_*`, `vp_get_fp_pso` or `Release` (grep). The FIFO thread therefore touches no GPU object between flips, which docs/66:86-91 also found.

## 2. Split point and edits

### 2.1 The split

The cut goes between the end of the legacy pass (9101) and `RF_CP(5)` (9103). The legacy pass stays on the FIFO thread for two reasons:
- It reads the live `current_rsx_state` (9014; docs/66:57-59).
- It reads `clear_color` (8983), which `d3d12_clear_display` rewrites at draw time (10406).

Five locals cross the cut:
- `fi` (8384)
- `_rf0` (8379)
- `barrier` (8960-8966)
- `rtv_handle` (8969-8971)
- `dsv_handle` (8974-8975)

If moved code needs any other local, it fails to compile. Globals give no such error, so §2.8 lists them to check by hand.

### 2.2 New declarations (insert after 8375, next to `s_vp_topo_strip`)

```c
/* RSX_RF_THREAD: render_frame's second half on a render thread (docs/56 §7). */
typedef struct RfJob {
    D3D12DrawRecord* draws;                  /* s_draw_lists[parity] */
    u32  n;                                  /* draw_count, capped at MAX_DRAWS */
    u32  fi;                                 /* 8384 */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;    /* 8969-8975 */
    D3D12_RESOURCE_BARRIER barrier;          /* 8960-8966 */
    u32  parity;                             /* vp_parity before 9701 */
    ID3D12Resource* vp_vb; void* vp_vb_mapped; ID3D12Resource* vp_ib;  /* before 9717/9721 */
    u32  vp_cap, vp_icap;                    /* s_vp_cap (601), s_vp_icap (605) before the grows */
    u32  serial;                             /* s_rf_serial (325) */
    int  present;                            /* s_present_this_frame (7044) */
    u32  composite_src;                      /* s_composite_src (7170), cleared at the hand-off */
    int  vp_go, any;                         /* 9106-9109 */
    ID3D12PipelineState* vpso;               /* 9112-9115 */
    int  aa_t; u32 aa_before;                /* 9187-9195 */
    int  pso_rec;                            /* 9403-9404 */
    int  zq;                                 /* the 9539 test: (s_zq_nev || s_zq_on_start) */
    int  threaded;
} RfJob;
static D3D12DrawRecord s_draw_lists[2][MAX_DRAWS];   /* ~18 MB more BSS (guess: sizeof ~1.1 KB) */
static RfJob  s_rf_job;
static HANDLE s_rf_thread, s_rf_go /* auto-reset */, s_rf_done /* manual-reset, starts set */;
static int    s_rf_busy, s_rf_dead;          /* FIFO thread only */
static volatile LONG s_rf_quit;
static u32    s_rf_n_thr, s_rf_n_inl, s_rf_n_zq;   /* GCM_RATES, per second */
static double s_rf_join_ms, s_rf_hand_ms;
static void rf_handoff(RfJob* j, u32 fi, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                       D3D12_CPU_DESCRIPTOR_HANDLE dsv, const D3D12_RESOURCE_BARRIER* b,
                       double rf0, int may_thread);
static void rf_second(RfJob* j);
static void rf_titlebar(void);

static DWORD WINAPI rf_thread_main(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(s_rf_go, INFINITE);       /* an event, no spinning */
        if (s_rf_quit) return 0;
        rf_second(&s_rf_job);
        SetEvent(s_rf_done);
    }
}

static int rf_thread_start(void)                      /* lazily, on the FIFO thread */
{
    if (s_rf_thread) return 1;
    if (s_rf_dead) return 0;
    s_rf_quit = 0;
    s_rf_go   = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_rf_done = CreateEventA(NULL, TRUE, TRUE, NULL);
    s_rf_thread = (s_rf_go && s_rf_done) ? CreateThread(NULL, 0, rf_thread_main, NULL, 0, NULL) : NULL;
    if (!s_rf_thread) { s_rf_dead = 1; fprintf(stderr, "[RF] no render thread: rendering inline%c", 10); return 0; }
    SetThreadPriority(s_rf_thread, GetThreadPriority(GetCurrentThread()));   /* as the FIFO thread */
    return 1;
}

/* Every render_frame entry and shutdown. The render thread's own waits are
 * bounded (2212-2224, 2238), so a wait here that never ends is a driver hang
 * the inline path would hit too. Like wait_for_gpu (2203-2211), it is never silent. */
static void rf_join(void)
{
    if (!s_rf_busy) return;
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    for (int s = 2; WaitForSingleObject(s_rf_done, 2000) == WAIT_TIMEOUT; s += 2)
        fprintf(stderr, "[RF] waiting %d s for the render thread%c", s, 10);
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    s_rf_join_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    s_rf_busy = 0;
}

static int rf_may_thread(const RfJob* j)              /* §4 says why for each line */
{
    if (j->zq) { s_rf_n_zq++; return 0; }
    if (write_color_on() && !write_color_defer()) return 0;
    if (j->pso_rec != 1) return 0;
    if (!s_window_thread) return 0;
    { static int gfb = -1; if (gfb < 0) gfb = getenv("GCM_GUEST_FB") ? 1 : 0; if (gfb) return 0; }
    if (getenv("RSX_CAP")) return 0;
    return rf_thread_start();
}
```

SetEvent and WaitForSingleObject are full memory barriers, per the Win32 rules for synchronization functions. The go and done events therefore also publish the job and everything written before the hand-off.

### 2.3 render_frame (8376-10221)

1. **8376.** Change the signature to `static void render_frame(int may_thread)`.
2. **New first statement, before 8378:** `rf_join();`.
3. **8379-9101 stay as they are.**
4. **9103-10221 become:**

```c
    { LARGE_INTEGER h0, h1, f; QueryPerformanceCounter(&h0);
      RfJob* j = &s_rf_job;
      rf_handoff(j, fi, rtv_handle, dsv_handle, &barrier, _rf0, may_thread);
      QueryPerformanceCounter(&h1); QueryPerformanceFrequency(&f);
      s_rf_hand_ms += (double)(h1.QuadPart - h0.QuadPart) * 1000.0 / (double)f.QuadPart;
      if (j->threaded) { s_rf_busy = 1; s_rf_n_thr++; ResetEvent(s_rf_done); SetEvent(s_rf_go); }
      else { if (may_thread) s_rf_n_inl++; rf_second(j); } }
    rf_titlebar();
}
```

`ResetEvent(s_rf_done)` must come before `SetEvent(s_rf_go)`. If it came after, it could clear a done the render thread had already set, and the join would never return.

To keep the diff readable, define `rf_handoff`, `rf_second` and `rf_titlebar` just below render_frame, where the moved code used to be.

### 2.4 rf_handoff (FIFO thread), in this order

The order matters. The batch has to be snapshotted before the parity flip (9701) and the grows (9717, 9721), while `s_d3d.draws` still points at this frame's list. Steps b-d also read `s_d3d.draws`.

a. **Snapshot.**
   - Copy `draws`, `n`, `fi`, `rtv`, `dsv`, `barrier`, `parity`, `vp_vb`, `vp_vb_mapped`, `vp_ib`, `vp_cap`, `vp_icap`, `serial` and `present`.
   - Take and clear the composite source: `j->composite_src = s_composite_src; s_composite_src = 0;`. No caller of `rsx_d3d12_composite_from_offscreen` (7174) is in the tree today (grep), so this is only a precaution.

b. **9106-9126, moved.**
   - `j->vp_go = s_d3d.vp_ready && s_d3d.draw_count > 0`.
   - `j->any` and `j->vpso` as in 9107-9115. They read `tex_ready`, which draw-time code clears (12739).
   - The `[VPPASS]` print (9116-9126).

c. **9182-9195, moved.** Only when `j->vp_go && j->any && j->vpso`, because today this code sits inside 9127. It fills `j->aa_t` and `j->aa_before`. `aa_pick` reads `s_d3d.draws` (8313, 8332).

d. **PSO retry**, so the render thread never needs a lookup:
   ```c
   static int pso_rec = -1; PS3_KNOB(pso_rec, ps3_env_int("RSX_PSO_IN_REC", 1));
   j->pso_rec = pso_rec;
   if (j->vp_go && j->any && j->vpso && pso_rec == 1)
       for (u32 d = 0; d < j->n; d++) {
           D3D12DrawRecord* dr = &j->draws[d];
           if (!dr->is_vp || !dr->fp_addr || (dr->pso && dr->pso_serial == s_rf_serial)) continue;
           dr->pso = vp_get_fp_pso(/* exactly the arguments of 9410-9415 */);
           dr->pso_serial = s_rf_serial;
       }
   ```
   In steady state the pre-pass (8944-8952) has already filled every record, so this is only a pass over the flags. It must come before step g, because `vp_gen++` (9740) is in there and a lookup today happens before it.

e. **Decide.**
   ```c
   j->zq = g_rsx_report_hook && (s_zq_nev || s_zq_on_start);
   j->threaded = may_thread && rf_may_thread(j);
   ```

f. **`if (j->threaded) zq_finish();`** For a threaded frame this only copies `s_zq_on` (7470). It runs here, on the thread `zq_hook` runs on (C:461-462), never on the render thread.

g. **9594-9745, moved verbatim.**
   - `_rf0` becomes the `rf0` parameter.
   - Right after 9701, add: `s_d3d.draws = s_draw_lists[s_d3d.vp_parity];`.

h. **9832-9895, moved verbatim.** This includes the `skip_dump_consider` label at 9844. Arming then happens at the same point in the frame as today, on one thread. Moving it is a choice: no race forces it.

i. **`if (j->present) pso_lib_tick();`**, moved from 10163. `s_pso_lib_quiet` and `s_pso_lib_new` (3174-3175) belong to the thread that creates pipelines.

### 2.5 rf_second(RfJob* j): the moved 9103-10171, with these edits

| Old line(s) | Change |
|---|---|
| new prologue | `const u32 fi = j->fi; const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = j->rtv, dsv_handle = j->dsv; D3D12_RESOURCE_BARRIER barrier = j->barrier; RF_CP(5);` |
| 9106-9127 | Becomes `if (j->vp_go && j->any && j->vpso) {`; uses of `vpso` become `j->vpso`. The removed lines are in the hand-off. |
| 9141-9143 | `s_d3d.vp_vb` → `j->vp_vb`, `s_d3d.vp_parity` → `j->parity`, `s_vp_cap` → `j->vp_cap` |
| 9146-9150 | `s_d3d.vp_ib` → `j->vp_ib`, parity, `s_vp_icap` → `j->vp_icap` |
| 9157, 9210 | `s_d3d.vp_parity` → `j->parity` |
| 9187-9195 | Replace with `int aa_t = j->aa_t; u32 aa_before = j->aa_before;` |
| 9196-9197 | `s_d3d.draw_count` → `j->n`, `s_d3d.draws[d]` → `j->draws[d]` |
| 9344-9345 | `s_d3d.vp_vb_mapped` → `j->vp_vb_mapped`, parity, `s_vp_cap` → `j->vp_cap` (VP_TESTTRI) |
| 9403-9404 | `const int pso_rec = j->pso_rec;` (no knob read here) |
| 9407 | `s_rf_serial` → `j->serial` |
| 9408 | `if (!dpso \|\| pso_rec == 2)` → `if (!j->threaded && (!dpso \|\| pso_rec == 2))`. A threaded draw whose lookups failed falls back as a failed lookup does today (9573). |
| 9498, 9502 | `s_d3d.vp_parity` → `j->parity` |
| 9539 | `(s_zq_nev \|\| s_zq_on_start)` → `j->zq` |
| 9581 | `fpwatch_batch();` → `fpwatch_batch(j->draws, j->n);` (it has to stay here: `fpwatch_unreadable` is called at 9472) |
| 9593 | `RF_CP(6);` stays, at the end of the VP pass |
| 9594-9745 | removed (in the hand-off) |
| 9747-9830 | unchanged. The statics at 9751-9754 come along, since 10025-10105 use them. |
| 9832-9895 | removed (in the hand-off) |
| 9897 | `composite_present(fi);` → `composite_present(fi, j->composite_src);` |
| 9900, 9961 | `s_present_this_frame` → `j->present` |
| 9979 | `zq_finish();` → `if (!j->threaded) zq_finish();` |
| 10132 | `rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done)` → `rsx_capture_frame(fi, j->draws, j->n, j->parity, s_cap_done)` |
| 10155, 10158, 10162, 10164, 10166 | `s_present_this_frame` → `j->present` |
| 10163 | removed (in the hand-off) |
| 10168-10171 | unchanged: `move_to_next_frame`, `swap_resize`, `frame_count++` |
| 10173-10220 | moved verbatim into `rf_titlebar()`, called at the end of render_frame |

For a threaded frame, 9969-9978 do no waiting:
- `write_color_defer()` is 1, because the gate requires it, so 9970 only sets `s_wb_pending`. The FIFO thread reads it after the join, at 8404.
- `s_zq_n` is 0, so neither 9956 nor 9978 fires.

### 2.6 Helpers whose signature changes

- **`fpwatch_batch` (7619):** becomes `static void fpwatch_batch(const D3D12DrawRecord* draws, u32 n)`. 7629-7630 read `draws[d]` and `n`.
- **`rsx_capture_frame` (2479):** becomes `(u32 fi, const D3D12DrawRecord* draws, u32 ndraws, u32 par, u32 capidx)`.
  - 2500 and 2535 read `&draws[d]`.
  - 2531 uses `par` instead of `(u32)(s_d3d.vp_parity ^ 1)`.
  - This is needed inline too, because of the list swap.
- **`composite_present` (7176):** becomes `static void composite_present(u32 fi, u32 want)`. Delete 7178-7179.

### 2.7 Callers, init, shutdown, counters

- **d3d12_fifo_flip (10394).** Becomes `{ static int rft = -1; PS3_KNOB(rft, ps3_env_int("RSX_RF_THREAD", 0)); render_frame(rft >= 1); }`.
  - PS3_KNOB is a per-site live knob (include/ps3emu/ps3_knob.h:45-50), so the console can flip it during a run. That is what an in-boot A/B needs.
  - Switching at any flip is safe, because every entry joins first.
- **The other callers pass 0:** 10288 (`d3d12_present`), 10499 (`d3d12_clear_display`) and 13090 (`rsx_d3d12_backend_present`).
- **Init.** After the memset at 12840: `s_d3d.draws = s_draw_lists[0];`.
- **Struct field.** Line 550, `D3D12DrawRecord draws[MAX_DRAWS];`, becomes `D3D12DrawRecord* draws;   /* s_draw_lists[vp_parity] */`. No `sizeof(s_d3d.draws)` exists (grep).
- **Shutdown.** Before 12914:
  ```c
  if (s_rf_thread && GetThreadId(s_rf_thread) != GetCurrentThreadId()) {
                     WaitForSingleObject(s_rf_done, 6000); InterlockedExchange(&s_rf_quit, 1);
                     SetEvent(s_rf_go); WaitForSingleObject(s_rf_thread, 2000);
                     CloseHandle(s_rf_thread); s_rf_thread = NULL; s_rf_busy = 0; }
  ```
  - It waits on the event rather than `s_rf_busy`, because shutdown may run on another thread (guess).
  - The thread-id check is there because the overlay's frame hook can call the app's quit (UI:523), and on a threaded frame that hook runs on the render thread. If quit ever reaches shutdown synchronously, the render thread would otherwise wait 8 s on itself. Whether quit does reach it is open (§6).
- **GCM_RATES.** In `rf_titlebar`, append ` | rf thr=%u inl=%u zq=%u join=%.1fms hand=%.1fms` to the line at 10207-10212, then zero the five counters.
- **Optional `RF_ASSERT_FIFO`.** A log-once check that compares `GetCurrentThreadId()` with the FIFO thread's id (saved in `d3d12_fifo_flip`), active only while `s_rf_thread` exists. Put it first in:
  - `zq_hook` (7419)
  - `zq_finish` (7468)
  - `vp_get_fp_pso` (4030)
  - `vp_vs_insert` (3222)
  - `vp_vb_grow` (7313)
  - `vp_ib_grow` (7356)
  - `rt_writeback_store` (6797)
  - `pso_lib_tick` (3157)

### 2.8 After the move: search rf_second for globals

Search `rf_second`'s body for each of these:
- `s_d3d.draws`, `s_d3d.draw_count`
- `vp_parity`, `s_d3d.vp_vb`, `s_d3d.vp_ib`, `vp_vb_mapped`
- `s_vp_cap`, `s_vp_icap`
- `s_rf_serial`, `s_present_this_frame`, `s_composite_src`
- `s_zq_nev`, `s_zq_on_start`, `s_zq_ev`
- `vp_get_fp_pso`, `vm_base`, `current_rsx_state`, `tex_ready`

The only hits allowed are:
- the inline-only lookup (9408);
- `zq_finish` (9979);
- `guest_fb_present`, which reads `vm_base` at 7261 and runs with GCM_GUEST_FB only, an inline-only setting;
- `rsx_capture_frame`, behind RSX_CAP, also inline-only.

## 3. Shared resources and what protects them

### 3.1 By resource

| Resource | FIFO side | Render side | Protection |
|---|---|---|---|
| Command list and allocators | Reset (8446-8447), pre-pass and legacy recording | VP pass through Close (9964) | Handoff order. The FIFO thread's next use comes after `rf_join` and `wait_for_gpu`. |
| fence, `fence_event`, `fence_values`, `frame_index` | `wait_for_gpu` (8401), after the join | `move_to_next_frame` (2228-2248), `swap_resize` (1327-1330), waits (6080, 9973, 9978, 10014, 10020, 10030) | The join. No other `wait_for_gpu` caller exists outside render_frame and shutdown (grep). |
| Shader-visible SRV heap | `srv_write` in the pre-pass (8808-8938) | 9469, 9487, AA (8225) | Sequential: the next pre-pass runs after the join and the GPU wait |
| Sampler heap and tables | — | 9136, 9494 → 6030-6128 only | One thread. `now` is `frame_count` (6040), which only the render thread advances. |
| off_rt[], s_zeta[], s_subvp, s_screen_copy, readback buffers | scale (8027-8052), rt-prepass (8677-8690), pre-pass finds | transitions, snapshots and captures (9246-9534, 9808-9820, 6725-6768, 7061+, 664-720) | Sequential. Draw-time code calls none of them (§1). |
| `s_rt_grave` | grows at the hand-off (7346, 7380), rt-prepass, scale | snapshots (6996), AA (8264), downscale (6661) | Sequential. It is emptied at 8409, after the join and `wait_for_gpu`, so frame N's resources outlive its GPU work. |
| Parity halves: vp_vb, vp_ib, vf_buf, vp_cb, vp_fpcb | draw time writes half `vp_parity` (11191-11696, 12047-12072, 5675, 5782) | binds half `j->parity` | Flipped at the hand-off. Half N is written again only after the join and the wait at the entry of N+2. |
| Draw records | write `s_draw_lists[parity N+1]` | read `j->draws` (const, 9197) | Two lists |
| zq state | `zq_hook` writes `s_zq_ev`, `s_zq_nev`, `s_zq_on` (7419-7431) | reads them only when `j->zq`, which is 0 for every threaded frame | `s_zq_rec`, `s_zq_area` and `s_zq_n` are written only by the VP pass and `zq_finish` |
| `s_composite_src` | setter (7174) | — | Snapshot and clear at the hand-off |
| Dump, F9 and F10 arming | read at the hand-off (9832-9895) | `dump_backbuffer_bmp` uses `s_hotshot_*` (2274-2300) | Sequential. Writers: the window thread (905) and F9 (G:4118-4119, an Interlocked increment after the name is written). |
| PSO cache, `vp_vs`, PSO library | pre-pass (8945), hand-off retry, `pso_lib_tick` | never (9408 is inline-only) | One thread |
| `frame_count` | read by TEX_BUDGET (4917, pre-pass only: 8903) and logs | `++` (10171); read at 6040, 6690, 9774, 10131 | Stable after the join. Between flips, FIFO-side readers are logs only. |
| `s_rf_cp` and the profile inputs | cp0-4, memset at 8383 after the join | cp5-9; `prof_record`, `rf_slow_report`, `frame_log` read them all | Sequential |
| Trace marks | `RSX_MARK` (8382) | `RSX_MARK("present")` (10164) | `ppu_trace_mark` (BM:1550) is the PPU tracer, called from many threads (guess) |

### 3.2 Present, resize and the window

These swap-chain calls exist (grep):

| Call | Line | Where it runs |
|---|---|---|
| `GetBuffer` | 1073 | `swap_buffers_get`, called at 1324 and 1607 |
| `ResizeBuffers` | 1313 | `swap_resize` |
| `GetCurrentBackBufferIndex` | 1327, 1581, 2233 | `swap_resize`, init, `move_to_next_frame` |
| `GetFrameStatistics` | 8008 | `frame_log` |
| `Present` | 10159 | the tail |
| `Release` | 12952 | shutdown |

`swap_resize`'s only caller is 10169, right after Present. Everything else is init or shutdown. So:

- **What moves with Present:**
  - overlay `frame` (10155-10157)
  - Present (10158-10160)
  - `frame_log` with `GetFrameStatistics` (10162)
  - `RSX_MARK` (10164)
  - `rf_slow_report` and `prof_record` (10165-10166)
  - `move_to_next_frame` (10168)
  - `swap_resize` with the overlay `release` (10169, 1307)
  - `frame_count++` (10171)
- **What does not move:** `pso_lib_tick` goes to the hand-off. The titlebar and GCM_RATES go to the FIFO side, after the hand-off.
- **No resize/Present lock exists today, and none is needed.** The join is the lock: every render_frame entry waits on `s_rf_done`, so swap-chain calls never overlap. The race docs/66:124-126 asked about cannot happen:
  - `d3d12_present` (10265-10298) is reached only from rsx_draw_engine.c:2343 (grep). It reaches the swap chain only through render_frame, which joins first.
  - `rsx_d3d12_backend_present` (13005-13093) is the ticker present (BM:415). It returns at 13018 once `s_clear_presents > 0`, and `d3d12_fifo_flip` sets that at its first present (10396). Before that it renders inline, after a join.
  - The window thread never calls DXGI. It publishes the size with `InterlockedExchange` (845) and only moves the window (`set_borderless` 811-830, `WM_DPICHANGED` 858-862).
  - `swap_resize` reads `s_win_req` (1303). On failure it writes it back with `InterlockedExchange` (1322).
  - `s_session_locked` is Interlocked too (878-879) and read at 10158.
- **Present already runs off the window's thread today:** "DXGI's flip model presents to a window owned by another thread" (1028-1029). swap_resize is already described as "Between frames, on the render thread" (1298-1300).
- **`set_borderless` (811-830) is window-thread state** (805-809).
  - With the window thread, `rsx_d3d12_set_borderless` posts `WM_RSX_BORDERLESS` (12981), and Alt+Enter is handled at 864-868.
  - The resulting `WM_SIZE` reaches `swap_resize` through `s_win_req`.
  - With RSX_WINDOW_THREAD=0 it runs on the caller's thread (12982), but that mode renders inline (§4.3).

### 3.3 Frame dumps and the overlay

**Dumps that keep working on the render thread.** F9 live presents, F10 bursts, CELLMARK_DUMP, the screen-copy dump and RTT_SAVERT:
- The arming is in the hand-off.
- The copies and file writes are in `rf_second`, with their own waits (10014, 10020, 10030). They hold up only the render thread, and the FIFO thread only through the next join.
- RSX_CAP runs inline only (§4.3).

**The overlay contract.** `frame` and `release` are called "on the render thread" (H:63-72). des_ui already treats that as a thread separate from the window thread:
- its queue and OSK state are under mutexes (UI:98, 146);
- the window-thread flags are atomics (UI:108-109).

With RSX_RF_THREAD=1, `frame` and `release` run on the render thread for threaded frames and on the FIFO thread for inline frames. The two never overlap, but the thread changes from frame to frame. No `thread_local` or `GetCurrentThreadId` exists in port/ (grep), so RmlUi and the overlay should not care (guess).

If one does care, run inline frames on the render thread too: `SetEvent(s_rf_go)`, then `rf_join()` before returning. Ordering is identical, at the cost of one wake-up per inline frame.

## 4. What forces a frame inline

### 4.1 Occlusion-query frames: `j->zq`

**Why today's design needs them inline:**
- `zq_finish` stores the ZPASS reports with `cellGcm_report_store` (7495 → G:3290-3295) once the GPU has finished.
- Today that happens before the FIFO thread passes the flip (docs/66:94-100, 112-116), and the guest may read a report right after.
- With the hook installed, GET_REPORT writes no placeholder for ZPASS (G:2179-2183). The slot holds whatever was stored last.
- The VP pass also reads `zq_hook`'s live event list (9539-9540; `zq_counted` 7434-7440), while the FIFO thread appends frame N+1's events (7425-7429).

**Stage 1 rule:** run inline when `g_rsx_report_hook && (s_zq_nev || s_zq_on_start)`.
- This is the necessary condition at 9539, so it is conservative.
- docs/56:1163 phrases the rule as `s_zq_n > 0`, but `s_zq_n` is known only after the VP pass. The hand-off has to predict it.

### 4.2 Guest memory at the flip

**Reads stay on the FIFO thread:**
- textures: `vp_upload_tex_slot` (8903 → 4907+)
- fragment ucode: `vp_get_fp_pso` (4037-4069, 4145-4156), in the pre-pass (8945) and the hand-off retry
- target checksums: `off_rt_drop_rewritten` (8679 → 6893-6910)
- late checks: `late_check` (8701 → 5584+)
- MEMPEEK and VRAMSCAN (9597-9636, now in the hand-off)
- label 255: GCM_RATES (10198-10199, now in `rf_titlebar`)
- `current_rsx_state` (9014)

**Writes:**
- The deferred write-back store keeps today's timing: it runs at the next entry (8404), on the FIFO thread, after the join.
- The non-deferred store (9973-9976) and `zq_finish` (9979) run inline.

### 4.3 The other gates in rf_may_thread

| Gate | Why |
|---|---|
| `write_color_on() && !write_color_defer()` (6504-6522; both default to 1) | The store before Present writes guest RAM (9973-9976) |
| `RSX_PSO_IN_REC != 1` (9403-9404) | Modes 0 and 2 look up at render time (9408-9415) |
| `!s_window_thread` (RSX_WINDOW_THREAD=0, 1052) | The FIFO thread then pumps the window (BM:418 → 12996-13001; BM:700). A DXGI or overlay call that sends it a message would wait on a thread blocked in `rf_join`. |
| GCM_GUEST_FB (7240) | `guest_fb_present` reads guest RAM in the post tail (7261-7264) |
| RSX_CAP (10112) | The capture reads the batch's records and mapped constants (2500-2538). Kept inline to keep it simple. |
| Render thread could not start | `s_rf_dead` |
| Callers other than `d3d12_fifo_flip` | They pass 0 (§2.7) |

**Not gates:**
- F9, F10 and CELLMARK dumps
- RTT_SAVERT, D3D12_IQ, AA
- RSX_SCALE changes, which `rsx_scale_apply` applies at the next entry, after the join
- resizing, Alt+Enter, the session lock

### 4.4 If Step 0 shows many zq frames

**Stage 1a: exact pre-check (no visible change).**
- Thread the event-only frames too. Call `zq_finish()` at the hand-off: with `s_zq_n == 0` it stores the same values, from `s_zq_acc`, as the inline path would.
- Replace the `j->zq` line with `j->zq = zq_batch_counts(j->draws, j->n)`:

  ```c
  /* Would 9538-9546 begin any query in this batch? Mirrors zq_counted (7434-7440). */
  static int zq_batch_counts(const D3D12DrawRecord* draws, u32 n)
  {
      if (!g_rsx_report_hook || (!s_zq_nev && !s_zq_on_start)) return 0;
      if (s_zq_on_start) return 1;
      int on = 0, e = 0; u32 last = 0;
      for (u32 d = 0; d < n; d++) {
          const D3D12DrawRecord* dr = &draws[d];
          if (!dr->is_vp || dr->is_clear) continue;
          if (dr->cb_slot < last) return 1;          /* not in record order: don't guess */
          last = dr->cb_slot;
          for (; e < s_zq_nev && s_zq_ev[e].at <= dr->cb_slot; e++)
              if (s_zq_ev[e].kind <= ZQ_ON) on = (int)s_zq_ev[e].kind;
          if (on) return 1;
      }
      return 0;
  }
  ```

**Stage 1b: `RSX_RF_THREAD=2`, opt-in, because the guest sees a change.**
- **Hand-off:** copy `s_zq_ev`/`s_zq_nev`/`s_zq_on_start` into the job, then set `s_zq_nev = 0` and `s_zq_on_start = s_zq_on`.
- **Render thread:** begins queries from the copy, then resolves (9956-9958), without the 9978 wait.
- **Next entry:** after `rf_join` and `wait_for_gpu` (8401), the FIFO thread runs `zq_finish` on the copy.
- **Visible change:** reports land one flip later than today, which is about two frames behind hardware (7396-7397).
- **Risk:** a title that waits on a report before its next flip would stall. Demon's Souls's flare reads the report without waiting (guess). Check with ZPASS_LOG and a live look.

## 5. Order of work, tests, risks

### 5.1 Steps

Put Step 1 and Step 2 in separate commits, so a bisect can tell the refactor from the thread.

**Step 0: measure, no build.**
- Do a Palace run with ZPASS_LOG (command 1 in §5.2), then count the `[ZPASS] report ... (N queries this batch)` lines in work/tmp/play.txt against presents/s on the `[gcm-rates]` lines.
  - `N > 0` means a frame that issued queries. Stage 1 runs those inline.
  - `N = 0` means an event-only frame. Stage 1 also runs those inline; Stage 1a can thread them.
  - A frame with no line at all has no zq activity, and threads.
- If most frames have lines, plan Stage 1a or 1b before expecting the gain.

**Step 1: refactor, no thread.**
- Contents:
  - RfJob and the two lists;
  - `rf_handoff`, `rf_second` and `rf_titlebar`;
  - the three helper signatures;
  - `render_frame(int)`, with every caller passing 0 for now.
- Behaviour should be identical. The exception is PERF=1 accounting: `s_perf_rf` (9594) now stops before the VP pass, and the VP pass's `s_perf_gpu` (9580) lands in the next 20-frame window.
- Tests:
  - regress at 30 and at 240, both against the good set;
  - a live look: HUD, sun flare outdoors, fog walls, particles, menus;
  - one F9 capture with capdiff;
  - one RSX_CAP capture, to confirm the manifest still lists the batch's ops.

**Step 2: the thread.**
- Contents: the declarations in §2.2, the knob read at flip, the gates, the counters, shutdown, and optionally `RF_ASSERT_FIFO`.
- Tests:
  1. RSX_RF_THREAD=0 regress must equal Step 1.
  2. RSX_RF_THREAD=1 regress at 30 and at 240.
  3. A same-spot pair with REGRESS_AB, comparing s5_ab with s6_back.
  4. A live look with GCM_RATES=1, checked against these expected values:
     - `thr` close to presents/s;
     - `inl` and `zq` small;
     - `join` about 0 ms;
     - no `[RF]` lines.
  5. During the live run, look for flicker and missing draws:
     - walk the Palace;
     - look at the sun;
     - fog walls;
     - the wrapper UI (F1) and the game menu;
     - drag-resize the window;
     - Alt+Enter twice;
     - F9 and F10;
     - Win+L, then unlock.

**Step 3: measure the gain.**
- An in-boot live-knob A/B with the knob line `RSX_RF_THREAD=1`. Use the in-boot knob A/B, `tools/des/knobab.sh` (docs/63).
- Then a cross-build A/B.
- Use the high vblank rate (knobab.sh's default, `VBLANK_MHZ=960000`) and vsync off (it sets `RSX_VSYNC=0`), so no cap hides the gain.
- Expected: about 1.0-1.1 ms off the FIFO thread per frame.

**Step 4: only if Step 0 or the `zq` counter shows many zq frames.** Stage 1a, then Stage 1b behind RSX_RF_THREAD=2.

### 5.2 Commands (single-line PowerShell)

For unattended runs:
- Set `RSX_PRESENT=0` while the session is locked.
- Run from the pinned save. regress.sh pins and restores the save itself (regress.sh:20-22, 36-44).

| # | What | Command |
|---|---|---|
| 1 | Step 0 | `& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 ZPASS_LOG=3000 GCM_RATES=1"` |
| 2 | Live look | `& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 RSX_RF_THREAD=1 GCM_RATES=1"` |
| 3 | Regress at 30 | `& "C:\Program Files\Git\bin\bash.exe" -c "RSX_RF_THREAD=1 tools/des/regress.sh"` |
| 4 | Regress at 240 | `& "C:\Program Files\Git\bin\bash.exe" -c "DES_FPS=240 RSX_RF_THREAD=1 tools/des/regress.sh"` |
| 5 | Same-spot pair | `& "C:\Program Files\Git\bin\bash.exe" -c "REGRESS_AB=RSX_RF_THREAD=1 tools/des/regress.sh"` |

### 5.3 Risks, ranked

1. **zq frames cancel the gain.**
   - If most Palace frames carry ZPASS events, Stage 1 runs them inline and saves nothing.
   - The docs/66:67-70 timing argues against this (inference). Step 0 and the `zq` counter decide; Stage 1a and 1b are the fallbacks.
2. **A global read that was missed.**
   - A FIFO-time write racing a render-thread read shows as flicker or missing draws.
   - Mitigations: the search in §2.8, `RF_ASSERT_FIFO`, regress at 30 and 240, and the live checks.
   - The known cases were all handled above: `tex_ready` (12739), `s_composite_src`, the zq events, `vp_vs`, the grows and the parity reads.
3. **PSO lookup and the `vp_vs` reallocation.**
   - A threaded frame must never reach 9409. The retry in step d and the `!j->threaded` guard cover it.
   - A draw with no PSO after both tries falls back exactly as a failed lookup does today.
4. **The overlay and DXGI change thread.**
   - frame and release alternate between the render thread and the FIFO thread (§3.3). The fallback is "inline frames run synchronously on the render thread".
   - A window deadlock is ruled out by the `!s_window_thread` gate, and by moving `SetWindowTextA` to the FIFO side.
5. **Grow and deferred-release ordering.**
   - The grows at the hand-off put the old buffers in the grave (7346, 7380) while the render thread still binds them through the job.
   - That is correct only because 8409 runs after `rf_join` and `wait_for_gpu`. Any future early release (6158 is a best-effort `wait_for_gpu` path) must keep that order.
6. **Profile semantics change.**
   - `legacy-pass` (cp5-cp4) now includes the hand-off, plus the wake-up when threaded.
   - `rf` in `prof_record` (7849) and FRAME_LOG (8014) becomes wall time from entry to Present.
   - `flat` (7850) can cross a flip if the render thread lags.
   - The join time shows only on the GCM_RATES line. Note this in docs/56 when the A/B is reported.
7. **Some debug views see only inline frames.** RSX_CAP, GCM_GUEST_FB, `RSX_PSO_IN_REC=0/2` and RSX_WINDOW_THREAD=0 all switch threading off while set.
8. **CPU contention and join waits.**
   - The render thread needs a free core. docs/66:144 counts 5.48 cores busy.
   - If it lags, the FIFO thread waits in `rf_join` and the saving shrinks. The `join` counter shows this directly.

## 6. Open points

None of these blocks Step 1. Each says how to close it.

1. **How many Palace frames issue occlusion queries.** Open. Step 0 (§5.1) answers it without a build.
2. **Which thread reaches `d3d12_present`.** Open.
   - Grep finds one caller, rsx_draw_engine.c:2343. I did not trace which thread runs it.
   - `s_rf_busy` assumes every render_frame caller is the FIFO thread.
   - If any caller is not: drop `s_rf_busy` and have `rf_join` wait on `s_rf_done` whenever `s_rf_thread` exists. The event stays set while the thread is idle, so the wait is one cheap call.
3. **`sizeof(D3D12DrawRecord)`.** Open. The 18 MB for the second list is a guess. Print the size once at init.
4. **Where the overlay host submits.** Open.
   - Known: des_ui hands the queue to its host at init (UI:406). That runs lazily from the frame hook (UI:518-520).
   - Not found: the host's own `ExecuteCommandLists` is not under port/ui. Grep finds only crash_dialog.cpp:180, so I did not read it.
   - No problem is expected (guess): a D3D12 queue is free-threaded, and only one thread calls the hook at a time.
   - No `thread_local` or `GetCurrentThreadId` is in port/ (grep).
5. **Whether quit reaches shutdown synchronously.** Open.
   - The frame hook can call `ui::app().quit()` (UI:523).
   - One close path in des_ui posts `WM_CLOSE` (UI:666), which is asynchronous. That suggests quit does not reach shutdown synchronously (guess: I did not trace `quit`).
   - The thread-id check in §2.7 covers the case either way.
6. **The crash dialog.** Open. It creates its own swap chain and submits its own work (crash_dialog.cpp:109-184, 303). I did not check whether it reads backend state when the crash happens on the render thread.
7. **Stage 1b's one-flip report delay.** Open, and only if Stage 1b is needed. Check with ZPASS_LOG and a live look that the sun flare still fades correctly.
8. **The size and day estimates are guesses.** Nothing was built or run.
9. **Checked and closed this session:**
   - **`d3d12_fifo_flip` after `render_frame` (10395-10401).** It touches no GPU object.
     - `cellGcm_rcap_cut` (10397) is in cellGcmSys.c, which has no d3d12 calls (grep).
     - Its resets of `draw_count`, `vb_offset` and `vp_vb_offset` (10399-10401) repeat the hand-off's 9698-9700. They run after the hand-off and are harmless, because the render thread reads `j->n` and the job's buffers.
   - **`d3d12_fifo_flip` before the call.** It reads `s_d3d.draws` (10389-10390) before render_frame, which is the current list either way.
   - **The ticker present.** It returns at 13018-13019 once framed presents start (10396). Before that, it renders inline after a join.
   - **The window and overlay setters.** `rsx_d3d12_set_borderless` posts to the window thread (12981). `rsx_d3d12_set_overlay` uses Interlocked flags (12972-12975).
   - **The UI's back-buffer access.** The UI gets the back buffer only as the frame hook's argument (H:77-78); port/ never calls `rsx_d3d12_back_buffer` (grep). The release hook drops it before `ResizeBuffers` (1307), on the same thread.

## 7. As built and measured (2026-10-21)

Built as planned: Step 1 and Step 2 as two commits on a local ps3recomp branch, `perf/rf-thread` (`662078c`, `012dee7`). A copy of the patches is in `work/tmp/scratch_20261021/rf/`, which is not in the repository. Step 0 was not run separately: the `zq` counter answered it (no frame stayed inline).

**Parked, not merged.** On this PC the render thread does not make the deep Palace faster: the three frame threads are all close to a full core, and image 2 sets the frame. It is headroom for when image 2 gets faster (docs/78, section 4).

### 7.1 Live run (`GCM_RATES=1`, deep Palace, presenting)

`rf thr=` matched presents per second (138-155). `inl=0 zq=0 join=0.0ms hand=1.1-1.3ms` per second, and no `[RF]` lines.

### 7.2 Regress (knob 1, against the good set)

| Rate | s1_spawn | s2_walk | s3_turn | s4_on |
|---|---|---|---|---|
| 30 fps | ok, 1.7% | ok, 1.8% | ok, 1.8% | ok, 0.2% |
| 240 fps | ok, 0.6% | ok, 2.8% | ok, 9.5% | ok, 13.4% |

### 7.3 Frame rate (`exeab.sh`, deep Palace save, uncapped)

| A/B | Rounds | Profile windows, A → B | Unperturbed (between windows), A → B |
|---|---|---|---|
| Knob 0 → knob 1, not presenting | 3 | 140.4 → 148.4 (+5.8%, faster in 3 of 3, counts) | 149.3 → 151.1 (+0.7%, inside the bar) |
| Knob 0 → knob 1, presenting | 4 | 136.6 → 144.6 (+6.4%, faster in 4 of 4, counts) | 143.3 → 147.9 (near the 144 Hz display cap) |
| Master → knob 0, not presenting | 3 | 147.3 → 146.0 (-1.3%, slower in 3 of 3, inside the bar, p 0.06) | 157.7 → 155.7 (-1.6%, inside the bar) |

- An earlier master → knob 0 batch read -4.5%. It was dropped: other load on the PC (a game and a bursty background process) landed on the knob-0 side of every round.
- The profiler loads the FIFO thread. That is where the thread pays: about 6% more frames. Unperturbed, the frame waits on image 2 instead.
- The knob-0 path may cost about 1%, which other titles would pay too. That was one more reason not to open the pull request yet. Look at it (the two draw lists, the job copy) before this comes back.

### 7.4 Where the time goes (console `cpu 5`, not presenting)

| Thread | Master | Render thread and `SPU_COH_FAST` (docs/78) on |
|---|---|---|
| FIFO thread | 99.4% | 85.6% |
| HighGraphics | 98.8% (10% in its usleep) | 97.2% (15% in its usleep) |
| Image 2 | 91.9% | 93.8% |
