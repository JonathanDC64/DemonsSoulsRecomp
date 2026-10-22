#!/usr/bin/env python3
"""kabplain.py <run dir>: knobab.sh's plain windows (KAB_PLAIN=1, tools/des/PERF.md).

Reads <run>/plainwin.txt, one line per window: "<window> <flips at its start> <flips at its end>"
(the console's `stat`), and <run>/frames.log (FRAME_LOG: one line per present, its time t in ms and
the flip words it took). The flip counts locate the window in frames.log, so no clock has to
agree, and the frame rate is the presents between the two over their frames.log time. Writes
<run>/plain_NNN.txt, "<s> s, <n> presents: <fps> fps average", the line knobstat.py reads.
"""
import bisect
import os
import sys


def main(rd):
    t, cum = [], []
    c = 0
    with open(os.path.join(rd, "frames.log"), encoding="utf-8") as f:
        next(f, None)   # the header line
        for line in f:
            p = line.split()
            if len(p) < 7:
                continue
            try:
                tt, fl = float(p[0]), int(p[5])
            except ValueError:
                continue
            c += fl
            t.append(tt)
            cum.append(c)
    bad = 0
    with open(os.path.join(rd, "plainwin.txt"), encoding="utf-8") as f:
        for line in f:
            p = line.split()
            try:
                w, f1, f2 = int(p[0]), int(p[1]), int(p[2])
            except (IndexError, ValueError):
                print(f"skipped: {line.strip()!r}")
                bad += 1
                continue
            i1, i2 = bisect.bisect_left(cum, f1), bisect.bisect_left(cum, f2)
            if i2 >= len(t) or i2 - i1 < 10 or t[i2] <= t[i1]:
                print(f"window {w}: flips {f1}-{f2} are not in frames.log (it has {cum[-1] if cum else 0})")
                bad += 1
                continue
            dur, n = (t[i2] - t[i1]) / 1000.0, i2 - i1
            with open(os.path.join(rd, f"plain_{w:03d}.txt"), "w", encoding="utf-8") as o:
                o.write(f"{dur:.2f} s, {n} presents: {n / dur:.2f} fps average (plain window, flips {f1}-{f2})\n")
            print(f"window {w}: {n} presents in {dur:.2f} s, {n / dur:.2f} fps")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
