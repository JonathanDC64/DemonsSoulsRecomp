#!/usr/bin/env python3
"""knobstat.py <batch dir>: summarize a knobab.sh batch (tools/des/PERF.md).

Reads <dir>/manifest.txt (window number, side, knob line; the same in every boot), runs.txt (one
line per boot: K, boot, number, validity, background cpu) and each valid boot's
lc_<boot>/plain_NNN.txt (plain windows, kabplain.py) or prof_NNN_summary.txt (KAB_PLAIN=0). Per knob line: the A and B window frame rates of every valid boot,
the B-A change in fps and in frame time, paired over the adjacent A/B pairs of the order (ABBAAB
gives AB, BA, AB: three pairs a boot), with a paired t over all the pairs of all boots.
Both sides share each boot, so the boot-to-boot spread drops out and the bar is lower than across
builds: PERF_BAR_KNOB (default 1.5 %). A change counts when |B-A| >= the bar and the paired
p < 0.05.
"""
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from abstat import paired  # noqa: E402

RXF = re.compile(r"([\d.]+) fps average")


def fps_of(path):
    with open(path, errors="replace") as f:
        m = RXF.search(f.readline())
    return float(m.group(1)) if m else None


def main(d):
    hdr = os.path.join(d, "batch.txt")
    if os.path.exists(hdr):
        print(open(hdr).read().rstrip())
        print()
    man = []
    with open(os.path.join(d, "manifest.txt")) as f:
        for line in f:
            p = line.split(None, 2)
            if len(p) == 3:
                man.append((int(p[0]), p[1], p[2].strip()))
    boots = []
    with open(os.path.join(d, "runs.txt")) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4:
                boots.append((p[1], p[3]))
    knobs = []
    for _, _, kv in man:
        if kv not in knobs:
            knobs.append(kv)
    per = {kv: {"A": [], "B": [], "pairs": [], "dms": []} for kv in knobs}
    for name, valid in boots:
        rd = os.path.join(d, "lc_" + name)
        print(f"boot {name}: {valid}")
        if valid != "ok":
            continue
        for kv in knobs:
            seq = []
            for i, s, k2 in man:
                if k2 != kv:
                    continue
                p = os.path.join(rd, f"plain_{i:03d}.txt")
                if not os.path.exists(p):
                    p = os.path.join(rd, f"prof_{i:03d}_summary.txt")
                f = fps_of(p) if os.path.exists(p) else None
                if f is not None:
                    seq.append((s, f))
                    per[kv][s].append(f)
            print(f"  {kv:<28} " + " ".join(f"{s}{f:.1f}" for s, f in seq))
            for j in range(0, len(seq) - 1, 2):
                (s1, f1), (s2, f2) = seq[j], seq[j + 1]
                if s1 != s2:
                    a, b = (f1, f2) if s1 == "A" else (f2, f1)
                    per[kv]["pairs"].append(b - a)
                    per[kv]["dms"].append(1000.0 / b - 1000.0 / a)
    bar = float(os.environ.get("PERF_BAR_KNOB", "1.5"))
    print()
    print(f"{'knob':<28} {'A fps':>7} {'B fps':>7} {'B-A %':>7} {'d ms':>7} {'p':>6}  verdict (bar {bar:.1f}%)")
    for kv in knobs:
        x = per[kv]
        if not x["A"] or not x["B"]:
            print(f"{kv:<28} (no windows)")
            continue
        a, b = st.mean(x["A"]), st.mean(x["B"])
        _, _, pp = paired(x["pairs"])
        dms = st.mean(x["dms"]) if x["dms"] else float("nan")
        pct = 100.0 * (b - a) / a
        counts = abs(pct) >= bar and pp == pp and pp < 0.05
        word = ("B faster" if pct > 0 else "B slower") if counts else "does not count"
        print(f"{kv:<28} {a:7.2f} {b:7.2f} {pct:+7.2f} {dms:+7.3f} {pp:6.3f}  {word} ({len(x['pairs'])} pairs)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
