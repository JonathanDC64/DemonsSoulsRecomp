#!/usr/bin/env python3
"""A release of the port as one zip of redistributable files (docs/54), the same zip on any machine.

  package.py check-tag TAG
      TAG is v<VERSION> (VERSION as committed at the tag), on origin/master, and the ps3recomp.pin committed with it
      names one of the fork's release tags.
  package.py build --rev REV [--ps3recomp DIR] [--out DIR] [--dry-run]
      Writes <out>/DemonsSoulsRecomp-<version>-source.zip and <out>/SHA256SUMS, and leaves the zipped files in
      <out>/stage for the game-file check. REV is the release tag, or any commit for a dry run.

The zip, under DemonsSoulsRecomp-<version>/: the port's tracked files at REV (without .github/ and .claude/); in
ps3recomp/, the fork's tracked files at the pinned commit (without .github/, scratch/, packaging/ and the runtime's
lifted SPU test programs, none of which a build of the port uses) with a copy of the pin; and release-info.txt (the
version and both commits; port/cmake/DesVersion.cmake reads it where there is no git). Nothing in either repository
is made from the game (the game-file check), so nothing in the zip is.

The same REV gives the same zip: entries sorted, dated with REV's commit time, fixed permissions, LF line endings as
committed. (Deflate output can differ between zlib versions; the release workflow pins Python.) --ps3recomp takes the
fork from a local checkout instead of fetching it; --dry-run allows a revision without a pin (the port alone).
Standard library only.
"""
import argparse
import hashlib
import io
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
    zip_name = f"{NAME}-{version}-source.zip"
    write_zip(os.path.join(out, zip_name), files, when)
    with open(os.path.join(out, zip_name), "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        f.write(f"{digest}  {zip_name}\n")
    size = os.path.getsize(os.path.join(out, zip_name))
    print(f"package: {zip_name}: {len(files)} files, {size / 1e6:.1f} MB, sha256 {digest}")
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
