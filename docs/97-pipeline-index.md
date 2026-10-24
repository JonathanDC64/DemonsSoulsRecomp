# The pipeline index (2026-10-23)

A list of the D3D12 pipelines the game asks for, found in play, kept in the repository for each game version:
`port/data/pso_index/<TITLE_ID>-<APP_VER>.tsv` (Demon's Souls US: `BLUS30443-01.00.tsv`). At a start the port
builds these pipelines in the background, before the game needs them. A draw then loads its pipeline from the
pipeline library instead of waiting for the graphics driver to build it.

The code is in ps3recomp (`libs/video/rsx_d3d12_backend.c`, "The pipeline index"; `rsx_shader_precompile.c`)
and in the port (`port/des_prework.cpp`, `tools/des/pso_index.py`).

## Why

- After the shader pass (docs/93) and the parallel pipeline builds (`PSO_THREADS`, docs/96), most of what is
  left of a first start's stutter is the driver building pipelines. On a PC whose driver has not seen them, a
  pipeline takes 7-20 ms (docs/93, docs/96).
- The pipeline library and its manifest only know the pipelines this PC's game has drawn before. A player's
  first start has neither.
- docs/49 planned to predict pipelines from rules about the game's programs. The index takes them from play
  instead, so a row is never a guess, and the list grows as people play.

## How it works

1. **Recording** (ps3recomp). Every pipeline a draw asks for becomes a row. That happens once for each pipeline
   in a run, in `vp_get_fp_pso`'s miss path. Rows the cache has not seen are added to
   `<shader cache>/pso_index_seen.tsv`, written off the render thread.
2. **Merging** (the maintainer). `tools/des/pso_index.py merge` adds those rows to the index in the repository.
   New rows go at the end, so the file keeps the order in which pipelines were first met.
3. **Building** (port and ps3recomp).
   - At a start, `des_prework` reads the index for the game's version (PARAM.SFO's TITLE_ID and APP_VER) and
     hands it to the shader pass.
   - The pass finds each row's two programs among the game's own, in the shader archives and the EBOOT, by
     their fingerprints. It makes their shader texts as a draw would and puts them in the shader cache.
   - Once the game runs and the manifest's startup build is done, worker threads build the rows the library
     does not have yet. That is CPUs/4 threads (1 to 4), below normal priority, in the file's order.
   - The pipelines built go into the library and the manifest like any other.
   - When the whole list is through, `<shader cache>/pso_index.built` keeps the list's hash. Later starts skip
     the index until the list changes, or until the library is made anew (another driver or GPU).
   - Over a cache that is up to date, the pass does only the index's part. It reads the archives, finds the
     programs and makes the texts the index needs, each text once. For 167 rows that took under 0.1 s.

## A row

Tab-separated, in hex:

| Field | What it is |
|---|---|
| vp | The vertex program: FNV-1a 64 of its words up to its END instruction, as the transform program store holds them. |
| vstart | The transform program start it ran from. |
| vfetch | 1: the fetch form (the program reads its vertex arrays itself). |
| fp | The fragment program: FNV-1a 64 of its instructions without their inline constants (`rsx_fp_code_hash64`). |
| exp32 | 1: 32-bit colour exports. |
| cube, d2c | The texture kinds of the units the program samples: cube and shadow units, depth read as colour. |
| clamp | 1: the sprite-edge clamp form, for draws without a depth test on a scaled target (RSX_SCALE). |
| blend, nrt, rtfmt, cmask, cull, depth | The draw state `vp_get_fp_pso` makes the pipeline with. |

- A fingerprint is a hash of a program's code. It finds the program in the player's own game files, and it
  cannot be turned back into the program. So nothing in the file is the game's. It is a small text file of
  numbers, and the game-file check (docs/54) passes it.
- Hashes and not shader names, because:
  - The runtime only sees the program's bytes. A name would need a second table from the archives' file names.
  - The 8 programs in the EBOOT have no names.
  - 70 of the 1,303 distinct programs sit under several names, up to 21.
  - A hash also stays right if a later version moves or renames files.

## Files

| File | Where | Written by | Notes |
|---|---|---|---|
| `<TITLE_ID>-<APP_VER>.tsv` | `port/data/pso_index/`, committed | `pso_index.py merge` | The index. A release has it at `<exe folder>/port/data/pso_index/`. |
| `pso_index_seen.tsv` | the shader cache | the game | The rows this cache has seen. Its first line names the game version. |
| `pso_index.built` | the shader cache | the game | The hash of the last index built in full. |

The port looks for the index in the exe's folder and up to three folders above it, as setup looks for its own
files.

## Knobs

| Knob | Default | Effect |
|---|---|---|
| `DES_PSO_INDEX` | the index for the game's version | `<file>`: another index; `0`: none. |
| `PS3_PSO_INDEX` | 1 | `0`: no recording and no building (ps3recomp). |
| `PS3_PSO_LIBRARY` | 1 | `0`: no library, so nothing is built ahead. Rows are still recorded, and written at the end. |

## Log lines

```
[des] shaders: pipeline index BLUS30443-01.00.tsv, 161 pipelines (the cache is up to date: building these only)
[des] shaders: pipeline index: 161 of 161 pipelines have both programs in the game's files
[D3D12] pipeline index: 161 built, 0 there already, 0 not built, of 161, in 134 ms
[D3D12] pipeline library saved: 2851 KB, 167 pipelines; draws so far: 180 loaded, 6 compiled
[D3D12] pipeline prebuild: 2 of 158 built, 156 there already, in 51 ms
```

- "have both programs": rows whose programs the pass found. Fewer means another game version, or programs the
  game makes or changes itself.
