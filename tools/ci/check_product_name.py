#!/usr/bin/env python3
"""Fail when the name people see is written anywhere but PRODUCT_NAME (docs/130).

PRODUCT_NAME, at the root, holds the project's shown name. The build, the release title and the notes
read it from there, so a rename is that one line. This fails, with each place named:
  - when a tracked file other than the docs has the name written out (it should read PRODUCT_NAME instead);
  - when one still has a former shown name (FORMER below; a rename adds the old name to it);
  - when the README's title or PRIVACY.md's server name is not the name. Those two are written by hand, so this is
    what catches a rename that missed them.
The technical name DemonsSoulsRecomp (the downloads, the folders, the signature) is a different thing and is not
looked for, and neither is the repository's address (JonathanDC64/<repository>, in links and the update feed),
which has its own name.

    python tools/ci/check_product_name.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORMER = ["Demon's Souls Recomp", "Demon’s Souls Recomp"]
# Prose about the project, kept by hand: docs/ (history), Trail's notes, and the Markdown files (the README and
# PRIVACY.md are checked on their own below). And this file, which lists the former names.
SKIP = (re.compile(r"^docs/"), re.compile(r"^\.trail/"), re.compile(r"\.md$"), re.compile(r"^PRODUCT_NAME$"),
        re.compile(r"^tools/ci/check_product_name\.py$"))
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 '.:-]*$")   # (as port/cmake/DesVersion.cmake takes it)
REPOSITORY_RE = re.compile(r"JonathanDC64/[A-Za-z0-9._-]+")   # (a repository's address, left out of the lines)


def main():
    problems = []
    try:
        with open(os.path.join(ROOT, "PRODUCT_NAME"), encoding="utf-8") as f:
            name = f.readline().strip()
    except OSError:
        print("::error::no PRODUCT_NAME at the root", file=sys.stderr)
        return 1
    if not NAME_RE.match(name) or len(name) > 40:
        problems.append(("PRODUCT_NAME", f"{name!r} is not a name: letters, digits, spaces and ' . : -, at most 40"))
    files = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    for path in sorted(p.decode("utf-8", "replace") for p in files.split(b"\0") if p):
        if any(rx.search(path) for rx in SKIP):
            continue
        try:
            with open(os.path.join(ROOT, path), "rb") as f:
                data = f.read()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue   # (a binary file)
        text = data.decode("utf-8", "replace")
        for n, line in enumerate(text.splitlines(), 1):
            line = REPOSITORY_RE.sub("", line)
            if name and name in line:
                problems.append((f"{path}:{n}", f"the name {name!r} is written out: read PRODUCT_NAME instead"))
            for old in FORMER:
                if old in line:
                    problems.append((f"{path}:{n}", f"a former name, {old!r}"))
    readme = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
    title = next((l[2:].strip() for l in readme.splitlines() if l.startswith("# ")), "")
    if title != name:
        problems.append(("README.md", f"the title is {title!r}, PRODUCT_NAME says {name!r}"))
    privacy = open(os.path.join(ROOT, "PRIVACY.md"), encoding="utf-8").read()
    if f'"{name}"' not in " ".join(privacy.split()):
        problems.append(("PRIVACY.md", f'the server is not called "{name}" (Settings > Network > Server)'))
    for where, what in problems:
        print(f"::error::{where}: {what}" if os.environ.get("GITHUB_ACTIONS") == "true" else f"{where}: {what}",
              file=sys.stderr)
    if not problems:
        print(f"the name {name!r} is only in PRODUCT_NAME, the README's title and PRIVACY.md")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
