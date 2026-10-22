#!/usr/bin/env python3
"""Lift the player's own decrypted libsre.prx and generate the port-side glue.

libsre.prx is Sony's SPURS library from the PS3 firmware, used by the
firmware-LLE path (LIBSRE_LLE=1, docs/22 and docs/25). Three steps:

  <work>/libsre_{image.bin,functions,exports,imports}.json   ps3recomp/tools/lift_prx.py
  <work>/lifted/ppu_recomp.c                                 ps3recomp/tools/ppu_lifter.py
  <glue>                                                     scripts/gen_libsre_exports.py

<work> defaults to work/libsre and <glue> to port/gen/libsre_glue.c. All of it
is firmware-derived and gitignored (docs/05-legal.md). Re-run cmake for
port/build afterwards: port/CMakeLists.txt links the lifted module only when
both files exist.

The lifter gets the module's own TOC, which lift_prx.py writes into
libsre_exports.json (0x30039AB0 for the firmware libsre this was written
against, docs/22); --toc overrides it.

Usage:  python scripts/build_libsre.py [libsre.prx]       (default ps3recomp/fw_spu/libsre.prx)
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x30000000


def run(cmd):
    print("+ " + " ".join(os.path.basename(c) if i == 1 else c for i, c in enumerate(cmd)), flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise SystemExit(f"build_libsre: {os.path.basename(cmd[1])} failed (exit {r.returncode})")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("prx", nargs="?", default=os.path.join(ROOT, "ps3recomp", "fw_spu", "libsre.prx"),
                    help="the decrypted libsre.prx")
    ap.add_argument("--ps3recomp", default=os.path.join(ROOT, "ps3recomp"), help="the ps3recomp checkout")
    ap.add_argument("--work", default=os.path.join(ROOT, "work", "libsre"), help="the lift's folder")
    ap.add_argument("--glue", default=os.path.join(ROOT, "port", "gen", "libsre_glue.c"), help="the glue file")
    ap.add_argument("--runtime-image", default="work/libsre/libsre_image.bin", metavar="PATH",
                    help="where the game loads the image from at run time (gen_libsre_exports.py)")
    ap.add_argument("--toc", type=lambda x: int(x, 0), default=os.environ.get("LIBSRE_TOC"),
                    help="the module's TOC (default: $LIBSRE_TOC, else lift_prx.py's)")
    a = ap.parse_args(argv)
    if not os.path.isfile(a.prx):
        print(f"no decrypted libsre.prx at {a.prx}", file=sys.stderr)
        return 1
    tools = os.path.join(a.ps3recomp, "tools")
    py = sys.executable
    os.makedirs(a.work, exist_ok=True)
    run([py, os.path.join(tools, "lift_prx.py"), a.prx, "--base", f"0x{BASE:08X}", "-o", a.work])
    toc = a.toc
    if toc is None:
        toc = int(json.load(open(os.path.join(a.work, "libsre_exports.json"), encoding="utf-8"))["toc"], 16)
    lifted = os.path.join(a.work, "lifted")
    run([py, os.path.join(tools, "ppu_lifter.py"), os.path.join(a.work, "libsre_image.bin"), "--raw",
         "--base", f"0x{BASE:08X}", "--toc", f"0x{toc:08X}",
         "--functions", os.path.join(a.work, "libsre_functions.json"),
         "--symbol-prefix", "libsre_", "--output", lifted])
    # One chunk; CMake builds it as ppu_recomp.c (compiled as C++).
    os.replace(os.path.join(lifted, "ppu_recomp_000.cpp"), os.path.join(lifted, "ppu_recomp.c"))
    run([py, os.path.join(ROOT, "scripts", "gen_libsre_exports.py"), "--src", a.work, "--out", a.glue,
         "--runtime-image", a.runtime_image])
    print("build_libsre: done; re-run cmake for port/build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
