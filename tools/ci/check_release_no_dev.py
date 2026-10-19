#!/usr/bin/env python3
"""Fail when a release contains developer-build code (docs/55).

A DES_DEV build compiles in the dev cheats and the settings window's Developer
tab. Both carry DES_DEV_MARKER, and the cheats read the knobs named below.
Release files must contain none of them. Run on the folder or zip that is
uploaded, after packaging:

    python check_release_no_dev.py dist/DemonsSoulsRecomp.zip
    python check_release_no_dev.py --expect-dev build-dev/des_ui.lib   # the self-test

Every file is read as bytes, zips are opened (also nested ones), and each
string is looked for in ASCII and in UTF-16LE. Exit 0 when nothing is found,
1 when something is (each hit is listed), 2 on a usage error. With
--expect-dev the result is inverted, so CI can check that the scan does find a
dev build. Standard library only.
"""
import io
import sys
import zipfile

MARKER = b"DES-DEV-BUILD"  # the start of DES_DEV_MARKER (port/des_dev_marker.h)
KNOBS = [b"DES_GOD", b"DES_ATK", b"DES_SPEED", b"DES_WARP"]
NEEDLES = [MARKER] + KNOBS
TEXT_SKIP = (".md", ".txt", ".html")  # docs may name the knobs; binaries may not


def patterns():
    for n in NEEDLES:
        yield n, n
        yield n + b" (UTF-16LE)", n.decode("ascii").encode("utf-16-le")


def scan_bytes(name, data, hits, depth=0):
    if name.lower().endswith(".zip") or data[:4] == b"PK\x03\x04":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for info in z.infolist():
                    if not info.is_dir() and depth < 4:
                        scan_bytes(name + "!" + info.filename, z.read(info), hits, depth + 1)
            return
        except zipfile.BadZipFile:
            pass  # not a zip after all: scan it as bytes
    if name.lower().endswith(TEXT_SKIP):
        return
    for label, pat in patterns():
        at = data.find(pat)
        if at >= 0:
            hits.append(f"{name}: {label.decode('ascii', 'replace')} at offset {at:#x}")


def main(argv):
    expect_dev = "--expect-dev" in argv
    paths = [a for a in argv[1:] if a != "--expect-dev"]
    if not paths:
        print(__doc__)
        return 2
    import os
    hits = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in files:
                    full = os.path.join(root, f)
                    with open(full, "rb") as fh:
                        scan_bytes(os.path.relpath(full, p), fh.read(), hits)
        else:
            with open(p, "rb") as fh:
                scan_bytes(p, fh.read(), hits)
    for h in hits:
        print("dev build code: " + h)
    if expect_dev:
        print("self-test: " + ("found the dev build, as expected" if hits else "FAILED: no dev marker found"))
        return 0 if hits else 1
    print("release check: " + ("FAILED" if hits else "no dev build code found"))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
