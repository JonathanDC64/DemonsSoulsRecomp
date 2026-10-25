"""Print ps3recomp.pin's fork and commit as a step's outputs (docs/54), for the jobs that check out the pinned
ps3recomp beside port/:

  python .github/scripts/read_pin.py >> "$GITHUB_OUTPUT"

prints repo=<owner>/<name> and commit=<40 hex digits>, or fails with an ::error:: line. pin.yml checks the commit
is on the fork's master; the port's CMake (DES_REQUIRE_PIN=ON) checks the checkout is that commit.
"""
import re
import sys
from pathlib import Path

PIN = Path(__file__).resolve().parents[2] / "ps3recomp.pin"


def main():
    keys = {}
    for line in PIN.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*(repo|tag|commit)\s*=\s*(\S*)", line)
        if m:
            keys[m.group(1)] = m.group(2)
    repo = re.fullmatch(r"https://github\.com/([\w.-]+/[\w.-]+?)(?:\.git)?/?", keys.get("repo", ""))
    commit = keys.get("commit", "")
    if not repo or not re.fullmatch(r"[0-9a-f]{40}", commit):
        print("::error file=ps3recomp.pin::needs repo=https://github.com/<owner>/<name> and a 40-digit commit=",
              file=sys.stderr)
        return 1
    print(f"repo={repo.group(1)}\ncommit={commit}")
    print(f"ps3recomp: {repo.group(1)} at {commit}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
