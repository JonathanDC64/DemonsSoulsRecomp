# 04 — Work Log

Reverse-chronological. Each entry: what we did, what we found, what's next.

---

## 2026-06-29 — PPU boot complete → RSX/GCM → SPU/SPURS gate

Condensed; the blow-by-blow lives in the milestone docs ([`07`](07-m2-boot-debug.md), [`08`](08-ppu-boot-complete.md),
[`09`](09-spu-spurs.md)–[`11`](11-spurs-runtime-spec.md)).

**Did / found (in order):**
- **M2 cleared → full PPU boot to the 60 fps main loop.** Six root-cause fixes; the keystone was a
  **rotate-by-zero UB** the lifter emitted (`v>>64` / `rs>>32` for shift==0) that clang miscompiled
  under -O2 — it was the root of the whole C++ static-init "construction cascade." Also: event-flag
  syscall numbers (collided with timer/usleep), MUTEX/COND max 256→8192, HLE ctx-handler return→r3,
  guest-pointer translation in libs/system. Full PPU reference + reproduce recipe in [`08`](08-ppu-boot-complete.md).
- **PIC jump-table lifting** — the lifter skipped gcc switch tables reached via a two-level-TOC base,
  emitting unresolved indirect calls (e.g. broke the HighGraphics state machine). Fixed: two-level
  base resolution + inline-table fallback, lifted as an in-function `ctr` switch. 18→695 dispatchers.
- **Shadow call stack** (`ppu_lifter --shadow-stack`) — reliable guest backtraces where host/back-chain
  walks fail on tail-call trampolines. The tool that cracked the next two bugs.
- **`sys_memory_get_page_attribute` (351)** was misnumbered + stubbed → a GCM-setup branch saw garbage
  page_size and skipped constructing a sub-object → null-deref. Implemented it (+ per-alloc page size).
  → boot reached **`_cellGcmInitBody`** (RSX/GCM init).
- **GCM FIFO control register** lived as a host pointer the game read as a guest address → its
  FIFO-drain wait spun. Moved it into guest memory + drain `get→put` each vblank. → graphics-init
  thread proceeds and **dispatches its first SPU workload**.

**Now at the SPU/SPURS gate (M3):** `func_00A31158` dispatches a 68KB SPU workload (currently a
`spu_workload` MISS) + calls 3 unresolved cellSpurs NIDs, then waits for completion. The SPU
recompiler is DeS-complete (0 ISA gaps, [`09`](09-spu-spurs.md)); remaining = clean-room HLE SPURS
runtime ([`11`](11-spurs-runtime-spec.md) has the 20-fn checklist).

**Repo:** ps3recomp changes pushed to the fork (github.com/JonathanDC64/ps3recomp); port pushed to the
private DemonsSoulsPC repo (firewall-clean — docs/patches/port glue only).

**Next:** implement the 3 cellSpurs job-chain NIDs; lift+register the DeS SPU images so the workload
HITs; run the job + deliver completion so the graphics thread releases (→ feeds the main thread's
cond=3 wait).

---

## 2026-06-28 (cont.) — git set up; M2 boot debugging begun (allocator bad_alloc)

**Did:**
- Set up version control: main repo `git init` (email `jonathan_delcorpo@hotmail.com`), copyright-firewall `.gitignore` (fixed two bugs: inline comment disabled `work/`; unanchored `ps3recomp/` hid `patches/ps3recomp/`). Verified no game-derived files staged. **Initial commit `73715ce`.** ps3recomp fixes committed in the clone's own git + exported to `patches/ps3recomp/`.
- Added env-gated **HLE call trace** (`YDKJ_HLETRACE`) to ps3recomp for boot debugging.
- **M2 blocker #1 diagnosed** (full detail in [`07-m2-boot-debug.md`](07-m2-boot-debug.md)): after CRT/TLS, game does `get_user_memory_size`(→192MB ok) + rwlock + 5× mutex_create, then `operator new` throws `bad_alloc` with **no memory syscall/HLE at all**. Ruled out: stub gaps, endianness, HLE-malloc-returns-0. Leading hypothesis: custom allocator computes a 0/invalid heap size (likely needs process-param/heap config) and bails before requesting memory. Error strings located at guest ~`0x1770150`.

**Next:** xref the "bad allocation" string to find the operator-new/allocator-init path; watch the allocator's heap-size global (`YDKJ_WWATCH`); ensure process-param heap config is surfaced.

---

## 2026-06-28 (cont.) — 🎉 FIRST BOOT: native des_boot.exe runs real DeS CRT, reaches sys_initialize_tls

