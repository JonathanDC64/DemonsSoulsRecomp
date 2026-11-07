#!/usr/bin/env python3
"""Posts a published game release and its notes to Discord (docs/54 section 8.7).

  discord_post.py --repo OWNER/NAME --tag vX.Y.Z [--dry-run] [--release-json FILE]

.github/workflows/release-discord.yml runs it when a release is published. It reads the release from GitHub
(GITHUB_TOKEN, if set, for the API), turns its notes into Discord's markdown, and posts them through the webhook in
DISCORD_RELEASE_WEBHOOK (a repository secret) as one or more messages of embeds:

- the first embed has the release's name as its title, linked to the release page;
- the notes follow, with headings in bold, list items as bullets and wrapped lines joined (Discord keeps every line
  break), issue numbers linked, and each <details> block (the full change list) left on GitHub behind a link;
- the last embed lists the downloads.

Only game releases (vX.Y.Z) are posted, never the compiler downloads or the third-party sources. Nobody is pinged:
the messages allow no mentions. --dry-run prints the messages instead of posting; --release-json reads the release
from a file (gh api repos/OWNER/NAME/releases/tags/TAG), which also takes a draft.

Standard library only. The webhook's address is never printed.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"
GAME_TAG = re.compile(r"^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
WEBHOOK = re.compile(r"^https://(?:(?:ptb|canary)\.)?discord(?:app)?\.com/api/webhooks/\d+/[\w-]+$")
UA = "discord-release-post/1"

# Discord's limits (embeds): description 4096, title 256, field value 1024, 10 embeds and 6000 characters in all
# per message. The text is cut a little under them.
DESC_MAX = 4000
TITLE_MAX = 256
FIELD_MAX = 1024
MSG_CHARS = 6000
MSG_EMBEDS = 10
MAX_MESSAGES = 5          # notes longer than this many messages end with a link to the rest
COLOR = 0xB08D57


class Error(Exception):
    pass


# --- notes -> Discord markdown ----------------------------------------------------------------------------------

_DETAILS = re.compile(r"<details>\s*(?:<summary>(.*?)</summary>)?(.*?)</details>", re.S | re.I)
_COMMENT = re.compile(r"<!--.*?-->", re.S)
_TAG = re.compile(r"</?(?:br|p|div|span|sub|sup|kbd|b|i|em|strong)\b[^>]*>", re.I)
_HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
_ITEM = re.compile(r"^(\s*)(?:[-*+]|\d+[.)])\s+(.*)$")
_TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
_IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)[^)]*\)")
_KEEP = re.compile(r"(`[^`\n]*`|\[[^\]\n]*\]\([^)\n]*\)|<https?://[^>\s]+>|https?://\S+)")
_ISSUE = re.compile(r"(?<![\w/#&\[])#(\d+)\b")


def _link_issues(text: str, repo: str) -> str:
    """#123 -> a link to the issue (GitHub sends an issue link on to a pull request), outside code, links and URLs."""
    parts = _KEEP.split(text)
    for i in range(0, len(parts), 2):
        parts[i] = _ISSUE.sub(lambda m: f"[#{m.group(1)}](https://github.com/{repo}/issues/{m.group(1)})", parts[i])
    return "".join(parts)


def _block_start(line: str) -> bool:
    s = line.lstrip()
    return (not s or bool(_HEADING.match(line)) or bool(_ITEM.match(line)) or s.startswith(("|", ">", "```", "<"))
            or bool(_TABLE_SEP.match(line)))


