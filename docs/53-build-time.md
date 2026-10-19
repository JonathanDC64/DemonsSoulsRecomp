# 53. Build time: where it goes and how to cut it

Date: 2026-10-18. Lane f3-buildtime (fan-out 3).

This measures each step of a from-scratch build of des_boot (lift, SPU and
libsre lifts, configure, each lifted chunk, link, shader precompile), then
tries the ideas from the request and ranks them by measured gain against
effort. Three small branches are prepared (section 13). Everything bigger is a
design that waits for your OK.

**[measured]** means timed or compared here today. **[read]** means seen in
code, a log or a doc. **[inferred]** means my conclusion, not tested.

## Summary

- A from-scratch build is about 43 minutes of CPU time without `/Zi`. 82% of
  it is the 14 lifted PPU chunks (2,128 s). Everything that is not
  game-derived (the runtime library, the UI, RmlUi, FreeType, the port's
  glue) is 13%. The lifts and configure add about 75 s of wall time.
  [measured]
- Master compiles with `/Zi`, but the link makes no PDB, so the debug
  information is thrown away. It costs 29 to 42% more CPU per chunk and 2.5
  to 3 times the memory: 5.8 to 6.9 GB per chunk job instead of 2.4 GB.
  Branch `build/no-debug-info` drops it. On 4 workers the build goes from
  about 13.0 to 10.3 minutes. On a 16 GB PC it is the difference between 2
  and 5 chunk jobs at once. [measured, simulated, inferred]
- In a fresh build folder, ninja first compiles about 280 small host jobs,
  then the chunks in file order. The most expensive chunk (008, 261 s) then
  starts in the second wave on 8 workers: 7.2 minutes, against 5.3 if the
  biggest chunks went first. On 16 workers chunk 008 alone sets the time
  (4.4 to 4.6 minutes). [measured CPU, simulated wall time]
- Smaller chunks (`--chunk-lines 150000`, 53 chunks) fix both problems. In a
  build from such a lift the chunks took no more CPU (1,904 s against
  2,128 s), the largest took 102 s, and a chunk job peaked at 0.7 GB
  (1.2 GB at most). Simulated in ninja's order: 9.6, 5.4 and 3.1 minutes on
  4, 8 and 16 workers, against 10.6, 7.2 and 4.6 today. They change which
  functions share a translation unit, so they need a frame-rate A/B (run
  request R3) before the recipe changes. [measured, simulated]
- A re-lift rewrote every chunk, so the next build recompiled all 14 even
  when one hook changed. Branch `tools/lifter-incremental-output` (ps3recomp)
  writes only the files that changed and moves the hook declarations out of
  the header. A hook change now rebuilds 1 or 2 chunks. [measured]
- The boot patch after the lift can fail with "Permission denied" while the
  VS Code C/C++ extension parses the chunk the lifter just wrote. It happened
  in 3 of 3 re-lifts where chunk 005 was the newest file. Branch
  `tools/boot-patch-retry` (port) makes the script wait up to 30 s for the
  file. Excluding `work/` from the extension also avoids it. [measured]
- No gain: precompiled headers (at most 0.3%), a real `-O1` (none), `/O1`
  (49% slower, it is `-Os`), and so hot and cold optimization levels.
  [measured]

## Setup and method

- PC: Ryzen 7 9800X3D, 8 cores and 16 threads, 31 GB. clang-cl 22.1.8,
  lld-link, ninja 1.13.2, Python 3.13. [measured, read]
