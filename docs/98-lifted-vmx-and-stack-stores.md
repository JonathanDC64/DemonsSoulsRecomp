# 98. The lifted AltiVec helpers and the stack stores (2026-10-24)

Two changes to how ps3recomp's PPU lifter writes guest code, made for the frame-rate dip after breaking things
(docs/73, docs/77) and measured on the deep Palace too:

1. **Seven AltiVec ops call SSE4.1 helpers** instead of byte or lane loops: `vperm`, `vsldoi`, `vmaxfp`, `vminfp`,
   `vrsqrtefp`, `lvlx` and `lvrx`. **Kept** (ps3recomp #117's first two commits).
2. **Stores with base r1** (frame saves, spills, the back chain) **skip the SPU-reservation test** while their
   address is in the stack region. **Kept, with the main thread's stack moved into that region** (#117's third
   commit; the port now sets `PS3_MAIN_STACK_LV2=1` by default). Without the move it lost (section 2).

The code is in ps3recomp's `tools/ppu_lifter.py` (the helpers in its preamble) and `include/ps3emu/ppu_lift.h`
(the stack-store accessors). Labels: **measured** (a run says so), **read** (seen in the code), **inferred**.

## Short answer

- **The helpers: kept.** On the break replay (docs/73's 10 recorded breaks), the setup's own lift with the new
  lifter against the old lift, on the same host: +9.1% fps in the 2 s after each break (237.1 -> 258.7), +11.5%
  before the breaks (239.0 -> 266.6), and the Havok step frames after the breaks 5.69 -> 5.34 ms. Every run with
  the helpers is above every run without (measured). `vperm` and `vsldoi` give most of it (+9.4% in the four-way
  A/B), the other five +0.9%. The worst frame after a break stays about 21 ms.
- **At the deep Palace nothing moves** (164.6 -> 164.9 fps, the runs overlap). Its frame is held by HighGraphics'
  hand-off to image 2 (docs/83), not by lifted vector code.
- **The stack stores: kept, with the main thread's stack in the stack region.** With `PS3_MAIN_STACK_LV2=1` on
  both sides: +5.8% fps after the breaks (257.1 -> 271.9), +4.8% before them, the Havok step 5.35 -> 5.13 ms,
  every run better; the deep Palace overlaps in two A/Bs (measured). Without the knob they lost 3.4% after the
  breaks: the main thread's stack was outside the region, so all of its frame stores took a longer path.
- The two together, against the old lift: about +15% fps after the breaks (inferred: two A/Bs on different
  sides, section 4).
- **Other titles**: ps3recomp keeps `PS3_MAIN_STACK_LV2` opt-in. A title that leaves it off should measure
  before taking the third commit.
- The PR's lift and build pass regress and the break replay (section 3).

## How it was found

- The dip's critical path is the Havok PPU Thread's own lifted code: the image 8 tasks it hands work to mostly
  wait (docs/77, measured).
- An instruction-class profile of that thread (1,810 samples in the game DLL) and a look at the machine code
  showed two ops that compile badly (measured, read):
  - `vperm` was a byte loop: 16 byte gathers, 16 byte stores, then a 16-byte reload of what was just stored,
    which stalls (a store-forwarding stall). 8,037 sites in the lift.
  - `vsldoi` copied both sources into a 32-byte temporary on the host stack and loaded 16 bytes across the two
    stores: the same stall, every time. 9,836 sites.
  - Together about 8% of the thread's lifted-code samples.
- A survey of every VMX and FP mnemonic in the EBOOT, each lifted alone and compiled with the DLL's flags, found
  three more that the Havok code runs often (measured): `vmaxfp`/`vminfp`, `vrsqrtefp`, `lvlx`/`lvrx`. Everything
  else common is 3 to 12 instructions.
- 67% of the lift's guest stores have base r1: 768,819 sites (counted). Each one tested the store limit, the SPU
  armed flag and the reservation bitmap before storing.

## 1. The helpers

Byte i of a lifted vector register is guest byte i, so the x86 byte lanes are the guest's and the byte ops map
straight onto SSE. The float ops swap each 32-bit lane on the way in and out, as the old loops did
(`ppu_vldf4`).

| op | sites | x86 instructions before | after | how |
|---|---|---|---|---|
| `vperm` | 8,037 | 211 | 11 | two `pshufb`, one blend on bit 4 of each index |
| `vsldoi` | 9,836 | 9 (and the stall) | 5 | one `palignr`; the shift is the instruction's literal |
| `vmaxfp` | 442 | 129 | 24 | `maxps`, then blends for the NaN and signed-zero rules |
| `vminfp` | 478 | 133 | 24 | `minps`, the same |
| `vrsqrtefp` | 1,353 | 76 | 9 | `sqrtps`, `divps` (1/sqrt, rounded once each, as before) |
| `lvlx` | 1,071 | 105 | 18 | one `pshufb`; an index past the end gives a zero |
| `lvrx` | 10 | 88 | 15 | one `pshufb`; an index before the start gives a zero |

(Instructions: each statement in a function of its own, clang-cl `/O2 -msse4.1`, the game DLL's flags, measured.)

- The rules the old loops had are kept lane for lane: a NaN operand gives itself, quieted, vA's first; of equal
  operands `vmaxfp` takes the AND of their bits (+0 over -0) and `vminfp` the OR (-0 over +0).
- The loops stay as the fallback where SSE4.1 is not on (`PPU_VMX_NO_SSE41` forces them). Every port build
  compiles with `-msse4.1` already.
- `tools/test_ppu_vmx_helpers.py` lifts each instruction with the lifter and runs the new statement against the
  old one on 20,000 random and special inputs each: NaNs (quiet and signalling, with payloads), signed zeros,
  infinities, denormals, equal operands, every `vsldoi` shift and `lvlx`/`lvrx` offset, a destination that is also
  a source, rA = 0. 85 statements; it builds with cl (both paths) and clang-cl (the SSE path). They agree bit for
  bit, and seven planted bugs each fail it.

## 2. The stack stores (measured, kept with the knob)

**The result first.** The first A/B lost. On the break replay the stack stores cut the Havok PPU Thread's cycles in
the windows after the breaks by 11.5%, but the frame rate went down 3.4% after the breaks and 3.9% before them, every
run below every run without them, with more windows over 16 ms (8 against 4.5) (measured, section 4).

**Why** (measured, read): every guest thread's r1 at the deep Palace (console `ctx`, three passes) is in the stack
region except the main thread's, 0x0FEFF3E0. The boot puts the main thread's stack below 0x0FF00000 unless
`PS3_MAIN_STACK_LV2=1` gives it lv2's place, the first allocation in the stack region (`boot_main.cpp`; opt-in,
written for Tornado Outbreak). So each of the main thread's frame stores failed the stack test and took the
out-of-line path, one compare and two jumps more than before, while the Havok thread's got cheaper.

**With the knob on both sides the same change wins** (section 4): +5.8% fps after the breaks, +4.8% before them,
the step -4.2%, every run better; the deep Palace overlaps in two A/Bs. So the port sets the knob by default: the
player's start (`run_defaults` in `des_options.cpp`), `play.sh`'s defaults (`deslib.sh`) and the AppImage's
launcher, each only when it is unset. With it, a regress run puts the main thread's stack top at 0xD0112000.

The machine code is what was meant: a stack store is one compare against the limit, then the store (read, in the
DLL). The design:

- **What.** The lifter writes a D-form store with base r1 as `vm_write*_stk(ctx->gpr[1] + off, v)`. It stores an
  address in the stack region (0xD0000000 up, vm.h) straight away while `ea - 0xD0000000` is below the stack
  limit; any other address takes the inline path as before.
- **The limit.** It is the word after the SPU armed flag, on the flag's own cache line, so a game module carries
  it with no new export (`game_module.c` names it). The runtime keeps it (`spu_coh_stk_eval`):
  - it follows the store limit, so it is 0 until the limits are decided and while any store diagnostic is on
    (a store watch, the null sweep, `PT`, `SPU_ATOM_EA`, a console env change);
  - it goes to 0 for good once an SPU reserves a line in the stack region (logged once, `[coh] an SPU reserved
    stack line`); those stores then take the full path.
- **Why that is safe.** The test it skips only matters for a line an SPU has reserved. A store that read the
  limit just before an SPU's first stack reservation can miss that one lost-reservation event, the same window
  every store has at the first GETLLAR of a run (read). `spu_resv_lost_poll` compares the reserved line at every
  poll, so a missed event costs one poll (10 ms), not a hang. The flag and the limit are seq_cst atomics, set
  then checked again, so a reservation racing a limit update cannot leave the limit on.
- **Where.** The rewrite happens as each function is written out, so the frame analyses (callee-save restores,
  spills) still see the plain stores. In Demon's Souls no SPU reserves a stack line (no `[coh]` line in any run
  tonight).
