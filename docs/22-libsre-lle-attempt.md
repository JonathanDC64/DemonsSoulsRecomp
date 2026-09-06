# 22 — libsre firmware LLE: attempt, result, and how to resume

**2026-09-05.** Attempt at docs/21 §19 option (2): replace the hand-written SPURS/PM layer with
the real, lifted Sony `libsre.prx` kernel. **Result: substantial progress, blocked on one
concrete PRX-layout problem. Pivoted back to option (1); everything is preserved and resumable.**

All work is on `ps3recomp` branch **`libsre-lle`**. The default build is unaffected — the whole
path is gated behind `LIBSRE_LLE=1`, so pivoting costs nothing and needs no rebuild.

---

## What worked

**Stage 1 — lift (passed).**

```
lift_prx.py fw_spu/libsre.prx --base 0x30000000 -o work/libsre
  -> cellSre_Library, TOC 0x30039AB0, image 205632 B
  -> relocations: type1 x1366, type4 x32, type6 x32
  -> 267 exports / 32 imports, 352 function seeds

ppu_lifter.py --raw --base 0x30000000 --toc 0x30039AB0 --functions ... --symbol-prefix libsre_
  -> 756 functions, 51.8 MB of C
```

Builds and links into `des_boot`. **Default boot unchanged at 13/14.**

**Coverage is good:** 75 of the game's imported NIDs are exported by libsre, 69 of them
SPURS/Sync — the entire surface we care about.

**Stage 2 — load and bind (mostly worked).** The module loads (`756/756` functions, 267
exports) and, once its own import stubs were bridged, **the real Sony kernel executed an
authentic SPURS bring-up**:

```
_sys_spu_image_import
sys_spu_thread_group_create(num=5 prio=250)  -> "DLSpursManager"
thread_init x5                                (the five SPU threads)
evt queue_create + port_create + connect
sys_ppu_thread_create x2  "DLSpursManager"    (the real handler threads)
```

That is RPCS3's sequence, and **our HLE never created those `DLSpursManager` threads at all**.
The approach is sound; the runtime's plumbing (PRX loader, export bridge, lv2 SPU syscalls,
event queues) carried it.

### Two things that had to be fixed to get there

1. **A latent lifter bug of our own.** `--override-funcs`'s `override_funcs`/`override_syms`
   initialisation had been merged into dead code *after* a `return` in `_range_insns()` instead
   of `__init__`. Any parallel lift crashed with `AttributeError: override_syms`. It had gone
   unnoticed because we had not re-lifted PPU code since the v0.10.0 merge. **This fix matters
   to the main line too** — a future PPU re-lift would have hit it.
2. **libsre's own import stubs (FIRMWARE_LLE.md step 4.2), which I initially skipped.** Each
   stub is
   `li r12,lo ; oris r12,r12,hi ; lwz r12,disp(r12) ; lwz r0,0(r12) ; lwz r2,4(r12) ; mtctr r0 ; bctr`
   so the pointer-table entry (at `0x3002E000`) holds a **pointer to an OPD**. Unpatched, the
   module branched to `0x39800000` — a bare `li r12,0` instruction word read as an address,
   which is exactly what `[ppu] unresolved indirect call -> 0x39800000` meant. The generator now
   decodes all 32 stubs and installs OPD + trampoline per NID.

---

## The blocker

**The module declares a TOC outside the memory its program headers describe.**

```
PT_LOAD[0]  vaddr 0x000000  filesz 0x2E000  memsz 0x30000
PT_LOAD[1]  vaddr 0x02E000  filesz 0x03FE0  memsz 0x04340
  -> memory extent = 0x32340

module_info gp/TOC          = 0x39AB0      <-- 0x7770 PAST the extent
```

The module-info parse is otherwise correct: the export table (`0x1DB24..0x1DBE8`) and import
table (`0x1DBF0..0x1DC48`) are both in range, and the module name reads cleanly. Every export
OPD is well-formed and consistent — e.g. `cellSpursCreateTaskset` → `{code=0x30014DC4,
toc=0x30039AB0}`. So the module really does use `gp = base + 0x39AB0`.

`lift_prx.py` lays segments out contiguously at `base + p_vaddr` and sizes the image to the
extent, so **everything the TOC points at is unmapped and reads back as zero.** The visible
symptom: libsre computes its handler-thread entry TOC-relatively, gets `0`, and
`sys_ppu_thread_create` is called with `entry=0x00000000` — the threads start and immediately
die (`[ppu] thread entry 0x00000000 not registered`). The boot reaches 5 threads instead of 18.

**This is a PRX layout/relocation-semantics problem, not a problem with our runtime.** PS3 PRX
relocations are segment-relative, and RPCS3 allocates each segment separately rather than
contiguously — so a faithful loader may place segment 1 somewhere that makes `0x39AB0` land
inside real memory. Resolving it means matching RPCS3's `ppu_load_prx` segment placement and
per-segment relocation bases, which is real work, not a tweak.

