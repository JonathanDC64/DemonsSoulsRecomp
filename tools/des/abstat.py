#!/usr/bin/env python3
"""abstat.py <batch dir>: summarize an exeab.sh batch (tools/des/PERF.md).

Reads <dir>/runs.txt (side, boot, round, validity, background cpu) and, per boot, lc_<boot>/:
prof_*_summary.txt and prof_*_frames.txt (the profile windows) and frames.log (FRAME_LOG, one line
per present over the whole boot). Two frame rates per window:
  prof  the profile session's own average (what docs/28 reports). A session perturbs what it
        measures: it times every texture, SRV and pipeline lookup on the FIFO thread with
        QueryPerformanceCounter and times every guest syscall (the `waits` table).
  gap   the presents between two sessions, from frames.log, 1 s in from either end: nothing armed.
        Each session is located in frames.log by matching its per-present draw counts and
        intervals, so no clock has to agree.
Per metric: per-side stats over windows and over boots, per-round deltas, and the verdict numbers:
median B vs A, rounds where B was faster, paired t on the round deltas, Welch t on the boot means.
The boot (mean of its windows) is the unit for the tests: windows in one boot are not independent.
A window under 0.6 x its side's median (a cutscene's 30 fps lock, a stall) is dropped and listed.
Noise bars, the boot-to-boot spread of one build across a batch: PERF_BAR_PROF (default 2.8 %,
profile fps) and PERF_BAR_GAP (default 4.5 %, gap fps); re-measure them with an A/A batch (the same
exe on both sides) after a big change. A gain counts when |median delta| >= the bar, the paired
p < 0.05, and B is on the same side in all but at most one round (at least 3 rounds).
"""
import math
import os
import re
import statistics as st
import sys


def betacf(a, b, x):
    fpmin = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > fpmin else fpmin)
    h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > fpmin else fpmin)
        c = 1.0 + aa / c
        c = c if abs(c) > fpmin else fpmin
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > fpmin else fpmin)
        c = 1.0 + aa / c
        c = c if abs(c) > fpmin else fpmin
        de = d * c
        h *= de
        if abs(de - 1.0) < 3e-14:
            break
    return h


def betai(a, b, x):
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1.0 - x))
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * betacf(a, b, x) / a
    return 1.0 - bt * betacf(b, a, 1.0 - x) / b


def t_p(t, df):
    """two-sided p of Student's t"""
    if df != df or df <= 0 or t != t:
        return float("nan")
    if math.isinf(t):
        return 0.0
    return betai(df / 2.0, 0.5, df / (df + t * t))


def welch(a, b):
    if len(a) < 2 or len(b) < 2:
        return float("nan"), float("nan"), float("nan")
    va, vb = st.variance(a) / len(a), st.variance(b) / len(b)
    if va + vb == 0:
        return float("inf"), float("nan"), 0.0
    t = (st.mean(b) - st.mean(a)) / math.sqrt(va + vb)
    df = (va + vb) ** 2 / ((va * va) / (len(a) - 1) + (vb * vb) / (len(b) - 1))
    return t, df, t_p(t, df)


def paired(d):
    if len(d) < 2:
        return float("nan"), float("nan"), float("nan")
    sd = st.stdev(d)
    if sd == 0:
        return float("inf"), len(d) - 1, 0.0
    t = st.mean(d) / (sd / math.sqrt(len(d)))
    return t, len(d) - 1, t_p(t, len(d) - 1)


def desc(xs):
    if not xs:
        return "n=0"
    sd = st.stdev(xs) if len(xs) > 1 else 0.0
    return (f"n={len(xs):2d} mean {st.mean(xs):7.2f} median {st.median(xs):7.2f} sd {sd:5.2f} "
            f"min {min(xs):7.2f} max {max(xs):7.2f} spread {max(xs) - min(xs):5.2f} "
            f"({100.0 * (max(xs) - min(xs)) / st.median(xs):.1f}%)")


RX1 = re.compile(r"([\d.]+) s, (\d+) presents: ([\d.]+) fps average, 1% low ([\d.]+) fps")
RX2 = re.compile(r"present interval ms: median ([\d.]+)\s+p90 ([\d.]+)\s+p99 ([\d.]+)")
RX3 = re.compile(r"render_frame \(frame start -> Present\) ms: median ([\d.]+)")


