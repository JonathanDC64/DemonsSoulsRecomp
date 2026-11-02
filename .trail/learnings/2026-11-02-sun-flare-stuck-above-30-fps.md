---
title: "Sun flare stuck above 30 fps: the game's label check never passes"
author: maintainer
date: 2026-11-02
branch: fix/sun-gate
touches:
  - docs/28-in-game.md
  - port/des_options.cpp
  - port/insn_hooks.txt
---

## Decided
- Issue #290 (above 30 fps the sun shines through walls, or is gone with its flare in the open): the game's own check, not our ZPASS counts. Ending the sun probe's query (0x00AB41E8) bumps a 64-bit counter, keeps it at query+0x18 and releases label 0x52 with it after GET_REPORT 1024. The three readers (0x003E2D38, in 0x003E5750, in 0x003E6368) read report 1024 only when label 0x52 >= the counter (low words), else keep their count (obj+0x28).
- At 30 fps the first reader passes every frame. Uncapped (~315 fps) the main thread was 2-3 probes ahead of the RSX at every check: in 130 s it passed about once per camera sweep, and the count froze at what that pass took (0 = sun gone, 576 = through walls). RPCS3 shows the same with its unlock patches.
- Fix: insn hooks after each label load (lwz r0 at 0x003E2DF0 / 0x003E6330 / 0x003E6518, counter in r28 / r29 / r29) raise r0 to at least the counter, so the reader takes the latest stored count. des_sun_gate in port/des_options.cpp, DES_SUNFIX (default 1, only with DES_FPS).

## Tried and ruled out
- 11-01: holding label 0x52's release until the count is stored (the walker parks at put before the flip comes), and waiting in GetReport (72% fresh, up to 5.9 ms). Both treat the lag between the store and the release. The real lag is the main thread running probes ahead of the RSX, which no runtime timing can close.
- The Palace glow (11-01) was the game's exposure, not this bug; a save in Stonefang 2-1 with the sun in view showed it.

## Watch out
- Ghidra stops on 0x003E5750 and 0x003E6368 (bad instruction data): read the lift or ppu_disasm.py --va.
- RSX_ZPASS=0 reports every probe visible, which hides the stale-count modes; test with ZPASS on.
- The probe runs only while the sun is in view: a save without the sun in frame (the pinned regress save) never exercises it.
