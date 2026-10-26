# What the three frame threads wait on (Palace, prof4)

Written on 2026-10-19 by a static-analysis agent for docs/56 (section 7). The inputs below were in that session's scratch dir and are gone; the source of the stack is the local ps3recomp branch `scratch/perf-stack2`. The first item of the ranking became `FIFO_PUT_WAKE` (ps3recomp #79); its A/B is in docs/56 section 6.

This is a static reading only. Nothing was built or run for it.

**Inputs**

- `$P` = the prof4 profile run (session scratch, gone): exe `perf2/exe/stack2` (md5 5233bde805bd), `RSX_PRESENT=1`, deep Palace save (`summary.txt:1-2`).
- `$W` = the ps3recomp worktree of `scratch/perf-stack2`: the source of that stack.
- `docs/` = the port repo's docs.

**Conventions**

- "Inferred" marks anything that is not read straight from code or from a profile file.
- The median frame is 7.204 ms (`chain.txt:3`).
- Profile shares are fractions of the thread's own samples. The FIFO thread has 3,583 samples over 6 s (`prof_fifo.txt:2`). 1% of a thread is about 0.07 ms a frame (inferred).

## Short answer

1. **The FIFO thread blocks about 10% of the time, not about 20%.**
   - The `.sym` listing prints `ntdll+0x161DC0` and `ntdll+0x161DD4` at 10.3% each (`summary.txt:86-87`). They are the same samples: one is the function row, the other the address row.
   - The 10.3% is one wait: the kick wait at the top of the ticker loop, `WaitForSingleObject(kick, 1 ms)`.
   - The kick event is never set in Demon's Souls (0 `kick` marks in `f.trace`). Every wait therefore runs to its timeout.
   - The walker reads `put` once per drain. So it sleeps even while image 2 has more segments ready. The sleep is stale, not real starvation.
   - The GPU costs 0.2% of the thread. The helper (vpj) spin costs at most 1.4%.
2. **The frame is latency-bound, but on the HighGraphics and image 2 hand-off, not the FIFO thread.**
   - HighGraphics waits 1.354 ms a frame for image 2's reply to command 35.
   - Image 2 then idles about 1.2-1.45 ms at the start of the next frame, until HighGraphics writes its first ring command.
   - These two waits sit end to end, so about 2.6-2.8 ms of each 7.2 ms frame runs only one of the two producers (inferred).
   - The FIFO thread trails by 4.4 ms. In the median frame no guest thread waits on it. It reaches HighGraphics only through the per-frame cond wait, in some frames (`hg_cond` median 0.024 ms, mean 0.177 ms).
3. **Ranking:**
   1. Wake the walker when the guest changes `put`, and re-check `put` before sleeping. Cheap and low risk.
   2. Wait on the reply word instead of the 64-poll timer fallback.
   3. Render thread.
   4. Write-back helper.
   5. Image 2 "earlier": there is no earlier start to take.
   6. Frames in flight: about zero.
4. **Measurement caveat.**
   - `RSX_PRESENT=1` runs present with vsync on: `RSX_VSYNC` defaults to 1 and no perf script sets it.
   - The display is 143 Hz (`docs/28-in-game.md:563`).
   - prof4's gap rates of 142.9 and 145.6 fps (`summary.txt:8`) sit at that refresh.

## Q1. Where the FIFO thread blocks

### The loop (`$W/runtime/ppu/tests/boot_main.cpp`, `vblank_ticker` at 586-734)

Each pass of the loop:

1. **Kick wait** (646-648): `cellGcm_fifo_kick_wait(FIFO_KICK_MS = 1)`. It does `WaitForSingleObject(ev, ms)` (`$W/libs/video/cellGcmSys.c:1597-1604`; `GCM_FIFO_KICK` is read at 1600).
2. **Vblank loop** (652): skipped, because `GCM_VBLANK_THREAD=1` is the default (512-517). A separate thread, `vblank_clock` (534-584), ticks the vblank and runs the flip handlers.
3. **Drain** (679): `cellGcm_rsx_process_fifo()`, which takes an SRW lock (`cellGcmSys.c:1639-1644`) and walks.
4. **Message pump** (700): `rsx_pump_messages()`.

The timer resolution is 1 ms: `timeBeginPeriod(1)` at `boot_main.cpp:1988`.

`render_frame` runs inside the drain:

