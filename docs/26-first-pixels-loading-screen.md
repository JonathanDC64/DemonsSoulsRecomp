# 26 — First pixels: the loading screen renders

The Demon's Souls loading screen now appears in the game window: the soul-sequence sprites and the logo in the bottom-left. Before this work every frame was black.

Five separate bugs stood between the asset pipeline in [doc 25](25-spurs-queue-lle-boot-assets-complete.md) and a visible frame. They are listed here in the order they were found.

## 1. Texture uploads never ran (SPURS JOBLIST)

DeS uploads textures through a SPURS job chain:

```
JOBLIST(0x45B4DD10) ; SYNC ; FLUSH ; JOB 0x45B5DF80 ; END
```

Our HLE job-chain walker (`jc_execute` in `libs/spurs/cellSpurs.c`) treated opcode 6 (JOBLIST) as a no-op, so VRAM stayed zero.

- **JOBLIST is now executed.** The list is a `CellSpursJobList {u32 numJobs; u32 sizeOfJob; u64 eaJobList}`, and each job in it runs through `jc_run_one_job`.
- **The copy job is now lifted.** Its binary is EBOOT 0x0177B480 (384 bytes). It was dumped to `work/spu_jobs/spujob_0177B480.bin` and lifted via `scripts/build_spu.py`. The job PUTs its ioBuffer (up to 16 KB from one DMA-list entry) to the texture's VRAM address.

VRAM now holds the TPF texels (verified with `findhex`).

## 2. VMX lane byte order in the old PPU lift

The lift in `work/recomp` is from 2026-07-01. It predates upstream's VMX fix (1e30602), and we can't simply re-lift because the current lifter doesn't boot DeS ([doc 24 §8](24-highgraphics-init-and-rsx-fifo.md)).

That lift has an inconsistent view of vector registers:

- **Raw big-endian bytes:** `lvx`/`stvx`, perm and logic ops treat `ctx->vr` this way.
- **Host little-endian lanes:** float, int-add, shift, convert and `vspltisw` read the same registers this way.

As a result, every VMX float result was computed on byte-swapped inputs.

The visible symptom was the boot-screen vertex shader's position matrix (c0–c3), which arrived as `0x0000C0FF` in all 16 lanes. That is the x86 default NaN, byte-reversed. Every quad collapsed.

**Fix:** `scripts/fix_vmx_byteorder.py` patches the lift in place, in the same style as `fix_jumptables.py`. It rewrites 60,963 blocks so they load lanes byte-swapped, run the same body, and store `d` back swapped. It also:
- adds saturation to `vctsxs`;
- places `lvewx`/`stvewx` data at lane `EA & 15`.

## 3. `vmrghw` lifted as `vmrghh`

`ppu_lifter.py` picks the element size with `"h" in mn[4:]`. That test is true for `vmrghw` (`mn[4:]` is `"hw"`), so every high-word merge was emitted as a halfword merge.

- DeS contains 1,500 `vmrghw` and no `vmrghh`.
- `vmrghw` is half of the matrix-transpose idiom, so transposed rows looked like `0x80003F80`: −0.0 and 1.0 interleaved by halfword.
- `fix_vmx_byteorder.py` rewrites all 1,475 sites.

The current lifter already gets this right: upstream 2070f4d (2026-07-10) added a merge handler that reads the size from `mn[5]` and runs first. The old handler stayed in the file as dead code; it is removed on the `fix/lifter-dead-vmrg-handler` branch.

## 4. Unlifted vector ops

Three vector ops were never lowered. The lifter emitted them as TODO comments, leaving the destination register stale:

| Op | Sites |
|---|---|
| `vrfin` (`vmx_x522`) | 140 |
| `vsumsws` | 113 |
| `vcmpgtuh` | 8 |

`fix_vmx_byteorder.py` lowers all three.

With fixes 2–4 in place, c0–c3 is a correct 1280×720 ortho matrix: `(2/1280, -2/720, 1/256, (-1, 1, 1, 1))`.

## 5. D3D12 backend: the final composite

The frame is drawn as three blended quads into RT 0x7A0000, using fp 0xB40481 (`tex * col0`, where col0 is a fade-in from c12/c13). Then fp 0x181 composites that RT through a 256×1 gamma LUT into the backbuffer. Two backend bugs broke this second pass:

- **Wrong texture for t0.** `SCREEN_AS_TEX` (a heuristic from another title, on by default) replaced every display-sized texture with last frame's backbuffer copy. It ran before the offscreen-RT lookup. It now applies only when no offscreen RT exists at that offset.
- **Wrong sampler for the LUT.** Samplers were four static ones: s0 point/clamp, s1–s3 linear/WRAP. With WRAP, the LUT at u≈0 blended texel 0 with texel 255, so black came out mid-grey (127). The VP root signature now takes a sampler descriptor table (root parameter 3), built per draw from the guest's `TEXTURE_ADDRESS` and `TEXTURE_FILTER`. See `vp_sampler_table`: tables are deduplicated and kept for the whole run, so a frame in flight never sees one change.

## Rebuild after a re-lift

The lift output is gitignored. Rebuild it in this order:

```
python scripts/fix_jumptables.py work/recomp
python scripts/fix_vmx_byteorder.py work/recomp
bash tools/des/build.sh
```

All three steps are idempotent. A full parallel build of the patched chunks can exhaust the page file; if so, build with `cmake --build port/build --target des_boot --parallel 4`.

## New diagnostics

| Knob | What it does |
|---|---|
| `VP_CONSTS=i,j,...` | Prints those vertex constants whenever they change, for draws that pass the `VP_ATTRS`/`VP_ATTRS_FP` filter. |
| `PPU_GUARD_CHANGE=1` | With `PPU_GUARD_EA`, names every writer that changes the watched word (value plus guest callstack), not just one that zeroes it. |
| `FP_SUB=<fp>:<file>` | Compiles a hand-written HLSL in place of one translated fragment program. Unlike `FP_ONE`/`FP_TEX`, it always applies. |
| `SPU_PUT_MAGIC[_ARM]` | Finds the SPU PUT that writes a given word and arms the guard on the next transfer. |
| Console: `findhex`, `nzmap`, `dumpbin` | Search guest memory, map its non-zero pages, and dump a range to a file. |
| `ppu_guard_page_rearm` | Re-arms the guard page at runtime, trapping reads or writes. |

A temporary NaN-origin tracer was also used (not committed). It logged every vector float op whose inputs were finite but whose output had a NaN. It found the `vrefp(0)·0` path that led to the `vmrghw` bug.

## Next

- The loading screen fades out after about 60 s and the screen stays black. Find what the boot waits on next (the auto-save notice and title screen should follow).
- The full VMX audit isn't done yet. Other lane ops that are byte-order sensitive but absent from DeS's template list aren't covered. Re-run the template census in `fix_vmx_byteorder.py`'s header comment if new symptoms appear.
- Upstream the dead `vmrg*` handler removal (`fix/lifter-dead-vmrg-handler`, not pushed).
