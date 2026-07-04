#!/usr/bin/env python3
"""sym.py -- reverse-symbolize host RVAs to guest functions via des_boot.map.

The recompiled binary has no usable PDB, but its linker .map lists all ~400k
`func_<guestaddr>` symbols with their VAs. This maps a host module RVA (as printed
by the runtime's [CRASH]/[WATCHDOG] backtraces) back to the guest function that
contains it -- so a hung/crashed thread's host stack becomes a guest call stack,
with NO instrumentation or perturbation of the fragile boot.

Usage:
  sym.py <rva> [rva ...]              symbolize one or more RVAs (hex, 0x optional)
  sym.py --stack < dump.txt          read stdin, symbolize every 'rva=0x...' line
  sym.py --tid N --watchdog f.txt    symbolize the WATCHDOG stack of host tid N
  --map PATH                         .map file (default port/build/des_boot.map)

Output: RVA -> func_<guestaddr> (+offset) so you can decompile the guest addr.
"""
import sys, re, bisect, os

DEF_MAP = os.path.join(os.path.dirname(__file__), "..", "..", "port", "build", "des_boot.map")
SYM_RE  = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s')
FUNC_RE = re.compile(r'func_[0-9A-Fa-f]{8}')

def load_map(path):
    base = 0x140000000
    syms = []  # (rva, name)
    with open(path, 'r', errors='ignore') as f:
        for line in f:
            if 'Preferred load address' in line:
                m = re.search(r'([0-9a-fA-F]{8,16})', line)
                if m: base = int(m.group(1), 16)
                continue
            m = SYM_RE.match(line)
            if not m: continue
            name, va = m.group(1), int(m.group(2), 16)
            fm = FUNC_RE.search(name)
            nm = fm.group(0) if fm else name.lstrip('?').split('@@')[0]
            syms.append((va - base, nm))
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]

def resolve(rvas, names, rva):
    i = bisect.bisect_right(rvas, rva) - 1
    if i < 0: return None
    return names[i], rva - rvas[i]

def main():
    args = sys.argv[1:]
    mappath = DEF_MAP
    if '--map' in args:
        k = args.index('--map'); mappath = args[k+1]; del args[k:k+2]
    rvas_addr, names = load_map(mappath)
    print(f"[sym] loaded {len(rvas_addr)} symbols from {os.path.basename(mappath)}", file=sys.stderr)

    def show(rva):
        r = resolve(rvas_addr, names, rva)
        if r: print(f"  0x{rva:08X} -> {r[0]} +0x{r[1]:X}")
        else: print(f"  0x{rva:08X} -> ??")

    if '--tid' in args and '--watchdog' in args:
        tid = args[args.index('--tid')+1]
        wf  = args[args.index('--watchdog')+1]
        cur = False
        for line in open(wf, errors='ignore'):
            m = re.search(r'\[WATCHDOG\]\s+tid\s+(\d+)\s+in', line)
            if m: cur = (m.group(1) == tid);
            if m: print(f"--- tid {m.group(1)} ({'MATCH' if cur else 'skip'}) ---") if cur else None
            if cur:
                rm = re.search(r'rva=0x([0-9a-fA-F]+)', line)
                if rm: show(int(rm.group(1), 16))
        return
    if '--stack' in args:
        for line in sys.stdin:
            rm = re.search(r'rva=0x([0-9a-fA-F]+)', line)
            if rm: show(int(rm.group(1), 16))
        return
    for a in args:
        show(int(a, 16))

if __name__ == '__main__':
    main()
