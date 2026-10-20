# 58. PGO builds: profile-guided optimization (a developer tool)

Date: 2026-10-19. The measurements are in docs/56, section 6 (rows 59-65 and pgo1).

A PGO build compiles des_boot twice. The first build counts how often every branch runs. A play session on it writes
the counts to a profile. The second build reads the profile and compiles for what the game actually does: the code it
runs most is packed together, branches are laid out the way they usually go, and inlining goes where the calls are. At
the Palace this made the frame 6 to 8% faster, on all three threads that hold the frame at once.

PGO is a developer and measurement tool. Nothing of it goes to players: no profile is shipped or downloaded, and the
player's setup has no profile step (decided 2026-10-19).

## Summary

- **Measured:** +6.3% in the profile windows and +7.9% between them, against the same sources built the normal way,
  at the deep Palace save, uncapped, 6 rounds a side. The PGO build was faster in all 6 rounds (p 0.001 or less), and
  the 1% low rose 8.8% (docs/56, pgo1).
- **Four commands** in `tools/pgo/pgo.sh`: `gen`, `play`, `merge` and `use` (section 1). Here the four take about 40 minutes, most of it the two builds.
- **Beside the normal build.** The PGO builds have their own folders (`port/build_pgo_gen`, `port/build_pgo_use`,
  `ps3recomp/build-pgo-gen`, `ps3recomp/build-pgo-use`), configured with the settings of `port/build` and
  `ps3recomp/build`, which stay as they are. The one CMake change is the cache variable `PS3RECOMP_BUILD_DIR`
  (default `ps3recomp/build`), so des_boot can link a second runtime build. The profile flags go in through a compiler
  launcher, `tools/pgo/pgocc.py`, so no CMake file carries them. A normal build does not change.
- **The profile stays out of git.** It is made by running the game, so it is game-derived, like the lift. It lives in
  `work/pgo/`, which git ignores.
- **What you play matters.** Code the session never ran is compiled as cold code, for size. A profile from the Palace
  covers the engine's shared code; how much of the gain holds elsewhere in the game was not measured.
- **For measurements:** a PGO build is a different exe from the one players run. Measure a change with both sides
  built the same way: both normal, or both PGO with a profile made for each side (section 6).

## 1. The commands

```
tools/pgo/pgo.sh gen            # instrumented build: port/build_pgo_gen/des_boot.exe
tools/pgo/pgo.sh play 10        # play it, up to 10 minutes; the counts go to work/pgo/profraw/
tools/pgo/pgo.sh merge          # work/pgo/profraw/*.profraw -> work/pgo/des.profdata
tools/pgo/pgo.sh use            # optimized build: port/build_pgo_use/des_boot.exe
DES_EXE=port/build_pgo_use/des_boot.exe tools/des/play.sh
```

`pgo.sh` needs what the normal build needs (configured `port/build` and `ps3recomp/build`, clang-cl, ninja,
vcvars64.bat; `VCVARS` and `NINJA_DIR` come from `tools/des/local.env`, as for `build.sh`), plus `llvm-profdata`, which
the LLVM install puts beside clang-cl. Nothing else is installed. `play` takes what `play.sh` takes (minutes, then
`ENV=val` knobs).

| Step | Time here (Ryzen 7 9800X3D, 32 GB) | Notes |
|---|---|---|
| `gen` | about 20 minutes at 3 jobs (each lift chunk 165 to 285 s, then the link) | Each instrumented lift chunk holds about 3 GB while it compiles, so the default is 3 jobs; 6 jobs ran this PC out of memory. `pgo.sh gen 2` on a 16 GB PC. |
| `play` | 5 minutes is plenty | The instrumented exe runs at about two thirds of the normal speed (70 to 85 fps at the Palace, against about 120). |
| `merge` | seconds | 5 minutes made a 77 MB profile. |
| `use` | about 11 minutes at 4 jobs | 4 jobs by default. |

## 2. What to play

- The profile decides what gets faster. In the measured build, the code the session ran a lot went to `.text$hot`
  (12.3 MB), and the code it never ran went to `.text$unlikely` (220.6 of the 272.3 MB of code). Cold code gets little
  inlining and is compiled for size.
- So play where the build should be fast. The measured profile was 2 minutes standing at the deep Palace save, then 3
  minutes of walks and camera turns (docs/56, row 63).
- Several sessions add up: `merge` takes every `.profraw` file in `work/pgo/profraw`. To start over, move the old ones
  out first.

