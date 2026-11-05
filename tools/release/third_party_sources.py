#!/usr/bin/env python3
"""The source archives of the LGPL libraries a release ships, on a release of their own (docs/54 section 8.6).

The downloads carry FFmpeg's libraries, and the Linux one vkd3d's too. Both are LGPL 2.1 or later, which asks for their
source to be offered with the binaries. The third-party-sources release holds the exact archives host.yml built them
from, so that each game release's page has only what a player needs. The versions and SHA-256s are read from host.yml's
env (VKD3D_VERSION, VKD3D_SHA256, FFMPEG_VERSION, FFMPEG_SHA256), so the two can't drift apart.

  third_party_sources.py [--workflow FILE] --check
  third_party_sources.py [--workflow FILE] --out DIR

--check: that release has each archive, by the pinned SHA-256. release.yml stops on a version with no archive there.
--out: downloads each archive into DIR from its own project, checks it, and prints the paths: what to add to the release
after moving to a new version (gh release upload third-party-sources DIR/*). Each address is tried three times, then the
next one: the project's own site first, then a copy of the same archive (FFmpeg's is attached to the fork's FFmpeg
release). The SHA-256 decides, so a copy can't change what ships.

Exit status: 0 good, 1 a download or check failed, 2 a usage or file error. Standard library only.
"""
import argparse
import hashlib
import os
import re
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_YML = os.path.join(ROOT, ".github", "workflows", "host.yml")

HOME = "https://github.com/JonathanDC64/Soulbrandt/releases/download/third-party-sources"

# name -> (env key of the version, env key of the SHA-256, URLs with {v} for the version (tried in order), file name
# with {v})
SOURCES = {
    "vkd3d": ("VKD3D_VERSION", "VKD3D_SHA256",
              ["https://dl.winehq.org/vkd3d/source/vkd3d-{v}.tar.xz"], "vkd3d-{v}.tar.xz"),
    # (the fork's FFmpeg release, ffmpeg-archive.yml, attaches the source archive it built from: the same bytes; its
    # tag carries the package revision, as ps3recomp's cmake/ffmpeg_download.cmake pins it)
    "ffmpeg": ("FFMPEG_VERSION", "FFMPEG_SHA256",
               ["https://ffmpeg.org/releases/ffmpeg-{v}.tar.xz",
                "https://github.com/JonathanDC64/ps3recomp/releases/download/ffmpeg-{v}-2/ffmpeg-{v}.tar.xz"],
               "ffmpeg-{v}.tar.xz"),
}
TRIES = 3          # per address
WAIT_S = (10, 30)  # before the second and the third try


def read_env(path):
    """KEY: value pairs from the workflow's env blocks (quotes stripped). Later keys win; each is set once."""
    env = {}
    pat = re.compile(r"^\s*([A-Z0-9_]+):\s*['\"]?([^'\"#\s]+)['\"]?\s*(#.*)?$")
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = pat.match(line)
            if m:
                env[m.group(1)] = m.group(2)
    return env


def fetch(url, dest, sha256):
    h = hashlib.sha256()
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            h.update(block)
            f.write(block)
    if h.hexdigest() != sha256.lower():
        os.remove(tmp)
        raise ValueError(f"{os.path.basename(dest)}: SHA-256 {h.hexdigest()}, expected {sha256}")
    os.replace(tmp, dest)


def fetch_any(urls, dest, sha256, name):
    """fetch from the first address that works; each one TRIES times. Raises the last error."""
    last = None
    for url in urls:
        for i in range(TRIES):
            if i:
                time.sleep(WAIT_S[i - 1])
            try:
                fetch(url, dest, sha256)
                return url
            except (OSError, ValueError) as e:
                last = e
                print(f"third_party_sources: {name}: {url}: {e}", file=sys.stderr)
                if isinstance(e, ValueError):
                    break  # (the wrong bytes: the same again from there)
    raise last


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--workflow", default=HOST_YML, help="host.yml, which pins the versions")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="the third-party-sources release has each archive")
    mode.add_argument("--out", help="where the archives go, from their own projects")
    a = ap.parse_args(argv)
    try:
        env = read_env(a.workflow)
    except OSError as e:
        print(f"third_party_sources: {e}", file=sys.stderr)
        return 2
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    status = 0
    for name, (ver_key, sha_key, urls, file) in SOURCES.items():
        ver, sha = env.get(ver_key), env.get(sha_key)
        if not ver or not sha or not re.fullmatch(r"[0-9a-fA-F]{64}", sha):
            print(f"third_party_sources: {name}: {ver_key} or {sha_key} missing from {a.workflow}", file=sys.stderr)
            return 2
        filename = file.format(v=ver)
        try:
            if a.check:
                with tempfile.TemporaryDirectory() as tmp:
                    fetch_any([f"{HOME}/{filename}"], os.path.join(tmp, filename), sha, name)
                print(f"ok   {filename} is on the third-party-sources release")
            else:
                dest = os.path.join(a.out, filename)
                fetch_any([u.format(v=ver) for u in urls], dest, sha, name)
                print(dest)
        except (OSError, ValueError) as e:
            print(f"third_party_sources: {name}: {e}", file=sys.stderr)
            if a.check:
                print(f"third_party_sources: add {filename} to the third-party-sources release (this script's --out, "
                      "then gh release upload)", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
