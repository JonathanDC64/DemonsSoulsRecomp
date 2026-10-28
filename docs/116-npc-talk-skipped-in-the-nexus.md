# 116. NPC talk skipped in the Nexus

Date: 2026-10-28. Status: not fixed. The skip follows from the sound streams not starting. Why they stop in the
player's session is still open. A log line now catches the game's own error reports.

## The report

Talking to people in the Nexus, their lines go by at once instead of waiting. It happens after travelling from
Boletarian Palace to the Nexus, for example with a Nexial Binding (Use from the inventory, then YES).

In the player's log (des.log) for that visit:
- The Nexus had no music and no voices.
- The Nexus music (`sound/ds_bgm_m01.mowb`) was opened once at the load and never again.
- `cellAtrac` set up one decoder all session, for the title music. No stream reached the decoder after that.

## What makes a talk end at once

A test-only change in the runtime's file open (`cellFsOpen`) made every open of `ds_bgm_m01.mowb` fail from the
Nexus arrival on. With it, after the travel, at 30 fps with every knob off:
- The first meeting with the Crestfallen Warrior, a scene with timed lines, played normally.
- A second talk with him showed no line at all. Each Cross ended it at once, and the Talk prompt came back.
- The game made no error report (below): a failed open does not go through it.

The same two talks without the fault (at unlocked, with the knobs on): the scene played, and the second talk's
lines each waited for a press.

So a talk's plain lines end at once when their voice stream cannot start. That matches the player's log, with no
music and no voices. Why the streams stop in the player's session is still open.

## Ruled out

These test runs did not reproduce the skip. The talks were the Crestfallen Warrior's.

| Run | Result |
|---|---|
| Direct Nexus load, then talk (unlocked, knobs on*) | Normal |
| Palace, Nexial Binding, Nexus, talk (unlocked, knobs on*) | Normal |
| The same with the movie player on (`PS3_SAIL_MOVIES=1`; the player's log shows it) | Normal; the Nexus music and voices start |
| The same at 30 fps with every knob off, pad only (the first talk) | Normal |
| The same at unlocked with the knobs on* and `DES_RESPEC`, both talks | Normal |
| Cross held for 45 pad polls as the talk starts | Normal |
| Presses during a voiced line | No skip; a voiced line does not take a press |
| `DES_SOULS_AT_ONCE` used in the palace, then travel and talk | Normal; the menus' slots reset on the area load |

\* the mouse in the game's menus, mouse look, `DES_SOULS_AT_ONCE`, `DES_CUTFPS`.

Also ruled out by reading the code or the logs:
- `DES_AUTO_CONTINUE`: it turns itself off at the first level load.
- The keyboard menu layer: the port never sets it.
- `AUDIO_MUTE`: it only silences the device; the game's sound path is the same.
- The mouse's flick: it moves only the right stick.
- `cellAtrac` running out of decoders: the runtime keeps 16 slots, and the player's log set up only one.
- Threads: the same threads start around the travel in the player's log and in the test runs.
- `cellSysmoduleLoadModule(0x13)`, whose result the stream manager keeps at +0xB62: the runtime always answers OK.

## The stream system (read)

- The sound manager is `*(0x019081C0)`. Its active streams are a list at +0x3F8.
- A stream starts as a file job on a game worker thread (0x00C3A878, then 0x00A11B00, then the open).
- The loader: 0x00DD4238 (the next load), 0x00DD5378 (the read's start), 0x00DD4FB0 (the memory's release).
- The streaming thread runs the stream manager's update 0x00D5C7C8 every 5 ms (its loop is near 0x00D64EC4). The
  update first checks byte +0xB62, which is written from the result of `cellSysmoduleLoadModule(0x13)` (stub
  0x016B85A4) and reset through `cellSysmoduleUnloadModule` (0x016B8584). In a working run its word (+0xB60) read
  0x00010100 throughout.
- Each stream's update is 0x00D7A6A0; it decodes ATRAC3plus through `cellAtrac`.
- The loader's three error paths call the game's error report 0x00A9DD58.

## The error report

0x00A9DD58(file, line, message) prints through the game's debug print and then stops the game. A retail build
drops the print. On a PS3 the stop is a write of 0xDEADBABE to address 0, which crashes. In the port that write
lands, so the game goes on with whatever failed and nothing shows.

`des_game_error` (port/des_options.cpp) is an instruction hook on the report's first instruction
(port/insn_hooks.txt). It always logs the report:

```
[des] game error #1: <file>:<line>: <message> (called from <address>)
```

It logs the first 32 reports, then every 100th. None of the runs above logged one.

**Needs a re-lift** (README step 3): the hook is new.

## Next step

Reproduce with this build and look in des.log for `[des] game error`.
- A report from the sound code names the failing step.
- With none, the next suspect is a stream job that never ends and keeps the loader busy. Finding it needs a log of
  the loader's state at the Nexus arrival.
