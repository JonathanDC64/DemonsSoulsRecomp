# 116. NPC talk skipped in the Nexus

Date: 2026-10-28. Status: fixed in ps3recomp #149 (pinned by this port's pull request). Some starts had no sound at
all. The game's sound streams then never play out, so after a trip back to the Nexus no new stream can start, and a
talk's lines end at once. The silence comes from a race in the game's own mixer library at start-up.

## The report

Talking to people in the Nexus, their lines go by at once instead of waiting. It happens after travelling from
Boletarian Palace to the Nexus, for example with a Nexial Binding (Use from the inventory, then YES).

The player later noticed that such a session has no sound at any point, from the title screen on. About one start
in two was like that, with nothing changed between starts.

In the player's log (des.log) for that visit:
- The Nexus had no music and no voices.
- The Nexus music (`sound/ds_bgm_m01.mowb`) was opened once at the load and never again.
- No stream reached `cellAtrac` after the trip.

## The cause

The game links Sony's mixer library (libmixer) into its own code. Its surround mixer has a thread,
`_cellsurMixerMain` (priority 400), and a start call, `cellSurMixerStart` (0x015462E0):
- The thread (0x01548190) stores its queue key, `0x8000CAFE02460300`, in the mixer's state at +0x500. It then
  creates an event queue under that key (taking the next key only when a queue already holds it), drains it, and
  waits on it for an event per audio block (`sys_event_queue_receive`, a 9 s timeout, again and again).
- `cellSurMixerStart` starts the audio port, then calls `cellAudioSetNotifyEventQueue(*(state + 0x500))`, so that
  cellAudio sends that queue an event per block.

On a PS3 the new thread runs before its creator reaches Start, so the key is set. In the port both run at once.
When Start wins, it passes key 0:

```
good start                                              bad start
[evt] queue_create -> id=16 key=0x8000CAFE02460300      [cellAudio] SetNotifyEventQueue(key=0x0)
[cellAudio] PortStart(port=1)                           [evt] queue_create -> id=16 key=0x8000CAFE02460300
[cellAudio] SetNotifyEventQueue(key=0x8000CAFE02460300)
```

(In the player's log these lines are interleaved with other threads' text, letter by letter.)

No queue has key 0, so cellAudio notifies nobody. The mixer never mixes, and the session is silent. The game's
sound engine (MagicOrchestra) goes on, but its streams never play out:
- The title music and the first Nexus music are still set up (their `cellAtrac` SetData lines are in the log).
- They never end, so after the trip no new stream starts: no music, and no voice lines.
- A plain talk line whose voice cannot start ends at once (next section).

### How it was found

The player started the game with the debug console (`PS3_DEBUG=<file>`) and left it open in the bad state. Read
through the console, with nothing changed:
- `waits 3`: every game thread was running, the streaming thread too. The mixer's threads made no call at all.
- `ctx 17`: `_cellsurMixerMain` was in `sys_event_queue_receive` on its queue (r3 = 16, r11 = 0x82), with the
  return address of its start-up still in lr: it had never had an event.
- `mem 58300000`: the audio ports' read positions moved, so cellAudio's mixing thread was running. Its events
  went nowhere.
- The log then showed the key-0 registration above.

## What makes a talk end at once

A test-only change in the runtime's file open (`cellFsOpen`) made every open of `ds_bgm_m01.mowb` fail from the
Nexus arrival on. With it, after the travel:
- The first meeting with the Crestfallen Warrior, a scene with timed lines, played normally.
- A second talk with him showed no line at all. Each Cross ended it at once, and the Talk prompt came back.

Without the fault, the second talk's lines each waited for a press. So a talk's plain lines end at once when their
voice stream cannot start.

## The fix

ps3recomp #149, in `cellAudioSetNotifyEventQueue`: with key 0 while `_cellsurMixerMain` is alive, it registers that
thread's key, `0x8000CAFE02460300`. cellAudio looks the queue up by key at every block, so the key can go in before
the queue exists. A start that needed it logs:

```
[cellAudio] SetNotifyEventQueue: key 0 before _cellsurMixerMain stored its own; using 0x8000CAFE02460300
```

RPCS3 has the same race, and its cellAudio corrects key 0 the same way.

## Checked

A scratch change (not committed) held `_cellsurMixerMain` back 300 ms at its start, so Start always won. The runs:
Demon's Souls, muted, from a copy of the player's save.

| Run | Without the fix | With it |
|---|---|---|
| Direct Nexus load | key 0; the mix silent all session (`AUDIO_PEAK`: every reading 0.0000) | the mixer's queue receives every block; the mix has sound |
| Nexus, talk with the Crestfallen Warrior | 4 voice streams set up; the lines wait | 10 voice streams; the lines wait |
| Archstone, palace, Nexial Binding, Nexus, the same talk | after the trip no stream starts; the talk shows no line, the Talk prompt comes back | after the trip the Nexus music and 10 voice streams start; each line waits for a press |

## Why the test runs missed it

The runs in the next section never hit the race (all were muted, from scratch saves). The player's starts hit it
about one in two. Holding the mixer's thread back is what makes it show every time.

## Ruled out before the cause was known

These test runs did not reproduce the skip. The talks were the Crestfallen Warrior's.

| Run | Result |
|---|---|
| Direct Nexus load, then talk (unlocked, knobs on*) | Normal |
| Palace, Nexial Binding, Nexus, talk (unlocked, knobs on*) | Normal |
| The same with the movie player on (`PS3_SAIL_MOVIES=1`) | Normal; the Nexus music and voices start |
| The same at 30 fps with every knob off, pad only (the first talk) | Normal |
| The same at unlocked with the knobs on* and `DES_RESPEC`, both talks | Normal |
| Cross held for 45 pad polls as the talk starts | Normal |
| Presses during a voiced line | No skip; a voiced line does not take a press |
| `DES_SOULS_AT_ONCE` used in the palace, then travel and talk | Normal; the menus' slots reset on the area load |

\* the mouse in the game's menus, mouse look, `DES_SOULS_AT_ONCE`, `DES_CUTFPS`.

The keyboard and mouse, suspected at first, play no part: the race is decided at start-up, before the title
screen.

## The stream system (read)

- The sound manager is `*(0x019081C0)`. Its update 0x00D5C7C8 runs on `MOStreamingThread` every few ms. It takes a
  lock at +0x3D0, then runs each stream in the list at +0x3F8 (begin +0x3FC, end +0x400) through 0x00D7A6A0, and
  drops a stream whose update returns 0.
- A stream starts as a file job on a game worker thread (0x00C3A878, then 0x00A11B00, then the open).
- The loader: 0x00DD4238 (the next load), 0x00DD5378 (the read's start), 0x00DD4FB0 (the memory's release).
- Byte +0xB62 is written from the result of `cellSysmoduleLoadModule(0x13)`. In a working run the word +0xB60
  read 0x00010100 throughout.
- Each stream's update decodes ATRAC3plus through `cellAtrac`.

## The error report

0x00A9DD58(file, line, message) is the game's error report: on a PS3 it prints (debug builds) and then crashes with
a write to address 0. In the port that write lands, so the game goes on. `des_game_error` (port/des_options.cpp, an
instruction hook in port/insn_hooks.txt) logs each report:

```
[des] game error #1: <file>:<line>: <message> (called from <address>)
```

It logs the first 32 reports, then every 100th. No run here logged one: the silent mixer is not an error the game
reports.
