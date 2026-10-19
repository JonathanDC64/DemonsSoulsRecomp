#!/usr/bin/env python3
"""regress_route.py: walk regress.sh's route on the game's own clock.

regress.sh runs this once the level's script archive has opened; it is not
meant to be run by hand. des_boot runs with DES_FRAMELOG=<frames> (one line
per flip request: the time, the frame step the game was handed, the cutscene
flag, the left stick the game read, the player's position) and with the
debug console on <console>.

Game time is the sum of those frame steps: 1/30 s a frame in the game's own
30 fps mode, the measured frame time (at most 50 ms) under DES_FPS. It falls
behind the wall clock whenever the game slows down, so the 8 s walk below is
240 frames at 30 fps and about 1920 at 240 fps, however long either takes.
Every time here is in game seconds (30 frames each at the original rate):

  anchor    the opening cutscene ends (no RemoMan track for 2 s); with no
            cutscene, the player character appears
  s1_spawn  anchor + --settle
  walk      left stick forward for --walk, starting GAP after s1
  s2_walk   STOP after the walk ends
  turn      right stick right for --turn, GAP after s2;  s3_turn STOP after
  walk on   left stick forward for --on, GAP after s3;   s4_on STOP after
  --ab K=V  GAP after s4 set the knob, s5_ab STOP later; clear it, s6_back
            STOP later

Each of these is planned from the anchor (s4_on at anchor + --settle + 3 GAP
+ the three holds + 3 STOP), not from when the step before it happened.

A hold is the console's `setenv PAD_STICK=lx,ly,rx,ry,0,<secs>`: the runtime
lets go of the stick itself <secs> of wall time after it picked the value up,
so the console's latency does not stretch it. <secs> is the game time over the
game/real rate of the last second, and is issued again with a new length when
the rate changes during the hold. The runtime times the window with
GetTickCount64, which moves in steps of about 16 ms, so at 30 fps a hold can
come out one frame long (240 or 241 frames for the 8 s walk). A shot is
`setenv CELLMARK_DUMP_NOW=1`: the runtime saves the next frame with draws.
The game flushes the frame log twice a second, so the current game time is
predicted from the newest line and the rate, on a clock mapped to the frame
log's from the times its lines arrive.

The defaults come from a run of the clock-driven script (the one that took
the accepted set) at 30 fps with a frame log: it took s1_spawn 35.9 s after
the opening cutscene ended, started each hold 0.8 s after the shot before it
(GAP), and took each shot 2.6-2.9 s after the hold ended (its 2 s sleep,
process start-ups, and up to half a second to its next periodic dump). STOP
is a little sooner, 2.3 s, so s4_on comes 59.7 s after the cutscene, not
61.1. In that run an enemy that follows the player into the corner stood in
front of the camera at s4_on; runs of this script with STOP at 2.8 s, that
run's mean, compared fine too, so the shorter STOP is a margin, not the fix.
The fix is that the stops no longer move.

Writes the shots into --set and a timeline into --log. Exit status: 0 all
shots taken, 1 a shot is missing, 2 the level never came up, 3 the game
stopped flipping.
"""
import argparse, bisect, os, shutil, sys, time

GAP = 0.80     # game s from a shot to the next hold
STOP = 2.30    # game s from the end of a hold to its shot
TOL = 0.10     # re-issue a hold when it is heading for more than this off
NO_CUT = 10.0  # game s with the player present and no cutscene: anchor there
STALL = 30.0   # wall s without a new frame-log line: the game stopped


def now():
    return time.perf_counter()


class Stalled(Exception):
    pass


