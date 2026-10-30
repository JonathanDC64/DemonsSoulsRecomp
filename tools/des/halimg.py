#!/usr/bin/env python3
"""halimg.py <a> <b> [--mode exact|vk|hdr] [--tol N] [--out <dir>] [--quiet]
halimg.py --selftest

Compare two images, or two folders of same-named images (BMP or PNG, and the
backend's HDRP snapshots: "HDRP <w> <h>" then RGBA16F pixels, RSX_HDR_SNAP),
for the renderer gates (halgate.sh). In a folder every file is matched by its
path under the folder, subfolders included; a file on one side only fails.

  exact  the pixels are identical (every channel, alpha too). Reports how many
         pixels differ. For a route that must be bit-identical (hal-d3d12).
  vk     the rule for a route that may round differently (Vulkan), all three:
           - at least 99.95% of pixels within +-tol/255 on every channel
             (--tol, default 1; 2 for the D32S8 tests);
           - no 16x16 block whose mean absolute difference (over its pixels
             and channels, in 0-255 steps) is above 1.0; edge blocks are
             averaged over the pixels they have;
           - at most 0.01% of pixels more than 8/255 off on any channel.
         An HDRP pair is held to the hdr rule instead.
  hdr    HDRP pairs: every channel within 1e-3 of the other, relative to the
         larger of 1 and its value (--hdr-tol). Other images as vk.

One line per image: its size, the pixels that differ, the share within
tolerance, the worst block mean, the share over 8/255, the largest difference,
and ok / FAIL. The last line is "halimg: images=N failed=M". With --out, a
heat map per image that differs, <name>_heat.png: dark blue for 1, blue for
up to the tolerance, red for more (brighter is larger), white above 8, and
the failing 16x16 blocks outlined in yellow. --quiet prints the failures and
the last line only.

Exit status: 0 every image passes, 1 one fails or is missing, 2 bad arguments
or an image that cannot be read.

The images are game-derived: keep them, and the heat maps, outside the
repository.
"""
import argparse
import os
import sys
import tempfile

import numpy as np
from PIL import Image

EXTS = ('.bmp', '.png', '.hdrp')
BLOCK = 16


class BadImage(Exception):
    pass


def load(path):
    """An HxWxC array: uint8 for BMP/PNG (RGB or RGBA as stored), float32 for HDRP."""
    if path.lower().endswith('.hdrp'):
        with open(path, 'rb') as f:
            head = f.readline().split()
            if len(head) != 3 or head[0] != b'HDRP':
                raise BadImage('%s: not an HDRP snapshot' % path)
            w, h = int(head[1]), int(head[2])
            raw = f.read()
        if len(raw) != w * h * 8:
            raise BadImage('%s: %d bytes of pixels, want %d' % (path, len(raw), w * h * 8))
        return np.frombuffer(raw, dtype='<f2').reshape(h, w, 4).astype(np.float32)
    try:
        im = Image.open(path)
        im.load()
    except Exception as e:  # PIL raises several types
        raise BadImage('%s: %s' % (path, e))
    if im.mode not in ('RGB', 'RGBA'):
        im = im.convert('RGBA' if 'A' in im.mode else 'RGB')
    return np.asarray(im)


def block_means(d):
    """Mean of d (HxW, already averaged over channels) per 16x16 block."""
    h, w = d.shape
    ys, xs = np.arange(0, h, BLOCK), np.arange(0, w, BLOCK)
    s = np.add.reduceat(np.add.reduceat(d, ys, axis=0), xs, axis=1)
    ch = np.minimum(BLOCK, h - ys)[:, None]
    cw = np.minimum(BLOCK, w - xs)[None, :]
    return s / (ch * cw)


