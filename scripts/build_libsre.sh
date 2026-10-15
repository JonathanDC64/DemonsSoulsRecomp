#!/usr/bin/env bash
# build_libsre.sh [libsre.prx]: lift the user's own decrypted libsre.prx (the
# Sony SPURS library from their PS3 firmware) for the firmware-LLE path
# (LIBSRE_LLE=1, docs/22 and docs/25), and generate the port-side glue.
#
#   work/libsre/libsre_{image.bin,functions,exports,imports}.json   lift_prx.py
#   work/libsre/lifted/ppu_recomp.{c,h}                             ppu_lifter.py
#   port/gen/libsre_glue.c                                          gen_libsre_exports.py
#
# All of it is firmware-derived and gitignored (docs/05-legal.md). Re-run
# cmake for port/build afterwards: port/CMakeLists.txt links the lifted module
# only when both files exist. Default input: ps3recomp/fw_spu/libsre.prx.
set -euo pipefail
cd "$(dirname "$0")/.."
PRX="${1:-ps3recomp/fw_spu/libsre.prx}"
[ -f "$PRX" ] || { echo "no decrypted libsre.prx at $PRX" >&2; exit 1; }
OUT=work/libsre
mkdir -p "$OUT"
python ps3recomp/tools/lift_prx.py "$PRX" --base 0x30000000 -o "$OUT"
# The TOC is the module's own (0x30000000 + its gp, which lift_prx.py prints);
# 0x30039AB0 for the firmware libsre this was written against (docs/22).
python ps3recomp/tools/ppu_lifter.py "$OUT/libsre_image.bin" --raw --base 0x30000000 \
  --toc "${LIBSRE_TOC:-0x30039AB0}" --functions "$OUT/libsre_functions.json" \
  --symbol-prefix libsre_ --output "$OUT/lifted"
# One chunk; CMake builds it as ppu_recomp.c (compiled as C++).
mv -f "$OUT/lifted/ppu_recomp_000.cpp" "$OUT/lifted/ppu_recomp.c"
python scripts/gen_libsre_exports.py
echo "build_libsre.sh: done; re-run cmake for port/build"
