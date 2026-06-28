# 02 — Demon's Souls Port Roadmap

Milestone-based plan. Each milestone produces a concrete artifact and de-risks the next. Early milestones are achievable; later ones are hard and uncertain. We proceed milestone by milestone and re-assess at each gate.

**Game:** Demon's Souls (USA), Title ID **BLUS30443**. Engine: FromSoftware proprietary (the early "Souls" engine).

---

## M0 — Environment & quantified assessment  ·  *achievable now*

**Goal:** know exactly what we're dealing with, in numbers.

- [ ] Install prerequisites: Python deps, CMake, MSVC + Windows SDK (D3D12). See [`03-setup.md`](03-setup.md).
- [ ] Obtain a **decrypted** `EBOOT.ELF` (via RPCS3 dump or public keys — never committed).
- [ ] Run analysis pipeline on the ELF:
  - `elf_parser.py` → header, segments, entry point, TOC
  - `prx_analyzer.py --coverage` → which HLE modules DeS imports, and our coverage %
  - `nid_database.py --resolve` → resolved vs unresolved NIDs
  - `find_functions.py` → function count (expect *far* more than flOw's ~50K)
  - `elf_parser.py --extract-spu` → **how many SPU images / how SPU-heavy DeS is** (this number drives the whole risk profile)
- [ ] Write findings into [`04-worklog.md`](04-worklog.md).

**Gate:** Is module coverage high enough and SPU usage bounded enough that M1+ is worth it? Decide here.

---

## M1 — PPU lift + build + boot to entry  ·  *hard but mechanical*

**Goal:** the recompiled binary starts, runs CRT init, and enters game code with a **null renderer**.

- [ ] Lift all PPU functions to C (`ppu_lifter.py`). Expect gigabytes of generated C and many lifter TODOs to fix.
- [ ] Set up the project from `templates/project/`, enable only DeS's modules.
- [ ] Get it to **compile and link** (iterate on undefined refs, lifter gaps).
- [ ] Reach CRT startup → main PPU thread → first game instructions (the flOw "TOC save" and "32-bit address sign-extension" lessons apply).

**Gate:** Does it execute recompiled code without immediately faulting?

---

## M2 — HLE coverage to reach the main loop  ·  *hard, iterative*

**Goal:** get past initialization to the game's main update loop.

- [ ] Run with `break_on_unimplemented = true`; cross-reference call sequences against RPCS3 logs.
- [ ] Implement/stub missing NIDs in priority order (init-critical first).
- [ ] Wire audio (cellAudio → WASAPI) and input (cellPad → XInput).

**Gate:** Engine reaches a stable per-frame loop (the wall flOw is stuck at — getting *past* it is novel territory).

---

## M3 — SPU job reimplementation  ·  *the crux, open-ended*

**Goal:** the SPU work DeS depends on produces correct output on the host.

- [ ] Instrument `sys_spu_image_open` to log every SPU image entry point DeS loads.
- [ ] Classify each job: physics (Havok), animation/skinning, decompression, audio, post-fx.
- [ ] For each, write a PPU-fallback (`spu_register_ppu_fallback`) that does the work on the host CPU and writes results back to guest memory deterministically (games checksum results).
- [ ] Decompression/audio jobs first (easier, well-known algorithms); physics/compute last.

**Gate:** No more "PPU reading garbage" from stubbed SPU jobs. This milestone may dominate the timeline.

---

## M4 — RSX → D3D12 with shader translation  ·  *the other crux, open-ended*

**Goal:** the deferred renderer actually draws Demon's Souls.

- [ ] Capture and log the RSX command stream; identify vertex formats, shader programs, draw patterns, render targets.
- [ ] Extend ps3recomp's D3D12 backend: RSX Cg fragment/vertex microcode → HLSL, texture upload + sampling (de-swizzle GTF), MRT framebuffers for the deferred G-buffer, blend/depth/stencil states.
- [ ] Bring up incrementally: clear → static geometry → textured → lit → post-processing.

**Gate:** Recognizable in-world rendering.

---

## M5 — Playable to main menu, then in-game  ·  *milestone of success*

- [ ] Title → main menu → character creation → Nexus → first level (Boletaria 1-1).
- [ ] Save/load (works on virtual filesystem). Online deliberately out of scope initially.

---

## M6 — Enhancements  ·  *only after stable*

The original motivation, but earned last:

- [ ] **Framerate uncap** — decouple simulation from the 30 fps timestep (DeS, like all Souls games, ties physics/animation to framerate; this needs care to avoid the speedups/bugs seen in Souls games).
- [ ] **Resolution / internal scaling** — native widescreen, arbitrary resolution.
- [ ] AOT shader compilation (the whole point — no runtime stutter / pop-in).

---

## Reality markers

- ps3recomp's own estimate for an AAA title: **months to years**. DeS is AAA.
- M0–M2 are concretely doable and informative. **M3 and M4 are where "uncertain" lives** — they are research, not just engineering.
- We upstream every general fix (lifter, HLE, RSX) to ps3recomp; game-specific code (SPU fallbacks, shader specializations) stays local and asset-free.
- Re-evaluate honestly at each gate. Stopping at "boots + partial render" would still be a real, novel result for a PS3 AAA game.