def compare(a, b, mode, tol, hdr_tol):
    """A dict with the measures and 'ok'. a and b have the same shape."""
    r = {'size': '%dx%d' % (a.shape[1], a.shape[0])}
    if a.dtype == np.float32:   # HDRP
        diff = np.abs(a - b)
        r['differ'] = int((a != b).any(axis=2).sum())
        r['max'] = float(diff.max()) if diff.size else 0.0
        if mode == 'exact':
            r['ok'] = r['differ'] == 0
        else:
            lim = hdr_tol * np.maximum(1.0, np.maximum(np.abs(a), np.abs(b)))
            over = (diff > lim).any(axis=2)
            r['within'] = 100.0 - 100.0 * float(over.mean())
            r['ok'] = not over.any()
        return r
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    dmax = d.max(axis=2)
    n = dmax.size
    r['differ'] = int((dmax > 0).sum())
    r['max'] = int(dmax.max()) if n else 0
    r['within'] = 100.0 * float((dmax <= tol).sum()) / n if n else 100.0
    r['over8'] = 100.0 * float((dmax > 8).sum()) / n if n else 0.0
    bm = block_means(d.mean(axis=2, dtype=np.float64)) if n else np.zeros((0, 0))
    r['block'] = float(bm.max()) if bm.size else 0.0
    r['bad_blocks'] = bm > 1.0
    if mode == 'exact':
        r['ok'] = r['differ'] == 0
    else:
        r['ok'] = r['within'] >= 99.95 and r['block'] <= 1.0 and r['over8'] <= 0.01
    r['_d'] = dmax
    return r


def heat(r, path, tol):
    d = r.get('_d')
    if d is None:
        return
    h, w = d.shape
    img = np.zeros((h, w, 3), np.uint8)
    one = d == 1
    small = (d > 1) & (d <= tol)
    big = (d > tol) & (d <= 8)
    huge = d > 8
    img[one] = (0, 0, 110)
    img[small] = (40, 40, 255)
    img[big, 0] = np.clip(120 + d[big].astype(np.int32) * 16, 0, 255)
    img[huge] = (255, 255, 255)
    bb = r.get('bad_blocks')
    if bb is not None:
        for by, bx in zip(*np.nonzero(bb)):
            y0, x0 = by * BLOCK, bx * BLOCK
            y1, x1 = min(y0 + BLOCK, h) - 1, min(x0 + BLOCK, w) - 1
            img[y0, x0:x1 + 1] = img[y1, x0:x1 + 1] = (255, 220, 0)
            img[y0:y1 + 1, x0] = img[y0:y1 + 1, x1] = (255, 220, 0)
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    Image.fromarray(img).save(path)


