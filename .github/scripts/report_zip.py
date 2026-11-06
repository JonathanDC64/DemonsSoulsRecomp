"""The report zip reminder (.github/workflows/report-zip.yml).

A problem report is an issue from the report form (.github/ISSUE_TEMPLATE/report.yml, its "Report file (.zip)"
heading) or from an older version's link, whose text names the report's file (setup-20261105-235012.zip,
issue-..., crash-...). When a report has no .zip attached, in its text or in any comment, it gets the label
"needs report zip" and, once, a comment saying where the file is. When a .zip is attached later, the label goes.

Reads the event from GITHUB_EVENT_PATH and acts with the gh command line (GH_TOKEN, REPO). REPORT_ZIP_DRY=1 prints
what it would do instead, and reads the comments from REPORT_ZIP_COMMENTS (a JSON file) for a local test.
"""
import json
import os
import re
import subprocess
import sys

LABEL = "needs report zip"
MARK = "<!-- report-zip-reminder -->"
REPORT = re.compile(r"^### Report file \(\.zip\)|\b[a-z]+-\d{8}-\d{6}\.zip\b", re.M)
# An attached file: https://github.com/user-attachments/files/<id>/<name>.zip (and the older
# https://github.com/<owner>/<repo>/files/<id>/<name>.zip).
ZIP = re.compile(r"/files/\d+/[^\s)\"'<>]+\.zip\b", re.I)
REMINDER = MARK + """
Thanks for the report! The report file (.zip) isn't attached yet. It has the logs we need to find the problem.

It's in the folder that opened when you reported, named like `issue-20261106-101500.zip`, `crash-...` or `setup-...`. \
If that folder is closed, it's the `reports` folder in `%APPDATA%\\DemonsSoulsRecomp\\DemonsSouls` on Windows \
(`~/.local/share/DemonsSoulsRecomp/DemonsSouls` on Linux). Drag the file into a comment here, please.
"""

DRY = os.environ.get("REPORT_ZIP_DRY") == "1"


def gh(*args, stdin=None) -> str:
    if DRY:
        print("gh " + " ".join(args) + (f"  <<< {stdin[:60]!r}..." if stdin else ""))
        return ""
    r = subprocess.run(["gh", *args], input=stdin, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit(f"gh {args[0]} {args[1] if len(args) > 1 else ''} failed")
    return r.stdout


def comments(repo: str, number: int) -> list:
    if DRY:
        path = os.environ.get("REPORT_ZIP_COMMENTS")
        return json.load(open(path, encoding="utf-8")) if path else []
    out = gh("api", "--paginate", f"repos/{repo}/issues/{number}/comments", "--jq", ".[] | {body: .body}")
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def decide(body: str, labels: list, notes: list, opened: bool) -> list:
    """What to do: [] or some of "label", "remind", "unlabel". notes: the comments' texts. Only a report that has
    just been opened is labelled and reminded; later events only take the label off once a .zip is there (so a
    label taken off by hand stays off)."""
    if not REPORT.search(body or ""):
        return []
    if ZIP.search(body or "") or any(ZIP.search(n or "") for n in notes):
        return ["unlabel"] if LABEL in labels else []
    if not opened:
        return []
    todo = [] if LABEL in labels else ["label"]
    if not any(MARK in (n or "") for n in notes):
        todo.append("remind")
    return todo


def main() -> int:
    event = json.load(open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8"))
    issue = event.get("issue") or {}
    if not issue or "pull_request" in issue:
        return 0
    repo, number = os.environ["REPO"], int(issue["number"])
    labels = [x["name"] for x in issue.get("labels", [])]
    opened = os.environ.get("GITHUB_EVENT_NAME") == "issues" and event.get("action") == "opened"
    todo = decide(issue.get("body") or "", labels, [c.get("body") for c in comments(repo, number)], opened)
    print(f"#{number}: {', '.join(todo) or 'nothing to do'}")
    if "label" in todo:
        gh("label", "create", LABEL, "--repo", repo, "--color", "FBCA04", "--force",
           "--description", "A problem report without its .zip")
        gh("issue", "edit", str(number), "--repo", repo, "--add-label", LABEL)
    if "remind" in todo:
        gh("issue", "comment", str(number), "--repo", repo, "--body-file", "-", stdin=REMINDER)
    if "unlabel" in todo:
        gh("issue", "edit", str(number), "--repo", repo, "--remove-label", LABEL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
