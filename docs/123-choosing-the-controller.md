# Choosing the controller (2026-11-02)

With more than one gamepad connected, Settings > Controls > **Controller** picks the one the game reads. Automatic, the default, keeps the rule from before.

## 1. Why

Demon's Souls reads only port 0 (read). Before this, port 0 was simply the first pad the runtime found:
- XInput users 1 to 4 sit on ports 0 to 3, by the number Windows gives them;
- PlayStation pads read over HID (`pad_hid_sony.c`) take the ports XInput left empty, in the order they connected.

So with an Xbox pad and a DualSense both connected, the Xbox pad always played, and there was no way to pick the other one short of unplugging it.

## 2. The knob (ps3recomp `libs/input/cellPad.c`)

`PAD_CONTROLLER`, live:

| Value | Port 0 is |
|---|---|
| `auto` or empty | the first pad found, as before |
| `controller1` .. `controller4` | that XInput user (off Windows, that SDL controller) |
| `playstation1` .. `playstation4` | the PlayStation pad in that HID slot (Windows); the log names each pad's slot when it connects |

- **How:** after each poll, the chosen pad and port 0's swap places (`pad_apply_controller`). Vibration follows the pad: each port's device now carries its XInput user or SDL slot, so the motors of the pad on port 0 run, not those of XInput user 1.
- **Not connected:** while the chosen pad is missing, the ports stay as found, so another pad still plays.
- **The log** says when the chosen pad becomes port 0 and when it goes:
  ```
  [cellPad] PAD_CONTROLLER: playstation1 is port 0 (found on port 1)
  [cellPad] PAD_CONTROLLER: controller2 isn't connected; the ports stay as found
  ```
- **`cellPad_host_controller(i, ...)`** gives the i-th connected pad's name above and what it is, for the settings row:
  - a PlayStation pad over HID: "DualSense", "DualSense Edge" or "DualShock 4", with "(USB)", "(Bluetooth)" or "(wireless adapter)" (`pad_hid_sony_describe`);
  - an XInput pad: XInput has no product names, so Microsoft's pads (vendor 045E, from `XInputGetCapabilitiesEx`) are "Xbox controller" or "Xbox 360 controller", and the rest go by their subtype ("Wheel", "Arcade stick", ..., else "XInput controller"), with "(wireless)" when XInput says so (`pad_controller_xinput_label`);
  - off Windows, SDL's name for the controller.
- **The menus** still take every pad.

## 3. The row (port)

Settings > Controls > The game > **Controller** lists:
- Automatic;
- each pad connected now, by what it is ("DualSense (USB)", "Xbox controller (wireless)"); a second pad of the same kind gets a number ("DualSense (USB) 2");
- the saved choice while its pad isn't connected ("PlayStation 2 (not connected)").

The list is read when the row is drawn, at most twice a second, since naming an XInput pad asks its driver.

## 4. Tested

- **ps3recomp's `test_pad_controller`** passes, with clang on Windows and with gcc `-Wall -Wextra -Werror` on Linux (WSL). It covers the names, the devices staying apart, the port found, the names reading back, and the XInput pads' labels.
- **In the game, with a scratch hook** (not committed). The hook put fake devices on the ports under `PS3_NO_USER_INPUT`: port 0 an XInput pad, port 1 a PlayStation pad. Vibration was off.
  - `PAD_CONTROLLER=playstation1`: "playstation1 is port 0 (found on port 1)". The list read `playstation1,controller1`.
  - `PAD_CONTROLLER=controller2`: "isn't connected", and the ports were unchanged.
- **`regress.sh`**: 4 of 4 shots ok.
- **Not tested here:** real pads, since test runs never read the user's controllers. Neither is the row's look.
