# Demon's Souls (PS3, 2009) → Native PC Port

A research-and-engineering project to investigate, and incrementally attempt, a **native PC port** of *Demon's Souls* (BLUS30443 / USA) via **static recompilation** — using [ps3recomp](https://github.com/sp00nznet/ps3recomp) as the foundation and improving it where needed.

The goal is **native execution** (not emulation), to eliminate RPCS3's runtime shader-compilation stutter and geometry pop-in, and — once stable — to raise framerate and resolution.

## Hard constraints

- **No copyrighted code or assets are ever committed to this project.** The tooling (ps3recomp, our patches) is open-source and clean-room. The game's decrypted executable, recompiled C, and assets stay on the local machine, supplied from the user's own legally-owned disc. See [`docs/05-legal.md`](docs/05-legal.md).
- The user owns the disc: `D:\Users\jonat\Documents\roms\ps3\Demon's Souls (USA)`.

## Honest status

This is an **ambitious, multi-stage research effort**, not a weekend project. As of the start of this project:

- No PS3 game has ever been made fully playable as a native PC port. ps3recomp's furthest demo (flOw, the simplest possible PS3 title) **boots but is not playable**.
- The two unsolved cruxes are **SPU code** (ps3recomp does *not* run SPU code; each SPU job must be hand-reimplemented — see [`docs/01-ps3recomp-analysis.md`](docs/01-ps3recomp-analysis.md)) and **RSX graphics/shader translation**.
- ps3recomp's own porting guide estimates a full AAA title at **"months to years."** Demon's Souls is in that bucket.

The bet that makes this worth attempting: the remaining work is mostly **normal systems programming** (HLE completion, per-game boot debugging, SPU reimplementation, renderer glue) where AI assistance genuinely multiplies effort — and the original tool was built largely solo without it.

## Documentation

| Doc | Purpose |
|---|---|
| [`docs/00-feasibility.md`](docs/00-feasibility.md) | The honest feasibility assessment (paths, why PS3 is hard) |
| [`docs/01-ps3recomp-analysis.md`](docs/01-ps3recomp-analysis.md) | Deep analysis of the ps3recomp tool — what it really does |
| [`docs/02-roadmap.md`](docs/02-roadmap.md) | Phased, milestone-based plan specific to Demon's Souls |
| [`docs/03-setup.md`](docs/03-setup.md) | Environment / prerequisites / decryption procedure |
| [`docs/04-worklog.md`](docs/04-worklog.md) | Running log of what we did and found |
| [`docs/06-m0-analysis-findings.md`](docs/06-m0-analysis-findings.md) | M0: decryption + analysis (functions, modules, SPU) |
| [`docs/07-m2-boot-debug.md`](docs/07-m2-boot-debug.md) | M2: iterative boot-debugging log (current blockers) |
| [`docs/05-legal.md`](docs/05-legal.md) | The clean-room / no-copyright operating rules |

## Layout

```
demons-souls-pc-port/
├── README.md
├── docs/              # our documentation
├── ps3recomp/         # cloned upstream tool (git, not modified here yet)
└── (future) work/     # decrypted ELF, analysis output, generated C — GITIGNORED, never committed
```
