# 06 — M0 Analysis Findings (Demon's Souls BLUS30443)

*Results of the M0 assessment. All derived from the user's own disc, locally. Nothing here is committed except this summary (no code/keys/assets).*

## Decryption — DONE ✅

Method that worked: **RPCS3 PPU/SPU debug dump.** Enabling `PPU Debug` + `SPU Debug` in the per-game config and booting once made RPCS3 write decrypted programs to `rpcs3/ppu_progs/` and `rpcs3/spu_progs/`.

- Decrypted main executable: `ppu_progs/BLUS30443-…-EBOOT.BIN/exec.elf` → copied to `work/game/EBOOT.ELF`.
- **Verified header:** `ELF64`, **big-endian**, OSABI `0x66` (CELL_LV2), `ET_EXEC`, machine `0x15` (**EM_PPC64**), entry `0x1916138`. 28 MB.
- Bonus: the 9 PRX modules DeS loads were also dumped decrypted (see below).

> The RPCS3 dump has **stripped section-header names**, so ps3recomp's section-name-based import detection returns 0 imports. Not blocking (we get the module list from the dumped PRX set + RPCS3 logs), but **fixing EXEC import parsing to use `proc_prx_param` is a good early upstream contribution** — see [`02-roadmap.md`](02-roadmap.md) M0/M1.

## PPU code size

- **93,667 functions** detected (`find_functions.py`; OPD descriptors + heuristic seed pass).
- Code+rodata segment ≈ 25 MB at vaddr `0x10000`.
- Scale context: flOw ≈ 51K functions → 156 MB generated C++. DeS at ~94K → expect **~250–300 MB generated C++** and a long but finite compile. Large, not unreasonable.

## PRX modules DeS loads (9) — all have HLE coverage in ps3recomp

| Module | Purpose | ps3recomp HLE |
|---|---|---|
| libsysmodule | module loader | ✅ |
| libfs | filesystem | ✅ cellFs |
| libgcm_sys | **RSX graphics** | ✅ cellGcmSys (state); backend partial |
| libresc | resolution/scaling | ✅ cellResc |
| libnet | networking (online) | ✅ sys_net/cellNet (online out of scope initially) |
| libsre | **SPURS runtime** | ✅ cellSpurs (mgmt only — see SPU note) |
| libl10n | localization | ✅ |
| libat3dec + libatrac3plus | **ATRAC3+ audio** | ⚠️ cellAdec (needs FFmpeg path) |

Module coverage outlook: **good.** No exotic/unsupported library in the load set.

## SPU usage — the risk driver, and it's BOUNDED

**134 SPU images** were dumped, but they split into three very different buckets:

### Bucket A — Firmware codecs (the large majority) → FFmpeg, NOT hand-ported
`CAvcTask{A..K}`, `CAvcTaskRealizer`, `CMp4Task*`, `CAwjThreadMgmt*` (H.264/AVC), `vc1_dec_*`, `m2v_dec`/`m2v_slice`/`m4v_dec` (MPEG-2/4), `aacenc`/`m4aacdec*`, `mp3dec`, `wmadec`, `ac3-dec`, `dtshd*`, `ddpdec*`, `celp*`, `lpcm_dec`, `at3dec`/`at3mdec`/`atxdec`/`msngSPURS_ATRAC` (ATRAC3+), `csc_*`/`yuv*`/`rgb*` (colour-space), `jpeg/png/gif` decoders, `synth2`, `apost_src_*`.

These come from the PS3 firmware `cellVdec`/`cellAdec`/`cellVpost` libraries (RPCS3 loads the whole set). ps3recomp's design already routes these to **host decoding (FFmpeg/stb)** via HLE — we never reverse-engineer them. DeS actually uses only a subset (ATRAC3+ audio + FMV movie decode for the `movie/` folder).

### Bucket B — SPURS framework + game tasks → investigate, partially HLE
`spurs_kernel_executable`, `spurs_kernel2_executable`, `SpursTaskCommandDispatcher`, and generic `task`, `thread_a/a1/a3`, `thread_b1..b4`, `thread_c1..c3`, `thread_d1/d2`. The kernel/dispatcher are handled by ps3recomp's SPURS HLE; the generic `thread_*`/`task` images are **the game's own SPU jobs** scheduled under SPURS — these need reverse-engineering to see what they compute (culling, skinning, particle, decompression, etc.). **This is the main unknown.** Countable (≈15 distinct), not unbounded.

### Bucket C — Havok physics/animation on SPU → the real hand-work, but KNOWN
- `hkaSpu` — Havok **animation**
- `hkpCollide` — Havok **collision**
- `hkpCollisionQuery` — Havok **collision queries**
- `hkpIntegrate` — Havok **physics integration**

These are gameplay-critical and ps3recomp will not run them. Options: (a) write PPU-fallback shims that run physics on the host CPU, or (b) drive a host-side Havok/Bullet path. Havok is **well-documented middleware**, which makes this hard-but-tractable rather than a black box.

## M0 Gate Decision

**Proceed to M1.** Rationale:
- Decryption solved; clean PPC64 ELF in hand.
- PPU size large but normal (~94K functions).
- Module coverage is good — no unsupported library.
- SPU risk, the thing that could have been fatal, is **bounded and categorized**: most SPU images are FFmpeg-able firmware codecs; the true custom work is ~4 Havok modules (known middleware) + ~15 SPURS task images (countable unknowns). This is a hard project, not an impossible one.

**Next (M1):** install MSVC+CMake; lift PPU to C with `ppu_lifter.py`; stand up the project from the template; get it to compile/link and boot to CRT init with the null renderer. In parallel, the `proc_prx_param` import-parsing fix is a clean first upstream patch.
