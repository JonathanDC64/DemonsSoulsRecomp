# PSO prediction: design (2026-10-18)

This is the design that follows docs/30's verdict. The plan is to predict D3D12 pipelines from game data, as a background top-up to the pipeline library and manifest (ps3recomp PR #34). It never replaces them. Nothing here is built yet.

Sources:
- ps3recomp fork/master f15a4f0:
  - `libs/video/rsx_d3d12_backend.c`: the pipeline library at lines 2686-3063, `vp_get_fp_pso` at 3839, `render_frame` at 7718.
  - `libs/video/rsx_shader_precompile.c`.
- The port's origin/master: `port/des_prework.cpp`, `tools/des/play.sh`, `desrun.sh`, `deslib.sh`.
- The spike scripts in `work/tmp/pso_spike/`, and the 6 sample manifests of docs/30.
- The live cache, `gamedata/shader_cache`: 616 manifest records, copied before reading.
- The new numbers come from prototype scripts in the lane scratch dir, not in the repo (section 9).

Tags:
- **(read)**: read from code or files.
- **(measured)**: counted by a script on this machine's data.
- **(inferred)**: estimated, not run.

No game run was made for this doc.

## Answer first

**What to build, in this order:**

| Step | What | Repo | Days |
|---|---|---|---|
| 0 | One sample in a new world: Stonefang, Latria, Storms or Valley. The maintainer plays 5-10 minutes there and runs two commands (section 7). | maintainer | 0.5 to analyse |
| 1 | `tools/des/pso_predict.py`, Python standard library only. It reads the user's own game files and writes `pso_predict.tsv` into the shader cache dir: the predicted pipelines, as programs and state. Rules v3, in 4 tiers. A `--check` mode scores a manifest. | port | 2-3 |
| 2 | The startup precompile also writes `shader_keys.tsv` (program and form → cache key), and compiles the EBOOT's 8 Cg programs. Fetch-form vertex programs are already precompiled (13f5fb3). | ps3recomp + port | 1-1.5 |
| 3 | Runtime: after the manifest prebuild, the same threads build the predicted pipelines at a lower priority, tier by tier. One desc helper is shared with `vp_get_fp_pso`, so the names match. A draw that loads a predicted pipeline adds it to the manifest. | ps3recomp | 2-3 |
| 4 | Validation by counts (section 6). | both | 1 |
| B | Later, for whatever prediction misses: the pre-pass compiles a frame's misses in parallel, then waits. The frames stay the same and the hitch is shorter (section 3). | ps3recomp | 1.5-2 |

The core is 6.5-8.5 days; docs/30 said about 1 week + 0.5 day. B adds 1.5-2.

Steps 1 and 2 do not change what the game draws, so they can start now. Step 3 waits for step 0.

**Decisions for the maintainer:**

| # | Question | Recommendation |
|---|---|---|
| D1 | Go or no-go after the new-world sample | Go if the best area scores 97% or more with the section 7 command. Below that, find the missing family first. |
| D2 | How much to predict | All 4 tiers, in order: 5062 pipelines, about 88 MB of library (inferred). Without the sfx tier: 3662 pipelines and about 64 MB (inferred), for 1.0 point less on the live cache and 0-1.4 points less on the samples (measured). |
| D3 | The 12 `DS_Dbg_*_PntNum` records in the live cache | Leave them out. Their source is unknown (a debug pass?). Promotion on use keeps them if they are real. |
| D4 | Where `pso_predict.tsv` lives, and who makes it | In the shader cache dir. The setup step makes it, and play.sh makes it when it is missing. Test scripts copy it into their scratch cache when a test needs it. |
| D5 | Threads and priority of the predicted phase | As many threads as the manifest prebuild (CPUs/4, from 1 to 4), at THREAD_PRIORITY_LOWEST. |
| D6 | Fallback for misses | B, after prediction. Async skip (C) as an opt-in at most, because what it draws depends on timing. |
| D7 | When to save during the predicted phase | Once per finished tier. Today's rule (save at 256 pending) would rewrite a growing library 17-20 times. |

**Key numbers:**
- **The docs/30 sfx numbers were wrong.** They came from a regex bug in predict2.py (section 1.1). With the bug fixed, docs/30's own rules predict 3718 pipelines for the whole game, not 2975 (measured).
- **Coverage.** Rules v3 cover all 6 docs/30 samples (100%) and 604 of the 616 live manifest records (98.1%) (measured). But v3 was fitted on these same manifests, so step 0 is the first out-of-sample test.
- **Size.** The whole game under v3 is 5062 pipelines, built from 452 pixel shaders and 108 vertex shaders (measured). Of the 5062, the live cache (every session so far) has used 604: 1 in 8.4.
- **Cost (inferred).** 35-101 CPU-seconds of driver compiles, once per driver or GPU. That is about 9-25 s on 4 idle threads. The library is about 88 MB.

## 1. What changed since docs/30 (measured)

