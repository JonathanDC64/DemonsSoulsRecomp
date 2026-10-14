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

Still wrong: the region that was black is now saturated cyan, with the same wedge outline over grey. The three scene meshes seem to project from almost on top of the camera.

## Still open

- **Graphics in the level**: the cyan wedge (above). About 260 draws a frame, about 13.6 fps. START/CROSS are read but show no visible change, so cutscene vs gameplay is unknown.
- The SCREENTEX hack.
- Logo fades and loading-screen seams.
- Audio, FMV decode, controller support.
