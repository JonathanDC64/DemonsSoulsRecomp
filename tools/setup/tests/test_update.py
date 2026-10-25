"""Tests for the update commands (update.py, docs/105), on made-up releases and installs only.

  cd tools/setup && python -m unittest discover -s tests
"""
import hashlib
import http.server
import io
import json
import os
import sys
import tarfile
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "release"))
from des_setup import fetch, journal, protocol, update  # noqa: E402
import package  # noqa: E402

EXE = update.EXE
PLAYER = "windows" if os.name == "nt" else "linux"   # this system's player's download
# The Linux download is a tar.xz, which fetch.unpack takes with the Linux setup work (docs/87). Until that is in,
# the tests that unpack one run on Windows only.
unpacks = unittest.skipUnless(PLAYER == "windows" or hasattr(fetch, "TARS"), "fetch.unpack takes no tar.xz yet")


def release(tag, assets=(PLAYER, "sums"), draft=False, pre=False, body="", base="http://127.0.0.1:1"):
    v = tag.lstrip("v")
    names = {"windows": f"DemonsSoulsRecomp-{v}-windows.zip", "sums": "SHA256SUMS",
             "linux": f"DemonsSoulsRecomp-{v}-linux.tar.xz"}
    return {"tag_name": tag, "draft": draft, "prerelease": pre, "body": body,
            "html_url": f"https://github.com/{update.REPO}/releases/tag/{tag}", "published_at": "2026-10-25T10:00:00Z",
            "assets": [{"name": names[a], "size": 0, "browser_download_url": f"{base}/{names[a]}"} for a in assets]}


def lines(out: io.StringIO) -> list:
    return out.getvalue().splitlines()


class FeedTest(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(update.version_of("v1.2.3"), (1, 2, 3))
        self.assertEqual(update.version_of("0.10.0"), (0, 10, 0))
        for bad in ("0.1.0-dev.5+g1234", "0.1.0+3.gabc", "llvm-22.1.8-1", "v0.2.0-rc.1", "", None):
            self.assertIsNone(update.version_of(bad), bad)

    def test_newest_stable_only(self):
        feed = [release("v0.3.0", pre=True), release("v0.4.0", draft=True), release("llvm-22.1.8-1"),
                release("v0.2.9", assets=(PLAYER,)),             # no SHA256SUMS: not offered
                release("v0.2.10"), release("v0.2.2"), release("v0.1.0")]
        self.assertEqual(update.newer(feed, "0.1.0")[0], "0.2.10")   # (numbers, not text: 10 > 9)
        self.assertIsNone(update.newer(feed, "0.2.10"))
        self.assertIsNone(update.newer(feed, "0.3.0"))
        self.assertIsNone(update.newer(feed, "0.1.0-dev.4+g123456789"))  # a developer's build compares nothing

    def test_check_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            feed = Path(tmp) / "feed.json"
            feed.write_text(json.dumps([release("v0.2.0", body="## What's new\r\n\r\n- Faster\tloads\r\n  - more")]))
            out = io.StringIO()
            with mock.patch.dict(os.environ, {"DES_UPDATE_FEED": str(feed)}):
                self.assertEqual(update.check(Path(tmp), "0.1.0", protocol.Emitter(out)), 0)
            got = lines(out)
            self.assertEqual(got[0], "@setup 2")
            self.assertTrue(got[1].startswith("@release 0.2.0 17"), got[1])
            self.assertTrue(got[1].endswith("/releases/tag/v0.2.0"), got[1])
            self.assertEqual(got[2:6], ["@note ## What's new", "@note", "@note - Faster    loads", "@note   - more"])
            self.assertEqual(got[-1], "@done ok")

            out = io.StringIO()
            with mock.patch.dict(os.environ, {"DES_UPDATE_FEED": str(feed)}):
                update.check(Path(tmp), "0.2.0", protocol.Emitter(out))
            self.assertFalse([x for x in lines(out) if x.startswith("@release")])
            self.assertEqual(lines(out)[-1], "@done ok")

            out = io.StringIO()
            with mock.patch.dict(os.environ, {"DES_UPDATE_FEED": str(Path(tmp) / "missing.json")}):
                self.assertEqual(update.check(Path(tmp), "0.1.0", protocol.Emitter(out)), 1)
            self.assertTrue(lines(out)[-2].startswith("@error download - "))
            self.assertEqual(lines(out)[-1], "@done failed")


class _Server(http.server.BaseHTTPRequestHandler):
    files = {}

    def do_GET(self):
        body = self.files.get(self.path.lstrip("/"))
        if body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def make_package(version, files) -> bytes:
    """This system's player's download as package.py writes it, DemonsSoulsRecomp-<version>/ with release-files.txt:
    a zip on Windows, a tar.xz on Linux (the program executable)."""
    top = f"DemonsSoulsRecomp-{version}/"
    entries = [(top + k, 0o755 if k == EXE else 0o644, v) for k, v in files.items()]
    entries.append(package.manifest(entries, top))
    buf = io.BytesIO()
    if PLAYER == "windows":
        with zipfile.ZipFile(buf, "w") as z:
            for name, _mode, data in entries:
                z.writestr(name, data)
    else:
        with tarfile.open(fileobj=buf, mode="w:xz") as t:
            for name, mode, data in entries:
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), mode
                t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def unpack_package(data, dest):
    if PLAYER == "windows":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            z.extractall(dest)
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as t:
            t.extractall(dest, filter="data")