### 1.1 The sfx family rule was narrower than meant

predict2.py, line 180:

```python
if '--sfxfam' in OPT and re.sub(r'Typed+','',a) != re.sub(r'Typed+','',b): continue
```

- `Typed+` means "Type" followed by one or more `d`, so it never matches `Type0`.
- As a result, the sfx "family" only paired a vertex program and a fragment program that had the same name.
- The intent was `Type[0-9]+`.

| Rule set | sfx pipelines | Whole game | Samples at 100% |
|---|---|---|---|
| docs/30 (with the bug) | 189 | 2975 (432 PS, 98 VS) | none |
| regex fixed | 932 | 3718 (448 PS, 100 VS) | Nexus; Palace deep, live and pal |

The Tutorial stays at 151/154. docs/30's numbers were reproduced before any change: 2975 pipelines, and 157/158 for the Nexus.

### 1.2 Rules v3

Each change came from a miss in a sample or in the live cache. The table shows how many pipelines each change adds to the whole-game set. It also shows how many live records that change alone covers, out of 616.

| Change | What it adds | Pipelines | Live records |
|---|---|---|---|
| sfx regex fixed | VS × FP pairs within a family (Type0 with Type1, ...) | +743 | +2 |
| `--fade` (already in predict2.py, off in docs/30) | Alpha-blended copies of opaque materials (depth test on, depth write off), with both the `DS_Phn_*` and `DS_Gst_*` vertex programs. Light type 1 only. | +264 | +14 |
| `--fadeall` | The same, for every light type | +444 | +2 |
| `--sub` (new) | A third sfx blend: reverse subtract (SRC_ALPHA, ONE, REV_SUBTRACT) | +468 | +1 |
| `--ghostfx` (new) | `DS_Ghost_Skin` and `DS_Ghost_Tod` × `DS_Ghost`; add or alpha; cull none or back | +16 | +4 |
| `--menufam` (new) | The menu and debug-font programs in every pairing; add or alpha; depth off or on | +152 | +1 |

Cumulative, on the live cache:

| Rule set | Whole game | Live records covered |
|---|---|---|
| docs/30 | 2975 | 580 (94.2%) |
| + regex fix | 3718 | 582 (94.5%) |
| + fade | 3982 | 596 (96.8%) |
| + fadeall | 4426 | 598 (97.1%) |
| v3 (+ sub, ghostfx, menufam) | 5062 | 604 (98.1%) |

Per area, v3 predicts 2738-3954 pipelines (docs/30: 1233-1987). 2522 of the 5062 use the fetch-form vertex shader and 2540 the plain one. Strip meshes get both, because the runtime chooses per draw.

### 1.3 Tiers

Tiers group the rules by how often their pipelines get used. Each pipeline goes in the first tier whose rules predict it. Coverage is cumulative: it counts everything up to and including that tier.

| Tier | Rules | Pipelines | Running total | Live cache | 6 samples |
|---|---|---|---|---|---|
| 1 | table, menu, shadow, material | 1756 | 1756 | 68.3% | 60.1-94.2% |
| 2 | ghost, ghostfx | 952 | 2708 | 88.6% | 93.5-98.1% |
| 3 | fade | 954 | 3662 | 97.1% | 98.6-100% |
| 4 | sfx | 1400 | 5062 | 98.1% | 100% |

Per sample, after tiers 1 / 2 / 3 / 4:

| Sample | Tier 1 | Tier 2 | Tier 3 | Tier 4 |
|---|---|---|---|---|
| Nexus | 60.1 | 98.1 | 98.7 | 100 |
| Tutorial | 93.5 | 93.5 | 99.4 | 100 |
| Palace deep | 84.9 | 97.2 | 98.6 | 100 |
| Palace live | 89.1 | 97.3 | 98.9 | 100 |
| Palace pal | 89.1 | 97.3 | 98.9 | 100 |
| Palace walk (round 1) | 94.2 | 94.2 | 100 | 100 |

The Nexus needs tier 2 because of the ghost twins.

For each rule: how many of its predicted pipelines were used in any of the 7 manifests. A pipeline can come from two rules, so these predicted counts add up to more than 5062.

| Rule | Used / predicted |
|---|---|
| table | 30/30 |
| shadow | 20/24 |
| material | 373/1550 |
| ghost | 123/936 |
| ghostfx | 4/16 |
| menu | 15/152 |
| fade | 70/1176 |
| sfx | 6/1400 |

The sfx tier is 28% of the set, and only 6 of its pipelines were used. It goes last; D2 is whether to keep it at all.

### 1.4 What v3 still misses

12 live records are not covered (records 565-576).
- All of them pair a `DS_Dbg_*_PntNum` vertex program with a `DS_Phn_*HemDir3` pixel shader. There are 2 such vertex programs, and 21 names share them.
- All are depth-only: no colour target, depth test and write on, LESS.
- They cull back or none, and come in both plain and fetch form.
- None of the 6 samples has them, and their source is unknown.

