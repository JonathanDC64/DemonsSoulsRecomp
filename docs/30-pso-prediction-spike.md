# PSO prediction spike: can game data predict the D3D12 pipelines?

## Status and plan (read this first; parked 2026-10-17)

**Where it stands.** Two rounds of measurement, no code written. Verdict: a
qualified GO, as a background top-up to the pipeline library and manifest
(ps3recomp PR #34, merged), never a replacement.

| Sample | Seen | Covered (ghost rule + sfx family) |
|---|---|---|
| Palace 1-1 walk (round 1) | 155 | 151 (97.4%), before the ghost rule |
| The Nexus | 158 | 157 (99.4%) |
| Tutorial (m08) | 154 | 151 (98.1%) |
| Palace 1-1, three saves | 183-218 | 99.5% |

- Whole game predicted: **2975 pipelines** (432 PS, 98 VS). About 54 MB of
  library, 5-15 s of background compiles on 4 threads, once per driver or GPU.
- Waste: 8-10 predicted per pipeline used, because shadow mode, point-light
  count and our plain/fetch VS path are decided per draw, not by data.
- Lesson from round 2: the first sample hid a whole family (ghost twins,
  `DS_Gst_*`). Without that rule the Nexus covered 61%.

**Before building:** sample one world beyond the Nexus / Tutorial / Palace
(Stonefang, Latria, Storms or Valley). Nothing on this machine reaches one
without manual play, so this needs the user: play a few minutes there with

    tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 PS3_PSO_PREBUILD=0 PS3_SHADER_CACHE=<empty scratch dir seeded with the vs_/ps_*.dxbc of gamedata/shader_cache>

then run `predict2.py` (below) on that dir's `pso_manifest.bin`. Above about
97% with the two rules: build. Below: find the missing family first.

**Build plan (about one week + half a day):**

1. Offline predictor as a `tools/des` script (Python, standard library only):
   the spike's readers (MTD, FLVER0, BND3, MSB) and `predict2.py --gst
   --sfxfam`, writing a predicted key list for all areas. 1-2 days.
2. Precompile tool: also build fetch-mode VS DXBC (`rsx_vp_decompile_fetch`)
   and the 4 EBOOT Cg programs. 0.5-1 day.
3. Runtime: read the key list, build each desc with the same packing as
   `vp_get_fp_pso` (one shared helper, so the desc hashes to the same
   `pso_desc_key`), and feed it to the existing prebuild threads after the
   manifest, at lower priority. 2-3 days.
4. Validate: regress run, Nexus walk, the new-world sample. Count first-use
   compiles. 1 day.

Optional, cuts the waste a lot: make plain vs fetch a per-mesh decision from
data instead of per draw (halves the VS dimension).

**Where the material is.** The scripts and both rounds' manifests are kept in
`work/tmp/pso_spike/` (local, not committed; the session scratchpad they were
made in is temporary): `py/` (readers, `predict.py`, `predict2.py`),
`map/mapper.c` (build notes in its header) and the program map (`map*.tsv`), per-area predictions
(`pred_*.json`, `predkeys_*.json`), and `manifests/` (`*_nex_*`, `*_soul_*`,
`*_deep_*`, `*_live_*`, `*_pal_*`). The details of both rounds follow.

Date: 2026-10-17. Time-boxed research and measurement. No runtime or source
changes, and the game was not run.

## Question

What fraction of the D3D12 pipelines (PSOs) a real session creates, as
recorded in `gamedata/shader_cache/pso_manifest.bin`, could be predicted ahead
of time from game data? The game data here is the DeS MTD materials, the FLVER
models, and the shader archives that `tools/des/shader_extract.py` reads, plus
a short hand-made table of engine render passes (shadow, post, UI, particles).

Short answer: on the one sample we have (a Palace walk), the prediction
covered **151 of 155 pipelines (97.4%)** at the full-description level. It got
there by listing every runtime variant. That means **927 predicted pipelines
were never seen** for this one area (1078 predicted in total). For the whole
game the predicted set is about **1850 pipelines** (322 fragment programs, 79
vertex programs). The verdict is a qualified GO (see the end).

## Data used, and caveats

- **Manifest:** copied to scratch before reading. 228160 bytes = **155
  records** of 1472 bytes each, last written 2026-10-17 10:36.
  - 155 is exactly the record count of the Palace spawn walk used by the test
    loop (pinned regression save). No records were added by later play.
  - **The sample does not include the Nexus.** Records carry no map ID, so
    this is inferred: every record is explained by Boletarian Palace 1-1 data
    (`m02_00_00_00`) plus the player.
  - So every coverage number below holds for one short walk in one area. The
    Nexus and the other worlds are untested, and the hand pass table was
    written from this same trace.
- **Desc layout:** field offsets of `D3D12_GRAPHICS_PIPELINE_STATE_DESC` come
  from the Windows SDK header, checked with a small clang-cl `offsetof` program.
  The size is 656. BlendState is at 120 (RT0 at 128, 40 bytes per RT),
  RasterizerState at 452 (CullMode at 456), DepthStencilState at 496,
  NumRenderTargets at 576, RTVFormats at 580, DSVFormat at 612.
- **Shader identity:** each `pso_blobs/<hash>.dxbc` was hash-matched (FNV-1a 64
  over the bytes) against every `shader_cache/{vs,ps}_5_0_*.dxbc`. That gives
  the cache key, which is FNV-1a 64 of the profile plus the HLSL text. The key
  was then traced back to a program with a scratch tool (`mapper.c`, linked
  against `ps3recomp_runtime.lib`). The tool reproduces the precompile tool's
  HLSL generation without compiling anything. It covers every archive program
  in every variant: plain, the `--unit` conventions, the `--alt` units, the
  recorded `fp_variants.txt` entries, and flipped exp32. It also covers
  **fetch-mode** VS text (`rsx_vp_decompile_fetch`), which the precompile tool
  does not build.
  - **Result: all 67 distinct PS and all 48 distinct VS in the manifest map
    back to game data.** 63 PS and 44 VS come from the `.shaderbnd` archives.
  - The other 2 PS and 2 VS are engine programs embedded in the EBOOT as Cg
    binaries. The EBOOT holds 8 of them, at ELF offsets 0x019caeb0-0x019cbba0.
    The used ones are FP `019cb4f0` (the opaque shadow-caster FP, also used by
    2 post draws), FP `019cb060`, and VP `019cb0d0` and `019cbba0` (post).
- **Game data:** read from the extracted disc (`PS3_VFS_ROOT`). Sources used:
  - `mtd/mtd.mtdbnd` (306 MTDs)
  - `map/m02_00_00_00/*.flver` and `map/mapstudio/<area>.msb` (the model list)
  - `chr/cXXXX/cXXXX.flver`, `obj/*.objbnd`, `parts/*.partsbnd`
  - `facegen/facegen.fgbnd` (the player's face)
  - `shader/*.shaderbnd` (1421 programs: 1260 FP, 161 VP)

  Nothing derived from the game is copied here beyond program names and counts.
- **Format references:** SoulsFormats (JKAnderson) is **GPL-3.0**. It was used
  only as a format reference, and no code was taken. The spike's MTD, FLVER0,
  BND3 and MSB readers are small parsers written from scratch. They live in the
  session scratchpad (`spike/py/`) and are not committed.

## What the PSO key holds (vp_get_fp_pso)

These are the fields that can differ between two pipelines:

| Field | Source in the desc | Values seen |
|---|---|---|
| VS bytes | per-VP blob, plain (16 x `ATTR` float4 input layout) or **fetch** (0 elements, SV_VertexID) | 48 programs: 72 records plain, 83 fetch |
| PS bytes | `fp_build_hlsl(ucode, exp32, cube, d2c)` | 67 programs |
| blend | RT0 blend, mirrored to all RTs | opaque 125, alpha (SRC_ALPHA, INV_SRC_ALPHA) 23, additive (SRC_ALPHA, ONE) 7 |
| cmask | RT write mask | 7 (RGB) 133, 15 in 6 post records, 8 in 2, none for the 14 depth-only |
| cull | CullMode + FrontCCW | none 69, back (FrontCCW=1) 86 |
| depth | enable, write, func | (on, write, LEQUAL) 93, off 25, (on, no write, LEQUAL) 23, (on, write, LESS) 14 |
| RTs | count, format | 1 x RGBA8 137, 1 x R32F 4 (luminance), 0 (depth-only) 14 |
| fixed | DSV D24S8, triangles, cut 0xFFFFFFFF, 1 sample | same in all 155 |

The input layout is not a material property in our port. A plain VS always
gets the same 16-slot layout, and a fetch VS gets none. So the only
vertex-format dimension is "which VS program, plain or fetch".

## Formats: what MTD and FLVER0 give us

- **MTD** (little-endian block format). Fields used:
  - the shader path (`...\spx\DS_Phn_ColDifSpcBmp.spx` and similar; 32
    distinct)
  - `g_BlendMode`: 0 opaque, 1 edge (alpha test), 2 alpha, 3 water, 4 add,
    5 sub
  - `g_LightingType`: 0, 1 or 3

  The name suffixes follow these values: `_Alp`, `_Edge`, `_Add`, `_Sub`,
  `[L]` lightmap, `[M]` multi-texture, `_Skin`.
- **FLVER0** (big-endian). Fields used:
  - per material: name, MTD path, texture list, buffer layouts (semantic and
    type per member)
  - per mesh: material index, `Dynamic`, byte 2, byte 3 (triangle list vs
    strip), layout index

  From these we get:
  - **VS layout code.** P, plus I if bone indices, plus W if weights, plus N,
    plus T per tangent or bitangent (max 2). The UV code comes from the
    shader: D, plus D for Mul, plus L for Lit. Example: `m[db][ml]` gives
    `PINTT_DDL`.
  - **Cull.** Mesh byte 2 is the backface-cull flag (1 = cull back, 0 = none).
    Inverting that rule drops coverage from 151 to 119, and forcing "cull back"
    for everything drops it to 113.
  - **Topology.** Every Palace mesh is a strip.
- **The shader archives encode a naming grid**, and that grid is what makes
  prediction possible:
  - FP `DS_Phn_Dif[Spc][Bmp][Mul][Lit]{Csd|Sdw|___}_{HemDir3|HemEnv|HemEnvLerp|Non}{|PntS|PntSS|PntSSSS}`
  - VS `DS_Phn_{PIN..PIWNTT}_{D|DD|DL|DDL}_{Non|Sdw|Dep|DepAlp}`
  - There are also `Gst` ghost twins, many of them byte-identical to `Phn`.
  - Many VS names share bytes. For example, all `_Dep` names of the static
    family compile to one program.

## Trace: which fields vary per material, per pass, or at run time

Grouped by program (names from the mapping above):

| Field | Decided by | Predictable from data? |
|---|---|---|
| FP base (Dif/Spc/Bmp/Mul/Lit) | MTD shader name | yes |
| FP lighting family | MTD `g_LightingType`: **1 gives HemDir3** (characters, player, armour), **3 gives HemEnv** (map pieces), 0 gives Non | yes |
| FP shadow mode Csd / Sdw / none | engine, per draw. Csd and none pair with the `_Non` VS, Sdw with the `_Sdw` VS. The same material shows up in 2-3 modes in one walk | **no** (runtime) |
| FP point-light count (0/1/2/4) | engine, per draw (lights near the object). The same material is seen with 0, 1 and 2 | **no** (runtime) |
| FP texture-kind variant (exp32/cube/d2c) | engine binding convention | yes, by convention. Material FPs all matched the `--unit` convention variant; 3 post FPs matched `--alt 1=d2c`, 1 particle FP matched `--alt 2=d2c` |
| VS program | FLVER layout + UV code + pass suffix | yes |
| VS plain vs fetch | **our runtime**: a strip goes to the GPU-fetch path if `upload_strip_vp_fetch` accepts it, otherwise the CPU path. The same program is seen in both modes | **no** (port detail, not game data) |
| blend / depth write | MTD `g_BlendMode` (0/1 opaque + write, 2/3 alpha, 4 add) | yes |
| cull | FLVER mesh byte 2 | yes |
| cmask, RT count/format, depth func | render pass | pass table |
| shadow pass | depth-only, LESS. Generic `_Dep` VS (one static and one skinned program) + EBOOT FP `019cb4f0` for opaque casters, `DepAlp` FP for edge casters | pass table + MTD (edge or not) |
| post / UI / debug font | fixed engine set: 18 post records (incl. 3 with EBOOT programs), 8 menu/font, 4 particles | pass table |

## Prediction method

The spike predictor is `spike/py/predict.py` (scratch). For an area:

1. **Models.** Read the area MSB's model names: map pieces `mXXXXBX`,
   characters `cXXXX`, objects `oXXXX`. Add the player's face (facegen) and,
   optionally, every armour/weapon `partsbnd`.
2. **Mesh facts.** For each mesh, collect (MTD shader, layout code, blend mode,
   lighting type, cull flag, strip).
3. **Material passes.** For each fact, build the cross product:
   - shadow mode {Csd + `_Non` VS, Sdw + `_Sdw` VS, none + `_Non` VS}
   - point count {"", PntS, PntSS, PntSSSS}, keeping only names that exist in
     the archive
   - VS mode {fetch, plain}

   Each combination gets blend, depth and cmask from the blend mode, and cull
   from the mesh. Water materials add `DS_Water` + `DS_Water_Env` (alpha) and a
   `DS_Water_Mask` pass (cmask 8).
4. **Shadow pass.** Every opaque or edge mesh is predicted as a caster: the
   `_Dep` VS + EBOOT FP for opaque meshes, the `_Dep`/`_DepAlp` VS + `DepAlp`
   FP for edge meshes.
5. **Hand pass table.** 30 rows: post chain, EBOOT post programs, menu/font,
   4 particle rows. They were written from the trace, so their match is circular
   by construction. The post and UI rows are engine constants. Which particle
   types an area uses is effect data, which was not parsed.
6. **Comparison.** Program names are turned into shader-cache keys with the
   mapper. A predicted entry matches a manifest record when the VS key, the PS
   key and every state field (blend, cmask, cull, depth, RT count/format,
   plain/fetch) are equal.

## Coverage (Palace 1-1 walk, 155 records)

Main run (area models + player face + all parts):

| | Count |
|---|---|
| Seen | **155** |
| Predicted and seen | **151** (97.4%). By origin: material 107 of 111, shadow 14 of 14, table 30 of 30 |
| Seen, not predicted | **4** (reasons below) |
| Predicted, never seen | **927** (1078 predicted; 917 material, 10 shadow) |

The same result at coarser levels:

| Level | Seen records covered | Predicted | Of which seen |
|---|---|---|---|
| PS program | 155 / 155 | 244 PS | 67 / 67 |
| VS program | 155 / 155 | 67 VS | 48 / 48 |
| (VS, PS) pair | 152 / 155 | 606 pairs | 113 |
| full desc | 151 / 155 | 1078 | 151 |

Seen but not predicted, by reason:

- **3 records: source model not found.** These are alpha-blended
  `Dif___Bmp______HemDir3` (0/1/2 points) on a static `PINT_D` VS, culled. The
  FP is predicted, but no mesh in the area list, parts or face has that
  material/layout pairing. The likely source is a model loaded outside the MSB
  list (item, effect or cutscene model). Lacking specular textures was checked
  and ruled out: every Spc material in the Palace and parts data has one.
- **1 record: state not derived.** Additive, cull none
  `Dif___Bmp______Sdw_Non` on `PIN_D`. The program pair is predicted, but no
  MTD in the list gives additive + no lighting with these programs. It is
  probably an effect or ghost-type material.
- **Not seen as a cause:** decompile variants (exp32/cube/d2c) and the EBOOT
  programs. Both are fully covered once the EBOOT Cg binaries and the
  precompile conventions are included.

Variants (same sample):

| Run | Predicted | Covered | Note |
|---|---|---|---|
| area + face, **no parts** | 784 | 131 (84.5%) | the 20 extra misses are the player's armour/weapons (P_ MTDs). The save's equipment would need reading |
| + all parts (main run) | 1078 | 151 (97.4%) | the whole armoury counted as possible |
| + all parts, fetch VS only | 554 | 108 | plain/fetch is a real dimension: 43 records need plain |
| + all parts, no point lights | 322 | 93 | point counts multiply predictions ~3.3x and carry 58 records |
| + all parts, "tuned" (no PntSSSS; no-shadow mode only for HemDir3) | 702 | 151 | fitted to this sample; expect misses elsewhere |

Whole game: the main-run predictor over all 25 non-test area MSBs gives a
union of **1850 pipelines** (322 PS, 79 VS programs). Each area alone gives
516-1126.

## Verdict

**Qualified GO**, as an addition to the existing manifest and pipeline library,
not a replacement for them.

- Coverage meets the bar on this sample: 97.4% of the descriptions, and 100% of
  the shader programs. Every program the game used can be named from the
  archives plus the 4 EBOOT programs.
- The wasted-build ratio is high: about 6 unused for every 1 used in one area.
  The cause is three dimensions game data cannot decide: shadow mode,
  point-light count, and our own plain/fetch VS path.
- In absolute terms the waste is cheap:
  - About 1850 pipelines for the whole game.
  - At the 7-20 ms per cold driver compile noted in the library code, that is
    roughly 3-9 s of below-normal-priority work on 4 threads, once per driver
    or GPU change.
  - At the current library size (about 18 KB per pipeline) the library would be
    about 33 MB.
- The sample is one short Palace walk, so confirm on a second sample before
  building: the Nexus plus one other world.

Recommended shape:

1. Keep the manifest and library as the primary path. They are exact and
   already exist.
2. Add a predicted whole-game set, compiled in the background after the
   manifest records, so first visits to new areas stop hitching.
3. Leftovers keep compiling on first use, as today.

One runtime change would cut the waste a lot, if wanted later: make the
plain/fetch choice deterministic per mesh. For example, always use fetch for
strip draws that qualify, decided from data rather than per draw. That halves
the VS dimension. (Not done here: no runtime changes in this spike.)

## Effort for the GO path (rough)

1. **Offline predictor** (Python, standard library only, portable). Turn the
   spike scripts into a `tools/des` script that writes a predicted key list for
   all areas. Read the save's equipment instead of the whole armoury if smaller
   sets are wanted. **1-2 days.**
2. **Precompile tool:** also build the fetch-mode VS DXBC
   (`rsx_vp_decompile_fetch`; the mapper shows the text matches what the game
   compiles) and the 4 EBOOT programs. The EBOOT programs are found by scanning
   for the Cg profile magic. **0.5-1 day.**
3. **Runtime prebuild of predicted keys.** Read the key list and build each
   desc with the same packing as `vp_get_fp_pso`:
   - one shared helper for blend, cull, depth, cmask, RTs and input layout, so
     the predicted desc hashes to the same `pso_desc_key`
   - feed the descs to the existing prebuild threads at lower priority than the
     manifest

   Doing this in-process avoids reproducing the root-signature hash offline.
   **2-3 days.**
4. **Validation:** regress run plus a Nexus walk. Diff the manifest against the
   predicted set and count first-use compiles (PERF "pipelines created").
   **1 day.**

Total: **about one week.**

## Could not determine

- Coverage outside the Palace 1-1 walk: the Nexus, other worlds, bosses,
  cutscenes, spells and effects. The particle (`ds_sfx`) part is not predicted
  from data at all. Effect files were not parsed; the 4 particle rows come from
  the trace.
- What exactly selects Csd vs Sdw vs no shadow, and the point-light count. They
  look per-draw (distance/cascade, nearby lights), but this was not traced in
  the EBOOT.
- The source of the 3 alpha `Dif___Bmp` HemDir3 records, and of the 1 additive
  `Sdw_Non` record.
- `g_LightingType` 2 (assumed to be HemEnvLerp) and blend mode 5 (sub): neither
  appears in the sample.
- Answered along the way: the branch base (0 vs the program's load slot) never
  changes the VS text. All 164 VPs (archives + EBOOT) gave identical keys both
  ways, in plain and fetch mode.

## Second and third samples (2026-10-17)

Research and measurement only: no runtime or source changes, nothing committed.
The scratch predictor `predict2.py` (the spike's `predict.py` plus three
options, below) and the manifests live in the session scratchpad.

### How the samples were recorded

Each run used a fresh scratch shader-cache dir (`PS3_SHADER_CACHE=<dir>`), so
`pso_manifest.bin` held only that run's pipelines. The dir was seeded with a
copy of the top-level `vs_/ps_*.dxbc` files of `gamedata/shader_cache` (so no
recompiles; the manifest, library and blobs were not copied). `gamedata/
shader_cache` itself was not touched: its 3805 files hash the same before and
after. Command, one per save (levelcap.sh copies the user's save aside and puts
it back; the save dir hashes the same before and after):

    DES_SCRATCH=<scratch> LC_SAVE=<save dir> LC_NOPRESS=1 LC_SETTLE=40 LC_MAX=420 LC_DUMP=0 bash tools/des/levelcap.sh <tag> <cmds> DES_AUTO_CONTINUE=1 PS3_PSO_PREBUILD=0 PS3_SHADER_CACHE=<scratch cache>

`<cmds>` held four `stick` holds (right 6 s, left 6 s, up 5 s, down 5 s) and
`stat`. RSX_PRESENT=0 came from deslib.sh (the session was locked). Each run
took about 90 s.

Where each save loads (from the `script/mNN.luabnd` the run opened, and a
screenshot of an old run with the same save):

| Tag | Save | World | Area used by the predictor | Records |
|---|---|---|---|---|
| nex | `work/tmp/lc_nex/save` (old user save) | **The Nexus** (`m01`, on-screen label "The Nexus") | m01_00_00_00 | 158 |
| soul | `work/tmp/usersave_soul` | **Tutorial** (`m08`, label "Tutorial") | m08_00_00_00 (best of the 3 m08 areas: 150 vs 137 and 134) | 154 |
| deep | `save_backups/20261015_2200_user_deep` | Palace 1-1 (m02_00), a deeper spot | m02_00_00_00 | 218 |
| live | `save_backups/20261016_1742_user_live` | Palace 1-1 | m02_00_00_00 | 184 |
| pal | `save_backups/20261015_103349_palace` | Palace 1-1 | m02_00_00_00 | 183 |

### Limitation: no other world was reachable

Every save on this machine loads the Nexus, the Tutorial or Palace 1-1 (m02).
No log or backup in `work/tmp` ever opened `m03`-`m07`. There is no warp in
the debug console (it has `mem`, `setenv`, `guard`, `dumpbin`). Reaching
Stonefang and the rest means walking to an archstone and driving menus blind,
and the archstones are locked until earlier bosses are dead, so this was not
attempted. **Stonefang, Latria, Storms and Valley remain untested.** The
Tutorial uses Palace-style art (97% coverage with the Palace-trained rules), so
it is a weaker test than a new world. The Nexus is the real new area; the two
extra Palace saves add other spots, not new art.

### Coverage

Same predictor and the same hand pass table as the first sample, whole armoury
counted, area MSB + facegen + all parts. "Base" is the spike predictor
unchanged; "+ ghost" adds the extension found below.

| Sample | Seen | Base covered | + ghost covered | + ghost + sfx family |
|---|---|---|---|---|
| Palace spike (first sample) | 155 | 151 (97.4%) | 151 | |
| nex (Nexus) | 158 | **96 (60.8%)** | **156 (98.7%)** | 157 (99.4%) |
| soul (Tutorial) | 154 | 150 (97.4%) | 150 (97.4%) | 151 (98.1%) |
| deep (Palace) | 218 | 188 (86.2%) | 215 (98.6%) | 217 (99.5%) |
| live (Palace) | 184 | 167 (90.8%) | 182 (98.9%) | 183 (99.5%) |
| pal (Palace) | 183 | 166 (90.7%) | 181 (98.9%) | 182 (99.5%) |

Levels for each sample after "+ ghost" (seen records covered, of seen):

| Level | nex | soul | deep | live | pal |
|---|---|---|---|---|---|
| PS program | 157/158 | 154/154 | 216/218 | 183/184 | 182/183 |
| (VS, PS) pair | 156/158 | 150/154 | 216/218 | 183/184 | 182/183 |
| full desc | 156/158 | 150/154 | 215/218 | 182/184 | 181/183 |
| distinct PS seen / predicted | 77 / 268 | 67 / 244 | 87 / 328 | 71 / 328 | 74 / 328 |
| distinct VS seen / predicted | 46 / 63 | 45 / 63 | 60 / 67 | 57 / 67 | 56 / 67 |
| predicted, never seen | 1360 | 1294 | 1559 | 1592 | 1593 |

(Without the ghost rule the predicted-never-seen counts are 892 / 718 / 890 /
911 / 912.)

### Seen but not predicted

**The first real miss: the ghost family (`DS_Gst_*`).** The Nexus run had 62
uncovered records, 60 of them ghost twins: `DS_Gst_<layout>_<uv>_{Non,Sdw}` VS
with `DS_Gst_Dif...{Csd,Sdw}_HemDir3{,PntS,PntSS,PntSSSS}` FP, drawn opaque or
alpha-blended (blend 1,5,6; no depth write). The spike had noted the Gst names
in the archive grid but never generated them, and the Palace trace had none, so
the gap was invisible there. It is a predictor gap, not a data gap: the names
are in the archives. The rule "every HemDir3 (character-type) material also
gets its Gst twin, in its own blend and in alpha" covers all 60, and the 27 and
15 in the deeper Palace saves. The Palace saves had these records too (30, 17
and 17 misses), so ghosts are not Nexus-specific. Cost: predictions go up by
about 600-700 per area. This is a required rule, not an area quirk.

After the ghost rule the residual misses are:

| Cause | nex | soul | deep | live | pal |
|---|---|---|---|---|---|
| ds_sfx effect programs (particles, distortion, line) | 2 | 1 | 3 | 2 | 2 |
| 3 alpha `Dif___Bmp______HemDir3` (0/1/2 points): `Gst` PINT_D VS with the Phn FP, cull none | 0 | 3 | 0 | 0 | 0 |

- The 3-record group has the same signature the first sample reported (alpha
  Bmp HemDir3 on PINT_D); here the VS is the Gst twin. Still no source model
  found; an alpha-faded copy of every opaque material (tried) did not produce
  it. 3 records of 154 in one sample, 0 elsewhere.
- Every other residual miss is a particle or effect type outside the 4 rows of
  the hand table (DistortionType0 VS with DistortionType5 FP; SimpleSpriteType0
  and LineType0 with other blends or culls). Predicting the whole `ds_sfx`
  family from the archives (VS x FP of the same family, add/alpha, 3 culls, 3
  decompile variants, plain/fetch: 189 pipelines) covers 1-2 more records in each
  sample. Every VS x every FP (4748 pipelines) covers every record in nex and
  deep, but it is wasteful.

### Did the hand pass table's circularity hurt?

Not for the rows it holds. Its 30 rows (post chain, EBOOT, menu/font, 4
particle rows) were written from the Palace trace. They matched 30/30 in nex
and soul and 28/30 in the three Palace saves (the 2 missing rows were just not
drawn there). All the post and UI rows held in the Nexus and Tutorial, so the
engine-constant assumption is good. Where it does hurt is particles: apart from
the 3-record group, every residual miss after the ghost rule is a ds_sfx effect
(2, 1, 3, 2, 2 records, 0.6-1.4% of seen). Which effect types an area spawns
is effect data that was not parsed, so a hand table cannot be complete there.
Small in records, but it is the open end of the method. The big miss (ghost
twins, 38% of the Nexus) was not in the table at all: it was a gap in the
material rules, hidden because the first sample did not contain it.

### Updated numbers

- Whole game with the ghost rule + sfx family (all 25 area MSBs, whole
  armoury): union **2975 pipelines** (432 PS, 98 VS programs), up from 1850
  (322 PS, 79 VS). The base predictor re-run reproduced the old 1850, so the
  two are comparable. Per area 1233-1987.
- At about 18 KB a pipeline that is about 54 MB of library. At 7-20 ms a cold
  compile it is 21-60 CPU-seconds, about 5-15 s on 4 threads, once per driver.
- Used to predicted is about 1:8 to 1:10 per area (154-218 seen of 1633-1963
  predicted).

### Verdict (updated)

**Still a qualified GO, now better supported** for the Nexus, the Tutorial and
Palace 1-1: 98-99.5% of seen descriptions with the ghost rule and the sfx
family, 98.7-99.4% in the Nexus, a different map with different NPCs and
effects. The first sample was not a fluke, and the hand-table circularity is
not what limits coverage.

What changes:

1. **Ghost twins are mandatory** in the predictor. Without them the Nexus
   falls to 61% and the Palace saves to 86-91%. The 2975 whole-game figure
   replaces 1850.
2. **ds_sfx must be predicted as a family** (189 pipelines) rather than from a
   4-row table, until effect files are parsed. Residual is about 1% of records.
3. **Keep the manifest and library as the primary path.** The predicted set is
   a background top-up; the few remaining misses compile on first use.
4. **Still open:** no new world (Stonefang, Latria, Storms, Valley) was
   sampled, because nothing here reaches one without long manual play. They
   bring new materials, enemies and bosses. The MTD/FLVER/pass rules are
   expected to hold, but another surprise like the ghost family is possible.
   The cheapest next check is one session in another world with
   `PS3_PSO_PREBUILD=0` and a scratch `PS3_SHADER_CACHE`, then `predict2.py`
   on its manifest (minutes of work once the manifest exists). If that sample
   also covers above about 97% with the two rules, build.

The effort estimate (about one week) is unchanged; add half a day for the ghost
and sfx rules in the offline predictor.
