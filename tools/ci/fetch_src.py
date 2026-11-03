#!/usr/bin/env python3
"""CI only: FetchContent's sources kept for a cache, and handed back to a configure.

FetchContent clones the wrapper UI's FreeType, RmlUi and stb by commit, so not
shallow: most of a configure's minute and a half. The workflows cache a copy
of the sources (without .git) and hand it to the configure through
FETCHCONTENT_SOURCE_DIR_<NAME>, which FetchContent takes instead of cloning.

    python tools/ci/fetch_src.py args DIR NAME...
        Prints one -DFETCHCONTENT_SOURCE_DIR_<NAME>=DIR/<name> line per name
        when DIR holds a complete copy (its .complete file), and nothing
        otherwise: the configure then clones, as without the cache.

    python tools/ci/fetch_src.py keep DIR BUILD NAME...
        Copies each BUILD/_deps/<name>-src into DIR/<name>, without .git, and
        writes DIR/.complete last. Never fails the job: when a copy fails, DIR
        is removed (so no half copy is saved) and a warning is printed.

NAME is FetchContent's name in lower case, as in BUILD/_deps (freetype, rmlui,
stb). Exit code 0, apart from wrong arguments.
"""
import os
import shutil
import sys

COMPLETE = ".complete"


def cmake_path(p):
    return os.path.abspath(p).replace("\\", "/")


def args(cache, names):
    if not os.path.isfile(os.path.join(cache, COMPLETE)):
        if os.path.isdir(cache):
            print(f"fetch_src: {cache} has no {COMPLETE}: the configure clones", file=sys.stderr)
        return 0
    for n in names:
        print(f"-DFETCHCONTENT_SOURCE_DIR_{n.upper()}={cmake_path(os.path.join(cache, n))}")
    return 0


def keep(cache, build, names):
    try:
        shutil.rmtree(cache, ignore_errors=True)
        for n in names:
            src = os.path.join(build, "_deps", f"{n}-src")
            if not os.path.isdir(src):
                raise FileNotFoundError(f"no {src}")
            shutil.copytree(src, os.path.join(cache, n), symlinks=True,
                            ignore=shutil.ignore_patterns(".git"))
        with open(os.path.join(cache, COMPLETE), "w", encoding="ascii") as f:
            f.write(" ".join(names) + "\n")
    except Exception as e:  # never in the way: the next run clones again
        shutil.rmtree(cache, ignore_errors=True)
        print(f"::warning::fetch_src: not kept for the cache ({e})")
        return 0
    for n in names:
        size = sum(os.lstat(os.path.join(d, f)).st_size
                   for d, _, fs in os.walk(os.path.join(cache, n)) for f in fs)
        print(f"{n}: {size / 1e6:.1f} MB")
    return 0


def main(argv):
    sys.stdout.reconfigure(newline="\n")   # bash's mapfile on Windows would keep a '\r' in each argument
    if len(argv) >= 3 and argv[0] == "args":
        return args(argv[1], argv[2:])
    if len(argv) >= 4 and argv[0] == "keep":
        return keep(argv[1], argv[2], argv[3:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
