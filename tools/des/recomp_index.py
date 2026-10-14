#!/usr/bin/env python3
"""recomp_index.py -- a call-graph index of the lifted PPU code (work/recomp).

The lifted code is ~450 MB of generated C++ (ppu_recomp_NNN.cpp, ~400k
func_XXXXXXXX functions), too big for a general code indexer, and grep over
it answers "where is it" but not "who calls it". This builds a SQLite index of
every function's location and its direct calls once (about a minute), and
rebuilds it on the next query when a lifted file is newer than the index.

  recomp_index.py def <addr>            file:line of func_<addr>
  recomp_index.py src <addr> [lines]    the function's source (default 200 lines)
  recomp_index.py callers <addr> [depth]  who calls it (default depth 1)
  recomp_index.py callees <addr> [depth]  what it calls
  recomp_index.py path <from> <to>      shortest direct-call path, if any
  recomp_index.py at <addr>             the function containing a guest address
  recomp_index.py build                 rebuild now

<addr> is hex with or without 0x / func_ (e.g. 00B5D8D8). Indirect calls
(through ctr / function pointers) are not edges; the guest's indirect-call
sites are what the runtime's shadow stacks and gstacks show instead.
Options: --dir (default work/recomp), --db (default work/recomp_index.sqlite).
"""
import os, re, sqlite3, sys, glob, collections

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEF_RE = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{')
CALL_RE = re.compile(r'\bfunc_([0-9A-F]{8})\(ctx\)')


def opt(name, default):
    if name in sys.argv:
        i = sys.argv.index(name); v = sys.argv[i + 1]; del sys.argv[i:i + 2]; return v
    return default


DIR = opt('--dir', os.path.join(ROOT, 'work', 'recomp'))
DB = opt('--db', os.path.join(ROOT, 'work', 'recomp_index.sqlite'))


def sources():
    return sorted(glob.glob(os.path.join(DIR, 'ppu_recomp_*.cpp')))


def build():
    tmp = DB + '.tmp'
    if os.path.exists(tmp): os.remove(tmp)
    db = sqlite3.connect(tmp)
    db.executescript('''
        create table fn(addr integer primary key, file text, line integer, end integer);
        create table call(src integer, dst integer);
    ''')
    nf = ne = 0
    for path in sources():
        fname = os.path.basename(path)
        cur, start, edges, rows = None, 0, set(), []
        with open(path, encoding='utf-8', errors='replace') as f:
            for n, ln in enumerate(f, 1):
                if ln.startswith('void func_'):
                    m = DEF_RE.match(ln)
                    if m:
                        if cur is not None: rows.append((cur, fname, start, n - 1))
                        cur, start = int(m.group(1), 16), n
                        continue
                if cur is not None and 'func_' in ln:
                    for m in CALL_RE.finditer(ln):
                        edges.add((cur, int(m.group(1), 16)))
            if cur is not None: rows.append((cur, fname, start, n))
        db.executemany('insert or replace into fn values (?,?,?,?)', rows)
        db.executemany('insert into call values (?,?)', edges)
        nf += len(rows); ne += len(edges)
        print('  %s: %d functions' % (fname, len(rows)), file=sys.stderr)
    db.executescript('create index call_src on call(src); create index call_dst on call(dst);')
    db.commit(); db.close()
    if os.path.exists(DB): os.remove(DB)
    os.replace(tmp, DB)
    print('indexed %d functions, %d call edges -> %s' % (nf, ne, DB), file=sys.stderr)


def open_db():
    srcs = sources()
    if not srcs:
        sys.exit('no ppu_recomp_*.cpp in ' + DIR)
    if not os.path.exists(DB) or max(os.path.getmtime(p) for p in srcs) > os.path.getmtime(DB):
        print('(re)building the index...', file=sys.stderr); build()
    return sqlite3.connect(DB)


def addr(s):
    s = s.strip()
    if s.startswith('func_'): s = s[5:]
    return int(s, 16)


def nm(a): return 'func_%08X' % a


def where(db, a):
    r = db.execute('select file, line from fn where addr=?', (a,)).fetchone()
    return '%s:%d' % r if r else '(not lifted)'


def walk(db, a, depth, sql):
    seen = {a}; frontier = [a]
    for d in range(1, depth + 1):
        nxt = []
        for x in frontier:
            for (y,) in db.execute(sql, (x,)):
                if y in seen: continue
                seen.add(y); nxt.append(y)
                print('%s%s  %s  (via %s)' % ('  ' * (d - 1), nm(y), where(db, y), nm(x)))
        frontier = nxt
    if len(seen) == 1: print('(none)')


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__); return
    cmd = sys.argv[1]
    if cmd == 'build':
        build(); return
    db = open_db()
    if cmd == 'def':
        print(nm(addr(sys.argv[2])), where(db, addr(sys.argv[2])))
    elif cmd == 'at':
        a = addr(sys.argv[2])
        r = db.execute('select addr, file, line from fn where addr<=? order by addr desc limit 1', (a,)).fetchone()
        print('%s +0x%X  %s:%d' % (nm(r[0]), a - r[0], r[1], r[2]) if r else '(none)')
    elif cmd == 'src':
        a = addr(sys.argv[2]); lim = int(sys.argv[3]) if len(sys.argv) > 3 else 200
        r = db.execute('select file, line, end from fn where addr=?', (a,)).fetchone()
        if not r: sys.exit('func_%08X not lifted' % a)
        with open(os.path.join(DIR, r[0]), encoding='utf-8', errors='replace') as f:
            for n, ln in enumerate(f, 1):
                if n > r[2] or n >= r[1] + lim: break
                if n >= r[1]: sys.stdout.write(ln)
    elif cmd == 'callers':
        walk(db, addr(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 1,
             'select src from call where dst=?')
    elif cmd == 'callees':
        walk(db, addr(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 1,
             'select dst from call where src=?')
    elif cmd == 'path':
        a, b = addr(sys.argv[2]), addr(sys.argv[3])
        prev = {a: None}; q = collections.deque([a])
        while q and b not in prev:
            x = q.popleft()
            for (y,) in db.execute('select dst from call where src=?', (x,)):
                if y not in prev: prev[y] = x; q.append(y)
        if b not in prev: print('(no direct-call path)'); return
        chain = []; x = b
        while x is not None: chain.append(x); x = prev[x]
        for x in reversed(chain): print(nm(x), where(db, x))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
