# Vibration (2026-10-23)

Gamepad vibration now works for every pad, and Settings > Controls has a Vibration switch, on by default. This doc covers:
- what the game asks for;
- what the runtime did with it before;
- what it does now, and how it was tested.

Tags: **(measured)** run on this PC; **(read)** from the code or the lift; **(inferred)** not run.

## 1. What the game asks for (read)

The PS3's `cellPadSetActDirect(port, param)` sets a pad's two motors:
- `motor[0]` is the small one, which is on or off;
- `motor[1]` is the large one, 0 to 255.

Demon's Souls imports it (`sys_io` NID 0xF65544EE, stub at 0x016B8C24) and calls it from two places:
- `func_00B72358(obj, small, large)` sets both motors. Its one direct caller, `func_001C2D98`, takes the small motor from a single bit, so it is 0 or 1.
- `func_00B72920(obj)` sets both to 0.

The patterns themselves come from `other/default.rumblebnd`, which the game loads at boot.

## 2. Before

`cellPadSetActDirect` sent the values straight to `XInputSetState`, with each byte times 257. This had several problems:
- **The small motor was never felt.** The game's 1 became 257 of 65,535, which is 0.4% speed.
- **Nothing reached a DualSense or DualShock 4.** On Windows those are HID pads here (`pad_hid_sony.c`, docs/28), and the XInput slot it wrote to was empty.
- **Nothing ever stopped a motor** that a game left running.
- **Off Windows,** SDL got 100 ms rumbles, so a pattern the game set once stopped after 100 ms.

## 3. Now (ps3recomp `libs/input`)

- **The mapping follows RPCS3's** (`cfg_pad::get_motor_speed`), reimplemented:
  - The small motor runs at full speed while it is on.
  - The large motor gets a DualShock 3's threshold: 63 and under is still, because a DualShock 3's large motor does not turn below that and games are tuned on one. The values 64 to 255 are spread over 0 to 255.
- **A vibration thread** sends the motors, so a slow write (a Sony pad's Bluetooth report) never holds a game thread. It sends:
  - on a change, at most every 20 ms;
  - again every 300 ms while the motors run (an SDL rumble has a length, and a pad that lost a report gets the next one);
  - off when the game has not set them for 3 s (RPCS3's rule, for a game that stopped calling), when cellPad ends, and at exit.
- **Which pad gets port i:**
  - XInput user i;
  - else the Sony HID pad that the poll put on that port;
  - with SDL (Linux), controller i, as 500 ms rumbles refreshed every 300 ms.

  The poll records what it found on each port and publishes it once the sweep is done, so the thread never sees a port empty partway through a sweep.
- **Sony pads** (`pad_hid_sony_rumble`): the pad's output report, written on a second handle opened for overlapped writes. The reader's handle is synchronous and stays in `ReadFile`, so a write on it would wait for the next input report. A write that does not finish in 200 ms is cancelled.

  The reports set the motors and nothing else, so the light bar and the player lights stay as they are. Their layouts are as Linux hid-playstation and SDL lay them out; offsets are from the report id:

  | Pad | Report | Bytes |
  |---|---|---|
  | DualShock 4 USB | 0x05, 32 bytes | [1] 0x01 (motors), [4] small, [5] large |
  | DualShock 4 Bluetooth | 0x11, 78 bytes | [1] 0xC4 (HID, CRC, 4 ms), [3] 0x01, [6] small, [7] large |
  | DualSense USB | 0x02, 48 bytes | [1] 0x03 (rumble the old way, haptics off), [3] small, [4] large, [39] 0x04 (the newer rumble emulation) |
  | DualSense Bluetooth | 0x31, 78 bytes | [1] sequence << 4, [2] 0x10, then as USB two bytes on |

  - Over Bluetooth, a CRC-32 of 0xA2 and bytes 0 to 73 goes at 74 to 77, little-endian.
  - The buffer is the length the device reports (`OutputReportByteLength`), with zeros after the report, as hidapi writes it.
  - Over Bluetooth, the first report also switches the pad to its full input reports, which the reader already reads.
  - The connect line in the log says ", no vibration" when the pad takes no output report.
- **Knobs:**
  - `PAD_RUMBLE=0` (live) keeps every pad still.
  - `PAD_RUMBLE_TRACE=1` logs the game's first set and each change, with what is sent.
  - `PS3_NO_USER_INPUT=1` (test runs) opens no pad of the user's, so a test never vibrates one.

## 4. The setting (port)

Settings > Controls > The game > **Vibration** (`PAD_RUMBLE`), on by default, live: "Lets the game make your gamepad vibrate."

## 5. Tested

- **`libs/input/tests/test_pad_rumble.c` (measured).** It runs against an XInput stub with one pad on port 0, and checks:
  - the mapping: small 1 gives full speed; large 63 gives still, 255 full and 159 half;
  - the resend every 300 ms;
  - `PAD_RUMBLE=0` and back, live;
  - the stop after 3 s and at `cellPadEnd`;
  - the four Sony reports, byte by byte, with their CRC (the CRC-32 check value too).

  `test_pad_keys.c` still passes, with the stubs the memory module and the new call need.
- **Linux:** `cellPad.c` and `pad_hid_sony.c` compile with gcc and with the Linux build's own flags.
- **A scripted run** with `PAD_RUMBLE_TRACE=1`: the game made no call while the player swung at the air (no hit). So the runs here cannot show the game's own patterns; a hit, a block or a boss does that.
- **Not tried with a live pad:** the test runs never open one. The player's pad is the check: an Xbox pad, a DualSense or a DualShock 4, over USB and over Bluetooth.
