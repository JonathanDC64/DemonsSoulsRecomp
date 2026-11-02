---
title: "Window closes with no crash report: a Lua error and the lifted longjmp"
author: maintainer
date: 2026-11-02
branch: master
touches:
  - ps3recomp.pin
  - tools/setup/des_setup/game_build.json
  - tools/setup/des_setup/game_build_linux.json
  - docs/31-ac4-acfa-ports.md
---

## Decided
- A tester's game closed with no crash report or WER entry in the Penetrator fight (1-3). The boss script `502000_battle` errors at line 86 ("attempt to compare nil with number"). That's the game's own bug, and a PS3 catches it. Lua 5.0 throws with longjmp (luaD_throw 0x00CAE730 -> luaD_rawrunprotected 0x00CAE690). The lifted SDK longjmp (0x01674A5C) restored the guest registers and then returned to its own C caller. So luaV_execute ran on with a NULL savedpc, and the game's abort path called sys_process_exit(1).
- Fix (ps3recomp #212): the lifter finds the SDK's setjmp/longjmp by their first five instructions.
  - Each guest setjmp call becomes PS3_GUEST_SETJMP: a host CRT setjmp in the caller's frame, on a per-thread slot list (g_ps3_sjlj_top).
  - The guest longjmp ends in ps3_guest_longjmp, which matches the slot by buffer and restored r1 and host-longjmps to it.
  - The thread proc clears the list at each guest thread entry (ps3_sjlj_thread_reset).
  - Split modules get _setjmp/longjmp as CRT thunks: PS3_GAME_ABI 3 -> 4, and both game_build*.json say "abi": 4.
- How it was found: a debug line in the lifted luaD_throw (log the Lua error string from the stack top) printed the error right before the NULL reads. Live check: with the fix, the same error was thrown twice and the fight went on.

## Tried and ruled out
- `__builtin_setjmp`/`__builtin_longjmp` under clang-cl on Windows x64: it fails fast (0xC0000409 at /Od, an access violation at /O2). Use the C library's setjmp/longjmp, which unwind through lifted frames with their unwind tables.
- Swapping only "changed" chunks into an old work/recomp: a lifter change moves functions between chunks (the two new lines per function count toward --chunk-lines), and the preamble differs with the lifter version. Build from a full new lift.

## Watch out
- "The window closed, no crash report" can be the game ending itself (sys_process_exit), not a host crash. Look for `[ppu] longjmp ... no live setjmp` and for Lua errors before the exit.
- A new scratch build dir of the port needs `-DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS_DEBUG= -DCMAKE_C_FLAGS_DEBUG=`. Otherwise it is a Debug build, and the link fails on _ITERATOR_DEBUG_LEVEL against the Release runtime.
- A ps3recomp worktree's work_spu junction must point at an SPU lift made by the same scripts/build_spu.py. A mismatch fails the link on spu_hook_* symbols.
