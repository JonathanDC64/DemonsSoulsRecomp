#!/usr/bin/env python3
"""Fail when a release contains developer-build code (docs/55).

A DES_DEV build compiles in the dev cheats and the settings window's Developer
tab. Both carry DES_DEV_MARKER, and the cheats read the knobs named below.
Release files must contain none of them. No workflow runs this yet: run it by
hand on the folder or archive you are about to share.

    python tools/ci/check_release_no_dev.py dist/DemonsSoulsRecomp.zip
    python tools/ci/check_release_no_dev.py port/build/des_boot.exe
    python tools/ci/check_release_no_dev.py --expect-dev port/build_dev/des_boot.exe   # the self-test

Every file is read as bytes, and each string is looked for in ASCII and in
UTF-16LE. Zip, tar, gzip, bzip2 and xz files are opened, nested up to 8 deep,
zip self-extractors included. A file it cannot see inside fails the check
instead of passing: other archives (7z, RAR, CAB, MSI, zstd, lz4, ISO, DMG,
xar, squashfs, AppImage and the like), installers (NSIS, Inno Setup, WiX
bundles, an executable with a 7z, RAR, CAB or MSI inside), executables packed
with UPX and the like, archives nested deeper than 8, encrypted or damaged
archive members, and files it cannot read. Check such a file another way
(unpack it and run this on the result), then leave it out with --skip; each
skipped file is listed.

    --skip PATTERN   leave out the files whose name (as printed) or last part
                     matches PATTERN (fnmatch, any case); repeatable
    --expect-dev     the self-test: succeed only when dev build code is found

Exit status: 0 nothing found, 1 dev build code found, 3 files not checked
(each hit and each file is listed), 2 usage error. With --expect-dev: 0 when
dev build code is found, else 1. Standard library only.
"""
import argparse
import bz2
import fnmatch
import gzip
import io
import lzma
import os
import re
import struct
import sys
import tarfile
import zipfile

MARKER = b"DES-DEV-BUILD"  # the start of DES_DEV_MARKER (port/des_dev_marker.h)
KNOBS = [b"DES_GOD", b"DES_ATK", b"DES_SPEED", b"DES_WARP"]
NEEDLES = [MARKER] + KNOBS
TEXT_SKIP = (".md", ".txt", ".html")  # docs may name the knobs; binaries may not
MAX_DEPTH = 8  # archives nested deeper than this are not opened, and fail the check