- "not built": a shader missing from the cache, or the driver refused the pipeline.
- "compiled" on the library line counts the draws that waited for the driver. That is the number the index
  should bring down.
- "there already" on the prebuild line (the manifest's startup build): records the library held although
  `pso_library.n` did not count them. They are loaded, not built. See the next section.

## The pipeline library

The index build stores hundreds of pipelines in the library at once. Two rules keep the library sound
(ps3recomp #115, its second commit):

- A save waits until the manifest has written the record of every pipeline stored so far. `pso_library.n`
  counts those records, and the next start builds the records after that count.
- No store uses a name the library holds. Every store looks the name up first, and the startup build loads a
  record the library has instead of building it.

Before these rules, the first save after the index build began while the manifest was still writing (181 of
327 records counted). The next start built the other 146 again and stored them under names the library had.
After that, the library's next save read past a buffer in D3D12Core and the game crashed: every start on that
cache, 5 of 5 (2026-10-23).

## Keeping the index

The maintainer adds the pipelines found in each test:

1. Find the `pso_index_seen.tsv` files in the shader caches the tests used. A test package's play.sh moves the
   old cache to `backups/<time>/shader_cache/`, so give the package's folder: the tool looks in every folder
   below it.
2. Run `python tools/des/pso_index.py merge port/data/pso_index/BLUS30443-01.00.tsv <folder or file>...`.
3. Run `python tools/des/pso_index.py check port/data/pso_index/BLUS30443-01.00.tsv` and `stats`.
4. Commit the index on its own, then start a pull request.

A seen file of another game version is left out, and so is a malformed row.

Record at a render scale above 100 as well (`RSX_SCALE=200`). There, draws without a depth test on a scaled
target use the clamp form, and its rows differ from the same draws' rows at 100. Most of them are the HUD,
menus and full-screen passes.

## Measured

The scripted Tutorial route (t38.sh: from the pinned regression save, walk a fixed path). Runs A to C and F
are a player's first start: an empty cache, setup's shader pass, then the game. Run D is the next start, on
C's cache. Counts from the library's log line.

| Run | Index | Pipelines compiled in play | Loaded from the library |
|---|---|---|---|
| A | none | 161 | 19 |
| B | A's 161 rows | 6 (new pipelines, now in the index) | 180 |
| C | A's and B's 167 rows | 1 | 182 |
| D | skipped (built in full in C) | 3 | 185 |
| F | the committed 327 rows | 0 | all 161 it asked for |

- Run F built all 327 rows in 458 ms. Every pipeline its walk asked for was in the index.

- Runs B and C found every row's programs and built all of them: 161 in 134 ms, 167 in 136 ms. Every row was
  asked for, and loaded.
- What was still compiled were pipelines the earlier walks did not meet. The route varies a little from run
  to run.
- Over the cache setup had just filled, the pass's index part took under 0.1 s (123 shader texts, all in the
  cache already).
- In run D, the pass skipped the index (pso_index.built held its hash).
- Run E started from a later save, in map m02 (the Tutorial is m08), on D's cache. It compiled 156 pipelines
  there, none of which the index had yet, and recorded them.
- The first committed index held the 327 rows of runs A to E.
- Later runs in the Nexus (scale 100 and 200), Boletarian Palace 1-1 and the Tutorial (scale 200) added 109
  rows. These are the Nexus' pipelines, and the clamp form's first 22 rows.
- With those, the index held 436 rows.
- The driver's own cache was warm in runs B and C, because they used the same exe as run A. So 134 ms is the
  cost with a warm driver cache. On a new player's PC, each pipeline takes the driver's 7-20 ms. On 4 threads,
  167 take about 0.3-0.8 s, and the game is still in its menus then.

**The first play test** (2026-10-23): a first start with the 436-row index, played by hand from the Tutorial
through the Nexus to Boletarian Palace 1-1, at RSX_SCALE=200 and 144 fps with VRR. No stutter was noticed.

- The start built 435 of the index's 436 pipelines in 357 ms.
- In play, 384 draws loaded their pipeline from the library, and 156 compiled one. By area: the Tutorial 95
  (over two loads of it), the Nexus 39, the Palace 21, the menus 1.
- No new row used the clamp form. The run met the index's 22 clamp rows, and no others.
- Its 155 new rows went into the index, which holds 591 rows now: 41 vertex and 195 fragment programs in 238
  pairs.

**The second play test** (2026-10-24): the next morning's test build, on a copy of the save the break recording
was made on, played by hand in Boletarian Palace 1-1 (two loads), then in the Tutorial's map, at RSX_SCALE=200.

- The start found all 591 of the index's pipelines in the library already (its cache came from the first play
  test's), in 255 ms.
- In play, 473 draws loaded their pipeline from the library, and 58 compiled one. By area: the Palace 21 and
  24, the Tutorial 12, the menus 1.
- No new row used the clamp form.
- Its 58 new rows went into the index, which holds 649 rows now: 42 vertex and 206 fragment programs in 258
  pairs.

## Limits

- A fragment program with more inline constants than the constant buffer holds has them baked into its text.
  Its pipelines are not recorded, because the game's files hold other values than the ones it drew with.
- Pipelines with a geometry shader (the clamp form) are not kept in the manifest. After a new library, the
  index builds them again. The start that makes the new library may already have checked the index; then the
  next start does it.
- The `FP_*` debugging knobs change the shader text. With one of them set, the pipelines built ahead do not
  match, and the draws compile their own, as they do now.
- Another game version needs its own file. The rows of one version are never used for another.
- Linux: rows are recorded, but nothing is built ahead, because the shader pass is D3DCompile's (Windows only).