- Sources: port origin/master 495d5a7, ps3recomp fork/master f15a4f0, and the
  shared lift `work/recomp_l2` (master's 19 insn hooks). [read]
- CPU time per job is user plus kernel time from GetProcessTimes. A small
  launcher (`CMAKE_<LANG>_COMPILER_LAUNCHER`) logged it for every compile,
  with the job's peak working set and later its peak commit. lld-link runs as
  a child of `cmake -E vs_link_exe`, so its CPU time is not logged; its wall
  time is. [measured]
- The PC was busy for all of it: the user's other project, and up to two
  builds of other lanes. My builds ran at `-j3`. Wall times are indicative
  only. The `.ninja_log` edge times are wall time per job and overstate CPU
  time under load: chunk 009 shows 464 s in the l2 build's log but took 228 s
  of CPU alone. So this doc compares CPU time and predicts wall time on 4, 8
  and 16 workers by list scheduling those CPU times (section 7.2). [measured]
- Load also inflates CPU time itself, because jobs share cores (SMT) and
  memory bandwidth. Chunks compiled alone against the same chunks in the
  loaded build: 000 170 s against 174 s, 001 151 against 179, 004 143
  against 144, 009 161 against 200. So 1 to 24% more in the build. The
  16-worker predictions assume 16 real cores. On 8 cores with SMT they are
  optimistic. [measured, inferred]
- Peak working set is trimmed when the PC runs short of memory, so for
  memory this doc uses peak commit where it has it. [measured]
- Observed on the side: while two lanes ran `-j3` builds with `/Zi` at once
  (six chunk compiles), available memory sat between 0.02 and 1 GB for over
  20 minutes and the PC was paging. Section 4 is why. [measured]

## 1. The whole pipeline

| Step | Wall time | CPU time | Notes |
|---|---|---|---|
| PPU lift (`ppu_lifter.py -j 16`) | 39.1 s | | analysis 18.5 s (one thread), lifting 11 s, mid-function wrappers 5.5 s, writing 3.4 s |
| Boot patch (`apply_boot_patches.py`) | 1.3 s | | reads every chunk up to the patched one |
| NID table (`gen_hle_nids.py`) | 0.4 s | | |
| SPU lift (`build_spu.py`) | 17.6 s | | one thread |
| libsre lift (`build_libsre.sh`) | 9.6 s | | |
| ps3recomp configure and build (runtime library, `rsx_shader_precompile`) | about 14 s | 129 s | 170 jobs at `-j16` |
| Port configure | 4 to 5 s | | FetchContent sources already on disk |
| des_boot compile | 1,019 s at `-j3`, loaded | 2,461 s | 322 jobs, without `/Zi` |
| Link | 1.7 s | not logged | lld-link |
| Shader precompile, first start | 28 s | | 3,535 shaders, 15 threads (docs/32) |

[measured; the shader line is read]

- The lifts are reproducible. Two PPU lifts with the same inputs, mine at
  `-j16` and another lane's, gave byte-identical files (14 chunks, header,
  NID table). The SPU and libsre lifts into scratch folders matched the
  outputs already in the tree, file for file. Re-lifts at `-j8` reproduced
  the earlier lift's files except the ones the change touched. That answers
  docs/45 R5. [measured]
- Everything but the compile is about 75 s. The compile is the build.

## 2. The des_boot compile

Without `/Zi`, from master's sources, `-j3` on the loaded PC. [measured]

| Part | Jobs | CPU | Share | Largest job | Peak working set of one job |
|---|---|---|---|---|---|
| lifted PPU chunks | 14 | 2,128 s | 86.5% | 261 s | 2.2 GB (2.4 GB commit, alone) |
| RmlUi | 202 | 142 s | 5.8% | 1.8 s | 0.2 GB |
| lifted SPU images | 17 | 92 s | 3.7% | 15.9 s | 0.3 GB |
| UI (des_ui) | 33 | 53 s | 2.1% | 3.5 s | 0.4 GB |
| lifted libsre | 3 | 27 s | 1.1% | 26.8 s | 1.7 GB |
| FreeType | 42 | 9 s | 0.4% | 0.8 s | 0.1 GB |
| port glue | 5 | 5 s | 0.2% | 1.5 s | 0.2 GB |
| runtime sources compiled into des_boot | 5 | 5 s | 0.2% | 1.3 s | 0.2 GB |
| NID table | 1 | 0.1 s | 0% | 0.1 s | 0.1 GB |
| All | 322 | 2,461 s (41.0 min) | | | |

- Chunk CPU times: 000 174 s, 001 179, 002 217, 003 150, 004 144, 005 150,
  006 165, 007 170, 008 261, 009 200, 010 166, 011 142, 012 7, 013 3.
  [measured]
- Game-derived code (chunks, SPU, libsre, NID table) is 91.3% of the
  compile. Host code is 8.7%, plus the ps3recomp runtime library (129 s).
  [measured]
