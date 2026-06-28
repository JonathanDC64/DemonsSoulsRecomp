# 00 — Feasibility Assessment

*Honest engineering assessment. Grounded in direct inspection of the disc and the ps3recomp source.*

## Verdict

A native static-recompilation PC port of Demon's Souls is an **unsolved, multi-year research problem** — but a **legitimate one to attempt incrementally**. No PS3 game is fully playable as a native port today. The value here is in the milestones (boot → init → main loop → first render), each of which teaches us something and is individually achievable, even if "fully playable" is uncertain.

This replaces the "is it possible at all" question with "how far can we get, and what do we have to build to get there."

## The three paths (and which one this is)

| Path | What it is | Status for Demon's Souls |
|---|---|---|
| **Emulation (RPCS3)** | Runs the whole PS3 at runtime | Works ("Playable"), but has the shader-stutter / pop-in we want to escape |
| **Static recompilation** ← *this project* | Translate the game's machine code to C ahead of time → native binary | Unsolved for any PS3 game; what we're attempting via ps3recomp |
| **Matching decompilation** | Hand-write byte-identical source | Infeasible at AAA scale (N64 games took teams years) |

## Why the PS3 specifically is the hard console

The Xbox 360 (also PowerPC) *was* successfully recompiled (Sonic Unleashed Recompiled, 2025). The PS3 is harder for three reasons:

1. **The Cell SPEs.** The PS3 pairs one PowerPC core (the PPE — recompilable) with **6 usable SPE coprocessors**: a different instruction set, 256 KB private local stores, DMA-driven control flow. Demon's Souls offloads render- and simulation-critical work here. **ps3recomp does not run SPU code at all** — each SPU job must be reverse-engineered and reimplemented by hand on the host CPU. This is the dominant risk.
2. **The RSX GPU.** Consumes a proprietary Nvidia command stream; its shaders (Cg microcode) aren't self-contained and need per-draw specialization to translate to HLSL/SPIR-V. Demon's Souls uses a deferred renderer — non-trivial.
3. **Encryption.** The retail `EBOOT.BIN` is an encrypted SELF; it must be decrypted to a PowerPC ELF before anything else (solved step — via RPCS3 or public keys; see [`03-setup.md`](03-setup.md)).

## What's on the disc (verified)

```
PS3_GAME/USRDIR/EBOOT.BIN   28 MB, magic "SCE\0"  → encrypted SELF (PPU + SPU code inside)
7.9 GB total assets, all FromSoftware formats:
  3604 .dcx (compressed)  2190 .hkx (Havok)  1776 .flver (models)
   356 .tpf (textures)     272 .lua + .esd (scripts/state machines)  .param (stats)
```

The asymmetry that defines the project: **the assets are well-understood** (the Souls modding scene reverse-engineered these formats — SoulsFormats, Smithbox), but **the engine is the 28 MB of encrypted code** that has to be recompiled and have its SPU/RSX dependencies reimplemented. A pile of decoded assets is not a game; the engine is the whole problem.

## Realistic expectation

- **Achievable, near-term:** decrypt → analyze → quantify SPU/module usage → lift PPU → build → boot to entry point / CRT init with a null renderer.
- **Hard, medium-term:** complete enough HLE to reach the main loop; reimplement the SPU jobs Demon's Souls depends on.
- **Hardest, long-term & uncertain:** RSX → D3D12 with full shader translation for a deferred renderer; reach main menu, then in-game.
- **Only after stable:** framerate uncap and resolution scaling.

See [`02-roadmap.md`](02-roadmap.md) for the milestone breakdown.
