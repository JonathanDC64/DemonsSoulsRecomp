#!/usr/bin/env python3
"""Recover a PS3 export's function name from its NID by brute-forcing candidate names.

PS3 export NID = first 4 LE bytes of SHA1(name + 16-byte suffix) -- the same scheme RPCS3 uses
(ppu_generate_id). RPCS3 only "knows" a function if its name is in its source, so an NID absent from
RPCS3's table is "undocumented" there; but if you can guess the SDK name, this confirms it.

Usage:  python scripts/nid_name.py 0x8F122EF8 [0x... ...]   # match against a built-in cellSpurs name list
        python scripts/nid_name.py --hash cellSpursCreateTask  # compute the NID for a name
"""
import hashlib, sys
SUFFIX = bytes([0x67,0x59,0x65,0x99,0x04,0x25,0x04,0x90,0x56,0x64,0x27,0x49,0x94,0x89,0x74,0x1A])
def nid(name): return int.from_bytes(hashlib.sha1(name.encode()+SUFFIX).digest()[:4],'little')
if len(sys.argv)>2 and sys.argv[1]=='--hash':
    for n in sys.argv[2:]: print(f"{n} = 0x{nid(n):08X}")
    sys.exit(0)
# Built-in candidate corpus: harvest cellSpurs* names from the RPCS3 reference if present.
import glob, re
names=set()
for f in glob.glob(r"C:/dev/rpcs3-reference/rpcs3/Emu/Cell/Modules/cellSpurs*.*"):
    for m in re.findall(r'\b(_?cellSpurs[A-Za-z0-9_]+)\b', open(f,encoding='utf-8',errors='ignore').read()):
        names.add(m)
table={nid(n):n for n in names}
for a in sys.argv[1:]:
    h=int(a,16); print(f"0x{h:08X} = {table.get(h,'(no match; extend the name corpus)')}")
