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
| `b838932` | SPU events on an unbound port: consume the data word, reply ENOTCONN (ports 12/14). |
| `760b682`, port `20b24dd` | `DES_SKIP_INTRO=1`: shorter boot logos (below). |

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

- **Ports 12 and 14** (`b838932`): the taskset that owned these ports disconnects them, but an SPU then still `send_event`s on them. lv2 consumes the outbound-mailbox data word and answers `CELL_ENOTCONN` in the inbound mailbox; we did neither (RPCS3 `SPUThread.cpp` does both). The leftover data word became the data of the next event, and the SPU read a stale inbound value as its result. Fixed.
- **`write_spu_mb` drops**: 8 writes from libsre to thread 0 (a placeholder) when a taskset is recreated. Benign, left alone.

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

## Still open

- **Graphics in the level**: a grey wedge over black, 260 draws a frame, about 13.6 fps. START/CROSS are read but show no visible change, so cutscene vs gameplay is unknown.
- PSO failure for `fp=0x014AD301`.
- The VPFB/SCREENTEX hacks.
- Logo fades and loading-screen seams.
- Audio, FMV decode, controller support.