def program(version, extra=None) -> dict:
    files = {EXE: f"exe {version}".encode(), "release-info.txt": f"version={version}\n".encode(),
             "tools/setup/run_setup.py": f"# {version}\n".encode(), "port/a.cpp": f"a {version}".encode(),
             "README.md": b"readme"}
    files.update(extra or {})
    return files


class ApplyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.HTTPServer(("127.0.0.1", 0), _Server)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.install, self.data = root / "game", root / "data"
        # The install of 0.1.0 (from its download, with release-files.txt), and the player's files beside it.
        unpack_package(make_package("0.1.0", program("0.1.0", {"docs/old-only.md": b"gone in 0.2.0"})), root / "x")
        os.replace(root / "x" / "DemonsSoulsRecomp-0.1.0", self.install)
        for rel, body in {"gamedata/dev_hdd0/home/00000001/savedata/S/SAVE": b"save",
                          "shader_cache/abc.bin": b"shader", "portable.txt": b"", "data/settings.ini": b"x=1",
                          "notes-of-mine.txt": b"mine"}.items():
            (self.install / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.install / rel).write_bytes(body)
        # The game build of 0.1.0.
        (self.data / "game").mkdir(parents=True)
        (self.data / "game" / "des_game.dll").write_bytes(b"module 0.1.0")
        (self.data / "setup").mkdir()
        (self.data / "setup" / "ready.txt").write_text("format=1\napp=0.1.0\n")
        jr = journal.Journal(self.data / "setup" / "journal.json")
        jr.run("link", "fp-0.1.0", lambda: None)
        self.feed = root / "feed.json"
        self.serve("0.2.0", program("0.2.0", {"docs/new.md": b"new"}))

    def tearDown(self):
        self.tmp.cleanup()

    def serve(self, version, files, sums_for=None):
        z = make_package(version, files)
        name = update.player_asset(version)
        digest = hashlib.sha256(sums_for if sums_for is not None else z).hexdigest()
        _Server.files = {name: z, "SHA256SUMS": f"{digest}  {name}\n".encode()}
        rel = release(f"v{version}", base=self.base)
        rel["assets"][0]["size"] = len(z)
        self.feed.write_text(json.dumps([rel]))

    def run_cmd(self, fn, *args):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"DES_UPDATE_FEED": str(self.feed)}):
            rc = fn(self.install, self.data, *args, protocol.Emitter(out))
        return rc, lines(out)

    def player_files_kept(self):
        self.assertEqual((self.install / "gamedata/dev_hdd0/home/00000001/savedata/S/SAVE").read_bytes(), b"save")
        self.assertEqual((self.install / "shader_cache/abc.bin").read_bytes(), b"shader")
        self.assertEqual((self.install / "data/settings.ini").read_bytes(), b"x=1")
        self.assertEqual((self.install / "notes-of-mine.txt").read_bytes(), b"mine")
        self.assertTrue((self.install / "portable.txt").is_file())

    @unpacks
    def test_apply_then_back(self):
        rc, out = self.run_cmd(update.apply, "0.1.0", "0.2.0")
        self.assertEqual(rc, 0, out)
        self.assertEqual(out[1], "@plan download install")
        self.assertIn("@step install done", out)
        self.assertEqual(out[-1], "@done ok")
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.2.0")
        self.assertTrue((self.install / "docs/new.md").is_file())
        self.assertFalse((self.install / "docs/old-only.md").exists())
        self.player_files_kept()
        kept = self.install / update.PREVIOUS / "0.1.0"
        self.assertEqual((kept / EXE).read_bytes(), b"exe 0.1.0")
        self.assertTrue((kept / "docs/old-only.md").is_file())
        self.assertFalse((kept / "gamedata").exists())
        self.assertEqual(update.kept_version(self.install), "0.1.0")
        self.assertEqual((self.data / "previous/0.1.0/game/des_game.dll").read_bytes(), b"module 0.1.0")
        self.assertIn("app=0.1.0", (self.data / "previous/0.1.0/ready.txt").read_text())
        self.assertTrue((self.data / "game/des_game.dll").is_file())       # (copied: the rebuild starts from it)
        self.assertFalse((self.install / update.UPDATE / update.SWAP).exists())
        self.assertFalse((self.install / update.UPDATE / "0.2.0").exists())
        self.assertFalse(list((self.data / "downloads").glob("DemonsSoulsRecomp-*")))

        # The rebuild of 0.2.0, then back to 0.1.0.
        (self.data / "game" / "des_game.dll").write_bytes(b"module 0.2.0")
        (self.data / "setup" / "ready.txt").write_text("format=1\napp=0.2.0\n")
        journal.Journal(self.data / "setup" / "journal.json").forget("link")
        journal.Journal(self.data / "setup" / "journal.json").run("link", "fp-0.2.0", lambda: None)
        rc, out = self.run_cmd(update.back, "0.2.0")
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.1.0")
        self.assertTrue((self.install / "docs/old-only.md").is_file())
        self.assertFalse((self.install / "docs/new.md").exists())
        self.player_files_kept()
        self.assertEqual(update.kept_version(self.install), "0.2.0")
        self.assertEqual((self.data / "game/des_game.dll").read_bytes(), b"module 0.1.0")
        self.assertIn("app=0.1.0", (self.data / "setup/ready.txt").read_text())
        self.assertEqual((self.data / "previous/0.2.0/game/des_game.dll").read_bytes(), b"module 0.2.0")
        self.assertEqual(sorted(p.name for p in (self.data / "previous").iterdir()), ["0.2.0"])
        self.assertEqual(sorted(p.name for p in (self.install / update.PREVIOUS).iterdir()), ["0.2.0"])
        # The journal's link made 0.2.0's module, which is aside now: a build of 0.2.0 again links again.
        self.assertFalse(journal.Journal(self.data / "setup" / "journal.json").is_done("link", "fp-0.2.0"))

    def test_bad_checksum_changes_nothing(self):
        self.serve("0.2.0", program("0.2.0"), sums_for=b"something else")
        rc, out = self.run_cmd(update.apply, "0.1.0", "0.2.0")
        self.assertEqual(rc, 1)
        self.assertTrue(out[-2].startswith("@error download download "), out)
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.1.0")
        self.assertIsNone(update.kept_version(self.install))
        self.assertFalse((self.data / "previous").exists() and list((self.data / "previous").iterdir()))

    @unpacks
    def test_file_in_use_undoes_the_swap(self):
        real = update._rename

        def rename(src, dst, tries=40):
            if Path(src).name == "port" and update.PREVIOUS in str(dst):
                raise PermissionError(32, "in use", str(src))
            real(src, dst, 1)

        with mock.patch.object(update, "_rename", rename):
            rc, out = self.run_cmd(update.apply, "0.1.0", "0.2.0")
        self.assertEqual(rc, 1)
        self.assertTrue(out[-2].startswith("@error busy install "), out)
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.1.0")
        self.assertTrue((self.install / "docs/old-only.md").is_file())
        self.assertTrue((self.install / "port/a.cpp").is_file())
        self.player_files_kept()
        self.assertFalse((self.install / update.PREVIOUS).exists())
        self.assertFalse((self.install / update.UPDATE / update.SWAP).exists())
        self.assertFalse((self.data / "previous" / "0.1.0.tmp").exists())

    def test_cut_short_swap_is_undone(self):
        # A swap cut short after two renames: the next apply undoes them first, then updates.
        sw = update.Swap(self.install / update.UPDATE / update.SWAP)
        sw.move(self.install / "README.md", self.install / update.PREVIOUS / "0.1.0" / "README.md")
        sw.move(self.install / "docs", self.install / update.PREVIOUS / "0.1.0" / "docs")
        self.assertTrue(update.recover(self.install))
        self.assertTrue((self.install / "README.md").is_file())
        self.assertTrue((self.install / "docs/old-only.md").is_file())
        self.assertFalse((self.install / update.PREVIOUS).exists())
        self.assertFalse(update.recover(self.install))

    @unpacks
    def test_install_without_a_list(self):
        # An install from before release-files.txt: the names the new release has are swapped, the rest stay.
        (self.install / "release-files.txt").unlink()
        rc, out = self.run_cmd(update.apply, "0.1.0", "0.2.0")
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.2.0")
        self.assertFalse((self.install / "docs/old-only.md").exists())   # (docs/ is in the new release)
        self.player_files_kept()

    def test_developer_tree_refused(self):
        (self.install / ".git").mkdir()
        rc, out = self.run_cmd(update.apply, "0.1.0", "0.2.0")
        self.assertEqual(rc, 1)
        self.assertEqual((self.install / EXE).read_bytes(), b"exe 0.1.0")

    def test_back_with_nothing_kept(self):
        rc, out = self.run_cmd(update.back, "0.1.0")
        self.assertEqual(rc, 1)
        self.assertIn("no earlier version", out[-2])


class ManifestTest(unittest.TestCase):
    def test_lists_itself(self):
        top = "DemonsSoulsRecomp-1.0.0/"
        name, _mode, text = package.manifest([(top + "des_host.exe", 0o755, b""), (top + "port/a.cpp", 0o644, b"")],
                                             top)
        self.assertEqual(name, top + "release-files.txt")
        listed = [x for x in text.decode().splitlines() if not x.startswith("#")]
        self.assertEqual(listed, ["des_host.exe", "port/a.cpp", "release-files.txt"])


if __name__ == "__main__":
    unittest.main()
