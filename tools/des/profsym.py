#!/usr/bin/env python3
"""profsym.py <map> <prof.out>: symbolize a debug-console `prof` dump.

Sums the exclusive section's "rva=0x..." entries by function, using the
linker map des_boot writes (port/build/des_boot.map; VA = rva + 0x140000000).
"""
import bisect
import re
import sys


def main(mapf, pf):
    syms = []
    with open(mapf, errors="replace") as f:
        for line in f:
            m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})", line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    keys = [a for a, _ in syms]

    def name(rva):
        i = bisect.bisect_right(keys, rva + 0x140000000) - 1
        n = syms[i][1] if i >= 0 else "?"
        m = re.match(r"\?([A-Za-z_0-9]+)@@", n)
        return m.group(1) if m else n

    tot, sec = {}, None
    with open(pf, errors="replace") as f:
        for line in f:
            if "--- exclusive" in line:
                sec = "x"
                continue
            if "--- inclusive" in line:
                sec = None
            m = re.match(r"\s+([\d.]+)%\s+(\S+)", line)
            if sec == "x" and m:
                w = m.group(2)
                n = name(int(w[6:], 16)) if w.startswith("rva=") else w
                tot[n] = tot.get(n, 0) + float(m.group(1))
    for n, p in sorted(tot.items(), key=lambda x: -x[1]):
        print(f"{p:6.1f}%  {n}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
