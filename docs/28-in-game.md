# 28 — Into the first level: name entry, level load, the in-game hang

Picks up from docs/27 (title screen reached). The port now goes from the title through the save load, character name entry and the first level's load, and keeps running in the level. What it draws there is still wrong (a grey wedge over black), so it is not yet known whether this is the opening cutscene or gameplay.

All fixes were tested on Demon's Souls only. The other titles (VF5, Simpsons, Tornado Outbreak, Tokyo Jungle, Midway Arcade Origins) were not re-run.

## Commits (ps3recomp unless noted)

| Commit | What |
|---|---|
| `e4241f9` | cellOskDialog posts LOADED/FINISHED/UNLOADED and takes the text on the host (small input window; `PS3_OSK_TEXT=<utf-8>` answers at once for scripts). Name entry works. |
| `c8a42de` | Closing the game window ends the process. |
| `9f1bb06` | Lifter: switch tables whose base was spilled to the stack. `func_01275EC0` (a 0x6C-case bytecode interpreter run during the first level load) lost a case, so every object built after it had a null vtable, and the game froze behind the loading screen after character creation. |
| `1470b82` (diag) | The d3d12 PERF line counts vertex-program compiles. |
| `4e01d53` | d3d12: stop recompiling shaders every frame in 3D scenes (VS cache key, eviction). First area went from under 1 fps to about 13. |
| `2619289` (diag) | `SPURS_TS_TRACE`, `SPU_PUT_WATCH`. |
| `b27bd49` | SPU interpreter runs the double-precision ops (image 6 stopped on its first `dfm`). |
| `1a28ab1` (diag) | `SPU_GET_LOG`, per-port drop counts for SPURS events, port-disconnect log. |
| `d5aeb3d` | **The in-game hang**: SPU lock-line commits break PPU reservations on the whole 128-byte line. |
| `b838932` | SPU events on an unbound port: consume the data word, reply ENOTCONN (ports 12/14). Reverted in `fbb89a9` (it stopped the level load), re-applied after the real fix (below). |
| `760b682`, port `20b24dd` | `DES_SKIP_INTRO=1`: shorter boot logos (below). |
| `8023619`, `a443ca2` (diag) | Console `setenv K=V` (through `cached_setenv`, so the cached knob changes too) and `guard <hex> [rw]` (arm the page guard on a live heap word); `PPU_GUARD_CHANGE=<n>`. |
| `fe203a5` | SPURS: synthetic completion events stay off queues that have a real SPU producer. This was the semaphore waiter runaway behind the port 12/14 sends. |
| `703489e` | Re-applies `b838932` (ENOTCONN on unbound ports). The load stall it was blamed for is intermittent and has another cause. |
| `7c23229` | PPU `lwarx` reservations cover the whole 128-byte line, as on Cell. The intermittent level-load stall went from 2 runs in 6 to 0 in 6. |
| `31b8f27` (diag) | `RTT_SAVERT=<hex>` saves that render target once per new value (`rt_save_<off>.bmp`). |
| `513aca3` | d3d12: the vkcube projection fallback is opt-in (`VP_GUESSPROJ=1`). It replaced Demon's Souls's real projection (c00 = 300) and flattened the level. |
| `62a073f` | d3d12: bind all 16 fragment texture units. The level shaders sample t4 and t7, which were outside the root signature, so their pipelines failed (`fp=0x014AD301` and one more). |
| `f7a0674` | RSX level shading: per-draw fragment constants (buffered b1, so each draw keeps its own patched constants), TC8/TC9 vertex outputs, primitive restart (`0x1DAC/0x1DB0`) and strip winding in the indexed strip path. The cyan and the stretched triangles are gone. |
| `5b4ce53` (diag) | `FPK_FP`, fragment-program knobs (`FP_KILL`, `FP_ONE`, ...) rebuild pipelines live from the console, `VP_ATTRS_FP=<hex>` dumps that vertex program as `vp_pick.hlsl`. |
| `f593fcc` (diag) | `RSX_CENSUS=<n>`: per-frame count of draw methods in the FIFO, plus every method left unhandled. |
| `ae13875` | **The frozen player and the missing map**: SPU user events go through ports bound to the thread (`sys_spu_thread_connect_event`) before the group's. MultiStream's replies were being dropped, so no sound ever started, the level-entry sequence waited for a voice line forever, and Havok was skipped every frame (below). |
| `b2ec5eb` (diag) | Sync-history ring and console `synchist <id>`; console `ctx <tid>` (live registers; r11/r3 of a parked thread name the syscall and its object); console `spu` (runs per lifted image); `HLE_LR=<nid>`, `SEM_LR=1`. |
| `f4dbfbd` (diag) | `GCM2D_WATCH=<hex>` logs inline (`NV3089`/`NV308A`) transfers that touch a guest range; console `find <addr> <len> <hexbytes>`. |
| `a296b91` | d3d12: render targets replaced while a frame is recorded (same offset, new size or format) are released after that frame finishes, not at once. Releasing them early removed the device (`0x887A0005`) once write-back was on. |
| `c403bd9` | d3d12: **colour write-back**. Offscreen render targets are copied back to guest memory after each frame (`RSX_WRITE_COLOR=0` turns it off). The surface's context DMA (`SET_CONTEXT_DMA_COLOR_A-D`) is now tracked. DeS's exposure constant went from 0 to the computed value (below). |
| `7c94369` (diag) | `D3D12_DRED=1` (removal reason, breadcrumbs, page fault, without the debug layer); `RSX_WRITE_COLOR_LOG=<n>`. |
| `00d9e68` (diag) | `TEXFMTDBG` logs every bound texture format, including the ones the backend then skips. |
| `42d79ca` | d3d12: sample every texture format the layout code decodes (one shared format table for upload and view). The level's environment cube maps (`t11`) were skipped, so their lighting term was 0. Depth formats are still left out. |
| `1043fc1` | Fragment decompiler: per-source precision modifiers (fx12/fx9/saturate, bits 19-27), `abs` on source 2, the destination clamp for fx12/fx9 writes, and saturated `COL0`/`COL1` reads, in RPCS3's order. The fog factor `tc2.w` is saturated now, so the level colour stopped going negative. |
| `8caa9c0` | d3d12: a draw that samples its own render target reads a copy taken just before the draw. DeS's exposure adaptation reads the previous exposure from the target it writes; in D3D12 that read was 0, so the exposure stayed one step from 0. |
| `9d0c9c0` | d3d12: **the level is lit**. The vertex-program vertex buffer (393,216 vertices) filled partway through every level frame (about 1.06M asked for), and every later draw was dropped: the rest of the scene, the post chain and the final composite. It now grows when a frame overflows it (`VP_VB_MAX_VERTS` caps it, default 2M). |
| `1975632` (diag) | `FP_IDCOLOR=h` colours only the half-precision (scene) fragment programs. |
| `3470cbd` | RSX reset state: front face CCW and cull face BACK, as the hardware resets. With CW the floor and most walls were culled. |
| `4e229b7` | d3d12: a depth surface per guest zeta offset (kept across draws and frames), the guest's depth test/write/function per pipeline, and depth/stencil clears to that surface. The floor stopped drawing over the player. |
| `fb24382` | d3d12: **shadows**. Colour target 0 (the shadow cascades) binds no render target; depth-format texture units sample the depth surface at their offset with a comparison sampler (`SampleCmpLevelZero`), compare function reversed as on RSX. |
| `e58297f`, `d613b7f` (tools/diag) | Live knobs (below); manifest lines carry colour target, zeta, depth state and cull. |
| `e4aea1e` (tools) | Console `cpu [s]` (below). Guest main records its host thread id. PAD_STICK restarts only on a new value. |
| `0d8c4f2` (tools) | Frame capture and offline replay (below). |
| port `2d6b3ab`, `72c3bd8` (tools) | Runs launch a staged copy of the exe; `regress.sh`, `shotdiff.py`, `recomp_index.py` (below). |
| `b4821e0` | d3d12 + fragment decompiler: **depth read as colour**. An A8R8G8B8/D8R8G8B8 unit over a depth surface samples that depth and decodes it to the bytes RSX fetches; a draw that samples the depth it is bound to without writing it gets a read-only depth view. Light shafts and distance haze (below). |
| port `91c3cfd` (tools) | `REGRESS_AB=KNOB=value`: an A/B of one knob at the last stop (below). |
| port `f7ce830` (tools) | `des_kill_own` also finds staged copies started from any shell (below). |
| `ad4b9b0`, port `ce22c2b` (tools) | `AUDIO_MUTE=1` (live) sends silence at the normal rate; test runs default to it (below). |
| `b46dc86` (tools) | Console `waits [s] [rows]`; GCM_RATES counts present boundaries (below). |
| `0c18b0b` | `sys_timer_usleep`: a long sub-ms usleep sleeps on the high-resolution timer and spins only the last `USLEEP_SPIN_TAIL_US` (500). MOStreamingThread 50% -> 13% of a core. |
| `0861c1c` | Vblank ticker: the FIFO kick wait's ceiling is 1 ms, was 4 (`FIFO_KICK_MS`, live). About 13.7 -> 15.8 presents/s. |
| `bab5001` | SPU `fma`/`fms`/`fnms` in double instead of the CRT software `fmaf` (no FMA target). |
| port `2b3dda3` | The lifted SPU images build `/O2` with SSE4.1 (`DES_SPU_OPT`, default ON). |
| `a90fc9a` | cellAtrac **streaming** (GetStreamDataInfo/AddStreamData and the rest; host copy of each file). ATRAC decode errors per run: ~650 -> 0. Needs the NID table regenerated. |
| `ba6305d` | d3d12: **per-draw vertex fetch plan** and vertex cache (`VP_FETCH_FAST`, live). About 15.5 -> 28 presents/s; the game's pacer leaves 15 fps mode (below). |
| `e50e28c` (tools) | `AUDIO_WRITEPOS=1` (where the game's writes land relative to the published read index) and `AUDIO_GAPS=1` (blocks read before they were written; totals on the `AUDIO_RATE` line) (below). |
| `1e4d419` | cellAudio: **one block per 5.33 ms by the clock**, not in WASAPI-period pairs; read lag 1 (`AUDIO_LAG_BLOCKS`), MMCSS mixer, 40 ms buffer, AUTOCONVERTPCM. Silent blocks inside sound 46.6% -> 0.0% (below). |
| `4efcb5e` | cellPad: **DualSense / DualShock 4 over raw HID** (USB and Bluetooth); the keyboard merges into port 0 instead of switching off when a pad is plugged in (below). |
| `6dee83f` | Vertex fetch: CMP (type 6) packed attributes, signed 11/11/10 bits (from robbe1912's fork). |
| `27202ad` | Fragment decompiler: a predicated write is a per-component select, not a `lerp` (upstream PR 278). |
| `ee40816` | d3d12: the frame copy is bound to a display-sized sampler only when the guest memory behind it is empty. The movie frames were replaced by last frame's backbuffer (below). |
| `5a63029` | cellSail **plays PAMF movies**: MPEG-2 through the Windows Media Foundation decoder, LPCM, frames on the wall clock. The intro advert and the title movie play; START skips them. Needs the NID table regenerated (below). |
| `017dd06` (diag) | `[cellSail] video GetFrame #N` / `sound GetFrame #N` / `[sail_movie] decoded N` counters (below). |
| `7f48932` | cellSail movie **sound**: a 16-bit LPCM sample split across two PES packets is carried over. Decoding each packet on its own read everything after an odd-length one a byte off, which played as seconds of full-scale white noise. 5.1 tracks are folded to stereo, and the stream database reports 2 channels to match. The title movie is now bit-exact against the source (below). |
| `ad2d649` | d3d12: a stale frame-size max no longer merges light frames into one present (`RSX_STALE_FLIPS`, plus a halving rule). Movies went from 15 to 31 presents/s; the level is unchanged (below). |
| `96757fc` (diag) | `SAIL_AUDIO_TONE=<peak>` (test tones instead of the movie's sound) and `PS3_SAIL_MOVIES=<text>` (play only the movies whose path contains it) (below). |
| port `fde7885` (tools) | desprobe starts pressing on either first-poll log line. The "intro-path boot stall" was the harness: a garbled log line meant no button press (below). |
| `0dc0854` | cellSail `Graphics/SoundAdapterPtsToTimePosition`: the opening movie's subtitles no longer freeze (below). Needs the NID table regenerated. |
| `d57cea9` | sail_movie hands video frames out in order, half a frame early, instead of the newest due frame (below). |
| `6dae2e5` | Vblank ticks on the performance counter at 59.94 Hz (`VBLANK_MHZ`); they were 62.5 Hz in ~15.6 ms steps. The game flips 30/s, not 31-32 (below). |
| `12a61a8` (diag), port `359c66f` (tools) | F9 frame capture, re-armable `RSX_CAP`, live `RSX_DBG_VTX` (below). |
| `dc3bb6f` | d3d12: 1024 per-frame texture slots behind a hash index (were 96). The HUD vanished whenever the view was busy (below). |
| `5a01d21` | d3d12: the texel scale also reaches programs with a shadow unit, which name their samplers one by one. |
| `535fbda` (diag) | F9 bursts (`RSX_HOTKEY_FRAMES`), F9 never overwrites an earlier capture, `DRAW_SKIP=a-b`, `FP_TEX` fixed for per-unit samplers (below). |
| `c01c7f7` | Demon's Souls' flips now go into the command stream, and the d3d12 backend presents at them (`GCM_FLIP_CALLBACK`, `RSX_PRESENT_AT_FLIP`). Hitches no longer present half a frame without its post chain and HUD (below). |
| `3f094e1` | d3d12: a partial triangle carries across the index ranges of one BEGIN/END. Particles (sparks, spawn smoke, fog sprites) drew as slivers out of phase (below). |
| `191b145` (diag) | `RSX_DBG_VTX=fp:<hex>:<n>:<v>` prints v vertices of each matching draw. |
| `5a78630` | d3d12: indexed strips and fans carry across index ranges too. Holes in the level geometry (below). |
| `9db1a5f` | Fragment programs are scanned up to 64 KiB, not 4 KiB. The player and NPCs drew as black silhouettes (below). |
| `df9d0f9` | d3d12: ZPASS occlusion queries are counted (`RSX_ZPASS`, `ZPASS_LOG`). The sun flare showed through walls (below). |
| `0beda1a` (diag) | F10 present bursts, `CELLMARK_DUMP_NOW`, `RSX_FRAME_SLEEP`, `[RT]` pool logs (below). |
| `ea26191` | d3d12: 8192 draw records a frame, not 2048, and a log line when a frame has more. A dark room flashed black (below). |
| port `d0ebf14` (tools) | `regress.sh pin-save`: the walk starts from a pinned save, not the player's (below). |
| `7c2753d` | d3d12: 16384 draw records a frame; Boletarian Palace reaches 7529 (below). |
| `7769d7c` | d3d12: an offscreen target whose guest memory the title rewrote is dropped (`RSX_RT_REWRITTEN`, below). |
| `b039057` (diag) | F9 also saves the live presents after its capture, `cap_NNN_live_XX_d<draws>.bmp` (`RSX_HOTKEY_LIVE`, below). |
| `d4eb290`, `28c69a9` (diag) | `RSX_LATE_CHECK`, `TEX_VERIFY`, live `TEX_NOCACHE` (below). |
| `b73b4ef` | d3d12: sampler tables are reused once all 128 are taken (`RSX_SAMP_TABLES`). Bright colours wrapped to blue, magenta and grey in the Nexus (below). |
| `daa2792` (diag), port `tools/des/capdiff.py` | F9 writes per-draw input fingerprints of the captured live batch; `capdiff.py` lines them up with the replay's. `RSX_LATE_CHECK` covers vertex arrays and indices (below). |
| `5bf4527` | d3d12: the texture checksum reads every part of a compressed block (below). |
| `82a3ea6` | d3d12: texture slot SRVs moved out of the per-draw SRV windows (below). |
| `bd40e8d`, `886ed2e` (diag) | Fingerprints hash each slot's uploaded bytes; `rsx_replay` chains captures (below). |
| `7cc4a1c` (diag) | `FP_WATCH`: per-frame draw counts of chosen programs, empty depth reads, one-frame gaps (below). |
| `ec8ad33` | d3d12: a reused draw record's clear flags are reset. The soul-form aura, the fog wall and the first cutscene's fog flickered (below). |

## The in-game hang (fixed, `d5aeb3d`)

Symptom: about 30 s after the first level loaded the picture froze (grey), START did nothing, and the window title still showed an fps.

Where it stopped:

- MAIN waited on HighGraphics, so the pad was never read.
- HighGraphics was parked in `cellSpursQueuePopBody` (real libsre, import stub `0x016B9E84`), called from `FUN_014c3c18` / `FUN_014c3958`.
- The queue is at `0x47FC5080`:
  - word 0 is the pop count (stored as its complement when a PPU waiter sleeps), word 1 the push count;
  - +0x14 is the buffer (`0x48196110`, 250 records of 16 bytes, `{group, n}`), +0x18 the SPU port (17 → lv2 event queue 18, `throw_event`), +0x1C the direction.
- At the hang the queue held a record (pop = ~97, push = 98) and nobody was going to wake the popper.

The pushers are SPU tasks of SPU group 1 (image 5, `spu_0004`). The push wrapper is at `0x10B58`, the commit and notify at `0x11050`, the event throw at `0x11658`. Each push is a GETLLAR / modify / PUTLLC of the queue's line. It throws the event only if word 0 says a PPU is waiting.

The race:

1. The PPU popper does `lwarx` on the pop word, reads the push word, sees the queue empty, and prepares to store "waiting" into the pop word.
2. An SPU push commits the push word (PUTLLC on the same 128-byte line). It sees no waiter, so it sends no event.
3. The PPU's `stwcx.` is a compare-and-swap of the reserved word only. The pop word had not changed, so the store went through, and the PPU slept on a non-empty queue.

On Cell the reservation covers the whole 128-byte line, so step 2 kills it and step 3 fails and retries.

The fix:

- PUTLLC/PUTLLUC take the store-conditional slot locks of the line's eight 16-byte blocks around the compare and commit.
- On commit they drop every PPU reservation that falls in the line.
- The hooks are installed by `ppu_loader.cpp` (`g_ppu_resv_line_enter/break/leave`). `PPU_RESV_OFF` still disables reservation tracking.

Verified: still flipping at 150 s and at 226 s. Before the fix it hung at about 100 s from boot.

## SPURS event drops

- **Ports 12 and 14**: an SPU `send_event`s on ports nothing is bound to. lv2 consumes the outbound-mailbox data word and answers `CELL_ENOTCONN` in the inbound mailbox; we did neither (RPCS3 `SPUThread.cpp` does both). `b838932` added that reply, but with it the first level never left its loading screen (220+ s against about 110 s), so it was reverted (`fbb89a9`) until the cause was found.
- **Why those ports were used at all: the semaphore waiter runaway** (`fe203a5`).
  - The FX particle task (SPU image 7, `SpursTaskFXClusterDraw`) and the Havok PPU thread share semaphore words at `0x477FD880/900/980`. Layout: `[port:8][waiters:8][count:16]`, +4 self EA, +0x10 original port.
  - The SPU signal (LS `0x17700`-`0x17780`) does GETLLAR/PUTLLC on the word, then sends one event on port `word>>24` per waiter it wakes.
  - The PPU wait (`FUN_010d43e8`): if count is non-zero, take one; otherwise add 0x10000 (one more waiter) and `sys_event_queue_receive` on queue 5.
  - Queue 5 also got events SPURS never sends on hardware. Of the first 64: 37 job-chain completion broadcasts (`jc_signal_done`, chain `0x45B5DE00`, HighGraphics), 8 task-completion injects (source `0x53505500`), and 19 real port-3 events.
  - Each extra wakeup re-registered the waiter. The waiter byte climbed and carried into the port byte (`0x0E7C0000` at 68 s), so the SPU sent to ports 12, 13 and 14.
  - Filtering by SPURS instance does not separate them: Demon's Souls has a single instance (DLSpursManager `0x01B92E00`).
  - Fix: a queue that has received a real SPU `send_event` through a SPURS port gets no synthetic events from then on. `SPURS_SYNTH_ALL=1` restores the broadcast.
  - Verified: all 64 traced queue-5 events are real; the word holds `0x03000000` in the level at 120 s; the level still loads.
- **`write_spu_mb` drops**: 8 writes from libsre to thread 0 (a placeholder) when a taskset is recreated. Benign, left alone.

## Intermittent stall on the level's loading screen

Some runs never leave "Now Loading" for the first level (seen 2 times in 6 runs with `fe203a5` + `703489e`); the others reach the level about 11 s after its script archive opens.

- The loading screen keeps animating at about 26-29 fps, and every thread is blocked in an lv2 wait. In both stalls the guest stacks matched (only the audio threads differed).
- Core.Res (Decompressor, CacheDeviceHandler, FileLoader, TaskManager) are all idle, so no file request is pending. The HLE call sets of a stalled run and a good run are identical (`HLE_FIRST`).
- The console `threads` view shows each blocked thread's syscall (r11) and object (r3). MAIN: cond 0x2E. Core.Res: conds 5, 3, 6, 7. HighGraphics: semaphore 1. SLSession: semaphore 4. ChrInsMan / ChrProxy / FaceGenMan / DsTrophyMan: conds 0x27-0x2D.
- Hypothesis: a lost wakeup like the in-game hang, PPU against PPU this time. Reservations were tracked per word, but `lwarx` reserves the whole 128-byte line on Cell.
- `7c23229` makes PPU reservations line-wide (a store anywhere in the line kills them; `PPU_RESV_WORD=1` restores the old behaviour). With it, 6 of 6 runs reached the level, each about 11 s after the script archive opened. Before it, 2 of 6 stalled. This is evidence, not proof: the stall was never caught in the act.

Measuring it: the scratchpad script `loadstat.sh <tag> <runs> [KNOB=V ...]` boots N times and records, per run, how long after the script archive opens the window title reports 200+ draws (the loading screen draws about 125), then takes a screenshot.

## DES_SKIP_INTRO (port, `port/des_options.cpp`)

Off by default. The boot logos are the `DsMenuDlgLogo` dialog. Its per-frame update is the vtable method `0x008EF430` (state machine tail `0x008EEF48`), with these fields:

| Offset | Field |
|---|---|
| +0xF8 | `m_elapseTime` (float) |
| +0xFC | `m_curLogType` (0..4) |
| +0x100 | `m_animState`: 0 start fade-in, 1 wait, 2 hold, 3 wait fade-out |
| +0x104/0x108/0x10C | pre-delay done flag, duration, elapsed |

Timings and exit:

- The hold is 3 s for logo 0 and 5 s for the rest (pool floats `0x18FA550` / `0x18FA558`); each fade is 1 s, about 33 s in all.
- After the fade-out of logo 4, the dialog itself calls `FUN_00720F50(0x3C, 0)` to move to the next menu state.

With the option on:

- A logo about to fade in becomes the last one.
- A hold counts as already over.
- The game still plays one fade-in and fade-out and leaves by its own path.

How it is wired:

- ps3recomp's `boot_main.cpp` gained a weak `ps3_title_options_init()` hook, called right after the function table is registered.
- The port defines that hook and re-registers `0x008EF430`. The method is only reached through its OPD, so every call goes through the address map.

Measured: the first level's script archive opens at t=45 s instead of about 70 s.

How the strings were found:

- The debug names (`m_preDelayTime`, ...) are referenced only by `func_008EFD88`, the dialog's init, which registers the tweakables.
- Its constant pool comes from TOC slot `r2-0x6FF8`.
- The scratchpad tool `poolxref.py` tracks `rX = lwz(r2+T)` then `lwz(rX+O)` in the lifted chunks and finds which functions read a given pool slot. It works where Ghidra's xrefs find nothing.

## Graphics in the level

What one level frame does (from `RTT_DUMP`):

1. Shadow pass: three meshes (138, 762 and 471 vertices) into four 1024x1024 quadrants.
2. Scene into RT `0x7A0000`: the same meshes with `fp=0x1572C81` (t0 = `0x6F9AF80`), then `fp=0xB4FE81` (colour mask 8), a 1050-vertex mesh with `fp=0x1584081`.
3. Bloom and downsample chain (`0x5110000`, `0x5210000`, `0x4D60000`, 16x16 luminance targets).
4. About 200 six-vertex UI quads (`fp=0x79FE81`, likely text).
5. `fp=0x181` composites `0x7A0000` to the display.

Fixed so far:

- `513aca3`: the projection fallback for vkcube (`VP_FIXPROJ`'s guess) fired whenever c00 was outside 0..8. Demon's Souls's projection has c00 = 300, so its real matrix was being replaced. Now it is opt-in.
- `62a073f`: 16 texture units, so the two level pipelines build.

After `f7a0674` the frame is grey (clear colour plus smoke particles, `fp=0x1584081`) with a few blocky meshes. `FP_KILL=B40D01` (the final post pass) gives black.

### Why the map is not drawn (2026-10-14)

The map pieces are never sent to the RSX. Evidence:

- `RSX_CENSUS=60`: about 117 batches a frame (9 triangle lists, 12 strips, 96 quads), all `DRAW_ARRAYS` or `DRAW_INDEX_ARRAY`. No draw method is unhandled. So the FIFO does not carry the map, and the backend is not dropping it.
- Ruled out: FIFO resync, blocking `SEMAPHORE_ACQUIRE` (`GCM_SEMA_ACQUIRE=1` changes nothing), ZCULL reports used as visibility (DeS reads `GET_REPORT` types 2-5 at indices 2040-2043 only for adaptive ZCULL in `FUN_009f6ebc`), job-chain misses (`0x45B5DE00` only uploads textures), and the image 11 lift (image 11 is MultiStream's `_synth2`, not Havok; see the image list below).
- Rendering is camera-relative. VS `c[0..3]` is the view-projection without translation, `c[7]` = (0,0,0,1) is the eye, and `c[8+a0]` holds bone matrices whose translation is (world − camera).
- The camera is at world (204.6, 0.22, -97.9). The player start `c0000_0000` in the map's MSB is at (201.0, -1.1, -96.2). The meshes we see draw with `fp=0x1572C81`; one is `m7777B0`, at the origin with draw group `0x80000000`.
- MSB parts near the player (`m1020B0`, collision `h1020B0` at (204.5, -0.9, -97.7)) have draw groups like `0x0F`/`0x7F`; `h1020B0` has display group `0x1`. The origin collision `h0010B0` has display group `0x80000000`.
- So the active display mask is the fallback `0x80000000`: the game has not found the collision the player stands on, and only the parts in that fallback group draw.
- The game is live: the right stick turns the camera. The left stick (`PAD_STICK=128,0,128,128,75,90`) does not move the player; the position at `0x0FEFE1E0` stays the same.

Both point at the physics side: the character controller and the ground contact come from Havok. The cause turned out to be audio (next section).

Scratchpad tools: `dcx.py` (unpacks EDGE DCX: `DCX`/`DCA`/`EgdT`, raw-deflate blocks), `msb2.py` (lists MSB parts by type with draw/display groups).

Known but unfixed: `cellGcmGetReportDataAddress` returns a host pointer, and `GET_REPORT` writes into a host table, not guest memory. DeS does not call the former.

### Root cause: a dropped MultiStream reply (fixed, `ae13875`)

The chain, from the symptom back:

1. Havok never steps in the level. `func_001D1E30` (main frame slot 5, world object `0x400E2DD0`) calls the step request `func_00010310` -> `func_01328B10` only when byte `+0x14` is 0. That byte is set every frame by `func_001D0E60`, called from the in-game update `func_00144160` when its local r24 is 0, and cleared again by the frame loop (lr `0x0001BAF0`).
2. r24 is 0 because a manager at TOC-0x7C20 -> -0x7F6C (`0x425F8160` in our runs) has `+0xD8 = 1` (a sequence is running) and `+0xDA = 0` (it does not allow physics). While `+0xD8` is set, physics runs only if `+0xDA` is set.
3. That manager is a step machine (`+0x8` step, step table built in `func_001A3370`: `21D0, 2170, 22C8, 23D8, 26F8, 29D8, 2088, 25F0, 3AF8`). It sat in step 4 (`func_001A26F8`), waiting for its request object (`+0x28`) to reach status 3.
4. The request's own step machine (`0x425F8FF0`, steps `1A7300, 1A5AE8, 1A59B8, 1A5908, 1A5870, 1A6DC0, 1A5D98, 1A5B28, 1A5FD0`) sat in step 6 (`func_001A5D98`): wait until the sound started in step 5 is invalid, started (status bit 8) or finished (bit 0x20). The sound is the voice line `v080000001` (name built in `func_003DC8F0` from id*1000+1); its object (vtable `0x187F418`, status at `+0x88`) stayed at 1, the constructor's value, with no definition at `+0x98`.
5. The play request (`func_00DBAF90`) posts a `MOMS` command to the sound manager's queue. MOMainThread (tid 12) handles it, but it was parked in `sys_mutex_lock` on MultiStream's mutex `0x45E` (read with `ctx 12`: r11 = 0x66, r3 = 0x45E).
6. MOMixerThread (tid 13) held that mutex. In `func_01541790` it locks it, sends command `0xFF6` to the synth SPU with `sys_event_port_send`, and waits on queue 13 for the reply (`func_01543448`).
7. The SPU (`_synth2`, an lv2 SPU thread group, lifted image 11) did reply: `0x3A000FF6` on SPU port 0x3A. The game had bound that port with `sys_spu_thread_connect_event(tid, queue 13, USER, 0x3A)`, which is per thread. `spu_deliver_user_event` looked only at the group's port table, found nothing, and answered ENOTCONN (`[spu-evt] ... port=58 -> queue=0 ... result=0x80010016` with `SPU_EVT_TRACE=1`).

So the first MultiStream command of the boot was never answered, and the whole sound engine had been deadlocked since boot. That is also why the game was silent.

After the fix: sound plays (`AUDIO_RATE=1` reports about half the blocks non-silent in the level, 100% silent before), the entry sequence finishes (sub-steps 6 -> 7 -> 8, manager 5 -> 6 -> 1), Havok steps every frame, the map draws, and `PAD_STICK=128,0,128,128,120,140` walks the player forward and stops when released.

SPU image ids (guest ELF EA = address in the file listing + 0x10000): 1 EDGE zlib, 2 SpursTaskCommandDispatcher, 3/4 FX movement, 5/6 FX ClusterDraw, 7 Havok solver (`0x017BE200`), 8 Havok narrowphase, 9 Havok queries, 10 Havok animation (`0x0181C600`), 11 MultiStream `_synth2`, 12 MultiStream DSP, 13/14 spujobs. The console `spu` verb counts only the spu_workload images; an lv2 SPU thread group such as `_synth2` is not counted there.

Method that found it: a temporary `fprintf` in the generated `func_001D1E30` and its callers (counting entries and steps, then the gate results), the page guard on the flag word (`guard 400E2DE4 rw` with `setenv PPU_GUARD_CHANGE=200`), then `ctx <tid>` on the sound threads. The generated files were restored afterwards.

### Black shading: the exposure constant (2026-10-14)

After `ae13875` the level geometry drew as black silhouettes. Every level fragment program ends with the same tail:

```
MUL r1 = h0 * c.x
DIV h0 = r1 / c.y   (SAT)
```

with `c` = global parameter `0x87`, (exposure, 2, exposure, 1). The exposure was 0, so everything came out black.

Where the exposure comes from: the engine's post chain (bloom and downsample `0x5110000`/`0x5210000` 640x360, `0x4D60000` 1280x720, `0x5310000` 320x180, `0x4CD2040`/`0x4D0A480` 320x180, `0x4D50A00`/`0x4D428C0` 160x90, a 64x64 F_X32 `0x4CCE000`) ends in four rotating 16x16 F_X32 luminance targets (`0x1392440`, `0x1392880`, `0x1377DC0`, `0x13AB840`). The game reads the first texel of the current one on the CPU and copies it into parameter `0x87` (with write-back on, the two hold the same value). Each of those offsets is first declared A8R8G8B8 and then F_X32 in the same frame.

Our d3d12 backend never wrote render targets back to guest memory, so the CPU read stale heap bytes (fp16 NaN, old fragment-program words) and the constant was 0. RPCS3 has the same need; its DeS config turns on "Write Color Buffers".

Fix (`c403bd9`): every render target a frame used is copied into a readback buffer before the command list closes; after the frame, it is converted to the guest layout (big-endian A8R8G8B8, fp16 or fp32 lanes; linear pitch or Morton swizzle) and stored at the surface's address, resolved through its context DMA. All DeS surfaces are in local memory. With it, the first texel of `0x13AB840` and parameter `0x87` hold 0.201796.

The device removal it first caused (`0x887A0005`, in all 7 runs without DRED): `off_rt_get` released the old D3D12 resource when a luminance offset changed format, while the open command list still referenced it. Whether that crashed depended on whether the freed memory was reused. DRED hid it (2 runs with `D3D12_DRED=1`, no removal). Fixed in `a296b91` by deferring those releases until the frame finishes. Ruled out on the way: writes by something else into the stored ranges (none), main-memory surfaces (none), overlap with fragment-program ucode (none).

After `42d79ca` and `1043fc1` the level's fragment programs compute a dim but correct colour before the tone-map tail (checked by saving `0x7A0000` with `FP_SHOW`). The level shader (`fp 0x1569101`) is: albedo `t0`, environment cube `t11` (by normal) times `c5`, lightmap `t6`, shadow map `t7` (a `DEPTH24_D8` texture, not supported yet, so it reads 0 = lit), ambient 0.05, then fog `h3 = h0 + sat(tc2.w)*0.3*(fogcol - h0)` and `h0 = h3*tc8 + tc9`, then the exposure tail. The first level's `FogBank` row 0 has colour alpha 0, so the fog colour is 0. `ToneMapBank` for the level: grey key 0.18, adapted luminance 0.08-0.2.

**Still too dark.** The post chain runs only during the level-entry sequence (806 frames in our runs) and then stops; nothing writes parameter `0x87` (`0x45EE4420`) or the param block at `0x45EE33A8` afterwards. So the exposure stays at the entry value, 0.2018, while `ToneMapBank`'s limits put it at about 0.9-2.25. Why the chain stops is the next question: either the game turns it off once the entry sequence ends and expects something else to update the exposure, or another gate (like the `+0xDA` physics gate above) is still closed.

### Why the tone map stopped: a full vertex buffer (fixed, `8caa9c0`, `9d0c9c0`)

How the exposure gets to the shaders, traced with a read guard on the texel and temporary logs in the lift:
- The main thread's per-frame scene submit (`func_0002B4F0`) posts, every frame, the post chain (scene object `+0x30`) and an exposure read (message `0x8041` for resource `+0x40`) to the HighGraphics queue.
- HighGraphics dispatches `0x8041` (`func_00CD84E0` -> `func_00022908` -> `func_00022068`). The handler looks the resource up, locks it, and the reader callback `func_000216C8` copies the first texel (NaN -> default) into the parameter block at `+0x1A8` every frame.
- The adaptation pass (`fp 0xB40B81`, unit 1 = luminance, unit 0 = the target itself): `cur = clamp(lum, 0.08, 0.2)`, `new = prev + (0.18/(cur+0.001) - prev) * (1 - 2^-k)`. So the exposure is 0.9-2.25 once converged.

Two bugs in our backend:
1. The adaptation reads its own target; D3D12 returned 0, so the exposure was always about 0.06 x the target. Fixed with a pre-draw copy.
2. In the level, the frame's vertex-program vertices hit the buffer's 393,216 exactly (summed from `RTT_DUMP`), and everything after was dropped, including the post chain. The game kept reading a texel no pass wrote any more. `FRAME_BUDGET=1` showed the demand: 408k, then 1.06M. The buffer now grows (384 MB, then 768 MB of upload heap). Each VP vertex is 256 bytes (16 float4 slots); packing only the enabled attributes would cut that by about 4x and is the next step if memory matters.

After both: the exposure converges to 2.22 and the first level is lit and textured. Still open there: the floor draws as the clear colour, no shadows (`DEPTH24_D8` textures are not sampled), about 14 fps.

### Floor, depth and shadows (fixed, `3470cbd`, `4e229b7`, `fb24382`)

- **Missing floor and walls**: our RSX state started with front face CW. The hardware resets to CCW (cull face BACK), and DeS never sets it, so its front faces were culled. `CULL_OFF=1` drew the floor and showed the next bug.
- **Floor over the player**: there was one shared depth buffer, cleared per target and not tied to the guest's depth surface, and the guest's depth function and write mask were ignored. Each guest zeta offset now has its own depth surface that lasts across draws and frames, the depth state is part of the pipeline key, and depth/stencil clears go to that surface.
- **Shadows**: the cascades draw with colour target 0 (none) into zeta `0x5360000`, and the scene samples it on t7 as `DEPTH24_D8` with a compare function. Depth-only draws now bind no render target; depth-format units read the zeta through an `R24_UNORM_X8` view with a comparison sampler. On the first try the whole courtyard was in shadow: the RSX compare function is stored reversed relative to the sampling compare (RPCS3's GL sampler swaps LESS/GREATER and LEQUAL/GEQUAL), and with that swapped the walls cast the right shadows.

### Depth read as colour: light shafts and haze (fixed, `b4821e0`)

RSX lets a fragment program read a depth surface through a colour format: an A8R8G8B8 unit over a zeta fetches the depth buffer's bytes. RPCS3 calls these "redirected" textures and decodes the 24-bit depth in the shader (`RSXFragmentTextureDepthConversion.glsl`, whose comment names Demon's Souls) to `(Z[15:8], Z[7:0], stencil, Z[23:16]) / 255`. We uploaded the guest memory at that offset instead, which never holds what the GPU rendered.

In the courtyard frame DeS reads the scene zeta `0xB50000` this way in the light shafts (fragment program `0x01584081`) and in three post-chain passes (`0x00B40D01`, `0x00B41181`, `0x00B41501`). `0x00B40D01` is camera motion blur: it turns the depth back into a position through a matrix, projects it with the previous frame's, and takes eight taps along the difference. Many other draws have `0xB50000` on t1/t2 left over from earlier draws without sampling it; `FP_PICK=<fp>` with `rsx_replay` shows which programs actually read a unit.

- The backend binds the zeta's `R24_UNORM_X8` view for such a unit and passes a per-draw mask to the decompiler (part of the pipeline key). Those units are `Texture2D<float>` and every sample goes through `rsx_d2c()`. Stencil reads 0; the unit's remap is not applied (DeS uses the identity). `RSX_DEPTH_AS_COLOR=0` (live) turns it off.
- The shafts depth-test against the scene (test on, write off) and read the same depth to fade where they meet geometry. D3D12 cannot sample a surface bound as a writable depth target, so that read was a null texture and the shafts vanished. A draw that samples the depth it is bound to and does not write depth now binds a read-only depth-stencil view (`DEPTH_READ | PIXEL_SHADER_RESOURCE`).
- Check: the shaft program rebuilds depth as `dot((Zhi, Zmid, Zlo) / 255, (2^-8, 2^-16, 2^-24)) * 255` and linearises it with near 0.1 / far 3000 (its inline constants, read from a capture), which matches the byte order.

Result: the courtyard light shafts fade softly into walls and floor (before, from guest memory, they cut through them with hard polygon edges), and the view out of the spawn tunnel gets its distance haze.

### Using an existing renderer instead

Considered and rejected for now:
- RPCS3's RSX backends are the only renderer known to draw Demon's Souls correctly, but they are GPL and tied to RPCS3's emulator core (its memory manager, threads, shader decompiler and surface cache), so they cannot be dropped into ps3recomp.
- Upstream ps3recomp's `rsx_live_draw` (`RSX_LIVE_DRAW=1`) has no write-back, turns F_X32 into RGBA8 and does not handle swizzled render targets, all three of which DeS needs.
- Upstream's `rsx_draw_engine` does write back, but only for Vulkan and Metal, and its Vulkan path builds only on Linux.

Upstream ps3recomp had nothing newer than `a2c2010`, which is already merged.

### HUD and half frames (fixed, `dc3bb6f`, `c01c7f7`)

The user reported the HUD coming and going, fog and light effects flickering, and things not rendering. They captured problem frames with F9.
- **The HUD came and went with the camera** (`dc3bb6f`). The F9 captures replayed deterministically, and the HUD was missing exactly where it was missing in game. A level frame binds up to about 130 distinct textures depending on what is in view, but the backend had 96 per-frame texture slots. Every texture first bound after they ran out drew with nothing in its unit. The HUD is drawn last (the last ~78 draws, fp `0x79FE81`), so it went first. Slots are now 1024, found through a hash index. Running out prints `[TEX] all N texture slots in use this frame` once. The same cause hid any late-bound texture in a busy view.
- **Half frames on hitches** (`c01c7f7`). `GCM_RATES` in the level showed `flipwords=0`: every flip took the immediate path. Demon's Souls calls `_cellGcmSetFlipCommand` on a context whose `current` is always past its `end`, a 32 KB ring at `0x49E08000` whose frames run on past it, and its buffer-full callback (`0x009F6A68`) only returns 0. `gcm_flip_into_fifo` read that as no room. The flip was then counted when the PPU made the call, not where the walker was. The d3d12 backend picks frame ends at display clears with rules keyed on that counter, so when the game hitched it presented frames cut short: no post chain, no HUD. A 6-frame F9 burst, whose file writes stall the game, showed it: frames 2-5 were the scene without post or HUD. The flip now calls the callback as libgcm does (RPCS3 `gcmSetPrepareFlip`) and writes its marker at `current` even past `end`. libgcm writes an 8-byte flip command there, and our marker is 4 bytes. At the marker the walker calls `g_gcm_fifo_flip_hook`, where the backend presents. Once it has seen a flip, display clears no longer present. Results:
  - The level presents 30 frames a second, one per flip. Before, it presented 24-29, with 4-16 batches a second held back as "small".
  - 80 of 80 dumped frames had the HUD, also through the same stalling burst.
  - The advert presents 30 frames a second.
  - `regress.sh`: all shots ok.
- **Skip frames.** When the game drops into its 15 fps mode (see Frame rate), each skip frame is a display clear and one quad. The quad redraws `0x7A0000`, the finished image with its HUD, through a 256x1 LUT (`fp 0x180`). They are now presented on their own flip, as on the console. In a burst they show up as 141-method captures.
- `5a01d21`: `fp_bind_b1` multiplied texel-addressed coordinates by 1/size only for the `rsx_samp[N]` spelling. A program with a shadow unit declares `rsx_sampN` one by one. The fog particle program (`0x1584081`, which has a leftover shadow map on unit 7) is one, but its units are normalised in the frames checked, so nothing visible changed there.
- **Particles drawn as slivers** (`3f094e1`). The user's next captures showed sparks as big orange triangles, the spawn effect as green slivers, cutscene fog as hard-edged sprites, all changing every frame. Leaving out one draw at a time (`skipscan`, `DRAW_SKIP`) found the sparks draw (fp `0x1583B01`). `RSX_DBG_VTX` showed sane positions, six vertices a spark, but the triangles joined the end of one spark to the start of the next. The raw methods in the capture: one BEGIN/END of TRIANGLES with three `DRAW_INDEX_ARRAY` ranges, (0,32) (32,256) (288,12). The RSX assembles the ranges as one stream, so a triangle can straddle a boundary (32 is not a multiple of 3). With primitive restart enabled, `upload_tris_vp_indexed` assembled each range on its own and dropped the leftover 1-2 indices, so every later triangle was out of phase. The partial triangle now carries into the next contiguous range of the same BEGIN/END. In replays of the captures, the sparks, the spawn cloud and the fog all look right; `regress.sh` is ok. The non-restart path already kept every index, and record concatenation kept it in phase. Strips and fans split across ranges are handled the same way since `5a78630`.
- **Holes in the level geometry** (`5a78630`). The user's capture showed two holes in a tunnel ceiling with the sky behind. Tallying the capture's draws: 110 level meshes are restart-enabled triangle strips sent as several `DRAW_INDEX_ARRAY` ranges in one BEGIN/END. `upload_strip_vp_indexed` started a new strip at every range and lost the two triangles across each boundary. It now expands one index at a time and carries the strip state (position, last two indices, fan centre) into the next contiguous range. Both holes close in replay; `regress.sh` ok, and that set (`strips`) is the new accepted baseline.
- **Black player and NPCs** (`9db1a5f`). In the user's capture the player was a black silhouette. `FP_IDCOLOR` listed every program but the player's (`0x1474C01`), and the log had `[FP] decompile fail` for it and two more. They are 4736, 4480 and 4352 bytes long; every backend looked for the END bit only in the first 4096 bytes, so the decompile failed and the draw fell back to a black pipeline. `RSX_FP_MAX_BYTES` (64 KiB, in `rsx_fp_decompiler.h`) now bounds the walk for D3D12, Vulkan and Metal. The walk stops at END, so this only bounds a corrupt program; RPCS3 has no limit.
- **Sun flare through walls** (`df9d0f9`). The game measures the sun once a frame: ZPASS counting on, `CLEAR_REPORT_VALUE`, one 24x24 probe draw, `GET_REPORT` type 1 into report 1024, counting off. It reads the result with `cellGcmGetReport(1, 1024)`. The walker answered every ZPASS report with 0xFFFF, so the probe was never hidden. These methods now reach the backend through `g_rsx_report_hook`, in stream order with the draws. Each draw made while counting is on gets a D3D12 occlusion query, and after the frame's GPU wait the counts are summed between clear and report and stored with `cellGcm_report_store`. The answer arrives a frame late, which a fading flare does not show. Behind the wall in the capture the probe counts 0; with `DEPTH_OFF=1` it counts 576. `RSX_ZPASS=0` (read at init) goes back to "visible"; `ZPASS_LOG=<n>` prints the first n stored values.
- **Black flicker in a dark room** (`ea26191`). Two of the user's F9 captures there (`cap_013`, `cap_016`) were skip frames: 141 methods, one draw (fp `0x181`) copying `0x7A0000` to the display through the 256x1 LUT, and their replays looked fine, because a replay cuts frames at display clears and has no flips. An F10 burst there showed the pattern: every present with 2048 draws was pure black, every 1-draw present had the picture. A frame in that room issues more than 2048 draws. The d3d12 backend kept at most `MAX_DRAWS` (2048) draw records a frame and dropped the rest without a word, the display copy at the end with them, so each full frame presented the cleared backbuffer. `MAX_DRAWS` is now 8192 (per-draw constants 138 MB, was 35 MB), and `[D3D12] frame has more than 8192 draws` says when a frame still runs out. The user reports the flicker gone; `regress.sh` ok. Boletarian Palace issues up to 7529 draws a frame, so the limit went to 16384 (`7c2753d`, 264 MB). The offscreen target pool was not it: 15 of 16 in use, nothing evicted in the room. F10 was also silently dead at first: Windows sends F10 as `WM_SYSKEYDOWN`, which the window procedure now handles (`6056325`).
- **Fog wall and Nexus cutscene colours (closed 2026-10-21: not seen since the fixes below).** The user reported the fog wall flickering (`cap_017`), odd colours walking through it (`cap_018`), and odd colours in the Nexus cutscene (`cap_019`-`cap_022`), and said the replays did not show what they saw. The replays render both scenes. One way live can differ: a texture bound at an offscreen target's offset sampled the target for good, even after the title had put a texture or an image in that memory; a replay starts with no targets and reads the guest bytes. With colour write-back on, the guest bytes after a store are our own, so a checksum taken then and compared at the start of each frame tells a foreign write, and the target is dropped (`7769d7c`, `RSX_RT_REWRITTEN=0` off, `[RT] ... rewritten by the title` logged). It does not fire on the regression walk or in Boletarian Palace; whether it fires at a fog wall is not known yet. F9 now saves the live presents with each capture (`b039057`), so the next report shows the screen beside the replay.
- **Nexus colours: sampler tables ran out** (`b73b4ef`). The next set of captures (`cap_023`-`cap_029`) came with live presents (`b039057`), and those showed what the replays could not: in the Nexus cutscene (`cap_027`) bright areas wrapped to blue, magenta and flat grey (white came out 127,127,127) where the replay was plain overexposed white, and entering the Nexus (`cap_028`) a dark foreground arch (op 1147, an ordinary level shader) was covered in red, yellow and grey. The player's log had `[D3D12] sampler tables full (128); later combinations reuse table 0` just before those captures. Sampler tables (16 samplers each, one per unit) were made per distinct combination and kept for the whole run; D3D12 allows 2048 shader-visible samplers, so 128 tables, and the tutorial plus the Nexus used them all. Every later combination got table 0's addressing and filtering: the final copy read the 256x1 grading LUT with linear wrap, so white blended the LUT's last entry (255) with its first (0) and came out grey. A replay starts with empty tables, hence the mismatch. A full heap now rewrites the table used longest ago that the frame being recorded has not bound (after draining the queue if the GPU may still read it); only a single frame with more than 128 combinations falls back to table 0, and that is logged. `RSX_SAMP_TABLES=<n>` uses fewer tables to exercise the reuse: `regress.sh` with 40 reuses tables from the title screen on and is ok; a `cap_027` replay with 32 is pixel-identical, with 8 (one frame needs more) it degrades the way the live frame did.
  - Checked and ruled out on the way: the LUT itself (an identity ramp in the capture), the dropped-target change (fired 7 times in the session, each right after a level load), `RSX_LATE_CHECK` and `TEX_VERIFY` (silent through a boot into the Nexus), the screen-copy texture path (never bound live), and ZPASS (cap_023 has no occlusion report).
  - Left open at the time, closed on 2026-10-21 (no longer seen in play; noted here if it comes back): `cap_023` (first cutscene) alternates a dense and a faint fog layer between rendered frames; it came before the tables ran out. `cap_024`'s soul-form aura has long blue vertical streaks; that capture is a skip frame, so its replay is the live image and cannot be bisected. (The streaks turned out to be the aura's own look: `cap_034`, below.)
- **Flickering soft particles (fixed, `ec8ad33`).** The soul-form aura, the fog wall and the first cutscene's fog are one fragment program, `0x1584081`. It samples the scene depth it is depth-tested against and fades by the difference. The backend binds that depth read-only only when the draw record has no Z/S clear flags. Draw records are reused from frame to frame, and the draw paths never reset `clr_flags`, so a slot that once held a depth clear kept its bits. A soft-particle draw that landed in such a slot got its depth texture bound NULL, read 0 and faded to nothing. Draw counts shift by a few records between frames, so the effects came and went. Replays start with zeroed records, which is why every capture's replay drew them. Found with `FP_WATCH=1584081` on the user's soul-form save, 60 s idle: 104 frames reported `samples its own writable depth surface (reads 0)`; 0 after the fix. The user reports the first cutscene's fog steady. `cap_007` (an aura burst) had caught frames where the aura was drawn: its live present matches the replay (mean difference 0.4 against 3.9 with the aura's draws skipped).
- **Aura and fog wall vanish live (history, now fixed).** The user's bursts: `cap_034` the blue aura around the player is there, `cap_035` it is gone (the player did not move), `cap_041` the fog wall at the top of the tutorial stairs is off, `cap_042` on, `cap_040` the fog flickers in the first cutscene.
  - The long vertical streaks are the aura itself: billboard particles (fp `0x1584081`, sprite `0x1690080`, additive) that rise around the player. Skipping their draw removes the aura. The draw's three index ranges (48, 256, 56) assemble correctly with `3f094e1`.
  - `cap_035`: the live presents of the captured frame and the next one have no aura; replays of `cap_035_00` and `cap_035_01` draw it. `cap_041` is the same: the replay has the fog wall (ops 797-809, fp `0x14DBA81`, shadowed by the 2048x2048 depth map), the live present does not. So the game issued the draws and something only live lost them.
  - Ruled out: occlusion queries (none in these frames), late writes to the particles' vertex arrays or indices (the extended `RSX_LATE_CHECK` reports only fp `0x181` HUD colour arrays in the Nexus), and the per-draw FP constant patches (242 a frame, sent as NV308A inline transfers in the command stream, so live and replay apply them in the same order).
  - Tool for the next report (`daa2792`): F9 now also writes what each draw of the captured live batch rendered from, and `tools/des/capdiff.py cap_NNN.rcap` diffs that against the replay. In a Nexus frame live and replay inputs are identical record for record. If an aura-less frame also shows identical inputs, the cause is GPU-side state a replay starts without (an offscreen target, a depth surface or a cached texture's contents); if not, the first differing record names it.
  - `cap_002` (fog wall at the tutorial stairs, 2026-10-15): `capdiff.py` finds live and replay inputs identical record for record. The fog wall is draw 795 (fp `0x1584081`, 456 vertices, `t0` = `T1F00E00`, a 128x128 DXT5 that only this draw uses). Replaying with that one draw skipped (`DRAW_SKIP=795-795`) reproduces the broken live present bit-exactly, so live the draw ran and added nothing. Same vertices, constants, pipeline and depth inputs means the texture the GPU sampled is the prime suspect (zero alpha or stale contents).
  - Two backend bugs fit that and are fixed (ps3recomp PR #1): the texture checksum read the same bytes of every compressed block (on a 128x128 DXT5 only the alpha endpoints, never the indices), so a texture rewritten in place could keep its checksum and draw stale cached contents (`5bf4527`); and each texture slot's SRV sat at heap index 1+slot, inside the per-draw windows from slot 31 on, so in a session with many cached textures an upload could rewrite a descriptor of one of the frame's first 63 draws (`82a3ea6`). Neither is confirmed as the flicker yet: that needs a new live capture.
  - Fingerprints now also hash the bytes each texture slot uploaded (`bd40e8d`): `capdiff.py` reports `sum tN` (guest checksum) and `up tN` (uploaded bytes) per unit. A differing `up` on the vanishing draw confirms the texture.
  - Chaining captures through one backend (`886ed2e`, below) to carry GPU state over: 13 captures in a row do not reproduce it. A replay's first frame shows the snapshot's written-back live picture, so only later frames of each capture count; an early "repro" with `RSX_RT_REWRITTEN=0` was that artifact.
  - Also ruled out for `cap_002`: F9 itself, alpha test, the PSO key, the sampler tables, texture eviction, mid-frame splits, skipped renders, predication.
  - Open lead: in the Nexus the log once reported `[VPVB] a frame needs 2295990 vertices ... later draws are dropped`. A frame normally needs about 230K, so one batch spanned many frames; dropped particle draws there would look like this. The warning prints only once.

## Debug and test tools (2026-10-14)

All of these keep game-derived output (screenshots, captures) in the scratch directory; none of it goes in the repo.

- **Staged exe** (port `2d6b3ab`): `desrun.sh`, `repro.sh` and `milestones.sh` copy `des_boot.exe` (and its `.pdb`, `.map`, DLLs) to `$DES_SCRATCH/bin` and run the copy, so the build can relink while a run is alive. `sym.py` against a staged run: `--map "$DES_SCRATCH/bin/des_boot.map"`.
- **Live knobs** (`e58297f`): the console's `setenv K=V` bumps `ps3_env_gen`, and knobs written as `PS3_KNOB(var, expr)` (`include/ps3emu/ps3_knob.h`) re-read on their next use (one load and compare otherwise). 272 sites were converted mechanically (`CULL_*`, `DEPTH_OFF`, `FP_*`, `FRAME_BUDGET`, `RT_OFFDBG`, `CELLMARK_DUMP_CHANGED`, ...). Pipeline-baked knobs (`CULL_*`, `DEPTH_OFF`, `FP_*`, `CMASK_FORCE`) rebuild the pipeline cache when their values change. `PAD_STICK=lx,ly,rx,ry,from,until` is live and its window counts from the setenv: `setenv PAD_STICK=128,0,128,128,0,10` walks forward for 10 s. Still read once (multi-statement parsers): `PAD_FILE`, `PAD_SWEEP`, `DBG_ZOOM`, `CELLMARK_DUMP` / `_SKIP` / `_EVERY` / `_DIR`, `D3D12_IQ`, `GCM2D_WATCH`, `TEX_NOCACHE`, `WATCHDOG_AT`, `GCM_FLIP_BT`, and most `SPU_*` / `PPU_*` / `PS3_*` trace knobs.
- **Manifest fields** (`d613b7f`): `RSX_CAP` manifests show `ct=` (colour target), `z=` (zeta), `zf=`, `depth=` (packed depth key: bit 0 test, bit 1 write, bits 2-4 function), `cull=`, and on clears `mask=`, `z=`, `zeta=`. A small phase summariser (one line per run of ops with the same target/zeta/depth) made the DeS frame layout readable: display copy, Z clear, three shadow cascades into zeta `0x5360000` (2048x2048, colour target 0), scene into `0x7A0000` / zeta `0xB50000`, post chain, HUD.
- **Regression shots** (`tools/des/regress.sh`, `shotdiff.py`): boots with `DES_SKIP_INTRO=1` (and, since 2026-10-16, `DES_AUTO_CONTINUE=1`), walks a fixed route with the live `PAD_STICK` (spawn, tunnel exit, camera turn, courtyard) and saves one shot per stop under `$DES_SCRATCH/regress/<set>`, then diffs against the accepted set (`regress.sh accept <set>`). Per shot: mean difference and the share of pixels that changed by more than 32 after a 4x downscale; REGRESSION above 25% or when a shot goes black. Two runs of one build differ by under 3% at `s1_spawn` and `s2_walk` and by up to 19% at `s3_turn` and `s4_on` (see below). The walk starts from a pinned save: `regress.sh pin-save [dir]` (port `d0ebf14`) copies a save directory (default: the current one) to `$DES_SCRATCH/regress/save`, and each run installs it over the player's save and puts the player's back after. Without it the route started wherever the player had last saved, and every shot regressed once the user's character moved on. About 2.5 minutes. The walk does not land in the same place every run (an enemy, a few frames of drift), so two runs cannot show what one knob does. `REGRESS_AB="KNOB=value"` (port `91c3cfd`) sets the knob live at the last stop and saves `s5_ab`, then clears it and saves `s6_back`, from one position. Since port `e7410fc` (2026-10-19) the route runs on the game's clock (`tools/des/regress_route.py`): the run writes a frame log (`DES_FRAMELOG`), game time is the sum of its frame steps (1/30 s a frame at 30 fps, the measured frame time under `DES_FPS`), and every stop is planned in game seconds from the end of the opening cutscene: `s1_spawn` at `REGRESS_SETTLE` (default 35.9 s, where the old 75 s after the level load put it at 30 fps), each hold (`REGRESS_WALK` / `REGRESS_TURN` / `REGRESS_ON`) 0.8 s after the shot before it, each shot 2.3 s after its hold ends. A hold is a `PAD_STICK` window sized from the game/real rate and re-issued with a corrected length if the rate changes. Since port `c15d23f` the window ends half a frame early: the runtime times it with `GetTickCount64` (steps of about 16 ms), and at 30 fps a window that ended right on a frame came out one frame long in some runs. A shot is the next frame after `CELLMARK_DUMP_NOW=1`. The clock-driven route moved with the load time, slow stretches and the dump interval, and `s4_on` flaked: in one 30 fps run an enemy that follows the player stood in front of the camera (40% changed). At 240 fps it also lost `s2`-`s4`: its shot picker sorted file names, and `frame_999.bmp` stayed the newest after `frame_1000.bmp`. In 11 runs on the game clock (30, 120 and 240 fps) every shot compared ok with the accepted set, the worst at 18.5% changed. Before that, the 30 fps camera turn's window ended right on the 46th frame, so the turn was 45 or 46 frames, and the 46th turns the camera far enough (8 degrees) that the walk on goes past the wall it ends at in the accepted set, to a message about 5 m further (3 runs of 6, `s3_turn` and `s4_on` then 15-19% from the other runs). With the half frame off, three 30 fps runs read the left stick for 240 and 150 frames, the turn's window (1.484 s) ended between the 45th and 46th frames, every walk on ended at the wall, and the second and third runs were within 2.2% of the first at every shot; three 240 fps runs passed too (`s4_on` 15.5-16.7% against the accepted set). `$DES_SCRATCH/regress_run/route.txt` has each run's timeline.
- **Frame capture and replay** (`0d8c4f2`): `setenv RSX_REPLAY_CAP=<file>` in a running game records the next frame (from the clear the backend presents at to the next one): RSX state and GCM tables at its start, every method the FIFO walker dispatched, and the committed non-zero pages of VRAM and the IO-mapped memory at its end (DeS courtyard: 46k methods, 117 MB). `ps3recomp/build/rsx_replay.exe <file> [frames]` (`cmake --build build --target rsx_replay`) plays it through the same dispatch and D3D12 backend with no game: about 4 s for the first frame (pipeline compiles), then about 80 ms per frame. `CELLMARK_DUMP=<n> CELLMARK_DUMP_DIR=<dir>` dumps the presented frames, `RSX_CAP=<n> RSX_CAP_DIR=<dir>` the manifest and render targets. The exposure adaptation reads the previous frame, so the first replays are darker; by about 20 frames it matches the live frame. The capture and the replayer must come from the same build. Replay frames alternate between the whole frame and a near-empty present, so pick dumps that are not black (`CELLMARK_DUMP=6 CELLMARK_DUMP_SKIP=50`), and for manifests capture a few frames (`RSX_CAP=3:4`) and use the one with about 1,700-1,900 draws. `FP_PICK=<fp hex>` writes that program's HLSL to `duck_fp.hlsl` in the current directory (run it from the scratch directory).
- **F9 capture** (`12a61a8`, port `359c66f`): F9 in the game window records the next frame like `RSX_REPLAY_CAP`, to `RSX_HOTKEY_DIR/cap_NNN.rcap`. `desrun.sh` sets that to `$DES_SCRATCH/rsx_hotkey`. Use it when someone playing sees a frame go wrong (about 120 MB a capture in the level). `RSX_CAP` now re-parses when its value changes, so the console can arm several manifest captures in one run (change `RSX_CAP_DIR` between them). `RSX_DBG_VTX=<tex0 raw hex>` prints every enabled vertex attribute of the next 8 draws that bind that texture. `RSX_DBG_VTX=fp:<hex>[:n]` prints vertex 0 of the next n draws with that fragment program. Both are live and re-armed by every setenv.
- **F9 bursts and bisecting a replay** (`535fbda`): `RSX_HOTKEY_FRAMES=n` (default 1) makes F9 capture n consecutive frames as `cap_NNN_00..cap_NNN_<n-1>.rcap`, for flicker that only shows from one frame to the next. Each capture writes ~114 MB when it ends, so a burst stalls the game; that is a hitch test in itself. F9 numbering starts at the first number not already in the directory, so a new run no longer overwrites the last one's captures. `DRAW_SKIP=a-b` (live) leaves out the frame's ops a..b (manifest numbering) to find which op breaks a later one. `FP_TEX` now compiles for programs with per-unit samplers.
- **Present bursts** (`0beda1a`): F10 in the game window dumps the next `RSX_HOTKEY_SHOTS` (default 12) presents to `RSX_HOTKEY_DIR` as `shot_<burst>_<n>_d<draws>.bmp`, every present, with its batch's draw count. Use it for a flicker between presents: a replay has no flips and cuts frames at display clears, so it cannot show one. `CELLMARK_DUMP_NOW=<n>` (live) does the same from the console into `CELLMARK_DUMP_DIR`. `RSX_FRAME_SLEEP=<ms>` (live) stalls each batch; about 30 puts the game in its 15 fps skip-frame mode at the spawn. Dump lines now give the draw count, and `CELLMARK_DUMP_CHANGED=0` means off. `[RT] all N offscreen targets in use` / `[RT] evicted` report the offscreen target pool.
- **Live presents with F9** (`b039057`): each F9 capture also writes the next `RSX_HOTKEY_LIVE` presents (default 4, 0 = off) as `cap_NNN_live_XX_d<draws>.bmp` beside `cap_NNN.rcap`: what the screen showed, to set against the replay, which rebuilds one batch from guest memory with no targets from earlier frames.
- **FP_WATCH** (`7cc4a1c`, live): `FP_WATCH=<hex>[,<hex>...]` logs every change in how many draws a full batch makes with those fragment programs, and every one of their draws that samples the depth surface it is bound to as writable (bound NULL, reads 0). `FP_WATCH=all` logs any program that one full batch drew, the next did not, and the one after drew again. A full batch has at least `FP_WATCH_MIN` draws (default 100), leaving out the skip batches that only copy the last frame; `FP_WATCH_MAX` (default 400) caps the lines. Use it before asking for a timed capture: a play session with it on says whether the game stopped issuing an effect or the backend lost it.
- **Live-vs-replay draw inputs** (`daa2792`, port `tools/des/capdiff.py`): each F9 capture also writes `cap_NNN_cap0_d<n>.txt` (and `cap1` when the capture ends in a later batch): one line per draw record of the live batch with the inputs it rendered from, with `# capture starts/ends at record N` marks so the lines match the replay's one for one. Fields: `fp`, `vs` (VS ucode hash), `n`, pipeline keys (`bk`, `dep`, `cull`, `cm`, `rt`, `z`, `d2c`), `vb` (every fourth emitted vertex), `cb` (VP constants), `fcb` (texture scales, alpha test and the program's inline constants, not the rest of the slot), and per unit `T<raw>:<checksum of the guest bytes>/<hash of the uploaded bytes>` (the hash since `bd40e8d`), `R<offset>` (offscreen target), `Z<offset>` (depth surface) or `-` (nothing). `python tools/des/capdiff.py <cap.rcap>` replays the capture with `RSX_DRAWPRINT`, aligns the records and prints structural differences and differing hashes. The live batch has one record the replay lacks, the closing display copy. `RSX_DRAWPRINT=<prefix>` writes the next `RSX_DRAWPRINT_N` (default 4) batches as `<prefix>_XX_d<n>.txt`, live or replay; `RSX_DRAWPRINT_K=1` adds the constants themselves. F9 still writes `cap_NNN_draws_XX_d<n>.txt` beside each live present. Reading the upload heap back is slow, so a capture stalls the game a little longer.
- **Chained replays** (`886ed2e`): `rsx_replay <capture> <capture> ... [frames]` plays each capture `frames` times through the same backend, so a frame starts with the targets, depth surfaces and cached textures the earlier ones left, as in a running game. Guest memory is not cleared between captures. A capture's first frame is its snapshot's written-back picture; use the later ones.
- **Late reads and stale textures** (`d4eb290`, `28c69a9`, vertices in `daa2792`): `RSX_LATE_CHECK=1` (live) hashes each draw's textures and fragment program when the draw is recorded and again when the batch renders, and logs `[LATE]` for anything that changed in between (both are read from guest memory only at render time). `TEX_VERIFY=1` (live) hashes every byte of each texture-cache hit and re-uploads one the sparse checksum missed (`[TEX] stale`). `TEX_NOCACHE` is now live. Vertices are the other way round (read when the draw is recorded): the check notes the guest bytes of every enabled array over the vertex range each upload fetched, and the index range, and logs `[LATE] ... written after the draw read it` if they change before the batch renders. A skip-frame capture (141 methods, one draw) replays as the image live had written back, which is the live frame, but has no draws to bisect.
- **Frame boundaries** (`c01c7f7`): `RSX_PRESENT_AT_FLIP=0` goes back to presenting at display clears, and `GCM_FLIP_CALLBACK=0` to refusing a flip marker past `end`. `GCM_RATES=1` shows which path presents: `flip:render` against `clear:render`, plus `flipwords`.
- **CPU per thread** (`e4aea1e`): console `cpu [s]` (default 5 s) prints each host thread's CPU (percent of one core), the split between the exe (`app`), ntdll/kernel32/KernelBase (`os`: yields, spin-waits, syscalls) and other DLLs (`dll`: driver, D3D12, audio), the guest thread name or the host start function, and the hottest exe function (for os/dll time, the exe function that called out). A blocked thread costs nothing, so a waiting thread with a large `os` share is spinning. First reading in the level (3.8 cores): an SPU executor thread 96% (`spu_shufb`), an SPU async thread 71%, HighGraphics 68%, `vblank_ticker` 49%, `MOStreamingThread` 28% inside `sys_timer_usleep` (99% os), `_sys_MixerChStrip` 18% in `sys_cond_wait` (84% os). The last three wait more than they work.
- **Waits per thread** (`b46dc86`): console `waits [s] [rows]` (default 5 s) times every lv2 syscall and HLE call for s seconds, keyed by guest thread, call and r3 (usually the object waited on). It prints:
  - each thread's share of wall time blocked in syscalls and in HLE;
  - a view folded by thread and call, with the number of distinct objects;
  - the top calls with their r3.

  `cpu` says who burns a core; `waits` says what a thread that is not burning one is waiting for. Costs nothing unless armed.
- **Killing only our runs** (port `f7ce830`): `des_stage_exe` drops a `.des_staged` marker beside the staged copy. `des_kill_own` kills the pids in the current pid file and every running `des_boot.exe` whose exe has the marker, so it works from a shell where `DES_SCRATCH` is unset or different. A copy the user starts from `port/build` has no marker and is left alone.
- **Muted test runs** (`ad4b9b0`, port `ce22c2b`): `AUDIO_MUTE=1` (live) writes zeros to the WASAPI buffer at the normal rate, so the audio clock and the game's pacing do not change. `PS3_NO_AUDIO` is different: it makes the title take its no-audio path. `des_env_defaults` sets `AUDIO_MUTE=1`; `AUDIO_MUTE=0` makes a run audible.
- **Audio block counters** (`e50e28c`): `AUDIO_WRITEPOS=1` prints, per port every 1000 writes, a histogram of where the game's writes land relative to the read index we publish (`published+0:N` means the block we read next). `AUDIO_GAPS=1` counts blocks that are still cleared when read with sound on both sides (a write that came too late), logs the first 50 with the ring (`R` read, `#` data, `.` cleared), and adds `gaps N (M blocks, x% of the window)` to the `AUDIO_RATE=1` line. With `AUDIO_WAV=<file>`, `audgaps`-style analysis of the dump (zero 256-frame blocks between sound blocks, jumps at block boundaries against mid-block) gives the same answer from the output side.
- **Lifted-code index** (`tools/des/recomp_index.py`): `def` / `src` / `at` / `callers` / `callees` / `path` over `work/recomp`, from a SQLite index (94k functions, 130k direct-call edges) that rebuilds in seconds when a lifted file changes. codebase-memory-mcp did not finish indexing the 450 MB of generated code in 30 minutes, so this replaces it for the lift; the hand-written code stays in the general index.
- **Performance A/B and profiling** (`tools/des`, 2026-10-16). All of them boot into the level through `levelcap.sh`, which installs a save (`LC_SAVE`, default the pinned regression save; empty for the current one), puts the player's save back on exit, and runs a file of console commands. Output goes under `$DES_SCRATCH`.
  - `exeab.sh <tag> <exeA> <exeB> [rounds]`: two builds, alternating runs, two 8 s profile windows each at `DES_FPS=60 VBLANK_MHZ=120000` (uncapped). `RSX_PRESENT` is chosen from the lock state once per batch. `<tag>.out` lists every window and the A/B means (`kabsum.sh`). Three rounds take about 25 minutes. While the PC is in use one build varies by about a frame between windows, so a change has to win in every window.
  - `knobab.sh <tag> <KNOB=val>`: one run, profile windows in `KAB_ORDER` (default `ABBAAB`) with the live knob cleared (A) or set (B).
  - `kprof.sh <tag>` with `PF_PAT=<thread name ERE>`: `prof <tid>` (or `PF_VERB=profrip` for instruction addresses) on the matching threads; `profsym.py port/build/des_boot.map <out>` sums the result by function.
  - `soak.sh <tag> [exe]`: six minutes at `DES_FPS=120`, walking, with `stat` lines; for changes to the atomics. Point `LC_SAVE` at a Nexus save.
- **Developer keys only** (2026-10-21): F9 (capture), F10 (present burst), F11 (profile session) and F8 (a `MARK` line in a `DES_FRAMELOG` log) work only with `RSX_DEV_HOTKEYS=1` (ps3recomp's `ps3_knob.h`, live). A player's build ignores them. A developer build (`-DDES_DEV=ON`) sets it at start when it is unset, and `des_env_defaults` sets it for every `tools/des` run, `play.sh` included, so the capture and `capdiff.py` workflow is unchanged. `RSX_DEV_HOTKEYS=0` shows what a player gets. F10 is still taken by the window with the keys off, so it never opens the window menu. F1 (or L3 + R3), Alt+Enter and Report an issue work in every build; the report takes its own screenshot. The console's profile toggle does not need the knob.

## Frame rate (2026-10-14)

The level shows about 14-16 frames a second, but the game runs at its normal tick rate, so the fault is not in game logic.

- **28 ticks, 28 flips, 14 presents a second.** HighGraphics flips once per loop, but every other frame is a near-empty "skip" frame (1.5k-12k FIFO words, 1-2 draws) between full frames (~42k words, ~1,725 draws). GCM_RATES counts presents at display clears and folds the small batches into the next frame.
- **The skip frames come from the game's own frame-rate pacer.** The pacer is the object at `0x45B643D0`, reached from HighGraphics's loop `FUN_00d1f188`. Its vtable `+0x3c` (`0x000CBB70`) times each frame in timebase/(freq/30) units:
  - It enters slow mode when a frame takes more than 109% of 33.3 ms (about 36.3 ms).
  - It leaves slow mode only when a render+skip pair takes under 211% (about 70.3 ms).
  - In slow mode `+0x2c` (`0x000CB7E8`) returns 0 on alternate frames, which makes the skip frames.
  - Fields: `+0x24` slow flag, `+0x25`/`+0x35` skip history, `+0x10` interval (2), `+0x18`..`+0x48` timestamps.
  - Measured: skip frames ~33 ms, rendered frames 40-47 ms, pairs 71-80 ms. The game therefore stays in 15 fps mode, and the target is a rendered frame of 36 ms or less.
- **What a rendered frame waits on.** The new `waits` tool shows HighGraphics blocked 73-75% of the time, almost all of it in the libsre LFQueue pop (`0x91066667`, semaphore 1 underneath). That queue is the command ring to the SPU command processor (image 2). HighGraphics waits for ring space about twice per flip, roughly 13.5 ms each time. Main waits on cond `0x2E`, which HighGraphics signals once per frame (about 26 ms per frame).
- **Ruled out:**
  - Presentation itself.
  - The vblank rate: 8 ms ticks left flips at 28-30.
  - The SPU idle nap: `SPU_LLAR_NAP=0` gave the same rate.
- **What helped:**
  - The FIFO walker's idle wait sits between the guest queuing commands and the labels the ring is gated on. At 1 ms (`0861c1c`), presents went from about 13.7 to 15.8 a second.
  - CPU freed by `0c18b0b` (usleep) and `bab5001`/`2b3dda3` (SPU code) did not move the rate on its own, which matches a latency chain rather than a throughput limit. The chain runs from HighGraphics to the FIFO, then to RSX labels, then to SPU image 2, and back to ring space.
- **The fix: vertex fetch** (`ba6305d`). A profile of the RSX thread (`prof` on `vblank_ticker`) put about 40% of its busy time in the CPU vertex fetch: `rsx_fetch_attrib` 28%, `read_vp_vertex` 5%, plus address resolves and getenv. A level frame fetches about a million vertices, 16 attributes each, and every attribute call re-derived its enable, type, divisor and context DMA.
  - Each draw now builds a fetch plan once: the constant registers plus only the enabled arrays. Each vertex is built on the stack and copied once into the write-combined upload heap.
  - Strip/fan expansions and indexed draws read through a per-draw vertex cache.
  - Result: about 28 presents/s standing (the game's 30 fps cap) and 20-28 while walking. `VP_FETCH_FAST=0` restores the old path for A/B.
  - Seen once: a regress run stopped flipping right after the spawn shot. The game was still running SPU work, but no flips came, and the process was killed before it could be inspected. Four runs since (one regress run, three 4-minute walks with `walkwatch.sh`, a scratch script that keeps a hung game alive) did not repeat it.
- **Next:**
  - If a stall shows up again: `waits` / `gstacks` on the live process. The faster frame rate may expose a race like the earlier lost wakeup (`d5aeb3d`).
  - Time the chain end to end: when HighGraphics queues a command, when the walker reaches it, when image 2 frees the slot.
  - Per-block SPU trampoline overhead (`spu_task_launch_check` 4%, `spu_drain_call` 3%).
  - The vblank tick is a flat 16 ms on `GetTickCount64` (62.5 Hz, not 59.94).

## Audio: ATRAC streaming (2026-10-14)

- MagicOrchestra streams every ATRAC3+ track.
  - It hands cellAtrac the first 20-100 KB of a ring (for example, 102,400 of 1,950,304 bytes for the level BGM).
  - Each tick it calls GetStreamDataInfo, copies the next sequential bytes from its own stream memory to the returned write pointer, and calls AddStreamData (`FUN_00d78ff0`).
  - It watches the returned read position only to detect a loop wrap.
  - It takes the no-second-buffer path (`FUN_00d79370`).
  - Those NIDs were unregistered and faked CELL_OK, so the decoder ran past the loaded part. That caused about 650 "Frame data doesn't match channel configuration" errors per run, plus "Invalid start bit" and MOPS3StreamLoader's "Stream Memory Free State Errror!!".
- `a90fc9a` implements streaming. Every byte that arrives is also copied into a host image of the file, and the decoder reads only that. After the fix:
  - 0 decoder errors and no stream-memory errors.
  - The dumped mix (`AUDIO_WAV`) looks like music (zero-crossing rate ~650/s), with no NaN or clipping.
  - Not listened to yet; test runs are muted.
- **`PS3_DEV_FLASH` / `flashATRAC.pic` hangs the boot; do not default it.** MultiStream still logs "atrac failed to load", because `flashATRAC.pic` (firmware) is missing. With `PS3_DEV_FLASH` pointed at an RPCS3 `dev_flash`, MultiStream opens it, and the game then stays on the first loading screen (no level after 10 minutes). In that state:
  - The MultiStream synth images 10/11 never run.
  - `Core.Res.Decompressor` waits in `cellSpursEventFlagWait` (`0x48BBBD80`) with 1 s timeouts.
  - `Core.Res.CacheDeviceHandler` spins at 99% of a core: about 2M lwmutex lock/unlock and semaphore trywait calls per 5 s.

  So with the firmware module loaded, MultiStream's SPU side diverges and the resource pipeline never gets its SPU completion. Without the firmware the game boots normally (4 of 4 runs after `a90fc9a`).

## Audio: half the blocks were silence (fixed, `1e4d419`)

The audio played but sounded terrible. The `AUDIO_WAV` dump showed why: 46.6% of the 256-frame blocks were all zero, mostly single blocks between sound blocks, so the sound was gated on and off every 5.33 ms. The rate was right (187.5 blocks/s, nothing dropped), and the game's mixer got every notify.

- Demon's Souls has two 8-channel ports. Port 1 (8 blocks) carries the sound. Its libmixer writes one block per notify, at the index we publish (`AUDIO_WRITEPOS`: 99.9% at `published+0`), which is the next block we read.
- The Windows mixer was paced on WASAPI room. WASAPI drains its buffer a 10 ms period at a time, so room for two blocks opened together and two blocks were mixed back to back. The second read of each pair came before the game had answered the first notify, found the block still cleared, and played silence. `AUDIO_GAPS` logged every gap as one block with the whole ring cleared.
- `1e4d419` mixes one block every 5.33 ms by the performance counter (a high-resolution waitable timer, no spin). The device buffer only corrects drift: no room slips the schedule a block; under one block queued gains one. Silent blocks: 46.6% -> 0.1%.
- The rest were writes a few ms late. `AUDIO_LAG_BLOCKS` (default 1, live) reads one block behind the published index, at 5.33 ms more latency: 0 silent blocks in the dump over 90 s of walking, and jumps at block boundaries no more frequent than at mid-block.
- Also in that commit: the mixer thread is on MMCSS "Pro Audio" (`AUDIO_MMCSS=0` turns it off), the WASAPI buffer is `AUDIO_BUF_MS` (default 40, was 20) of headroom, and AUTOCONVERTPCM lets a device that is not 48 kHz stereo resample.
- soyjxck's ps3recomp fork found the same pairing on Drakengard 3 (`be94e5a5`, `c42360b7`). This is a reimplementation of that idea, not a merge.

## Controller: DualSense over HID (`4efcb5e`)

XInput does not see Sony pads, so a DualSense was an empty slot unless Steam Input or DS4Windows stood in. `libs/input/pad_hid_sony.c` reads DualSense, DualSense Edge and DualShock 4 input reports through hid.dll / setupapi (in the OS; no SDL2 or other new dependency), over USB and Bluetooth, in both report formats each pad uses. Sony pads take the ports XInput left empty.

- Mapping: face buttons, d-pad, L1/R1/L2/R2/L3/R3 as on a PS3 pad; Create/Share and a touchpad click are SELECT, Options is START; analog triggers and sticks pass through.
- Hot-plug: a scanner thread rescans every 2 s; each pad's reader thread blocks in ReadFile, so it costs nothing when idle. The log says `[cellPad] DualSense connected (HID Bluetooth, PID 0CE6) as HID pad 0`.
- `PAD_NO_HID=1` turns it off: if Steam Input already presents the pad as an XInput device, the game would otherwise see it twice (on port 1, which Demon's Souls ignores). `PAD_HID_DEADZONE` (default 10 of 128) centres a stick resting a few counts off 128.
- The keyboard (arrows, Z/X/A/S, Q/W, 1/2, Enter, Tab) now merges into port 0 with a pad: keys OR into the buttons and the arrows drive the left stick while held. Before, a plugged-in pad switched the keyboard off.
- Checked against synthetic reports for every layout. Not yet tried with the live pad (the paired DualSense was off). Rumble was not sent to Sony pads then; docs/95 adds it.
- soyjxck's fork does this with SDL2 (`46b12fec`), which this machine does not have; the HID reader avoids adding it.

## Movies (FMV, `5a63029`, `ee40816`)

Demon's Souls plays its movies through cellSail: `movie/ds_advertise.pam` (the intro advert after the logos, 130 s, 6-channel LPCM), `ds_title.pam` (behind the title screen, 10 s, stereo), and `ds_opening`, `ds_oldtale`, `ds_goodending`, `ds_badending`. All are PAMF: a header, then an MPEG program stream with MPEG-2 video at 1280x720, 29.97 fps (PES 0xE0) and 16-bit LPCM at 48 kHz (private stream 1, sub-stream 0x40). cellSail used to end every stream as soon as it started.

How a frame gets to the screen:
- The game calls `GraphicsAdapterGetFrame(adapter, &info)` once per game frame from its render thread and, when it returns a buffer, points a texture at `info.pBuffer` (A8R8G8B8 linear, remap 0xA993: it asked for RGBA bytes, so the remap rotates the channels and forces alpha to 1). It draws the movie quad (fp 0x0079FE81 = texture x vertex colour) only on a frame GetFrame filled.
- The buffers are the game's own: the library calls the adapter's `pAlloc(pArg, size, num, ppFrame)`, and Demon's Souls allocates them in RSX local memory (0xC4C23500 ...). We ask for 4 per stream and give them back through `pFree` on close.
- When no new frame is due, GetFrame hands the current one out again. Answering EMPTY flashed the screen black every time the game ran faster than the movie.
- Sound: the game's mixer calls `SoundAdapterGetFrame(adapter, 256, &info)` and reads 256 frames of big-endian float PCM from `info.pBuffer` (our scratch page at 0x59800000). It reads the format once per stream with `SoundAdapterGetFormat`. The channel count comes from the PAMF reader on the stream database (below). Movie sound is always handed out as stereo (`7f48932`).
- The end: once the last frame has been handed out, the frame fetch sends SOURCE_EOS, STOP_TRANSITION and OPENED, which is what the game waits for before closing the stream.

Why it played black at first: the d3d12 backend gave every 1280x720 sampler with no render target at its offset a copy of the rendered frame (`19d4613`, meant for surfaces the GPU rendered, whose guest memory is never written). The movie frames are exactly display-sized, so the quad sampled last frame's backbuffer. `ee40816` keeps that substitution only when the guest memory there is empty. The DeS display pass (fp 0x181 sampling 0x7A0000) still gets it.

### Movie sound was noise (fixed, `7f48932`)

The user heard broken sound in all three movies. Diagnosis from `AUDIO_WAV` captures of the final mix, compared with each movie's LPCM decoded offline from the `.pam` file (a short script; nothing game-derived is kept):
- `ds_title.pam` (stereo): stretches that matched the source sample for sample (gain 1.000) alternated with seconds of uniform white noise in [-1, 1], plus stretches with left and right swapped. The noise was the source read one byte off. A PAMF LPCM payload can end in the middle of a sample, and the sample's other byte starts the next packet. `feed_lpcm` decoded each packet on its own and dropped the odd byte, so every sample after it was misread until another odd packet realigned the stream. The swaps came from the overflow path, which dropped one float instead of one frame. Both are fixed. The title movie is now bit-exact for its whole length.
- `ds_advertise.pam` (5.1, L R C Ls Rs LFE in the file): the game reads the LPCM channel count with `cellPamfReaderGetStreamInfo` on the PAMF reader that `cellSailDescriptorCreateDatabase` builds, and sets up a stereo or a 5.1 mix from it (FUN_00fdad08: count 2 uses the object at +0x734, count 6 the one at +0x738). Through the 5.1 path, test tones (`SAIL_AUDIO_TONE`) came out mono, at the wrong pitch and about -45 dB. Stereo test tones handed out while the database still said 6 channels came out scrambled. So 5.1 is folded to stereo in `sail_movie` (ITU-R BS.775 weights, scaled so they cannot clip, LFE dropped), and the database's copy of the header reports 2 channels. 99.9% of the advert's 256-sample blocks then match the reference downmix bit for bit. The exception is one dropout block in 25 s with no lasting offset, a hitch while the guest runs at about 95% CPU. `SAIL_AUDIO_DOWNMIX=0` hands 5.1 out again, in the L C R Ls Rs LFE order that layout 8 promises, for a title whose 5.1 path works.
- The game pulls sound at about real time: about 194 GetFrame calls of 256 frames a second, against 187.5 nominal, measured over 10 s.

### Movies at 15 fps and 1 fps (fixed, `ad2d649`)

During the advert the game flipped 32 times a second but the backend presented 14. `d3d12_clear_display` holds back a display batch smaller than a quarter of the largest frame seen, as an intermediate of a double-buffered title. That max covered the whole run. Movie frames are 5 draws: against a max of about 30, one frame was "small" and two were not, so frames went out merged in pairs (15 fps, and the first of each pair was never shown). After the 1,700-draw level frames, a movie frame would only be presented after a few hundred draws had piled up: the 1 fps the user saw in `ds_opening`, which plays after character creation.
- `RSX_STALE_FLIPS` (default 4, 0 = off): if no present has happened for that many flips, the current batch becomes the new max.
- If three presents in a row each merge two or more flips and are still under half the max, the max is halved.
- Neither fires where a full frame arrives at least every other flip, which is the case the rule protects (the Demon's Souls level). A per-present decay was tried first: it let a G-buffer intermediate of the level through as a frame (regress.sh `s4_on` came out as a dark red buffer), so it was dropped.
- Advert: 31 presents/s, 0 merges. Title movie: 31/s. Level walk: all shots ok, about 28 presents/s. The opening was not reached here.

Measured (`017dd06` counters): the game calls GetFrame about 31 times a second and gets 29.4 new frames a second during the advert (the movie is 29.97). Sound calls deliver about 48,000 frames a second. Screenshots (`SHOTS=` / `CELLMARK_DUMP`) stall the frame loop and make the movie look choppy; judge smoothness without them.

Tools and knobs:
- `PS3_SAIL_MOVIES=0`: never play; every stream ends at once (the old behaviour).
- `PS3_SAIL_MOVIES=<text>` (`96757fc`): play only the movies whose path contains `<text>`; the rest end at once. `PS3_SAIL_MOVIES=title` reaches the title movie in about 50 s without sitting through the 130 s advert, which CROSS does not skip.
- `PS3_SAIL_SKIP=<text>[,<text>...]`: never play the movies whose path contains one of the texts; the rest play.
- `PS3_SAIL_SKIP_FIRST=<text>[,<text>...]`: the same, but only the first time each such movie opens in a run. `DES_SKIP_INTRO_MOVIE=1` sets it to `ds_advertise.pam`.
- `SAIL_AUDIO_TONE=<peak>` (`96757fc`): replace the movie's sound with one sine per channel, 250 Hz x (channel + 1). Pair it with `AUDIO_WAV=<file>` and look at the spectrum per channel.
- `SAIL_AUDIO_DOWNMIX=0` (`7f48932`): hand 5.1 out as 5.1 (default: stereo).
- `RSX_STALE_FLIPS=<n>` (`ad2d649`): flips without a present before the frame-size max counts as stale (default 4; 0 = never).
- `VBLANK_MHZ=<n>` (`6dae2e5`): the vblank rate in millihertz (default 59940; 60000 = 60 Hz).

### Choppy movies and frozen subtitles (fixed, 2026-10-14)

After `ad2d649` the user reported the movies at a steady 31 fps but slightly choppy, and the opening movie's subtitles frozen on one line.
- **Subtitles** (`0dc0854`): the game times them with `cellSailGraphicsAdapterPtsToTimePosition(adapter, pts, &pos)` (NID `0x1872331B`, from `FUN_00fe0138`; `FUN_00fd8cc4` divides `pos` by 1e6, so the unit is microseconds). It was an unresolved import faked as CELL_OK, so `pos` was never written. It now returns `pts * 100 / 9`; our pts are 90 kHz from 0. Measured: called once per frame, frame 300 of the title movie gives 9.98 s. After adding it, regenerate the NID table: `python ps3recomp/tools/gen_hle_nids.py --all --out work/recomp/hle_nid_table.cpp` (the diff should be just the two new entries).
- **Vblank rate** (`6dae2e5`): the ticker in `boot_main.cpp` scheduled vblanks with `GetTickCount64` + 16 ms, which is 62.5 Hz. `GetTickCount64` also moves in ~15.6 ms steps, so ticks came in bursts. The game flips on every other vblank, so it ran at 31.25/s against the movies' 29.97, and one movie frame a second was shown twice. It now runs on the performance counter at 59.94 Hz, and the game flips 30/s. The level regression walk is unchanged.
- **Frame choice** (`d57cea9`): `sail_movie_video` handed out the newest frame due by the wall clock. With the game fetching at the movie's own rate, a millisecond of jitter at a frame boundary gave two frames in one call and none in the next. Frames now go out one per call, half a frame early, and are skipped only to catch up more than two behind. Title movie: 297 of 300 frames shown, 253 before.
- The user confirmed the movies look right after these three.
- Log: `[sail_movie] <file>: video 1280x720 @ 30000/1001, LPCM 6 ch (handed out as 2)` on open; `[cellSail] 4 frame buffers of 0x384000 bytes from the title, first at 0x...`; `[cellSail] stream ended`.
- `017dd06` (diag): `[cellSail] video GetFrame #N: <handed out>, playing=<0/1>, last 300 calls <n>/s, new frames <n>/s` every 300 calls; `[cellSail] sound GetFrame #N: <filled>` every 2,000; `[sail_movie] decoded N frames` every 300.
- To watch a movie in a test run: `PRESS_UNTIL="PlayerStart" WHEN="stream ended" tools/des/desprobe.sh out.txt 260` (no `DES_SKIP_INTRO`; pressing stops once the advert starts, so it is not skipped).

Not done: AVC video and ATRAC3+ or AC3 audio in PAMF (Demon's Souls has none); `UpdateAvSync` is ignored, so audio and video each follow the wall clock; non-Windows hosts keep the old path (no decoder).

A bug found on the way (fixed before the commit): skipping a movie with START closed it on the main thread while the mixer was inside SoundAdapterGetFrame, a divide by zero on freed memory. The movie pointer is now read under a lock that no guest callback is made under.

## Fork review (2026-10-14)

Forks and open PRs of upstream ps3recomp, checked against this tree:
- Taken: robbe1912's CMP vertex type (`6dee83f`), upstream PR 278 predicated select (`27202ad`). soyjxck's SDL2 pad was redone over HID (`4efcb5e`).
- Not useful here: zdiemer's commits and PRs 279/280/282 target the opt-in live-draw engine (`RSX_LIVE_DRAW`), not the D3D12 path; PR 281 (AA surfaces) — Demon's Souls uses none (surface formats 0x148/0x14D/0x150, AA bits 0); `387d1faa` and `7db67309` are already covered; tekulvw's cellSail series is tied to Metal and FFmpeg.
- andrebrumdev/gow2-recomp (God of War II, playable, same ps3recomp lineage, macOS/Metal, engine code private): ideas worth taking later are profile-guided devirtualisation of indirect calls, config-declared mid-function hooks, RPCS3 `patch.yml` support and clamping transform constants to 0-467.

## Shader stutter: disk cache and precompile (ps3recomp `ee98227`, `c055090`, 2026-10-15)

Every guest program was decompiled to HLSL and compiled with FXC the first time a draw used it, in every run. FXC takes about 40 ms for a fragment program and over 100 ms for the large vertex programs. With PERF=1, one boot to the first level compiled 139 programs and spent 5.9 s in FXC, in bursts as each new area or effect appeared.

- **Disk cache** (`ee98227`):
  - Compiled programs are kept as `<profile>_<hash>.dxbc`, keyed by the HLSL text, so a decompiler change simply misses.
  - Location: `PS3_SHADER_CACHE`. `des_env_defaults` sets it to `gamedata/shader_cache`, next to the saves and outside the staged exe's directory. `=0` turns the cache off.
  - Second boot to the same level: 0.03 s compiling.
  - `CreateGraphicsPipelineState` then costs about 0.04 s per run, because the driver keeps its own cache, so there is no pipeline library.
- **Unused texture units no longer change a program:**
  - A fragment program's HLSL depends on what kind of texture each unit holds: cube, shadow (depth compare) or depth read as colour.
  - It was declared from every bound unit, so a cube map left on a unit the program never samples gave another text and another pipeline.
  - Only the sampled units count now (`rsx_fp_texture_units`): 92 texts became 52 in the same run.
- **Precompile before the first boot** (`c055090`, port `tools/des/precompile_shaders.sh`):
  - `play.sh` runs it before the game. It takes about 20 s once, then a few seconds.
  - It unpacks `USRDIR/shader/*.shaderbnd` with `tools/des/shader_extract.py`. Each archive is a BND3 whose entries are a 0x20-byte FromSoftware header followed by a standard `CgBinaryProgram`; the extracted files go to scratch and are deleted afterwards.
  - ps3recomp's `rsx_shader_precompile` then builds every program's HLSL through the backend's own code and compiles all of it on every core: 1421 programs, 3535 shaders.
  - Vertex ucode is byte-swapped to the word order `rsx_state` keeps, and branch targets are made relative to the program's `instructionSlot`.
  - The fragment binaries carry no parameter tables (FromSoftware's header holds the constants), so texture kinds come from the game's unit conventions:
    - `--unit 7=shadow,8=cube,11=cube,...`: the shadow map is always on 7 and cube maps on 8 and 11;
    - `--alt 1=d2c,2=d2c`: post effects that read depth on 1 or 2.
  - The runtime appends each (program, kinds) pair it compiles to `fp_variants.txt` in the cache. The precompiler rebuilds all of them, so a cache emptied by a decompiler change refills before the next boot.
  - Measured on a first boot to the level after the precompile: 4 programs compiled at draw time (0.04 s), against 139 (5.9 s). The four are vertex/fragment programs the game changes after loading them (a different `o[0]` write, a different texcoord input).
- **Tools:**
  - PERF=1 adds `[fxc <time> <n>, disk <hits> | create pso <time> <n> | misses <time>]`.
  - `PS3_SHADER_CACHE_HLSL=1` writes each program's source beside its `.dxbc`, which is how the precompiled and runtime texts were compared.
- **Still costly:** the pipeline lookup itself. `vp_get_fp_pso` is called about 4 million times in a two-minute run, for about 0.5 µs each (2 s), as a linear search with a `getenv` and a ucode hash per call.

## Frame pacing and hitches (ps3recomp `b7e3a52`, `78d26a0`, `97aaeeb`, 2026-10-15)

The user reported frame drops while the counter said 30, and stutter that was not shader compiles.

**Measuring.**
- `FRAME_LOG=<file>` writes one line per present. A full frame has hundreds of draws; a skip frame (the pacer's 15 fps mode, see Frame rate) has 1-2, so the draw column counts skip frames.
- Runs of the same build differed by 3x in skip frames. The cause: `PS3_AFFINITY=all` passed as a run argument reached the exe, which parsed it as hex 0xA, so every such run was pinned to two logical CPUs. The console `trace` timeline showed it: `usleep(1000)` took 2 ms at the median and up to 32 ms, on the thread that posts the semaphore HighGraphics waits on, with only ~1.9 cores busy. `78d26a0` makes "all" mean no limit. `play.sh` sessions were never affected (they unset it).
- With every CPU, walking the first level: 29.8 presents/s, skip frames 3-4%, frame-to-frame intervals 33.3 ms median and 35.5 ms p90. The pacer's limit is 36.3 ms, so there is little headroom.

**Fixed.**
- **A 100-400 ms hitch** when the busiest frames first appear (in the walk about 5 s after the spawn). The vertex upload buffer (256 bytes per vertex, double-buffered) started at 192 MB. The first frame needing more than 393k vertices created a 384 MB replacement on the render thread (114 ms), and a frame with 854k vertices then needed 768 MB (381 ms); the overflowing draws of that frame were dropped as well. `VP_VB_VERTS=<n>` (`b7e3a52`) sets the starting size, and `des_env_defaults` starts it at 1,572,864 vertices (768 MB), the size the level grows it to. Growing on a worker thread once a frame used three quarters of the buffer was tried and never fired: the vertex count jumps from under 3/4 to over the size in one frame.
- **Write-back before Present.** Each frame waited for the GPU between submit and Present to copy render targets back to guest memory (the exposure value), on the thread that also drains the FIFO. The store now runs at the start of the next frame (`RSX_WRITE_COLOR_DEFER`, default 1; `0` restores the old order). Frame start to Present, p90: 14.0 -> 11.1 ms with all CPUs. `regress.sh`: ok, exposure looks right.

**Tried, no effect.** Presenting only at vblank ticks (`FLIP_AT_VBLANK`, removed). Opting out of Windows power throttling (EcoQoS, timer resolution; not committed).

**What RPCS3 does** (RPCS3 issues, PRs and source; their wiki was not reachable):
- No Demon's Souls-specific pacing fix was found. Issue #17167 (stutter and unstable pacing, open) is the closest; users report that raising "PPU Threads" fixes it there.
- The general work is precise vblank timing (#10496, #11070 for 59.94 Hz) and flip handling (#12045, #12052, #12108).
- Its frame limiter spaces presents as `max(now, target) + period`, so a late frame catches up by one period.
- It raises the Windows timer resolution to 0.5 ms (`NtSetTimerResolution`); we use `timeBeginPeriod(1)`.
- The "Unlock FPS" patch in RPCS3's patch.yml sets the byte at pacer object + 0x6ED to 1 (BLUS30443 `0x25ED8`), which looks like the pacer's no-skip flag.

**Still open.**
- About one 50-60 ms frame when a new area comes into view: `RF_SLOW_MS=30 PERF=1` splits it into ~35 ms of texture uploads and ~11 ms creating ~22 pipelines first seen in that run.
- Present spacing still ranges 30-40 ms. On a 143 Hz display without VRR a 30 fps frame is shown for 4 or 5 refreshes. A present limiter in the RPCS3 style would need the present off the FIFO thread, because holding the present there holds the drain.
- The 256-byte vertex stride: only the enabled attributes need uploading.

**Tools** (`97aaeeb`):
- `FRAME_LOG=<file>`: per present, the time, interval, frame start -> Present, time in Present, flip word -> Present, flips and draws in the batch, DXGI statistics (compositor-dependent, not reliable), the GPU wait at frame start and the deferred store.
- Console `trace [s] [file]`: every syscall and HLE call (start, duration, thread, call, r3), plus marks from the render side (flip word, semaphores, labels, kicks, render_frame, Present), as a text timeline. Read it with a script: who posts the object a thread wakes on, how long sleeps really take.
- `RF_SLOW_MS=<ms>` (live): a render_frame over that prints its time per phase; with `PERF=1` also the pre-pass split (texture uploads, SRVs, pipeline lookups and creation, shader compiles).
- **F11 profile sessions** (`686645c`): F11 in the game window (or the console's `profile`) starts a session and the next press stops it; the caption shows `[profiling #N]` meanwhile. Files go to `PROF_DIR` (`des_env_defaults`: `work/tmp/prof`) as `prof_NNN_*`:
  - `_summary.txt`: average fps, 1% low, interval percentiles, presents over 36.3 ms, skip frames, render_frame percentiles and mean phases, and the 15 worst presents with where their time went.
  - `_frames.txt`: the same per present.
  - `_threads.txt`: CPU per thread every second and over the session, and the `waits` report.

  An interval much longer than its render_frame means the game was late with the frame, and `_threads.txt` says on what. A long render_frame is the renderer. For the player to mark the stretch that felt bad. First session on the scripted walk (37 s): 29.9 fps, 1% low 21.3, 113 presents over 36.3 ms, one 81 ms present from new textures, and ~6 ms per frame in the deferred write-back store.

## Boletarian Palace: frame rate and freezes (2026-10-15)

The user's save is in Boletarian Palace, which performed much worse than the first level, and the Tutorial and Nexus still had micro-stutters. Measured on the user's save with a scripted walk (F11 profile session, `PS3_AFFINITY=all`).

| Build | presents/s | skip frames | notes |
|---|---|---|---|
| start | 24.2 | 50% | heavy view (~6,400 draws): render_frame ~21 ms |
| SRV memo (ps3recomp `a0833f7`) | 24.3 | 50% | render_frame mean 19.0 -> 13.7 ms |
| lifted chunks /O2 (port `a4bfc8f`) | 27.7 | 26% | main <-> ChrInsManThread hand-off was ~70 ms a frame |
| FP pipeline lookup (`f6c8517`) | 29.6 | 25% | heavy view render_frame 21 -> 10.5 ms; PSO lookup 2.85 -> 0.25 ms |
| VP lookup memo (`990b1dc`) + freeze fix (`a8b18af`) | 29.8 | 25% | longest frame 66 ms |
| all of des_boot /O2 (port `20940b9`) | 29.5 | 23% | HighGraphics 43 -> 31% of a core, main 33 -> 24% |

Light areas (up to ~3,000 draws) now hold 30. The heaviest view still alternates render and skip frames (pairs of ~66 ms).

**Fixed.**
- **Per-draw SRVs** (`a0833f7`): every draw re-created its texture views. A memo per slot keeps a view while its resource, format and mapping are unchanged. `RSX_SRV_MEMO=0` disables it.
- **Fragment-program pipeline lookup** (`f6c8517`): every draw hashed its fragment program and scanned a list for the pipeline. Now the hash is memoized per program address (checked against the bytes; `RSX_FP_HASH_MEMO=0` disables it) and pipelines are found through a hash bucket.
- **Vertex-program lookup** (`990b1dc`): sizing, hashing and scanning per draw. Memoized per program start until the program store is written (`RSX_VP_MEMO=0` disables it). Three `getenv` calls per draw became knobs.
- **/O2** (port `a4bfc8f`, `20940b9`): the lifted chunks, then the runtime sources in des_boot (`ppu_loader.cpp` holds the guest memory accessors). `DES_PPU_OPT=OFF` goes back to /Od.
- **Multi-second freezes** (`a8b18af`): once or twice a minute while walking, no presents for 6-9 s (7-20 s with heavy logging). How it was found:
  - The console's `stat` (flip count) polled twice a second caught the freeze. `ctx 4` then showed HighGraphics in `usleep(30)` polls returning to 0x00ACB0A0.
  - That is `func_00ACAFA8`: it posts command 0x23 (`0x9023` = 0x90 payload bytes) into a 1 MB ring at 0x45A4B280 (control block at 0x01B51000: read pointer +0, write pointer +0x80) and waits for a reply word at 0x01B50FD0.
  - `PPU_WWATCH` showed no PPU store sets the reply. `SPU_DMA_RANGE` showed SPU image 2 (`spu_0001_at_0176C200`, the HighGraphics ring consumer) PUTting it from pc 0x0C408. The reply is a buffer from a ~6 MB circular pool (0x49E0xxxx-0x4A40xxxx). Image 2 also writes the RSX put register and reads GCM label 0x81.
  - During a freeze the consumer still worked through the ring 16 bytes at a time, at a fifth of its normal rate. `SPU_LLAR_HIST=2` showed why: every GETLLAR of the write-pointer line became a 2 ms nap (955 GETLLARs, 955 naps per 2 s).
  - The idle back-off naps after 20,000 GETLLARs of an unchanged line, and the consumer reads the write pointer once per entry. With HighGraphics blocked on the reply, that pointer never moves, so a full ring took ~9 s to drain.
  - Now any DMA since the previous GETLLAR restarts the count. Five walks had no freeze (three timed by reply, at most 234 ms between replies; two by frame time, longest frame 66 ms), and the consumer still naps ~400 times a second when idle. A smaller backlog would have cost hundreds of ms the same way, so this may also be the Tutorial and Nexus micro-stutters (not yet re-checked there).

**Still open.**
- Heavy view: the render thread is at ~60% (FIFO drain: `rsx_fetch_vertex`, `upload_strip_vp_indexed`; `rt_writeback_store`), the SPU threads at ~45% each.
- The deferred write-back store, present pacing, texture-upload hitches and the 256-byte vertex stride (see Frame pacing).

**Tools.**
- Stall watch (scratch script; the console commands are general): poll `stat` for the flip count; when it stops, take `ctx <tid>`, `mem <addr> 64` on the registers, `cpu 1` and `prof <host tid> 2`.
- `SPU_DMA_RANGE=<lo>-<hi>` lines now carry the time (ms), the image and the first word moved (`15bac63`). `SPU_DMA_RANGE_MAX=<n>` (0 = all) and `SPU_DMA_RANGE_IMG=<id>` are new.
- `SPU_LLAR_HIST=<image id>`: every 2 s, that image's busiest GETLLAR lines and its idle-nap count.

## Palace choppiness and Tutorial micro-stutter (ps3recomp branch `perf/frame-time`, 2026-10-15)

The user's F11 profiles: the Palace still felt choppy, and the Tutorial stuttered on camera turns. They also asked whether batching draws would help. The heaviest Palace view has ~6,000 draws, but neighbouring draws differ in shaders, textures or constants, so they cannot be merged; the per-draw cost was cut instead (SRV, VP and FP memos above).

| Run | presents/s | presents over 36.3 ms | skip frames |
|---|---|---|---|
| Tutorial walk, before the write-back fix | ~30 | 36 of ~1100 | |
| Tutorial walk, after (p99 35.5 ms) | ~30 | 5 of ~1100 | |
| Palace walk, port `20940b9` | 29.5 | | 23% |
| Palace walk, after (1% low ~18 fps, p99 ~53 ms) | 29.6 | ~330 of ~1090 | 11-12% |

**Fixed.**
- **Late vblanks** (`ca23b8b`): the render loop marked vblank ticks between drains, and the handlers ran at the next HLE call of any guest thread. The semaphore HighGraphics flips on came 7-25 ms apart instead of 16.7, and every late one pushed a frame past 36.3 ms, so the game's pacer dropped to 15 fps. A clock thread now ticks at 59.94 Hz and runs the handlers itself on a guest stack of its own. `GCM_VBLANK_THREAD=0` restores the old ticking; `VBLANK_MHZ` sets the rate in millihertz (default 59940).
- **SPU per-step and channel overhead** (`6cba5d4`): image 2 (the HighGraphics ring consumer) makes ~12 M trampoline hops and ~20 M channel writes a second. The MFC parameter channels and the per-step check now skip the diagnostics unless one is set. Image 2's busy time per Palace frame: ~38 -> 31-33 ms.
- **Event acknowledge without the lock** (`47468c7`): event bits are changed atomically, so `WrEventAck` (~2 M/s on image 2) no longer takes the lock-line lock, and the lock and the coherence flag have cache lines of their own. The unit test races acks against re-reserves and checks that no lost-reservation event is dropped.
- **Texture creation on camera turns** (`a9ef735`): a Tutorial turn bringing ~67 textures into view took 58 ms, 30 ms of it in creating two committed resources per texture. Uploads now decode into one staging buffer per frame (`TEX_STAGE_MB`, default 32, grows up to `TEX_STAGE_MAX_MB`, default 512), and textures are placed in pooled 64 MB heaps (`TEX_PLACED=0` goes back to committed resources). That frame: 18-20 ms.
- **Render-target write-back** (`1fa13f0`): ~25 MB a frame was converted a byte at a time with the format test inside the loop, 3.5-6 ms at the start of every render_frame. Now a word at a time with one loop per format: 3.8 -> 0.5 ms a frame. This was the common factor in the Tutorial frames just over 36.3 ms.

**Tried, no gain (not committed; patches in the session scratchpad).**
- **GETLLAR without the lock** (a sequence-number read): correct and unit-tested, but no change in image 2's rate. The lock line's cost is the cache traffic between the PPU thread writing the ring's write pointer and image 2 reading it, and that line moves either way.
- **Guest memory accessor fast path**: `vm_read*`/`vm_write*` in `ppu_loader.cpp` run every diagnostic hook on each access, which is ~26% of HighGraphics' samples. A version that tests one "any diagnostic armed" flag first ran the Palace at the same 29.6 fps, because HighGraphics is not what limits it. One of the two Palace runs with it, and one of the user's, stayed on the loading screen. Reverted.

**Still open.**
- The Palace is limited by image 2 and HighGraphics together. HighGraphics builds a frame in ~25 ms, then waits for the vblank semaphore and image 2's reply before it flips. Image 2 naps ~36% of the time, so it is often waiting for work. The remaining skip frames are the heaviest views.
- The Tutorial's worst camera-turn frames are 46-49 ms, mostly ~20 pipelines created on first use (~5.5 ms) plus texture decode.

**Tools** (`4bf9e11`).
- `SPU_RATE=<image id>`: every 2 s, that image's drain steps, slow-path channel writes, DMAs, GETLLARs, naps, nap share, and steps per busy ms. Steps per busy ms is the runtime's speed on that image's code. It varies by ~10% between sessions, so compare builds within one sitting.
- `SPU_LOCKLINE_STATS=1`: per host thread, the time per 2^20 lock-line acquisitions and the share that found the lock held.
- Console `trace`: SPU threads add `spu_nap`/`spu_wake` marks, and with `SPU_DMA_RANGE` set, `spu_put`/`spu_get` marks.

## Havok freeze: a lost task bit (ps3recomp `10e2690`, branch `perf/frame-time`, 2026-10-15)

The user hit a freeze in the Tutorial: the HUD stayed up, the title bar read about 9.7 fps and 1 draw, and nothing moved. It also happened about once in 13 loads of the Palace save, which made it reproducible.

**Signature.** MAIN waits on cond 13 (`ctx 1`: r3 = 0xD, lr 0x01328C0C). The Havok PPU Thread (tid 11) waits in `sys_event_queue_receive` on queue 5 (`ctx 11`: lr 0x013293DC, r31 = 0x477FD900).

**The handshake.** Each physics step, the Havok PPU thread starts its SPU tasks in taskset 0x477ED800 and then waits N times on a semaphore at 0x477FD900. Image 7 (`spu_0006_at_017AE200`) runs as tasks 0-3. Image 8 (`spu_0007_at_017C5900`) runs as tasks 4-7, which image 7 creates itself.
- The semaphore word: bits 24-31 are the event port (3), bits 16-23 the waiter count, bits 0-15 the count.
- Wait (`func_010D43E8`): `lwarx`. If the count is non-zero, `stwcx.` it minus 1. If not, add a waiter and `sys_event_queue_receive(5)`, then retry.
- The SPU's post (helper at LS 0x17638, many call sites) adds 1 with GETLLAR/PUTLLC. If there was a waiter, it removes it and sends one event on port 3.

At the freeze the word is 0x03010000 (one waiter, count 0). One post of the step never came.

**Why.** Image 7 starts an image-8 task by setting the task's bits in the taskset header with PUTLLC. The runtime clears an exited task's ready, pending and enabled bits on the host, and tasks 0-7 share one byte of each bitset. Those clears were plain read-then-write (`spu_taskset_task_exited`, `ts_start_task_locked`, `spurs_bitset_set/clear`), outside the lock-line lock. A PUTLLC that committed between the read and the write was erased, or the other way round.

Caught live: the step before ran image-8 tasks 4-7, the last step only 4-6. The header showed task 7 enabled but not ready, pending or running, so it never started and its post never came.

**Fix.** `spu_coh_rmw32` (`spu_coherency.c`) changes one big-endian guest word under the lock-line lock and fails any reservation on the line, as a store from another processor does. Every host-side bitset change goes through it. `test_spu_lockline_peer` races PUTLLC toggles of one bit against host set/clear of another. It fails with the old read-then-write (an SPU commit erased) and passes with the fix.

Soak with the fix: 40 loads of the Palace save with no freeze and no other stall. Before it: 3 freezes in 37 loads over three catcher runs.

**How it was found.** None of this showed in a log. Logging changes the timing enough that the freeze goes away.
- A catcher script boots the Palace save over and over. A good load is ended as soon as the level draws. A load stuck for 60 s with tid 11 at lr 0x013293DC is dumped through the console and left running. Every other stall is dumped and skipped.
- `spuhist` (`c7a621f`) showed the last step's four image-7 tasks ran to completion. So the post was missing, not lost in the event queue.
- `SPU_HIST_LINE=477FD900` added every PUTLLC and `stwcx.` on the semaphore line. In a good step it showed posts from image 8 as well as image 7. The dump at the first catch had shown image 7 only.
- Ruled out along the way: PPU `stwcx.` against SPU PUTLLC on the semaphore (both serialize on the lock-line lock), the event queue, WAIT_SIGNAL, and the 8-context coherency registry (its overflow warning appears in good runs too).

**Tools** (`c7a621f`).
- Console `spuhist [image] [n]`: the last n entries of an always-on ring of 64K SPU task events.
  - Task events: D dispatched, N not started (taskset shut down), S started, R lifted entry returned, X thread done.
  - Signals: G task signalled, W/w WAIT_SIGNAL park and resume.
  - P: a user event a SPURS task sent to a PPU queue (raw SPU thread groups are left out).
  - The `htid` ties the events of one task thread together. Filter by image after a hang: a busy image (12's WAIT_SIGNAL loop) fills the shared view in a second.
- `SPU_HIST_LINE=<hex ea>`: adds that word's 128-byte line to the ring.
  - L/F: SPU PUTLLC committed or failed.
  - l: PPU `stwcx.` commit.
  - s: plain PPU store, once an SPU has reserved the line.

  Each entry has the caller's lr and the word before and after.

**Still open.** Two older stalls, seen on builds from before this branch:
- Boot: MAIN on cond 0x23, black screen at 30 fps with 2 draws.
- Level loading screen: MAIN on cond 0x2E, about 186 draws.

The catcher logs them as "other stall" and keeps going.

## Palace frame rate: the render thread's vertex uploads (ps3recomp branch `perf/palace-fps`, 2026-10-15)

The heaviest Palace view (the save's spawn point, ~6,470 draws) presented every ~43 ms (median), and 29% of presents were skip frames (the game's 15 fps mode). A profile of the FIFO/render thread had it ~94% busy, nearly all of it writing VP vertex uploads: a frame fetched ~565k vertices and, with strips expanded to lists, wrote ~1.43M vertices of 256 bytes (~350 MB) to the upload heap. HighGraphics polls a label that thread writes before every flip, so its speed was on the frame's critical path.

**Fixed.**
- **Indexed strips** (`89ceb2c`, `VP_INDEXED`, live, default 1): strips and fans write each vertex once plus a u32 index list into a double-buffered index buffer, and draw with `DrawIndexedInstanced`. ~350 -> ~150 MB a frame. A draw that would not fit takes the old list path.
- **Shared vertex-constant banks** (`89ceb2c`, `VP_CB_REUSE`, live, default 1): a draw whose vertex constants and viewport z match the last 8 KB bank written this frame binds that bank instead of copying its own. `rsx_state.vertex_constants_gen` counts constant writes.
- **Constant template**: each vertex writes only the components its arrays supply on top of the draw's constants (`rsx_fetch_vertex_arrays`), instead of copying 256 bytes first.
- **Fetch threads** (`099343f`, `VP_FETCH_THREADS=<n>`, read once; default a quarter of the logical CPUs, at most 4; 0 = off): the FIFO thread walks a strip's indices and queues the vertices; worker threads fetch and convert them. Draws with an array type the plan does not convert itself, and the debug knobs `RSX_LATE_CHECK`, `RSX_DBG_VTX` and `VTX_POS`, keep the inline path. A queued vertex is read from guest memory later than the FIFO reached its draw, so the queue is drained (the FIFO thread helps) before anything the title can see: labels and reports (`g_rsx_sync_hook`, called from `rsx_process_method`), NV406E semaphores, the reference, user commands, flips, the 2D engine, `get`, and the frame's render.

Palace spawn view, same save, 15 s profile sessions:

| Build | median interval | skip frames | over 36.3 ms | render thread busy |
|---|---|---|---|---|
| master (`VP_INDEXED=0`) | 43.1 ms | 29.3% | | ~94% |
| indexed | 38.5 ms | 18.5% | | |
| + shared banks + template | 35.6 ms | 16.7% | | ~81% |
| + fetch threads (`VP_FETCH_THREADS=0` -> 4) | 35.5 -> 34.0 ms | 14.5% -> 13.3% | 190 -> 119 | 78% -> 47% |

Repeat runs of the final build: 13.1% and 14.3% skip frames, 1% low ~18-19 fps. The first run after a rebuild came out at 49% once, so one run is not enough for an A/B. `regress.sh`: no visual change.

**Tools.**
- `VP_VSTAT=<N>` (live, `de5aa7a`): every N frames, the vertices fetched from guest memory, the vertices written to the upload heap (and MB), the mean enabled-attribute count per fetch, and the indexed uploads by primitive.

**What limits it now.** The render thread waits ~40% of the time. HighGraphics waits ~47% (35% in the ring-space semaphore, 12% in usleep), and its busy time is about half guest-memory accessor hooks. Image 2 (the HighGraphics ring consumer) is ~80% busy:
- All of its atomics are on the ring's one 128-byte line (0x01B51080): ~660k GETLLAR+PUTLLC pops a second (pc 0x04618/0x04694) and ~770k empty polls (pc 0x05120, followed by the idle wait).
- Its busy time is spread across the runtime's channel, MFC and lock-line code (`spu_wrch_full` ~51% inclusive, `spu_lockline_lock` 9.5%, `spu_step_check` 4.4%, `mfc_do_transfer` 4.1%), with no single hot spot.

**Tried, no gain.**
- **Polling `put` before the FIFO walker's idle wait** (`FIFO_SPIN_US`, not committed): image 2 writes the RSX `put` by DMA and never kicks the walker, so each idle wait (1 ms) looked like latency in the chain. Spinning 500 us first made it worse: 22-23% skip frames against 13-14% (two runs each). The walker's wake-up latency is not the gate, and a spinning thread costs the others.

## Level loading (ps3recomp branch `perf/palace-fps`, 2026-10-15)

Loading the Palace save took ~20.5 s from the level load starting (the first `luabnd` read) to the level being drawn. Each DCX file is inflated by an SPU task (image 1, `spu_0000_at_01760280`, Edge zlib) that Core.Res.Decompressor feeds one 64 KB block at a time through libsre's `cellSpursQueuePush` (8A85674D, lifted).

**Fixed.**
- **Signals from the PPU** (`cb9de16`, `SPU_PPU_SIGNAL_WAKE`, live, default 1): the lifted `cellSpursQueuePush` signals the task with an lwarx/stwcx. on the taskset's signalled bitset, not through our `cellSpursSendSignal`. Nothing woke the task parked in WAIT_SIGNAL, so it saw each block only when another signal broadcast the condition variable: the audio mixer's, about every 10 ms. `ppu_stwcx32`/`ppu_stdcx64` now check a 256-way filter of the lines holding a parked task's bitset and wake the parked tasks on a hit.
- **Host inflate** (`35dca92`, `SPU_NATIVE_INFLATE`, live, default 1): the lifted inflate took ~3.5 ms a block. `spu_lifter.py --native-hook ADDR=SYMBOL` makes a lifted function call a host function first, and `scripts/build_spu.py` (`NATIVE_HOOKS`) hooks image 1's inflate wrapper at 0x8700 to `spu_hook_inflate_ls` (runtime/spu/spu_inflate.c, ~0.19 ms a block). It handles only a stream that ends with exactly the requested size and hands anything else to the lifted code. The hook is applied only to the image fingerprint it was written for. Every compressed segment in the game's DCX files (47,210, 3.0 GB out) decodes to zlib's bytes. After pulling this, run `python scripts/build_spu.py` to re-lift (only image 1 changes).

Palace save, wall seconds from launch (`loadtime.sh`-style runs: window, level load starts, level drawn with 200+ draws):

| Build | level load starts | level drawn | load |
|---|---|---|---|
| before | 38.8 | 59.3 | ~20.5 s |
| + PPU signal wake | 32.1 | 41.2 | ~9-10 s |
| + host inflate (`SPU_NATIVE_INFLATE=0` -> 1) | 34.6 -> 33.2 | 44.7 -> 37.3 | 10.1 -> 4.1 s |

The level-load start moves with the harness's menu presses (one every 2 s; the game sits at ~0.8 core in the menus), not with loading. With both fixes the level's file reads finish ~1.9 s after the load starts; the rest is the renderer ramping up. `regress.sh` shots match in content; the camera in `s3_turn` differs from the Oct 14 good set because the scripted turn now lands at a different moment.

**Tried, no gain.** The resource cache thread (Core.Res.CacheDeviceHandler) polls an lwmutex in a loop (~370k lock/unlock pairs during a load). Replacing the lwmutex's kernel semaphore with a WaitOnAddress lock word cut that cost, but the load was not faster (9.9 s with it, 9.0 and 9.6 s without); the thread is not on the load's critical path.

## Palace frame rate: runtime overhead (ps3recomp branch `perf/spu-regions`, 2026-10-15)

The user's profile deeper into the Palace (`prof_005`, 96 s) averaged 29.9 fps with 7.8% skip frames and a 1% low of 13.7 fps. Its slow frames have 8,000-10,000 draws and come in at 65-95 ms while the renderer's own share (`render_frame`) is 14-27 ms, so the game side was late. No thread is saturated in those seconds: 4.4-5.3 cores busy, the audio mixer's SPU thread ~75%, image 2 (the HighGraphics ring consumer) 50-80%, the render thread 50-65%, HighGraphics 40-70%. The limit is latency along the HighGraphics -> image 2 -> FIFO walker chain rather than any one thread's CPU, so these changes cut per-operation cost on that chain.

**Fixed.**
- **PPU memory watches** (`c477483`): `barrier_watch_hit` (PPU_WVAL, PPU_WWATCH), `ppu_rwatch_hit` (PPU_RWATCH) and `vm_hotmap` (PPU_HOTMAP) ran as real calls on every guest load and store. They are now an inlined flag test with the body out of line. They were 7.5% of HighGraphics' samples in the heavy view and are gone from its profile.
- **Debug knobs on the draw and FIFO paths** (`8fa3ee5`): seven getenv calls per draw, bind or command (~200k a second, ~3% of the render thread) are cached `PS3_KNOB`s.
- **Parked-task wake** (`9ad2466`): a PPU store-conditional on a signalled line woke every task parked there. Now only a task whose own bit is set gets woken.
- **SPU regions** (`bc329d6`, port `scripts/build_spu.py` `REGIONS`): the function detector seeds a function at every branch target, so a lifted image was one C function per basic block and every branch was a trampoline hop: ~20 million a second on image 2. `spu_lifter.py --regions` lifts the functions between two call targets as one C body, and a direct branch inside it becomes a goto (`SPU_RGOTO`). Every function keeps its exported entry, so the function table is unchanged. A goto takes the trampoline whenever the drain loop has per-hop work: a pending interrupt, lockstep, step tracing and similar. `SPU_GOTO_OFF=1` (live) sends every branch through the trampoline, for A/B. Enabled for image 2 (`spu_0001_at_0176C200`) and the audio mixer (`spu_0010_at_01818080`). After pulling, run `python scripts/build_spu.py` to re-lift (those two images change).

Same binary, the user's current save, two runs each:

| | regions on | `SPU_GOTO_OFF=1` |
|---|---|---|
| audio mixer SPU thread | 48-50% | 64-65% |
| image 2 | 82-83% | 86-89% |

Compared with the previous lift (one function per block), the mixer is ~6% lower and image 2 ~4% lower. Entering a region body costs more than entering a small function (its prologue saves eight registers and xmm6-15), which is why `SPU_GOTO_OFF` comes out slower than the old lift. Every Demon's Souls image lifts with `--regions` to the same function table, but only these two have been run. `regress.sh`: ok.

**Measured along the way.**
- Image 2 (`SPU_RATE=2`): 2.6M DMAs, 2.3M GETLLARs and 12M slow-path channel writes a second, but only ~0.66M ring commands. Most of its busy time is polling an empty ring, so it waits on HighGraphics more than it holds it up.
- The lock-line lock (`SPU_LOCKLINE_STATS=1`): image 2 takes it ~3.2M times a second (18% contended), HighGraphics ~1.2M (13%). Every PPU store to a line any SPU has ever reserved goes through it.
- HighGraphics' usleep(30) polls (`PS3_POLLTOP=4`) come from one site, lr 0x00ACB0A0 (FUN_00acafa8): it writes command 0x9023 into the ring and spins until image 2 writes a reply back, a full round trip per call. About 1,100 polls a second, too few to trip the usleep streak timer, so these are precise spins.
- At the user's current save point the game holds 30 fps (90 presents in 3 s), so the slow spots in `prof_005` are elsewhere along their route.

**Next.** Regions for the other images (Havok, the decompressor) once they have been run. A cheaper region entry. Fewer lock-line acquisitions on image 2's ring line, without repeating the seqlock dead end (no gain).

## Frame-rate unlock: DES_FPS (ps3recomp `afb30e6`, port, 2026-10-16)

RPCS3's "Unlock FPS" patch for BLUS30443 (Gibbed, Whatcookie; `patches/patch.yml` in an RPCS3 install) changes two things, and both turn out to be game settings rather than code:

- `stb r29,0x6ed(r31)` at 0x00025ED8 becomes `stb r0` (r0 = 1). That is the constructor (0x00025DD8) of an engine object whose pointer lives at 0x01AD6298. Bytes +0x6EC and +0x6ED are a flip mode; the game's debug menu prints it (0x000290A8) as `FlipMode【30FPS】` (+0x6ED 0, the default), `FlipMode【30FPS(NO SKIP)】` (+0x6ED 1, +0x6EC 0) and `FlipMode【60FPS(NO SKIP)】` (both 1). The default mode is where the skip frames come from: a late frame is replaced with a near-empty one (the "under 50 draws" frames in the profile summary). 0x000265F8 is the menu's setter.
- `lfs f31,-0x7f78(r30)` at 0x0001B964, in the frame update 0x0001B8D0 that main calls once a loop, loads the frame's time step: a float at 0x018C8DB4 holding 1/30. The patch replaces it with a branch to code that measures the time since the last frame (mftb), capped at 50 ms. `PPU_RWATCH=018C8DB4,4` shows 0x0001B8D0 is its only reader (`00461f1` makes the watch print every reader function once, not just the first 24 hits).

`DES_FPS=60` (port `port/des_options.cpp`) sets 60FPS(NO SKIP) once the object exists and rewrites the float every frame with the time since the previous flip request, capped at 50 ms. `DES_FPS=30` sets 30FPS(NO SKIP) the same way, with the same real frame time. The time is taken on the guest thread that asks for the flip, through a new runtime hook (`g_gcm_flip_request_hook`, `afb30e6`); HighGraphics calls `_cellGcmSetFlipCommand` once a frame, and main ran 336 updates for 334 presents, so the game advances by real time. The frame limit is the vblank, so `VBLANK_MHZ=120000` would let 60 mode run up to 120 (not tried; RPCS3 warns of physics trouble above 60).

Unattended, 10 s profiles, `RSX_PRESENT=0`:

| save | default | FlipMode 60 poked live | `DES_FPS=60` |
|---|---|---|---|
| Palace spawn (heaviest view) | 30.0 fps, 1.3% skip | 32.9 fps, 0 skip | |
| the user's current save | 30.0 fps, 0.3% skip | 34.7 fps, 0 skip | 33.8 fps, 0 skip |

So the unlock works, and the skip frames are gone, but in these views the game manages 33-35, not 60. The limit is throughput along HighGraphics -> image 2 -> FIFO walker: with 60 mode on, image 2's thread runs at 90% of a core, the render thread 81% (its `render_frame` share is only ~10 ms a frame; the rest is walking the FIFO) and HighGraphics 66%, and every other thread mostly waits.

**Unattended runs and the lock screen.** With the Windows session locked, every run fell to 4 fps (50% skip frames): the render thread spent 96% of its time in `wait_for_gpu`, because DWM holds each back buffer 250-500 ms behind the lock screen and Present still returns S_OK, not DXGI_STATUS_OCCLUDED. `RSX_PRESENT=0` (`db6c18a`, live) renders every frame without presenting it; the same run then measures 30.0 fps again. Use it for any profile taken while nobody is at the machine. The renderer now does this by itself (2026-10-16): it registers the window for session notifications (`WTSRegisterSessionNotification`), asks the session state at start-up for a run launched behind the lock screen, and skips Present while the session is locked, so a run presents again once the user unlocks. `RSX_PRESENT=0` is still how an A/B keeps both sides alike: presenting costs a little, so a batch should pin it for every run rather than follow a lock that comes or goes halfway.

**Above 60** (port, ps3recomp `b67dbd9`, `d824c73`): `DES_FPS=<n>` with n from 61 to 240 sets 60 mode and a vblank of n Hz (the frame cap; `VBLANK_MHZ` wins if set) and `RSX_VSYNC=0`, presents that do not wait for the display's refresh, or a 60 Hz desktop holds the game to 60. The option sets them through `ps3_setenv`: the runtime's getenv cache had already cached the miss, and `_putenv_s` alone did nothing. At the deep Palace save, `DES_FPS=120` runs ~67 fps with real presents. Movies keep their speed (cellSail hands out the frame due by the clock), and the regress walk ends in the same place as at 30.
- **Vsync** (2026-10-17, `DES_VSYNC`, ps3recomp PR #32): `RSX_VSYNC=0` did not reach past the display's refresh. The swap chain was made without `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, and a windowed flip-model swap chain presenting with sync interval 0 but without `DXGI_PRESENT_ALLOW_TEARING` is still paced by the compositor: `DES_FPS=240` showed 144 fps on a 144 Hz display. The backend now asks DXGI for tearing support at start-up, makes the swap chain with the flag when it has it, and presents `RSX_VSYNC=0` frames with tearing; without support it presents with vsync and logs that once. `DES_FPS` above 60 no longer turns vsync off; `DES_VSYNC=0` does (default 1).
- **Any rate, and unlocked** (2026-10-18): `DES_FPS` takes any number from 30 to 500, and `unlocked`. Every rate except 30 and 60 sets 60 mode and a vblank of that many Hz: 45 runs a steady 45 fps, with game time equal to real time. `unlocked` is a 1 kHz vblank, so a frame waits at most 1 ms for its flip. On the pinned save behind the lock screen (`RSX_PRESENT=0`) it ran 270-290 fps in the level and ~900 on the loading screen, game time equal to real time, and the save's arrival cutscene still dropped to 30. The settings window's Frame rate row gained Unlocked and Custom, and a Custom frame rate row sets the number (kept as `fps_custom` in settings.ini).

**Cutscenes judder above 30.** The user saw the character vibrating in the tutorial arrival cutscene at 120. `cut.sh`-style captures (8 consecutive presents dumped with `CELLMARK_DUMP_NOW`, the shift between frames measured by phase correlation) show it: in the default mode the scene moves a steady 7-8 px a frame; at `DES_FPS=60` it goes back and forth every other frame (-4, +3, -4, +3); at 120 it creeps forward three frames and snaps back on the fourth (+4, +4, +4, -18; frame 4 matches frame 0). The cutscene manager (RemoMan, `remo/scnNNNNNN.remobnd`) moves things at 30 Hz while the animation follows the frame time. RPCS3's patch notes warn against vblank above 60 as well.

**Cutscenes at 30** (port, ps3recomp `155b97e`). The game now plays its cutscenes as shipped, and unlocks again when they end.
- **Detecting a cutscene.** The Lua `RequestRemo` (binding 0x00445B08) goes through 0x00425D80 and 0x001A1EC0 to RemoMan's request, 0x001A1AF0. RemoMan's object is at `*0x01B3A28C`. The request takes the first free of four slots at +0x28, +0x40, +0x58 and +0x70 (0x18 bytes each), and stores the cutscene object in the slot's first word.
  - Polled live through the arrival cutscene: slot +0x28 holds the object from the level load (the cutscene is requested during the load) until the cutscene ends, about 40 s after the level's script opens. It is 0 in gameplay.
- **The switch.** While any slot is non-zero, `DES_FPS` above 30 sets:
  - 30 mode (+0x6EC = 0);
  - the fixed 1/30 step;
  - the PS3's 59.94 Hz vblank, through `ps3_vblank_set_mhz`, so 30 mode flips every other vblank exactly as shipped.

  When the slots clear, it puts back 60 mode, real frame time, and the vblank `DES_FPS` asked for. The log shows each switch (`[des] DES_FPS: cutscene, 30 fps` / `cutscene over, unlocked again`, and `[vblank] clock now at ...`).
- **Why not a sleep.** Holding each frame to 33 ms with a sleep in the flip hook was the first idea. At a 120 Hz vblank a flip can only land every 8.3 ms, so a little oversleep turns 33.3 ms into 41.7 and the judder comes back.
- **Measured** at `DES_FPS=120`, the same bursts:
  - before: +2, +2, +2, -9 and +4, +4, +4, -18;
  - after: steady shifts (-5, -5, -6, -8, -10, -10, -10 and 5, 5, 4, 4, 3, 2, 4).

  The bursts at 44 s and 52 s are gameplay again, unlocked.
- Regress is ok at the default and at `DES_FPS=120`.

## Faster launches: DES_AUTO_CONTINUE (ps3recomp `322ce60`, port, 2026-10-16)

The harnesses drove the boot menus from outside: every 2 s, if the log showed the game polling the pad, write CROSS, CROSS or START into `PAD_FILE`, each held 40 polls. Timed on the user's save, the level's script archive opened 32.5 s after launch: 2 s to the first pad poll, the notices until 10 s, the logos until 11 s, then 20 s and twelve presses through the title, "load game" and the save list.

`DES_AUTO_CONTINUE=1` (port `port/des_options.cpp`) presses in-process instead: CROSS, CROSS, START in a 600 ms cycle from the first poll (100 ms each), through a new runtime hook that adds buttons to port 0 on every poll (`g_pad_title_hook`). It never presses CIRCLE or the d-pad, so it only goes forwards and never moves a cursor off its default: the title's first entry and the first save in the list. A second hook (`g_fs_open_hook`, the path of every file `cellFsSdataOpen` opens) tells it when the level's script archive (`script/m*.luabnd`) opens, and it lets go of the pad for good.

The same save's level now starts loading at 15.2 s instead of 32.5 s, and runs normally after (29.8 fps unattended). `regress.sh` uses it (all four shots ok, 117 s instead of about 150). For playing: `tools/des/play.sh DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1` goes straight into the last save.

RPCS3's "Skip Intro Videos" patch for BLUS30443 (GalCiv) writes `li r0,3; stw r0,0x100(r28)` at 0x008EEF78: every logo's `m_animState` goes straight to 3 (wait for the fade-out). `DES_SKIP_INTRO` jumped to the last logo and skipped its hold instead, so one fade-in and fade-out still played, about 2 s more.

### Skip options: logos and the opening movie (2026-10-17)

The two are now separate options:

- `DES_SKIP_INTRO=1` does what the RPCS3 patch does. State 3's branch (`0x008EF168`) does not wait for a fade. It moves to the next logo, and after logo 4 it requests the title (menu state 0x3C) itself. The hook writes 3 to `m_animState` before each update, so all five logos pass in five frames and none is drawn. A test run reached the auto-save notice 6 s after boot.
- `DES_SKIP_INTRO=2` is the first version (the last logo, about 2 s). It is kept as an option.
- `DES_SKIP_INTRO_MOVIE=1` skips only the opening movie, `movie/ds_advertise.pam`, the 130 s advert after the auto-save notice. It sets the runtime's `PS3_SAIL_SKIP` (ps3recomp `feat/sail-skip`: a comma list of path texts that are never played, while the rest play). The skipped movie ends as soon as it starts, as START ends it on hardware. In a test run the advert was skipped, and `ds_title.pam`, the movie behind the title screen, still played all 300 frames. Left idle, the title screen replays the advert as its attract movie, so that is skipped too, and the title movie plays again. `ds_opening` (after character creation) and the endings are not touched.

`PS3_SAIL_MOVIES=0` still skips every movie. The settings window's "Intro movies" row (`PS3_SAIL_MOVIES`) is replaced by "Skip opening movie" (`DES_SKIP_INTRO_MOVIE`). A settings.ini with `PS3_SAIL_MOVIES = 0` reads as `DES_SKIP_INTRO_MOVIE = 1`.

**2026-10-27: the first play only.** The title screen replays the advert after every 10 s loop of `ds_title.pam`, not after a long idle, so `PS3_SAIL_SKIP` skipped every one of those plays. The option is meant to skip only the play after the logos. `DES_SKIP_INTRO_MOVIE=1` now sets ps3recomp's `PS3_SAIL_SKIP_FIRST` (#141), which skips a listed movie only the first time it opens in a run. A test run with it: the advert after the logos was skipped, the title movie played its 300 frames, then the advert played in full (3,896 frames), then the title again. Every open counts, so turning the option on in the settings window after the advert has played skips nothing.

## 60 fps mode: runtime cost on the HighGraphics ring (ps3recomp branch `perf/spu-regions`, 2026-10-16)

All numbers are from `DES_FPS=60` at the user's deep Palace save, unattended (`RSX_PRESENT=0`). A/Bs toggle a live knob in one run in ABBAAB order, 8 s windows. Builds are compared by alternating runs of two exes (`DES_EXE`). Run-to-run spread is about 0.1 fps when alternated, and up to 2 fps between unrelated runs, so only same-run or alternated comparisons count.

**Fixed.**
- **Guest loads** (`881a716`): `vm_read8/16/32/64` ran every read diagnostic on every guest load (13% of HighGraphics). They now take a gated fast path and fall back to `*_full` only for special addresses or while a read diagnostic is set. 33.9 -> 35.2 fps. `VM_READ_FULL=1` forces the full path; `PPU_HOTREAD=1` brings back the `[HOTREAD]` detector.
- **Plain DMA and frequent channel writes** (`131e636`): plain GET/PUT skip `mfc_submit`'s diagnostics, and `spu_wrch` answers the command, tag-update, event-mask and event-ack writes itself. 34.1 -> 37.7 fps. `SPU_MFC_FULL=1` takes the full path. The 256-identical-transfers halt (`SPU_DMA_REPEAT_LIMIT`) is one counter shared by both paths. Kept apart, the lean path's leftovers looked like 256 in a row and halted image 2.
- **No-op PUTLLCs** (`ab974d5`): image 2 snapshots the ring's put line with GETLLAR and then commits it back unchanged with PUTLLC, ~1.3M times a second (89% of its PUTLLCs). A PUTLLC that would succeed and writes the bytes already there is now answered under the lock-line lock alone: no store, no PPU reservation slot or break, no notify. The runtime's commit hooks (`spu_taskset_line_committed`, the event-flag hook) still run. A first version that skipped them froze the game within seconds: they stand in for the SPURS kernel, and a task left pending by an earlier commit is started on the next one. 35.6 -> 37.2 fps (every B window above every A window), and again 36.6 -> 38.2 on the final build. `SPU_PUTLLC_SAME=0` (live) stores every time. regress ok; a 2.5-minute soak held 37.7-38.1 with no freeze.
- **Async job threads** (`cfae5d2`): each async SPU job (~650 a second: Havok image 7, image 10, images 3-6) used to get its own CreateThread. Finished workers now park and take the next job. A dispatch with no idle worker still creates a thread, so no job waits for another. In the idle view the frame rate is unchanged. It is aimed at bursts like breaking objects, which have not been measured. `SPU_ASYNC_POOL=0` (live) goes back to a thread per job.

**The ring.** HighGraphics writes 16-byte commands into a ring, and image 2 (`spu_0001_at_0176C200`) turns them into RSX commands:
- HighGraphics stores the put pointer (0x01B51080) once per command, about 1.45M plain stores a second (FUN_00acafa8 and its siblings; `SPU_HIST_LINE=01B51080` plus `spuhist` shows them: `s img=4 ... c=0x45A8AB00, AB10, AB20...`).
- The get pointer is on the line before it (0x01B51000).
- Every put store goes through the lock-line lock and kills image 2's reservation, so many of image 2's PUTLLCs fail (`F` in `spuhist`) and retry.
- Image 2 runs 2.3M GETLLARs, 3.1M DMAs and 5M slow-path channel writes a second, all its atomics from one site (pc 0x4818, `SPU_LLAR_HIST=2`), and is 90-95% busy.
- HighGraphics waits 41% of its time in `usleep(30)` polls, about 3,100 a second (averaging 134 us), for the reply to command 0x9023.

**Where the frame goes.**
- Present intervals are continuous from 23 to 32 ms (median ~26.5), not stepped at 16.7/33.3, so the vblank is not what limits it.
- Causal test (`SPU_SLOW_GETLLAR=2:100`, 100 ns more per image-2 GETLLAR, about 20% more of its time): 36.5 -> 32.6 fps. Image 2 is on the critical path, but only about half of its extra time shows up in the frame.
- Per ring command image 2 spends ~0.65 us. 60 fps would need ~0.43 us, and the lifted guest code alone is about 55% of its time (the rest: lock-line lock ~15%, channel and MFC code, the PPU reservation hooks, the taskset commit check).

**Tried, no gain** (same-run or alternated A/B):
- Per-line PUT locking: a PUT over a reserved line copied each line under its own hold, instead of the whole span under one. fps unchanged; image 2's lock waits unchanged.
- Notify filter: a compact per-slot tag of each context's reserved line, so a notify only reads the matching context. Image 2's lock waits fell from 14% to 11% of its time; fps unchanged.
- HighGraphics and image 2 pinned to one core's two SMT threads (console `affin`): 32.7 fps, against 35.7 on two cores and 36.5 unpinned.
- `usleep` spinning on `pause` instead of `SwitchToThread`, and never switching to the timer after 64 back-to-back polls: unchanged. HighGraphics' wake-up latency after the reply is not what limits the frame.
- Skipping PPU stores of the bytes already in a reserved line: none of HighGraphics' ring stores are identical.
- `/GS-` for the lifted SPU code (no stack cookie in the region prologues): 38.4 against 38.7.
- From the session before: `USLEEP_SPIN=1`, padding `spu_context`'s cross-thread fields, gating the MFC atomic diagnostics behind one flag, moving the WWS LS probe behind a knob, a seqlock GETLLAR, a lock-free `vm_write` fast path (it froze one load).

**Tools** (`1cb2346`, `da7030d`):
- `SPU_LOCKLINE_STATS=1` also names the holders a thread waited behind and for how long.
- `PPU_COH_HIST=1` lists the lines whose PPU stores took the lock-line lock, with the thread and lr.
- `SPU_RATE=<img>` gains `polling=N%` (re-reading an unchanged line) and `putllc=N (same N)`.
- `SPU_SLOW_GETLLAR=<img>:<ns>` is the causal test above.
- Console: `profrip <tid> [s]` adds the hottest instruction addresses to `prof`; `affin <tid> <hexmask>` pins a host thread (0 = back to the process mask).

**Second pass (2026-10-16).**
- **Quadword byte shifts** (ps3recomp `fdf9a4f`): `shlqby`, `rotqby`, `rotqmby` and their bit-count and immediate forms, plus `cbd/chd/cwd/cdd`, ran a 16-iteration byte loop each. With SSE4.1 (the lifted SPU code is built with it) they are now one `pshufb` on a computed index; the byte loops stay for builds without it. Image 2 runs about 18M SPU steps a second and these are among its most frequent ops. 38.4 -> 41.8 fps (alternated runs). `test_spu_helpers` checks every shift count against the old loops, with and without SSE4.1.
- **Lock-line lock striping** (ps3recomp `a20c521`): the one lock-line lock is now 64 stripes keyed by the 128-byte line. A GETLLAR, PUTLLC or DMA PUT takes only the stripes its span covers; reservation registry changes still take all of them, in order. Image 2's lock waits fell about 10x; fps unchanged (41.8 either way), so the lock was not on the critical path. Kept because it frees CPU that physics bursts (breaking objects) may want. `SPU_LL_STRIPES` set to anything (read at the first lock) goes back to a single lock.
- Tried, no gain: answering a no-op PUTLLC without the lock at all (re-checking the line after the fact): 0.45 fps worse.
- Also no gain (six alternated runs each, all within the ~0.5 fps noise): inlining the DMA-parameter channel writes (`wrch` to MFC_LSA/EAH/EAL/Size/TagID/WrTagMask, ~20M a second in image 2): 41.96 -> 42.17; an inline fast path for a lifted call's return (`spu_drain_call` with nothing pending) plus `spu_img_restore` inline, a thread-local depth-guard ring, and `/Zc:tlsGuards-` for the SPU TUs: 42.03 -> 41.69. The reason showed up next: image 2 was no longer the limit.

**The render thread is the limit (2026-10-16).** After the SPU helpers, `cpu 4` showed the FIFO thread (host thread `vblank_ticker`, hot in `upload_strip_vp_indexed`) at 94-98% of a core, busier than image 2 (84-89%). Its profile: the indexed strip walk 34% (self), `render_frame` at the flip 25%, method decode (`rsx_process_method` plus the walker loop) ~17%. `TCONST_FIFO=1` (a live knob, its histogram is printed after 200,000 method headers) showed what the walker decodes in a level: 48% of the words are vertex-program uploads (`0x0B80`, ~30 words a block, re-sent per draw) and 30% vertex constants (`0x1EFC` load + `0x1F00` window).
- **Index base resolved once** (ps3recomp `d55f860`): the strip walk read each index through `read_guest_index`, which called `cellGcmResolveLocated` (out of line) for the array base every time. 41.96 -> 44.82 fps.
- **Vertex jobs published in batches** (ps3recomp `5d39bc5`): each draw's job was published with a locked increment on the cache line the four `vpj_worker` threads spin on (~6% of the FIFO thread). The counters now have a line each, and jobs are published every `VPJ_BATCH` vertices (read once, default 1024); `vpj_sync` publishes the rest first. 44.92 -> 45.43 fps.
- **Vertex-program and constant uploads in bulk** (ps3recomp `7b48929`): `rsx_process_method_run` takes an incrementing run of those methods straight from guest memory, with the same stores and counters as the per-word cases. The walker uses it for subchannel-0 runs contiguous in guest memory while no per-word consumer (capture, `GCM_VAL_WATCH`, `RSX_CENSUS`, live draw, draw engine) or debug knob of those cases (`RSX_TRACE`, `LOAD_DBG`, `TCONST_DBG`, `SEQ_DBG`) is on. 45.48 -> 47.89 fps.
- Together 41.96 -> 47.89 fps (+14%), each step regress ok. Afterwards the FIFO thread is at ~86% and image 2 at ~87%, so the two are close to balanced again.
- No gain: reusing the strip's two previous corners' output slots in the walk instead of looking them up in the vertex cache (the cache is 256 entries, L1-resident; the lookups were not the cost).
- **Per-draw costs, from the run's own frame profile** (`prof_*_summary.txt` in the `lc_*` run directory): `render_frame` took 9.4 ms of a ~21 ms frame, 7 ms of it the SRV fill, at 6,360 draws a frame and ~88,500 texture lookups (about 14 per draw).
  - ps3recomp `2bd276e`: the cube mask (a walk over all sixteen units) once per draw instead of once per unit and again for the PSO; the fragment program's inline constants kept with a copy of the program's bytes and re-read only when those change (`RSX_FPK_MEMO=0` walks every time). 47.46 -> 47.69 fps for the cube mask.
  - **One draw per strip** (ps3recomp `fcea7a5`): the level meshes are strips split into `DRAW_INDEX_ARRAY` ranges of 256 indices inside one BEGIN/END, and each range was its own draw. A range that continues the last draw's strip, with its vertices and indices right after that draw's, now extends it (indices count from the draw's vertices; the vertex cache carries over, so vertices two ranges share are converted once). `RSX_STRIP_MERGE=0` draws each range on its own. 6,360 -> 973 draws a frame, `render_frame` 9.4 -> 2.4 ms, 47.66 -> 51.90 fps, regress ok.
  - **Only the sampled units** (ps3recomp `ead2d65`): the fragment program's texture-unit mask is kept in `fp_hash_at`'s memo, and the units it does not sample get a null SRV without looking anything up. `RSX_SRV_USED=0` binds all sixteen. `render_frame` 2.6 -> 1.8 ms; fps unchanged (50.52 -> 50.48), since after the strip merge the render thread is no longer the limit.
- **Image 2 is the limit again** after these. `cpu 4`: image 2 (`spu_async_thread` in `spu_region_00004498`) 89.5%, HighGraphics 76% (37% of it in `usleep`), the FIFO thread 60%. Image 2's time: lifted code ~40%, the reservation path ~23% (`spu_ll_acquire`, lock-line lock/unlock, `spu_taskset_line_committed`), `spu_wrch` ~9%, and ~9% napping in `spu_llar_idle_wait` with the ring empty. Part of its lifted-code time is the GETLLAR poll loop itself (compare the ring's put and get, re-arm), so it waits on HighGraphics as well. `USLEEP_SPIN=1` (HighGraphics' reply waits spin instead of sleeping on the timer once in a streak): 51.60 -> 51.33, no gain, so that wake latency is still not critical.
  - **Channel latches in place** (ps3recomp `67b24d7`): lifted code writes the DMA-parameter channels (LSA, EAH, EAL, Size, TagID, WrTagMask) into the context itself (`spu_wrch_latch`) instead of calling `spu_wrch`, ~20 million calls a second in image 2, and reads tag status, atomic status and tag mask the same way (`spu_rdch_latch`). A channel diagnostic (`SPU_CHHIST`, `SPU_POLLTRACE`, `SPU_TAGPROBE`) still sends them through the calls. The same change was worth nothing while the render thread was the limit (above); now: writes 51.71 -> 54.90 fps, reads on top 55.04 -> 55.64. Re-lift (`python scripts/build_spu.py`) after pulling.
  - **Commit hook filtered** (ps3recomp `0999183`): `spu_taskset_line_committed` runs after every committed SPU lock-line and scanned the 32 tracked tasksets and the 64 parked-task slots each time (~3.3% of image 2). It now tests 256-way filters on the line address first (`g_spu_parked_lines`, which the PPU store-conditional path already kept, and `s_ts_lines` for the tasksets). 55.78 -> 57.89 fps.
  - **The frame at ~57 fps** (console `trace`, HighGraphics = guest tid 4): after `_cellGcmSetFlipCommand` HighGraphics computes ~2.6 ms with no ring entries (image 2 idles), then writes ~40k entries over ~10 ms; image 2 is saturated from ~2.5 ms to the end of the frame at ~0.36 us an entry; HighGraphics posts command 0x23 and waits 4-5 ms (`func_00ACAFA8`, `usleep(30)` loop) for image 2 to drain, then flips. Per second image 2 does 3.5M GETLLARs, 2.1M PUTLLCs (1.8M no-op) and 4.8M DMAs. Causal: +60 ns per image-2 GETLLAR (`SPU_SLOW_GETLLAR=2:60`) -6.4% fps; +100 ns per HighGraphics store to a reserved line (a temporary `PPU_SLOW_COH` knob, not kept) -2.4%. So image 2's throughput sets most of the frame and HighGraphics' frame-start compute the rest. To profile image 2, select it by its hot leaf: `PF_PAT=spu_llar_idle_wait` (the thread is `spu_async_thread`).
  - **SPU atomics without the lock's cache line** (ps3recomp `6a8285a`): image 2's hottest single instructions were the stripe lock's `xchg` in `spu_ll_acquire` (4%) and in the unlock (3%), a round trip per GETLLAR and no-op PUTLLC against HighGraphics, which takes the same stripe for every ring store. Each stripe is now a sequence lock (`lock bts` to take, increment to release). A GETLLAR on a line already marked reserved reads it without the lock and redoes it locked if a writer took the stripe meanwhile (`SPU_GETLLAR_SEQ=0`, live, always locks): +2.7%. The no-op PUTLLC check runs the same way (`SPU_PUTLLC_SEQ=0`): +1%. And `spu_wrch` answers GETLLAR and a no-op PUTLLC itself (`spu_atomic_lean`) while no atomic-path diagnostic is set, instead of going through `spu_wrch_full` and its stack-probed 4.5 KB frame (`SPU_ATOM_LEAN_OFF=1`, live, takes the long way): 56.4 -> 58.8 fps. regress ok. The session before, a seqlock GETLLAR showed no gain; then the render thread was the limit. A first version that read the stripe and then compare-exchanged it cost two transfers of the line (shared, then the upgrade) where `xchg` cost one.
  - **Measuring above 60.** At ~59 fps the 60 fps mode hits the vblank, so A/Bs from here on run with `VBLANK_MHZ=120000` as well (`DES_FPS=60 VBLANK_MHZ=120000`): the game's 60 mode then flips up to 120 times a second, and the number is throughput again. Measurement only; RPCS3 warns of physics trouble above 60.
  - **One vertex fetch worker** (ps3recomp `99f1b14`): `VP_FETCH_THREADS` defaulted to a quarter of the logical CPUs (4 here). Uncapped: 1 worker 63-64 fps, 2 about 61, 4 57-59; each of the four sat at ~55% of a core, half of it in the kernel, and the threads the frame waits on ran slower beside them. Waking one worker per publish instead of all did not change it. The default is now 1 on any machine: 57.8 -> 64.6 fps, regress ok. `VP_FETCH_THREADS=<n>` still picks the count (0: ~46 fps).
  - **Guest memory inline in the lifted PPU code** (ps3recomp `2d1aedf`): every `vm_read*`/`vm_write*` in the lifted chunks was a call into the runtime, ~20% of HighGraphics. `ppu_recomp.h` now has inline fast paths for the lifted sources (their preamble defines `PPU_VM_INLINE`): a plain big-endian load or store when the address is above the null page, below the raw-SPU window, not the one special word the full read handles, and (for a store) not on an SPU-reserved line. The runtime publishes the limits (`g_vm_inl_rd_lim`, `g_vm_inl_wr_lim`) and sets them to 0, which sends everything through the call again, while any diagnostic the accessors serve is live (`VM_READ_FULL`, store watches, `PT`, `SPU_ATOM_EA`, the Bink sync watch, a null sweep) and on every console `setenv`. The stores' reserved-line test is inline in the runtime's own stores too. Uncapped: 61.7 -> 62.6 (reserved test) -> ~66 fps, regress ok. Needs a re-lift of the PPU code (`ppu_lifter.py` writes the header and the preamble), then the 14-chunk rebuild (~12 min); building with `PPU_VM_NO_INLINE` defined turns it off.
  - **A context registered twice** (ps3recomp `e6ee7b9`): with the inline accessors, regress crashed 1 run in 4 in `spu_coh_notify_write`, reading a dead `jc_thread` stack during a level load. `spu_coh_reserve` stopped at the first free registry slot before looking for the context further on; since the GETLLAR fast path reserves without a stripe lock, a slot below the context's own entry could empty during another thread's unregister and the context went in twice. Unregister cleared one entry. The `[spu-coh] leaked ctx ... dropped at thread exit` safety-net lines (7-16 a run, all along) were these; now 0. SPU thread contexts never left the registry at all; they now do when the thread exits and in `group_destroy`.
  - No gain, reverted:
    - A register cache in the SPU lifter (`--regions` bodies kept SPU registers in `u128` locals between labels, written back at labels, branches and calls): 58 -> 49 fps. clang split the union locals into 32-bit lanes and rebuilt them with `pinsrd`/`pextrd` around every vector op, which cost more than the `ctx->gpr` loads and stores it removed, and the loops still reloaded at every label. A version would need vector-typed locals and helpers.
    - A release store instead of `xchg` for the stripe unlock, and an inline 128-byte line compare in the PUTLLC check: both within noise.
- **Image 2's lifted code, 2026-10-16** (ps3recomp branch `perf/hooks`). Measured presenting (`RSX_PRESENT=1`), so about 2 fps under the `RSX_PRESENT=0` numbers above.
  - Profile first (`prof` on image 2): lifted code ~45%; the MFC and atomic path ~35% (`spu_wrch`, `mfc_submit`, `spu_atomic_lean`, `spu_ll_acquire`, the seqlock reads, `memcmp`/`memcpy`); the call wrappers (`spu_drain_call`, `spu_depth_guard`, `spu_img_restore`, `spu_indirect_branch`) ~5.5%; ~8.5% napping with the ring empty.
  - **LS accessors forced inline** (`111e3f7`). `spu_ls_read128`/`spu_ls_write128` are `static inline`, but in the biggest `--regions` bodies clang stopped inlining them: the hot region (`spu_region_00004498`) made 128 real calls to the read, with the quadword returned through a stack slot, and 51 to the write. They are now `__forceinline` (`SPU_LS_INLINE`); their probes stay out of line and cold. `llvm-objdump -dr` on the image's object shows no accessor calls left. 59.15 -> 64.72 fps (every window of the new build above every window of the old), 1% lows ~38 -> ~48. No re-lift needed.
  - **`spu_call_done`** (`263699b`). Every lifted call ended in `spu_drain_call(ctx, ret); ctx->host_depth--; spu_img_restore(ctx, _si);`: two out-of-line calls, and in the drain a write to the depth guard's static ring (shared by every SPU thread) and an interrupt frame set up. Most callees come back to the link address with nothing on the trampoline, and the drain then only clears `drain_ret_pc`. `spu_lifter.py` now emits `spu_call_done(ctx, ret, _si)`, an inline helper in `spu_context.h` that does that case itself and calls `spu_drain_call` otherwise. The depth guard stays on the drain path, which any lifted-call recursion goes through. 64.35 -> 66.47 fps. Needs an SPU re-lift (`python scripts/build_spu.py`, ~17 s); older lifts still build and run.
  - Regress ok.
- Bigger levers on the render (FIFO) thread, not started: converting vertices on the GPU from the raw guest arrays instead of on the CPU (the strip walk, the `vpj_worker` threads, and the 256-byte-per-vertex upload heap traffic would go), and building the D3D12 frame (`render_frame`) on its own thread. The latter needs care: `render_frame` reads guest textures and render targets at the flip, so the FIFO thread cannot simply run ahead into the next frame.

**Next.**
- Faster lifted SPU code for image 2: keeping SPU registers in host registers within a region (not with `u128` union locals, see above). clang-cl compiles with relaxed aliasing, so every LS store makes it reload `ctx->gpr` and the gate globals.
- The MFC path per ring command (~35% of image 2): one GETLLAR and one no-op PUTLLC per 16-byte command, each through `spu_wrch` -> `spu_atomic_lean` -> the seqlock helpers.
- Fewer runtime calls per command: the PPU reservation enter/break/leave hooks and `spu_taskset_line_committed` run on every image-2 commit, and each walks a list.
- If image 2 gets much faster, the render thread (77% busy in the FIFO walker and vertex uploads) is next in line.

## Image 2's MFC path, and the plan from here (branch `perf/mfc-path`, 2026-10-16)

Measured presenting (`RSX_PRESENT=1`), one change per A/B. While the user uses the PC, one build alone varies 66.7-69.1 fps across windows, so a change has to clear that.

**Dropped, inside the noise or worse:**
- PUTLLC fast-fail without the lock (370k fails a second, 96% "no reservation"): 66.75 -> 67.05.
- The MFC engine slot cached in the context, plus a thread-local repeat check: 69.28 -> 68.40, every window worse.
- The seqlock readers inline through `spu_coherency.h`: 68.00 -> 64.55, but the same build also added `_b` LS accessor wrappers to the lifted code, so it is confounded.
- An inline SSE 128-byte line compare and no nap copy while busy: 67.92 -> 68.35 alone against master.
- `restrict` on the LS base parameter in image 2's regions: 66.90 -> 67.43.

Before and after each inlining of the atomics, the stripe stress test (12 threads x 300k lock/unlock, `ops=3600000 bad=0`) and a seqlock torn-read test both passed.

**Lazy lock-line notify** (`SPU_NOTIFY_LAZY`). Every HighGraphics store to the ring's put line (~1.45M a second) notifies image 2: it writes image 2's context (`resv_valid`, `event_status`, a wake), and image 2 then pulls that cache line back. In lazy mode the notify skips an SPU that is running (`lr_watch` is set only while its host thread sleeps, in `spu_llar_idle_wait` and `spu_ch_wait`). A running SPU learns of the store from the line instead: its PUTLLC, its event-status reads (`spu_resv_lost_poll`) and its nap check compare the bytes against its snapshot, and a per-line write version that every notified store bumps (eight counters in each stripe's cache line, so no extra line moves; a store that puts the same bytes back still counts). Waking is Dekker-ordered: the SPU sets `lr_watch`, fences, then checks the line; the writer bumps the version with a locked add before it reads `lr_watch`. `test_spu_coherency.c` runs its cases eager, then lazy with every SPU parked, plus the running case.
- First version (watching only while asleep, no versions): same-run A/B +4.9%, every lazy window above every eager one.
- With the version checks and an LR check in the lifted interrupt test (`SPU_RGOTO`, the drain macro): lazy on beat lazy off by 3.5% in the same run, but the build as a whole was slower than master (65.1 vs 68.2). The interrupt test sits at every region branch in the lifted code, and the extra inline code there cost more than lazy saved.
- With the interrupt test restored and only the PUTLLC, poll and nap checks kept: 67.38 (master) vs 67.63 fps (lazy on), alternating builds over six windows each. In that build lazy on still beat lazy off (68.0 vs 64.1), so the build's eager path had got slower than master, and image 2's own profile was nearly the same as master's. A gain inside the noise, so **dropped** (the user's call: no more rounds on it). The diff is kept outside the repo; what it would need is a version check that does not read HighGraphics' lines on image 2's every event-status read and PUTLLC.

**The plan from here** (the user's, 2026-10-16). Micro-tuning of the MFC path is paused; these are the big levers, in order:
1. **Register promotion in the lifted SPU code.** Keep each region's SPU registers in vector-typed locals (`__m128i` or `vector_size(16)`, not unions; see the `u128` attempt above, 58 -> 49 fps), written back to `ctx` only at region exits, calls, and anything else that can see `ctx`.
   - Re-profile image 2 first and confirm the lifted code is still about 45% of its time.
   - Prototype by hand on image 2's hottest region (`spu_region_00004498`) and A/B it with alternating builds.
   - Only a clear win (5% or more) goes into `spu_lifter.py`, followed by a full re-lift and regress.
2. **The same in the PPU lifter**, for HighGraphics' compute (its ~2.6 ms after the flip, and the ring writes).
3. **Less cross-core traffic per ring command**: the GETLLAR / unchanged-PUTLLC snapshot pattern costs line transfers between image 2 and HighGraphics on every 16-byte command.

**Step 1 result: register promotion in image 2's regions does not pay** (2026-10-16, prototype by a script over the lift, alternating builds, presenting):
- Profile first, master: image 2's lifted regions ~44% (`0x4498` 13.8%, `0xC358` 7.0%, `0x3C90` 6.0%, `0x8CB8` 5.2%, the rest under 3% each), the MFC/atomic path ~30%, napping ~9.5%. Within `0x4498` no instruction is above 0.6%: the cost is spread over the whole body.
- Whole-region promotion with `u128` locals (loaded at entry, dirty ones written back at calls, channel accesses, exits and region-branch slow paths, everything reloaded after a call): 67.05 -> 49.82 fps. clang split the unions into lanes again (1007 `pinsrd`, 392 `pextrd`), and `spu_call_done` stopped inlining.
- The same with `__m128i` locals and SSE versions of every op the region uses (no union anywhere in the data path, `spu_call_done` forced inline): 67.32 -> 60.47, every window worse. No lane splitting now, but the region grew from 11,372 to 19,226 instructions and from 371 to 4,371 stack references: 86 SPU registers live across a body that calls out every ~30 instructions do not fit in 16 xmm registers, and the spills cost more than the `ctx->gpr` loads and stores they replace (`ctx->gpr` already is the spill slot).
- Caching registers only within straight-line segments (load on first use, write back at the next label, branch, call or exit), applied to the top 8 regions: statically worse in the long regions (`0x8CB8` 35k -> 39k instructions, stack references 736 -> 3,393; only the short `0xC358` shrank, 883 -> 741), and the build hung at boot, so it was not measured.
- Together with the earlier `restrict` LS base (+0.5, noise), which removes the same aliasing barrier, this says the `ctx->gpr` traffic is not where the lifted code's time goes. The ops are: `shufb` is ~20 instructions (three byte swaps for the word-reversed layout plus the special-selector handling), `cwd` is a compare and blend, and every LS access tests up to four debug-probe globals. Cheaper ops are the lever left in lifted SPU code, for example `cwd` on `$1` (16-byte aligned by the SPU ABI) folded to a constant so `cwd`+`shufb` becomes a single blend. Not started; promotion does not go into `spu_lifter.py`.

**Step 2 result: per-function overhead in the lifted code is not where the time goes either** (2026-10-16, alternating builds against master, three rounds, presenting):
- HighGraphics' profile on master: ~40% asleep in `usleep` waiting for image 2, ~15% in the ring-store path (`spu_ll_acquire` 5.2%, `spu_coh_notify_write` 3.8%, `spu_lockline_unlock_ea` 3.3%, `lock_ea` 1.3%, `vm_write32` 1.1%), and the lifted functions spread thin (`func_00A43718` 2.2%, `func_00103878` 2.1%, `func_000B90F0` 1.9%, ...).
- TLS guards off (`/Zc:tlsGuards-` on every TU). Every lifted PPU function's `DS_FRAME` read the thread-local shadow stack through `__tls_guard` / `__dyn_tls_on_demand_init` checks, and image 2's hot region had 64 of them around `g_spu_trampoline_fn`. The flag removed all of them: 67.85 -> 67.72 fps. **Dropped.**
- Shadow call stack off (`DS_SHADOW_STACK` not defined, so `DS_FRAME` is a no-op): 67.75 -> 66.45 fps. **Dropped.** It is also what the console's backtraces and the `sys_cond` deadlock dump read, so keep lifting with `--shadow-stack`.
- SSE4.1 for the PPU chunks (`/clang:-msse4.1` on the target, as the SPU images already have). The lifted VMX ops keep vectors in guest byte order and swap every lane on each load and store (`ppu_vldf4` and friends); without SSSE3 each 16-byte swap is ~8 SSE2 shuffles, and HighGraphics' hottest function `func_00A43718` (VMX culling) was 1,798 instructions with only 35 `addps`. With SSE4.1 the swaps become one `pshufb` each and the function 1,105 instructions, but 65.67 -> 64.57 fps, no window better. **Dropped.** HighGraphics' lifted compute is not on the critical path at this frame rate: it spends ~40% of its time waiting for image 2.

**The order from here** (the user's, 2026-10-16, after the results above): no more per-function overhead experiments on HighGraphics.
1. Ring cross-core traffic (step 3 above). Measured sensitivity: +60 ns per image-2 GETLLAR = -6.4% fps. Profile image 2's GETLLAR / unchanged-PUTLLC snapshot path and HighGraphics' reserved-line stores, find what moves the line between cores on every command, and prototype the cheapest cut.
2. Cheaper SPU ops in `spu_lifter.py`: `cwd` on a known-constant address followed by `shufb` becomes a constant blend; drop the LS probe checks where the probe is off. Prototype by hand on `0x4498` first; it goes into the lifter only at 5% or more.
3. The A/B and profiling scripts move into `tools/des` (their own commit and branch).

**Step 3, ring traffic: first findings** (2026-10-16):
- What one HighGraphics store to the put line does when image 2 holds a reservation on it (`VM_WRITE_COH` in `ppu_loader.cpp`): it takes the line's stripe lock (`spu_ll_acquire`), copies the bytes, and runs `spu_coh_notify_write`, which reads image 2's `resv_valid` / `resv_ea`, atomically ORs `SPU_EVENT_LR` into its `event_status`, clears `resv_valid` and calls `WakeAllConditionVariable` on its wait CV; then it increments the stripe. Image 2's GETLLAR reads the stripe word twice (seqlock begin and check) and rewrites `resv_ea`, `resv_valid` and the 128-byte snapshot. So per command the cache lines that cross between the two cores are the ring line, the stripe word, image 2's reservation line and its `event_status` line.
- `event_status` shared a 64-byte line with `ls`, `pc` and `image_id`, which image 2's lifted code reads on every LS access. Moving it to a line of its own (a placeholder kept in place, the field at the end between 128-byte pads): 67.87 -> 67.78 fps. **Dropped**; that false sharing costs nothing measurable.
- Thread load on master at ~67.6 fps (presenting): image 2 88% of a core (naps ~10%), HighGraphics 86% (a third of it waiting in `usleep`), and the vblank_ticker thread that runs `render_frame` 85% (hot in `upload_strip_vp_indexed`). No thread is saturated: the frame is set by the hand-offs between them.
- Causal, same run (knob A/B): +60 ns per image-2 GETLLAR (`SPU_SLOW_GETLLAR=2:60`, ~1.3 ms a frame) 66.73 -> 58.33 fps (-12.6%, twice the -6.4% at ~57 fps); +1 us per render-thread draw (`VP_SLOW_DRAW_NS=1000`, ~1 ms a frame) 68.20 -> 57.20 (-16%). Both cost their full time per frame, so the two threads are in series.
- Why, from a console `trace` (a 15.3 ms frame): the FIFO thread reaches frame N's flip about 12 ms after HighGraphics issued it, runs `render_frame` and `present` there (~2.1 ms: wait+store 0.44, draw-prepass 0.95, vp-pass 0.34, submit 0.09, present 0.14), and ~1 ms later writes label `0xFF0`. HighGraphics polls that label (its `usleep(30)` loop; each `usleep` lasts ~515 us, the timer's granularity) and issues frame N+1's flip one poll after it. So the game holds the CPU about a frame ahead of the RSX, our FIFO thread is that RSX, and everything on the FIFO thread between the flip and the label is on every frame's path, with image 2's throughput ahead of it.
- Image 2 at instruction level (`profrip`): ~3.8% in `spu_ll_acquire` (its `pause` spin and the `lock bts`, waiting for the stripe HighGraphics holds or just wrote), 1.8% at the seqlock check's fence (`spu_lockline_read_ok`), 1.1% at the seqlock's first stripe load. The per-command idiom is a GETLLAR snapshot and then a PUTLLC that writes the same bytes back; for that no-op PUTLLC, `spu_atomic_lean` read the stripe word, compared the 128-byte line (which HighGraphics had usually just written), fenced and re-read the stripe, and spun on the lock when HighGraphics held it.
- **No-op PUTLLC decided from the reservation.** Every store into a reserved line (lifted PPU stores, `vm_write*`, `stwcx.`/`stdcx.`, SPU DMA PUTs and PUTLLCs) clears the SPU's `resv_valid` under the line's stripe (`spu_coh_notify_write`), so a reservation still held means no store has finished since the GETLLAR, and a store in flight can be ordered after a PUTLLC that writes nothing. The no-op PUTLLC is now one atomic exchange on `resv_valid` and touches none of HighGraphics' lines. Same run, `SPU_PUTLLC_RESV` live: 60.35 -> 64.32 fps (+6.6%), every window with it above every window without. A raw `memcpy` into guest memory does not clear reservations (that is what `spu_resv_lost_poll` covers); none of those writers is a line image 2 polls, and `SPU_PUTLLC_RESV=0` compares the bytes as before. `test_spu_lockline_peer.c` has no-op cases (succeeds alone; fails after a peer's commit, a plain PUT, or a PUT of the same bytes) that pass in both modes.

**The heavy hitters, in order** (the user's, 2026-10-16 night, after the trace above). The game is not heavy for a PC; the chain HighGraphics -> ring -> image 2 -> RSX FIFO -> our FIFO thread -> `render_frame` -> label `0xFF0` -> HighGraphics is, with each stage one host thread.
1. HighGraphics' poll granularity: its `usleep(30)` sleeps ~515 us (the timer's resolution) while it polls label `0xFF0`. A high-resolution timer, or waking sleepers when the FIFO thread writes a label.
2. `render_frame` and `present` off the FIFO thread, so the label is written ~2 ms sooner. Its guest texture reads (draw-prepass, ~1 ms) have to happen at draw time or from a snapshot. Also: what fills the ~1 ms between `present` and the label.
3. Vertex fetch on the GPU (the FIFO thread is 85% busy, mostly `upload_strip_vp_indexed`).
4. A native replacement for image 2's ring-consumer program, checked against the lifted one by a differential test.
5. Cheaper lifted SPU ops (step 2 of the order above).

Rules that carry over: one change per A/B, drop anything inside the noise, and run several minutes in the Nexus at `DES_FPS=120` before committing any change to the atomics (the lwarx freeze below was a memory-ordering bug that short runs never showed).

## Nexus freeze: a lifted lwarx took its reservation late (ps3recomp `a72bac3`, 2026-10-16)

The user hit a freeze in the Nexus at `DES_FPS=120`: the picture stopped, the music played on, and the title bar still read 120 fps and 385 draws.

**Signature** (read from the live process; nothing in the log):
- HighGraphics (tid 4, lr 0x014C3CB8) was asleep in libsre's `cellSpursQueuePopBody` (NID 0x91066667, `libsre_func_30016CFC`) on event queue 18.
- The queue at 0x47FC5080 held 0xFFFFFE17 in its pop word (488, marked waiting) and 0x1ED in its push word (493). Five records were waiting.
- Event queue 18 had one waiter and no event. The SPU that pushed them had seen no waiter and sent nothing.
- MAIN waited on a cond behind HighGraphics. Audio runs in its own taskset, so the music kept going.

**The pop.**
1. `lwarx` the pop word.
2. `lwsync`.
3. Read the push word.
4. If they are equal, `stwcx.` the complement (waiting) and sleep on the queue.

The SPU push commits the push word with PUTLLC, and sends an event only if the pop word it read was negative. This works on Cell because the push's store to the line kills the PPU's reservation, so step 4 fails and retries. The runtime emulates that: an SPU commit stores the line, fences, then clears the `reserve_addr` of every PPU context reserved on it (`ppu_resv_line_break`).

**Why it failed.** The lifted `lwarx` loaded the word first and stored `reserve_addr` after it, with no fence. Both sides were therefore store-then-load:
- the PPU stores `reserve_addr`, then reads the push word;
- the SPU stores the push word, then reads `reserve_addr`.

x86 lets each load pass the store before it, so both could read the old value. The commit found no reservation to break, the PPU read the push word from before the commit, and `stwcx.` succeeded. The guest's `lwsync` does not order a store before a later load. The September libsre lift printed it as `/* sync: no-op */`, which made it look like a full `sync`.

**Fix.**
- `ppu_lifter.py`: `lwarx`/`ldarx` now store `reserve_addr`, then `PPU_FENCE(seq_cst)`, then load. Hardware takes the reservation and the load as one operation, so the guest's own barriers can't be relied on to cover the gap. `test_ppu_lift.py` checks the order, and fails on the old lifter.
- libsre re-lifted (`scripts/build_libsre.sh` steps; the old lift is kept beside it as `work/libsre/lifted_sep05_backup`). Besides the fix, the new lift has:
  - 222 `lwsync` and 93 `eieio` fences that were no-ops;
  - `lvewx`/`stvewx` aligned and placed in the right lane;
  - `vpkuhus` and `vmsumshs`, which had been TODO no-ops.
- EBOOT: only the 31 `lwarx` sites change (chunks 004, 005, 008 and 009). Copy those four from a fresh lift and run `apply_boot_patches.py`.

**Checked.**
- Regress: all four shots ok.
- Frame rate: old and new builds alternating, Palace save, `DES_FPS=60 VBLANK_MHZ=120000`, two 8 s windows a run, four runs. Old 61.1 fps, new 62.7: the fence costs nothing measurable.
- The freeze itself is rare and was not reproduced. The live state above is the case for the fix.

**After pulling.** Re-lift both:
- libsre: `bash scripts/build_libsre.sh`.
- EBOOT: the recipe in docs/27.

Then rebuild `des_boot`.

## The heavy hitters, worked through (2026-10-16 night to 10-17)

All A/Bs: `DES_FPS=60 VBLANK_MHZ=120000` at the deep Palace save, unattended. Live knobs are toggled in one run, ABBAABBA, ~6 s windows (`tools/des/knobab.sh`). Run-to-run spread is about 1 fps at ~100 fps. Palace went from ~66 fps (uncapped) to ~97-100 over the night, to ~105-107 by the morning of 10-17, and to ~110 with the HLE-call changes.

**Cheaper lifted SPU ops** (item 5, done first; ps3recomp PR #18):
- `shufb` in the host layout (`6a019d5`): 10 ops instead of 20. +1.8%.
- One gate in front of the LS accessors' debug probes (`c2a2c84`): every LS access tested up to four probe globals; it now tests one. +6.8%.

**HighGraphics' poll granularity** (item 1): no gain, twice. `USLEEP_SPIN=1` (spin every `usleep`, never fall back to the 0.5 ms timer after 64 polls): 69.25 -> 69.10 before items 3 and 4, and 97.88 -> 97.92 after them. HighGraphics' wake-up latency is not on the frame's path.

**Vertex fetch on the GPU** (item 3; ps3recomp PR #20, `f7337e9`): strip and fan draws send the raw guest vertex arrays and a fetch description to the vertex shader, which fetches and converts the attributes itself (`VP_GPU_FETCH=1`, the default). Same run: 73.4 -> 78.6 fps. FIFO thread 96% -> 73% of a core, the `vpj_worker` 80% -> 14%. Done before item 2, because the FIFO thread was the busiest thread at the time.

**The shield** (item 6, RPCS3 issue #3088 / #4302; ps3recomp PR #19, `758d3ec`): the Soldier's starting shield was invisible. The RSX decodes vertex type 1 (S1, signed 16-bit normalized) as (2x + 1) / 65535, not x / 32768 (RPCS3 PR #12450, `fetch_attribute`). Demon's Souls passes a skinning weight of 0 in that format and the shader takes `pow(w, -1)`: with x / 32768 that is 1/0 and the mesh collapses. The CPU and GPU fetch paths both decode it the RSX way now. The shield shows in regress set `20261016_225628`.

**A native fetch step for image 2** (item 4; ps3recomp PR #21, port PR #19):
- `spu_lifter.py --native-block ADDR-END=SYMBOL` (`6b1accb`): a C function runs in place of the lifted instructions from ADDR up to END. It returns 1 when it did their work, with every register, LS byte and channel they would write left as they leave them; 0 runs the lifted code. `scripts/build_spu.py` lists them per image (`NATIVE_BLOCKS`), keyed by the image's hash.
- `spu_ring_hook.c` (`ac4058b`): image 2's per-command fetch, LS 0x4618-0x4730. Lifted, each 16-byte command costs a GETLLAR of the ring's control line and a PUTLLC writing it back unchanged. Natively, when the line shows a command, the pair is one seqlock read of the line (`spu_llar_noop_commit`, up to 8 tries; with 1 try 6.4% fell back to the lifted path, with 8 0.06%); the GET of the command and its tag wait still go through the channels. An empty ring returns 0 and the lifted code waits exactly as before. Same run: 77.58 -> 89.30 fps (+15%).
- `87115e8`: while the LS copy from the last read still shows a put ahead of get, the commands up to it are published (the producer moves put only past whole commands), so the line is not read at all. 89.92 -> 97.20 (+8%).
- `SPU_NATIVE_RING` (live): 1 the above (default), 2 reads the line for every command, 0 the lifted code. Regress ok; six-minute soaks in the Nexus and the Palace at `DES_FPS=120` ok.
- No gain: extending the native block over the state PUT that follows (0xC358).
- 2026-10-20, `spu_hook_ring_post`: the end of each command, LS 0x4808-0x4818, the call to 0x3C90 that stores the next output position into word 3 of 0x13350. While the local buffer has room and the command is not 35 (99.94% of commands), 0x3C90 only saves 13 registers, reads the buffer's descriptor at 0x13380, stores cur and restores them; the block does the same in C. The segment switch and command 35 run lifted. `SPU_NATIVE_POST` (live): 1 the block (default), 0 the lifted call, 2 a check: the lifted call runs and the next fetch compares it with what the block would have left (`[ring-post]` lines). Check mode at the deep Palace save: ~90 million commands, 0 mismatches. In-boot A/B, uncapped (`RSX_VSYNC=0 DES_FPS=unlocked`): deep Palace save +3.8% with `RSX_PRESENT=0` (p = 0.019; ~130-140 fps) and about +5% presenting; tutorial (~430 fps) -0.2%, p = 0.87, no change, since image 2 is not on the frame's path there. Regress ok at 30 and 240.

**render_frame off the frame's path** (item 2): measured, then dropped; not implemented.
- Causal, after item 3: `RSX_FRAME_SPIN_US=1000` cost 0 fps, so render_frame was off the path. After item 4 it is back on: a 1.5 ms spin costs 1.0 ms a frame (98.7 -> 89.8 fps).
- From a console `trace`: flip -> `present` 2.4 ms, `present` -> label `0xFF0` 0.5 ms, `0xFF0` -> next flip 7.1 ms.
- The cache-hit test's texture checksums in the prepass (~235 textures a frame, ~1,000 scattered reads each) were the obvious cut. Three ways, all without gain:
  - At draw time, invalidated by the 2D engine's writes and the render-target write-back: flip -> `0xFF0` 2.97 -> 1.87 ms, but `0xFF0` -> flip 7.13 -> 7.75 ms. Same run: 100.40 vs 100.53 fps.
  - The same, on the vertex-fetch worker: `0xFF0` -> flip still 7.66 ms.
  - In render_frame on three helper threads, before the prepass: flip -> `present` 2.54 -> 2.02 ms, `0xFF0` -> flip 7.20 -> 7.44. Same run: 99.75 (on) vs 100.73 (off).
- Each time the saved time came back in HighGraphics' window. The frame has a second chain of about the same length: HighGraphics -> ring -> image 2. Adding time to render_frame costs, but taking it away only lets HighGraphics start building a frame it then waits on image 2 to drain.
- Also no gain at the time: a branch-light strip walk on the FIFO thread (`vf_strip_list`, checked against the old walk with 0 mismatches over 6.8M draws), and `tex_csum` without the divide (+0.6%; again nothing at ~100 fps, 100.85 vs 100.53). The strip walk paid off later, once the FIFO thread was on the path; see "The FIFO thread joins the chain" below.

**HighGraphics is the critical path** (causal, after item 4): `TMP_SLOW_COH_NS=100`, a 100 ns spin after each of HighGraphics' coherent stores (~4.1M a second, nearly all to the ring's line), cost 25% of the frame rate. Each nanosecond per store is worth about 0.25%.
- **lwmutex in user space** (ps3recomp PR #22): `sys_lwmutex` was a kernel semaphore per mutex, and every lock and unlock a system call, waited on or not: ~90,000 of each a second on HighGraphics at ~0.4 us apiece, 7% of the thread. The count now lives in the mutex's slot (one compare-exchange to lock, an exchange to unlock); only a contended lock sleeps, on a condition variable in the slot. Alternated builds, six windows each: 98.17 -> 101.95 fps, every window above every window of the old build. Regress ok; six-minute Nexus soak at `DES_FPS=120` ok (~118 fps there, ~115 before).
- **The reservation fields on a line of their own** (ps3recomp PR #23): `PPU_COH_HIST=1` shows the ring's line taking ~4.4M of HighGraphics' stores a second, and each store's notify read `resv_valid`/`resv_ea` from image 2's context, on the same cache line as the MFC staging registers, the tag status and `atomic_stat`, which image 2 writes several times per command. Every notify was a cross-core miss. 64 bytes of padding on each side of the pair (contexts are malloc'd or on a stack, so no alignment attribute): alternated builds, 101.80 -> 105.38 fps, every window above every window of the old build. `spu_coh_notify_write` went from 6.8% of HighGraphics to 2.0%. Regress ok; six-minute Nexus soak ok.
- No gain:
  - The per-slot line tags in front of the notify's context reads (`SPU_COH_TAGS`), before and after the padding (105.68 vs 106.30 fps).
  - Waiting on an empty ring natively: the native fetch reading the put word until it moves, instead of the lifted wait's GETLLAR per pass (so HighGraphics' stores would find no reservation to break): 99.07 (on) vs 99.30.
  - One runtime call for the whole coherent store (stripe, copy, notify, release) with the stripe released by a plain store instead of a locked increment: 105.17 -> 105.93, inside the noise.
  - A per-thread cache of the SPU's MFC engine slot (the lookup scanned slots next to engines other SPU threads write) plus a per-thread DMA repeat counter: 104.98 -> 105.13.
  - `USLEEP_SPIN=1`, a third time: 104.20 -> 103.03.

**Both chains are critical now** (causal, after the padding, ~106 fps): 50 ns more per ring command in image 2 (`TMP_SLOW_RING_NS=50`, a temporary knob in the native fetch) costs 13.8% (106.55 -> 91.82); 50 ns more per HighGraphics coherent store (`TMP_SLOW_COH_NS=50`) costs 9.8% (105.80 -> 95.40). With ~42,000 commands a frame, that is ~71% of image 2's added time showing up in the frame and ~52% of HighGraphics'.
- Why neither is fully critical: HighGraphics' ring writers (`func_00ACAFA8`, `func_000B90F0` and siblings) send command `0x9023` with a 0x90-byte payload and then poll a reply word in `usleep(30)` until image 2 answers; the reply (minus 88) becomes the new command-buffer position. Each one drains the ring: HighGraphics waits (11.8% of its time in `usleep`) while image 2 catches up, then image 2 naps on the empty ring (14.4% of its time in `SleepConditionVariableSRW`) while HighGraphics produces again. Faster wake-ups do not help (`USLEEP_SPIN` above); only less work per command in either thread does.
- Profile now (`profrip`, ~105 fps): HighGraphics 96% of a core, its ring stores ~10% (`spu_lockline_unlock_ea` 2.3%, `spu_lockline_lock_ea` 2.1%, `spu_coh_notify_write` 2.0%, `vm_write32` 1.9%, `spu_ll_acquire` 1.5%). Image 2 87%: region `0xC358` (the checked DMA PUT of its state after each command) 11.0%, region `0x4498` 8.0%, `0x3C90` 5.7%, `mfc_submit` (inlined into `spu_wrch`) ~10% with its helpers. FIFO thread 87%: the strip walk 16% (`vf_strip_list`), memcpy 11%, `tex_csum` 6.6%.

**The FIFO thread joins the chain** (ps3recomp PR #20, `vf_strip_list`): the branch-light strip walk had been kept as a live testbed knob. Cleaning the testbed out of the tree cost 7% (alternated builds, 106.57 -> 98.92 fps), and the knob alone, same run, is 100.23 -> 106.20. At ~100 fps the FIFO thread was at 78% of a core with room to spare; at ~105 it is at 87% with the fast walk and saturates without it. The walk is now the strip path of `upload_strip_vp_fetch`; fans keep the general loop. With three threads within a few percent of saturation, each one's slack has to be measured again after every gain elsewhere.

**The lifted code itself** (alternated builds, four rounds of two windows each):
- **The shadow stack compiled out** (ps3recomp PR #24, port PR #21, `DES_SHADOW_STACK`): the lift is made with `--shadow-stack`, which gives every lifted function a `DS_FRAME` guard pushing the guest address onto a thread-local stack for `ds_dump_shadow()` backtraces. That is a thread-local access (with its TLS guard) on entry and exit plus an unwind cleanup, in all ~94,000 lifted functions. The lifter now emits the define under `#ifndef DS_NO_SHADOW_STACK`, and the port build defines that unless configured with `-DDES_SHADOW_STACK=ON`: 104.39 -> 106.90 fps (+2.4%), all four rounds. A first try under a busy editor gave 103.80 -> 104.50, same direction. At ~67 fps the same change measured nothing (Step 2 above) and was dropped; at ~105 HighGraphics is on the path and its per-call overhead counts. The trade: with the default build, the console's backtraces and the `sys_cond` deadlock dump have no shadow stack to read (`ds_dump_shadow` says so); configure with `-DDES_SHADOW_STACK=ON` when chasing a hang. The lift still uses `--shadow-stack`, so switching needs only a rebuild.
- No gain: `__restrict` on the context pointer of every lifted function (a sed over the lifted chunks), so the compiler could keep `ctx->gpr` in registers across guest stores, which it now reloads after each `memcpy` into guest memory. 106.24 -> 104.48 fps (-1.7%), all four rounds, and the exe grew by 0.5 MB: more inlining and register pressure, not fewer loads where it matters. The same on image 2's lifted SPU code: 99.31 -> 98.51 (-0.8%).
- No gain: the strip walk split further (a no-inline outer loop, a copy specialised for 16-bit indices): 105.70 -> 105.64.

**HighGraphics' HLE calls** (alternated builds, four rounds of two windows each). With HighGraphics on the path, what every HLE call does before its handler counts, and it makes a few hundred thousand a second (`sys_lwmutex_*`, `sys_cond_*`, `cellSpurs*`):
- **The PRX export lookup** (ps3recomp PR #26): `ps3_hle_call` asks the PRX loader whether a loaded module exports the NID before it runs the HLE stub. That was a linear scan of the whole export table, and for most calls the answer is no, found only at the end. Now a hash index. The `LIBSRE_LLE_ONLY` list was also matched as text, with a `snprintf` of the NID, on every call into a loaded PRX's export; it is parsed once. 104.69 -> 107.15 fps (+2.4%).
- **The GCM pump and the profiler stamp** (ps3recomp PR #27): `ppu_gcm_pump()` took its guard and exchanged both pending words (three locked writes to lines every guest thread shares) to find nothing pending; it now looks first with plain loads. The profiler's breadcrumb ran a binary search over every lifted function to name the caller, and bumped a shared debug counter; it now stores the host return address and names it when read. 108.02 -> 109.59 fps (+1.5%), ahead in every round.

**The FIFO thread again: strips as strips** (ps3recomp PR #20, `vf_strip_native`). At ~109 fps the FIFO thread was back at 91% of a core, and a 300 us spin per batch (a temporary knob) cost 0.28 ms a frame: on the path, about 1:1. The strip walk was ~15% of it: every strip index became up to three list indices, with a winding choice per triangle. The fetch path now draws a strip as a D3D12 `TRIANGLESTRIP`: the guest's indices go into the index buffer byte-swapped and otherwise as they are, the restart index becomes the cut value `0xFFFFFFFF` (`IBStripCutValue` on the VP PSOs), and the VP pass switches topology when a draw's kind changes. A range that continues a strip starts with a cut and restarts it from the carry, with one degenerate triangle when the winding parity needs it. Same run, `VF_NATIVE_STRIP` alternated: 108.90 -> 112.70 fps. A temporary check ran both walks on each range and compared the triangles (degenerates dropped): 26 million ranges over the Tutorial and the Palace, no difference.
- The first version lost 20% (109 -> 88). It found the strip's last restart by scanning the index buffer backwards, and the index buffer is an upload heap: write-combined memory, which the CPU reads uncached. That scan was 29% of the FIFO thread. The walk now keeps the restart position as it goes and takes the carry from the guest's indices. Nothing on the CPU reads an upload heap back.
- After it the FIFO thread is at 83%, with slack again; HighGraphics (98%, 13% of it waiting for image 2's replies) and image 2 (89%) are the chain.

**HighGraphics and image 2: a ping-pong.** At ~112 fps HighGraphics spends 11% of its time in `sys_timer_usleep` (the `SwitchToThread` spin of its `usleep(30)` poll for image 2's reply), and image 2 naps 11% waiting for HighGraphics' next ring write. Time cut from either thread shortens the other's wait.
- **The PUT after every ring command** (ps3recomp PR #29, DemonsSoulsRecomp PR #22). After each 16-byte command image 2 calls a helper at LS `0xC358` that PUTs its state back and waits for the tag. Lifted, the helper's prologue works out the checks for every case (1/2/4/8 bytes, 16..16384 aligned, chunked, the halting assert) in SPU registers kept in memory before it branches: 11% of image 2. A native hook (`spu_hook_put_wait`, `--native-hook 0xC358`) does the case the ring takes, with the same stack stores and channel accesses in the same order, and hands every other case to the lifted code. Same run, `SPU_NATIVE_PUT` alternated: 112.4 -> 118.2 fps (+5.1%), ahead in every window. Far more than its share of image 2, because HighGraphics was waiting on it.
- **The per-DMA bookkeeping** (ps3recomp PR #28), ~5% of image 2:
  - `mfc_for` scanned the slot table on every channel access; each thread now remembers its slot.
  - `mfc_repeat_check`'s statics were shared by all SPU threads, so every DMA wrote a line the other SPUs wrote too; they are per thread now.
  - `mfc_ea_range_committed` was not inlined, because of its system calls on a bitmap miss; the bit tests are inline now, the miss out of line.
  - Two builds alternated, twice, in opposite orders: 111.1 -> 112.5, then 109.8 -> 110.4 fps (about +0.9%).
- **Not a gain: a shorter poll.** A temporary knob that cut HighGraphics' `usleep(30)` to 10 us lost 1.3% (110.4 -> 108.9): the extra polls cost more than the reply latency they save. Spinning on `pause` instead of `SwitchToThread` made no difference (117.0 vs 117.4).

**Where the time goes at ~118 fps** (`cpu 4` and `profrip`, all the open PRs):
- HighGraphics 96% of a core: 6.8% in its usleep poll for image 2, 6.1% in `sys_cond_wait`.
  - The cond wait is on the main thread. The console's new `synctop` verb (ps3recomp PR #30) ranks the sync-history ring. Each frame the main thread broadcasts cond `0x2` to hand HighGraphics a request, then waits on a cond it creates for that request (`0x2E`, destroyed after), which HighGraphics signals when it is done. HighGraphics' wait is the main thread's work between the two: game code, spread thin (the main thread is ~30% busy, the rest waiting for HighGraphics). Cond traffic is ~5,000 events a second, so the syscalls themselves cost nothing that matters.
  - Of its own work, the ring stores are ~11% (`vm_write32` inclusive) and the rest is lifted game code, none of it above 5%.
- Image 2 89%, napping 15%. The DMA runtime is ~16% (`mfc_submit` 5.9%, `spu_wrch` 4.8%, `mfc_for` 2.7%, the repeat check, memcpy), indirect-branch dispatch ~6.5%, the ring loop region `0x4498` 10.5%.
- The FIFO thread 86-88%, `d3d12_draw_indexed` hot (a fifth of it in the driver).
- Image 2 is off the margin now: inlining `mfc_for`'s fast path (2.7% of it) gave 116.9 vs 117.0. The next gains are on HighGraphics.
- **Not a gain at a 120 Hz vblank: an indirect-call fast path** (+2% at 240 Hz, ps3recomp PR #33; see "The A/Bs were capped at 120" below). `ps3_indirect_call` tests about fifteen diagnostic knobs, one static at a time, before every call (~2.5% of HighGraphics, which makes most of them). A fast path that skipped them all while none was set gave 115.8 vs 116.9 fps, then 117.1 vs 116.9 with the builds in the other order: nothing either way.

**The FIFO thread on the path again** (2026-10-17, ~118 fps). Two trims in a row with nothing to show, on HighGraphics and on image 2, sent the question back to which thread sets the frame. Causal, a temporary spin knob per frame, same run each:
- 1 ms on the FIFO thread (in `present_guest_frame`): 117.55 -> 106.62 fps, 0.87 ms a frame.
- 1 ms on HighGraphics (in `cellGcmSetFlipCommand`, which it calls once a frame): 117.20 -> 107.52 fps, 0.77 ms a frame.
- Both chains are within a fraction of a millisecond of each other, so a cut to either alone shows almost nothing. The FIFO thread's share had come back with image 2's speed-ups (the state PUT hook above).
- **Strip indices eight at a time** (ps3recomp PR #31). `profrip` on the FIFO thread had one loop at ~10%: `vf_strip_native_w` over 16-bit indices, a byte swap, a restart compare, the cut value and an unsigned min/max per index, as a chain of `cmov`s. It now runs eight indices per step with SSE2 (the min/max biased by 0x8000, since SSE2 has only the signed ones; the last restart from the compare mask). 32-bit indices and the tail keep the scalar loop; `VF_STRIP_SSE=0` (live) uses it throughout. A temporary check ran both on every range: 24 million ranges, no difference. Same run, alternated: 107.3 -> 113.1 fps, then 111.0 -> 114.0 in the other order (about +4%).
- After it the FIFO thread is at ~80% of a core, HighGraphics 97%, image 2 90% (13.5% of it napping).
- **SSE4.1 for the lifted PPU code** (DemonsSoulsRecomp PR #23), dropped at ~67 fps in Step 2 above: with HighGraphics on the path, alternated builds, 115.60 -> 117.84 fps, then 114.75 -> 116.94 in the other order (+1.9%).
- Not a gain (at 120 Hz; see below): image 2's MFC engine slot kept in the SPU context (one load and a check instead of the thread-local lookup, `mfc_for` 3.6% of image 2): 114.95 vs 114.02 fps, alternated builds.

**The A/Bs were capped at 120** (2026-10-17). They ran at `DES_FPS=60 VBLANK_MHZ=120000`, and 60 mode flips at most once a vblank, so no window could pass 120 fps; the baselines read 117-118. The mean stayed under the cap, but every frame under 8.3 ms waited for the vblank, and that squashed the gains near it. The indirect-call fast path, same two builds: +0.9% at 120 Hz (116.65 -> 117.71), +1.9% at 240 Hz (116.97 -> 119.19), +2.0% in the other order (118.14 -> 120.50, windows up to 123.5). `tools/des/exeab.sh` now runs at `VBLANK_MHZ=240000` (DemonsSoulsRecomp PR #25), and knob A/Bs pass the same rate.
- Re-checked at 240 Hz (alternated, four rounds; `VF_STRIP_SSE` live, ABBAABBA):
  - The strip loop eight at a time: 115.55 -> 116.95 (+1.2%).
  - SSE4.1 for the lifted PPU code: 115.42 -> 116.79 (+1.2%).
  - The indirect-call fast path (ps3recomp PR #33): +1.9%, then +2.0% reversed, as above. Dropped at 120 Hz earlier the same morning.
  - The MFC slot in the SPU context, "not a gain" at 120 Hz: 115.71 -> 117.11 (+1.2%) at 240 Hz; then, on top of the three above, 120.46 -> 121.80 and 119.59 -> 120.36 in the other order (+0.6 to +1.1%, one round the other way). Inside the window spread, so still dropped.
- With the three in, the Palace runs ~120 fps uncapped (presenting; 120.4 and 119.6 for the baseline build in the last two runs). Regress ok (set `20261017_095353`).
- Physics above 60 fps is still untested (RPCS3 warns of trouble there); these runs measure frame rate only.

**Where the time goes now** (`cpu 4` and `profrip`, ~97 fps, before the lwmutex change):
- HighGraphics 98.8% of a core. Its ring stores are ~15%: `spu_coh_notify_write` 6.8%, `spu_lockline_unlock_ea` 2.8%, `spu_ll_acquire` 1.8%, `spu_lockline_lock_ea` 1.7%, `vm_write32` 1.7%. The lifted functions are spread thin, none above 2.3%.
- Image 2 84%. The state PUT after each command (region `0xC358`) 11.8%, `mfc_submit` 6.2%, `spu_wrch` 5.3%, region `0x4498` 8.0%, the native fetch 5.0%, region `0x3C90` 5.4%; napping ~15%.
- FIFO thread 78%, not saturated.
- Next levers: the per-command state PUT in image 2 (r3 = EA, r4 = LSA, r5 = size, synchronous), and HighGraphics' ring stores (each takes the line's stripe and notifies image 2).

## Camera catch-up at any frame rate (ps3recomp `--insn-hooks`, port `des_cam_rate`, 2026-10-17)

**Symptom:** above 30 fps the camera caught up with the player too fast when walking, not only when turning it with the right stick. It was fine at 30 fps.

**Cause:** the cameras ease towards their targets by a fixed fraction a frame, `cur += (target - cur) * k`, with k tuned for 30 fps. The camera manager (`0x00115370`, vtable `0x01855FB8`) passes the real frame time to the follow camera's update (`0x00119670`, ChrExFollowCam). But the eases ignore it, so at 240 fps the camera takes eight steps where the game takes one. The main one is the follow point's chase at `0x0011C2BC`: `lfs f0, 0x17C(r31)` (k = 0.1), splatted into a vector lerp. The same k is stored to `r1+0xC4` for the eases further down.

**Fix:**
- ps3recomp PR: the lifter's `--insn-hooks ADDR=sym` calls `sym(ctx, addr)` right after an instruction.
- The port lists each load of k in [`port/insn_hooks.txt`](../port/insn_hooks.txt); the re-lift recipe (docs/27, README) passes it with `--insn-hooks @port/insn_hooks.txt`.
- `des_cam_rate` (port/des_options.cpp) reads the lfs at that address for its register and replaces k with `1 - (1 - k)^(dt * 30)`. That leaves the same remainder per second as k does at 30 fps. dt is the game's frame time (`0x018C8DB4`).
- It does nothing at 30 fps, in a cutscene (dt is 1/30 there) or without DES_FPS.
- `DES_CAMFIX=0` turns it off live. `DES_CAMLOG=1` prints per-site counts every 20000 calls.

**How the sites were found:** read-only sampling of the manager and its sub-cameras during a scripted walk (levelcap plus a ReadProcessMemory sampler; the manager is found by its vtable, since its heap address moves with DES_FPS), at 30 and 240 fps. Then a pattern search over the lifted camera functions for `a + (b - c) * k`, and the k loads located in the EBOOT by their encoding. `DES_CAMLOG` showed that 7 of the 15 sites run every frame while walking: `0x119818`, `0x1198D8`, `0x119FAC`, `0x11A018`, `0x11B830`, `0x11C0E8` and `0x11C2BC`. The ChrFollowCam and second-camera sites did not run on that walk. They are hooked because they follow the same pattern.

**Result** (the same walk; distance from the camera's follow point at `sub+0x90` to the player while moving):

| Run | Median lag | Lag per m/s |
|---|---|---|
| 30 fps | 0.69 m | 0.27 |
| 240 fps, `DES_CAMFIX=0` | 0.21 m | 0.08 |
| 240 fps, fix on | 0.87 m | 0.31 |

The ~10% left is expected: a smooth ease at fine steps lags `v * tau` (tau = 0.316 s for k = 0.1 at 30 Hz), while 30 Hz steps lag `v * dt * (1 - k) / k` = 0.30 s.

**Watch out:**
- Changing the hook list touches `ppu_recomp.h`, so all 14 chunks rebuild (~15 min).
- A site may be hooked only if its register is used as an ease factor before it is overwritten. Check the lifted code after the load, not just the field's name.
- Vector stores in the lift are raw `memcpy`, so `PPU_WWATCH` does not see the camera's vec4 fields change. Sample them instead.

## Snagged on small ledges above 30 fps (port `des_slide_rate`, 2026-10-17)

**Symptom:** at high frame rates (easiest at 240) the player could get caught on a small lip in the ground. Walking into it, the character rose a few centimetres, slid back, and repeated forever. At 30 fps the same spot is crossed without a hitch. Reproduced in the Tutorial at about (135.54, 3.06, -3.32): a ~47° lip at the foot of a ~20° ramp.

**Cause:** the slide on ground too steep to stand on runs once a frame without dt. Each frame, a character proxy's ground probe (`0x00200E60`, called from the proxy step `0x00201D78`) casts a ray down. If the face it hits is steeper than the walkable limit, it sets wrapper `+0x33`. The velocity update `0x001FE778` then:
- on a steep frame, adds gravity along the slope times a gain of 1.0 (`0x019D9C3C`) to the slide velocity at wrapper `+0x100`;
- on any other frame, multiplies the slide by 0.65 (`0x019D9C40`), and zeroes it under 0.1 m/s.

Both values are in the game's debug menu (registered at `0x00202C44` and `0x0020340C`); nothing else reads them. A 30 fps step (several cm) carries the character over the narrow steep band, so the probe rarely lands on it, and one steep frame adds about 0.7 m/s. At 240 fps the steps are about 1 cm, the probe lands on the lip for several frames in a row, and the slide adds up to about 3 m/s, more than the walk.

**Fix:** two more lines in [`port/insn_hooks.txt`](../port/insn_hooks.txt) call `des_slide_rate` (port/des_options.cpp) after the two `lfs f0` loads:
- `0x001FED50`, the gain: scaled by `dt * 30`;
- `0x001FEAF4`, the decay: raised to `dt * 30`.

dt is the step's own (`r26 + 0x20`, the one the same function integrates gravity with), so the slide per second matches 30 fps. It does nothing at 30 fps or without DES_FPS. `DES_SLIDEFIX=0` turns it off live; `DES_SLIDEFIX=2` adds a grace period (below). It applies to every character proxy, on whichever thread steps it.

**Result** (scripted pushes into the lip, 5 offsets x 2 angles, each warped onto the line mid-stride, 3 s each):

| Run | Snags |
|---|---|
| 240 fps, `DES_SLIDEFIX=0` | 5 of 10 |
| 240 fps, fix on | 0 of 10 |
| 60 fps, fix on | 0 of 10 |
| 30 fps, fix on | 0 of 10 |

**At a slow walk the original game snags too.** The same lip with the stick at half (6 angles x 5 offsets, 3 s each):

| Run | Snags |
|---|---|
| 30 fps | 20 of 30 |
| 240 fps, fix on | 21 of 30 |
| 240 fps, `DES_SLIDEFIX=2` | 12 of 30 |

So with the fix, 240 fps matches 30 fps at both speeds. `DES_SLIDEFIX=2` is kinder than the game. A slide starts only once the ground has been too steep for 1/30 s without a break, so a lip crossed faster than that is never slid on (the run is kept per proxy, `r29` = the wrapper, in a small table per thread). A real steep slope then slides at most 1/30 s late. It is off by default because it changes how the game plays.

**How it was found:**
- The frame log's speed factor (`DES_FRAMELOG`) stayed at 1.0, which ruled out the 30 Hz stuck test that `des_move_post` already handles.
- Read-only sampling of the character, its movement object and the proxy wrapper during the pushes showed the wrapper's ground fields toggling each bounce.
- `PPU_WWATCH` on wrapper `+0x184` (the ground material id) found the ground probe.
- Its disassembly (`ps3recomp/tools/ppu_disasm.py`; Ghidra stops at its VMX) gave the steep flag. The flag's readers led to `0x001FE778`.

**Watch out:**
- Ghidra cannot decompile `0x00200E60` or the steep branch of `0x001FE778` (it gives up at a VMX instruction). Read the disassembly instead.
- Wrapper `+0x184` is not Havok's supported state. It is the ground's material id (2 for the Tutorial floor); `+0x32` is on-ground, `+0x33` too-steep, `+0x220` the slope angle in degrees, `+0x150` the ground normal, `+0x160` the time in the air.
- Other per-frame accumulators may hide in the same class (`0x001FE778`'s neighbours `0x001FF160`, `0x001FF8F0` also write `+0x33`). They were not on this path.

## Locked-on direction changes above 30 fps (port `des_twist_rate`, 2026-10-25)

**Symptom:** moving while locked on, above 30 fps (reported unlocked), the character's change from one direction to another looked jumpy, and so did holding the stick on a diagonal. Rolls felt odd too. At 30 fps it all looked right.

**How locked-on movement is animated:** the locomotion controller (vtable `0x0185C338`, update `0x003259F8`) plays one of four direction clips: forward 710, back 711, left 712, right 713 (`+0x138`). Each clip change asks for a 0.2 s blend (the anim request's slot `+0x18`); the blend and the clips' playback run on the update's dt. For any other direction it turns three bones by the angle between the move and the clip's direction: `Master` gets it all, `Upper_Root` and `Neck` turn back by a share of it, so the legs point along the move and the chest stays toward the target. The bone objects are at `+0x144`, `+0x140` and `+0x148`; each has its target angle at `+0x10`. On a diagonal the clip changes when that angle passes about 55-62°, and then the target jumps by about 85° the other way. The jump comes at the same game time at 30 fps and unlocked.

**Cause:** the bone turner (`0x003D3BC0`, called from `0x00322A60` for the three bones) does not set the bone to its target. It eases toward it by a fixed share an update (`0x003D42B0`):

    cur (+0x14) += (target (+0x10) - cur) * k (+0x20)

k is the controller's `+0x154`, 0.35 (set by the constructors `0x00322428` and `0x00322DD0`, stored to each bone at `0x00322AB0`). With k = 0.35 the gap is 10% after 5.3 updates: 178 ms at 30 fps, alongside the clips' 0.2 s blend. At 240 fps it closes in 22 ms, and unlocked (about 500 fps) in 11 ms. So above 30 fps the body snapped round to the new angle while the legs were still blending between clips.

**Fix:** one line in [`port/insn_hooks.txt`](../port/insn_hooks.txt) calls `des_twist_rate` (port/des_options.cpp) after `lfs f12, 0x20(r31)` at `0x003D42BC`. k becomes `1 - (1 - k)^(dt * 30)`, with dt the game's frame time (`0x018C8DB4`). That leaves the same gap per second as at 30 fps.
- It does nothing at 30 fps or without DES_FPS. A bone with k = 1 (the turner's default) stays at 1.
- The turner's other user, a two-bone controller (`0x0031B56C`, k from its `+0x60` or `+0x64`), gets the same correction. It eases the same way.
- `DES_TWISTFIX=0` turns it off live.

**Result:** a scripted session in the Tutorial, locked on: holds at 45° and 135°, the same with noise on the stick, a full sweep, and swings between directions. "Settles" is the time from a jump of the `Master` target (more than 0.5 rad, with a clip playing) until the drawn angle (`+0x14`) is within 10% of the jump:

| Run | Frame | Jumps | Settles (median) | Range |
|---|---|---|---|---|
| 30 fps | 33.3 ms | 18 | 167 ms | 133-200 ms |
| Unlocked (532 fps), `DES_TWISTFIX=0` | 1.9 ms | 31 | 12 ms | 10-14 ms |
| Unlocked (515 fps), fix on | 1.9 ms | 26 | 162 ms | 101-229 ms |

The drawn angle after a jump of 0.82-0.84 rad, at 30 fps and unlocked with the fix:

| After | 25-33 ms | 67-73 ms | 97-100 ms | 122-133 ms | 167-171 ms | 196-200 ms |
|---|---|---|---|---|---|---|
| 30 fps | 0.28 | 0.47 | 0.59 | 0.67 | 0.73 | 0.77 |
| Unlocked, fix on | 0.23 | 0.52 | 0.60 | 0.67 | 0.76 | 0.79 |

Rolls were not measured. When the clip stops, the target goes to 0 and the same ease turns the body back, so a roll from a diagonal probably snapped in the same way above 30 fps.

**How it was found:**
- A scratch frame-log probe (`DES_LOCOADDR`) recorded the controller, its bones and the anim request slots every frame during scripted locked-on holds, noisy diagonals, a sweep and direction swings, at 30 fps and unlocked. The clip changes, the blend times, the slot clocks and the target angle all matched in game time, so the controller itself was not the cause.
- Frame dumps (`CELLMARK_DUMP_NOW`) did not help: at the test spot the camera was pushed against a wall and the legs were off screen.
- The bone names (`Master`, `Upper_Root`, `Neck`) led to the setup (`0x003228B0`) and the bone class (`0x003D35B8`). Its only read of `+0x10` is the ease, and the controller method that stores `+0x154` into each bone's `+0x20` (`0x00322AB0`).

**Watch out:**
- The probe's bone angle at `+0x10` is the target, not what is drawn. The drawn angle is `+0x14`.
- `0x003259F8` and `0x003D3BC0` stop Ghidra at a VMX instruction. Read them with `ps3recomp/tools/ppu_disasm.py --va`.

## Still open

- **Graphics in the level**: HUD, half frames and particles fixed (`dc3bb6f`, `c01c7f7`, `3f094e1`). Holes in the level geometry fixed (`5a78630`), black characters (`9db1a5f`), sun flare through walls (`df9d0f9`, not yet checked live with the sun in view). Lit, textured, culled and depth-tested correctly, with shadows (`3470cbd`, `4e229b7`, `fb24382`). Depth read as colour works (`b4821e0`: light shafts, haze, motion blur). Frame rate about 28 (cap 30) since `ba6305d`; see Frame rate. Heaviest Palace view: 13-14% skip frames since `099343f` (was 29%). Still: no side-by-side check against RPCS3 at the same spot yet.
- The SCREENTEX hack: narrowed to samplers over empty guest memory (`ee40816`).
- The "intermittent stall early in the boot" was the test harness (port `fde7885`). Caught live on try 11 of 11 (`waits`/`gstacks`): main was polling the pad at the auto-save notice and no button was ever pressed. Another thread's log write had landed inside the "GetData polling begins" line (`[cellPad] G[THREAD 22] unwound ...`), and desprobe waited for that exact line. It now also accepts `[cellPad] PAD_FILE=`.
- Logo fades and loading-screen seams.
- Audio: the block gaps are fixed (`1e4d419`); not yet compared with RPCS3 by ear. FMV plays (`5a63029`) with correct sound, frame rate and subtitles (`7f48932`, `ad2d649`, `0dc0854`, `d57cea9`, `6dae2e5`); the user checked all three movies. Controller: DualSense over HID (`4efcb5e`) not yet tried live; no rumble for Sony pads (since added, docs/95).
  Audio leads: ATRAC streaming is fixed (`a90fc9a`). `cellAudioSetPortLevel` (0x56DFE179) is still faked as CELL_OK. MultiStream's own ATRAC path needs `flashATRAC.pic`, and loading it hangs the boot (above).
- **Camera catch-up too fast above 30 fps** (reported 2026-10-17, `DES_FPS=240`, tutorial to the Nexus). This isn't only the right stick. When the player just walks, the follow camera swings back behind them faster than it should. Likely cause: the follow is a per-frame blend (a fixed fraction each update) instead of one scaled by the time step. `DES_FPS` only replaces the 1/30 step at 0x018C8DB4, so code that never reads it still runs once per frame, and runs 8 times as often at 240. RPCS3's unlock patch replaces the same single constant, so RPCS3 should show the same thing at 60. Next: compare 30, 60 and 240 at one spot, find the camera update (the per-frame code that writes the camera's position/angle; `PPU_WWATCH` on the camera object), and see whether it reads the step.
- **Snagging on small ground geometry** (reported 2026-10-17, `DES_FPS=240`). The player sometimes catches on a low obstacle, such as a plank on the floor. Walking the same way again goes past fine, so it depends on exactly where the player is on a given frame. Suspects:
  - the Havok character controller run with a very small step (about 4 ms at 240 instead of 33): step-up and penetration tolerances are tuned per step;
  - a lifter or SPU float difference in the collision code.

  Next: check whether it happens at `DES_FPS=30`. If it doesn't, it belongs with the RPCS3 patch's warning about physics above 60.
- **Remaining cold-cache hitches** (prof_012, 2026-10-17, tutorial to the Nexus at `DES_FPS=240`, first run after clearing the shader cache): 224 fps average, 15 presents over 36 ms in 250 s, and only 1 shader compiled during play. The worst five are D3D12 pipeline creation on the render thread: 43 pipelines took 81 ms at t=25 s, 17 took 92 ms at t=235 s, 42 took 73 ms at t=31.6 s, plus 30.7 ms and 72 ms at t=224 s and t=216 s. A second run, with the pipeline library warm, should not have them; docs/30 has the prediction plan for the first run.