# Formats this script cannot open, by their first bytes ...
MAGIC_UNOPENABLE = [
    (b"7z\xbc\xaf\x27\x1c", "a 7z archive"),
    (b"Rar!\x1a\x07", "a RAR archive"),
    (b"MSCF\x00\x00\x00\x00", "a CAB archive"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an MSI installer or other OLE file"),
    (b"\x28\xb5\x2f\xfd", "a zstd stream"),
    (b"\x04\x22\x4d\x18", "an lz4 stream"),
    (b"LZIP", "an lzip stream"),
    (b"xar!", "a xar archive (a macOS .pkg)"),
    (b"hsqs", "a squashfs image"),
    (b"\xed\xab\xee\xdb", "an RPM package"),
    (b"MSWIM\x00\x00\x00", "a WIM image"),
]
# ... and by name, for the ones without fixed first bytes.
EXT_UNOPENABLE = (".7z", ".rar", ".cab", ".msi", ".msp", ".msm", ".iso", ".dmg", ".pkg", ".xar", ".zst", ".lz4",
                  ".lz", ".lzh", ".lha", ".arj", ".ace", ".wim", ".sit", ".sitx", ".cpio", ".rpm", ".deb", ".z",
                  ".squashfs", ".snap", ".appimage")
# Installers and self-extractors: an executable with one of these inside.
EXE_INSIDE = [
    (b"\xef\xbe\xad\xdeNullsoftInst", "an NSIS installer"),
    (b"Inno Setup Setup Data", "an Inno Setup installer"),
    (b"7z\xbc\xaf\x27\x1c", "an executable with a 7z archive inside (an installer or self-extractor)"),
    (b"Rar!\x1a\x07", "an executable with a RAR archive inside (an installer or self-extractor)"),
    (b"MSCF\x00\x00\x00\x00", "an executable with a CAB archive inside (an installer or self-extractor)"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an executable with an MSI or other OLE file inside (an installer)"),
]
# PE section names of executable packers and installer bundles.
PACKER_SECTIONS = {
    b"UPX0": "UPX", b"UPX1": "UPX", b"UPX2": "UPX", b".aspack": "ASPack", b"MPRESS1": "MPRESS",
    b"MPRESS2": "MPRESS", b".petite": "Petite", b".nsp0": "NsPack", b".nsp1": "NsPack", b".themida": "Themida",
    b".winlice": "WinLicense", b".vmp0": "VMProtect", b".vmp1": "VMProtect", b".enigma1": "Enigma",
    b".enigma2": "Enigma", b".wixburn": "a WiX bundle",
}
KIND = {"zip": "a zip", "zip-sfx": "a zip self-extractor", "tar": "a tar archive", "gzip": "a gzip stream",
        "bzip2": "a bzip2 stream", "xz": "an xz stream", "lzma": "an lzma stream"}


def patterns():
    for n in NEEDLES:
        yield n, n
        yield n + b" (UTF-16LE)", n.decode("ascii").encode("utf-16-le")


def pe_sections(data):
    """The section names of a PE executable, or [] for anything else."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        return []
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    first = pe + 24 + optsize
    names = []
    for i in range(min(nsec, 96)):
        at = first + 40 * i
        if at + 40 > len(data):
            break
        names.append(data[at:at + 8].rstrip(b"\x00"))
    return names


def container_kind(name, data):
    low = name.lower()
    if data[:4] in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08") or low.endswith(".zip"):
        return "zip"
    if data[:2] == b"MZ" and zipfile.is_zipfile(io.BytesIO(data)):
        return "zip-sfx"
    if data[:2] == b"\x1f\x8b" or low.endswith((".gz", ".tgz")):
        return "gzip"
    if (data[:3] == b"BZh" and data[3:4].isdigit()) or low.endswith((".bz2", ".tbz", ".tbz2")):
        return "bzip2"
    if data[:6] == b"\xfd7zXZ\x00" or low.endswith((".xz", ".txz")):
        return "xz"
    if low.endswith(".lzma"):
        return "lzma"
    if data[257:262] == b"ustar" or low.endswith(".tar"):
        return "tar"
    return None


def unopenable(name, data):
    """Why this file cannot be checked inside, or None."""
    for magic, what in MAGIC_UNOPENABLE:
        if data.startswith(magic):
            return what
    if data[0x8001:0x8006] == b"CD001":
        return "an ISO image"
    if len(data) >= 512 and data[-512:-508] == b"koly":
        return "a DMG image"
    if data[:4] == b"\x7fELF" and data[8:11] == b"AI\x02":
        return "an AppImage"
    if data[:2] == b"MZ":
        for s in pe_sections(data):
            if s in PACKER_SECTIONS:
                what = PACKER_SECTIONS[s]
                return what if what.startswith("a ") else "an executable packed with " + what
        for sig, what in EXE_INSIDE:
            if data.find(sig) >= 0:
                return what
    low = name.lower()
    for ext in EXT_UNOPENABLE:
        if low.endswith(ext):
            return "a " + ext + " file"
    return None


def inner_name(name, kind):
    """The name of what a compressed stream holds: x.tar.gz -> x.tar, x.tgz -> x.tar."""
    low = name.lower()
    for ext, repl in ((".tgz", ".tar"), (".tbz2", ".tar"), (".tbz", ".tar"), (".txz", ".tar"), (".gz", ""),
                      (".bz2", ""), (".xz", ""), (".lzma", "")):
        if low.endswith(ext):
            return os.path.basename(name[:len(name) - len(ext)] + repl)
    return os.path.basename(name) + " (" + kind + " data)"


def members(kind, name, data):
    """(member name, bytes or None, why it could not be read) for each file in a container."""
    if kind in ("zip", "zip-sfx"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                if info.flag_bits & 0x1:
                    yield info.filename, None, "encrypted, so it cannot be read"
                    continue
                try:
                    yield info.filename, z.read(info), None
                except Exception as e:  # unsupported method, damaged, bad CRC
                    yield info.filename, None, "cannot read it (%s)" % e
    elif kind == "tar":
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as t:
            for m in t:
                if m.isfile():
                    yield m.name, t.extractfile(m).read(), None
    else:
        if kind == "gzip":
            out = gzip.decompress(data)
        elif kind == "bzip2":
            out = bz2.decompress(data)
        elif kind == "xz":
            out = lzma.decompress(data, format=lzma.FORMAT_XZ)
        else:
            out = lzma.decompress(data, format=lzma.FORMAT_ALONE)
        yield inner_name(name, kind), out, None


class Scan:
    def __init__(self, skip):
        self.skip = [p.lower() for p in skip]
        self.hits, self.problems, self.skipped = [], [], []
        self.unchecked = set()  # names with a problem
        self.files = 0

    def skipped_name(self, name):
        low = name.lower()
        last = re.split(r"[/\\!]", low)[-1]
        return any(fnmatch.fnmatchcase(low, p) or fnmatch.fnmatchcase(last, p) for p in self.skip)

    def problem(self, name, why):
        self.problems.append("%s: %s" % (name, why))
        self.unchecked.add(name)

    def unreadable(self, name, why):
        """A file or archive member whose bytes could not be read."""
        if self.skipped_name(name):
            self.skipped.append(name)
            return
        self.files += 1
        self.problem(name, why)

    def scan(self, name, data, depth=0):
        if self.skipped_name(name):
            self.skipped.append(name)
            return
        self.files += 1
        kind = container_kind(name, data)
        if kind:
            if depth >= MAX_DEPTH:
                self.problem(name, "%s nested more than %d deep, not opened" % (KIND[kind], MAX_DEPTH))
            else:
                try:
                    for member, mdata, why in members(kind, name, data):
                        if mdata is None:
                            self.unreadable(name + "!" + member, why)
                        else:
                            self.scan(name + "!" + member, mdata, depth + 1)
                    if kind != "zip-sfx":
                        return  # its bytes are what was just read; a self-extractor's own code is scanned below
                except Exception as e:
                    self.problem(name, "cannot open it as %s (%s)" % (KIND[kind], e))
            # not opened: its bytes are still scanned below, since a hit is a hit
        why = unopenable(name, data)
        if why:
            self.problem(name, why + ", which this script cannot open")
        if name.lower().endswith(TEXT_SKIP):
            return
        for label, pat in patterns():
            at = data.find(pat)
            if at >= 0:
                self.hits.append("%s: %s at offset %#x" % (name, label.decode("ascii", "replace"), at))


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, usage="%(prog)s [--skip PATTERN]... [--expect-dev] PATH...")
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--skip", action="append", default=[], metavar="PATTERN")
    ap.add_argument("--expect-dev", action="store_true")
    a = ap.parse_args(argv[1:])
    if not a.paths:
        print(__doc__)
        return 2
    missing = [p for p in a.paths if not os.path.exists(p)]
    if missing:
        for p in missing:
            print("error: no such file or folder: " + p)
        return 2
    s = Scan(a.skip)
    for p in a.paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs.sort()
                for f in sorted(files):
                    full = os.path.join(root, f)
                    name = os.path.relpath(full, p).replace("\\", "/")
                    if len(a.paths) > 1:
                        name = p.rstrip("/\\") + "/" + name
                    try:
                        with open(full, "rb") as fh:
                            data = fh.read()
                    except OSError as e:
                        s.unreadable(name, "cannot read it (%s)" % e)
                        continue
                    s.scan(name, data)
        else:
            try:
                with open(p, "rb") as fh:
                    data = fh.read()
            except OSError as e:
                s.unreadable(p, "cannot read it (%s)" % e)
                continue
            s.scan(p, data)
    for h in s.hits:
        print("dev build code: " + h)
    for n in s.skipped:
        print("skipped (--skip): " + n)
    for pr in s.problems:
        print("not checked: " + pr)
    if a.expect_dev:
        print("self-test: " + ("found the dev build, as expected" if s.hits else "FAILED: no dev marker found"))
        return 0 if s.hits else 1
    if s.hits:
        print("release check: FAILED, dev build code found (%d files checked)" % s.files)
        return 1
    if s.problems:
        print("release check: FAILED, %d of %d files not checked (see above): check them another way, then "
              "leave them out with --skip" % (len(s.unchecked), s.files))
        return 3
    print("release check: no dev build code found (%d files checked%s)"
          % (s.files, ", %d skipped" % len(s.skipped) if s.skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