### 1.5 Other checks

- **Front face.** In all 1052 records of the 6 samples, FrontCounterClockwise is 0 with culling off and 1 with culling on (back or front). No two records differ only by it. (read) rsx_cull_key returns 0 when culling is off. So front face needs no dimension of its own.
- **No other-world data in the live cache.** No log shows an m03-m07 script loading.
  - 3 live records (43, 129 and 500) are predicted only through m05_00's model lists. They are alpha "fade" copies with the ghost vertex program and a `DS_Phn_Dif___Bmp…HemDir3` pixel shader.
  - That points to a model that the Palace and Nexus lists miss (player or object models, probably). It is not a visit.
  - So the live cache cannot stand in for step 0.
- **Area order matters little.** m08_00 alone predicts 3432 of the 5062 (68%). The order of the tiers matters more.
- **sfx records seen.** There are 8 distinct ones over the 6 samples.
  - Most use the plain VS; SimpleSpriteType2 also appears in fetch form.
  - Cull is none or front, blend is add or alpha, and the variant is conv or alt2.
  - The Distortion VS (Type0) pairs with FP Type1 and FP Type5. This is the case the regex fix adds.

## 2. Architecture

```
user's game files               (1) tools/des/pso_predict.py, once per install
  shaderbnd, mtd, msb, flver,   ------------------------------------------>  <cache>/pso_predict.tsv
  parts/obj/chr archives, EBOOT                                              programs by content hash + state

shaderbnd + EBOOT               (2) startup precompile (exists), after each exe update
                                ------------------------------------------>  <cache>/vs_5_0_*.dxbc, ps_5_0_*.dxbc
                                                                             + <cache>/shader_keys.tsv (new)

(3) runtime worker threads, after the manifest prebuild:
    pso_predict.tsv + shader_keys.tsv -> .dxbc -> vp_fp_desc -> pso_desc_key
    -> skip what the manifest or the library has -> build at low priority -> pso_library.bin
    draw: a library hit on a predicted pipeline -> pso_manifest_add (new)
```

**Why the predictor does not write cache keys.**
- A cache key is an FNV hash of the HLSL text (read: shader_key). Any change to the decompiler's output changes it, and any exe update can do that.
- A program's content hash and form do not change.
- The exe turns them into cache keys at startup, from a table that its own precompile writes.
- So the predictor runs once per install, not once per build.

### 2.1 The predictor: tools/des/pso_predict.py

- **Language.** Python 3, standard library only (zlib for DCX). That keeps the patcher rule: nothing to install beyond what the build already needs.
- **Inputs.**
  - The USRDIR of the user's extracted game: `$PS3_VFS_ROOT/PS3_GAME/USRDIR`, as deslib.sh sets it.
  - The EBOOT: `work/game/EBOOT.ELF`, the file desrun.sh runs. It is needed for the program ids of the EBOOT's Cg programs.
- **What it reads.**
  - The shaderbnd archives: program names and bytes.
  - `mtd.mtdbnd`.
  - The model lists of the 25 area `.msb` files (m01-m08; not the m99 test maps).
  - Map flvers, partsbnd, objbnd and chr flvers.
- **Plain files and DCX.**
  - (read) This machine's extracted disc has a plain copy next to almost every `.dcx`. 12 `chrbnd.dcx` files have none. It also has loose `.flver` files next to the chrbnds.
  - (read) Its 3600 DCX files come in two kinds: 3365 EDGE (raw deflate in chunks) and 235 DFLT (one zlib stream). Python's zlib reads both.
  - Other dumps may lack the plain copies (inferred). So the reader opens the archives itself (BND3, plain or inside a DCX), and uses loose files only as a shortcut.
  - (read) The prework already needs the plain `.shaderbnd` files.
- **The code.**
  - It ports the spike's readers and rules v3 into one process for all areas. The spike's readers were written from scratch; SoulsFormats (GPL-3.0) was only a format reference (docs/30).
  - The spike runs one process per area: 1.4 s for m01 (measured), so about 35 s for all 25 (inferred). One process shares the archive parsing.
- **Gaps to fix in this step.**
  - 6 meshes use `ds_item[difspcbmp].mtd`, which the spike cannot resolve.
  - The spike reads chr flvers only as loose files.
- **Output.** `<cache>/pso_predict.tsv`, sorted by tier. Within a tier, the rows that more areas predict come first.
- **`--check [cache dir]`** scores that cache's `pso_manifest.bin` against the list.
  - It prints coverage per tier, and the records seen but not predicted, with their program names.
  - It hashes the cache's `.dxbc` files to map the manifest's shaders to cache keys, as join.py does. Then it maps cache keys to programs with `shader_keys.tsv` (step 2), where the spike used mapper.c's `map_all.tsv`.
  - It replaces join.py, mapper.c, predict2.py's report and attrib.py.