def to_discord(body: str, repo: str, url: str, title: str = "") -> str:
    """The release notes as Discord shows them in an embed. The first heading is left out when it is the title."""
    text = body.replace("\r\n", "\n").replace("\r", "\n")
    text = _COMMENT.sub("", text)

    def details(m):
        summary = re.sub(r"<[^>]+>", "", m.group(1) or "").strip() or "More"
        return f"\n\n**{summary}:** [on GitHub]({url})\n\n"
    text = _DETAILS.sub(details, text)
    text = _IMAGE.sub(lambda m: f"[{m.group(1) or 'image'}]({m.group(2)})", text)
    text = _TAG.sub("", text)

    out = []           # finished lines
    fence = False      # inside a ``` block: lines kept as they are
    joinable = False   # the last line is a paragraph's or a list item's, which a wrapped line carries on
    first_heading = True
    for raw in text.split("\n"):
        line = raw.rstrip()
        if line.lstrip().startswith("```"):
            fence = not fence
            out.append(line)
            joinable = False
            continue
        if fence:
            out.append(raw)
            continue
        h = _HEADING.match(line)
        if h:
            words = h.group(2).replace("**", "").strip()
            joinable = False
            if first_heading and h.group(1) == "#" and (not title or words == title.strip()):
                first_heading = False
                continue
            first_heading = False
            if out and out[-1]:
                out.append("")
            out.append(f"**{words}**")
            continue
        if line.strip():
            first_heading = False
        if _TABLE_SEP.match(line) and "-" in line:
            joinable = False
            continue
        if line.lstrip().startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            out.append(" · ".join(c for c in cells if c))
            joinable = False
            continue
        it = _ITEM.match(line)
        if it:
            depth = len(it.group(1).expandtabs(4)) // 2
            bullet = "•" if depth == 0 else "◦"
            out.append(" " * depth + bullet + " " + it.group(2).strip())
            joinable = True
        elif not line.strip():
            out.append("")
            joinable = False
        elif joinable and not _block_start(line):
            out[-1] = out[-1] + " " + line.strip()     # a wrapped line: Discord would break it, so join it on
        else:
            out.append(line if line.lstrip().startswith(">") else line.strip())
            joinable = not line.lstrip().startswith((">", "<"))
        if raw.endswith(("  ", "\\")):                 # markdown's own line break stays one
            joinable = False
    text = "\n".join(out)
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return _link_issues(text, repo)


# --- splitting and messages -------------------------------------------------------------------------------------

def split_text(text: str, limit: int = DESC_MAX) -> list:
    """Pieces of at most limit characters: whole paragraphs where they fit, then whole lines, then cut lines."""
    pieces, cur = [], ""

    def put(block: str, sep: str):
        nonlocal cur
        if not cur:
            cur = block
        elif len(cur) + len(sep) + len(block) <= limit:
            cur = cur + sep + block
        else:
            pieces.append(cur)
            cur = block

    for para in text.split("\n\n"):
        if len(para) <= limit:
            put(para, "\n\n")
            continue
        first = True
        for line in para.split("\n"):
            while len(line) > limit:
                cut = line.rfind(" ", 0, limit)
                cut = cut if cut > limit // 2 else limit
                put(line[:cut], "\n\n" if first else "\n")
                first = False
                line = line[cut:].lstrip()
            put(line, "\n\n" if first else "\n")
            first = False
    if cur:
        pieces.append(cur)
    return pieces


def _size(n: int) -> str:
    return f"{n / 1e6:.1f} MB" if n >= 1e6 else f"{max(1, round(n / 1e3))} kB"


# What each kind of file is, by the end of its name (tools/release/package.py's names), in the order shown.
KINDS = (("-windows.zip", "Windows"), ("-linux.tar.xz", "Linux"), ("-source.zip", "Source"),
         ("-symbols.zip", "Debug symbols"))


def downloads(assets: list) -> str:
    """The release's files, one link a line, the players' first (not the checksums or their signature), cut to a
    field's limit."""
    def kind(name):
        return next((i for i, (end, _) in enumerate(KINDS) if name.endswith(end)), len(KINDS))
    lines = []
    for a in sorted((a for a in assets if not a["name"].startswith("SHA256SUMS")), key=lambda a: kind(a["name"])):
        k = kind(a["name"])
        label = f"**{KINDS[k][1]}:** " if k < len(KINDS) else ""
        lines.append(f"{label}[{a['name']}]({a['browser_download_url']}) ({_size(a.get('size', 0))})")
    text = ""
    for ln in lines:
        if len(text) + len(ln) + 1 > FIELD_MAX:
            break
        text = f"{text}\n{ln}" if text else ln
    return text


def _embed_chars(e: dict) -> int:
    n = len(e.get("title", "")) + len(e.get("description", ""))
    n += sum(len(f["name"]) + len(f["value"]) for f in e.get("fields", []))
    return n + len(e.get("footer", {}).get("text", ""))