- Ninja's order in a fresh build folder: all 277 UI, RmlUi and FreeType jobs
  first (84 s at `-j3`), then chunks 000 to 013 in file order, then the SPU images
  and libsre. [measured] My reading of why: ninja schedules by the critical
  path, and with no `.ninja_log` every job weighs the same. The libraries'
  objects (UI, RmlUi, FreeType) sit one step further from the exe (object,
  `.lib`, exe) than a chunk's, so they go first, and the chunks follow in
  file order. A rebuild in the same folder has the log and should start the
  longest jobs first. [inferred]

## 3. Inside one chunk (-ftime-trace)

Chunk 009 (580k lines, 6,552 functions), alone on the PC, with des_boot's
flags plus `-ftime-trace`. [measured]

| Phase | No /Zi | With /Zi |
|---|---|---|
| Whole compile | 162.7 s | 231.5 s |
| Frontend (parse) | 9.2 s | 12.5 s |
| Optimizer | 84.9 s | 107.2 s |
| Code generation | 68.6 s | 111.8 s |

- The inliner's call-graph pass (CGSCC pipeline) is the biggest part of the
  optimizer: 52.4 s. Then InstCombine 16.2 s, the SLP vectorizer 10.6 s, GVN
  9.9 s, loop passes 13.7 s. [measured]
- No single function dominates: 886 functions are over 10 ms and add up to
  54.8 s; the slowest is func_0129F4B8 at 3.2 s. [measured]
- `/Zi` adds 26% to the optimizer (debug intrinsics) and 63% to code
  generation (CodeView), and kernel time went from 5 s to 40 s (memory).
  [measured]
- The frontend is 6% of the compile, and most of it is the chunk's own 580k
  lines, not the header (section 6). [measured]

## 4. /Zi: a quarter of the compile for nothing (branch prepared)

The link has no `/DEBUG` (`/machine:x64 /subsystem:console /MAP:des_boot.map`
in build.ninja), so lld writes no PDB and drops every `.debug$S`/`.debug$T`
section. The `/Zi` work in each compile is thrown away. Host backtraces
already go through `des_boot.map` (`tools/des/sym.py`). [read]

Each chunk alone on the PC: [measured]

| | Chunk 004 with /Zi | without | Chunk 009 with /Zi | without |
|---|---|---|---|---|
| CPU (user + kernel) | 184.9 s | 142.8 s | 228.2 s | 160.7 s |
| Of which kernel | 4.9 s | 3.7 s | 39.5 s | 5.0 s |
| Peak memory | 5.8 GB commit | 2.3 GB commit | 6.9 GB working set | 2.4 GB working set |
| Object size | 112 MB | 37 MB | 124 MB | 36 MB |

- So `/Zi` costs 29% (004) to 42% (009) more CPU. User time alone is +29%
  and +21%; the rest of 009's extra is kernel time from memory pressure.
  [measured]
- A whole build with `/Zi`, predicted from chunk 004's factor: about 52
  minutes of CPU instead of 41. [simulated]