---

## How to resume

1. Read RPCS3's `ppu_load_prx` (`C:/dev/rpcs3-reference`) for segment placement and the
   segment-relative meaning of the `0x700000A4` relocation entries, then make `lift_prx.py`
   reproduce that layout (most likely: independent segment bases + per-segment reloc bases,
   with the TOC landing inside the allocated data segment).
2. Sanity check afterwards: **`gp` must resolve inside a mapped segment**, and
   `vm_read32(gp - 0x8000)`-ish TOC reads must return non-zero data.
3. Re-run: `LIBSRE_LLE=1 ./port/build/des_boot.exe work/game/EBOOT.ELF`. The acceptance signal
   is `sys_ppu_thread_create name="DLSpursManager" entry=<non-zero>` and the boot continuing
   past 5 threads.
4. Then bind incrementally — `YDKJ_FORCE_HLE=<nid,...>` pushes any individual NID back onto the
   HLE stub, so families can be moved one at a time.

**Everything needed is in the tree:** `tools/lift_prx.py`, `ppu_lifter --raw`,
`runtime/prx/prx_loader.c`, the already-live `prx_resolve_export` bridge in `ps3_hle_call`, and
`fw_spu/libsre.prx`. Rebuild the inputs with `python scripts/gen_libsre_exports.py` after
re-running the lift.

---

## Why the pivot is free

- `work/libsre/` and `port/gen/` are **gitignored** (firmware-derived, docs/05-legal.md).
- `port/CMakeLists.txt` compiles the LLE sources only `if(EXISTS ...)`, and the loader is a
  no-op unless `LIBSRE_LLE=1`.
- So the default build is byte-for-byte the same behaviour, **13/14 verified after the LLE
  sources were linked in**. Reverting is "don't set the env var"; there is no rebuild.

The one thing that should NOT stay on the branch is the `ppu_lifter.py` `override_syms` fix —
it is a genuine bug on the main line and is cherry-picked across.

---

## CORRECTION (checked against the RPCS3 source) — the TOC is NOT the blocker

The "TOC outside the extent" diagnosis above is **wrong**. Verified against RPCS3
(`rpcs3/Emu/Cell/PPUModule.cpp`, clone updated to 2026-09-02):

**1. PRX relocations are per-segment on both sides**, and `lift_prx.py` already implements this
correctly (it documents the semantics from RPCS3):

```c
struct ppu_prx_relocation_info { u64 offset; u16 pad; u8 index_value; u8 index_addr;
                                 u32 type;   u64 ptr; };
raddr     = segs[index_addr].addr + offset      // where to patch
data_base = index_value == 0xFF ? 0 : segs[index_value].addr
```

**2. The TOC's own relocation says exactly what gp is.** The entry patching the module-info
`toc` field is:

```
offset=0x1DC6C  index_addr=0  index_value=1  type=1(ADDR32)  ptr=0xBAB0
   =>  toc = segs[1].addr + 0xBAB0
```

which for a contiguous layout is `base + 0x2E000 + 0xBAB0 = base + 0x39AB0` — precisely what
`lift_prx` produced. **It was right.**

**3. And that address is correct PPC64.** On ELFv1 `r2` conventionally points **0x8000 past the
start of `.toc`**, so the real TOC begins at `gp - 0x8000 = base + 0x31AB0` — comfortably inside
the image (which ends at `base + 0x32340`). Dumping it confirms real, relocated content:

```
0x30031AB0: 3002E088 3001EE90 3001EE98 3001E6B8
0x30031AC0: 3001EEA8 3001EEB8 3002E0E0 3001EEC0
```

All valid in-module pointers. **The TOC is well-formed and reachable.**

### So what is the blocker?

Still `entry=0x00000000` on the handler-thread creation, but the cause is unknown and the TOC
explanation is retracted. The useful new datum is the **argument pattern**:

```
sys_ppu_thread_create  entry=0x00000000  arg=0x01B92E00  stack=0x8000  prio=0
                        ^^^ zero          ^^^ correct     ^^^ correct   ^^^ zero
```

`arg` (the real SPURS instance EA) and `stack` are right, so the ABI alignment is fine — libsre
genuinely computed a null entry *and* a zero priority while getting the neighbouring arguments
right. That is a much narrower question than "the TOC is unmapped", and it is where a resuming
session should start: find the libsre function that calls `sys_ppu_thread_create` and see where
it sources the entry OPD and priority from.

The other genuine difference from RPCS3 remains worth knowing: **RPCS3 allocates each PRX
segment independently** (`vm::alloc(mem_size, vm::main)`, `p_vaddr` ignored) rather than
contiguously at `base + p_vaddr`. That does not appear to matter for the TOC, but it may matter
elsewhere, and it is the one structural difference between our loader and the reference.
