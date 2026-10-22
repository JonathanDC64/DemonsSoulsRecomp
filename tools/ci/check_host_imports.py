#!/usr/bin/env python3
"""check_host_imports.py: fails when des_host.exe needs the Visual C++ redistributable (docs/85).

The host exe links the Visual C++ runtime and the C++ library into itself (/MT) and uses the UCRT that
Windows 10 and 11 include (ucrt.lib, which imports the api-ms-win-crt-*.dll API sets): the "hybrid" C
runtime. So it must import no MSVCP140*.dll and no VCRUNTIME140*.dll, which only the Visual C++
redistributable installs. This reads the import table and the delay-load table of the PE file itself (no
dumpbin or llvm-readobj needed), prints every DLL, and fails on those.

    python tools/ci/check_host_imports.py build/host/des_host.exe
    python tools/ci/check_host_imports.py --require-ucrt build/host/des_host.exe
    python tools/ci/check_host_imports.py --functions build/host/des_host.exe
    python tools/ci/check_host_imports.py --self-test

    --require-ucrt  also fail when no UCRT DLL is imported (api-ms-win-crt-*.dll or ucrtbase.dll). A plain
                    /MT build carries its own copy of the UCRT: it runs, but its heap, environment and
                    stdio are not the ones the game DLL uses (docs/85 2.4).
    --functions     list the functions imported from each DLL
    --self-test     check the reader and the rules on made-up PE files first

Exit status: 0 nothing found, 1 a DLL that players lack is imported (or, with --require-ucrt, no UCRT
DLL), 2 usage error, or a file that is not a PE image. Standard library only.
"""
import argparse
import io
import os
import re
import struct
import sys
import tempfile

# DLLs that only the Visual C++ redistributable (or Visual Studio) installs. Matched against the lower-case
# name without ".dll". msvcp_win.dll is Windows' own and does not match (no digit after "msvcp").
FORBIDDEN = [
    (re.compile(r"msvcp\d+.*"), "the C++ library as a DLL: only the Visual C++ redistributable installs it"),
    (re.compile(r"vcruntime\d+.*"), "the Visual C++ runtime as a DLL: only the Visual C++ redistributable installs it"),
    (re.compile(r"concrt\d+.*"), "the Concurrency Runtime: part of the Visual C++ redistributable"),
    (re.compile(r"vccorlib\d+.*"), "the C++/CX runtime: part of the Visual C++ redistributable"),
    (re.compile(r"vcomp\d+.*"), "the OpenMP runtime: part of the Visual C++ redistributable"),
    (re.compile(r"msvcr\d+.*"), "an older Visual C++ runtime"),
    (re.compile(r"ucrtbased"), "the debug UCRT: only Visual Studio installs it"),
]
# The UCRT of Windows 10 and 11: the API sets ucrt.lib imports, or ucrtbase.dll itself.
UCRT = [re.compile(r"api-ms-win-crt-.*"), re.compile(r"ucrtbase")]
# Allowed, but worth a line.
NOTES = [
    (re.compile(r"msvcrt"), "Windows' own old C runtime: on every PC, but a clang-cl build should not need it"),
]

MACHINES = {0x8664: "x64", 0x14C: "x86", 0xAA64: "arm64"}
MAX_DLLS = 4096       # descriptors read before giving up on a table with no end
MAX_FUNCS = 65536     # functions per DLL, likewise
CLASS_ORDER = {"FAIL": 0, "note": 1, "ucrt": 2, "": 3}


class NotPE(Exception):
    """The file is not a PE image this reader understands."""


