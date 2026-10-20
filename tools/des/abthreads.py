#!/usr/bin/env python3
"""abthreads.py <batch dir>: per-thread CPU from the profile sessions of an exeab.sh batch.

For each valid boot (lc_<tag>_<A|B><round>, "ok" in runs.txt; the others are listed as skipped)
and profile session (prof_NNN_threads.txt): the session's busy share of HighGraphics, the FIFO
thread (vblank_ticker), the busiest spu_async_thread (image 2's host thread), main and the cores
busy, plus the share of the window that HighGraphics and main spent in guest syscalls (the `waits`
table). Prints a row per boot (the mean of its sessions), the side means, B-A and a paired t over
the rounds (the boot means of each round).
"""
import glob
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from abstat import paired  # noqa: E402

KEYS = ["HG", "FIFO", "img2", "main", "cores", "HG sys", "main sys"]
NAN = float("nan")


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        t = f.read()
    m = re.search(r"over the session.*?\n(.*?)\n\s+([0-9.]+) cores busy", t, re.S)
    if not m:
        return None
    r, spu = {}, []
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*([0-9.]+)%\s+(.*)$", line)
        if not mm:
            continue
        v, name = float(mm.group(1)), mm.group(2).strip()
        if name == "ppu 4 HighGraphics":
            r["HG"] = v
        elif name.startswith("?vblank_ticker"):
            r["FIFO"] = v
        elif name == "ppu 1 main":
            r["main"] = v
        elif name == "spu_async_thread":
            spu.append(v)
    r["img2"] = max(spu) if spu else NAN
    r["cores"] = float(m.group(2))
    for tid, key in (("4", "HG sys"), ("1", "main sys")):
        mm = re.search(r"^\s*tid %s\s+\S+.*?syscalls\s+([0-9.]+)%%" % tid, t, re.M)
        r[key] = float(mm.group(1)) if mm else NAN
    return r


def mean(xs):
    xs = [x for x in xs if x == x]
    return sum(xs) / len(xs) if xs else NAN


def main(d):
    # Only the boots exeab.sh found valid (runs.txt), as abstat.py does.
    valid = None
    rp = os.path.join(d, "runs.txt")
    if os.path.exists(rp):
        valid = {}
        with open(rp, errors="replace") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) >= 4:
                    valid[p[1]] = p[3]
    rows = {}
    for lc in sorted(glob.glob(os.path.join(d, "lc_*"))):
        m = re.search(r"_([AB])([0-9]+)$", lc)
        if not m or not os.path.isdir(lc):
            continue
        side, rnd = m.group(1), int(m.group(2))
        why = "ok" if valid is None else valid.get(os.path.basename(lc)[3:], "not in runs.txt")
        if why != "ok":
            print(f"{side}{rnd}: skipped ({why})")
            continue
        ss = [parse(p) for p in sorted(glob.glob(os.path.join(lc, "prof_*_threads.txt")))]
        ss = [s for s in ss if s]
        if not ss:
            continue
        rows[(side, rnd)] = {k: mean([s.get(k, NAN) for s in ss]) for k in KEYS}
        print(f"{side}{rnd} ({len(ss)} sessions): " + "  ".join(f"{k} {rows[(side, rnd)][k]:.1f}" for k in KEYS))
    if not rows:
        print("no profile sessions with a thread table (prof_*_threads.txt)")
        return
    rounds = sorted({r for _, r in rows})
    print()
    print(f"{'':10}" + "".join(f"{k:>10}" for k in KEYS))
    for side in "AB":
        vals = [rows[(side, r)] for r in rounds if (side, r) in rows]
        if vals:
            print(f"{side + ' mean':10}" + "".join(f"{mean([v[k] for v in vals]):10.2f}" for k in KEYS))
    line_d, line_p = f"{'B-A':10}", f"{'paired p':10}"
    for k in KEYS:
        ds = [rows[("B", r)][k] - rows[("A", r)][k] for r in rounds if ("A", r) in rows and ("B", r) in rows]
        ds = [x for x in ds if x == x]
        _, _, p = paired(ds)
        line_d += f"{mean(ds):+10.2f}"
        line_p += f"{p:10.3f}" if not math.isnan(p) else f"{'-':>10}"
    print(line_d)
    print(line_p)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