- **`--max-tier N`** leaves out the higher tiers (D2).

**Format.** One pipeline per line, tab-separated; `#` starts a comment.

```
# pso_predict 1 rules=v3
# vs_prog vs_form ps_prog ps_form blend cmask cull depth nrt rtfmt tier origin vs_name ps_name
```

| Column | Meaning |
|---|---|
| vs_prog, ps_prog | FNV-1a 64 (hex) of the CgBinaryProgram: `totalSize` bytes (the word at offset 8), starting at its profile word. The prework hands the precompiler these same bytes, and ebscan.py cuts the EBOOT's programs the same way. |
| vs_form | `plain` or `fetch` |
| ps_form | `plain`, `conv`, `alt 1=d2c` or `alt 2=d2c`: the forms that add_program builds |
| blend, cull, depth | The runtime's packed keys (hex), exactly as rsx_blend_key, rsx_cull_key and rsx_depth_key build them |
| cmask | Colour write mask, 0-15. Use 15 when nrt is 0, because it does not reach the desc then. |
| nrt, rtfmt | Number of colour targets (0-4) and their DXGI format (28 = R8G8B8A8_UNORM, 41 = R32_FLOAT) |
| tier | 1-4 |
| origin, vs_name, ps_name | The rule and the program names, for reports only |

Because the state is stored in packed form, the runtime calls the desc helper with exactly the arguments that `vp_get_fp_pso` gets. There is no second translation to keep in sync.

Packing examples (read from the desc code in vp_get_fp_pso):

| State | Packed value |
|---|---|
| Opaque blend | 0 |
| Alpha blend | 0x0126298B |
| Add blend | 0x0122288B |
| Sub blend | 0x0162288B |
| Depth: opaque (test, write, LESS_EQUAL) | 0xF |
| Depth: transparent (test, no write) | 0xD |
| Depth: shadow (test, write, LESS) | 0x7 |
| Depth: off | 0 |
| Cull back, CCW front | 5 |
| Cull front, CCW front | 7 |
| Cull none | 0 |

### 2.2 The precompile step

What exists (read):
- **The prework.** `ps3_title_prework` (port/des_prework.cpp) runs after the ELF is loaded and before the game starts, behind a progress bar.
  - It queues every program in `USRDIR/shader/*.shaderbnd` into ps3recomp's rsx_shader_precompile.
  - A stamp skips the pass when nothing changed. The stamp covers the archives, the exe's link time and image size, the texture kinds and fp_variants.txt.
  - The pass takes about 20 s the first time and 4 s over a full cache (its header comment).
- **What add_program queues.**
  - Vertex programs twice: plain and fetch. Fetch came in 13f5fb3 (2026-10-17): `rsx_d3d12_shader_hlsl(2, ...)` calls rsx_vp_decompile_fetch.
  - Fragment programs: plain; conv (kUnits); each alt (kAlts) whose unit the program uses; and the variants in fp_variants.txt.
- **Program count.** The spike counted 1421 programs in the archives: 161 vertex and 1260 fragment (measured).

What to add:
1. **The key table.**
   - `queue()` already has the HLSL text. It also computes the cache key there (shader_key: FNV-1a 64 over the profile, a 0xFF byte and the text).
   - It records `prog_id form key label` for every text, including the ones it drops as duplicates.
   - At the end of a full pass it writes `<cache>/shader_keys.tsv`, through a temp file and a rename like the other cache files.
   - About 7000 lines (inferred: about 1429 programs × about 5 forms).
2. **The EBOOT's programs.**
   - ebscan.py finds 8 Cg programs in `work/game/EBOOT.ELF`, at file offsets 0x019caeb0-0x019cbba0 (measured).
   - The manifests use 4 of them, the hand table's display passes: vertex 0x019cb0d0 and 0x019cbba0, fragment 0x019cb060 and 0x019cb4f0.
   - Queue all 8; they cost almost nothing.
   - boot_main has the ELF path, which it passes to ppu_load_elf, but keeps it in a local. Export it as one global.
   - The prework scans that file as ebscan.py does: profile 0x1B5B or 0x1B5C at a 4-byte boundary, with the sizes inside the program.
3. **The stamp.**
   - A missing `shader_keys.tsv` forces a pass.
   - Keep the file itself out of the stamp: the pass rewrites it, so the stamp would never match again.
   - Keep `pso_predict.tsv` out too: the pass does not depend on it.

### 2.3 The runtime

All of this goes in `rsx_d3d12_backend.c`, next to the PR #34 code.

**One desc helper.** Move vp_get_fp_pso's desc block into a helper. The block starts at line 4133 (`D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;`) and ends at `pd.SampleDesc.Count = 1;`, about 80 lines.

```c
static void vp_fp_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, D3D12_INPUT_ELEMENT_DESC il[16],
                       const void* vs, SIZE_T vs_len, const void* ps, SIZE_T ps_len,
                       const void* gs, SIZE_T gs_len, int fetch, u32 blend, int nrt,
                       DXGI_FORMAT rtfmt, u32 cmask, u32 cull, u32 depth);
```

