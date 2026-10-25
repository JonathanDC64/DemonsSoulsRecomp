#!/usr/bin/env python3
"""A release of the port as one zip of redistributable files (docs/54), the same zip on any machine.

  package.py check-tag TAG
      TAG is v<VERSION> (VERSION as committed at the tag), on origin/master, and the ps3recomp.pin committed with it
      names one of the fork's release tags.
  package.py build --rev REV [--ps3recomp DIR] [--host EXE] [--linux-host DIR] [--out DIR] [--dry-run]
      Writes <out>/DemonsSoulsRecomp-<version>-source.zip and <out>/SHA256SUMS, and leaves the zipped files in
      <out>/stage for the game-file check. REV is the release tag, or any commit for a dry run. --host: also
      <out>/DemonsSoulsRecomp-<version>-windows.zip, the player's download: the same files with EXE (host.yml's
      des_host.exe) and the des_host.map beside it at the top, where the exe finds setup's engine (tools/setup),
      and release-files.txt, the list of the zip's files, which tells the update which files are the program's
      (tools/setup/des_setup/update.py, docs/105).

The zip, under DemonsSoulsRecomp-<version>/: the port's tracked files at REV (without .github/ and .claude/); in
ps3recomp/, the fork's tracked files at the pinned commit (without .github/, scratch/, packaging/ and the runtime's
lifted SPU test programs, none of which a build of the port uses) with a copy of the pin; and release-info.txt (the
version and both commits; port/cmake/DesVersion.cmake reads it where there is no git). Nothing in either repository
is made from the game (the game-file check), so nothing in the zip is.

--linux-host DIR also writes <out>/DemonsSoulsRecomp-<version>-linux.tar.xz, the Linux player's download: the same
files with DIR's at the top (host.yml's host-linux folder: des_host, its map, the SDL2 and vkd3d libraries it finds
beside it, and their licences in licenses/) and its own release-files.txt. A tar, which keeps the execute bits that
a zip unpacked by Python loses, with the same fixed dates, owner and modes.

The same REV gives the same zip: entries sorted, dated with REV's commit time, fixed permissions, LF line endings as
committed. (Deflate output can differ between zlib versions; the release workflow pins Python.) --ps3recomp takes the
fork from a local checkout instead of fetching it; --dry-run allows a revision without a pin (the port alone).
Standard library only.
"""
import argparse
import hashlib
import io
import lzma
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAME = "DemonsSoulsRecomp"
TAG_RE = re.compile(r"^v([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")
FORK_TAG_RE = re.compile(r"^des-[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$")
PORT_SKIP = (re.compile(r"^\.github/"), re.compile(r"^\.claude/"))
PS3RECOMP_SKIP = (re.compile(r"^\.github/"), re.compile(r"^scratch/"), re.compile(r"^packaging/"),
                  re.compile(r"^runtime/spu/tests/out_[^/]+/"))
# Plain text, so the archives come out byte for byte as committed (LF) whatever this machine's git settings.
GIT_PLAIN = ["-c", "core.autocrlf=false", "-c", "core.eol=lf"]


class Error(Exception):
    pass


def git(args, cwd=ROOT, binary=False):
    p = subprocess.run(["git", *GIT_PLAIN, *args], cwd=cwd, capture_output=True, check=False)
    if p.returncode != 0:
        raise Error(f"git {' '.join(args)}: {p.stderr.decode('utf-8', 'replace').strip()}")
    return p.stdout if binary else p.stdout.decode("utf-8", "replace").strip()


def try_git(args, cwd=ROOT):
    p = subprocess.run(["git", *GIT_PLAIN, *args], cwd=cwd, capture_output=True, check=False)
    return p.stdout.decode("utf-8", "replace").strip() if p.returncode == 0 else None


def remove_tree(path):
    def retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    try:
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=retry)
        else:
            shutil.rmtree(path, onerror=retry)
    except OSError:
        pass


def parse_keys(text):
    values = {}
    for line in (text or "").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return values


def read_pin(rev):
    text = try_git(["show", f"{rev}:ps3recomp.pin"])
    if text is None:
        return None, None
    pin = parse_keys(text)
    if not re.fullmatch(r"[0-9a-f]{40}", pin.get("commit", "")) or not pin.get("repo"):
        raise Error(f"ps3recomp.pin at {rev} has no valid repo= and commit= lines")
    return pin, text.replace("\r\n", "\n").rstrip("\n") + "\n"


def tree_files(tar_bytes, prefix, skip):
    """(path under prefix, mode, data) for each regular file of a `git archive` tar, minus the skipped paths."""
    files = []
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as tar:
        for member in tar:
            if member.isdir() or member.name == "pax_global_header" or member.type == tarfile.XGLTYPE:
                continue
            if any(rx.search(member.name) for rx in skip):
                continue
            if not member.isfile():
                raise Error(f"{member.name}: not a regular file (symbolic links are not packaged)")
            mode = 0o755 if member.mode & 0o111 else 0o644
            files.append((f"{prefix}{member.name}", mode, tar.extractfile(member).read()))
    return files