def read_rows(path, draws_col=6, dt_col=1, t_col=0):
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) <= draws_col or not p[0][0].isdigit():
                continue
            try:
                rows.append((float(p[t_col]), float(p[dt_col]), int(p[draws_col])))
            except ValueError:
                pass
    return rows


def read_window(summary):
    w = {}
    with open(summary, errors="replace") as f:
        for line in f:
            m = RX1.search(line)
            if m and "fps" not in w:
                w["fps"], w["low"] = float(m.group(3)), float(m.group(4))
            m = RX2.search(line)
            if m:
                w["med_ms"], w["p99"] = float(m.group(1)), float(m.group(3))
            m = RX3.search(line)
            if m:
                w["rf"] = float(m.group(1))
    fr = summary.replace("_summary.txt", "_frames.txt")
    if os.path.exists(fr):
        w["rows"] = read_rows(fr)
        if w["rows"]:
            w["draws"] = st.median([r[2] for r in w["rows"]])
    return w if "fps" in w else None


def locate(log, rows, k=60):
    """index in log of the session's first present, or None"""
    if len(rows) < k + 1 or len(log) < k + 1:
        return None
    want = rows[1:k + 1]
    best, at = 0, None
    for i in range(1, len(log) - k):
        s = 0
        for j in range(k):
            lg, pr = log[i + j], want[j]
            if lg[2] == pr[2] and abs(lg[1] - pr[1]) < 0.15:
                s += 1
        if s > best:
            best, at = s, i
            if s == k:
                break
    return at - 1 if at is not None and best >= 0.9 * k else None


def gap_fps(log, a, b):
    """presents/s over log rows with t in [a, b] (ms)"""
    ts = [r[0] for r in log if a <= r[0] <= b]
    if len(ts) < 30 or ts[-1] <= ts[0]:
        return None
    return 1000.0 * (len(ts) - 1) / (ts[-1] - ts[0])


def boot_gaps(rd, wins):
    """the frame rate of the gap after each session (1 s margins, at most 8 s), from frames.log"""
    fl = os.path.join(rd, "frames.log")
    if not os.path.exists(fl):
        return []
    log = read_rows(fl)
    spans = []
    for w in wins:
        i = locate(log, w.get("rows", []))
        if i is None:
            return []
        spans.append((log[i][0], log[min(i + len(w["rows"]) - 1, len(log) - 1)][0]))
    out = []
    for n, (s0, s1) in enumerate(spans):
        end = spans[n + 1][0] - 1000.0 if n + 1 < len(spans) else log[-1][0] - 1000.0
        g = gap_fps(log, s1 + 1000.0, min(end, s1 + 9000.0))
        if g:
            out.append(g)
    return out