- At a flip marker, the walker calls `g_gcm_fifo_flip_hook` (`cellGcmSys.c:1851`). The hook is `d3d12_fifo_flip` (`$W/libs/video/rsx_d3d12_backend.c:10382-10394`, registered at 12837), which calls `render_frame`.
- The profile agrees: inclusive `d3d12_fifo_flip` 22.5% and `render_frame` 22.8%, both under `gcm_rsx_process_fifo_unlocked` at 84.1% (`prof_fifo.txt`, inclusive table).

### Blocking calls, per frame

| # | Call | Code | Share of FIFO samples (`prof_fifo.txt`) | Class |
|---|---|---|---|---|
| 1 | Kick wait, 1 ms timeout | `boot_main.cpp:646-648`, `cellGcmSys.c:1597-1604` | `ntdll+0x161DC0` 10.3% exclusive; `KERNELBASE+0x1BDF0` 10.4% inclusive; exe caller `vblank_ticker` (rva 0x10DC3710) 10.6% | Starved by the code's test, but stale (below) |
| 2 | `Present`, sync interval 1 (`RSX_VSYNC` default 1) | `rsx_d3d12_backend.c:10148-10160` | `dxgi+0x13720` 1.8% inclusive (CPU work and any wait together) | Display |
| 3 | `wait_for_gpu` at every `render_frame` entry: fence, then `WaitForSingleObject` in 2 s steps | 8401, defined at 2194-2226 | rva 0x10E7D380 0.2% as exe caller of a DLL leaf | GPU |
| 4 | `move_to_next_frame`'s back-buffer fence wait | 10168, defined at 2235-2238 | Not separable | GPU. It should not block, since #3 drained the queue at frame start (inferred) |
| 5 | `vpj_sync`: runs the unclaimed vertex jobs, then spins `while (s_vpj_done < s_vpj_pub) YieldProcessor()` | 8378, defined at 10913-10919 | rva 0x10EA5650: 1.4% exclusive (spin plus inlined job work), 6.3% inclusive, 1.5% DLL leaf | Helper (a busy wait, so tslack counts it as busy) |
| 6 | `tcj_dispatch`: spins until last frame's `tex_csum` workers leave (normally none left), then wakes 2 workers | 4667-4693 | Not in the top rows | Helper |
| 7 | Other kernel calls: `ntdll+0x162B50` (a syscall stub; its samples sit at +0x14, 0x162B64); win32u stubs at +0x3FF0, +0x5F10 and others | Message pump `boot_main.cpp:700`. Condition-variable wakes at `rsx_d3d12_backend.c:10906-10909` and 4693 (inferred). D3DKMT calls under `dxgi` (inferred) | 0.9% + about 1.6% | Not attributed |
| - | SRW lock around the drain (`cellGcmSys.c:1639-1644`); any `ntdll+0x1659B0`-class wait | | Below 0.3% (not in the top 40 rows) | None |

How the thread's time adds up: busy 88.1% (`summary.txt:15`) plus kick wait 10.3% is about 98%.

### The kick wait in detail

**Nothing sets the event.**

- The only `SetEvent` is `cellGcm_ref_on_poll` (`cellGcmSys.c:1634`, with its mark at 1635). It fires only when a read of the ref register finds the fence queue empty.
- `f.trace` has 0 `kick` marks over 2 s, against 269 `flipword` marks.
- The comment at `boot_main.cpp:638-645` already says that a title that writes `put` directly never kicks.
- Image 2 writes `put` by MFC PUT (`docs/48-spu-ring-loops-native-plan.md:275`; `docs/28-in-game.md:720`). So every wait is a full timeout of about 1 ms.

**The walker uses a stale put.**

- `put` is read once per drain (`cellGcmSys.c:1687`, with an acquire fence after it).
- The loop is `while (s_fifo_getoff != put && budget-- > 0)` (1773; the budget is 0x100000 words, 1768).
- Nothing re-reads it. The only other assignment, at 2359-2361, is the title-ring recycle.
- So when the walker reaches the snapshot it returns, and the next pass sleeps the full timeout.

**`f.trace` shows a sleep with work waiting.**

