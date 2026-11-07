"""Tests for tools/release/discord_post.py, on made-up notes (nothing is posted).

  python -m unittest discover -s tools/release/tests
"""
import io
import json
import sys
import unittest
import urllib.error
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import discord_post as d  # noqa: E402

REPO = "Owner/Game"
URL = "https://github.com/Owner/Game/releases/tag/v1.2.3"
NOTES = """# Game 1.2.3 (alpha)

An update. Your saves are kept: start the game and it offers
the update.

## What's new

- **Faster.** A frame takes less time
  than before (issue #12):
  - one part;
  - another part.
- **Fixed sound** (issues #3, #4 and #5). See [#99 in a link](https://example.com/#99) and `#7 in code`.

| Setting | Before | After |
|---|---|---|
| A | 1 | 2 |

![a picture](https://example.com/p.png)
<!-- a note for us -->

<details>
<summary>Every change in this release</summary>

### Fixes
- a long list nobody reads on Discord
</details>
"""


def release(body=NOTES, assets=None, name="Game 1.2.3 (alpha)"):
    if assets is None:
        assets = [{"name": n, "size": s, "browser_download_url": f"https://example.com/{n}"}
                  for n, s in (("Game-1.2.3-source.zip", 8_000_000), ("Game-1.2.3-linux.tar.xz", 11_500_000),
                               ("SHA256SUMS", 408), ("SHA256SUMS.sig", 326), ("Game-1.2.3-windows.zip", 9_700_000))]
    return {"tag_name": "v1.2.3", "name": name, "html_url": URL, "body": body, "assets": assets,
            "published_at": "2026-11-07T12:00:00Z"}


class Convert(unittest.TestCase):
    def setUp(self):
        self.t = d.to_discord(NOTES, REPO, URL, "Game 1.2.3 (alpha)")

    def test_title_heading_left_out(self):
        self.assertNotIn("Game 1.2.3", self.t)
        self.assertTrue(self.t.startswith("An update."))

    def test_wrapped_lines_joined(self):
        self.assertIn("start the game and it offers the update.", self.t)
        self.assertIn("A frame takes less time than before", self.t)

    def test_headings_bold_and_bullets(self):
        self.assertIn("**What's new**", self.t)
        self.assertIn("\n• **Faster.**", self.t)
        self.assertIn("\n ◦ one part;", self.t)

    def test_issues_linked_but_not_in_links_or_code(self):
        self.assertIn("(issue [#12](https://github.com/Owner/Game/issues/12))", self.t)
        self.assertIn("[#4](https://github.com/Owner/Game/issues/4)", self.t)
        self.assertIn("[#99 in a link](https://example.com/#99)", self.t)
        self.assertIn("`#7 in code`", self.t)

    def test_table_image_comment(self):
        self.assertIn("Setting · Before · After", self.t)
        self.assertIn("A · 1 · 2", self.t)
        self.assertNotIn("---", self.t)
        self.assertIn("[a picture](https://example.com/p.png)", self.t)
        self.assertNotIn("a note for us", self.t)

    def test_details_left_on_github(self):
        self.assertNotIn("nobody reads", self.t)
        self.assertNotIn("<", self.t.replace("<@", ""))
        self.assertTrue(self.t.endswith(f"**Every change in this release:** [on GitHub]({URL})"))

    def test_code_block_kept(self):
        t = d.to_discord("Run:\n\n```\nline one\n  line two\n```\nafter", REPO, URL)
        self.assertIn("```\nline one\n  line two\n```", t)


class Split(unittest.TestCase):
    def test_limits_and_nothing_lost(self):
        text = "\n\n".join(f"**Part {i}**\n\n" + "\n".join(f"• item {i}.{j} " + "x" * 90 for j in range(30))
                           for i in range(6))
        pieces = d.split_text(text, 4000)
        self.assertTrue(all(len(p) <= 4000 for p in pieces))
        self.assertGreater(len(pieces), 1)
        self.assertEqual("".join(text.split()), "".join("".join(pieces).split()))

    def test_one_long_line(self):
        pieces = d.split_text("word " * 3000, 1000)
        self.assertTrue(all(len(p) <= 1000 for p in pieces))


