---
title: Naming connected pads in the Controller row
author: maintainer
date: 2026-11-02
branch: feat/controller-row
---

## Decided
- The Controller row lists Automatic, the pads connected now by what they are, and a saved choice whose pad is gone ("PlayStation 2 (not connected)"). A fixed "Controller 1-4 / PlayStation 1-4" list confused the user: they couldn't tell which number was their pad.
- Labels come from ps3recomp's cellPad_host_controller(i, id, name): HID pads from pad_hid_sony_describe (product ID + USB/Bluetooth by input report length), XInput pads from XInputGetCapabilitiesEx (xinput1_4.dll ordinal 108, as SDL does) - vendor 045E is an Xbox controller, other vendors go by subtype - and SDL_GameControllerName off Windows.
## Tried and ruled out
- Real product names for XInput pads: XInput has none, and tying an XInput user to its HID device (or using Windows.Gaming.Input) is far more code than the row is worth.
## Watch out
- The row's value is drawn every frame; pad_choices caches the list for 500 ms because naming an XInput pad is a driver call.
- Changing PadHidSonyState's size breaks the port's copy (des_ui.cpp static_asserts 10 bytes); describe a pad through a function, not a new field.
