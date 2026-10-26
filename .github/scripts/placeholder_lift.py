"""Empty files where the lift goes, so CI can configure the split build without a copy of the game (ci.yml's
game build description jobs, docs/86):

  python .github/scripts/placeholder_lift.py <folder for RECOMP_DIR> >> "$GITHUB_ENV"

One file of each kind the port's CMake reads: a PPU chunk and the HLE NID table in the given folder (RECOMP_DIR),
an SPU image and the SPU workload table in ps3recomp/work_spu, the lifted libsre in work/libsre and its glue in
port/gen. Prints DES_LIFT=<folder>. It never replaces a file: a tree with a real lift in it is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main(argv):
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    lift = Path(argv[0]).resolve()
    files = {
        lift / "ppu_recomp_0000.cpp": "",
        lift / "hle_nid_table.cpp": "",
        ROOT / "ps3recomp" / "work_spu" / "placeholder" / "spu_recomp.c": "",
        # CMake reads this one and removes its static constructor (DES_SPLIT): a comment is enough.
        ROOT / "ps3recomp" / "work_spu" / "_gen" / "spu_workloads.cpp": "/* CI: no lift */\n",
        ROOT / "work" / "libsre" / "lifted" / "ppu_recomp.c": "",
        ROOT / "port" / "gen" / "libsre_glue.c": "",
    }
    there = [str(p) for p in files if p.exists()]
    if there:
        print(f"::error::not replacing files that are already there: {', '.join(there)}", file=sys.stderr)
        return 1
    for p, text in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    print(f"DES_LIFT={lift.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
