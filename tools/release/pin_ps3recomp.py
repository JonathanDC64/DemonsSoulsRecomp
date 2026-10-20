#!/usr/bin/env python3
"""ps3recomp.pin: the one ps3recomp commit this port builds with and, for a release, the fork's tag on it.

docs/54 has the scheme. This script reads, checks and changes the pin. port/cmake/Ps3recompPin.cmake makes
the same comparison at configure time, and .github/workflows/pin.yml runs "check --remote".

  pin_ps3recomp.py [--file PIN] show
  pin_ps3recomp.py [--file PIN] check [--remote] [--release]
      The file's format. --remote: the commit is on the fork's master, and the tag (if one is set) points
      to it. --release: a tag is set, because a release never pins a bare commit.
  pin_ps3recomp.py [--file PIN] local [--checkout DIR] [--exact]
      A ps3recomp checkout (default: ps3recomp/ in this repository) contains the pinned commit.
      --exact: it is that commit, with no changes to tracked files (what a release build needs).
  pin_ps3recomp.py [--file PIN] set (--tag TAG | --commit REV) [--checkout DIR]
      Rewrites the pin with TAG or REV resolved to a full commit hash in the checkout.
  pin_ps3recomp.py self-test

Exit status: 0 good, 1 a check failed, 2 a usage, file or git error. Standard library only.
"""
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PIN_FILE = os.path.join(ROOT, "ps3recomp.pin")
DEFAULT_CHECKOUT = os.path.join(ROOT, "ps3recomp")
DEFAULT_REPO = "https://github.com/JonathanDC64/ps3recomp"
KEYS = ("repo", "tag", "commit")
REPO_RE = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
TAG_RE = re.compile(r"^des-[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
HEADER = """\
# The ps3recomp this port builds with (docs/54): one exact commit of the fork, never a branch.
#   repo=    the fork
#   tag=     for a release, the fork's tag on that commit (des-<upstream version>-<n>); "none" between releases
#   commit=  the full commit hash
# Between releases the commit is the oldest ps3recomp this revision of the port works with. A port change
# that needs a newer one moves it forward in the same pull request; the commit has to be on the fork's
# master, so the ps3recomp pull request merges first. Change it with tools/release/pin_ps3recomp.py.
"""


class PinError(Exception):
    """A pin file, git or usage problem: exit status 2."""


def parse(text):
    """The pin's values from the file's text. PinError lists everything wrong with it."""
    values, errors = {}, []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if not sep:
            errors.append(f"line {n}: not key=value: {line!r}")
        elif key not in KEYS:
            errors.append(f"line {n}: unknown key {key!r}")
        elif key in values:
            errors.append(f"line {n}: {key} given twice")
        else:
            values[key] = value
    errors += [f"no {key}= line" for key in KEYS if key not in values]
    if "repo" in values and not REPO_RE.match(values["repo"]):
        errors.append(f"repo is not a GitHub https URL: {values['repo']!r}")
    if "tag" in values and values["tag"] != "none" and not TAG_RE.match(values["tag"]):
        errors.append(f"tag is neither none nor des-<x.y.z>-<n>: {values['tag']!r}")
    if "commit" in values and not COMMIT_RE.match(values["commit"]):
        errors.append(f"commit is not a full 40-character lowercase hash: {values['commit']!r}")
    if errors:
        raise PinError("; ".join(errors))
    return values


def render(values, old_text=None):
    """The file's text: the old file's leading comments (or the standard ones), then the three keys."""
    header = HEADER
    if old_text:
        lead = []
        for raw in old_text.splitlines():
            if raw.strip() and not raw.lstrip().startswith("#"):
                break
            lead.append(raw)
        if any(line.strip() for line in lead):
            header = "\n".join(lead).rstrip("\n") + "\n"
    return header + "".join(f"{key}={values[key]}\n" for key in KEYS)


def read_text(path):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()
    except OSError as e:
        raise PinError(f"cannot read {path}: {e.strerror}") from None


def write_text(path, text, like):
    """Writes text with the line endings of `like` (the old file), so a checkout's CRLF stays CRLF."""
    with open(path, "w", encoding="utf-8", newline="\r\n" if "\r\n" in (like or "") else "\n") as f:
        f.write(text)


def git(args, cwd=None):
    """(exit status, stdout, stderr) of a git command, outputs stripped. --no-optional-locks: git status
    would otherwise rewrite the index of a checkout it only reads."""
    try:
        p = subprocess.run(["git", "--no-optional-locks", *args], cwd=cwd, capture_output=True, encoding="utf-8",
                           errors="replace", check=False)
    except OSError as e:
        raise PinError(f"cannot run git: {e}") from None
    return p.returncode, p.stdout.strip(), p.stderr.strip()


def remove_tree(path):
    """Deletes a temporary git folder (its pack files are read-only on Windows)."""
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


def same_repo(a, b):
    def norm(url):
        url = re.sub(r"^git@github\.com:", "https://github.com/", url.strip()).rstrip("/")
        return (url[:-4] if url.endswith(".git") else url).lower()
    return norm(a) == norm(b)


def remote_problems(values):
    """What the fork says: the commit is on its master; the tag, if one is set, points to the commit."""
    problems = []
    repo, tag, commit = values["repo"], values["tag"], values["commit"]
    if tag != "none":
        rc, out, err = git(["ls-remote", repo, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"])
        if rc != 0:
            raise PinError(f"git ls-remote {repo} failed: {err}")
        refs = {}
        for line in out.splitlines():
            sha, _, ref = line.partition("\t")
            refs[ref] = sha
        target = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
        if not target:
            problems.append(f"the fork has no tag {tag}")
        elif target != commit:
            problems.append(f"tag {tag} points to {target}, not to the pinned {commit}")
        elif f"refs/tags/{tag}^{{}}" not in refs:
            print(f"note: {tag} is a lightweight tag; the scheme uses annotated ones (git tag -a)")
    tmp = tempfile.mkdtemp(prefix="ps3recomp_pin_")
    try:
        # Commits only, no trees or file contents: enough to walk the history of master.
        rc, _, err = git(["clone", "--quiet", "--bare", "--filter=tree:0", "--single-branch",
                          "--branch", "master", "--no-tags", repo, tmp])
        if rc != 0:
            raise PinError(f"cannot fetch the fork's master from {repo}: {err}")
        rc, out, err = git(["rev-list", "refs/heads/master"], cwd=tmp)
        if rc != 0:
            raise PinError(f"cannot list the fork's master: {err}")
        if commit not in set(out.split()):
            problems.append(f"commit {commit} is not on the fork's master. If it belongs to a ps3recomp "
                            "pull request, merge that first, then run this check again")
    finally:
        remove_tree(tmp)
    return problems


def local_problems(values, checkout, exact):
    """How a ps3recomp checkout differs from the pin: (its HEAD, problems)."""
    if not os.path.exists(os.path.join(checkout, ".git")):
        raise PinError(f"{checkout} is not a git checkout")
    rc, head, err = git(["rev-parse", "HEAD"], cwd=checkout)
    if rc != 0:
        raise PinError(f"cannot read the commit of {checkout}: {err}")
    commit, problems = values["commit"], []
    if exact:
        if head != commit:
            problems.append(f"{checkout} is at {head}, not at the pinned {commit}")
        rc, out, err = git(["status", "--porcelain", "--untracked-files=no"], cwd=checkout)
        if rc != 0:
            raise PinError(f"git status failed in {checkout}: {err}")
        if out:
            problems.append(f"{checkout} has changes to tracked files:\n{out}")
    else:
        rc, _, _ = git(["merge-base", "--is-ancestor", commit, "HEAD"], cwd=checkout)
        if rc == 1:
            problems.append(f"{checkout} ({head[:12]}) does not contain the pinned commit {commit[:12]}: "
                            "update it to the fork's master")
        elif rc != 0:
            problems.append(f"{checkout} does not have the pinned commit {commit[:12]} at all: fetch the fork")
    return head, problems


def report(problems, path):
    for p in problems:
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::error file={os.path.relpath(path, ROOT).replace(os.sep, '/')}::{p.splitlines()[0]}")
        print(f"pin_ps3recomp: {p}", file=sys.stderr)
    return 1 if problems else 0


def cmd_set(args):
    old = read_text(args.file) if os.path.exists(args.file) else None
    values = parse(old) if old is not None else {"repo": DEFAULT_REPO}
    rev = args.tag or args.commit
    rc, sha, err = git(["rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"], cwd=args.checkout)
    if rc != 0 or not COMMIT_RE.match(sha):
        raise PinError(f"{rev!r} is not a commit in {args.checkout} {err}".rstrip())
    if args.tag:
        if not TAG_RE.match(args.tag):
            raise PinError(f"{args.tag!r} is not a fork release tag (des-<x.y.z>-<n>)")
        rc, _, _ = git(["rev-parse", "--verify", "--quiet", f"refs/tags/{args.tag}"], cwd=args.checkout)
        if rc != 0:
            raise PinError(f"{args.checkout} has no tag {args.tag}")
    rc, out, _ = git(["remote", "-v"], cwd=args.checkout)
    remotes = [line.split()[0] for line in out.splitlines()
               if len(line.split()) >= 2 and same_repo(line.split()[1], values["repo"])]
    if remotes:
        rc, _, _ = git(["merge-base", "--is-ancestor", sha, f"refs/remotes/{remotes[0]}/master"],
                       cwd=args.checkout)
        if rc != 0:
            print(f"warning: {sha[:12]} is not on {remotes[0]}/master in {args.checkout} "
                  "(fetch it, or merge the ps3recomp pull request first)")
    else:
        print(f"note: no remote of {args.checkout} is {values['repo']}; run 'check --remote' next")
    values = dict(values, tag=args.tag or "none", commit=sha)
    write_text(args.file, render(values, old), old)
    print(f"ps3recomp.pin: tag={values['tag']} commit={sha}")
    return 0


def self_test():
    """Synthetic pins and a throwaway git repository; no network."""
    failures = []

    def expect(name, ok):
        if not ok:
            failures.append(name)

    sha_a = "a" * 40
    good = f"# comment\nrepo={DEFAULT_REPO}\ntag=none\ncommit={sha_a}\n"
    expect("good pin", parse(good) == {"repo": DEFAULT_REPO, "tag": "none", "commit": sha_a})
    expect("tagged pin", parse(good.replace("tag=none", "tag=des-0.14.0-1"))["tag"] == "des-0.14.0-1")
    for name, text in [
            ("missing commit", f"repo={DEFAULT_REPO}\ntag=none\n"),
            ("short commit", good.replace(sha_a, "a" * 12)),
            ("uppercase commit", good.replace(sha_a, "A" * 40)),
            ("v tag", good.replace("tag=none", "tag=v0.14.0")),
            ("tag without n", good.replace("tag=none", "tag=des-0.14.0")),
            ("branch as tag", good.replace("tag=none", "tag=master")),
            ("unknown key", good + "branch=master\n"),
            ("twice", good + "tag=none\n"),
            ("not key=value", good + "commit\n"),
            ("http repo", good.replace("https://", "http://"))]:
        try:
            parse(text)
            failures.append(f"accepted: {name}")
        except PinError:
            pass
    expect("render keeps comments", render(parse(good), good).startswith("# comment\n"))
    expect("render round trip", parse(render(parse(good), good)) == parse(good))
    expect("render default header", render(parse(good)).startswith(HEADER))
    expect("same repo", same_repo(DEFAULT_REPO + ".git/", DEFAULT_REPO.upper()))

    tmp = tempfile.mkdtemp(prefix="ps3recomp_pin_test_")
    try:
        ident = ["-c", "user.name=pin test", "-c", "user.email=pin-test@example.invalid",
                 "-c", "commit.gpgsign=false"]

        def run(*args):
            rc, out, err = git([*ident, *args], cwd=tmp)
            if rc != 0:
                raise PinError(f"self-test git {' '.join(args)}: {err}")
            return out

        run("-c", "init.defaultBranch=master", "init", "-q")
        with open(os.path.join(tmp, "f.txt"), "w") as f:
            f.write("1\n")
        run("add", "f.txt")
        run("commit", "-q", "-m", "a")
        first = run("rev-parse", "HEAD")
        run("commit", "-q", "--allow-empty", "-m", "b")
        second = run("rev-parse", "HEAD")
        run("checkout", "-q", "-b", "side", first)
        run("commit", "-q", "--allow-empty", "-m", "c")
        side = run("rev-parse", "HEAD")
        run("checkout", "-q", "master")

        def problems(commit, exact):
            return local_problems({"repo": DEFAULT_REPO, "tag": "none", "commit": commit}, tmp, exact)[1]

        expect("older pin is contained", problems(first, False) == [])
        expect("same pin is contained", problems(second, False) == [])
        expect("exact pin", problems(second, True) == [])
        expect("exact pin, older", len(problems(first, True)) == 1)
        expect("pin on another line", len(problems(side, False)) == 1)
        expect("unknown pin", len(problems("b" * 40, False)) == 1)
        with open(os.path.join(tmp, "f.txt"), "w") as f:
            f.write("2\n")
        expect("exact pin, dirty", len(problems(second, True)) == 1)
        expect("contained pin ignores dirt", problems(second, False) == [])
    finally:
        remove_tree(tmp)

    for name in failures:
        print(f"self-test FAILED: {name}", file=sys.stderr)
    print(f"self-test: {'FAILED' if failures else 'ok'} ({len(failures)} failures)")
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=PIN_FILE, help="the pin file (default: ps3recomp.pin in this repository)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show", help="print the pin")
    c = sub.add_parser("check", help="check the pin file, and with --remote the fork")
    c.add_argument("--remote", action="store_true", help="ask the fork: commit on master, tag on the commit")
    c.add_argument("--release", action="store_true", help="also require a tag")
    loc = sub.add_parser("local", help="compare a ps3recomp checkout with the pin")
    loc.add_argument("--checkout", default=DEFAULT_CHECKOUT)
    loc.add_argument("--exact", action="store_true", help="the checkout must be the pinned commit, clean")
    s = sub.add_parser("set", help="rewrite the pin from a checkout")
    which = s.add_mutually_exclusive_group(required=True)
    which.add_argument("--tag", help="a fork release tag (des-<x.y.z>-<n>)")
    which.add_argument("--commit", help="a commit (any revision the checkout knows)")
    s.add_argument("--checkout", default=DEFAULT_CHECKOUT)
    sub.add_parser("self-test", help="run the built-in tests")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "self-test":
            return self_test()
        if args.cmd == "set":
            return cmd_set(args)
        values = parse(read_text(args.file))
        if args.cmd == "show":
            print("".join(f"{key}={values[key]}\n" for key in KEYS), end="")
            return 0
        if args.cmd == "check":
            problems = []
            if args.release and values["tag"] == "none":
                problems.append("a release needs a tag: tag=none")
            if args.remote:
                problems += remote_problems(values)
            if not problems:
                print(f"ps3recomp.pin ok: tag={values['tag']} commit={values['commit']}"
                      + (" (checked against the fork)" if args.remote else ""))
            return report(problems, args.file)
        head, problems = local_problems(values, args.checkout, args.exact)
        if not problems:
            print(f"{args.checkout} at {head[:12]} {'is' if args.exact else 'contains'} the pinned "
                  f"{values['commit'][:12]}")
        return report(problems, args.file)
    except PinError as e:
        print(f"pin_ps3recomp: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
