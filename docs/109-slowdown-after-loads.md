# 109. Slowdown after loads: the taskset table and the stop that ran forever (issue #202)

Date: 2026-10-26. Status: fixed in ps3recomp (JonathanDC64/ps3recomp#129); the side note's movie problem is
ps3recomp#130. This port's `ps3recomp.pin` moves to them in a later pull request.

## Short answer

- Issue #202: after some play, entering 2-1 (Stonefang Tunnel) dropped the game to 12-40 fps, the sound fell
  behind, and the CPU sat at 100%. The reporter's PC (i7-8700K, 12 threads) had about ten host threads at 91-93%,
  all `spu_async_thread` in `spu_indirect_branch` / `spu_lookup`.
- Two faults in ps3recomp, one feeding the other:
  1. The runtime tracked at most 32 taskset addresses. Demon's Souls used 17 to 33 of them by 2-1 in six runs
     here, so some sessions go past the limit. A taskset past it is not tracked, so its shutdown did not stop its
     waiting tasks. They stayed parked on memory the game then reused.
  2. When one of those tasks woke on that memory, the zlib inflater (image 1) hit its bad-data `stop 0` at LS
     0x4690. Inside a call, the drain loop resumed that same function again and again: one host thread at 100%
     per task, for the rest of the session.
- The fix: room for 256 addresses, and a stop inside a call now ends the SPU program, as it does on hardware.
- The spin never happened on this PC, before or after the fix. The evidence is the reporter's files and the
  table counts in section 2.
- Not involved: the online setting (the reporter's network was off, because the "project" server preset has no
  address yet), the frame rate and the render scale.
- Until the fix is in a build, restarting the game clears it: the tables live only as long as the process.

## 1. What the reporter's files show

- `runtime.txt`: 90 threads, 11.36 cores busy. The top threads are `spu_async_thread` at 91-93%, in
  `spu_indirect_branch` and `spu_lookup`.
- `des.log` (6 min 54 s from start):
  - `[spu] drain-resume return_pc=0x07AE8 at lifted entry 0x04690 img=1 depth=5` over and over (the print stops
    at 8);
  - then `write_spu_mb: thread 0x0 not found` lines: tasks writing to an SPU thread that no longer exists.
- Just before that, taskset 0x48BBA800 was created, had task 3 in WAIT_SIGNAL, and was shut down and joined, with
  no `stopped in WAIT_SIGNAL: taskset was shut down` line. Every other taskset created once gets 4 or 5 of those
  lines.
- The log's lines are often interleaved, so the count of distinct addresses in it is a floor: 27 from clean
  lines, about 29 in all.

## 2. Runs on this PC

Setup for every run:

- The reporter's save at the 2-1 entrance (`work/tmp/saves/i202.orig`, run only as copies).
- levelcap, with the reporter's settings: VSync off, FXAA, anisotropic 4x, texture filter 3, motion blur and
  depth of field off, online off.
- About two minutes in the level.
- "Old" is the rolltwist test build. "New" is the same build's game module with a host built on the fixed
  runtime.

| Run | Build | fps (median, p10) | Cores busy | Distinct tasksets | Last new one: tasks stopped at shutdown | Spins |
|---|---|---|---|---|---|---|
| 120 fps, 200% | old | 120, 120 | 3.51 | 33 | 0 (the 33rd) | 0 |
| 120 fps, 100% | old | 120, 120 | 3.64 | 32 | 5 | 0 |
| 30 fps, 200% | old | 30, 30 | 1.37 | 27 | 30 | 0 |
| 30 fps, 100% | old | 30, 30 | 1.38 | 17 | 30 | 0 |
| 120 fps, 200% | new | 120, 120 | 3.74 | 23 | 5 | 0 |
| 120 fps, 200%, VSync on | new | 120, 120 | 3.96 | 33 | 5 (the 33rd) | 0 |

How to read the table:

- "Last new one" is the last taskset address the game created for the first time in the run. The 30 fps runs'
  last one was created several times, hence 30.
- In the first old run the 33rd address got no stops: the same thing the reporter's log shows. With the fix, the
  same address gets its 5.
- Neither the frame rate nor the render scale made a spin; on this PC none happened at all.

## 3. The fix (ps3recomp#129, `runtime/spu/`)

- `spu_workload.c`: `TS_TRACK` and `TS_LIFE_MAX` go from 32 to 256.
  - Entries are still never cleared. The scans stop at the first unused entry, so the per-lock-line check in
    `spu_taskset_line_written` costs no more.
  - A full table prints `the taskset ... table is full`, once.
- `spu_drain.c`, `spu_drain_call`: sometimes one resume runs a single function that stops
  (`SPU_STATUS_STOPPED_BY_STOP`) and leaves the pc where the resume put it. The program then ends through
  `spu_halt`, as a task's EXIT does. It prints `[spu] img=N stopped (stop 0xC) in lifted code at 0xPC, D calls
  deep: the program ends there` (8 times at most).
- `tests/test_spu_lifted_start.c`:
  - a stop inside a call ends the run after one retry;
  - a callee that returned to a lifted entry still runs on to the return pc;
  - against the old drain loop, the test spins until its alarm.

## 4. If it comes back

- `table is full`: a session used more than 256 taskset addresses. Make the entries reusable rather than raising
  the number again.
- `stopped (stop ...)`: some SPU code reached a stop that is not a syscall. Once, it is a task that ran into bad
  data; many times from one image, that image needs a look.
- The debug console's `cpu` command names the threads: `spu_async_thread`s at 90% or more with no gain in frame
  rate are this kind of spin.

## 5. The side note: "skip intro also skips cutscenes"

- `DES_SKIP_INTRO` changes only the boot-logo dialog. Its hook replaces method `0x008EF430`, and the EBOOT has
  one reference to that method's OPD (one vtable slot, `0x01870A7C`). It cannot reach a cutscene.
- The reporter's log has it off anyway (`DES_SKIP_INTRO = 0`). It shows something else: both movies the session
  opened (`ds_advertise.pam`, `ds_title.pam`) ended after `stream read: 0 frames decoded`, with no error.
- The cause, reproduced on this PC:
  - Media Foundation lists two MPEG-2 decoders here: the Microsoft Store's MPEG-2 Video Extension first, then
    Windows' own "Microsoft MPEG Video Decoder MFT".
  - Windows' own fails to start with `0xC004F011` (`SL_E_LICENSE_FILE_NOT_INSTALLED`): it needs the extension's
    licence.
  - sail_movie took only the first decoder listed and gave up silently. So on a PC without the extension, every
    movie has no picture and ends after a moment: the opening, the old tale, the endings.
- The level cutscenes run in the engine, not as movies. If the reporter means one of those, it is something else,
  and the reply asks which.
- ps3recomp#130 tries every listed decoder and logs why one can't be used. When Windows' decoder lacks its
  licence, it names the extension. The player's fix is to install the free MPEG-2 Video Extension.
- Not decided: bundling FFmpeg's MPEG-2 decoder on Windows, as the Linux build does, so movies need nothing from
  the Store.
