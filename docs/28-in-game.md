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
- **Fog wall and Nexus cutscene colours (open).** The user reported the fog wall flickering (`cap_017`), odd colours walking through it (`cap_018`), and odd colours in the Nexus cutscene (`cap_019`-`cap_022`), and said the replays did not show what they saw. The replays render both scenes. One way live can differ: a texture bound at an offscreen target's offset sampled the target for good, even after the title had put a texture or an image in that memory; a replay starts with no targets and reads the guest bytes. With colour write-back on, the guest bytes after a store are our own, so a checksum taken then and compared at the start of each frame tells a foreign write, and the target is dropped (`7769d7c`, `RSX_RT_REWRITTEN=0` off, `[RT] ... rewritten by the title` logged). It does not fire on the regression walk or in Boletarian Palace; whether it fires at a fog wall is not known yet. F9 now saves the live presents with each capture (`b039057`), so the next report shows the screen beside the replay.
- **Nexus colours: sampler tables ran out** (`b73b4ef`). The next set of captures (`cap_023`-`cap_029`) came with live presents (`b039057`), and those showed what the replays could not: in the Nexus cutscene (`cap_027`) bright areas wrapped to blue, magenta and flat grey (white came out 127,127,127) where the replay was plain overexposed white, and entering the Nexus (`cap_028`) a dark foreground arch (op 1147, an ordinary level shader) was covered in red, yellow and grey. The player's log had `[D3D12] sampler tables full (128); later combinations reuse table 0` just before those captures. Sampler tables (16 samplers each, one per unit) were made per distinct combination and kept for the whole run; D3D12 allows 2048 shader-visible samplers, so 128 tables, and the tutorial plus the Nexus used them all. Every later combination got table 0's addressing and filtering: the final copy read the 256x1 grading LUT with linear wrap, so white blended the LUT's last entry (255) with its first (0) and came out grey. A replay starts with empty tables, hence the mismatch. A full heap now rewrites the table used longest ago that the frame being recorded has not bound (after draining the queue if the GPU may still read it); only a single frame with more than 128 combinations falls back to table 0, and that is logged. `RSX_SAMP_TABLES=<n>` uses fewer tables to exercise the reuse: `regress.sh` with 40 reuses tables from the title screen on and is ok; a `cap_027` replay with 32 is pixel-identical, with 8 (one frame needs more) it degrades the way the live frame did.
  - Checked and ruled out on the way: the LUT itself (an identity ramp in the capture), the dropped-target change (fired 7 times in the session, each right after a level load), `RSX_LATE_CHECK` and `TEX_VERIFY` (silent through a boot into the Nexus), the screen-copy texture path (never bound live), and ZPASS (cap_023 has no occlusion report).
  - Still open: `cap_023` (first cutscene) alternates a dense and a faint fog layer between rendered frames; it came before the tables ran out. `cap_024`'s soul-form aura has long blue vertical streaks; that capture is a skip frame, so its replay is the live image and cannot be bisected. (The streaks turned out to be the aura's own look: `cap_034`, below.)
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
- **Regression shots** (`tools/des/regress.sh`, `shotdiff.py`): boots with `DES_SKIP_INTRO=1`, walks a fixed route with the live `PAD_STICK` (spawn, tunnel exit, camera turn, courtyard) and saves one shot per stop under `$DES_SCRATCH/regress/<set>`, then diffs against the accepted set (`regress.sh accept <set>`). Per shot: mean difference and the share of pixels that changed by more than 32 after a 4x downscale; REGRESSION above 25% or when a shot goes black. Two runs of one build differ by under 8%. The walk starts from a pinned save: `regress.sh pin-save [dir]` (port `d0ebf14`) copies a save directory (default: the current one) to `$DES_SCRATCH/regress/save`, and each run installs it over the player's save and puts the player's back after. Without it the route started wherever the player had last saved, and every shot regressed once the user's character moved on. About 2.5 minutes. The walk does not land in the same place every run (an enemy, a few frames of drift), so two runs cannot show what one knob does. `REGRESS_AB="KNOB=value"` (port `91c3cfd`) sets the knob live at the last stop and saves `s5_ab`, then clears it and saves `s6_back`, from one position.
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
- Checked against synthetic reports for every layout. Not yet tried with the live pad (the paired DualSense was off). Rumble is not sent to Sony pads yet.
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

## Still open

- **Graphics in the level**: HUD, half frames and particles fixed (`dc3bb6f`, `c01c7f7`, `3f094e1`). Holes in the level geometry fixed (`5a78630`), black characters (`9db1a5f`), sun flare through walls (`df9d0f9`, not yet checked live with the sun in view). Lit, textured, culled and depth-tested correctly, with shadows (`3470cbd`, `4e229b7`, `fb24382`). Depth read as colour works (`b4821e0`: light shafts, haze, motion blur). Frame rate about 28 (cap 30) since `ba6305d`; see Frame rate. Still: no side-by-side check against RPCS3 at the same spot yet.
- The SCREENTEX hack: narrowed to samplers over empty guest memory (`ee40816`).
- The "intermittent stall early in the boot" was the test harness (port `fde7885`). Caught live on try 11 of 11 (`waits`/`gstacks`): main was polling the pad at the auto-save notice and no button was ever pressed. Another thread's log write had landed inside the "GetData polling begins" line (`[cellPad] G[THREAD 22] unwound ...`), and desprobe waited for that exact line. It now also accepts `[cellPad] PAD_FILE=`.
- Logo fades and loading-screen seams.
- Audio: the block gaps are fixed (`1e4d419`); not yet compared with RPCS3 by ear. FMV plays (`5a63029`) with correct sound, frame rate and subtitles (`7f48932`, `ad2d649`, `0dc0854`, `d57cea9`, `6dae2e5`); the user checked all three movies. Controller: DualSense over HID (`4efcb5e`) not yet tried live; no rumble for Sony pads.
  Audio leads: ATRAC streaming is fixed (`a90fc9a`). `cellAudioSetPortLevel` (0x56DFE179) is still faked as CELL_OK. MultiStream's own ATRAC path needs `flashATRAC.pic`, and loading it hangs the boot (above).
