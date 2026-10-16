#!/usr/bin/env python3
"""Lift Demon's Souls' SPU images and generate the workload-dispatch registration.

Pipeline (run after extracting SPU images):
  1. work/spu_images/*.elf  (from: tools/extract_spu_images.py work/game/EBOOT.ELF --out work/spu_images)
  2. each image -> ps3recomp/work_spu/<img>/spu_recomp.{c,h}   (spu_lifter.py --symbol-prefix <img>_)
  3. ps3recomp/work_spu/_gen/spu_workloads.cpp                 (registration, auto-registered at startup)

SPURS job binaries (jobchain jobs) are raw PIC blobs, not ELFs: put each in
work/spu_jobs/<name>.bin (e.g. from SPU_DUMP_MISS=<dir>, or cut from the EBOOT at the
descriptor's eaBinary). Each is wrapped as an SPU ELF at LS 0 (where spu_run_spurs_job
loads it), lifted the same way, and registered by the FNV of the RAW bytes -- the
fingerprint spu_workload_dispatch_job computes.

port/CMakeLists.txt globs ps3recomp/work_spu/*/spu_recomp.c + that _gen file into des_boot, so
cellSpurs AddWorkload/CreateTask dispatch (spu_workload_dispatch) runs the recompiled SPU code.

Output lives in ps3recomp/work_spu/ — GAME-DERIVED, gitignored in the fork (copyright firewall).
The lifted code is placed there (not work/) only because the SPU lifter emits relative includes
("../../runtime/spu/...") that must resolve to ps3recomp/runtime/spu/.

Usage:  python scripts/build_spu.py            # lift all + regenerate registration
        python scripts/build_spu.py --gen-only # only regenerate the registration (skip lifting)
"""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF_DIR = os.path.join(ROOT, "work", "spu_images")
JOB_DIR = os.path.join(ROOT, "work", "spu_jobs")
OUT_DIR = os.path.join(ROOT, "ps3recomp", "work_spu")
LIFTER  = os.path.join(ROOT, "ps3recomp", "tools", "spu_lifter.py")


# Lifted functions replaced by a host call (spu_lifter.py --native-hook): image ->
# (image fingerprint, [ADDR=SYMBOL]). A hook is applied only to the exact image it
# was written for; with another build of the game the image lifts without it.
NATIVE_HOOKS = {
    # Core.Res.Decompressor's SPU task (Edge zlib): its inflate wrapper, run on
    # every 64 KB block of a DCX file. Lifted it is ~3.5 ms a block, most of a
    # level load; the host inflate is ~0.2 ms. SPU_NATIVE_INFLATE=0 turns it off.
    "spu_0000_at_01760280": (0x4F421747C0F7966A, ["0x8700=spu_hook_inflate_ls"]),
}


# Images lifted with spu_lifter.py --regions: the functions between two call
# targets become one C body, so a direct branch among them is a goto rather than
# a trampoline hop. Same entries and semantics; much less per-branch overhead.
REGIONS = {
    # HighGraphics' ring consumer (SPURS image 2): ~20 million hops a second.
    "spu_0001_at_0176C200",
    # The audio mixer's raw SPU thread: its resampling loops.
    "spu_0010_at_01818080",
}


def be16(b, o): return (b[o] << 8) | b[o + 1]
def be32(b, o): return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def img_size(b):
    """On-disk extent (matches the runtime fingerprint); see gen_spu_workloads.py."""
    e_phoff, e_shoff = be32(b, 0x1C), be32(b, 0x20)
    e_phentsize, e_phnum = be16(b, 0x2A), be16(b, 0x2C)
    e_shentsize, e_shnum = be16(b, 0x2E), be16(b, 0x30)
    end = e_shoff + e_shnum * e_shentsize
    for k in range(e_phnum):
        po = e_phoff + k * e_phentsize
        if po + 0x14 > len(b): break
        end = max(end, be32(b, po + 0x04) + be32(b, po + 0x10))
    for k in range(e_shnum):
        so = e_shoff + k * e_shentsize
        if so + 0x18 > len(b): break
        if be32(b, so + 0x04) != 8:  # not NOBITS
            end = max(end, be32(b, so + 0x10) + be32(b, so + 0x14))
    return min(end, len(b))


