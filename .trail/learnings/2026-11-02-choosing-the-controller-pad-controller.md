---
title: Choosing the controller (PAD_CONTROLLER)
author: maintainer
date: 2026-11-02
branch: feat/controller-row
---

## Decided
- Demon's Souls reads only port 0. PAD_CONTROLLER (ps3recomp #218, cellPad.c pad_apply_controller) swaps the chosen pad with port 0 after each poll; "auto" keeps the old order (XInput users on their own ports, HID PlayStation pads in the empty ones). A missing chosen pad leaves the ports as found, so another pad still plays.
- Names: controller1-4 = XInput user (SDL slot off Windows), playstation1-4 = HID slot (the slot pad_hid_sony.c logs when a pad connects). The rules live in libs/input/pad_controller.h with a no-OS test, like pad_sdl_slots.h.
- Vibration follows the device, not the port: s_poll_dev packs the XInput user / SDL slot (PAD_DEV), and rumble_send uses it.
- The Settings row (Controls > Controller) shows the pads connected now via cellPad_host_controllers.
## Tried and ruled out
- Testing with real pads in automated runs: PS3_NO_USER_INPUT (all test runs) skips the backends, so the swap was checked with a scratch hook that put fake devices on the ports, with PAD_RUMBLE=0 so nothing reached a real pad.
## Watch out
- The menus (des_ui read_pads) still take every pad; only the game's port 0 follows the choice.
- PAD_CONTROLLER's default in settings.cpp is "auto", not "": apply_env skips empty values for most keys, so "" would leave an old choice in the environment.
