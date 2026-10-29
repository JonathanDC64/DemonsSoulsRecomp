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
        python scripts/build_spu.py --eboot work/game/EBOOT.ELF -j 4
                                               # cut the images and jobs out of the EBOOT first

--eboot cuts the SPU images and the two SPURS job blobs out of EBOOT.ELF and checks each by
its fingerprint in scripts/spu_table.json (addresses, sizes and hashes; no game bytes).
--elf-dir, --job-dir, --out-dir and --ps3recomp move the inputs and outputs (setup keeps
them in its data folder); --only lifts single images. The lifted files include
"../../runtime/spu/...": outside ps3recomp/work_spu, compile them with ps3recomp/runtime/spu
on the include path, whose ../.. is ps3recomp.
"""
import argparse, glob, json, os, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF_DIR = os.path.join(ROOT, "work", "spu_images")
JOB_DIR = os.path.join(ROOT, "work", "spu_jobs")
PS3RECOMP = os.path.join(ROOT, "ps3recomp")
TABLE = os.path.join(ROOT, "scripts", "spu_table.json")


# Lifted functions replaced by a host call (spu_lifter.py --native-hook): image ->
# (image fingerprint, [ADDR=SYMBOL]). A hook is applied only to the exact image it
# was written for; with another build of the game the image lifts without it.
NATIVE_HOOKS = {
    # Core.Res.Decompressor's SPU task (Edge zlib): its inflate wrapper, run on
    # every 64 KB block of a DCX file. Lifted it is ~3.5 ms a block, most of a
    # level load; the host inflate is ~0.2 ms. SPU_NATIVE_INFLATE=0 turns it off.
    "spu_0000_at_01760280": (0x4F421747C0F7966A, ["0x8700=spu_hook_inflate_ls"]),
    # HighGraphics' ring consumer (image 2): the PUT-and-wait helper it calls
    # after every ring command to store its state. SPU_NATIVE_PUT=0 turns it off.
    "spu_0001_at_0176C200": (0x4FBCAB40231BF72B, ["0xC358=spu_hook_put_wait"]),
}


# Lifted instruction ranges replaced by a host call (spu_lifter.py --native-block,
# --regions images only): image -> (image fingerprint, [ADDR-END=SYMBOL]), applied
# only to the exact image they were written for, like NATIVE_HOOKS.
NATIVE_BLOCKS = {
    # HighGraphics' ring consumer (image 2): the step that takes each ring
    # command, its GETLLAR and unchanged PUTLLC as one read when the ring
    # already holds a command. SPU_NATIVE_RING=0 turns it off. And the end of
    # each command, the call to 0x3C90 that stores the next output position,
    # while the buffer has room. SPU_NATIVE_POST=0 turns it off. And the table
    # compare-and-copy loop of ring commands 31-34 (0x8E18-0x8F60, 32 passes
    # over two pairs of 2 KB tables), the largest block of the image's lifted
    # code. SPU_NATIVE_DIFF=0 turns it off. And the 16-entry scan of the same
    # commands (0x7348-0x7440), up to the next entry that goes to a handler.
    # SPU_NATIVE_SCAN=0 turns it off.
    "spu_0001_at_0176C200": (0x4FBCAB40231BF72B, ["0x4618-0x4730=spu_hook_ring_fetch", "0x4808-0x4818=spu_hook_ring_post",
                                                  "0x8E18-0x8F60=spu_hook_state_diff",
                                                  "0x7348-0x7440=spu_hook_entry_scan"]),
}


# Jump-table `bi`s lifted as a switch of region gotos (spu_lifter.py --bi-switch,
# --regions images only): image -> (image fingerprint, [ADDR=TABLE:COUNT]), applied
# only to the exact image they were written for, like NATIVE_HOOKS.
BI_SWITCH = {
    # HighGraphics' ring consumer (image 2): the dispatch on each ring command's
    # number (37 handlers, 0x47F0-0x5094) and the one in 0x7D70 (16 words, 7
    # targets). SPU_BI_SWITCH=0 turns them off.
    "spu_0001_at_0176C200": (0x4FBCAB40231BF72B, ["0x4758=0x475C:37", "0x81A8=0x81AC:16"]),
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


def _write_if_changed(path, data):
    """Write data to path through a temporary file, leaving an identical file
    (and its time stamp) alone."""
    try:
        if open(path, "rb").read() == data:
            return
    except OSError:
        pass
    tmp = path + ".tmp"
    open(tmp, "wb").write(data)
    os.replace(tmp, path)


def _load_segments(buf):
    """(vaddr, file offset, file size) of each PT_LOAD of a 64-bit big-endian ELF."""
    phoff = struct.unpack_from(">Q", buf, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", buf, 0x36)
    segs = []
    for k in range(phnum):
        t, _fl, off, va, _pa, fs = struct.unpack_from(">IIQQQQ", buf, phoff + k * phentsize)
        if t == 1:
            segs.append((va, off, fs))
    return segs


def cut(eboot, elf_dir, job_dir, ps3recomp, table=TABLE):
    """Cut the SPU images and the SPURS job blobs out of EBOOT.ELF into elf_dir
    and job_dir, each checked against the table. Returns the problems, one line
    each (none: everything matched)."""
    sys.path.insert(0, os.path.join(ps3recomp, "tools"))
    from extract_spu_images import find_spu_images
    t = json.load(open(table, encoding="utf-8"))
    buf = open(eboot, "rb").read()
    os.makedirs(elf_dir, exist_ok=True)
    os.makedirs(job_dir, exist_ok=True)
    bad, found = [], set()
    for idx, (off, size) in enumerate(find_spu_images(buf)):
        name = f"spu_{idx:04d}_at_{off:08X}"
        b = buf[off:off + size]
        want = t["images"].get(name)
        if want is None or int(want["fnv"], 16) != fnv(b[:img_size(b)]):
            bad.append(f"{name}: not an SPU image the table lists")
            continue
        found.add(name)
        _write_if_changed(os.path.join(elf_dir, name + ".elf"), b)
    bad += [f"{name}: not found in the EBOOT" for name in t["images"] if name not in found]
    segs = _load_segments(buf)
    for name, j in t["jobs"].items():
        va, size = int(j["vaddr"], 16), j["size"]
        fo = next((off + va - sva for sva, off, fs in segs if sva <= va and va + size <= sva + fs), None)
        raw = buf[fo:fo + size] if fo is not None else b""
        if fnv(raw) != int(j["fnv"], 16):
            bad.append(f"{name}: the bytes at 0x{va:08X} are not the job the table lists")
            continue
        _write_if_changed(os.path.join(job_dir, name + ".bin"), raw)
    return bad


def _lift(name, cmd):
    print(f"lifting {name} ...", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return f"{name}: the lifter failed\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}"
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--gen-only", action="store_true",
                    help="only regenerate the registration (skip lifting)")
    ap.add_argument("--eboot", metavar="ELF",
                    help="first cut the SPU images and jobs out of this EBOOT.ELF (checked against "
                         "scripts/spu_table.json)")
    ap.add_argument("--elf-dir", default=ELF_DIR, help="the SPU images (default work/spu_images)")
    ap.add_argument("--job-dir", default=JOB_DIR, help="the SPURS job blobs (default work/spu_jobs)")
    ap.add_argument("--ps3recomp", default=PS3RECOMP, help="the ps3recomp checkout (its tools/)")
    ap.add_argument("--out-dir", help="the lifted code (default <ps3recomp>/work_spu)")
    ap.add_argument("--only", action="append", default=[], metavar="NAME",
                    help="lift only this image or job (repeatable); the registration still lists all")
    ap.add_argument("-j", "--jobs", type=int, default=1, help="lifts to run at once (default 1)")
    a = ap.parse_args(argv)
    lifter = os.path.join(a.ps3recomp, "tools", "spu_lifter.py")
    out_dir = a.out_dir or os.path.join(a.ps3recomp, "work_spu")
    gen_only = a.gen_only

    if a.eboot:
        bad = cut(a.eboot, a.elf_dir, a.job_dir, a.ps3recomp)
        for line in bad:
            print(f"ERROR: {line}", file=sys.stderr)
        if bad:
            return 1

    elfs = sorted(glob.glob(os.path.join(a.elf_dir, "*.elf")))
    if not elfs:
        print(f"ERROR: no SPU images in {a.elf_dir}\n"
              f"  run: python scripts/build_spu.py --eboot work/game/EBOOT.ELF",
              file=sys.stderr)
        return 1
    jobs = sorted(glob.glob(os.path.join(a.job_dir, "*.bin")))
    names = [os.path.splitext(os.path.basename(f))[0] for f in elfs + jobs]
    unknown = [n for n in a.only if n not in names]
    if unknown:
        print(f"ERROR: no image or job named {', '.join(unknown)}", file=sys.stderr)
        return 1

    def wanted(name):
        return not gen_only and (not a.only or name in a.only)

    imgs, tasks = [], []
    for f in elfs:
        img = os.path.splitext(os.path.basename(f))[0]
        b = open(f, "rb").read()
        imgs.append((img, fnv(b[:img_size(b)]), be32(b, 0x18)))  # name, fingerprint, e_entry

        if wanted(img):
            out = os.path.join(out_dir, img)
            os.makedirs(out, exist_ok=True)
            hooks, blocks, tables = [], [], []
            if img in NATIVE_HOOKS:
                want, hooks = NATIVE_HOOKS[img]
                if want is not None and want != imgs[-1][1]:
                    print(f"  {img}: fingerprint 0x{imgs[-1][1]:016X} is not the one its "
                          f"native hooks were written for; lifting without them")
                    hooks = []
            if img in NATIVE_BLOCKS and img in REGIONS:
                want, blocks = NATIVE_BLOCKS[img]
                if want != imgs[-1][1]:
                    print(f"  {img}: fingerprint 0x{imgs[-1][1]:016X} is not the one its "
                          f"native blocks were written for; lifting without them")
                    blocks = []
            if img in BI_SWITCH and img in REGIONS:
                want, tables = BI_SWITCH[img]
                if want != imgs[-1][1]:
                    print(f"  {img}: fingerprint 0x{imgs[-1][1]:016X} is not the one its "
                          f"jump tables were written for; lifting without them")
                    tables = []
            tasks.append((img, [sys.executable, lifter, "--auto-functions", f,
                                "--symbol-prefix", img + "_", "-o", out] +
                               (["--regions"] if img in REGIONS else []) +
                               [x for h in hooks for x in ("--native-hook", h)] +
                               [x for h in blocks for x in ("--native-block", h)] +
                               [x for h in tables for x in ("--bi-switch", h)]))

    # Raw SPURS job binaries: wrap at LS 0 (entry 0), fingerprint the raw bytes.
    sys.path.insert(0, os.path.join(a.ps3recomp, "tools"))
    from wrap_spu_elf import wrap
    for f in jobs:
        img = os.path.splitext(os.path.basename(f))[0]
        raw = open(f, "rb").read()
        imgs.append((img, fnv(raw), 0))
        if wanted(img):
            out = os.path.join(out_dir, img)
            os.makedirs(out, exist_ok=True)
            elf = os.path.join(out, img + ".elf")
            open(elf, "wb").write(wrap(raw, base=0, entry=0))
            tasks.append((img, [sys.executable, lifter, "--auto-functions", elf,
                                "--symbol-prefix", img + "_", "-o", out]))

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        errors = [e for e in ex.map(lambda t: _lift(*t), tasks) if e]
    if errors:
        print("\n".join(errors), file=sys.stderr)
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
    gen_dir = os.path.join(out_dir, "_gen")
    os.makedirs(gen_dir, exist_ok=True)
    open(os.path.join(gen_dir, "spu_workloads.cpp"), "w", newline="").write("\n".join(L) + "\n")
    print(f"wrote {gen_dir}/spu_workloads.cpp for {len(imgs)} images")
    for img, fp, ent in imgs:
        print(f"  {img}: fp=0x{fp:016X} entry=0x{ent:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
