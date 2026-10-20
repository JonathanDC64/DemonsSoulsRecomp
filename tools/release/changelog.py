#!/usr/bin/env python3
"""Release notes for a version tag (docs/54), from git alone.

The pull requests merged into master since the previous release, read from their merge commits ("Merge pull request
#N from owner/branch", the pull request's title on the next line) and grouped by the branch's prefix (feat/, fix/,
perf/, docs/, tools/, ...). Then the ps3recomp the release pins (ps3recomp.pin) and, when the pin moved, the fork's
pull requests between the two pinned commits.

  changelog.py [--to REV] [--from REV] [--ps3recomp DIR] [--out FILE]

--to: the release (default HEAD). --from: the previous release (default: the nearest release tag before --to; for a
final release the nearest final one, so 0.2.0 lists everything since 0.1.0 and not only since 0.2.0-rc.2; the start
of the history if there is none). --ps3recomp: a checkout of the fork that has both pinned commits; without it, the
fork's history (commits only) is fetched from the repository the pin names. Standard library only.
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
TAG_RE = re.compile(r"^v([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
MERGE_RE = re.compile(r"^Merge pull request #([0-9]+) from ([^/\s]+)/(\S+)$")
GROUPS = [  # (heading, branch prefixes), in the order they are printed
    ("New and changed", ("feat", "feature", "ui")),
    ("Fixes", ("fix", "bugfix", "hotfix")),
    ("Performance", ("perf",)),
    ("Documentation", ("docs", "doc")),
    ("Tools, build and CI", ("tools", "tool", "build", "ci", "meta", "chore", "test", "tests", "release")),
]
OTHER = "Other"
DIRECT_LIST_MAX = 15  # direct commits on master listed one by one; above this, only counted


class Error(Exception):
    pass


def git(args, cwd=ROOT, ok_codes=(0,)):
    p = subprocess.run(["git", *args], cwd=cwd, capture_output=True, encoding="utf-8", errors="replace", check=False)
    if p.returncode not in ok_codes:
        raise Error(f"git {' '.join(args)}: {p.stderr.strip()}")
    return p.returncode, p.stdout


def git_out(args, cwd=ROOT):
    return git(args, cwd)[1].strip()


def try_git(args, cwd=ROOT):
    """stdout of a git command, or None when it fails."""
    p = subprocess.run(["git", *args], cwd=cwd, capture_output=True, encoding="utf-8", errors="replace", check=False)
    return p.stdout.strip() if p.returncode == 0 else None


def previous_release(to):
    """The nearest release tag before `to` (see the module text), or None."""
    final = True
    m = TAG_RE.match(to)
    if m:
        final = m.group(2) is None
    args = ["describe", "--tags", "--abbrev=0", "--match", "v[0-9]*"]
    if final:
        args += ["--exclude", "v*-*"]
    tag = try_git(args + [f"{to}^"])
    return tag if tag and TAG_RE.match(tag) else None


def first_parent_commits(span, cwd=ROOT):
    """(sha, parents, subject, body) of the first-parent history in `span`, oldest first."""
    _, out = git(["log", "--first-parent", "--reverse", "--format=%H%x1f%P%x1f%s%x1f%b%x1e", span], cwd)
    commits = []
    for record in out.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        sha, parents, subject, body = (record.split("\x1f") + ["", "", ""])[:4]
        commits.append((sha, parents.split(), subject.strip(), body.strip()))
    return commits


def pull_requests(span, cwd=ROOT):
    """([(number, branch, title)], [direct commit subjects]) of the first-parent history in `span`."""
    prs, direct = [], []
    for _sha, parents, subject, body in first_parent_commits(span, cwd):
        m = MERGE_RE.match(subject)
        if m and len(parents) > 1:
            title = next((line.strip() for line in body.splitlines() if line.strip()), m.group(3))
            prs.append((int(m.group(1)), m.group(3), title))
        else:
            direct.append(subject)
    return prs, direct


def group_of(branch):
    prefix = branch.split("/", 1)[0].lower() if "/" in branch else ""
    for heading, prefixes in GROUPS:
        if prefix in prefixes:
            return heading
    return OTHER


def pr_sections(prs, direct, ref_prefix):
    """Markdown sections: the pull requests by group, then the commits made straight on master."""
    lines = []
    by_group = {}
    for number, branch, title in prs:
        by_group.setdefault(group_of(branch), []).append((number, title))
    for heading in [g[0] for g in GROUPS] + [OTHER]:
        if heading in by_group:
            lines += ["", f"### {heading}", ""]
            lines += [f"- {title} ({ref_prefix}#{number})" for number, title in by_group[heading]]
    if direct:
        lines += ["", "### Commits made directly on master", ""]
        if len(direct) <= DIRECT_LIST_MAX:
            lines += [f"- {subject}" for subject in direct]
        else:
            lines.append(f"- {len(direct)} commits (see the history)")
    return lines


def read_pin(rev):
    """{repo, tag, commit} from ps3recomp.pin at `rev`, or None if there is none."""
    text = try_git(["show", f"{rev}:ps3recomp.pin"])
    if text is None:
        return None
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    if not re.fullmatch(r"[0-9a-f]{40}", values.get("commit", "")):
        return None
    return values


def pin_name(pin):
    short = pin["commit"][:12]
    tag = pin.get("tag", "none")
    return f"{tag} ({short})" if tag and tag != "none" else short


def repo_slug(url):
    m = re.match(r"^https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?$", url or "")
    return m.group(1) if m else None


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


def fork_section(old, new, checkout):
    """Markdown lines for the pinned ps3recomp and the fork's pull requests between the two pins."""
    if new is None:
        return ["", "### ps3recomp", "", "Not pinned: this revision has no ps3recomp.pin."]
    lines = ["", "### ps3recomp", "", f"Built with ps3recomp {pin_name(new)}, from {new.get('repo', 'the fork')}."]
    if old is None or old["commit"] == new["commit"]:
        if old is not None:
            lines.append("Unchanged since the previous release.")
        return lines
    slug = repo_slug(new.get("repo")) or "ps3recomp"
    tmp = None
    try:
        cwd = checkout
        if cwd is None:
            tmp = tempfile.mkdtemp(prefix="changelog_fork_")
            git(["clone", "--quiet", "--bare", "--filter=tree:0", "--single-branch", "--branch", "master", "--no-tags",
                 new["repo"], tmp])
            cwd = tmp
        prs, direct = pull_requests(f"{old['commit']}..{new['commit']}", cwd)
    except Error as e:
        lines.append(f"(The fork's changes since {pin_name(old)} could not be listed: {e})")
        return lines
    finally:
        if tmp:
            remove_tree(tmp)
    lines.append(f"Changes since {pin_name(old)}:")
    lines += [line.replace("### ", "#### ", 1) for line in pr_sections(prs, direct, slug)]
    return lines


