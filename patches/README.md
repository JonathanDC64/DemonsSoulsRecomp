# Patches to ps3recomp

We use [ps3recomp](https://github.com/sp00nznet/ps3recomp) as the recompilation
base. The full clone lives in `../ps3recomp/` (gitignored — third-party, large).
Our fixes to it are committed inside that clone's own git **and** exported here as
portable patches so they survive a re-clone and are ready to upstream.

Apply with: `cd ps3recomp && git apply ../patches/ps3recomp/<file>.patch`

## 01-portability-and-scaling-fixes.patch

Four fixes, all needed to lift+build+boot a large (106k-function) PS3 title that
the tool's prior test targets (≤14k functions, e.g. Uncharted) never exercised.
All are general improvements, not Demon's Souls-specific.

| # | File | Problem | Fix |
|---|------|---------|-----|
| 1 | `tools/ppu_lifter.py` | `generate_mid_function_entries()` rescans the full ~6M-instruction list twice per target × thousands of targets × up to 25 passes → multi-O(N²) hang (~40 min, no output) on a 96k-fn binary. | Binary-search-slice the instruction list to `[target, span)` before each `lift_function` call. Behaviour-preserving (it already ignores out-of-range insns). Lift time → <1 min. |
| 2 | `runtime/ppu/ppu_loader.cpp` | Function registry is a fixed **64K-slot** open-addressing hash (`PPU_HASH_BITS=16`), sized for ~14k-fn games. A 106k-fn title overflows it; `ppu_register_function` then probes a full table **forever** (100% CPU hang during init). | `PPU_HASH_BITS` 16 → 19 (524,288 slots, ~0.2 load at 106k) **and** a bounded-probe guard that logs+returns instead of spinning. |
| 3 | `CMakeLists.txt` | `libs/network/sceNpCommerce.c` defines the same `sceNpCommerce2*` symbols as `sceNpCommerce2.c`. MSVC `lib` only warned (LNK4006); `lld-link` errors on the duplicates when linking an exe. | Exclude the duplicate `sceNpCommerce.c` from the runtime lib sources. |
| 4 | (see project `port/compat/dirent.h`) | `runtime/ppu/ppu_fs.cpp` includes `<dirent.h>` unconditionally (only guards `<io.h>` for MinGW) → no build under MSVC/clang-cl. | Minimal Win32 `<dirent.h>` shim shipped in our `port/compat/` and put on the include path (kept in our tree, not the clone). |

> Note: the "EXEC has 0 imports" issue seen early on was **not** a tool bug — it was
> using the wrong tool (`prx_analyzer.py`/`elf_parser.py`). `ppu_loader.py` reads
> imports from `proc_prx_param` correctly despite stripped section names.