## 3. A profile that no longer fits

- A profile fits the sources and the lift it was made from. When a function's code changes, clang warns once for it
  ("hash mismatch", `-Wbackend-plugin`) and compiles it as if it had no profile; every other function still uses its
  counts. `pgo.sh use` prints how many functions that hit in what it compiled.
- After a re-lift, the lifted functions whose code changed lose their counts the same way. The lift is deterministic
  (the same EBOOT and the same lifter give the same lift), so a profile made in one checkout fits another.
- Make a new profile when that count grows. `pgo.sh use` builds from a copy of the profile named by its contents
  (`work/pgo/use/des-<md5>.profdata`), so a new profile recompiles everything by itself. Old copies can be removed.

## 4. How it works

- **IR instrumentation** (`-fprofile-generate` and `-fprofile-use`), not the front-end kind
  (`-fprofile-instr-generate`): in the smoke test only the IR kind gave the optimized build a `.text$hot` section
  (docs/56, row 59).
- **Continuous mode.** `pgo.sh play` sets `LLVM_PROFILE_FILE=work/pgo/profraw/des-%p%c.profraw`. `%p` gives each run
  its own file. `%c` keeps the counters in the mapped file while the game runs, so closing the window, a kill or a
  crash loses nothing. On Windows that needs `-mllvm -runtime-counter-relocation` at compile time. Value profiling is
  off (`-mllvm -disable-vp=true`), because continuous mode does not keep it.
- **Static functions** are named in the profile by their file's base name, not its full path
  (`-mllvm -static-func-full-module-prefix=false`), so a profile also fits a checkout in another folder.
- **The instrumented link.** The objects pull in clang's profile runtime, `clang_rt.profile-x86_64.lib`. It is built
  /MT and asks for libcmt, and des_boot is /MD, so `pgo.sh gen` adds `/NODEFAULTLIB:libcmt.lib` and the runtime's folder
  through lld-link's `_LINK_` variable. It also links `tools/pgo/getenv_thunk.c`. ps3recomp's
  `runtime/ppu/tests/boot_main.cpp` defines `__imp_getenv` (its getenv cache). Without the thunk, the profile runtime's
  call to plain `getenv` pulls in ucrt's import member for `getenv`, which defines `__imp_getenv` a second time, and the
  link stops.
- **CMake's own checks** (try_compile, under CMakeScratch or CMakeTmp) run without the profile flags.
- **Folder names** follow each repo's .gitignore: `build_pgo_*` in the port (`build_*/`), `build-pgo-*` in ps3recomp
  (`build-*/`).

## 5. Measured

pgo1 (docs/56): A is the base, B the PGO build of the same sources (port 589cb27, ps3recomp b249b7a) and the same lift,
built the same way except for the profile. The deep Palace save, `VBLANK_MHZ=240000`, alternating exes, 6 rounds.

| | Base | PGO | PGO - base, median of the rounds | Rounds PGO faster | Paired p |
|---|---|---|---|---|---|
| Profile-window fps, boot median | 122.95 | 130.98 | +7.80 fps (+6.34%) | 6 of 6 | 0.001 |
| Gap fps, boot median | 127.64 | 137.58 | +10.19 fps (+7.94%) | 6 of 6 | <0.001 |
| 1% low (profile windows) | 82.25 | 89.45 | +8.8% | | |

The three threads that hold the frame stayed as busy as before (HighGraphics 95-96%, the FIFO thread 91-92%, image 2
87-88%), so each did its frame's work 6 to 8% faster: the lifted PPU code, the lifted SPU code and the runtime's C all
got faster together.

The PGO build in this table was made by the measurement lane's scripts, the steps `pgo.sh` puts into one
script (docs/56, rows 59-65). It passed regress.sh at 30 and 240 fps (docs/56, row 69). `pgo.sh` itself has
not yet been run end to end: that takes about 40 minutes of the PC, and today's optimization runs came first.

## 6. Open

- **A profile route in the tree.** The measured profile came from a scripted route (a frozen save and a list of stick
  moves, run through `levelcap.sh`) in the session's lane, not in the tree. A route here would make profiles
  repeatable from one measurement to the next.
- **PGO on both sides of an A/B.** A change measured on PGO builds needs a profile made for each side, since the
  changed code has no counts in the other side's profile. That is one more instrumented build and play session per
  side, so today's A/Bs stay on normal builds.
