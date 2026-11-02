---
title: "Physics cap at 30 fps: decide by a frame mean, not each frame"
author: maintainer
date: 2026-11-02
branch: master
touches:
  - port/des_options.cpp
  - docs/71-physics-step-rate.md
---

## Decided
- des_phys_rate's cap goes on/off by the mean of the last 8 frame steps (on under 0.85/n, off over 0.95/n), not by each frame's dt. With DES_FPS=30 the step is real frame time and 44% of frames are a hair under 1/30; the per-frame test skipped those steps, dropped their time at the next long frame, and toggled DES_RAGINTERP/DES_PARTINTERP every frame (port #300, docs/71 "Frames at 30 fps").
- Under the cap a frame of 1/n or longer steps its own dt and keeps the leftover; des_phys_kf acts only under the cap.

## Tried and ruled out
- Arrow position sampling (arpi*.py) to prove it: too few arrows found, and its per-update metric drops the player's ~10 cm moves at 30 fps. The Havok world's time against real time (wt2.py) is the clean check: 0.66-0.68 before, 1.00 after.

## Watch out
- Havok moves its world time back by about 30 s now and then; a two-point rate reads about -2.75. Sum positive increments.
- Any rule on frame steps must hold for jittery real frame times near 1/n, not just the fixed 1/30 of the game's own mode.
