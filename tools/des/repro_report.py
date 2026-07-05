#!/usr/bin/env python3
"""repro_report.py -- distribution report over N boot logs (docs/18 domain D).

The boot is nondeterministic (SPU host-thread races): crash counts, thread
counts, milestone reached, event selectors all vary run-to-run. A single run
CANNOT answer "did this fix help?". This aggregates N runs and reports, per
observable, min/max/mode/stddev/#distinct + a STABLE/VARIANT tag, plus a
monotonic "max milestone reached" (fixes the milestones.sh non-monotonic grep
bug where a missed intermediate match understated progress).

Usage:
  python tools/des/repro_report.py <run_1.txt> [run_2.txt ...]
  python tools/des/repro_report.py --baseline <dirA> --candidate <dirB>

Whitelist: Bash(python tools/des/repro_report.py *)
"""
import sys, re, glob, os, statistics
from collections import Counter

# Ordered boot milestones (mirror tools/des/milestones.sh). Monotonic scoring:
# "reached" = highest index i such that milestone i OR any later one matched.
MILESTONES = [
    ("D3D12 backend ready",           r"D3D12\] Backend ready"),
    ("SPURS init",                    r"cellSpurs\] InitializeWithAttribute"),
    ("cellUserInfo stat",             r"cellUserInfo\] GetStat"),
    ("HighGraphics thread",           r'thread_create name="HighGraphics"'),
    ("Core.Res subsystem",            r'thread_create name="Core.Res.Decompressor"'),
    ("Core.Res.FileLoader",           r'thread_create name="Core.Res.FileLoader"'),
    ("Havok PPU Thread",              r'thread_create name="Havok PPU Thread"'),
    ("MO audio",                      r'thread_create name="MOMainThread"'),
    ("NexusRevolution Main",          r'thread_create name="NexusRevolution Main"'),
    ("DsSaveLoadMan",                 r'thread_create name="DsSaveLoadMan'),
    ("SLSession thread",              r'thread_create name="SLSession"'),
    ("cellGameDataCheckCreate2",      r"cellGame\] DataCheckCreate2\(version"),
    ("init worker threads exit",      r"sys_ppu_thread_exit\(tid=1[0-9] "),
    ("cellSaveDataAutoLoad2 (DIALOG)", r"AutoLoad2\(|SaveDataAutoLoad"),
]

def score_run(text):
    """Return dict of observables for one log."""
    matched = [bool(re.search(p, text)) for _, p in MILESTONES]
    # monotonic: highest index with this-or-any-later match
    reached = 0
    for i in range(len(MILESTONES)):
        if any(matched[i:]): reached = i + 1
    crash = len(re.findall(r"\[CRASH\]|Runtime error|Dantelion2 Panic|no runnable target", text))
    created = len(re.findall(r"host thread started", text))
    exited  = len(re.findall(r"sys_ppu_thread_exit|entry RETURNED", text))
    spu_enter = len(re.findall(r"async image=\d+ ENTER run", text))
    spu_ret   = len(re.findall(r"async image=\d+ RETURNED", text))
    helper_recv = len(re.findall(r"\[spurs-helper\] recv", text))
    realpost = len(re.findall(r"POST real USER event", text))
    # MAIN's final wait object (a proxy for how far MAIN advanced)
    mains = re.findall(r'name="MAIN" WAITING wait=\w+ obj=0x([0-9A-F]+)', text)
    main_obj = mains[-1] if mains else "-"
    return {
        "milestone": reached, "crash": crash, "created": created, "exited": exited,
        "spu_enter": spu_enter, "spu_ret": spu_ret, "helper_recv": helper_recv,
        "realpost": realpost, "main_wait_obj": main_obj,
    }

def load_dir_or_files(args):
    files = []
    for a in args:
        if os.path.isdir(a): files += sorted(glob.glob(os.path.join(a, "*.txt")))
        else: files += glob.glob(a)
    runs = []
    for f in files:
        try:
            with open(f, "r", errors="replace") as fh: runs.append((f, score_run(fh.read())))
        except OSError: pass
    return runs

def summarize(runs, keys):
    """key -> (min,max,mode,stdev,ndistinct, values)"""
    out = {}
    for k in keys:
        vals = [r[k] for _, r in runs]
        numeric = all(isinstance(v, (int, float)) for v in vals)
        distinct = sorted(set(vals), key=lambda x: (str(type(x)), x))
        if numeric:
            sd = round(statistics.pstdev(vals), 2) if len(vals) > 1 else 0.0
            out[k] = (min(vals), max(vals), Counter(vals).most_common(1)[0][0], sd, len(distinct), vals)
        else:
            out[k] = (None, None, Counter(vals).most_common(1)[0][0], None, len(distinct), vals)
    return out

KEYS = ["milestone","crash","created","exited","spu_enter","spu_ret","helper_recv","realpost","main_wait_obj"]

def print_table(runs):
    s = summarize(runs, KEYS)
    print(f"===== repro report: {len(runs)} runs =====")
    print(f"{'observable':16s} {'min':>6s} {'max':>6s} {'mode':>8s} {'stdev':>7s} {'#uniq':>5s}  tag")
    for k in KEYS:
        mn, mx, mode, sd, nd, _ = s[k]
        tag = "STABLE" if nd == 1 else "VARIANT"
        mns = "-" if mn is None else str(mn); mxs = "-" if mx is None else str(mx)
        sds = "-" if sd is None else str(sd)
        print(f"{k:16s} {mns:>6s} {mxs:>6s} {str(mode):>8s} {sds:>7s} {nd:>5d}  {tag}")
    return s

def compare(base, cand):
    sb = summarize(base, KEYS); sc = summarize(cand, KEYS)
    print(f"\n===== compare: baseline({len(base)}) vs candidate({len(cand)}) =====")
    for k in KEYS:
        bmn,bmx,bmode,_,_,_ = sb[k]; cmn,cmx,cmode,_,_,_ = sc[k]
        verdict = "within-noise"
        if k in ("milestone","helper_recv","spu_ret"):
            if isinstance(cmode,(int,float)) and isinstance(bmode,(int,float)):
                if cmode > bmode: verdict = "IMPROVED"
                elif cmode < bmode: verdict = "REGRESSED"
        if k == "crash" and isinstance(cmode,(int,float)) and isinstance(bmode,(int,float)):
            if cmode < bmode: verdict = "IMPROVED (fewer crashes)"
            elif cmode > bmode: verdict = "REGRESSED (more crashes)"
        print(f"  {k:16s} base_mode={bmode} cand_mode={cmode}  -> {verdict}")

def main():
    a = sys.argv[1:]
    if "--baseline" in a and "--candidate" in a:
        bi = a.index("--baseline"); ci = a.index("--candidate")
        base = load_dir_or_files([a[bi+1]]); cand = load_dir_or_files([a[ci+1]])
        if not base or not cand: print("empty baseline/candidate"); return 1
        print("BASELINE:"); print_table(base); print("\nCANDIDATE:"); print_table(cand)
        compare(base, cand); return 0
    runs = load_dir_or_files(a)
    if not runs: print("no run logs found"); return 1
    print_table(runs)
    return 0

if __name__ == "__main__":
    sys.exit(main())
