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

### Using an existing renderer instead

Considered and rejected for now:
- RPCS3's RSX backends are the only renderer known to draw Demon's Souls correctly, but they are GPL and tied to RPCS3's emulator core (its memory manager, threads, shader decompiler and surface cache), so they cannot be dropped into ps3recomp.
- Upstream ps3recomp's `rsx_live_draw` (`RSX_LIVE_DRAW=1`) has no write-back, turns F_X32 into RGBA8 and does not handle swizzled render targets, all three of which DeS needs.
- Upstream's `rsx_draw_engine` does write back, but only for Vulkan and Metal, and its Vulkan path builds only on Linux.

Upstream ps3recomp had nothing newer than `a2c2010`, which is already merged.

## Debug and test tools (2026-10-14)

All of these keep game-derived output (screenshots, captures) in the scratch directory; none of it goes in the repo.

- **Staged exe** (port `2d6b3ab`): `desrun.sh`, `repro.sh` and `milestones.sh` copy `des_boot.exe` (and its `.pdb`, `.map`, DLLs) to `$DES_SCRATCH/bin` and run the copy, so the build can relink while a run is alive. `sym.py` against a staged run: `--map "$DES_SCRATCH/bin/des_boot.map"`.
- **Live knobs** (`e58297f`): the console's `setenv K=V` bumps `ps3_env_gen`, and knobs written as `PS3_KNOB(var, expr)` (`include/ps3emu/ps3_knob.h`) re-read on their next use (one load and compare otherwise). 272 sites were converted mechanically (`CULL_*`, `DEPTH_OFF`, `FP_*`, `FRAME_BUDGET`, `RT_OFFDBG`, `CELLMARK_DUMP_CHANGED`, ...). Pipeline-baked knobs (`CULL_*`, `DEPTH_OFF`, `FP_*`, `CMASK_FORCE`) rebuild the pipeline cache when their values change. `PAD_STICK=lx,ly,rx,ry,from,until` is live and its window counts from the setenv: `setenv PAD_STICK=128,0,128,128,0,10` walks forward for 10 s. Still read once (multi-statement parsers): `PAD_FILE`, `PAD_SWEEP`, `DBG_ZOOM`, `CELLMARK_DUMP` / `_SKIP` / `_EVERY` / `_DIR`, `D3D12_IQ`, `GCM2D_WATCH`, `TEX_NOCACHE`, `WATCHDOG_AT`, `GCM_FLIP_BT`, and most `SPU_*` / `PPU_*` / `PS3_*` trace knobs.
- **Manifest fields** (`d613b7f`): `RSX_CAP` manifests show `ct=` (colour target), `z=` (zeta), `zf=`, `depth=` (packed depth key: bit 0 test, bit 1 write, bits 2-4 function), `cull=`, and on clears `mask=`, `z=`, `zeta=`. A small phase summariser (one line per run of ops with the same target/zeta/depth) made the DeS frame layout readable: display copy, Z clear, three shadow cascades into zeta `0x5360000` (2048x2048, colour target 0), scene into `0x7A0000` / zeta `0xB50000`, post chain, HUD.
- **Regression shots** (`tools/des/regress.sh`, `shotdiff.py`): boots with `DES_SKIP_INTRO=1`, walks a fixed route with the live `PAD_STICK` (spawn, tunnel exit, camera turn, courtyard) and saves one shot per stop under `$DES_SCRATCH/regress/<set>`, then diffs against the accepted set (`regress.sh accept <set>`). Per shot: mean difference and the share of pixels that changed by more than 32 after a 4x downscale; REGRESSION above 25% or when a shot goes black. Two runs of one build differ by under 8%. The save directory is copied aside and restored, so autosaves cannot move the spawn. About 2.5 minutes.
- **Frame capture and replay** (`0d8c4f2`): `setenv RSX_REPLAY_CAP=<file>` in a running game records the next frame (from the clear the backend presents at to the next one): RSX state and GCM tables at its start, every method the FIFO walker dispatched, and the committed non-zero pages of VRAM and the IO-mapped memory at its end (DeS courtyard: 46k methods, 117 MB). `ps3recomp/build/rsx_replay.exe <file> [frames]` (`cmake --build build --target rsx_replay`) plays it through the same dispatch and D3D12 backend with no game: about 4 s for the first frame (pipeline compiles), then about 80 ms per frame. `CELLMARK_DUMP=<n> CELLMARK_DUMP_DIR=<dir>` dumps the presented frames, `RSX_CAP=<n> RSX_CAP_DIR=<dir>` the manifest and render targets. The exposure adaptation reads the previous frame, so the first replays are darker; by about 20 frames it matches the live frame. The capture and the replayer must come from the same build.
- **CPU per thread** (`e4aea1e`): console `cpu [s]` (default 5 s) prints each host thread's CPU (percent of one core), the split between the exe (`app`), ntdll/kernel32/KernelBase (`os`: yields, spin-waits, syscalls) and other DLLs (`dll`: driver, D3D12, audio), the guest thread name or the host start function, and the hottest exe function (for os/dll time, the exe function that called out). A blocked thread costs nothing, so a waiting thread with a large `os` share is spinning. First reading in the level (3.8 cores): an SPU executor thread 96% (`spu_shufb`), an SPU async thread 71%, HighGraphics 68%, `vblank_ticker` 49%, `MOStreamingThread` 28% inside `sys_timer_usleep` (99% os), `_sys_MixerChStrip` 18% in `sys_cond_wait` (84% os). The last three wait more than they work.
- **Lifted-code index** (`tools/des/recomp_index.py`): `def` / `src` / `at` / `callers` / `callees` / `path` over `work/recomp`, from a SQLite index (94k functions, 130k direct-call edges) that rebuilds in seconds when a lifted file changes. codebase-memory-mcp did not finish indexing the 450 MB of generated code in 30 minutes, so this replaces it for the lift; the hand-written code stays in the general index.

## Still open

- **Graphics in the level**: lit, textured, culled and depth-tested correctly, with shadows (`3470cbd`, `4e229b7`, `fb24382`). Still missing: the post chain samples the scene depth (zeta `0xB50000`) as an A8R8G8B8 texture on t1/t2 (depth read as colour), which the backend does not support yet; about 14 fps.
- The SCREENTEX hack.
- Logo fades and loading-screen seams.
- Audio quality and coverage (it plays now; not yet listened to or compared with RPCS3), FMV decode, controller support.
  Audio leads: unresolved imports `cellAtracGetStreamDataInfo` (0x2BFFF084), `cellAtracIsSecondBufferNeeded` (0x99EFE171), `cellAudioSetPortLevel` (0x56DFE179) are faked as CELL_OK, and MultiStream logs "atrac failed to load" after the `flashATRAC.pic` open fails.