- `tools/test_ppu_stack_stores.py`: the lifter writes the r1 stores, and only those, as `vm_write*_stk`; the
  header stores straight only inside the limit, big-endian, and calls the accessor otherwise, with the limit on
  and off and with `PPU_VM_NO_INLINE`.
- **Other titles.** ps3recomp keeps `PS3_MAIN_STACK_LV2` opt-in. In a title that leaves it off, the main thread's
  stores take the longer path, as in the first A/B here; it should measure before taking the third commit, or
  turn the knob on.

## 3. Checks

- **The real lift.** The setup's own lift steps with the new lifter: every one of the 53 lifted chunks and
  `ppu_recomp.h` has the new forms everywhere the scratch rewrite that the four-way measurements used had them,
  and is otherwise byte for byte the same once the new helper block in the preamble is cut out (compared through
  a lift with both commits, its stack stores turned back). No old loop is left in a function. libsre's lift has
  the new forms too (26 `vperm`, 32 `vsldoi`); the scratch DLLs of the four-way did not convert it, the real
  build of the A/B in section 4 does.
- **Tests.** Every `tools/test_*.py` passes, the conformance suite's 2,665 checks included (it now runs the SSE
  path for `vmaxfp`/`vminfp`). The stack-store test passes in CI on Windows, Linux and macOS (macOS after
  #117's fourth commit, which sets the test's `vm_base` at run time).
- **Regress** on the real build (the pinned save, the four stops): every shot ok (mean differences 3.8, 7.6, 5.7
  and 2.2), no crash line, no HALT (measured). With the stack stores and the knob: every shot ok, stack top
  0xD0112000, no `[coh] an SPU reserved stack line` (measured).
- **The break replay** on the real build: after the breaks 267.2 fps, step 5.13 ms; no HALT, no crash line, no
  `[coh]` line (one run, measured).

## 4. Measured

All on this PC with the session locked (`RSX_PRESENT=0` in every run of an A/B; an A/B whose lock state changed
part way was run again), uncapped, muted.

**The break replay** (docs/73: 10 recorded breaks replayed on the same save; fps in the two 2 s windows after each
break and in the windows before them; the Havok PPU Thread's step-frame time; its cycles in the windows after
the breaks). Four DLLs from the same lift, each step adding to the last, ABCDDCBA, the same probe host:

| DLL | after breaks fps | step ms | before fps | Havok Mcycles after breaks |
|---|---|---|---|---|
| A: the old lift | 235.2, 239.7 | 5.70, 5.66 | 237.8, 244.4 | 10,063, 10,240 |
| B: + `vperm`, `vsldoi` | 256.6, 262.8 | 5.46, 5.22 | 264.4, 266.5 | 9,931, 8,696 |
| C: B + the stack stores | 249.7, 252.3 | 5.46, 5.31 | 253.2, 257.1 | 8,770, 7,723 |
| D: C + the other five helpers | 253.6, 253.1 | 5.22, 5.21 | 257.3, 258.6 | 7,259, 7,865 |

- B against A: +9.4% after the breaks, +10.1% before them, step -6.0%; every B run above every A run.
- C against B: -3.4% after, -3.9% before, every C run below every B run; the Havok cycles -11.5%.
- D against C: +0.9% after, +1.1% before, step -3.2%; every D run above every C run.

**The deep Palace** (Palace 1-1, `m02`; per boot the mean of 8 windows of 6 s after 20 s), the same four, ABCDDCBA:

| DLL | boot 1 | boot 2 |
|---|---|---|
| A | 165.75 | 166.68 |
| B | 162.52 | 167.29 |
| C | 167.27 | 166.90 |
| D | 166.95 | 165.79 |

B against A -0.8%, C against B +1.3%, D against C -0.4%, every pair overlapping: none of them moves the Palace.
Its frame is held elsewhere (docs/83: HighGraphics' coherent stores and image 2's hand-off).

**The rule**: a change is kept only if it wins on one of the two benchmarks and loses on neither. The seven helpers
win on the replay and lose on neither: kept. The stack stores lose on the replay here; with the main thread's stack
in the stack region they win on it and lose on neither (below): kept, with the knob.

**The real build against the old lift** (the PR's numbers): the base DLL and the DLL from the setup's own lift with
the new lifter, three runs each, ABBAAB, the same host:

| | the old lift | the new lifter | change |
|---|---|---|---|
| fps after the breaks | 236.5, 235.9, 238.8 | 261.9, 253.8, 260.4 | +9.1% |
| fps before the breaks | 238.8, 236.7, 241.5 | 268.4, 266.2, 265.2 | +11.5% |
| step-frame ms after the breaks | 5.62, 5.73, 5.71 | 5.42, 5.44, 5.15 | -6.2% |
| worst frame after the breaks, ms | 20.1, 19.3, 26.5 | 22.1, 21.8, 19.0 | overlap |
| windows with a frame over 16 ms | 7, 4, 7 | 5, 5, 3 | overlap |

Every run of the new lifter is above every run of the old lift on the first three lines (measured).

**The deep Palace, the same two DLLs** (8 windows of 6 s per boot, ABBAAB): the old lift 163.09, 164.96 and 165.61
fps, the new lifter 166.69, 163.92 and 164.14. +0.2%, the runs overlap: no loss (measured).

**The stack stores with the knob**: the PR's real build, one host, the game DLL with and without the third commit,
`PS3_MAIN_STACK_LV2=1` on both sides, ABBAAB:

| | without | with | change |
|---|---|---|---|
| fps after the breaks | 258.8, 254.8, 257.7 | 269.9, 274.7, 271.1 | +5.8% |
| fps before the breaks | 261.4, 267.4, 269.2 | 277.5, 280.6, 278.4 | +4.8% |
| step-frame ms after the breaks | 5.31, 5.39, 5.36 | 5.22, 5.06, 5.10 | -4.2% |
| Havok Mcycles after the breaks | 9,191, 9,249, 9,320 | 9,100, 7,099, 8,461 | -11.2% |
| worst frame after the breaks, ms | 19.0, 20.5, 21.2 | 21.0, 27.9, 21.5 | overlap |
| windows with a frame over 16 ms | 3, 3, 7 | 6, 7, 2 | overlap |

Every run with them is better than every run without on the first four lines (measured).

**The deep Palace, the same two DLLs with the knob**, two ABBAAB A/Bs: 166.93, 168.20 and 170.76 fps without,
157.02, 169.50 and 167.74 with (-2.3%; the 157.02 run is 6% under the other five); then 165.93, 167.54 and 167.60
without, 166.19, 168.07 and 168.27 with (+0.3%). Both overlap: no loss (measured).

## 5. Next

- **The context pointer** (measured, dropped): every lifted function got `ppu_context* __restrict ctx`, so a guest
  store (a byte store through `vm_base + ea`, which may alias anything) no longer makes clang reload the guest
  registers it holds in host registers. The context load/store class is the largest in the Havok thread's lifted
  code (23.4% of its game-DLL samples on the new build). On the replay +0.6% fps after the breaks (257.5 ->
  259.2), the step the same, the worst frame and the windows over 16 ms up; on the deep Palace -0.8%; every pair
  overlapping. No win, so not kept.
- **A `const` vm_base** (tried, dropped before a build): clang marks the global constant but still loads it once
  per access, as each fast path is its own branch. No change in the code.
- Two classes need a minimum CPU above SSE4.1, which is a decision, not a lifter change: the byte swaps (11.7%,
  `movbe` loads and stores) and `fmadd` (a call to the C runtime's `fma` at each of 4,889 sites; FMA3 makes it one
  instruction).
