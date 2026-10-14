#!/usr/bin/env python3
"""shotdiff.py <good-dir> <new-dir> [--out <diff-dir>] [--pct N]

Compare two sets of regression screenshots (same file names, from
regress.sh) and print a per-shot summary:

  shot          mean   changed   luma(good->new)  verdict
  s1_spawn.bmp  4.1    6.2%      38 -> 37         ok

mean     mean absolute difference per channel (0-255), after a 4x box
         downscale so a moved enemy or a flickering flame counts for less
         than a pass that stopped drawing.
changed  share of the downscaled pixels whose largest channel moved by more
         than 32.
verdict  REGRESSION when changed > --pct (default 25) or the new shot went
         near-black (mean luma < 4) while the good one was not; MISSING when
         a good shot has no counterpart.

Gameplay is not frame-exact (the route is walked with timed stick holds), so
small differences are expected; the threshold is for "a pass broke", not
"a pixel moved". With --out, a heat-map PNG per shot (diff x4, red) goes there.
Exit status: 1 if any shot regressed or is missing, else 0.

Screenshots are game-derived: keep both sets outside the repository.
"""
import argparse, os, sys
import numpy as np
from PIL import Image


def load(p):
    a = np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)
    h, w = a.shape[0] // 4 * 4, a.shape[1] // 4 * 4
    return a[:h, :w].reshape(h // 4, 4, w // 4, 4, 3).mean(axis=(1, 3))


def luma(a):
    return float((a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('good'); ap.add_argument('new')
    ap.add_argument('--out'); ap.add_argument('--pct', type=float, default=25.0)
    o = ap.parse_args()
    names = sorted(f for f in os.listdir(o.good) if f.lower().endswith(('.bmp', '.png')))
    if not names:
        print('no shots in', o.good); return 1
    if o.out: os.makedirs(o.out, exist_ok=True)
    bad = 0
    print('%-22s %6s %8s %16s  %s' % ('shot', 'mean', 'changed', 'luma(good->new)', 'verdict'))
    for n in names:
        q = os.path.join(o.new, n)
        if not os.path.exists(q):
            print('%-22s %6s %8s %16s  MISSING' % (n, '-', '-', '-')); bad += 1; continue
        g, c = load(os.path.join(o.good, n)), load(q)
        if g.shape != c.shape:
            print('%-22s size %s vs %s  REGRESSION' % (n, g.shape[:2], c.shape[:2])); bad += 1; continue
        d = np.abs(g - c)
        mean = float(d.mean()); pct = 100.0 * float((d.max(axis=2) > 32).mean())
        lg, lc = luma(g), luma(c)
        reg = pct > o.pct or (lc < 4 and lg >= 4)
        bad += reg
        print('%-22s %6.1f %7.1f%% %7.0f -> %-6.0f  %s' % (n, mean, pct, lg, lc, 'REGRESSION' if reg else 'ok'))
        if o.out:
            heat = np.zeros_like(d); heat[..., 0] = np.clip(d.max(axis=2) * 4, 0, 255)
            Image.fromarray(heat.astype(np.uint8)).save(os.path.join(o.out, os.path.splitext(n)[0] + '_diff.png'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