- Segment 526BE was released at 4758.4 µs and 526BF at 5676.3 µs (lines 4032, 5513): 918 µs apart. The other releases in that stretch are 20-460 µs apart.
- HighGraphics flipped that frame at 5084.5 µs (line 4045). It flips only after image 2's reply (`docs/28-in-game.md:597-603`), so by then image 2 had written the whole frame.
- The walker still had 526BF-526DA ahead of it (28 segments) and reached the flip at 9193.3 µs (line 7376).

**The walker is always behind.**

- The lag is 4.446 ms median (`chain.txt:3`).
- At HighGraphics' flips, the walker trails image 2 by 19-29 segments (Q2).
- So nearly every sleep is stale (inferred).
- The sleeps add up to 10.3% and each lasts about 0.75-1 ms, so there is about one sleep a frame (inferred).

**Flip break.**

- After a flip marker, the walker returns early only when less than 64 KB is queued (`GCM_FIFO_CATCHUP_BYTES`, `cellGcmSys.c:850`; break at 1861-1863).
- `chain.txt` column `lbl` (present to the next 0xFF0 label write) is 0.007 ms median and 0.311 ms mean.
- In most frames the walker runs straight on. For example, FF0 was written 3.9 µs after the present at 4297.9 µs (`f.trace` lines 4012-4014).
- In some frames it returns and sleeps first (inferred from the mean).

**Classification.**

| Cause | Share of FIFO samples |
|---|---|
| Starved for commands (stale) | 10.3% |
| Waiting on the GPU | 0.2% (plus at most 1.8% inside Present, mostly CPU) |
| Waiting on a helper | at most 1.4% (a spin) |
| Unattributed kernel calls | about 2.5% |

## Q2. The hand-offs

### One stretch of `f.trace`

| t (µs) | Thread | Event | `f.trace` line |
|---|---|---|---|
| 4297.9 | FIFO | Present of frame 30F8 | 4012 |
| 4301.8 | FIFO | Label 0xFF0 = 0x3111. No break at the flip | 4014 |
| 4758.4 | FIFO | Releases segment 526BE, then the kick wait | 4032 |
| 5084.5 | HighGraphics | `_cellGcmSetFlipCommand`: end of frame 30F9 | 4045 |
| 5676.3 | FIFO | Wakes and releases 526BF | 5513 |
| 5909.4 / 6534.2 | Image 2 | Naps on ring line 0x01B51080, then wakes on HighGraphics' first command | 5541, 6007 |
| 9193.3 | FIFO | Flipword 30F9 (lag 4.11 ms), then `render_frame` | 7376-7377 |
| up to 11593.6 | HighGraphics | `usleep(30)` polls, about 30.1 µs each, waiting for the reply | 10728-10749 |
| 11225.7 | FIFO | Present of 30F9 (`render_frame` took 2.03 ms) | 10732 |
| 11624.2 | HighGraphics | `SetFlipCommand`: end of 30FA | 10750 |
| 11630.8 | HighGraphics | `sys_cond_wait` on cond 2 for 750 µs | 10850 |
| 12312.1 | Image 7 | Wakes on line 0x477FD980 | 10767 |
| 12328.4 | Guest thread 11 | Signals cond 0xD. It had polled `cellSyncMutexTryLock` on 0x477FD800 since about 9.2 ms | 7393-7480, 10777 |
| 12341.4 / 12371.9 | main | Its cond 0xD wait ends (2,858 µs); it signals all on cond 2 | 10791, 10846 |
| 12381.1 | HighGraphics | Released. Image 2 then naps 12392.4 to 13622.9 waiting for commands | 10850, 10872, 12478 |

### HighGraphics to image 2: the reply barrier

**Protocol.** HighGraphics posts command 35 and polls the reply word at 0x01B50FD0 (`docs/28-in-game.md:597-603`). Image 2 PUTs that word from pc 0x0C408 (`docs/28-in-game.md:600`).

**The polls.**

- Each poll is `usleep(30)`: a 30.0-30.2 µs QPC spin around `SwitchToThread` (`$W/runtime/syscalls/sys_timer.c:371-376`; `f.trace` 10728-10749).
- There are 11,932 such calls over 267 frames, about 45 a frame (`f.trace`).
- `hg_us` is 1.354 ms median and 1.491 ms mean (`chain.txt:3-4`): 20.4% of HighGraphics' time (`waits.txt`).
- `ntdll+0x1623C0` holds 16.2% of HighGraphics' samples (`prof_hg.txt`). It all sits under `sys_timer_usleep`, which is the exe caller of 22.2% of DLL-leaf samples.
- The poll period sets the wake delay: at most 30 µs, about 15 µs on average (inferred).