def fnv(b):  # FNV-1a-64, matching the runtime spu_workload_fingerprint
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    gen_only = "--gen-only" in sys.argv
    elfs = sorted(glob.glob(os.path.join(ELF_DIR, "*.elf")))
    if not elfs:
        print(f"ERROR: no SPU images in {ELF_DIR}\n"
              f"  run: python ps3recomp/tools/extract_spu_images.py work/game/EBOOT.ELF --out work/spu_images",
              file=sys.stderr)
        return 1

    imgs = []
    for f in elfs:
        img = os.path.splitext(os.path.basename(f))[0]
        b = open(f, "rb").read()
        imgs.append((img, fnv(b[:img_size(b)]), be32(b, 0x18)))  # name, fingerprint, e_entry

        if not gen_only:
            out = os.path.join(OUT_DIR, img)
            os.makedirs(out, exist_ok=True)
            print(f"lifting {img} ...", flush=True)
            hooks = []
            if img in NATIVE_HOOKS:
                want, hooks = NATIVE_HOOKS[img]
                if want is not None and want != imgs[-1][1]:
                    print(f"  {img}: fingerprint 0x{imgs[-1][1]:016X} is not the one its "
                          f"native hooks were written for; lifting without them")
                    hooks = []
            r = subprocess.run([sys.executable, LIFTER, "--auto-functions", f,
                                "--symbol-prefix", img + "_", "-o", out] +
                               (["--regions"] if img in REGIONS else []) +
                               [a for h in hooks for a in ("--native-hook", h)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-2000:], r.stderr[-2000:], file=sys.stderr)
                return 1

    # Raw SPURS job binaries: wrap at LS 0 (entry 0), fingerprint the raw bytes.
    sys.path.insert(0, os.path.join(ROOT, "ps3recomp", "tools"))
    from wrap_spu_elf import wrap
    for f in sorted(glob.glob(os.path.join(JOB_DIR, "*.bin"))):
        img = os.path.splitext(os.path.basename(f))[0]
        raw = open(f, "rb").read()
        imgs.append((img, fnv(raw), 0))
        if not gen_only:
            out = os.path.join(OUT_DIR, img)
            os.makedirs(out, exist_ok=True)
            elf = os.path.join(out, img + ".elf")
            open(elf, "wb").write(wrap(raw, base=0, entry=0))
            print(f"lifting job {img} ...", flush=True)
            r = subprocess.run([sys.executable, LIFTER, "--auto-functions", elf,
                                "--symbol-prefix", img + "_", "-o", out],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-2000:], r.stderr[-2000:], file=sys.stderr)
                return 1

    # Registration TU (compiled as C++: extern "C" bridges to the C-lifted symbols).
    L = ['/* GENERATED by scripts/build_spu.py — registers DeS lifted SPU images with the',
         ' * workload-dispatch registry (by FNV-1a-64 image fingerprint) at startup. */',
         '#include "../../runtime/spu/spu_workload.h"', '',
         'extern "C" void spu_begin_image(int image_id);', '']
    for img, fp, ent in imgs:
        L.append(f'extern "C" void {img}_spu_func_{ent:08X}(spu_context*);')
        L.append(f'extern "C" void {img}_spu_recomp_register(void);')
    L.append('')
    L.append('extern "C" void des_spu_register_all(void)')
    L.append('{')
    for i, (img, fp, ent) in enumerate(imgs, 1):
        L.append(f'    spu_begin_image({i}); {img}_spu_recomp_register();')
        L.append(f'    spu_workload_register_img(0x{fp:016X}ULL, {img}_spu_func_{ent:08X}, {i}, "{img}");')
    L.append('    spu_begin_image(0);')
    L.append('}')
    L.append('')
    L.append('__attribute__((constructor)) static void des_spu_autoreg(void) { des_spu_register_all(); }')
    gen_dir = os.path.join(OUT_DIR, "_gen")
    os.makedirs(gen_dir, exist_ok=True)
    open(os.path.join(gen_dir, "spu_workloads.cpp"), "w", newline="").write("\n".join(L) + "\n")
    print(f"wrote {gen_dir}/spu_workloads.cpp for {len(imgs)} images")
    for img, fp, ent in imgs:
        print(f"  {img}: fp=0x{fp:016X} entry=0x{ent:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
