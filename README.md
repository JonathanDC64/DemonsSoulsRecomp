# Demon's Souls (PS3, 2009) → Native PC Port

A research-and-engineering project to investigate, and incrementally attempt, a **native PC port** of *Demon's Souls* (BLUS30443 / USA) via **static recompilation** — using [ps3recomp](https://github.com/sp00nznet/ps3recomp) as the foundation and improving it where needed.

The goal is **native execution** (not emulation), to eliminate RPCS3's runtime shader-compilation stutter and geometry pop-in, and — once stable — to raise framerate and resolution.

## Hard constraints

- **No copyrighted code or assets are ever committed to this project.** The tooling (ps3recomp, our patches) is open-source and clean-room. The game's decrypted executable, recompiled C, and assets stay on the local machine, supplied from the user's own legally-owned disc. See [`docs/05-legal.md`](docs/05-legal.md).
- The user owns the disc: `D:\Users\jonat\Documents\roms\ps3\Demon's Souls (USA)`.

## Honest status

This is an **ambitious, multi-stage research effort**, not a weekend project. It remains uncertain whether it reaches "playable" — but it has progressed well past where it started.

**Progress so far** (the recompiled `des_boot.exe` runs the real game's PPU + system code natively):

- ✅ **M0–M1** — decrypted/analysed the EBOOT (106k+ functions, 12 SPU images), lifted all PPU code to C, and got it to compile, link, and execute.
- ✅ **M2 — full PPU boot to the 60 fps main loop.** Cleared the C++ static-init cascade (root cause: a rotate-by-zero UB the lifter miscompiled), reached module load, RSX/GCM init, and the per-frame loop. Boot journey documented in [`docs/08-ppu-boot-complete.md`](docs/08-ppu-boot-complete.md).
- ✅ **RSX/GCM bring-up started** — the GCM FIFO control register now lives in guest memory and drains, so the graphics-init thread proceeds and dispatches its first **SPU workload**.
- 🔜 **M3 — SPU/SPURS execution (current frontier).** The boot now genuinely needs the SPUs to run: it dispatches a SPURS job and waits for completion. The SPU recompiler is DeS-complete (0 ISA gaps); the remaining work is the clean-room **HLE SPURS runtime** (scheduler + job-chain + completion events). Fully specced in [`docs/09`](docs/09-spu-spurs.md)–[`11`](docs/11-spurs-runtime-spec.md).
- ⏳ **M4 (RSX shader translation) / M5 (playable)** — open-ended, not yet reached.

The two long-known cruxes are **SPU code** (each SPURS job must run, via recompilation + an HLE scheduler) and **RSX graphics/shader translation**. The bet that makes this worth attempting: the remaining work is mostly **normal systems programming** (HLE completion, per-game boot debugging, SPU reimplementation, renderer glue) where AI assistance genuinely multiplies effort — and the original tool was built largely solo without it.

> Reproduce: see [`docs/08-ppu-boot-complete.md`](docs/08-ppu-boot-complete.md) §6 (lift → patch → build → run recipe).

## Documentation

| Doc | Purpose |
|---|---|
| [`docs/00-feasibility.md`](docs/00-feasibility.md) | The honest feasibility assessment (paths, why PS3 is hard) |
| [`docs/01-ps3recomp-analysis.md`](docs/01-ps3recomp-analysis.md) | Deep analysis of the ps3recomp tool — what it really does |
| [`docs/02-roadmap.md`](docs/02-roadmap.md) | Phased, milestone-based plan specific to Demon's Souls |
| [`docs/03-setup.md`](docs/03-setup.md) | Environment / prerequisites / decryption procedure |
| [`docs/04-worklog.md`](docs/04-worklog.md) | Running log of what we did and found |
| [`docs/05-legal.md`](docs/05-legal.md) | The clean-room / no-copyright operating rules |
| [`docs/06-m0-analysis-findings.md`](docs/06-m0-analysis-findings.md) | M0: decryption + analysis (functions, modules, SPU) |
| [`docs/07-m2-boot-debug.md`](docs/07-m2-boot-debug.md) | M2: iterative boot-debugging log |
| [`docs/08-ppu-boot-complete.md`](docs/08-ppu-boot-complete.md) | **PPU boot: complete reference** (root-cause fixes, diagnostics, reproduce recipe) |
| [`docs/09-spu-spurs.md`](docs/09-spu-spurs.md) | SPU/SPURS phase: framework, DeS's 12 images, the gate, the plan |
| [`docs/10-spurs-hle-blueprint.md`](docs/10-spurs-hle-blueprint.md) | SPURS HLE bring-up blueprint (from RPCS3 behaviour, clean-room) |
| [`docs/11-spurs-runtime-spec.md`](docs/11-spurs-runtime-spec.md) | SPURS runtime spec + 20-function HLE checklist + prior-art survey |

## Layout

```
demons-souls-pc-port/
├── README.md
├── docs/              # our documentation (00–11)
├── patches/           # exported diff of our ps3recomp changes (see patches/README.md)
├── port/              # game-specific glue: CMake, des_heap, missing stubs, Win32 dirent shim
├── scripts/           # build env (vsenv.bat) + apply_boot_patches.py (re-run after each re-lift)
├── ps3recomp/         # the recompiler — heavily modified; our fork:
│                      #   github.com/JonathanDC64/ps3recomp (GITIGNORED here; tracked via its own git)
└── work/              # decrypted ELF, analysis output, generated C — GITIGNORED, never committed
```

> Our ps3recomp changes live in the fork **[github.com/JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp)** (27 commits ahead of upstream) and are also exported to [`patches/`](patches/).