**The timer fallback.**

- After 64 back-to-back calls, the rest of a poll block sleeps on the high-resolution timer instead of spinning (`sys_timer.c:330-334, 341-349`).
- A normal block is about 45 calls, so only blocks longer than about 1.9 ms take this path.
- `f.trace` has 88 such calls in about 60 of 267 frames. Most last 500-525 µs, and a few up to 1,304 µs.
- Example: in the 8.6 ms frame from 11624 to 20256 µs, the 65th poll (19388.0 µs) slept 867.3 µs, and HighGraphics flipped 0.6 µs after it ended (`f.trace` 18087-18088).
- The FIFO thread's own timeout expired on the same tick (segment 52703 at 20252.2 µs, line 18086).
- The reply landed somewhere inside that sleep (inferred).
- Cost: the 88 calls total about 44 ms over 1,992 ms. Roughly half of that is overshoot past the reply, so about 0.05-0.1 ms a frame on average, concentrated in the slow frames (inferred).

### Image 2 to HighGraphics: idle at the start of each frame

**Where it naps.** All 284 of image 2's naps are on ring line 0x01B51080 (`f.trace`; 284 of 284). It waits for HighGraphics' commands and never sleeps on an RSX label.

**When.** In each frame it polls for about 760-830 µs after HighGraphics' flip, then naps for 360-640 µs until the first command arrives:

| HighGraphics flip (µs) | Nap starts (µs) | Wakes (µs) |
|---|---|---|
| 5084.5 | 5909.4 | 6534.2 |
| 20255.9 | 21087.0 | 21566.8 |
| 26613.0 | 27371.3 | 28008.0 |
| 34350.3 | 35123.3 | 35608.0 |
| 41031.0 | 41840.3 | 42203.4 |

**How long.**

- That is about 1.2-1.45 ms of idle a frame (inferred).
- `img2_nap` (0.547 ms median) counts only the nap part.
- SPU_RATE polling of 17-18% (`summary.txt:54-56`) is about 1.25 ms a frame.

**Caveat.** These times are measured from HighGraphics' last ring write, which follows its flip (`f.trace` 20258-20294). So they do not date image 2's reply.

### Image 2 to the FIFO thread: segments, `put`, label 0x81

**How image 2 hands over commands.**

- Image 2 writes the RSX put register only when it switches 32 KB segments, and the value points at the closing words (`docs/48:22, 274-276`).
- Command 35 flushes without writing `put` (`docs/48:283-288`).
- A frame is about 31 segments (526BC-526DA between flipwords 30F8 and 30F9, `f.trace`).

**Is image 2 ever held back by the walker?**

- At a switch, image 2 waits on label 0x81 until the RSX has released the segment it is about to reuse (`docs/48:279-280`).
- Its lead over the walker at HighGraphics' flips: 28 segments (526DA against 526BE), 29 (526F8 against 526DB) and 19 (52716 against 52703) (`f.trace`).
- The lead has no fixed ceiling, and image 2 never naps on a label line. So the walker did not hold image 2 in these frames, and the pool has at least 30 segments (inferred).

### The FIFO thread to the guest

**HighGraphics' cond wait.**

- In prof4, the only HighGraphics wait that could depend on the walker is `hg_cond`: cond 2, main's per-frame request (`synctop.txt:12-13, 17`).
- It is 0.024 ms median and 0.177 ms mean. 59 of these waits last 100 µs or more over 267 frames (`f.trace`).

**One long case traced.**

- The 750 µs wait above traces to main, then guest thread 11, then image 7's SPURS work, not to the walker.
- Image 7 woke 1.09 ms after the previous present. Whether that job waits for the flip is not shown (open).

**Present timing.** Presents of frame N land 0.40-3.23 ms before HighGraphics flips N+1 (6 frames in `f.trace`). Only the 0.40 ms case is followed by a long cond wait.

### How much of the 7.2 ms frame is serial

| Wait in the median frame | Time | Share of the frame |
|---|---|---|
| HighGraphics idle on image 2 | 1.354 ms (`chain.txt`) | 19% |
| Image 2 idle on HighGraphics | about 1.2-1.45 ms (polls plus the 0.547 ms nap) | 17-20% |

