#!/usr/bin/env python3
"""Signs a draft release's SHA256SUMS with the project's release key, so the launcher's updates take it (docs/105
section 7, docs/54 section 8.4). Run it on your own PC after the release workflow has drafted the release and before
you publish it. The key stays on your PC; this script never reads it, ssh-keygen does.

  sign_release.py TAG --key FILE [--repo OWNER/NAME] [--no-attestation] [--dry-run]

1. Downloads the draft's SHA256SUMS and every file it lists, and checks each file against it.
2. Checks each file's build attestation (gh attestation verify): made by this repository's release workflow
   (release.yml) from TAG. --no-attestation skips it, for a draft made before release.yml attested its files.
3. Signs SHA256SUMS: ssh-keygen -Y sign -f FILE -n demons-souls-recomp-release. It asks for the key's passphrase.
4. Checks the signature with the launcher's own code (tools/setup/des_setup/signing.py) against release-keys.txt as
   committed at TAG and at the release tag before it: the players on that version check this update with its list.
5. Uploads SHA256SUMS.sig to the draft. Then read the draft and publish it.

--dry-run does steps 1 and 2 only. Needs gh (signed in) and OpenSSH's ssh-keygen (part of Windows 10 and later, and
of every Linux and macOS). Standard library only.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "setup"))
from des_setup import signing  # noqa: E402

REPO = "JonathanDC64/DemonsSoulsRecomp"
SUMS = "SHA256SUMS"
KEYS_PATH = "tools/setup/des_setup/release-keys.txt"
STABLE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


class Error(RuntimeError):
    pass


def run(args, **kw):
    p = subprocess.run(args, capture_output=True, text=True, check=False, **kw)
    if p.returncode:
        raise Error(f"{' '.join(args[:3])}: {(p.stderr or p.stdout).strip()}")
    return p.stdout


def keys_at(rev):
    """The release keys committed at `rev`, or None if it has no list (a version from before signed updates)."""
    if subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"], cwd=ROOT,
                      capture_output=True, check=False).returncode:
        raise Error(f"{rev} is not in this checkout: run git fetch origin --tags")
    p = subprocess.run(["git", "show", f"{rev}:{KEYS_PATH}"], cwd=ROOT, capture_output=True, check=False)
    if p.returncode:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "keys.txt"
        path.write_bytes(p.stdout)
        return signing.trusted_keys(path)


def release_before(tag):
    """The stable release tag before `tag` (vX.Y.Z, by number), or None for the first."""
    m = STABLE.match(tag)
    if not m:
        return None
    mine = tuple(map(int, m.groups()))
    older = []
    for t in run(["git", "tag", "--list", "v*"], cwd=ROOT).split():
        n = STABLE.match(t)
        if n and tuple(map(int, n.groups())) < mine:
            older.append((tuple(map(int, n.groups())), t))
    return max(older)[1] if older else None


def sums_of(path: Path) -> dict:
    out = {}
    for line in path.read_text(encoding="ascii").splitlines():
        w = line.split()
        if len(w) != 2 or not re.fullmatch(r"[0-9a-f]{64}", w[0]) or "/" in w[1] or "\\" in w[1]:
            raise Error(f"{SUMS}: a line that is not '<sha256>  <file name>': {line!r}")
        out[w[1].lstrip("*")] = w[0]
    return out


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag")
    ap.add_argument("--key", help="your release key's private file (ssh-keygen -t ed25519)")
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--no-attestation", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if not a.dry_run and not a.key:
        ap.error("--key is needed to sign (or --dry-run)")
    for tool in ("gh", "git") + (() if a.dry_run else ("ssh-keygen",)):
        if not shutil.which(tool):
            raise Error(f"{tool} is not on PATH")

    rel = json.loads(run(["gh", "release", "view", a.tag, "--repo", a.repo, "--json", "isDraft,assets"]))
    if not rel.get("isDraft"):
        raise Error(f"{a.tag} is published already: sign a release while it is a draft")
    names = {x["name"] for x in rel.get("assets") or []}
    if SUMS not in names:
        raise Error(f"{a.tag} has no {SUMS}")

    with tempfile.TemporaryDirectory(prefix="des-sign-") as tmp:
        d = Path(tmp)
        run(["gh", "release", "download", a.tag, "--repo", a.repo, "--dir", str(d), "--pattern", SUMS])
        sums = sums_of(d / SUMS)
        missing = sorted(set(sums) - names)
        if missing:
            raise Error(f"{SUMS} lists files the draft does not have: {', '.join(missing)}")
        print(f"sign: downloading the {len(sums)} files {SUMS} lists")
        pats = [x for n in sorted(sums) for x in ("--pattern", n)]
        run(["gh", "release", "download", a.tag, "--repo", a.repo, "--dir", str(d), *pats])
        for n, want in sorted(sums.items()):
            if sha256(d / n) != want:
                raise Error(f"{n} does not match {SUMS}")
            print(f"ok   {n} matches {SUMS}")
        if not a.no_attestation:
            for n in sorted(sums):
                run(["gh", "attestation", "verify", str(d / n), "--repo", a.repo,
                     "--signer-workflow", f"{a.repo}/.github/workflows/release.yml",
                     "--source-ref", f"refs/tags/{a.tag}"])
                print(f"ok   {n} was built by release.yml from {a.tag}")
        if a.dry_run:
            print("sign: dry run, nothing signed")
            return 0

        # The lists that must hold the key: this release's (its players check the next update with it) and the
        # release's before it (its players check this one).
        lists = {a.tag: keys_at(a.tag)}
        if not lists[a.tag]:
            raise Error(f"{KEYS_PATH} at {a.tag} lists no key: add your public key line to it first")
        before = release_before(a.tag)
        if before:
            keys = keys_at(before)
            if keys is None:
                print(f"note {before}'s launcher checks no signature, so its players take this update either way")
            elif not keys:
                print(f"WARN {before} lists no release key, so its players cannot update in the launcher")
            else:
                lists[before] = keys

        print("sign: ssh-keygen signs SHA256SUMS; it asks for your key's passphrase")
        p = subprocess.run(["ssh-keygen", "-Y", "sign", "-f", a.key, "-n", signing.NAMESPACE, str(d / SUMS)],
                           check=False)
        sig = d / (SUMS + signing.SIG_SUFFIX)
        if p.returncode or not sig.is_file():
            raise Error("ssh-keygen did not sign")
        for rev, keys in lists.items():
            try:
                signing.verify((d / SUMS).read_bytes(), sig.read_text(encoding="ascii"), keys)
            except signing.BadSignature as e:
                raise Error(f"the signature does not check against {KEYS_PATH} at {rev}: {e}") from e
            print(f"ok   the signature checks against the keys at {rev}")
        run(["gh", "release", "upload", a.tag, str(sig), "--repo", a.repo, "--clobber"])
        print(f"sign: {sig.name} is on the draft {a.tag}. Read the draft, then publish it.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Error as e:
        print(f"sign: {e}", file=sys.stderr)
        sys.exit(1)