def fork_tar(pin, checkout):
    """`git archive` of the fork at the pinned commit, from a local checkout or fetched (that commit only)."""
    commit = pin["commit"]
    if checkout:
        if try_git(["cat-file", "-e", f"{commit}^{{commit}}"], cwd=checkout) is None:
            raise Error(f"{checkout} does not have the pinned commit {commit}")
        return git(["archive", "--format=tar", commit], cwd=checkout, binary=True)
    tmp = tempfile.mkdtemp(prefix="package_fork_")
    try:
        git(["init", "-q"], cwd=tmp)
        git(["fetch", "-q", "--depth", "1", "--no-tags", pin["repo"], commit], cwd=tmp)
        return git(["archive", "--format=tar", commit], cwd=tmp, binary=True)
    finally:
        remove_tree(tmp)


def write_zip(path, files, when):
    """A zip that depends only on `files` and `when`: sorted, one date, fixed attributes."""
    stamp = time.gmtime(max(when, 315532800))[:6]  # (zip dates start in 1980)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name, mode, data in sorted(files):
            info = zipfile.ZipInfo(name, stamp)
            info.create_system = 3  # Unix, so the permissions below are read
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data, compresslevel=9)


def write_tar_xz(path, files, when):
    """A tar.xz that depends only on `files` and `when`: sorted, one date, owner 0, fixed modes (POSIX tar, which
    takes long paths; GNU tar, bsdtar and Python read it)."""
    with open(path, "wb") as raw:
        with lzma.open(raw, "wb", format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=9) as xz:
            with tarfile.open(fileobj=xz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for name, mode, data in sorted(files):
                    info = tarfile.TarInfo(name)
                    info.size, info.mode, info.mtime = len(data), mode, max(when, 0)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    tar.addfile(info, io.BytesIO(data))


def linux_host_files(folder, top):
    """The Linux tarball's extra entries: every file in host.yml's host-linux folder, under the same relative path
    at the top of the tree; des_host executable, the rest (libraries, map, licences) read-only data."""
    if not os.path.isfile(os.path.join(folder, "des_host")):
        raise Error(f"--linux-host {folder}: no des_host in it")
    entries = []
    for base, dirs, names in os.walk(folder):
        dirs.sort()
        for n in sorted(names):
            path = os.path.join(base, n)
            if os.path.islink(path) or not os.path.isfile(path):
                raise Error(f"--linux-host: {path} is not a regular file")
            rel = os.path.relpath(path, folder).replace(os.sep, "/")
            with open(path, "rb") as f:
                entries.append((top + rel, 0o755 if rel == "des_host" else 0o644, f.read()))
    return entries


def cmd_check_tag(args):
    m = TAG_RE.match(args.tag)
    if not m:
        raise Error(f"{args.tag}: not a release tag (v<major>.<minor>.<patch>, optionally -<pre-release>)")
    if try_git(["rev-parse", "--verify", "-q", f"refs/tags/{args.tag}"]) is None:
        raise Error(f"there is no tag {args.tag} here")
    problems = []
    version = try_git(["show", f"{args.tag}:VERSION"])
    if version is None:
        problems.append("there is no VERSION file at the tag")
    elif version.strip() != m.group(1):
        problems.append(f"VERSION at the tag says {version.strip()}, the tag says {m.group(1)}")
    if try_git(["rev-parse", "--verify", "-q", "refs/remotes/origin/master"]) is None:
        problems.append("origin/master is not known here (fetch it)")
    elif subprocess.run(["git", "merge-base", "--is-ancestor", args.tag, "refs/remotes/origin/master"], cwd=ROOT,
                        capture_output=True, check=False).returncode != 0:
        problems.append(f"{args.tag} is not on origin/master")
    if try_git(["cat-file", "-t", f"refs/tags/{args.tag}"]) != "tag":
        print(f"note: {args.tag} is a lightweight tag; the scheme uses annotated ones (git tag -a)")
    pin, _ = read_pin(args.tag)
    if pin is None:
        problems.append("there is no ps3recomp.pin at the tag")
    elif not FORK_TAG_RE.match(pin.get("tag", "")):
        problems.append(f"ps3recomp.pin at the tag names no fork release tag (tag={pin.get('tag', '')})")
    for p in problems:
        print(f"::error::{p}" if os.environ.get("GITHUB_ACTIONS") == "true" else f"package: {p}", file=sys.stderr)
    if not problems:
        print(f"{args.tag}: version {m.group(1)}, on origin/master, ps3recomp {pin['tag']} ({pin['commit'][:12]})")
    return 1 if problems else 0


def host_files(exe, top):
    """The player's zip's extra entries: the host exe as des_host.exe, and its linker map (which symbolizes crash
    addresses in it) when one is beside it, both at the top of the tree."""
    if not os.path.isfile(exe):
        raise Error(f"--host {exe}: no such file")
    entries = []
    mapfile = os.path.splitext(exe)[0] + ".map"
    for path, name, mode in ((exe, "des_host.exe", 0o755), (mapfile, "des_host.map", 0o644)):
        if os.path.isfile(path):
            with open(path, "rb") as f:
                entries.append((top + name, mode, f.read()))
    return entries


MANIFEST = "release-files.txt"


def manifest(files, top):
    """release-files.txt for a zip of `files` (itself listed too): the update swaps the names at the top of the
    install that it lists, and leaves the player's own files alone (docs/105)."""
    names = sorted({f[0][len(top):] for f in files} | {MANIFEST})
    text = ("# Written by tools/release/package.py: the files of this release, which an update replaces (docs/105).\n"
            + "".join(n + "\n" for n in names))
    return (top + MANIFEST, 0o644, text.encode())


def cmd_build(args):
    sha = git(["rev-parse", "--verify", f"{args.rev}^{{commit}}"])
    when = int(git(["log", "-1", "--format=%ct", sha]))
    m = TAG_RE.match(args.rev)
    if m:
        version = m.group(1)
    else:
        base = (try_git(["show", f"{sha}:VERSION"]) or "0.0.0").strip()
        if not VERSION_RE.match(base):
            raise Error(f"VERSION at {args.rev} is not a version: {base!r}")
        version = f"{base}{'.' if '-' in base else '-'}dev+g{sha[:9]}"
    top = f"{NAME}-{version}/"
    files = tree_files(git(["archive", "--format=tar", sha], binary=True), top, PORT_SKIP)
    pin, pin_text = read_pin(sha)
    info = [f"# Written by tools/release/package.py: which source this is (docs/54). Builds read it where there is",
            f"# no git (port/cmake/DesVersion.cmake).", f"version={version}", f"commit={sha}"]
    if pin is None:
        if not args.dry_run:
            raise Error(f"there is no ps3recomp.pin at {args.rev}: a release needs one")
        print(f"note: no ps3recomp.pin at {args.rev}; the dry run packages the port alone")
    else:
        fork = tree_files(fork_tar(pin, args.ps3recomp), f"{top}ps3recomp/", PS3RECOMP_SKIP)
        files += fork
        files.append((f"{top}ps3recomp/ps3recomp.pin", 0o644, pin_text.encode()))
        info += [f"ps3recomp_tag={pin.get('tag', 'none')}", f"ps3recomp_commit={pin['commit']}"]
    files.append((f"{top}release-info.txt", 0o644, ("\n".join(info) + "\n").encode()))
    names = [f[0] for f in files]
    if len(names) != len(set(names)):
        raise Error("two packaged files have the same path")

    out = os.path.abspath(args.out)
    stage = os.path.join(out, "stage")
    if os.path.exists(stage):
        remove_tree(stage)
    for name, _mode, data in files:
        dest = os.path.join(stage, *name.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
    zips = [(f"{NAME}-{version}-source.zip", files)]
    if args.host:
        player = files + host_files(args.host, top)
        zips.append((f"{NAME}-{version}-windows.zip", player + [manifest(player, top)]))
    sums = []
    for zip_name, zipped in zips:
        write_zip(os.path.join(out, zip_name), zipped, when)
        with open(os.path.join(out, zip_name), "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        sums.append(f"{digest}  {zip_name}\n")
        size = os.path.getsize(os.path.join(out, zip_name))
        print(f"package: {zip_name}: {len(zipped)} files, {size / 1e6:.1f} MB, sha256 {digest}")

    if args.linux_host:
        tar_name = f"{NAME}-{version}-linux.tar.xz"
        tarred = files + linux_host_files(args.linux_host, top)
        names = [f[0] for f in tarred]
        if len(names) != len(set(names)):
            raise Error("--linux-host: a file of the host folder has the path of a packaged file")
        tarred.append(manifest(tarred, top))   # the update's list of program files, as in the Windows zip
        write_tar_xz(os.path.join(out, tar_name), tarred, when)
        with open(os.path.join(out, tar_name), "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        sums.append(f"{digest}  {tar_name}\n")
        size = os.path.getsize(os.path.join(out, tar_name))
        print(f"package: {tar_name}: {len(tarred)} files, {size / 1e6:.1f} MB, sha256 {digest}")
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        f.writelines(sums)
    print(f"package: version {version}" + (f", ps3recomp {pin.get('tag')} ({pin['commit'][:12]})" if pin else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check-tag", help="check a release tag before packaging it")
    c.add_argument("tag")
    b = sub.add_parser("build", help="write the zip, SHA256SUMS and the staged files")
    b.add_argument("--rev", default="HEAD", help="the release tag, or any commit for a dry run (default HEAD)")
    b.add_argument("--ps3recomp", help="a checkout of the fork that has the pinned commit (default: fetch it)")
    b.add_argument("--host", help="des_host.exe (host.yml's): also write the Windows zip with it")
    b.add_argument("--linux-host", help="host.yml's host-linux folder: also write the Linux tar.xz with it")
    b.add_argument("--out", default=os.path.join(ROOT, "dist"), help="output folder (default dist/)")
    b.add_argument("--dry-run", action="store_true", help="allow a revision without ps3recomp.pin")
    args = ap.parse_args(argv)
    try:
        return cmd_check_tag(args) if args.cmd == "check-tag" else cmd_build(args)
    except Error as e:
        print(f"package: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