These two sit end to end at the frame boundary:

1. Image 2 finishes its tail.
2. It replies, and HighGraphics flips.
3. HighGraphics runs its pre-command phase: about 1.2-1.45 ms of work with no ring output (inferred from image 2's wake times).
4. HighGraphics writes its first command.

So about 2.6-2.8 ms of each frame (36-39%) runs one producer alone. The two work together for the other ~4.4 ms (inferred).

No thread is saturated by real work:

| Thread | Real work |
|---|---|
| HighGraphics | 79% (`hg_work` 5.674 of 7.204) |
| Image 2 | about 72% (89.7% busy minus about 17.5% polling; `summary.txt:14, 54-56`; inferred) |
| FIFO thread | about 88% (`summary.txt:15`) |

The frame is set by HighGraphics' pre-command phase plus image 2's conversion of the frame, with the reply acting as a barrier. That is a latency limit, not a throughput limit.

The FIFO thread overlaps both producers:

- From flip request to present takes 6.2 ms (lag 4.446 + `render_frame` 1.780).
- Its 0.74 ms sleep is off HighGraphics' path in the median frame.
- It enters that path only through `hg_cond`, in some frames: mean minus median is about 0.15 ms a frame (inferred).

### Against docs/56 ("the FIFO thread holds the frame on the stack")

- docs/56 (lines 888-894, 921-922 and 955) rests on stackk1: with all FIFO cuts off, the frame rate drops 6.7% (+0.56 ms a frame). That shows that more FIFO work binds the frame.
- prof4, with the cuts on, shows the median frame waiting on image 2.
- Both fit a FIFO thread at the edge.
  - Its only slack is the stale sleep, about 0.75 ms a frame.
  - The drain gives that slack up as the lag grows: when it is further behind, each drain covers more and returns less often (inferred).
  - So extra FIFO work first costs lag. It costs frames once lag plus `render_frame` plus the flip path passes HighGraphics' frame (inferred).
- A knob A/B of rank 1 below would settle it.

## Q3. Overlap candidates, ranked

| Rank | Candidate | Cost | Risk | Expected gain (inferred unless cited) |
|---|---|---|---|---|
| 1 | **Wake the walker on a guest `put` change, and re-check `put` before sleeping** | About 20-40 lines plus a live knob | Low | Removes the stale sleep: 0.6-0.75 ms a frame of FIFO idle (10.3%, `prof_fifo.txt`) and about 0.7 ms of lag. Frame rate: 0-3% at prof4's balance (the `hg_cond` frames); up to the full 0.75 ms (about +10%) wherever the FIFO path binds (docs/56's reading) |
| 2 | **HighGraphics' reply wait as a wait on the reply word** (guest state), replacing the 64-poll timer fallback | About 1 day | Low to medium: a missed wake must fall back to the timeout | About 0.05-0.1 ms a frame on average (about 1%), more in the slow ~22% of frames (1% lows). Also frees HighGraphics' spin (16.2% of its samples). Shorter polls alone save at most ~15 µs a frame |
| 3 | **`render_frame` on a render thread** (docs/56:946, stage 1) | 2-3 days, ~300 lines | Medium: races show as flicker or missing draws | 1.0-1.1 ms a frame of FIFO time (docs/56:946; `rf` is 1.78 ms here). Little extra frame rate on top of rank 1 at this balance; it is headroom for when HighGraphics or image 2 get faster |
| 4 | **`rt_writeback_store` off the FIFO thread** | 1-2 days | Medium to high: the guest and the texture hash read those pages | 0.35-0.45 ms a frame of FIFO time (4.9% exclusive, rva 0x10EA5820; docs/56:938). Headroom only, like rank 3 |
| 5 | **Image 2 "earlier"** | - | - | Not available (details below) |
| 6 | **Frames in flight** | 1-2 days | Medium: torn geometry (`rsx_d3d12_backend.c:8393-8398`) | About zero. The GPU wait is 0.2% (rva 0x10E7D380), Present at most 1.8% |

### Notes

**Rank 1: wake the walker on a `put` change.**

- *SPU side.* Image 2's put-register write is a 4-byte MFC PUT to the control register (`docs/48:275`).
  - Every SPU PUT already reaches `gcm_note_spu_put` through `g_spu_put_hook` (`cellGcmSys.c:511-514, 1357-1373`). It returns early for EAs outside the IO mappings.
  - A test of whether the PUT covers `GCM_CONTROL_GUEST_ADDR`, placed before that early return, plus `SetEvent(s_gcm_kick_ev)` while the walker waits, is a guest-state trigger that costs nothing.
