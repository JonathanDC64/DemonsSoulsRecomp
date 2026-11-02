---
title: "Sound gone after a while: the game did not follow Windows' default output device"
author: maintainer
date: 2026-11-02
branch: pin/audio-follow
---

## Decided
- Issue #296 (sound gone for the rest of the session after the player was away 1-2 minutes): cellAudio's WASAPI backend opened Windows' default device once and never followed a change. The old device keeps working, so no error and no log line. It also never reopened after AUDCLNT_E_DEVICE_INVALIDATED.
- Fix (ps3recomp #220, pinned here): an IMMNotificationClient flags a default-device change; the mixing thread reopens on the new default (not if the id is the same). A failed stream call closes it and the mixer retries the default device about once a second. The log names the device kind ("[cellAudio] output: the default device, headphones"), never its name.

## Tried and ruled out
- The Nexus BGM stream ending at its loop point (252 s): a 305 s hidden Nexus run kept 0% silent blocks past it (AUDIO_RATE=1).
- A focus effect: nothing in the port mutes or sends sysutil events when the window loses focus.
- The report's 120 fps: that is the F1 menu's overlay repaints, not game frames.

## Watch out
- Testing a real default-device switch changes the PC owner's sound: ask, or leave it to the user. A scratch hook that sets the change flag, clears the stored id, or fails the open covers the code paths.
- For "no sound" reports, read the [cellAudio] output / "default output device changed" / "output device stopped" lines first.