def listing(root):
    out = []
    for dp, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith(EXTS):
                out.append(os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'))
    return sorted(out)


def line(name, r):
    if r is None:
        return '%-44s %s' % (name, 'MISSING')
    if 'error' in r:
        return '%-44s %s  FAIL' % (name, r['error'])
    parts = ['%-44s %-10s differ %-8d' % (name, r['size'], r['differ'])]
    if 'within' in r:
        parts.append('within %8.4f%%' % r['within'])
    if 'block' in r:
        parts.append('block %6.3f' % r['block'])
    if 'over8' in r:
        parts.append('>8 %7.4f%%' % r['over8'])
    parts.append(('max %g' % r['max']) if isinstance(r['max'], float) else ('max %3d' % r['max']))
    parts.append('ok' if r['ok'] else 'FAIL')
    return '  '.join(parts)


def run(a, b, mode, tol, hdr_tol, out, quiet, show=print):
    """Compare files or folders; returns the exit status."""
    if os.path.isdir(a) != os.path.isdir(b):
        show('halimg: %s and %s are not both folders or both files' % (a, b))
        return 2
    if os.path.isdir(a):
        la, lb = listing(a), listing(b)
        names = sorted(set(la) | set(lb))
        pairs = [(n, os.path.join(a, n) if n in la else None, os.path.join(b, n) if n in lb else None)
                 for n in names]
    else:
        for p in (a, b):
            if not os.path.isfile(p):
                show('halimg: no file %s' % p)
                return 2
        pairs = [(os.path.basename(a), a, b)]
    if not pairs:
        show('halimg: no images in %s' % a)
        return 1
    failed = 0
    unreadable = 0
    for n, pa, pb in pairs:
        if pa is None or pb is None:
            failed += 1
            show(line(n + (' (only in b)' if pa is None else ' (only in a)'), None))
            continue
        try:
            ia, ib = load(pa), load(pb)
        except BadImage as e:
            unreadable += 1
            failed += 1
            show(line(n, {'error': str(e)}))
            continue
        if ia.shape != ib.shape or ia.dtype != ib.dtype:
            failed += 1
            show(line(n, {'error': 'shape %s vs %s' % (ia.shape, ib.shape)}))
            continue
        m = mode
        if ia.dtype == np.float32 and mode == 'vk':
            m = 'hdr'
        r = compare(ia, ib, m, tol, hdr_tol)
        if not r['ok']:
            failed += 1
        if not quiet or not r['ok']:
            show(line(n, r))
        if out and r['differ']:
            heat(r, os.path.join(out, os.path.splitext(n)[0] + '_heat.png'), tol)
    show('halimg: images=%d failed=%d mode=%s tol=%d' % (len(pairs), failed, mode, tol))
    if unreadable:
        return 2
    return 1 if failed else 0


def selftest():
    """Synthetic pairs, each with the verdict every mode must give."""
    rng = np.random.default_rng(1234)
    base = rng.integers(16, 240, size=(720, 1280, 3), dtype=np.uint8)
    n = base.shape[0] * base.shape[1]

    def noisy(share, amp):
        m = rng.random(base.shape[:2]) < share
        s = rng.choice([-amp, amp], size=base.shape[:2])
        out = base.astype(np.int16)
        out[m] += s[m][:, None]
        return np.clip(out, 0, 255).astype(np.uint8)

    one = base.copy(); one[100, 200, 1] ^= 1                      # one pixel, one step
    blk = base.astype(np.int16); blk[64:80, 64:80] += 2           # one 16x16 block, +2
    blk = np.clip(blk, 0, 255).astype(np.uint8)
    spike = base.copy()                                           # 4 pixels +80 in one block
    for y, x in ((290, 290), (291, 295), (296, 292), (300, 301)):
        spike[y, x] = (spike[y, x].astype(np.int16) + 80) % 256
    spread = base.astype(np.int16)                                # 0.02% of pixels +20, one per block
    k = int(n * 0.0002) + 1
    ys = (np.arange(k) * 16) % 720 + 3
    xs = ((np.arange(k) * 16) // 720) * 16 % 1280 + 5
    spread[ys, xs, 2] += 20
    spread = np.clip(spread, 0, 255).astype(np.uint8)
    cases = [
        # name, b image, tol, exact ok, vk ok
        ('identical', base.copy(), 1, True, True),
        ('one pixel off by 1', one, 1, False, True),
        ('half the pixels +-1', noisy(0.5, 1), 1, False, True),
        ('1% of pixels +-2, tol 1', noisy(0.01, 2), 1, False, False),
        ('1% of pixels +-2, tol 2', noisy(0.01, 2), 2, False, True),
        ('a 16x16 block +2, tol 2', blk, 2, False, False),
        ('4 spikes in one block', spike, 1, False, False),
        ('0.02% of pixels +20, spread', spread, 1, False, False),
    ]
    bad = 0

    def check(what, got, want):
        nonlocal bad
        ok = got == want
        bad += not ok
        print('  %-48s %-5s %s' % (what, 'pass' if got else 'fail', 'ok' if ok else 'WRONG (want %s)' % want))

    print('halimg self-test')
    for name, b, tol, want_exact, want_vk in cases:
        check(name + ' / exact', compare(base, b, 'exact', tol, 1e-3)['ok'], want_exact)
        check(name + ' / vk', compare(base, b, 'vk', tol, 1e-3)['ok'], want_vk)
    # The two rules the cases above isolate, checked by their measures.
    r = compare(base, spike, 'vk', 1, 1e-3)
    check('spikes: within and >8 pass, the block fails',
          r['within'] >= 99.95 and r['over8'] <= 0.01 and r['block'] > 1.0, True)
    r = compare(base, spread, 'vk', 1, 1e-3)
    check('spread: within and block pass, >8 fails',
          r['within'] >= 99.95 and r['block'] <= 1.0 and r['over8'] > 0.01, True)
    r = compare(base, one, 'exact', 1, 1e-3)
    check('one pixel off counts 1 differing pixel', r['differ'] == 1, True)
    # Edge blocks: a 20x20 image has a 4-wide right and bottom edge.
    e = np.zeros((20, 20, 3), np.uint8); f = e.copy(); f[17:20, 17:20] = 3
    check('edge block averaged over its own pixels', abs(block_means(np.abs(
        e.astype(np.int16) - f.astype(np.int16)).mean(axis=2))[1, 1] - 3 * 9 / 16) < 1e-9, True)

    with tempfile.TemporaryDirectory(prefix='halimg') as t:
        da, db, ho = os.path.join(t, 'a'), os.path.join(t, 'b'), os.path.join(t, 'heat')
        for d in (da, db):
            os.makedirs(os.path.join(d, 'sub'))
        Image.fromarray(base).save(os.path.join(da, 'frame_000.bmp'))
        Image.fromarray(base).save(os.path.join(db, 'frame_000.bmp'))
        Image.fromarray(base[:64, :64]).save(os.path.join(da, 'sub', 'rt.bmp'))
        Image.fromarray(noisy(0.5, 1)[:64, :64]).save(os.path.join(db, 'sub', 'rt.bmp'))
        hd = (np.linspace(0, 4, 32 * 8 * 4).reshape(8, 32, 4)).astype('<f2')
        for d, v in ((da, hd), (db, (hd.astype(np.float32) * (1 + 4e-4)).astype('<f2'))):
            with open(os.path.join(d, 'hdr.hdrp'), 'wb') as f:
                f.write(b'HDRP 32 8\n'); f.write(v.tobytes())
        quiet = lambda *_: None
        check('folders: bmp round trip, exact fails on sub/rt.bmp',
              run(da, db, 'exact', 1, 1e-3, None, True, quiet) == 1, True)
        check('folders: vk passes (hdrp within 1e-3)', run(da, db, 'vk', 1, 1e-3, ho, True, quiet) == 0, True)
        check('heat map written for sub/rt.bmp', os.path.isfile(os.path.join(ho, 'sub', 'rt_heat.png')), True)
        check('hdrp: a relative 4e-4 fails at 1e-4',
              run(da, db, 'hdr', 1, 1e-4, None, True, quiet) == 1, True)
        os.remove(os.path.join(db, 'frame_000.bmp'))
        check('folders: a file on one side only fails', run(da, db, 'vk', 1, 1e-3, None, True, quiet) == 1, True)
        Image.fromarray(base[:100]).save(os.path.join(db, 'frame_000.bmp'))
        check('folders: a size change fails', run(da, db, 'vk', 1, 1e-3, None, True, quiet) == 1, True)
        with open(os.path.join(db, 'frame_000.bmp'), 'wb') as f:
            f.write(b'not an image')
        check('an unreadable image gives status 2', run(da, db, 'vk', 1, 1e-3, None, True, quiet) == 2, True)
    print('halimg self-test: %s' % ('passed' if not bad else '%d WRONG' % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description='Compare images for the renderer gates (see the docstring).')
    ap.add_argument('a', nargs='?')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--mode', choices=('exact', 'vk', 'hdr'), default='exact')
    ap.add_argument('--tol', type=int, default=1, help='vk: steps of 1/255 a pixel may be off (default 1)')
    ap.add_argument('--hdr-tol', type=float, default=1e-3, help='HDRP: relative tolerance (default 1e-3)')
    ap.add_argument('--out', help='folder for heat maps')
    ap.add_argument('--quiet', action='store_true', help='print failures and the summary only')
    ap.add_argument('--selftest', action='store_true')
    o = ap.parse_args()
    if o.selftest:
        return selftest()
    if not o.a or not o.b:
        ap.print_usage()
        return 2
    return run(o.a, o.b, o.mode, o.tol, o.hdr_tol, o.out, o.quiet)


if __name__ == '__main__':
    sys.exit(main())
