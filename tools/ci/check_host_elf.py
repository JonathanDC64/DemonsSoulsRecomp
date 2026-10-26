#!/usr/bin/env python3
"""check_host_elf.py: fails when the Linux host folder needs a library players may not have (docs/87).

The Linux des_host links the C++ library into itself and has SDL2, vkd3d and FFmpeg beside it, which it finds
through RUNPATH $ORIGIN. So every program and library in the folder may need only glibc's own libraries, GCC's
runtime (libgcc_s.so.1, on every Linux) and the folder's other libraries, and no glibc symbol newer than the
floor (2.35, the ubuntu-22.04 runner's). This reads each ELF file itself (no readelf or ldd needed), prints
what it needs, and fails on:

  - a NEEDED library outside those, or one of the folder's libraries without a RUNPATH ($ORIGIN) leading to it;
  - a GLIBC_ symbol version newer than the floor;
  - the C++ library as a shared library (libstdc++.so.6, libc++.so.1, or GLIBCXX_ and CXXABI_ versions);
  - a RUNPATH or RPATH other than $ORIGIN (a build folder of the runner, say).

    python tools/ci/check_host_elf.py dist/host-linux
    python tools/ci/check_host_elf.py --glibc 2.35 dist/host-linux
    python tools/ci/check_host_elf.py --self-test

Exit status: 0 nothing found, 1 something found, 2 usage error or a file that is not x86-64 ELF. Standard
library only.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

GLIBC_LIBS = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libresolv.so.2",
              "libutil.so.1", "ld-linux-x86-64.so.2"}
EVERYWHERE = GLIBC_LIBS | {"libgcc_s.so.1"}
CXX_LIBS = {"libstdc++.so.6", "libc++.so.1", "libc++abi.so.1"}
ORIGIN = {"$ORIGIN", "${ORIGIN}"}


class NotELF(Exception):
    """The file is not an x86-64 ELF file this reader understands."""


def elf_info(data):
    """NEEDED, SONAME, RUNPATH, RPATH and the symbol versions needed per library, from an x86-64 ELF file."""
    if data[:4] != b"\x7fELF" or len(data) < 64:
        raise NotELF("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise NotELF("not 64-bit little-endian ELF")
    etype, machine = struct.unpack_from("<HH", data, 16)
    if machine != 62:
        raise NotELF(f"machine {machine}, not x86-64")
    shoff, = struct.unpack_from("<Q", data, 40)
    shentsize, shnum = struct.unpack_from("<HH", data, 58)
    if not shoff or shentsize != 64 or shoff + shnum * 64 > len(data):
        raise NotELF("no section headers")
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * 64) for i in range(shnum)]

    def cstr(sec, off):
        if sec >= len(secs):
            raise NotELF("a string table index out of range")
        o = secs[sec][4] + off
        end = data.find(b"\0", o)
        if end < 0:
            raise NotELF("a string without its end")
        return data[o:end].decode("utf-8", "replace")

    info = {"type": etype, "needed": [], "soname": None, "runpath": None, "rpath": None, "versions": {}}
    for s in secs:
        stype, off, size, link, count = s[1], s[4], s[5], s[6], s[7]
        if stype == 6:                                   # SHT_DYNAMIC
            for o in range(off, min(off + size, len(data) - 15), 16):
                tag, val = struct.unpack_from("<qQ", data, o)
                if tag == 0:
                    break
                if tag == 1:
                    info["needed"].append(cstr(link, val))
                elif tag in (14, 15, 29):
                    info[{14: "soname", 15: "rpath", 29: "runpath"}[tag]] = cstr(link, val)
        elif stype == 0x6FFFFFFE:                        # SHT_GNU_verneed
            o = off
            for _ in range(min(count, 4096)):
                _, cnt, file_, aux, nxt = struct.unpack_from("<HHIII", data, o)
                names, a = [], o + aux
                for _ in range(min(cnt, 4096)):
                    _, _, _, vname, anext = struct.unpack_from("<IHHII", data, a)
                    names.append(cstr(link, vname))
                    if not anext:
                        break
                    a += anext
                info["versions"].setdefault(cstr(link, file_), []).extend(names)
                if not nxt:
                    break
                o += nxt
    return info


def version_key(v):
    m = re.fullmatch(r"[A-Z_]+?_?(\d+(?:\.\d+)*)", v)
    return tuple(int(n) for n in m.group(1).split(".")) if m else ()


def check(name, info, folder_libs, floor):
    """(lines to print, problems) for one file. folder_libs: the libraries the folder carries."""
    problems = []
    newest = max((version_key(v) for lib, vs in info["versions"].items() if lib in GLIBC_LIBS
                  for v in vs if v.startswith("GLIBC_")), default=())
    cxx = sorted({v for vs in info["versions"].values() for v in vs if v.startswith(("GLIBCXX_", "CXXABI_"))})
    search = [p for p in (info["runpath"] or info["rpath"] or "").split(":") if p]
    lines = [f"{name}: needs {', '.join(info['needed']) or 'nothing'}; newest GLIBC_{'.'.join(map(str, newest)) or '-'}"
             f"{'; RUNPATH ' + info['runpath'] if info['runpath'] else ''}{'; RPATH ' + info['rpath'] if info['rpath'] else ''}"]
    for n in info["needed"]:
        if n in CXX_LIBS:
            problems.append(f"{name}: needs {n}: the C++ library must be linked in (-static-libstdc++)")
        elif n in folder_libs:
            if not ORIGIN & set(search):
                problems.append(f"{name}: needs the folder's {n}, but has no RUNPATH $ORIGIN to find it")
        elif n not in EVERYWHERE:
            problems.append(f"{name}: needs {n}, which players may not have and the folder does not carry")
    if cxx:
        problems.append(f"{name}: needs C++ library versions ({', '.join(cxx)}): link the C++ library in")
    if newest > floor:
        problems.append(f"{name}: needs GLIBC_{'.'.join(map(str, newest))}, newer than the floor "
                        f"{'.'.join(map(str, floor))} (docs/87)")
    other = [p for p in search if p not in ORIGIN]
    if other:
        problems.append(f"{name}: searches {', '.join(other)} for libraries (RUNPATH or RPATH): only $ORIGIN may be "
                        "there")
    return lines, problems


def check_folder(folder, floor):
    files = sorted(p for p in Path(folder).iterdir() if p.is_file())
    elves = {}
    for p in files:
        head = p.read_bytes()[:4]
        if head == b"\x7fELF":
            elves[p.name] = p
    if not elves:
        raise NotELF(f"{folder}: no ELF file in it")
    libs = {}
    for name, p in elves.items():
        info = elf_info(p.read_bytes())
        libs[name] = info
    folder_libs = {info["soname"] or name for name, info in libs.items() if info["type"] == 3} | set(elves)
    out, problems = [], []
    for name, info in libs.items():
        lines, probs = check(name, info, folder_libs, floor)
        out += lines
        problems += probs
    return out, problems


def self_test():
    """The rules on made-up readings, and the reader on this system's own programs."""
    floor = (2, 35)
    base = {"type": 3, "soname": None, "runpath": None, "rpath": None, "versions": {}}
    cases = [
        ("plain", dict(base, needed=["libc.so.6", "libm.so.6"], versions={"libc.so.6": ["GLIBC_2.34"]}), 0),
        ("folder lib with $ORIGIN", dict(base, needed=["libSDL2-2.0.so.0"], runpath="$ORIGIN"), 0),
        ("folder lib without RUNPATH", dict(base, needed=["libSDL2-2.0.so.0"]), 1),
        ("unknown lib", dict(base, needed=["libfoo.so.1"]), 1),
        ("shared C++ library", dict(base, needed=["libstdc++.so.6"], versions={"libstdc++.so.6": ["GLIBCXX_3.4.30"]}), 2),
        ("glibc too new", dict(base, needed=["libc.so.6"], versions={"libc.so.6": ["GLIBC_2.36", "GLIBC_2.2.5"]}), 1),
        ("runner path", dict(base, needed=[], runpath="$ORIGIN:/home/runner/work/x/lib"), 1),
        ("libgcc_s", dict(base, needed=["libgcc_s.so.1"], versions={"libgcc_s.so.1": ["GCC_3.0"]}), 0),
    ]
    bad = 0
    for label, info, want in cases:
        _, probs = check("t", info, {"libSDL2-2.0.so.0"}, floor)
        if len(probs) != want:
            print(f"self-test: {label}: {len(probs)} problems, expected {want}: {probs}")
            bad += 1
    for prog in ("/bin/sh", "/usr/bin/env"):
        p = Path(prog).resolve()
        if p.is_file():
            try:
                info = elf_info(p.read_bytes())
            except NotELF as e:
                print(f"self-test: {p}: {e}")
                bad += 1
                continue
            if "libc.so.6" not in info["needed"]:
                print(f"self-test: {p}: libc.so.6 not among {info['needed']}")
                bad += 1
    for junk in (b"MZ\0\0", b"\x7fELF" + b"\0" * 60):
        try:
            elf_info(junk)
            print("self-test: a non-ELF file was read")
            bad += 1
        except NotELF:
            pass
    print("self-test: ok" if not bad else f"self-test: {bad} failed")
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", nargs="?", help="the host folder: des_host and the libraries beside it")
    ap.add_argument("--glibc", default="2.35", help="the floor: the newest GLIBC_ version allowed (default 2.35)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.folder:
        ap.error("a folder, or --self-test")
    try:
        lines, problems = check_folder(a.folder, version_key("GLIBC_" + a.glibc))
    except (NotELF, OSError, struct.error) as e:
        print(f"::error::{e}")
        return 2
    print("\n".join(lines))
    for p in problems:
        print(f"::error::{p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
