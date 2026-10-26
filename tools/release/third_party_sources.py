#!/usr/bin/env python3
"""The source archives of the LGPL libraries a release ships, for the release page.

The Linux tarball carries vkd3d's and FFmpeg's libraries beside des_host. Both are LGPL 2.1 or later, which asks
for their source to go with the binaries, so every release attaches the exact archives host.yml built them from.
The versions and SHA-256s are read from host.yml's env (VKD3D_VERSION, VKD3D_SHA256, FFMPEG_VERSION,
FFMPEG_SHA256), so the two can't drift apart.

  third_party_sources.py [--workflow FILE] --out DIR

Downloads each archive into DIR, checks it against the pinned SHA-256, and prints the paths. Exit status: 0 good,
1 a download or check failed, 2 a usage or file error. Standard library only.
"""
import argparse
import hashlib
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_YML = os.path.join(ROOT, ".github", "workflows", "host.yml")

# name -> (env key of the version, env key of the SHA-256, URL with {v} for the version, file name with {v})
SOURCES = {
    "vkd3d": ("VKD3D_VERSION", "VKD3D_SHA256",
              "https://dl.winehq.org/vkd3d/source/vkd3d-{v}.tar.xz", "vkd3d-{v}.tar.xz"),
    "ffmpeg": ("FFMPEG_VERSION", "FFMPEG_SHA256",
               "https://ffmpeg.org/releases/ffmpeg-{v}.tar.xz", "ffmpeg-{v}.tar.xz"),
}


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


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--workflow", default=HOST_YML, help="host.yml, which pins the versions")
    ap.add_argument("--out", required=True, help="where the archives go")
    a = ap.parse_args(argv)
    try:
        env = read_env(a.workflow)
    except OSError as e:
        print(f"third_party_sources: {e}", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    status = 0
    for name, (ver_key, sha_key, url, file) in SOURCES.items():
        ver, sha = env.get(ver_key), env.get(sha_key)
        if not ver or not sha or not re.fullmatch(r"[0-9a-fA-F]{64}", sha):
            print(f"third_party_sources: {name}: {ver_key} or {sha_key} missing from {a.workflow}", file=sys.stderr)
            return 2
        dest = os.path.join(a.out, file.format(v=ver))
        try:
            fetch(url.format(v=ver), dest, sha)
        except (OSError, ValueError) as e:
            print(f"third_party_sources: {name}: {e}", file=sys.stderr)
            status = 1
            continue
        print(dest)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
