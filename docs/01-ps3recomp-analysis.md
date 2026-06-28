# 01 — ps3recomp Tool Analysis

*Analysis of [github.com/sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp), cloned locally at `./ps3recomp/` (commit `fa2d2da`, v0.6.3 "Knowing the Names", June 2026). Based on reading the source and docs, not just the README.*

## What it is

A **clean-room, MIT-licensed static recompilation toolkit** for PS3 → native PC. ~70K LOC. No Sony code or keys included. Per-game workflow (one project per game), unlike an emulator.

- **`tools/`** — Python pipeline: ELF/SELF/PRX parsers, PPU + SPU disassemblers and lifters, NID database, function finder, Ghidra/IDA helpers.
- **`runtime/`** — C/C++: PPU context (registers, big-endian memory), syscall dispatch (LV2), SPU *fallback* registry, DMA, threading.
- **`libs/`** — 97 HLE modules + 1 partial (media decode needs FFmpeg): cellGcmSys, cellAudio (WASAPI), cellPad (XInput), cellFs, sys_net, cellSpurs, etc.
- **`include/ps3emu/`** — public headers.
- **`templates/project/`** — per-game starter (config.toml + CMake).

### Pipeline
`SELF decrypt → ELF parse → function detection → PPU disasm → PPU lift to C → link with runtime + HLE → compile → native exe`

## How PPU recompilation works

Deterministic, instruction-by-instruction lift of PowerPC64 (+ VMX/AltiVec) to C. Each guest function becomes a C function taking a `ppu_context*` (32 GPR / 32 FPR / CR / LR / CTR / FPSCR). Memory is a 256 MB array with big-endian byte-swap macros. A `func_table` maps guest address → host function pointer for indirect calls. **This part is real and works** — it's the same proven technique as N64Recomp / XenonRecomp, and needs no AI.

`tools/ppu_lifter.py` is the heart (148 KB). README notes "~10K TODOs remaining" in the lifter (down from 27K) — i.e. instruction forms not yet handled; these surface as gaps per game.

## How SPU is *actually* handled — the critical finding

The marketing README says it "fully recompiles SPU programs." **The source says otherwise.** From `ps3recomp/docs/SPU_FALLBACK.md`, line 3:

> "ps3recomp does **not execute SPU code** (no SPU ISA JIT, no SPU local-store model)."

Reality:
- There *is* an SPU disassembler/lifter (`spu_disasm.py`, `spu_lifter.py`), but it is validated on **synthetic unit tests** (sum, shufb, DMA, brsl), **not** wired to run a real game's SPU workloads in the runtime.
- The runtime instead offers a **per-game PPU-fallback registry** (`spu_register_ppu_fallback(entry_point, handler)`): for each SPU job a game runs, **you hand-write a C function on the host CPU** that reads the job's args/local-store, does the equivalent work, and writes results back to guest memory.
- Default behavior for an unhandled SPU job: the thread "exits cleanly" with status 0 — i.e. it **silently does nothing**. `SPU_FALLBACK.md`: *"Stubbing those silently leaves PPU code reading garbage."*

**Implication for Demon's Souls (severe):** DeS uses SPUs for real work (Havok physics, animation, decompression, likely post-processing). Each such job is a separate reverse-engineering + reimplementation task. This is the single largest body of unknown work, and the biggest reason "fully playable" is uncertain. It is *tractable* (identify job by entry point → reverse → reimplement on host), but labor-heavy.

## How graphics is handled

- **cellGcmSys** (HLE) tracks RSX state; **RSX command processor** parses the NV47xx FIFO (surfaces, viewport, 16 texture units, 16 vertex attribs, draw calls) and dispatches to a backend via 12 callback points.
- **Backends:** `null` (Win32 window showing clear color — used to validate command flow) and a **D3D12** backend. The D3D12 backend renders simple geometry in the flOw demo (~88 FPS), but **full RSX shader translation (Cg microcode → HLSL) and texture sampling are partial/incomplete**. `GAME_PORTING_GUIDE.md` lists "Vertex/fragment program translation," "texture upload and sampling," and "framebuffer management" as not-done. Vulkan is planned, not built.
- DeS's deferred renderer + post-processing will exercise far more of this than flOw's trivial scene. This is the second crux.

## Proof-of-concept reality check

| Game | Functions lifted | Generated C++ | State |
|---|---|---|---|
| flOw (simplest PS3 title) | ~102K | ~190 MB | Boots, ~88 FPS, **"title-state wall" — not playable**; SPU sidestepped via real `libsre.prx`; input never naturally polled |
| Tokyo Jungle | ~33K | — | Reaches main loop, **"draws nothing"** |
| Simpsons Arcade | ~14.7K | — | Boots to GCM init |

Even the simplest title required **manual per-game surgery** (a hand-edited main loop) and is not playable. This is the honest baseline.

## What this means for us

- The **deterministic PPU lift + HLE framework is a real head start** — we are not starting from zero. Decryption, analysis, function detection, PPU lifting, and ~97 HLE modules already exist.
- The work *we* must supply for Demon's Souls is concentrated in: (1) **SPU job reimplementation**, (2) **RSX shader translation for a deferred renderer**, (3) **HLE gap-filling + per-game boot debugging**, (4) lifter fixes for any unhandled PPU instruction forms.
- All four are normal (if hard) systems programming — the kind of work AI can accelerate, and where we can **upstream improvements** back to ps3recomp.

## Per-game workflow (from `docs/GAME_PORTING_GUIDE.md`)

1. Assess in RPCS3 (log modules/NIDs/SPU usage). 2. Decrypt EBOOT → ELF. 3. Analyze (imports, coverage). 4. Detect functions + disasm. 5. Lift PPU (+ extract SPU images). 6. Set up project from template. 7. Build + debug (expect errors). 8. Stub/implement missing NIDs. 9. Graphics (null → D3D12). 10. Audio/input. 11. SPU fallbacks. 12. Test/polish. See [`02-roadmap.md`](02-roadmap.md) for how we map these to milestones.