- The code is nearly the same. Comparing every object of a full build with
  `/Zi` against one without, debug sections ignored (`coffcmp.py`): 13 of the
  14 chunks, the lifted libsre, the NID table and all SPU objects are
  byte-identical in code, data and relocations. Chunk 004 differs in 3 of its
  8,162 functions (`func_0096AD18`, `func_0096B6D0`, `func_0096B8A8`), each
  one byte longer with `/Zi`. Compiling chunk 004 again gives the same bytes
  both ways, so `/Zi` nudges clang's code generation; it is not noise. The
  other differing objects had different sources (the `/Zi` build was another
  lane's branch) or sit in another folder, which changes anonymous-namespace
  names and RTTI strings. Four UI objects compiled both ways in the same
  worktree: `/Zi` changes two functions in `modal.cpp` (`Modal::Modal` and
  `Modal::add_action`, same size, other instruction bytes) and the exception
  tables of a few objects; the rest differs only by the git hash the UI
  embeds. [measured]
- Memory matters more than CPU. With `/Zi`, four chunk jobs at once (the
  README's `--parallel 4`) can need over 20 GB. docs/45's "3 GB per job" rule
  holds only without `/Zi`. Section 7.3 has the numbers per PC size.
  [measured, inferred]
- Branch `build/no-debug-info` (port) drops `/Zi` by default and adds
  `DES_DEBUG_INFO=ON`, which compiles with `/Zi` and links `des_boot.pdb`. It
  keeps `/OPT:REF,ICF`, which `/DEBUG` would turn off, so that build links
  like the default one. See section 13.
- Side note: `port/build/des_boot.pdb` is from June 28 (251 MB). No build
  since has made a PDB, so it matches no exe, and `des_stage_exe`
  (`tools/des/deslib.sh`) copies it next to every staged exe. The crash
  reporter loads only an exactly matching PDB, so it is ignored. [read,
  measured]

## 5. Optimization levels do not cut build time

Chunk 009, no `/Zi`, alone on the PC. [measured]

| Flags | cc1 level | CPU | Peak | Object |
|---|---|---|---|---|
| `/O2` (today) | `-O3`, vectorizers on | 160.7 s | 2.4 GB | 36.2 MB |
| `/O2 /clang:-O1` | `-O1`, no vectorizers | 163.8 s | 2.3 GB | 35.5 MB |
| `/O1` | `-Os`, vectorizers on | 239.5 s | 2.4 GB | 33.2 MB |

- clang-cl's `/O2` is `-O3` (read from `clang-cl -###`), and its `/O1` is
  `-Os`, not `-O1`. [read]
- A real `-O1` saves nothing: less optimizer work leaves more code for code
  generation. `-Os` costs 49% more. [measured]
- So "hot functions at O2, cold at O1" would not shorten the build. Clang
  also has no per-function `-O1` (only `optnone`, `minsize` and `cold`), so it
  would need separate hot and cold chunks. Dropped. This also answers the
  build-time half of docs/45 R3. [measured, read]

## 6. Precompiled headers: at most 0.3%

- A file with only a chunk's preamble (`ppu_recomp.h` and what it includes,
  no functions) compiles in 0.49 s of CPU and 141 MB. That is the most a
  precompiled header could save per chunk. [measured]
- 14 chunks: 7 s of 2,461 s, 0.3%. With 53 smaller chunks: 26 s, 1%.
  [measured, inferred]
- The frontend's 9.2 s in chunk 009 is the chunk's own code. A PCH cannot
  help with that. Not worth the build complexity. [measured]

## 7. Chunk size and balance

### 7.1 Line count is a poor measure of cost

The lifter cuts a chunk every 600,000 lines. [read] The chunks then cost
very different amounts. [measured]

- Chunks 000 to 011 have 570k to 588k lines each and took 142 to 261 s.
  Chunk 008 took 261 s and chunk 004 144 s for nearly the same line count.
  Per thousand live lines that is 0.25 s (004) to 0.46 s (011). [measured]
- 12.1% of the lifted lines (907k of 7.5M) are dead code clang drops early.
  Most of it (862k lines) is in about 850 of the 1,024 tail-entry wrappers,
  which the lifter emits after all other functions: each one copies the rest
  of a function after an unconditional trampoline exit. They fill the second
  half of chunk 011 and all of chunks 012 and 013. Chunk 012 has 597k lines,
  99% of them dead, and took 7 s. [measured, read]
- So the build has 12 full-cost chunks and two near-free ones, and the costs
  of the full ones differ by a factor of 1.8. [measured]

### 7.2 What more chunks would give

List scheduling of the measured CPU times from the 14-chunk build: each job
takes its CPU time on one worker, and the link (3.5 s) waits for every
compile. "Equal chunks" splits the chunks' total CPU into n equal jobs plus
0.5 s each for the header. [simulated from measured CPU times]

| Scenario | 4 workers | 8 workers | 16 workers | CPU |
|---|---|---|---|---|
| 14 chunks, ninja's order (as measured) | 10.6 min | 7.2 min | 4.6 min | 41.0 min |
| 14 chunks, longest first | 10.3 min | 5.3 min | 4.4 min | 41.0 min |
| 14 chunks with /Zi (x1.29), longest first | 13.0 min | 6.9 min | 5.7 min | 51.9 min |
| host prebuilt, longest first | 9.5 min | 5.3 min | 4.4 min | 37.5 min |
| 24 equal chunks, longest first | 10.4 min | 5.2 min | 3.0 min | 41.2 min |
| 48 equal chunks, longest first | 10.4 min | 5.2 min | 2.7 min | 41.4 min |
| 24 equal chunks, host prebuilt | 9.5 min | 5.0 min | 3.0 min | 37.7 min |
| Lower bound (CPU / workers) | 10.3 min | 5.1 min | 2.6 min | |

- On 4 workers only less CPU helps; the order and the chunk count do not
  matter. [simulated]
- On 8 workers today's order costs 2 minutes: chunk 008 starts in the second
  wave. Longest-first or equal chunks get within 0.2 minutes of the lower
  bound. [simulated]
- On 16 workers the longest chunk is the floor. Smaller chunks take it from
  4.4 to about 3 minutes. [simulated]

Then I lifted with `--chunk-lines 150000` (53 chunks) and built des_boot from
it, the same way as the 14-chunk build: no `/Zi`, `-j3`, CPU logged per job.
[measured; the simulated rows are list scheduling of each build's own CPU
times]

| | 14 chunks (600k lines) | 53 chunks (150k lines) |
|---|---|---|
| Chunk CPU in all | 2,128 s | 1,904 s |
| All des_boot jobs | 2,461 s | 2,238 s |
| Per chunk: median, largest | 166 s, 261 s | 34 s, 102 s |
| Near-free chunks (wrappers) | 2 | 6, plus one at 11 s |
| Peak commit of a chunk job | 2.4 GB (alone) | 0.71 GB median, 1.2 GB max |
| Wall time at `-j3`, loaded | 1,019 s | 808 s |
| Simulated, ninja's order (4 / 8 / 16 workers) | 10.6 / 7.2 / 4.6 min | 9.6 / 5.4 / 3.1 min |
| Simulated, longest first | 10.3 / 5.3 / 4.4 min | 9.4 / 4.7 / 2.4 min |
| Lower bound | 10.3 / 5.1 / 2.6 min | 9.3 / 4.7 / 2.3 min |

- The chunks took 10% less CPU in all, not more. The header costs 0.5 s per
  chunk, and clang seems to save more than that on smaller translation units
  (the inliner's call-graph work grows faster than the code). The two builds
  ran next to different builds of other lanes, so read it as "no extra
  CPU" rather than a saving. [measured, inferred]
- Gains: 9% on 4 workers (only from the lower CPU), 25% on 8, 33% on 16, in
  ninja's own order. [simulated]
- Costs still vary: chunk 035 took 102 s, three times the median of 34 s, and
  it now sets the 16-worker time. A split by estimated cost instead of by
  lines would get 16 workers near 2.3 minutes. [measured, simulated]
- The exe links. Its code differs from the 14-chunk build only in what
  clang could inline across functions, which is what R3 measures.
  [measured, inferred]

### 7.3 Memory: the bigger win on small PCs

Ninja does not look at memory. The jobs that fit are about (RAM - 4 GB) /
peak memory per job. [inferred]

| Peak per chunk job | 16 GB PC | 32 GB PC |
|---|---|---|
| 14 chunks with /Zi (master): 5.8 to 6.9 GB | 2 jobs | 4 jobs |
| 14 chunks without /Zi: 2.4 GB | 5 jobs | 11 jobs |
| 53 chunks without /Zi: 0.7 GB median, 1.2 GB max | 10 jobs | 23 jobs |

[peaks measured, job counts inferred]

- An 8-core PC with 16 GB, from the CPU totals: about 26 minutes with `/Zi`
  (2 jobs), about 8.5 without (5 jobs), about 5.4 with smaller chunks (all 8
  cores). With more jobs than fit, it pages, as this PC did. [inferred]
- docs/45 R2 and R4 asked for these peaks. R2: chunk 009 at `/O2` takes
  161 s and 2.4 GB without `/Zi`; `/O1` 240 s and 2.4 GB. R4: answered by the
  table above. The patcher's job formula should use about 2.5 GB per job for
  600k-line chunks without `/Zi`, and measure again if the chunk size
  changes. [measured, inferred]

## 8. Rebuilding after a re-lift or an update (branch prepared)

### 8.1 What a re-lift rebuilt

The lifter deleted every chunk first and wrote all of them and the header
again, so every timestamp moved and ninja recompiled everything: 2,128 s of
CPU for a one-hook change. The header also declared every insn hook, so a
hook change changed the header that all chunks include. [read, measured]

### 8.2 The branch

Branch `tools/lifter-incremental-output` (ps3recomp):

- writes each output file only if its text changed, so unchanged files keep
  their timestamps;
- removes stale chunks after the writes, not before;
- declares the insn-hook functions at the top of the chunks that call them,
  not in the header.

Re-lifts with the port's recipe into a scratch copy of a fresh lift:
[measured]

| Re-lift | Files rewritten | Chunks to rebuild | CPU to rebuild |
|---|---|---|---|
| Same input | chunk 005 | 1 of 14 | about 150 s |
| One hook more | chunk 007, chunk 005 | 2 of 14 | about 320 s |
| Back to master's hooks | chunk 007 | 1 of 14 | about 170 s |

- The header was never rewritten. [measured]
- Chunk 005 is rewritten every time because `apply_boot_patches.py` edits it
  after the lift, so it never matches the lifter's output. [measured, read]
- Chunks 000 and 001 from the new lifter compile to the same code as
  master's. The 53-chunk build in section 7.2 used the new lifter, so the
  port compiles and links against its header. [measured]
- A host file that defines a hook needs `extern "C"` now, because the header
  no longer declares it. The port's four hook definitions have it. [read]

### 8.3 The boot patch and the VS Code C/C++ extension

In 3 of 3 re-lifts where the lifter had just rewritten chunk 005, the boot
patch failed with `PermissionError: [Errno 13]` and left the chunk
unpatched. In the 3 re-lifts where chunk 005 was left alone, it patched
fine. A build from an unpatched lift would crash at the NULL heap.
[measured, inferred]

- Windows' Restart Manager named the holder: "C/C++ Extension for Visual
  Studio Code". It opened a freshly written 34 MB chunk about 3 s after the
  write and held it without write sharing until about 13 s. A file written
  outside the VS Code workspace could be rewritten at any time. [measured]
- The patch runs about 3 s after the lifter's last write: it first reads
  every chunk up to the patched one. [measured]
- Master has the same race; it is just rarer, because in a fresh lift the
  extension is busy with 13 other new chunks. Both fresh lifts I made today
  patched fine. With the branch, chunk 005 is often the only new file.
  [measured, inferred]
- Fixes, either is enough: retry the write in `apply_boot_patches.py` (a
  write that fails with a sharing violation truncates nothing, so retrying
  is safe), or `"C_Cpp.files.exclude": {"**/work/**": true}` in the VS Code
  settings. The setting also stops the extension from parsing about 480 MB
  of lifted code after every lift. Reads of the held file worked; only
  writes failed. [measured, inferred]
- Branch `tools/boot-patch-retry` (port) does the retry: every 0.5 s for up
  to 30 s, then an error that names the file, and exit code 1. I tested it
  offline on a copy of chunk 005 with the patch taken out. A helper process
  held the chunk open the way the extension does (read access, sharing
  reads only), or exclusively. [measured]

  | Run | Exit | Time | Chunk afterwards |
  |---|---|---|---|
  | master, no lock | 0 | 0.4 s | patched (reference) |
  | branch, no lock | 0 | 0.3 s | same bytes and output as master |
  | master, held 5 s | 1 | 0.3 s | unpatched, `PermissionError` at the write |
  | branch, held 5 s | 0 | 5.3 s | waited, then the same bytes as master |
  | branch, held 40 s | 1 | 30.3 s | unpatched, the error names the file |
  | branch, exclusive lock 5 s | 1 | 0.1 s | unpatched, `PermissionError` at the read, as on master |

  An exclusive lock also blocks reads, and the branch does not retry those.
  The extension's lock does not block them.

### 8.4 Toward "an update that does not change the lift only relinks"

- With the branch, a re-lift with the same input still rewrites chunk 005,
  because of the boot patch. Three ways out [inferred]:
  - A lifter entry guard (for example `--entry-guards
    0xAC8078=des_heap_guard`: the lifted function starts with `if
    (des_heap_guard(ctx)) return;`). The patch then lives in the port's code
    and `apply_boot_patches.py` goes away. Best, and small, but a lifter
    feature: design first.
  - Lift into a staging folder, patch there, and copy into `work/recomp` only
    the files whose bytes changed. No lifter change; costs one more 480 MB
    copy and compare.
  - Have `apply_boot_patches.py` restore the old timestamp when the patched
    text equals what the file held before the lift. That needs the old text,
    which the lifter has already overwritten. Not workable alone.
- An object cache keyed by a content hash (chunk text, header, compiler
  version, flags) would also reuse objects across build folders, for
  example after an update installs into a new folder. ccache and sccache
  both handle clang-cl but need an install. A small home-grown launcher
  could do it for the chunks only. Worth it only if updates will rebuild in
  new folders. [read, inferred]
- Such a cache, or docs/45's "same DLL on every PC", needs objects that do
  not depend on the folder. Chunk 000 compiled without `/Zi` from two
  different lift folders, into two different output folders, 15 minutes
  apart: the two 38 MB objects differ only in the 4-byte timestamp in the
  object header. Without `/Zi` the object holds no path; its small
  `.debug$S` holds only the compiler version. So `/Brepro`, which zeroes
  the timestamp, should make them identical (docs/45 R6); not tried.
  [measured, inferred]

## 9. Prebuilding the host part in CI

- Host code is 213 s of des_boot's CPU (8.7%), plus 129 s for the runtime
  library. [measured]
- Prebuilding it saves about 0.8 minutes on 4 workers and nothing on 8 or 16,
  where the chunks set the time (table in 7.2). [simulated]
- The port's glue includes `ppu_recomp.h` from the lift, so it cannot be
  prebuilt as it is. docs/45's option 2 (a prebuilt host DLL) would need that
  split first. [read]
- The gain for players is small; what it buys is fewer compiler jobs that
  can fail on a player's PC. A packaging decision, not a build-time one.
  [inferred]

## 10. Lift, SPU and libsre

- The lift's 40.8 s: 18.5 s of analysis on one thread (boundary recovery and
  jump tables), 11 s of lifting on 16 workers, 5.5 s for the 1,024
  mid-function wrappers, 3.4 s of writing. [measured]
- Caching the analysis by a hash of its inputs (the ELF, the function list,
  the lifter version) would save about 18 s per re-lift. Small next to the
  compile. [inferred]
- The SPU lift (17.6 s) runs on one thread; libsre takes 9.6 s. Both run
  once per install. Not worth work now. [measured]
- The boot patch reads up to six 34 MB chunks to find its anchor (1.3 s).
  Fine. [measured]

## 11. Shader precompile

The first start precompiles the shader cache behind a progress bar
(`port/des_prework.cpp`): 3,535 shaders in 28 s on 15 threads with an empty
cache (docs/32). It is not part of the build and runs again only when the
stamp changes. [read] Run request R1 re-measures it on master.

## 12. Ideas ranked

| # | Idea | Measured gain | Effort | State |
|---|---|---|---|---|
| 1 | Drop `/Zi` by default | -23 to -30% CPU per chunk, -60% memory per job; 4 workers 13.0 to 10.3 min | trivial | branch ready (port) |
| 2 | Lifter writes only changed files, hooks declared per chunk | hook change rebuilds 1 or 2 chunks instead of 14 (-85 to -93% CPU) | small | branch ready (ps3recomp) |
| 3 | Retry the boot patch write (or exclude `work/` in VS Code) | reliability, no time | trivial | branch ready (port); merge before #2 |
| 4 | Smaller chunks, `--chunk-lines 150000` | -9% on 4 workers, -25% on 8, -33% on 16; memory per job 2.4 to 1.2 GB at most | one flag in the recipe | needs R3 |
| 5 | Entry-guard hook instead of the boot patch | same-input re-lift rebuilds nothing; enables relink-only updates | small to medium | design first |
| 6 | Cost-balanced chunks in the lifter | like #4 with fewer, even chunks; little on top of #4 | medium | design first |
| 7 | Object cache by content hash | rebuilds in a new folder reuse objects | medium, or an install | design first |
| 8 | Prebuilt host in CI | -0.8 min on 4 workers, 0 on 8 and 16 | medium (CI, versioning) | packaging decision |
| 9 | Cache the lifter's analysis | -18 s per re-lift | small to medium | later |
| 10 | Faster clang (a PGO or mimalloc build) | unknown here; often 10 to 20% elsewhere | a toolchain download | ask first |
| - | Precompiled headers | at most 0.3% (1% with 53 chunks) | | dropped |
| - | Hot `/O2`, cold `-O1` or `/O1` | none, or 49% slower | | dropped |

## 13. Branches prepared

All three were opened as PRs and merged on 2026-10-18: port #60
(`build/no-debug-info`), ps3recomp #57 (`tools/lifter-incremental-output`)
and port #61 (`tools/boot-patch-retry`).

- **Port, `build/no-debug-info`**, commit 94974b4 on master (495d5a7): `/Zi`
  off by default, `DES_DEBUG_INFO=ON` for a PDB. Changes only
  `port/CMakeLists.txt` and `port/ui/CMakeLists.txt`. Needs a boot check (R2)
  before merging.
- **ps3recomp, `tools/lifter-incremental-output`**, commit 7d566fd on
  fork/master (f15a4f0): write only changed files, stale removal after the
  writes, hook declarations in the chunks, and
  `tools/test_lifter_incremental.py`. Merge it after `tools/boot-patch-retry`,
  or accept that a re-lift can need the patch run again by hand.
- **Port, `tools/boot-patch-retry`**, commit 53b5b06 on master (e094102):
  `apply_boot_patches.py` retries a write that another program blocks, for
  up to 30 s (section 8.3). Changes only that script. Needs no build and no
  game run.

None changes optimization levels. The lifter branch should go in after the
retry, or with the VS Code exclusion in place. Otherwise they are
independent.

Both masters moved while I worked (port e094102, ps3recomp dedfdaf). Neither
touched the files the first two branches change, and both still merge into
them without conflicts (checked with `git merge-tree`). [measured]

## 14. Run requests

These run the game. The exes are in the lane folder under `exe_nozi14/` and
`exe_bt3/`, each with its `des_boot.map`.

- **R1. Shader precompile on master.** Start master's exe with an empty
  shader cache: `PS3_SHADER_CACHE` set to a new folder (the runtime creates
  it), plus the usual knobs `DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0
  DES_AUTO_CONTINUE=1`. Log the `[des] shaders:` line. Expect about 3,535
  shaders compiled in about 28 s on 15 threads. `DES_PRECOMPILE=force` alone
  is not enough: it runs the pass, but shaders already in the cache count
  as "already cached". [read, port/des_prework.cpp]
- **R2. Boot check of the no-/Zi exe.** `DES_EXE=<lane>/exe_nozi14/des_boot.exe`
  with the usual knobs. Load into the Nexus, walk for a minute, open the menu.
  Expect: same as master. Against a `/Zi` build, three functions in chunk
  004 differ by a byte and two UI functions by a few instruction bytes;
  nothing else in the code.
- **R3. Frame-rate A/B, 14 against 53 chunks.** Both exes are without `/Zi`
  and come from the same EBOOT and hooks. In Boletarian Palace, alternate
  `exe_nozi14` and `exe_bt3` with `tools/des/exeab.sh` at `DES_FPS=60
  VBLANK_MHZ=240000` and the usual knobs, when the PC is quiet. Log the
  frame rate of each. Expect: within a few percent. Smaller chunks give the
  inliner fewer callees to see, which could cost a little or nothing.
  Needed before the recipe switches to `--chunk-lines 150000`.

## 15. Decisions for you

1. Merge `build/no-debug-info` after R2.
2. Merge `tools/boot-patch-retry`, then `tools/lifter-incremental-output`
   (or the lifter branch alone, with the VS Code exclusion in place).
3. After R3: switch the recipe to `--chunk-lines 150000`, or keep 600k?
4. Entry-guard hook to replace `apply_boot_patches.py`: OK to design?
5. Object cache: home-grown launcher, or install sccache or ccache?
6. A faster clang build would need a download. Want it measured?
7. Delete the stale `port/build/des_boot.pdb` (June, 251 MB)?
