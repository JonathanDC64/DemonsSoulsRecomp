#!/usr/bin/env python3
"""capdiff.py <cap.rcap> [--show N]: what a live frame rendered from vs its replay.

An F9 capture writes, next to cap_NNN.rcap, the backend's per-draw fingerprints
of the live batches holding the captured frame: cap_NNN_cap0_d<n>.txt (where
the capture starts) and, when it ends in a later batch, cap_NNN_cap1_d<n>.txt.
This replays the capture with RSX_DRAWPRINT, takes the live records between
the two marks, and lines them up with the replay's.

Each record line carries the inputs the draw rendered from: emitted vertices
(vb), VP constants (cb), the program's inline FP constants (fcb), pipeline
keys, and per texture unit what it resolved to (T<raw>:<checksum of the guest
bytes>/<hash of the decoded bytes uploaded>, R<offset> for an offscreen
target, Z<offset> for a depth surface).
Records that differ in structure (a draw missing or added) are listed first,
then the hash fields that differ on records that otherwise match.

Identical inputs with a different picture mean GPU-side state the replay
starts without: a target or depth surface left from an earlier frame, a
cached texture.
"""
import collections
import difflib
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPLAY = os.path.join(ROOT, "ps3recomp", "build", "rsx_replay.exe")
HASHES = re.compile(r" (vb|cb|fcb)=([0-9A-F]+)")
TEXSUMS = re.compile(r" (t\d+)=T[0-9A-F]+:([0-9A-F]+)(?:/([0-9A-F]+))?")


def body(line):
    return line.split(" ", 1)[1] if " " in line else line


def marked(path):
    lines = open(path).read().splitlines()
    marks = dict((m.group(1), int(m.group(2))) for m in
                 (re.match(r"# capture (starts|ends) at record (\d+)", l) for l in lines) if m)
    return [l for l in lines if not l.startswith("#")], marks


def live_records(cap):
    base = cap[:-5] if cap.endswith(".rcap") else cap
    c0 = glob.glob(base + "_cap0_d*.txt")
    if not c0:
        sys.exit("no %s_cap0_d*.txt: capture it with a build that writes fingerprints" % base)
    rows, marks = marked(c0[0])
    if "ends" in marks:                       # started and ended in one batch
        recs = rows[marks.get("starts", 0):marks["ends"]]
    else:
        recs = rows[marks.get("starts", 0):]
        c1 = glob.glob(base + "_cap1_d*.txt")
        if c1:
            rows1, marks1 = marked(c1[0])
            recs += rows1[:marks1.get("ends", len(rows1))]
    return [body(r) for r in recs]


def replay_records(cap):
    d = tempfile.mkdtemp(prefix="capdiff")
    env = dict(os.environ, RSX_DRAWPRINT=os.path.join(d, "r").replace("\\", "/"),
               RSX_DRAWPRINT_N="2")
    subprocess.run([REPLAY, cap, "2"], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=600)
    files = sorted(glob.glob(os.path.join(d, "r_*_d*.txt")),
                   key=lambda p: -int(re.search(r"_d(\d+)\.txt$", p).group(1)))
    if not files:
        sys.exit("the replay wrote no fingerprints")
    return [body(l) for l in open(files[0]).read().splitlines() if not l.startswith("#")]


def main():
    args = sys.argv[1:]
    show = 8
    if "--show" in args:
        i = args.index("--show"); show = int(args[i + 1]); del args[i:i + 2]
    cap = args[0]
    L, R = live_records(cap), replay_records(cap)
    # The K detail (RSX_DRAWPRINT_K) is in the hashes already; one side may lack it.
    strip = lambda l: re.sub(r"(=T[0-9A-F]+):[0-9A-F/]+", r"\1",
                             HASHES.sub("", l.split(" at=(")[0]))
    sm = difflib.SequenceMatcher(None, [strip(x) for x in L], [strip(x) for x in R],
                                 autojunk=False)
    print("live %d records, replay %d" % (len(L), len(R)))
    nd = 0
    for op, a0, a1, b0, b1 in sm.get_opcodes():
        if op == "equal":
            continue
        nd += 1
        if nd <= show:
            print("%s live %d-%d / replay %d-%d" % (op, a0, a1, b0, b1))
            for x in L[a0:a0 + 2]:
                print("  live:   " + x[:200])
            for x in R[b0:b0 + 2]:
                print("  replay: " + x[:200])
    print("structural differences: %d" % nd)
    cnt, first = collections.Counter(), {}
    for op, a0, a1, b0, b1 in sm.get_opcodes():
        if op != "equal":
            continue
        for i, j in zip(range(a0, a1), range(b0, b1)):
            a, b = dict(HASHES.findall(" " + L[i])), dict(HASHES.findall(" " + R[j]))
            # Per unit: the guest-bytes checksum and the uploaded-bytes hash
            # (captures from before the hash have none; the checksum's
            # sampling changed with it, so older ones differ there).
            for x, y in ((a, L[i]), (b, R[j])):
                for u, cs, uh in TEXSUMS.findall(" " + y):
                    x["sum " + u] = cs
                    if uh: x["up " + u] = uh
            for k in [k for k in a if k.startswith("up ") and k not in b]:
                del a[k]
            for k in a:
                if a[k] != b.get(k):
                    cnt[k] += 1
                    first.setdefault(k, (i, L[i][:160]))
    print("hash differences: %s" % (dict(cnt) or "none"))
    for k, (i, x) in first.items():
        print("  first %s at live record %d: %s" % (k, i, x))


if __name__ == "__main__":
    main()