def messages(release: dict, repo: str) -> list:
    """The webhook messages for a release, in order."""
    url = release["html_url"]
    title = (release.get("name") or release["tag_name"]).strip()
    notes = to_discord(release.get("body") or "", repo, url, title)
    pieces = split_text(notes) if notes else []
    room = MAX_MESSAGES          # one embed of up to DESC_MAX a message, at worst
    if len(pieces) > room:
        pieces = pieces[:room - 1] + [split_text(pieces[room - 1], DESC_MAX - 60)[0] +
                                      f"\n\n**The rest of the notes:** [on GitHub]({url})"]
    embeds = []
    for i, p in enumerate(pieces or [""]):
        e = {"description": p, "color": COLOR}
        if i == 0:
            e.update(title=title[:TITLE_MAX], url=url)
            if release.get("published_at"):
                e["timestamp"] = release["published_at"]
        embeds.append(e)
    files = downloads(release.get("assets") or [])
    if files:
        embeds[-1]["fields"] = [{"name": "Downloads", "value": files}]
        if _embed_chars(embeds[-1]) > MSG_CHARS:
            embeds.append({"color": COLOR, "fields": embeds[-1].pop("fields")})
    out, cur, chars = [], [], 0
    for e in embeds:
        n = _embed_chars(e)
        if cur and (len(cur) == MSG_EMBEDS or chars + n > MSG_CHARS):
            out.append(cur)
            cur, chars = [], 0
        cur.append(e)
        chars += n
    if cur:
        out.append(cur)
    return [{"embeds": m, "allowed_mentions": {"parse": []}} for m in out]


# --- GitHub and Discord ---------------------------------------------------------------------------------------------

def fetch_release(repo: str, tag: str, token: str = "") -> dict:
    req = urllib.request.Request(f"{API}/repos/{repo}/releases/tags/{tag}",
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": UA})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise Error(f"no published release {tag} in {repo} (a draft is not seen here)") from None
        raise Error(f"GitHub said {e.code} for the release {tag}") from None


def webhook_url(value: str) -> str:
    """The webhook as Discord takes a message: a GitHub- or Slack-style address loses its /github or /slack."""
    url = (value or "").strip()
    url = re.sub(r"/(?:github|slack)/?$", "", url).split("?")[0].rstrip("/")
    if not WEBHOOK.match(url):
        raise Error("DISCORD_RELEASE_WEBHOOK is not a Discord webhook address (https://discord.com/api/webhooks/...)")
    return url


def post(url: str, message: dict, opener=urllib.request.urlopen, sleep=time.sleep, tries: int = 6) -> None:
    """One message, waiting out Discord's rate limit (429) as it asks. Errors never show the address."""
    data = json.dumps(message).encode("utf-8")
    for _ in range(tries):
        req = urllib.request.Request(url + "?wait=true", data=data, method="POST",
                                     headers={"Content-Type": "application/json", "User-Agent": UA})
        try:
            with opener(req, timeout=30) as r:
                r.read()
            return
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")[:500]
            if e.code == 429:
                try:
                    wait = float(json.loads(text).get("retry_after", 1.0))
                except ValueError:
                    wait = 1.0
                sleep(min(max(wait, 0.5), 30.0))
                continue
            raise Error(f"Discord said {e.code}: {text}") from None
        except urllib.error.URLError as e:
            raise Error(f"Discord could not be reached: {e.reason}") from None
    raise Error("Discord kept asking to wait (429); gave up")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dry-run", action="store_true", help="print the messages, post nothing")
    ap.add_argument("--release-json", help="read the release from this file instead of GitHub")
    a = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # the bullets, on a Windows console too
    try:
        if not GAME_TAG.match(a.tag):
            print(f"discord_post: {a.tag} is not a game release (vX.Y.Z); nothing to post.")
            return 0
        if a.release_json:
            with open(a.release_json, encoding="utf-8") as f:
                rel = json.load(f)
        else:
            rel = fetch_release(a.repo, a.tag, os.environ.get("GITHUB_TOKEN", ""))
        msgs = messages(rel, a.repo)
        if a.dry_run:
            for i, m in enumerate(msgs, 1):
                n = sum(_embed_chars(e) for e in m["embeds"])
                print(f"--- message {i} of {len(msgs)}: {len(m['embeds'])} embed(s), {n} characters")
                print(json.dumps(m, indent=2, ensure_ascii=False))
            return 0
        hook = os.environ.get("DISCORD_RELEASE_WEBHOOK", "")
        if not hook:
            print("::warning::DISCORD_RELEASE_WEBHOOK is not set: nothing posted. Add it as a repository secret "
                  "(docs/54 section 8.7).")
            return 0
        url = webhook_url(hook)
        for i, m in enumerate(msgs, 1):
            post(url, m)
            print(f"discord_post: message {i} of {len(msgs)} posted")
            if i < len(msgs):
                time.sleep(1.0)        # in order, and well inside the webhook's rate
        return 0
    except Error as e:
        print(f"::error::discord_post: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