class PEImage:
    """The parts of a PE file the import check needs: the headers, the sections and two data directories."""

    def __init__(self, data):
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise NotPE("no MZ header")
        pe = self._u32(0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise NotPE("no PE signature")
        coff = pe + 4
        self.machine, nsect = self._unpack("<HH", coff)
        opt_size = self._u16(coff + 16)
        opt = coff + 20
        magic = self._u16(opt)
        if magic == 0x20B:       # PE32+ (64-bit)
            self.plus = True
            self.base = self._unpack("<Q", opt + 24)[0]
            nrva_at, dirs_at = 108, 112
        elif magic == 0x10B:     # PE32 (32-bit)
            self.plus = False
            self.base = self._u32(opt + 28)
            nrva_at, dirs_at = 92, 96
        else:
            raise NotPE("unknown optional header (magic 0x%X)" % magic)
        if opt_size < dirs_at:
            raise NotPE("optional header too short")
        self.headers_size = self._u32(opt + 60)
        nrva = min(self._u32(opt + nrva_at), (opt_size - dirs_at) // 8, 16)
        self.dirs = [self._unpack("<II", opt + dirs_at + 8 * i) for i in range(nrva)]
        self.sections = []
        table = opt + opt_size
        for i in range(nsect):
            vsize, va, rsize, rptr = self._unpack("<IIII", table + 40 * i + 8)
            self.sections.append((va, max(vsize, rsize), rptr, rsize))

    def _unpack(self, fmt, off):
        try:
            return struct.unpack_from(fmt, self.data, off)
        except struct.error:
            raise NotPE("truncated at offset 0x%X" % off) from None

    def _u16(self, off):
        return self._unpack("<H", off)[0]

    def _u32(self, off):
        return self._unpack("<I", off)[0]

    def directory(self, index):
        return self.dirs[index] if index < len(self.dirs) else (0, 0)

    def offset(self, rva):
        """The file offset of an RVA."""
        for va, span, rptr, rsize in self.sections:
            if va <= rva < va + span:
                if rva - va >= rsize:
                    raise NotPE("RVA 0x%X has no data in the file" % rva)
                return rptr + rva - va
        if 0 <= rva < self.headers_size:
            return rva
        raise NotPE("RVA 0x%X is in no section" % rva)

    def cstr(self, rva, limit=512):
        off = self.offset(rva)
        end = self.data.find(b"\0", off, off + limit)
        if end < 0:
            raise NotPE("unterminated name at RVA 0x%X" % rva)
        return self.data[off:end].decode("ascii", "replace")

    def thunks(self, rva, adjust=0):
        """The function names of one import name table: "#n" for an import by ordinal."""
        if not rva:
            return []
        fmt, size, flag = ("<Q", 8, 1 << 63) if self.plus else ("<I", 4, 1 << 31)
        off = self.offset(rva)
        names = []
        for _ in range(MAX_FUNCS):
            (v,) = self._unpack(fmt, off)
            if v == 0:
                break
            if v & flag:
                names.append("#%d" % (v & 0xFFFF))
            else:
                names.append(self.cstr(((v - adjust) & 0x7FFFFFFF) + 2))   # skip the 2-byte hint
            off += size
        return names

    def _names(self, rva, adjust=0):
        try:
            return self.thunks(rva, adjust)
        except NotPE:
            return ["(function names unreadable)"]

    def imports(self):
        """[(dll, delay_loaded, [function names])], in table order."""
        out = []
        rva, _size = self.directory(1)       # IMAGE_DIRECTORY_ENTRY_IMPORT
        if rva:
            off = self.offset(rva)
            for i in range(MAX_DLLS):
                oft, _stamp, _chain, name, ft = self._unpack("<5I", off + 20 * i)
                if not (oft or name or ft):
                    break
                out.append((self.cstr(name), False, self._names(oft or ft)))
        rva, _size = self.directory(13)      # IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
        if rva:
            off = self.offset(rva)
            for i in range(MAX_DLLS):
                attrs, name, _hmod, _iat, names, _biat, _uiat, _stamp = self._unpack("<8I", off + 32 * i)
                if not name:
                    break
                adjust = 0
                if not attrs & 1:            # the old format: virtual addresses, not RVAs (PE32 only)
                    if self.plus:
                        raise NotPE("old delay-load table format in a PE32+ image")
                    adjust = self.base
                out.append((self.cstr(name - adjust), True,
                            self._names(names - adjust if names else 0, adjust)))
        return out


def classify(dll):
    """("FAIL" | "ucrt" | "note" | "", why)."""
    key = dll.lower()
    if key.endswith(".dll"):
        key = key[:-4]
    for rx, why in FORBIDDEN:
        if rx.fullmatch(key):
            return "FAIL", why
    for rx in UCRT:
        if rx.fullmatch(key):
            return "ucrt", "the UCRT, part of Windows 10 and 11"
    for rx, why in NOTES:
        if rx.fullmatch(key):
            return "note", why
    return "", ""


def check_file(path, require_ucrt=False, functions=False, out=None):
    if out is None:
        out = sys.stdout
    try:
        with open(path, "rb") as f:
            image = PEImage(f.read())
        dlls = image.imports()
    except OSError as e:
        print(f"{path}: cannot read it: {e}", file=out)
        return 2
    except NotPE as e:
        print(f"{path}: not a PE image this check can read: {e}", file=out)
        return 2
    kind = "PE32+" if image.plus else "PE32"
    arch = MACHINES.get(image.machine, "machine 0x%04X" % image.machine)
    delayed = sum(1 for _, d, _ in dlls if d)
    print(f"{path}: {kind}, {arch}, {len(dlls)} DLLs imported ({delayed} delay-loaded)", file=out)
    rows = sorted(((classify(dll), dll, d, fns) for dll, d, fns in dlls),
                  key=lambda r: (CLASS_ORDER[r[0][0]], r[1].lower()))
    bad = ucrt = 0
    for (cls, why), dll, d, fns in rows:
        bad += cls == "FAIL"
        ucrt += cls == "ucrt"
        line = f"  {cls or '':4}  {dll}" + (" (delay-loaded)" if d else "")
        if why:
            line += f": {why}"
        print(line, file=out)
        if functions:
            for fn in fns:
                print(f"          {fn}", file=out)
    failed = False
    if bad:
        print(f"{path}: FAIL: {bad} DLL(s) that players do not have without the Visual C++ redistributable",
              file=out)
        failed = True
    if require_ucrt and not ucrt:
        print(f"{path}: FAIL: no UCRT DLL is imported (api-ms-win-crt-*.dll or ucrtbase.dll): this is a "
              f"plain /MT build with its own UCRT, not the hybrid one (docs/85)", file=out)
        failed = True
    if not failed:
        print(f"{path}: ok ({ucrt} UCRT DLLs, no Visual C++ runtime DLL)", file=out)
    return 1 if failed else 0


# ---- Self-test ---------------------------------------------------------------------------------------

def make_pe(imports=(), delayed=(), plus=True):
    """A minimal PE image: the headers, then one section holding the import and delay-load tables.
    imports and delayed: [(dll, [function name, or "#n" for an import by ordinal])]."""
    sect_rva, file_align = 0x1000, 0x200
    ptr_fmt, ptr_size, ord_flag = ("<Q", 8, 1 << 63) if plus else ("<I", 4, 1 << 31)
    body = bytearray()

    def put(blob, align=2):
        while len(body) % align:
            body.append(0)
        rva = sect_rva + len(body)
        body.extend(blob)
        return rva

    def name_table(funcs):
        vals = []
        for fn in funcs:
            if fn.startswith("#"):
                vals.append(ord_flag | int(fn[1:]))
            else:
                vals.append(put(struct.pack("<H", 0) + fn.encode("ascii") + b"\0"))
        return b"".join(struct.pack(ptr_fmt, v) for v in vals) + bytes(ptr_size)

    descs = b""
    for dll, funcs in imports:
        name = put(dll.encode("ascii") + b"\0")
        table = name_table(funcs)
        ilt = put(table, 8)
        iat = put(table, 8)
        descs += struct.pack("<5I", ilt, 0, 0, name, iat)
    imp = (put(descs + bytes(20), 4), len(descs) + 20) if imports else (0, 0)
    ddescs = b""
    for dll, funcs in delayed:
        name = put(dll.encode("ascii") + b"\0")
        names = put(name_table(funcs), 8)
        iat = put(bytes(ptr_size * (len(funcs) + 1)), 8)
        hmod = put(bytes(8), 8)
        ddescs += struct.pack("<8I", 1, name, hmod, iat, names, 0, 0, 0)
    dly = (put(ddescs + bytes(32), 4), len(ddescs) + 32) if delayed else (0, 0)
    if not body:
        body.append(0)
    raw = bytes(body) + bytes(-len(body) % file_align)

    opt_size = (112 if plus else 96) + 16 * 8
    hdr = bytearray(file_align)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x40)
    hdr[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, 0x44, 0x8664 if plus else 0x14C, 1, 0, 0, 0, opt_size, 0x22)
    opt = 0x58
    struct.pack_into("<H", hdr, opt, 0x20B if plus else 0x10B)
    if plus:
        struct.pack_into("<Q", hdr, opt + 24, 0x140000000)
    else:
        struct.pack_into("<I", hdr, opt + 28, 0x400000)
    struct.pack_into("<II", hdr, opt + 32, 0x1000, file_align)                     # section, file alignment
    image_size = sect_rva + -(-len(body) // 0x1000) * 0x1000
    struct.pack_into("<II", hdr, opt + 56, image_size, file_align)                 # SizeOfImage, SizeOfHeaders
    struct.pack_into("<I", hdr, opt + (108 if plus else 92), 16)                   # NumberOfRvaAndSizes
    dirs = opt + (112 if plus else 96)
    struct.pack_into("<II", hdr, dirs + 8 * 1, *imp)
    struct.pack_into("<II", hdr, dirs + 8 * 13, *dly)
    sh = opt + opt_size
    hdr[sh:sh + 8] = b".idata\0\0"
    struct.pack_into("<IIII", hdr, sh + 8, len(body), sect_rva, len(raw), file_align)
    struct.pack_into("<I", hdr, sh + 36, 0xC0000040)                               # initialized data, read, write
    return bytes(hdr) + raw


def self_test():
    k32 = ("KERNEL32.dll", ["GetTickCount64", "LoadLibraryExA"])
    crt = [("api-ms-win-crt-runtime-l1-1-0.dll", ["_initterm", "_cexit"]),
           ("api-ms-win-crt-heap-l1-1-0.dll", ["malloc", "free"])]
    md = [("MSVCP140.dll", ["_Thrd_yield"]), ("VCRUNTIME140.dll", ["memcpy"]),
          ("VCRUNTIME140_1.dll", ["__CxxFrameHandler4"])]
    # (case, imports, delay-loaded, PE32+, --require-ucrt, exit status, words the output must hold)
    cases = [
        ("hybrid CRT exe", [k32] + crt, [("d3d12.dll", ["D3D12CreateDevice"])], True, True, 0,
         ["GetTickCount64", "D3D12CreateDevice", "_initterm", "ok (2 UCRT DLLs"]),
        ("today's /MD exe", [k32] + crt + md, [], True, False, 1,
         ["FAIL  MSVCP140.dll", "FAIL  VCRUNTIME140.dll", "FAIL  VCRUNTIME140_1.dll", "3 DLL(s)"]),
        ("a runtime DLL only delay-loaded", [k32] + crt, [("VCRUNTIME140.dll", ["memset"])], True, False, 1,
         ["VCRUNTIME140.dll (delay-loaded)", "memset"]),
        ("plain /MT exe", [k32], [], True, False, 0, ["ok (0 UCRT DLLs"]),
        ("plain /MT exe, UCRT required", [k32], [], True, True, 1, ["no UCRT DLL"]),
        ("32-bit, debug C++ library, odd case, by ordinal", [k32, ("msvcp140d.DLL", ["#5"])], [], False, False, 1,
         ["PE32,", "x86", "FAIL  msvcp140d.DLL", "#5"]),
        ("Windows' own C and C++ libraries", [k32, ("msvcp_win.dll", ["f"]), ("msvcrt.dll", ["printf"]),
                                              ("ucrtbase.dll", ["abort"])], [], True, True, 0,
         ["note  msvcrt.dll", "ucrt  ucrtbase.dll", "msvcp_win.dll"]),
        ("imports by ordinal", [k32, ("WS2_32.dll", ["#23", "#115"])], [], True, False, 0, ["#23", "#115"]),
        ("no imports at all", [], [], True, False, 0, ["0 DLLs imported"]),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, imps, dly, plus, req, want_rc, words) in enumerate(cases):
            path = os.path.join(tmp, f"case{i}.exe")
            with open(path, "wb") as f:
                f.write(make_pe(imps, dly, plus))
            buf = io.StringIO()
            rc = check_file(path, req, True, buf)
            text = buf.getvalue()
            missing = [w for w in words if w not in text]
            if rc != want_rc or missing:
                failures += 1
                print(f"self-test FAILED: {name}: exit {rc} (want {want_rc}); missing {missing}\n{text}")
        # Files that are not PE images: exit status 2, never a pass.
        good = make_pe([k32] + crt)
        for name, blob in (("not a PE file", b"not a PE file\n"), ("a cut-off PE file", good[:0x100]),
                           ("a PE signature missing", good[:0x40] + b"XX\0\0" + good[0x44:])):
            path = os.path.join(tmp, "bad.exe")
            with open(path, "wb") as f:
                f.write(blob)
            buf = io.StringIO()
            rc = check_file(path, False, False, buf)
            if rc != 2:
                failures += 1
                print(f"self-test FAILED: {name}: exit {rc} (want 2)\n{buf.getvalue()}")
        total = len(cases) + 3
    if failures:
        print(f"self-test: {failures} of {total} cases failed")
        return 1
    print(f"self-test: ok ({total} cases)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Fail when an exe imports a Visual C++ runtime DLL (docs/85).")
    ap.add_argument("files", nargs="*", help="the PE files to check (des_host.exe)")
    ap.add_argument("--require-ucrt", action="store_true",
                    help="also fail when no UCRT DLL is imported (a plain /MT build)")
    ap.add_argument("--functions", action="store_true", help="list the functions imported from each DLL")
    ap.add_argument("--self-test", action="store_true", help="check the reader on made-up PE files first")
    args = ap.parse_args(argv)
    rc = 0
    if args.self_test:
        rc = self_test()
        if rc or not args.files:
            return rc
    if not args.files:
        ap.error("name a file to check, or pass --self-test")
    for path in args.files:
        rc = max(rc, check_file(path, args.require_ucrt, args.functions))
    return rc


if __name__ == "__main__":
    sys.exit(main())