- *PPU side.* HighGraphics' own put write at the flip goes through the HLE flip path (`cellGcmSys.c:2677-2706`), which can set the same event.
- *Inside the walker.* When it reaches its snapshot, it should re-read `put` (with the acquire fence, as at 1687-1694) and carry on if `put` moved. Without the event, a walker that catches up would still sleep 1 ms.
- *Not a repeat of a dead end.* docs/28:720 (`FIFO_SPIN_US`, a 500 µs spin, worse at the 30 fps balance) was a spin. This is not, so it does not repeat that dead end.

**Rank 2: wait on the reply word.**

- *How.* Put an insn hook at HighGraphics' poll site (the callers of `func_00B5D8D8`, docs/56:254), or give it a usleep variant that waits on the line holding 0x01B50FD0. Image 2's PUT of the reply (pc 0x0C408) wakes it through the same SPU PUT hook. Keep the timeout as a safety net.
- *Dead-end check.* docs/56:266-267 lists the "event-driven reply wake" class as not worth retrying. That verdict came at the base balance, and the timer overshoot shown above was not part of it (inferred).

**Rank 4: the write-back is already a frame late.**

- `s_wb_pending` runs it at the next `render_frame` entry, after `wait_for_gpu` (`rsx_d3d12_backend.c:8401-8404`). So the guest already tolerates a frame of delay (inferred).
- A helper started at the GPU fence would make it earlier and take it off the FIFO thread.
- It must finish before the walker's texture uploads hash the same pages.

**Rank 5: image 2 already starts as early as it can.**

- Image 2 wakes on HighGraphics' first ring write (the wakes in Q2). Its idle time is HighGraphics' pre-command phase, which the guest's reply barrier serializes.
- The real lever is image 2's conversion speed: docs/48 Stages 2-3, at +1-2% on the base per docs/56:228. Both have since merged (ps3recomp #87 and #91, docs/72).
- The bi-switch did not show on the stack at first (docs/56:893-894); a later stack run measured it, and it merged (ps3recomp #73, port #83).

## Measurement caveat: vsync at `RSX_PRESENT=1`

- **What the script sets.** `perfprof.sh:44, 52, 59` sets `RSX_PRESENT` from the lock state and runs with `DES_FPS=60 VBLANK_MHZ=960000`.
- **What it leaves out.** No file in `lanes/perf/*.sh`, `perflib.sh`, `perfprof.log` or `batch.txt` sets `RSX_VSYNC`.
- **The default.** `RSX_VSYNC` defaults to 1, so it presents with sync interval 1 (`rsx_d3d12_backend.c:10148-10160`).
- **The display.** 143 Hz (`docs/28-in-game.md:563`; `docs/31-ac4-acfa-ports.md:115`).
- **The evidence.** prof4's gap rates of 142.9 and 145.6 fps (`summary.txt:8`) sit at that refresh. The profile windows, at 135-139 fps (`summary.txt:5-7`), are below it, and Present is at most 1.8% of FIFO samples. So prof4's profile is not distorted much.
- **The consequence.** An A/B with `RSX_PRESENT=1` cannot show a gain past about 143 fps (inferred). Use `RSX_VSYNC=0` (it says so once if tearing is unsupported, 10150-10153) or `RSX_PRESENT=0`. This is the same trap as the 120 and 240 vblank caps (docs/56:76-79).

## What would settle the open points

1. **Date the reply.** Add a trace mark when image 2 PUTs the reply word (in the SPU PUT hook). It dates the reply exactly, which measures the timer overshoot and HighGraphics' true wait.
2. **Count stale sleeps.** Add marks at the kick wait's entry and exit, with `put - get` re-read at entry. It counts the stale sleeps per frame directly.
3. **Find what image 2 polls.** Split SPU_RATE's polling by lock line (ring line or label line). It shows whether any of image 2's 17-18% polling waits on label 0x81.
4. **Measure rank 1.** Run rank 1 behind a live knob, in `perfknob.sh` ABBAAB windows with `RSX_VSYNC=0`. It decides whether the FIFO path binds at this balance.