class Messages(unittest.TestCase):
    def check_limits(self, msgs):
        for m in msgs:
            self.assertLessEqual(len(m["embeds"]), d.MSG_EMBEDS)
            self.assertLessEqual(sum(d._embed_chars(e) for e in m["embeds"]), d.MSG_CHARS)
            self.assertEqual(m["allowed_mentions"], {"parse": []})
            for e in m["embeds"]:
                self.assertLessEqual(len(e.get("description", "")), 4096)
                self.assertLessEqual(len(e.get("title", "")), 256)
                for f in e.get("fields", []):
                    self.assertLessEqual(len(f["value"]), 1024)

    def test_short_release(self):
        msgs = d.messages(release(), REPO)
        self.assertEqual(len(msgs), 1)
        e = msgs[0]["embeds"][0]
        self.assertEqual((e["title"], e["url"], e["timestamp"]), ("Game 1.2.3 (alpha)", URL, "2026-11-07T12:00:00Z"))
        files = msgs[0]["embeds"][-1]["fields"][0]["value"].split("\n")
        self.assertEqual([f.split("[")[1].split("]")[0] for f in files],
                         ["Game-1.2.3-windows.zip", "Game-1.2.3-linux.tar.xz", "Game-1.2.3-source.zip"])
        self.assertTrue(files[0].startswith("**Windows:** "))
        self.check_limits(msgs)

    def test_long_release_capped(self):
        body = "\n\n".join(f"## Section {i}\n\n" + "\n".join(f"- point {i}.{j} " + "y" * 150 for j in range(20))
                           for i in range(20))
        msgs = d.messages(release(body), REPO)
        self.check_limits(msgs)
        self.assertLessEqual(sum(len(m["embeds"]) for m in msgs), d.MAX_MESSAGES + 1)
        last_text = [e["description"] for m in msgs for e in m["embeds"] if e.get("description")][-1]
        self.assertTrue(last_text.endswith(f"**The rest of the notes:** [on GitHub]({URL})"))

    def test_no_notes_no_files(self):
        msgs = d.messages(release(body="", assets=[]), REPO)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0]["embeds"][0]["title"], "Game 1.2.3 (alpha)")


class Webhook(unittest.TestCase):
    def test_address(self):
        base = "https://discord.com/api/webhooks/123456/abc_DEF-9"
        self.assertEqual(d.webhook_url(base + "/github"), base)
        self.assertEqual(d.webhook_url(" " + base + "/ "), base)
        for bad in ("", "https://example.com/api/webhooks/1/x", "http://discord.com/api/webhooks/1/x"):
            with self.assertRaises(d.Error):
                d.webhook_url(bad)

    def test_error_hides_the_address(self):
        try:
            d.webhook_url("https://example.com/secret-token")
        except d.Error as e:
            self.assertNotIn("secret-token", str(e))

    def test_post_waits_out_429(self):
        calls, slept = [], []

        class Ok(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def opener(req, timeout):
            calls.append(json.loads(req.data))
            if len(calls) == 1:
                raise urllib.error.HTTPError(req.full_url, 429, "Too Many", {}, io.BytesIO(b'{"retry_after": 0.7}'))
            return Ok(b"{}")
        d.post("https://discord.com/api/webhooks/1/x", {"embeds": []}, opener=opener, sleep=slept.append)
        self.assertEqual(len(calls), 2)
        self.assertEqual(slept, [0.7])

    def test_post_error(self):
        def opener(req, timeout):
            raise urllib.error.HTTPError(req.full_url, 400, "Bad", {}, io.BytesIO(b'{"message": "Invalid Form Body"}'))
        with self.assertRaises(d.Error) as c:
            d.post("https://discord.com/api/webhooks/1/token-part", {"embeds": []}, opener=opener, sleep=lambda s: None)
        self.assertIn("400", str(c.exception))
        self.assertNotIn("token-part", str(c.exception))


class Main(unittest.TestCase):
    def test_other_releases_not_posted(self):
        self.assertEqual(d.main(["--repo", REPO, "--tag", "third-party-sources", "--dry-run"]), 0)
        self.assertEqual(d.main(["--repo", REPO, "--tag", "llvm-22.1.8-1", "--dry-run"]), 0)

    def test_game_tags(self):
        for t in ("v0.1.1", "v1.20.3", "v2.0.0-rc.1"):
            self.assertTrue(d.GAME_TAG.match(t), t)
        for t in ("v1.2", "vkd3d-2.1", "des-0.14.0-7"):
            self.assertFalse(d.GAME_TAG.match(t), t)


if __name__ == "__main__":
    unittest.main()