def notes(to, since, checkout):
    to_sha = git_out(["rev-parse", "--verify", f"{to}^{{commit}}"])
    m = TAG_RE.match(to)
    version = m.group(1) + (m.group(2) or "") if m else None
    if version is None:
        file_version = try_git(["show", f"{to_sha}:VERSION"])
        version = f"{file_version or '0.0.0'} (not tagged: {to_sha[:9]})"
    span = f"{since}..{to_sha}" if since else to_sha
    prs, direct = pull_requests(span)
    lines = [f"DemonsSoulsRecomp {version}."]
    lines.append(f"Changes since {since}:" if since else "The first release: everything up to it.")
    lines += pr_sections(prs, direct, "")
    lines += fork_section(read_pin(since) if since else None, read_pin(to_sha), checkout)
    lines += ["", "### The download", "",
              "The source of the port and of the ps3recomp it pins, nothing made from the game. Building it needs your "
              "own copy of Demon's Souls; the README says how. SHA256SUMS has the zip's SHA-256."]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--to", default="HEAD", help="the release (a tag) or any revision (default HEAD)")
    ap.add_argument("--from", dest="since", help="the previous release (default: found from the tags)")
    ap.add_argument("--ps3recomp", help="a checkout of the fork with both pinned commits (default: fetch)")
    ap.add_argument("--out", help="write the notes here (default: print them)")
    args = ap.parse_args(argv)
    try:
        since = args.since or previous_release(args.to)
        text = notes(args.to, since, args.ps3recomp)
    except Error as e:
        print(f"changelog: {e}", file=sys.stderr)
        return 2
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"changelog: {args.out} ({text.count(chr(10))} lines)")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
