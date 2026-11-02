---
title: "Setup's link fails on __CxxFrameHandler3: the freestanding module has no C++ exceptions"
author: maintainer
date: 2026-11-02
branch: fix/game-module-no-eh
---

## Decided
- Setup's link of des_game.dll failed with "undefined symbol: __CxxFrameHandler3" (issue #304). The freestanding module compiles C++ with /EHsc but links /NODEFAULTLIB, so no C runtime and no C++ unwind handler. The first lifted local with a destructor (ps3recomp #212's guest-setjmp scope, ps3_sjlj_scope) pulled the handler into the 7 chunks with a guest setjmp call.
- Fix (port #305): the module's C++ is compiled with exceptions off, /EHs-c- on Windows and -fno-exceptions on Linux (there the names were __gxx_personality_v0 and _Unwind_Resume). Both game_build*.json and DesGameFreestanding.cmake carry it; game_build_desc.py --check keeps them equal.
- Safe because the module never throws or catches, unwind tables stay (a host longjmp still unwinds the frames), and ps3_guest_longjmp sets g_ps3_sjlj_top itself, so skipped frames' destructors are not needed.

## Tried and ruled out
- Our own runs never showed it: the developer single-exe build links the C runtime. Only setup's freestanding link fails, and CI has no lift to build it with.

## Watch out
- Any lifter or module change that adds C++ runtime needs (a destructor under /EHsc, RTTI, operator new, thread-safe statics) breaks only setup's link on a player's PC. Check a chunk compiled with game_build.json's arguments with llvm-nm for names outside game_module/game_libc.
- Players need a new zip for a game_build.json change; setup keeps its lift when the ps3recomp pin is unchanged.