class FrameLog:
    """Follows a DES_FRAMELOG file as the game appends to it."""

    def __init__(self, path):
        self.path, self.f, self.buf = path, None, b''
        self.t, self.g, self.cut, self.ls, self.pos = [], [], [], [], []
        self.gsum = 0.0
        self.off = None   # now() minus frame-log time: the least seen, an upper bound
        self.grew = now()

    def poll(self):
        if self.f is None:
            try:
                self.f = open(self.path, 'rb')
            except OSError:
                if now() - self.grew > STALL:
                    raise Stalled('no frame log at %s after %d s (an exe without DES_FRAMELOG?)' % (self.path, STALL))
                return
        b = self.f.read()
        if not b:
            if now() - self.grew > STALL:
                raise Stalled('the frame log stopped growing for %d s: the game stalled or quit' % STALL)
            return
        seen = now()
        self.grew = seen
        self.buf += b
        i = self.buf.rfind(b'\n')
        if i < 0:
            return
        lines, self.buf = self.buf[:i].split(b'\n'), self.buf[i + 1:]
        stamp = None
        for ln in lines:
            p = ln.split()
            if len(p) > 1 and p[0] == b'SUM':   # written and flushed at its time stamp
                try:
                    stamp = float(p[1]) / 1000.0
                except ValueError:
                    pass
                continue
            # t_ms interval_ms step_ms cutscene pad buttons lstick reads empty [x y z stamina want factor]
            if len(p) < 9 or p[4] != b'pad':
                continue
            try:
                t, step = float(p[0]) / 1000.0, float(p[2]) / 1000.0
                cut, ls = int(p[3]), int(p[6], 16)
            except ValueError:
                continue
            self.gsum += step
            self.t.append(t); self.g.append(self.gsum)
            self.cut.append(cut); self.ls.append(ls); self.pos.append(len(p) >= 14)
            stamp = t
        if stamp is not None and (self.off is None or seen - stamp < self.off):
            self.off = seen - stamp

    def rate(self, span=1.0):
        """Game seconds per second the game runs at now: steps over intervals in the last `span` s of the log,
        leaving out frames over 2.5 times the median interval (and over 20 ms). A hitch costs the game time it
        costs (the log has it); the frames after it are not expected to repeat it."""
        n = len(self.t)
        if n < 3:
            return 1.0
        k = min(max(1, bisect.bisect_left(self.t, self.t[-1] - span)), n - 2)
        iv = [self.t[j] - self.t[j - 1] for j in range(k, n)]
        lim = max(2.5 * sorted(iv)[len(iv) // 2], 0.02)   # (frames that come in bursts make the median tiny)
        st = sw = 0.0
        for j, v in zip(range(k, n), iv):
            if v <= lim:
                st += self.g[j] - self.g[j - 1]
                sw += v
        return 1.0 if sw <= 0 else min(2.0, max(0.05, st / sw))

    def game_at(self, T):
        """Game time at frame-log time T; past the newest line, at the recent rate."""
        if not self.t:
            return 0.0
        if T >= self.t[-1]:
            return self.g[-1] + self.rate() * (T - self.t[-1])
        k = bisect.bisect_right(self.t, T) - 1
        if k < 0:
            return self.g[0]
        t0, t1, g0, g1 = self.t[k], self.t[k + 1], self.g[k], self.g[k + 1]
        return g0 + (g1 - g0) * (T - t0) / (t1 - t0) if t1 > t0 else g0

    def fl_time(self, p):
        """Frame-log time of the moment now() read p."""
        return p - self.off

    def game_now(self):
        return self.game_at(self.fl_time(now()))


class Console:
    """The runtime's file console (PS3_DEBUG): one command at a time."""

    def __init__(self, path):
        self.path, self.out = path, path + '.out'
        self.lat = 0.05   # mean wait for the console to pick a command up (it looks every 100 ms)
        self.n = 0

    def cmd(self, c, cap=15.0):
        """Run c; return (the moment the reply was seen, the reply)."""
        try:
            os.remove(self.out)
        except OSError:
            pass
        try:   # the console appends: if the old reply could not go, read past it
            skip = os.path.getsize(self.out)
        except OSError:
            skip = 0
        sent = now()
        with open(self.path, 'w') as f:
            f.write(c + '\n')
        while now() - sent < cap:
            try:
                with open(self.out, 'rb') as f:
                    f.seek(skip)
                    s = f.read()
                if b'[dbg] done' in s:
                    done = now()
                    self.n += 1
                    self.lat += (done - sent - self.lat) / min(self.n, 8)
                    return done, s.decode('utf-8', 'replace')
            except OSError:
                pass
            time.sleep(0.003)
        raise TimeoutError('console: no reply to ' + c)

    def setenv(self, kv):
        return self.cmd('setenv ' + kv)[0]


def dump_index(name):
    d = ''.join(ch for ch in name if ch.isdigit())
    return int(d) if d else -1


def bmp_complete(p):
    try:
        with open(p, 'rb') as f:
            h = f.read(6)
        return len(h) == 6 and h[:2] == b'BM' and os.path.getsize(p) >= int.from_bytes(h[2:6], 'little')
    except OSError:
        return False


class Route:
    def __init__(self, a):
        self.a = a
        self.fl = FrameLog(a.frames)
        self.con = Console(a.console)
        self.t0 = now()
        self.log = open(a.log, 'w')
        self.anchor_g = 0.0
        self.missing = 0
        self.hold_log = []

    def say(self, s):
        line = '[regress] t=%ds %s' % (now() - self.t0 + self.a.t_offset, s)
        print(line, flush=True)
        self.log.write(line + '\n'); self.log.flush()

    def note(self, s):   # timeline only
        self.log.write('  ' + s + '\n'); self.log.flush()

    def g_str(self, g):
        d = g - self.anchor_g
        return 'game %.2f s (anchor %+.2f s, %d frames at 30 fps)' % (g, d, round(d * 30))

    def wait_game(self, target):
        """Return once the game reaches `target` (predicted; the log lags up to 0.5 s)."""
        while True:
            self.fl.poll()
            if self.fl.off is not None and self.fl.t:
                left = target - self.fl.game_now()
                if left <= 0:
                    return
                time.sleep(min(0.02, max(0.001, left / max(self.fl.rate(), 0.05) / 2)))
            else:
                time.sleep(0.02)

    def anchor(self):
        """The opening cutscene's end, or the player's arrival when no cutscene plays."""
        fl, start = self.fl, len(self.fl.t)
        cut_last = pos_first = None
        i = start
        while now() - self.t0 < self.a.max_wait:
            fl.poll()
            while i < len(fl.t):
                if fl.cut[i]:
                    cut_last = i
                elif fl.pos[i] and pos_first is None:
                    pos_first = i
                i += 1
            if cut_last is not None and cut_last + 1 < len(fl.t) and fl.g[-1] - fl.g[cut_last] >= 2.0:
                k = cut_last + 1
                self.note('cutscene from frame-log %.2f s to %.2f s' % (fl.t[cut_first(fl, cut_last, start)], fl.t[k]))
                return k, 'the opening cutscene ended'
            if cut_last is None and pos_first is not None and fl.g[-1] - fl.g[pos_first] >= NO_CUT:
                return pos_first, 'the player appeared (no cutscene)'
            time.sleep(0.05)
        return None, None

    def shot(self, name, at_g):
        a, fl = self.a, self.fl
        self.wait_game(at_g - self.con.lat * fl.rate())
        before = set(os.listdir(a.shots))
        t_req = self.con.setenv('CELLMARK_DUMP_NOW=1')
        g = fl.game_at(fl.fl_time(t_req))
        got = None
        while now() - t_req < 20:
            new = [n for n in os.listdir(a.shots) if n not in before and n.lower().endswith('.bmp')]
            if new:
                n = min(new, key=dump_index)
                p = os.path.join(a.shots, n)
                if bmp_complete(p):
                    shutil.copyfile(p, os.path.join(a.set, name + '.bmp'))
                    got = n
                    break
            time.sleep(0.02)
        self.con.setenv('CELLMARK_DUMP_NOW=')   # the next shot's 1 is a change again
        if got:
            self.say('%s <- %s at %s' % (name, got, self.g_str(g)))
        else:
            self.say('%s: no frame saved within 20 s' % name)
            self.missing += 1
        return g

    def hold(self, what, sticks, secs, start_g):
        """Hold the sticks for `secs` of game time from `start_g`; return the game time it ended."""
        fl, con = self.fl, self.con
        self.wait_game(start_g - con.lat * fl.rate())
        spec = lambda w: 'PAD_STICK=%d,%d,%d,%d,0,%.3f' % (tuple(sticks) + (w,))
        w = secs / fl.rate()
        s = spec(w)
        t_app = con.setenv(s)
        end = t_app + w
        fixes = []
        while now() < end - 0.2:   # a re-issue must reach the console before the window ends
            fl.poll()
            T0, T1 = fl.fl_time(t_app), (fl.t[-1] if fl.t else 0.0)
            if T1 > T0 + 0.25:
                r = fl.rate(min(1.0, T1 - T0))
                done = fl.game_at(T1) - fl.game_at(T0)
                heading = done + r * (fl.fl_time(end) - T1)
                if abs(heading - secs) > TOL and len(fixes) < 4:
                    at = done + r * (fl.fl_time(now() + con.lat) - T1)
                    w2 = max(0.05, (secs - at) / r)
                    s2 = spec(w2)
                    if s2 == s:
                        s2 = spec(w2 + 0.001)
                    t2 = con.setenv(s2)
                    fixes.append('%.2f s in, heading for %.2f s: %.3f s more' % (at, heading, w2))
                    s, end = s2, t2 + w2
                    continue
            time.sleep(0.02)
        while now() < end:
            time.sleep(0.002)
        con.setenv('PAD_STICK=')   # a later hold with the same value then still restarts the window
        g0, g1 = fl.game_at(fl.fl_time(t_app)), fl.game_at(fl.fl_time(end))
        self.hold_log.append((what, sticks, secs, t_app, end, fixes))
        self.note('%s: %s for %.2f s, from %s to %.2f%s' % (what, s, secs, self.g_str(g0), g1,
                                                           ''.join('; re-issued ' + x for x in fixes)))
        return g1

    def report_holds(self):
        """What the game read: left-stick holds counted from the frame log's stick column."""
        fl = self.fl
        for what, sticks, secs, t_app, end, fixes in self.hold_log:
            T0, T1 = fl.fl_time(t_app), fl.fl_time(end)
            g = fl.game_at(T1) - fl.game_at(T0)
            msg = '%s: %.2f s wanted, %.2f s by the clock map' % (what, secs, g)
            want = (sticks[0] << 8) | sticks[1]
            if want != 0x8080:
                lo = bisect.bisect_left(fl.t, T0 - 1.0)
                hi = bisect.bisect_right(fl.t, T1 + 1.0)
                frames = [k for k in range(lo, hi) if fl.ls[k] == want]
                if frames:
                    held = sum(fl.g[k] - (fl.g[k - 1] if k else 0.0) for k in frames)
                    msg += ', left stick read for %.2f s (%d frames)' % (held, len(frames))
            if fixes:
                msg += ', re-issued %d time(s)' % len(fixes)
            self.say(msg)

    def run(self):
        a = self.a
        k, why = self.anchor()
        if k is None:
            self.say('no level after %d s' % a.max_wait)
            return 2
        fl = self.fl
        self.anchor_g = fl.g[k]
        self.say('anchor: %s, game %.2f s (game/real %.3f)' % (why, fl.g[k], fl.rate()))
        # Every stop is planned from the anchor, so a console reply that comes late (it looks every 100 ms)
        # moves its own step, not the ones after it.
        t = self.anchor_g + a.settle
        self.shot('s1_spawn', t)
        for name, what, sticks, secs in (('s2_walk', 'walk', (128, 0, 128, 128), a.walk),
                                         ('s3_turn', 'turn', (128, 128, 255, 128), a.turn),
                                         ('s4_on', 'walk on', (128, 0, 128, 128), a.on)):
            self.hold(what, sticks, secs, t + GAP)
            t += GAP + secs + STOP
            self.shot(name, t)
        if a.ab:
            self.wait_game(t + GAP - self.con.lat * fl.rate())
            self.con.setenv(a.ab)
            t += GAP + STOP
            self.shot('s5_ab', t)
            self.con.setenv(a.ab.split('=', 1)[0] + '=')
            t += STOP
            self.shot('s6_back', t)
        self.report_holds()
        self.say('game/real over the route %.3f, frame step now %.2f ms' % (
            self.fl.rate(max(1.0, self.fl.t[-1] - self.fl.t[k])), 1000 * (self.fl.g[-1] - self.fl.g[-2])))
        return 1 if self.missing else 0


def cut_first(fl, last, start):
    k = last
    while k > start and fl.cut[k - 1]:
        k -= 1
    return k


def main():
    ap = argparse.ArgumentParser(description='Walk the regress.sh route by game time.')
    ap.add_argument('--frames', required=True, help='the DES_FRAMELOG file')
    ap.add_argument('--console', required=True, help='the PS3_DEBUG command file')
    ap.add_argument('--shots', required=True, help='CELLMARK_DUMP_DIR')
    ap.add_argument('--set', required=True, help='where the named shots go')
    ap.add_argument('--log', required=True, help='timeline file')
    ap.add_argument('--settle', type=float, default=35.9)
    ap.add_argument('--walk', type=float, default=8.0)
    ap.add_argument('--turn', type=float, default=1.5)
    ap.add_argument('--on', type=float, default=5.0)
    ap.add_argument('--ab', default='', help='KNOB=value for s5_ab / s6_back')
    ap.add_argument('--max-wait', type=float, default=400.0, help='wall s to wait for the anchor')
    ap.add_argument('--t-offset', type=float, default=0.0, help='added to the t= shown (regress.sh elapsed)')
    a = ap.parse_args()
    r = Route(a)
    try:
        return r.run()
    except Stalled as e:
        r.say(str(e))
        return 3
    except TimeoutError as e:
        r.say('%s: the game stopped answering' % e)
        return 3


if __name__ == '__main__':
    sys.exit(main())