- vp_get_fp_pso and the predicted phase both call it.
- It keeps the CULL_OFF, CULL_FLIP, CMASK_FORCE and DEPTH_OFF knobs, so both sides still agree when one of them is set.
- The FP_* knobs change the pixel shader text instead. So the predicted phase does nothing while any knob in vp_pso_knobs_check's list has a non-default value, and logs one line saying so.

**The predicted phase.** What happens today (read):
- Once the root signature exists, pso_lib_tick starts CPUs/4 threads (1-4) at THREAD_PRIORITY_BELOW_NORMAL.
- They build the manifest records that the library lacks.
- The last one prints `pipeline prebuild: N of M built in T ms`.
- Saves wait for them.

After that line, the predicted phase runs:
1. One thread reads `shader_keys.tsv` into a map: (prog_id, form) → key. Then it reads `pso_predict.tsv`. Rows that name an unknown program or form are counted and dropped.
2. It loads each distinct `.dxbc` once: about 560 files. shader_cache_load checks each one.
3. For each row, it fills a desc with vp_fp_desc and takes pso_desc_key. That name is what a draw with this state looks up.
4. It sorts the rows into three groups:
   - already in the manifest (pso_names_has, taken under s_pso_lib_lock, because the render thread grows that set);
   - already in the library (`pso_predict.done`, below);
   - to build.

   Then it prints one line:
   `[D3D12] pipeline prediction: 5062 listed, 4458 to build; 604 of 616 manifest pipelines are in the list`

   The last figure checks steps 1-3 end to end, with no gameplay. It must equal what `pso_predict.py --check` reports for the same cache.
5. The same number of threads, at THREAD_PRIORITY_LOWEST, build the rest in tier order:
   - CreateGraphicsPipelineState, then StorePipeline under s_pso_lib_lock.
   - Just before each build, they check the manifest set again, because a draw may have compiled it in the meantime.
   - A StorePipeline that fails because the name exists is not an error.
6. At the end it prints: `[D3D12] pipeline prediction: 4458 built in T ms (K skipped: shader missing)`.
7. s_pso_pre_abort stops it at shutdown, as it stops the manifest phase today.

**Already built?**
- The library has no "contains" call. LoadGraphicsPipeline needs the whole desc and creates the PSO.
- So each library save also writes `<cache>/pso_predict.done`: the sorted 8-byte names of the predicted pipelines that save holds.
- The file is ignored when the library did not load: a new driver rebuilds everything.
- Names hash the content. So a new `pso_predict.tsv` keeps every pipeline it shares with the old one.

**Saves.**
- Today (read), a save waits for the prebuild threads. Then it happens after 120 presents with no new pipeline, or with 256 pending.
- The predicted phase can run for 10-30 s, so it must not hold saves back.
- Its stores must not count toward the 256 either. 4458-5062 stores would mean 17-20 saves of a library that grows toward 88 MB.
- So predicted stores are counted separately, and the phase asks for one save at the end of each tier.
- Live pipelines keep today's rule. The shutdown save (rsx_d3d12_backend_shutdown) keeps whatever was built.

**Promotion on use.**
- Today (read), a library hit in pso_create_cached returns at once. Only a live compile calls pso_manifest_add.
- So a pipeline that prediction built would never reach the manifest. After a driver change it would come back only at prediction priority.
- The change: on a hit whose name is not in the manifest set, call pso_manifest_add. That is the same file writes as after a live compile, without the compile.
- The manifest stays the record of what this user's game drew.

**Counters.** The save line gets two more counts:

`draws so far: 1200 loaded (40 predicted), 3 compiled (1 listed, not built yet)`

- "compiled" is the number the test plan watches.
- "listed, not built yet" separates scheduling (the phase had not got there yet) from real gaps.

**Not covered: pipelines with a geometry shader.**
- With Render resolution other than 100%, depth-test-off draws on scaled targets get uvclamp_gs (docs/37).
- pso_manifest_add skips any desc with a GS (read), and the predictor does not predict them. See section 8.

### 2.4 Files

All of these live in the shader cache dir (PS3_SHADER_CACHE; deslib.sh defaults it to gamedata/shader_cache). A scratch cache for a test carries its own copies. All of it is derived from the game, so it stays under gamedata/ and is never committed.

