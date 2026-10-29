# 117. The world stops drawing: the viewport command overran the render ring

Date: 2026-10-29. Status: fixed in this port (`des_viewport_room`, an instruction hook). The cause is a bug in the
game's own code. One writer to HighGraphics' command ring does not wait for room: it sleeps 30 µs once and writes.
On a PS3 that was always enough. On a busy PC, the SPU program that reads the ring can be held up for
milliseconds. The write then overran it, HighGraphics wrote over commands not yet read, and the SPU program
halted.

## What it looks like

The 3D world stops drawing. The screen goes flat gray with a diagonal edge, while the HUD still draws and the frame
counter in the window's title goes on. Only a restart brings the world back.

`run.txt` has the SPU side's halt: `HALT-ASSERT`, or `REJECTED malformed` for a DMA with a garbage address, a while
after the overrun.

It was seen in soak runs at `DES_FPS=120` on a PC that was in use: 2 of 23 runs on 10-29. Uncapped runs and runs on
an idle PC did not show it. The frame rate itself is not the cause. What it needs is the SPU side stopping for a
while with the ring full, and a high frame rate fills the ring sooner.

## The ring

HighGraphics (the render thread) writes its commands to a 1 MB ring at 0x45A4B280. SPU image 2 reads them and
turns them into RSX work. The ring's control block is at 0x01B51000:

| Offset | Field |
|---|---|
| +0x00 | get: how far image 2 has read. Image 2 writes it (a 16-byte DMA put of its state) |
| +0x10 | the ring's start |
| +0x14 | the wrap slot, the ring's last 16 bytes |
| +0x80 | put: how far HighGraphics has written |

A command is 16 bytes, or 16 bytes plus a payload. The viewport command (9) is 32 bytes. put == get is an empty
ring; get == put + 16 is a full one. A command that would not fit before the wrap slot goes at the start, after a
wrap command (1) at put.

Each writer waits until its command fits, polling `usleep(30)`:
- the 16-byte commands (0x00CF6F88, 0x00CF73F8, 0x00CFA090, 0x00CFAA98): while get == put + 16;
- the commands with a payload (0x00CF8790 command 32, 0x00CF8CB8 command 34, 0x00CFEC70 commands 19 and 21):
  while the command would reach get;
- the wrap, in every writer: while put < get, or while get is the start.

A get that lags only makes a writer wait longer. These checks never let put reach get from behind.

## The cause

The viewport command's writer, 0x00D22670, reads get once. With get 16 or 32 bytes past put, it sleeps once and
writes:

```
0x00D226E8  lwz   r9, 0(r28)          get
0x00D226EC  addi  r26, r31, 0x10
0x00D226F0  cmpw  cr7, r26, r9
0x00D226F4  beq   cr7, 0x00D2299C     put + 16 == get: the ring is full
0x00D226F8  addi  r0, r31, 0x20
0x00D226FC  cmpw  cr7, r0, r9
0x00D22700  beq   cr7, 0x00D2299C     put + 32 == get: only 16 bytes free
0x00D22704  ...                       the write: word 0 = 9, the rectangle as halfwords at +0x10, the depth range
                                      as floats at +0x18
...
0x00D2299C  li    r3, 0x1E
0x00D229A0  bl    0x00B5D8D8          usleep(30) (syscall 141)
0x00D229A4  nop
0x00D229A8  b     0x00D22704          the write; get is not read again
```

These are the EBOOT's own instructions, read from `EBOOT.ELF`, not the lift's. The wrap path (0x00D22958) reaches
0x00D2299C the same way, with put at the start. The same function's next command (0x10, 16 bytes) waits in a loop,
as every other writer does (0x00D227C0).

On a PS3, image 2 runs alone on an SPU and reads 32 bytes of the ring in far less than 30 µs. In the port it is a
host thread, and on a busy PC the system can hold it for milliseconds. If image 2 has not moved get when the sleep
ends, the write does one of two things:
- with get == put + 32, put lands on get. Image 2 sees an empty ring, though it holds a whole lap of unread
  commands.
- with get == put + 16, put lands 16 bytes past get. Image 2 reads the command's second half (the rectangle and the
  depth range) as a command.

Either way, put has reached or passed get, so every writer's check passes, and HighGraphics writes the next lap
over commands image 2 has not read. Image 2 then reads stale or half-written commands and halts on one it cannot
run.

## How it was found

1. Soaks of a performance change at 120 fps on a busy PC halted image 2 in 2 of 23 runs. Its last steps were
   reading nonsense: in one run a vertex program descriptor of all zeros, in the other a float word taken as a
   command.
2. Checks in a test build of the runtime (not committed) found image 2's own steps right. A test knob that held
   image 2 back (20 ms every 524288 ring reads) made full rings common. The fault below then came back in 5 of 15
   runs, 3 of them with the halt.
3. A check on every store to put, comparing it with the published get across laps, found HighGraphics overrunning
   image 2. One store moved put 32 bytes, onto the published get or 16 bytes past it, while image 2 was held up
   with the ring full. HighGraphics then wrote a lap over unread commands.
4. Recording each store to put with the guest return address named the writer each time: lr 0x00D229A4, a 32-byte
   command 9. One example's bytes: `00000009 00000001 00000000 00000000 00000000 050002D0 00000000 3F800000`, a
   1280 x 720 viewport with depth 0 to 1. In another it was a 160 x 90 viewport.

## The fix

`des_viewport_room` in port/des_options.cpp is an instruction hook after the sleep (port/insn_hooks.txt,
0x00D229A4). There r28 is the control block and r31 is put. While get is 16 or 32 bytes past put, the hook sleeps
again in the same `usleep(30)`, the way the other writers wait, so the write always fits.

A wait that was needed is one that would have overrun the ring. Each one goes to the log, the first 8 and then
every 100th:

```
[des] viewport command #1 waited 3.42 ms for room in the render ring
```

`DES_RINGFIX=0` turns the hook off, live. It works from the ring's own get and put, not from timing, and costs
nothing when the ring has room: the hook runs only on the full-ring path.

It changes the lifted code, so it needs a re-lift of the PPU code. Setup does that on its own when it updates,
because port/insn_hooks.txt is one of its inputs. In a source build, the re-lift (README step 3) changes only
`ppu_recomp_029.cpp`.

## Checked

Demon's Souls, muted, from the soak's save deep in Boletarian Palace, on a PC in use. The runtime was the day's
test build: ps3recomp master plus three image 2 speed-ups that are not merged yet, and the ring checks above,
which are not committed. A soak (`tools/des/soak.sh`) is a 6-minute run at `DES_FPS=120`, walking left and right,
here with the test knob holding image 2 back 20 ms every 524288 ring reads.

| Soaks | Without the fix (three check builds) | With the fix |
|---|---|---|
| Runs | 15 | 8 |
| An overrun (put stored onto or past get) | 5 | 0 |
| Image 2 halted, the world gray | 3 | 0 |
| Runs where image 2 took overwritten commands | 4 (the fifth run was stopped at its halt, before its counts) | 0 |
| The hook waited | (no hook) | twice: 7.16 ms and 0.16 ms |

The ring was full 22 times in the 8 runs with the fix. Most full rings found the viewport command elsewhere, or
freed room within the game's own 30 µs. The two waits are the ones that would have been overruns.

The regression route (`tools/des/regress.sh`: 30 fps, from the pinned save, four screenshots on a fixed walk) on
the same build: all four shots match the accepted set.

A scan of the lift's 1511 calls to `usleep` (0x00B5D8D8) finds no other call after which the code goes on without
testing again: each other call sits in a loop that reads memory and branches back to the sleep.