def verdict(runs, key, label, bar):
    vals = {s: [x for r in runs if r["side"] == s and r["valid"] == "ok" for x in r[key]] for s in "AB"}
    med = {s: st.median(v) if v else 0.0 for s, v in vals.items()}
    dropped = []
    for r in runs:
        r[key + "_keep"] = []
        if r["valid"] != "ok":
            continue
        for i, x in enumerate(r[key]):
            if med[r["side"]] and x < 0.6 * med[r["side"]]:
                dropped.append(f"{r['name']} {label} window {i + 1}: {x:.1f}")
            else:
                r[key + "_keep"].append(x)
        r[key + "_mean"] = st.mean(r[key + "_keep"]) if r[key + "_keep"] else None
    print(f"== {label} fps")
    if dropped:
        print("  dropped (< 0.6 x median): " + "; ".join(dropped))
    for s in "AB":
        wins = [x for r in runs if r["side"] == s for x in r[key + "_keep"]]
        boots = [r[key + "_mean"] for r in runs if r["side"] == s and r.get(key + "_mean") is not None]
        print(f"  {s} windows: {desc(wins)}")
        print(f"  {s} boots:   {desc(boots)}")
    deltas = []
    for rn in sorted({r["round"] for r in runs}):
        a = [r[key + "_mean"] for r in runs if r["round"] == rn and r["side"] == "A" and r.get(key + "_mean") is not None]
        b = [r[key + "_mean"] for r in runs if r["round"] == rn and r["side"] == "B" and r.get(key + "_mean") is not None]
        if a and b:
            d = st.mean(b) - st.mean(a)
            deltas.append(d)
            print(f"  round {rn}: A {st.mean(a):7.2f}  B {st.mean(b):7.2f}  B-A {d:+6.2f} ({100.0 * d / st.mean(a):+.2f}%)")
        else:
            print(f"  round {rn}: incomplete")
    aw = [x for r in runs if r["side"] == "A" for x in r[key + "_keep"]]
    bw = [x for r in runs if r["side"] == "B" for x in r[key + "_keep"]]
    ab = [r[key + "_mean"] for r in runs if r["side"] == "A" and r.get(key + "_mean") is not None]
    bb = [r[key + "_mean"] for r in runs if r["side"] == "B" and r.get(key + "_mean") is not None]
    if not ab or not bb:
        print("  verdict: not enough valid boots")
        return
    med_d = st.median(bw) - st.median(aw)
    pct = 100.0 * med_d / st.median(aw)
    pos, neg = sum(1 for x in deltas if x > 0), sum(1 for x in deltas if x < 0)
    tp, dfp, pp = paired(deltas)
    tw, dfw, pw = welch(ab, bb)
    same = len(deltas) >= 3 and max(pos, neg) >= len(deltas) - 1
    counts = abs(pct) >= bar and pp == pp and pp < 0.05 and same
    print(f"  median B vs A: {med_d:+.2f} fps ({pct:+.2f}%); B faster in {pos} of {len(deltas)} rounds, slower in {neg}")
    print(f"  paired t (round deltas): t={tp:.2f} df={dfp:.0f} p={pp:.3f};  Welch t (boot means): t={tw:.2f} df={dfw:.1f} p={pw:.3f}")
    print(f"  noise bar {bar:.2f}%: {'COUNTS' if counts else 'does not count'} "
          f"({'B faster' if med_d > 0 else 'B slower'}{'' if counts else ': inside the bar, not significant, or not consistent'})")


def main(d):
    runs = []
    with open(os.path.join(d, "runs.txt")) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4:
                runs.append({"side": p[0], "name": p[1], "round": int(p[2]), "valid": p[3],
                             "bg": p[4] if len(p) > 4 else "?"})
    hdr = os.path.join(d, "batch.txt")
    if os.path.exists(hdr):
        print(open(hdr).read().rstrip())
        print()
    for r in runs:
        rd = os.path.join(d, "lc_" + r["name"])
        wins = []
        if os.path.isdir(rd):
            for fn in sorted(os.listdir(rd)):
                if re.match(r"prof_\d+_summary\.txt$", fn):
                    w = read_window(os.path.join(rd, fn))
                    if w:
                        wins.append(w)
        r["wins"] = wins
        r["prof"] = [w["fps"] for w in wins]
        r["gap"] = boot_gaps(rd, wins) if os.path.isdir(rd) else []
    print("boots:")
    for r in runs:
        print(f"  {r['side']} {r['name']:<22} round {r['round']}  {r['valid']:<8} bg {r['bg']:>4}%  "
              f"prof {' '.join(f'{x:.1f}' for x in r['prof']):<12}  gap {' '.join(f'{x:.1f}' for x in r['gap'])}")
    for s in "AB":
        ws = [w for r in runs if r["side"] == s and r["valid"] == "ok" for w in r["wins"]]
        extra = []
        for k, lab in (("low", "1% low"), ("med_ms", "interval median ms"), ("rf", "render_frame ms"),
                       ("draws", "draws/frame")):
            v = [w[k] for w in ws if k in w]
            if v:
                extra.append(f"{lab} {st.median(v):.2f}")
        print(f"  {s} (profile windows): " + ", ".join(extra))
    print()
    verdict(runs, "prof", "profile-window", float(os.environ.get("PERF_BAR_PROF", "2.8")))
    if any(r["gap"] for r in runs):
        print()
        verdict(runs, "gap", "between-window (unperturbed)", float(os.environ.get("PERF_BAR_GAP", "4.5")))
    else:
        print("\n(no frames.log gaps: FRAME_LOG was not set, or a session could not be located)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