**Did:** linked `des_boot.exe` (426 MB native Windows binary) and ran it. Sequence of link/run fixes:
1. **dup symbol** `sceNpCommerce2*` — excluded duplicate `sceNpCommerce.c` from runtime lib (upstream fix #3); regenerated NID table without it (981 handlers).
2. **undefined DXGI GUIDs** — added `dxguid` to link libs.
3. **undefined `func_FFFFFFE8`** — degenerate lifter branch target in a misdecoded region; stubbed via `port/missing_stubs.cpp` → `ppu_unlifted_stub`.
4. **100% CPU hang in init** — root cause: `ppu_loader.cpp` function registry is a fixed **64K-slot** open-addressing hash; DeS has **106k functions** → table overflow → infinite probe loop. **Fix (upstream #4):** `PPU_HASH_BITS` 16→19 (524k slots) + bounded-probe overflow guard.

**Result — it boots and runs real game code:**
```
[boot] dispatching entry OPD 0x01916138
[ppu] run: code 0x000124C0, toc 0x019B5040, sp 0x0FF00000
[crt] sys_initialize_tls: block 0x0E000000, r13=0x0E007000   <- textbook first CRT call
<game prints its own fatal strings> "Continue... (Lv-2 is still running.)"
[sysPrxForUser] sys_process_exit(code=1)
```
Reached the **same first-boot milestone (`sys_initialize_tls`)** the docs cite for Uncharted — for a far larger title, in one session.

**Current blocker (first M2 target):** after CRT/TLS init the **game's own allocator fails** (it prints its bad-alloc/abort strings via our printf HLE, then sys_process_exit(1)). Almost certainly the `sys_memory`/`sys_mmapper` heap HLE returning failure/zero, so the game's heap init fails. Next: trace the HLE/syscall sequence between `sys_initialize_tls` and the abort; make the memory-allocation syscalls return a valid heap.

**Build recipe (reproducible):** `scripts/vsenv.bat` + `cmake --build port\build` (clang-cl). Run: `set PS3_VFS_ROOT=<disc dir>` then `des_boot.exe work\game\EBOOT.ELF`.

---

## 2026-06-28 (cont.) — M1: re-lift (OPD+imports), runtime built, boot project wired, first full build

**Did:**
- Realized first lift used heuristic `find_functions.json` and no imports. Switched to the canonical flow per `docs/PPU_RECOMP.md`:
  - `ppu_loader.py` → authoritative **OPD table (73,295 fns)**, TOC `0x19b5040`, entry OPD `0x1916138`, and **389 imports across 25 libs** (via `proc_prx_param` — works despite stripped section names; this resolves the earlier "0 imports" gap, no code fix needed — I'd just used the wrong tool).
  - Re-lifted with `--functions EBOOT.functions.json --hle-stubs EBOOT.imports.json` → **106,127 fns, 36 chunks, ~1.46 GB** (vs 262k/2.0GB heuristic). Imports now dispatch to `ps3_hle_call(NID)`.
- Built **`ps3recomp_runtime.lib`** (2.1 MB) with MSVC `cl` — 110/110 objs, only cosmetic warnings.
- Single-chunk compile test (cl, /Od): **8.6 s → 47 MB obj**, clean. Full 36-chunk build is feasible.
- Generated **HLE NID table** (`gen_hle_nids.py` over 93 modules) → `hle_nid_table.cpp`, **988 handlers** (defines `ppu_hle_register_all`).
- Chose **clang-cl** for the game build: per-game files (`boot_main.cpp`, `ppu_hle.cpp`) use `__attribute__((weak))`/`((aligned))` (GCC/clang-cl only; `cl` rejects), and generated chunks natively support clang-cl. Installed **LLVM 22.1.8**; ABI-compatible with the cl-built runtime lib.
- Wrote build infra: `scripts/vsenv.bat` (VS+LLVM+CMake+Ninja env), `port/CMakeLists.txt` (links runtime lib + runtime/ppu glue + 36 chunks + NID table + boot_main; `/bigobj /Od`), `port/compat/dirent.h`.

**Bug/portability fix #2 (upstream-worthy):** `runtime/ppu/ppu_fs.cpp` includes `<dirent.h>` unconditionally (only guards `<io.h>` for MinGW) → won't build on MSVC/clang-cl. Wrote a minimal Win32 `<dirent.h>` shim (`port/compat/dirent.h`, FindFirstFile-based) covering opendir/readdir/closedir. Glue now compiles 6/6.

**In progress:** full `des_boot.exe` build (clang-cl, 36 chunks + glue + link ~1.7 GB of objs).

**Next:** first run of `des_boot.exe <EBOOT.ELF>` → expect it to execute real DeS boot code until the first unimplemented import/syscall/unlifted fn (each logged) — a live to-do list. Goal: reach/pass `sys_initialize_tls`.

---

## 2026-06-28 (cont.) — M1 started: toolchain installed + PPU lift complete (after fixing a lifter bug)

**Did:**
- Installed full build toolchain via winget: **MSVC 14.44.35207**, **Windows SDK 10.0.26100.0** (has `d3d12.h`), **CMake 4.3.3**, **Ninja 1.13.2**.
- Ran `ppu_lifter.py` on `EBOOT.ELF`. First runs **hung** (~40 min, ~2 cores, zero output).

**Bug found & fixed (upstream-worthy) — ppu_lifter.py O(N²) mid-function hang:**
- After the fast parallel lift, `generate_mid_function_entries()` calls `lift_function(all_insns, …)` with the **full ~6M-instruction list**, which `lift_function` rescans twice per call. Across thousands of mid-function/gap targets × up to 25 passes → multi-O(N²) on a 96k-function binary.
- **Fix:** binary-search-slice `all_insns` to `[target, span)` before each call (behaviour-preserving; the function already ignores out-of-range insns). Edited in `ps3recomp/tools/ppu_lifter.py` (`generate_mid_function_entries`). → lift now finishes in well under a minute.
- Minor secondary issue noted (not fixed): the multiprocessing Pool isn't joined at exit, leaving idle worker processes. Cleaned up manually.

**Lift result:**
- **262,572 functions** lifted (96,183 base + mid-function tail-entry wrappers), 19,431 unique call targets.
- **~2.0 GB** generated C++ → 51 chunks `ppu_recomp_000–050.cpp` (~600k lines each) + `ppu_recomp.h`, in `work/recomp/`.
- ⚠️ Note: 2.7× function expansion (base→total) and 2 GB is large vs flOw (51k fn / 156 MB). Possible mid-function over-generation; compile will be heavy (MSVC RAM/time). Revisit if build is unbearable.

**Next:**
1. Build the ps3recomp **runtime** lib with MSVC (root CMake).
2. Set up DeS project from `templates/project/`: `config.toml` (9 modules, `backend=null`), wire the 51 chunks into CMake, `main.cpp`/`stubs.cpp`.
3. First compile (expect errors → iterate) → boot to CRT init.
4. Upstream: submit the lifter O(N²) fix; fix EXEC import parsing via `proc_prx_param`.

---

## 2026-06-28 (cont.) — M0 executed: decryption + analysis complete

**Did:**
- Installed Python tool deps (pycryptodome, capstone, construct, tabulate, tqdm).
- Decrypted the retail EBOOT via **RPCS3 PPU/SPU debug dump** (enabled `PPU Debug`/`SPU Debug` in the BLUS30443 custom config — backed up & restored afterward). Got `exec.elf` → `work/game/EBOOT.ELF`. User also did a full manual boot+LLVM compile, confirming a complete dump.
- Ran ps3recomp analysis: `elf_parser.py`, `prx_analyzer.py`, `find_functions.py`.

**Found (full detail in [`06-m0-analysis-findings.md`](06-m0-analysis-findings.md)):**
- Clean decrypted **PPC64 BE ELF**, 28 MB, entry `0x1916138`.
- **93,667 functions** (~2× flOw).
- **9 PRX modules**, all with ps3recomp HLE coverage.
- **SPU = 134 images but bounded:** mostly FFmpeg-able firmware codecs; real custom work = 4 Havok modules (hkaSpu/hkpCollide/hkpCollisionQuery/hkpIntegrate) + ~15 SPURS task images.
- Tooling gap: RPCS3 dump strips section names → ps3recomp EXEC import detection returns 0 (fixable via `proc_prx_param`).

**M0 gate: PROCEED to M1.**

**Next (M1):**
1. Install MSVC + Windows SDK + CMake/Ninja.
2. `ppu_lifter.py` → generate C; set up project from template; compile/link; boot to CRT init (null renderer).
3. (Upstream) fix EXEC import parsing via `proc_prx_param`.

---

## 2026-06-28 — Project kickoff, tool acquisition & analysis

**Did:**
- Inspected the disc at `D:\…\Demon's Souls (USA)`. Confirmed: `PS3_GAME/USRDIR/EBOOT.BIN` = 28 MB encrypted SELF (magic `SCE\0`); 7.9 GB assets in FromSoftware formats (3604 .dcx, 2190 .hkx, 1776 .flver, 356 .tpf, 272 .lua, .esd, .param).
- Researched the recompilation/decompilation field (N64Recomp, XenonRecomp, decomp, RPCS3) and the PS3's specific blockers (Cell SPE, RSX, encryption).
- Cloned **ps3recomp** to `./ps3recomp/` (commit `fa2d2da`, v0.6.3) and read its source + docs.

**Found (key):**
- **ps3recomp does NOT run SPU code** — confirmed in `docs/SPU_FALLBACK.md` line 3, contradicting the README's "fully recompiles SPU." SPU jobs must be hand-reimplemented per game as PPU-side fallbacks. This is the dominant risk for Demon's Souls.
- Graphics: D3D12 backend exists and draws simple geometry, but full RSX shader translation + texturing is incomplete.
- Best PoC (flOw) boots but is **not playable** ("title-state wall").
- Environment: Python 3.13 + git present; **missing** CMake, MSVC+Win SDK, Python tool deps, and a located RPCS3 install.
- ps3recomp ships no keys → retail `EBOOT.BIN` needs RPCS3 dump or public keys to decrypt.

**Wrote:** docs 00–05, README, .gitignore.

**Next (M0):**
1. `pip install -r ps3recomp/requirements.txt`
2. Locate RPCS3; produce a decrypted `EBOOT.ELF` into `work/game/` (Option A in `03-setup.md`).
3. Run the analysis pipeline → record function count, module coverage, unresolved NIDs, and **SPU image count** here.
4. M0 gate decision: proceed to M1 or not.

---

<!-- Template for new entries:
## YYYY-MM-DD — <title>
**Did:**
**Found:**
**Next:**
-->