| File | Written by | Read by | Format |
|---|---|---|---|
| `pso_predict.tsv` | pso_predict.py, once per install | runtime | text (2.1), about 0.6 MB (inferred) |
| `shader_keys.tsv` | the precompile pass, after each exe update | runtime, `--check` | text: `prog_id form key label` |
| `pso_predict.done` | each library save | runtime | sorted u64 names, little-endian; at most 40 KB |
| `pso_library.bin`, `pso_library.n`, `pso_manifest.bin`, `pso_blobs/` | as today (PR #34) | | unchanged |

### 2.5 Knobs

| Knob | Default | Effect |
|---|---|---|
| PS3_PSO_PREDICT | on when both .tsv files exist | `0` turns it off; a path uses another list |
| PS3_PSO_PREBUILD | 1 | `0` now skips the predicted phase too, so cold runs (section 7) stay cold |
| PS3_PSO_LIBRARY | 1 | `0` turns everything off, as today |
| pso_predict.py `--max-tier N` | 4 | drops the higher tiers (D2) |
| PS3_PSO_PARALLEL (step B) | on | `0` compiles misses one at a time, as today |
| PS3_PSO_ASYNC (option C, if built) | 0 | opt-in; regress.sh forces 0 |

### 2.6 When the driver, GPU, exe or game files change

- **Driver or GPU.**
  - pso_library.bin fails to load (read: "is from another driver or GPU; rebuilding it").
  - The manifest phase rebuilds every manifest record first, then the predicted phase builds the rest. pso_predict.done is ignored.
  - That is about 616 + 4450 builds: 9-25 s of background work on 4 idle threads (inferred).
- **Exe update.**
  - The stamp holds the exe's link time, so the precompile runs again (about 4 s over a full cache) and rewrites shader_keys.tsv.
  - Rows whose shader text changed get new names and are built again. The rest stay.
  - The old pipelines stay in the library, unused (section 8).
- **Other game files.** Run pso_predict.py again. Unknown programs are counted and skipped, and nothing breaks.

### 2.7 Failure modes

| Failure | Effect | Where it shows |
|---|---|---|
| Names never match (the helper drifted, or a form is mislabelled) | Background compiles are wasted; draws compile live, as today | Startup line: "0 of 616 manifest pipelines are in the list" |
| shader_keys.tsv missing (the pass was skipped or cancelled) | Phase skipped | One log line |
| A row's .dxbc is missing (FXC failed) | Row skipped | "K skipped: shader missing" |
| pso_predict.tsv made from other game files | Unknown programs skipped | Startup line |
| pso_predict.done torn | Ignored; those pipelines are built again, and StorePipeline refuses the duplicates | Nothing |
| A pipeline knob is set | Phase skipped | One log line |
| CreateGraphicsPipelineState fails | Counted; the phase goes on | End line |
| Shutdown in the middle of the phase | It stops; the shutdown save keeps what was built | The next start builds the rest |
| Low RAM | Use `--max-tier` | (read) The library blob stays in RAM while the library lives, and each save allocates a second buffer of the same size. About 88 MB each at v3 (inferred). |

## 3. The fallback: compile on first use, off the render thread

What happens today (read):
- render_frame's pre-pass (around line 8284) calls vp_get_fp_pso for every draw with a guest FP. So all of a frame's pipelines exist before the draw loop (around line 8695) records anything.
- A miss compiles right there, one after another:
  - FXC first, if the pixel shader text is new (rare since the precompile);
  - then the driver: 7-20 ms without its own cache, 0.1 ms with it (the comment above the library code).
- A NULL result draws with the hardcoded atlas/colour pipeline (`vpso`). It does not skip the draw.

**B. Parallel pre-pass (recommended, after prediction).**
- In the pre-pass, a library miss does not compile. It records its desc in a batch, keeps its shader blobs alive, and marks the cache entry as pending.
- After the pre-pass loop, worker threads compile the batch (ID3D12Device methods are free-threaded). The render thread waits for all of them. Then it stores them in the library, adds them to the manifest and fills in the entries.
- The draw loop finds every pipeline ready. The frames are the same as today; only the wait is shorter. (inferred) A burst of 12 misses at 7-20 ms takes 84-240 ms today and 21-60 ms on 4 workers.
- The decompile stays on the render thread, because it reads guest memory and uses static buffers. FXC misses can join the batch later.
- A refinement: record draws while the batch compiles, and wait only at the first draw that needs a pending pipeline. This is still deterministic.
- 1.5-2 days.

**C. Async skip (opt-in only, if at all).**
- A miss returns "pending" and the draw is skipped. Drawing it with vpso would show wrong colours. The pipeline compiles in the background, and a later frame draws it.
- Some draws still wait: those with the depth test off (post, UI, full screen) and depth-only shadow draws. Skipping one of those for a frame makes the whole screen or the shadows flash.
- With C, what a frame shows depends on compile timing. That breaks regress.sh's frame comparison and the "guest state, not timing" rule.
- So it is never on by default, and regress forces it off.
- 2-3 days, plus a visual review.

RPCS3 offers the same choices (read). Its `shader_mode` setting has four values:
- recompiler;
- async_recompiler, which skips draws;
- async_with_interpreter, which draws with an interpreter ubershader in the meantime;
- interpreter_only.

An RSX fragment ubershader in HLSL would take weeks, so it is not proposed.

**How they combine.**
- The manifest covers what this user drew before.
- Prediction covers what they have not drawn yet, and a new driver or a new install. The target is under about 2% of a session's pipelines left to compile.
- B shortens the hitch for that remainder.
- C, if built, hides the hitch at the price of pop-in.

## 4. Effort

| Step | Work | Days |
|---|---|---|
| 0 | Analyse the new-world sample, and fold any new family into v3 | 0.5 |
| 1 | pso_predict.py: archive and DCX readers (EDGE, DFLT); flver, mtd and msb readers; rules v3 with tiers; one process; `--check`; `--max-tier`; the two gaps | 2-3 |
| 2 | The key table in rsx_shader_precompile; the EBOOT scan in the prework (and the ELF path from boot_main); the stamp rule | 1-1.5 |
| 3 | The vp_fp_desc refactor (0.5); the predicted phase (1); pso_predict.done and the save rule (0.5); promotion, counters and the startup line (0.5) | 2-3 |
| 4 | The test plan (section 6) | 1 |
| Core | | 6.5-8.5 |
| B | Parallel pre-pass | 1.5-2 |
| C | Async skip, opt-in | 2-3, plus a review |
| Option | Predict plain vs fetch per mesh (see below) | about 2 |
| Option | GS pipelines in the manifest (a record format that holds the GS) | about 0.5 |

**The plain vs fetch option (inferred).**
- upload_strip_vp_fetch falls back to the plain path in three cases (read):
  - a vertex type it does not take;
  - a sparse index range;
  - the frame's index buffer is full.
- The first two come from the mesh, so a rule could drop many plain rows.
- The third happens at run time, so some plain draws stay unpredictable.

## 5. Risks

1. **New families in other worlds.** Other worlds may use families that the 3 sampled areas do not. This is the main risk, and step 0 measures it. A gap means a hitch, as today, not a failure. Promotion covers it from the next session on.
2. **In-sample fit.** v3 was fitted on the manifests it is scored against. Expect a few points less on new data.
3. **Library size and RAM.**
   - About 88 MB at v3 (inferred), from 17.4 KB per pipeline (measured: 10,715,422 bytes for 616 pipelines).
   - The library stays in RAM while it lives, and each save needs a second buffer of the same size.
   - Tiers cap it.
4. **Startup.** pso_lib_open reads the library on the render thread when the device is created. (inferred) That is 0.1-0.3 s for 88 MB. It could move to a thread later.
5. **Background CPU.** Lowest priority still uses cores, and battery on laptops. PS3_PSO_PREDICT=0 turns it off.
6. **Hash drift.** A later desc change that bypasses vp_fp_desc breaks the names without any error. The startup line shows it, and test T3 checks it.
7. **Waste.** The live cache uses 604 of the 5062 (1 in 8.4). Most of the rest are plain/fetch pairs and shadow and point-light variants, which the runtime decides per draw.
8. **Large libraries.** Libraries this big are untested here (inferred risk). PR #34 already depends on ID3D12PipelineLibrary.

## 6. Test plan (counts only, no fps)

| # | Test | Pass |
|---|---|---|
| T1 | `pso_predict.py --check` on the 6 sample manifests and the live cache | 100% on all 6 and 604/616 on the live cache: the port matches the spike |
| T2 | shader_keys.tsv against the spike's map_all.tsv, for the (program, form) rows both have | Identical keys. The decompilers' output has not changed since map_all.tsv was made (checked against fork/master f15a4f0): rsx_vp_decompiler.c is unchanged, rsx_fp_decompiler.c only gained a log line, and fp_build_hlsl is identical. |
| T3 | Start on a copy of the live cache, to the title screen. Exit on the prediction line. | "604 of 616 manifest pipelines are in the list", the same figure as T1. This checks vp_fp_desc and the names end to end. |
| T4 | The pinned regression save and the regress walk, with a scratch cache seeded with the .dxbc files and the two .tsv files (no library, no manifest) | At exit, "compiled" minus "listed, not built yet" is at most 2% of the walk's pipelines (at most 3 of about 155) |
| T5 | Driver change: in a scratch copy of the cache, remove pso_library.bin. Start, exit after the "built" line, then start again. | 1st start: "new"; all manifest records and listed pipelines are built. 2nd start: "0 to build", and the predicted phase has nothing left to build (pso_predict.done works). |
| T6 | regress.sh with prediction on and off | Frames identical to the baseline |
| T7 | Each of PS3_PSO_PREDICT=0, PS3_PSO_PREBUILD=0 and FP_IDCOLOR=1 | No predicted phase, and one log line, in each case |
| T8 | Out of sample: step 0, then the first real sessions | The save line's "compiled" stays at a few per new area |

Every test exits on a log line, not after a fixed time. None of them compares frame rates.

## 7. The missing sample: what the maintainer does

This is the first out-of-sample test. Every save on this machine loads the Nexus, the Tutorial or Palace 1-1 (docs/30), and the live cache has no other-world data (1.5).

1. With your normal save, get into Stonefang, Latria, Storms or Valley by normal play. Quit the game there, so the save loads in that world. In Demon's Souls, the other four archstones open once the 1-1 boss (Phalanx) is dead.
2. Run this from the repo root. It copies the existing shaders into a scratch shader cache (about 25 s for 3637 files). Then it starts the game with an empty pipeline library on purpose, at Render resolution 100% for this run only:

```
& "C:\Program Files\Git\bin\bash.exe" -c "mkdir -p work/tmp/pso_world/cache; cp -n gamedata/shader_cache/vs_5_0_*.dxbc gamedata/shader_cache/ps_5_0_*.dxbc work/tmp/pso_world/cache/; tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 RSX_SCALE=100 PS3_PSO_PREBUILD=0 PS3_SHADER_CACHE=work/tmp/pso_world/cache"
```

3. Play 5-10 minutes in that world.
   - Walk a good stretch, fight, and use an item or a spell.
   - If it suits your playthrough, include a death: soul form draws with other shaders.
   - Do not go back to the Nexus.
   - Expect hitches: every pipeline compiles live in this run.

   Then close the game.
4. Run this. It maps the session's shaders, then scores the prediction for each m03-m07 area (about 1.5 s per area):

```
& "C:\Program Files\Git\bin\bash.exe" -c '. tools/des/deslib.sh; des_env_defaults; IFS=; W=work/tmp/pso_world; cp $W/cache/pso_manifest.bin $W/; cp -n work/tmp/pso_spike/map_all.tsv $W/; python -B work/tmp/pso_spike/py/join.py $W $W/cache; for a in m03_00_00_00 m03_01_00_00 m03_01_00_99 m03_02_00_00 m03_03_00_00 m04_00_00_00 m04_01_00_00 m04_02_00_00 m05_00_00_00 m05_01_00_00 m05_02_00_00 m06_00_00_00 m06_01_00_00 m06_02_00_00 m07_00_00_00 m07_01_00_00 m07_02_00_00; do python -B work/tmp/pso_spike/py/predict2.py $W $PS3_VFS_ROOT/PS3_GAME/USRDIR $a --parts --gst --sfx --sfxfam --fade --fadeall > $W/pred_$a.txt; grep -e ^area -e ^seen $W/pred_$a.txt; done'
```

5. Read the best `seen N covered M` line; that is the area you played.
   - If M/N is 97% or more: build (D1).
   - If it is below: `work/tmp/pso_world/pred_<area>.txt` lists the records that were seen but not predicted, by program name. That shows the missing family.

Notes:
- **It is a lower bound.** The analysis uses the spike's predict2.py as it is, plus `--fade --fadeall`. It lacks the regex fix and the three new v3 rules. On the live cache, this rule set covers 596 of 616 and v3 covers 604, so v3 scores about 1.3 points higher.
- **Nothing outside the scratch dir changes.** Both commands write only under `work/tmp/pso_world`, which is scratch and untracked; delete it afterwards. The save is your normal one: play.sh does not swap saves.
- **Why RSX_SCALE=100.** On the command line, it pins Render resolution for this run, and settings.ini cannot override it. Other scales add pipelines with a geometry shader, which the manifest does not record.
- **The map file's age.** `map_all.tsv` was made on 2026-10-17 by the spike's mapper. The decompilers' output has not changed since (see T2). If a later merge changes it, many records show up as unmapped, and step 2's `shader_keys.tsv` is needed first.

## 8. Related gaps (not in this plan)

- **GS pipelines.** Pipelines with a geometry shader (RSX_SCALE uvclamp_gs) never reach the manifest. After a driver change, each one compiles live again, once. A record format that holds the GS (about 0.5 day) would fix it.
- **Stale entries.** After a decompiler change, old manifest records still prebuild at every driver change, even though their names never match a draw. Old pipelines also stay in the library. Nothing removes them. A cleanup is separate work: either drop records not loaded in N sessions, or rebuild the library from the recent records.
- **One unresolved MTD.** `ds_item[difspcbmp].mtd` (6 meshes) is unresolved in the spike; step 1 fixes it.

## 9. Prototype scripts

These are in the lane scratch dir, not in the repo. Step 1 folds them into pso_predict.py.

| Script | What it does |
|---|---|
| predict2fix.py | predict2.py with the regex fixed |
| predict3.py, predict3o.py | Rules v3 (`--sub`, `--ghostfx`, `--menufam`); predict3o.py also records each pipeline's rule |
| union.py | Whole-game union size for a directory of per-area predictions |
| attrib.py | Per-record coverage and area attribution against a manifest |
| origin.py, tiers.py | Coverage per rule and per tier (section 1.3) |
| fccw.py | The front-face check (1.5) |
| sfxseen.py, names2.py | The sfx records seen; all the names that share a record's programs |
