"""Tests for the setup core, on made-up files only (no game data).

  cd tools/setup && python -m unittest discover -s tests
"""
import hashlib
import http.server
import io
import json
import os
import re
import sys
import tarfile
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from des_setup import fetch, gamefiles, journal, sfo  # noqa: E402
from des_setup.__main__ import lift_blockers, stage  # noqa: E402


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


EBOOT_BIN = b"SCE\0" + b"x" * 60          # encrypted, as on the disc
EBOOT_ELF = b"\x7fELF" + b"y" * 60        # what RPCS3 writes beside it
SDAT = b"NPD\0" + b"z" * 28
SDAT_DEC = b"DCX\0" + b"w" * 20
LIBSRE = b"\x7fELFsre"                   # the firmware's file, decrypted


def make_table(sfo_bytes):
    return {
        "title_id": "BLUS30443", "app_ver": "01.00", "version": "01.00",
        "other_titles": {"BLES00932": "the European disc"},
        "originals": [
            {"path": "PS3_GAME/PARAM.SFO", "size": len(sfo_bytes), "sha256": sha(sfo_bytes)},
            {"path": "PS3_GAME/USRDIR/EBOOT.BIN", "size": len(EBOOT_BIN), "sha256": sha(EBOOT_BIN)},
            {"path": "PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat", "size": len(SDAT), "sha256": sha(SDAT)},
        ],
        "decrypted": [
            {"path": "PS3_GAME/USRDIR/EBOOT.elf", "of": "PS3_GAME/USRDIR/EBOOT.BIN", "size": len(EBOOT_ELF),
             "sha256": sha(EBOOT_ELF), "magic": "7f454c46"},
            {"path": "PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat.unedat",
             "of": "PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat", "size": None, "sha256": None, "magic": "44435800"},
        ],
        "firmware": [{"name": "libsre.prx", "size": len(LIBSRE), "sha256": sha(LIBSRE), "magic": "7f454c46",
                      "from": "dev_flash/sys/external/libsre.sprx, decrypted"}],
    }


class GameTree:
    """A fake disc dump in a temporary folder."""

    def __init__(self, title="BLUS30443", ver="01.00", eboot_name="EBOOT.elf", decrypted=True, firmware=True):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "dump"
        g = self.root / "PS3_GAME"
        (g / "USRDIR" / "script").mkdir(parents=True)
        self.sfo = sfo.build({"TITLE_ID": title, "APP_VER": ver, "VERSION": ver, "CATEGORY": "DG"})
        (g / "PARAM.SFO").write_bytes(self.sfo)
        (g / "USRDIR" / "EBOOT.BIN").write_bytes(EBOOT_BIN)
        (g / "USRDIR" / "script" / "m01.luabnd.dcx.sdat").write_bytes(SDAT)
        if decrypted:
            (g / "USRDIR" / eboot_name).write_bytes(EBOOT_ELF)
            (g / "USRDIR" / "script" / "m01.luabnd.dcx.sdat.unedat").write_bytes(SDAT_DEC)
        if firmware:
            (self.root / "libsre.prx").write_bytes(LIBSRE)
        good = sfo.build({"TITLE_ID": "BLUS30443", "APP_VER": "01.00", "VERSION": "01.00", "CATEGORY": "DG"})
        self.table = make_table(good)

    def close(self):
        self.tmp.cleanup()


class SfoTest(unittest.TestCase):
    def test_round_trip(self):
        v = {"TITLE_ID": "BLUS30443", "APP_VER": "01.00", "PARENTAL_LEVEL": 5}
        self.assertEqual(sfo.parse(sfo.build(v)), v)

    def test_bad_magic(self):
        with self.assertRaises(sfo.SfoError):
            sfo.parse(b"junk" * 10)


class CheckTest(unittest.TestCase):
    def check(self, tree, **kw):
        return gamefiles.check(tree.root, table=tree.table, **kw)

    def test_ok(self):
        t = GameTree()
        try:
            rep = self.check(t)
            self.assertTrue(rep.ok, [i.message for i in rep.problems()])
            states = {i.name: i.state for i in rep.items}
            self.assertEqual(states["PS3_GAME/USRDIR/EBOOT.elf"], "ok")
            self.assertEqual(states["PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat.unedat"], "unmeasured")
            self.assertEqual(len(rep.inputs()), 3)
        finally:
            t.close()

    def test_ps3_game_folder_and_case(self):
        t = GameTree(eboot_name="EBOOT.ELF")
        try:
            rep = gamefiles.check(t.root / "PS3_GAME", table=t.table)
            self.assertTrue(rep.ok, [i.message for i in rep.problems()])
        finally:
            t.close()

    def test_wrong_title(self):
        t = GameTree(title="BLES00932")
        try:
            rep = self.check(t)
            self.assertFalse(rep.ok)
            self.assertIn("European", rep.items[0].message)
        finally:
            t.close()

    def test_wrong_version(self):
        t = GameTree(ver="01.04")
        try:
            rep = self.check(t)
            self.assertFalse(rep.ok)
            self.assertIn("01.04", rep.items[0].message)
        finally:
            t.close()

    def test_no_game(self):
        with tempfile.TemporaryDirectory() as d:
            rep = gamefiles.check(d, table=make_table(b""))
            self.assertFalse(rep.ok)
            self.assertEqual(rep.items[0].state, "missing")

    def test_missing_decrypted_then_extra_folder(self):
        t = GameTree(decrypted=False)
        try:
            rep = self.check(t)
            self.assertFalse(rep.ok)
            self.assertEqual({i.state for i in rep.problems()}, {"missing"})
            extra = Path(t.tmp.name) / "decrypted"
            extra.mkdir()
            (extra / "EBOOT.elf").write_bytes(EBOOT_ELF)
            (extra / "m01.luabnd.dcx.sdat.unedat").write_bytes(SDAT_DEC)
            rep = self.check(t, decrypted_dirs=[extra])
            self.assertTrue(rep.ok, [i.message for i in rep.problems()])
        finally:
            t.close()

    def test_extra_folder_keeps_the_game_subfolders(self):
        t = GameTree(decrypted=False)
        try:
            for top in ("", "USRDIR", "PS3_GAME/USRDIR"):
                extra = Path(t.tmp.name) / ("tree_" + top.replace("/", "_"))
                d = extra / top if top else extra
                (d / "script").mkdir(parents=True)
                (d / "EBOOT.elf").write_bytes(EBOOT_ELF)
                (d / "script" / "m01.luabnd.dcx.sdat.unedat").write_bytes(SDAT_DEC)
                rep = self.check(t, decrypted_dirs=[extra])
                self.assertTrue(rep.ok, (top, [i.message for i in rep.problems()]))
        finally:
            t.close()

    def test_still_encrypted(self):
        t = GameTree()
        try:
            (t.root / "PS3_GAME" / "USRDIR" / "script" / "m01.luabnd.dcx.sdat.unedat").write_bytes(SDAT)
            rep = self.check(t)
            self.assertEqual([i.state for i in rep.problems()], ["encrypted"])
        finally:
            t.close()

    def test_mismatch(self):
        t = GameTree()
        try:
            (t.root / "PS3_GAME" / "USRDIR" / "EBOOT.elf").write_bytes(b"\x7fELF" + b"q" * 60)
            rep = self.check(t)
            self.assertEqual([i.state for i in rep.problems()], ["mismatch"])
        finally:
            t.close()

    def test_damaged_original(self):
        t = GameTree()
        try:
            (t.root / "PS3_GAME" / "USRDIR" / "EBOOT.BIN").write_bytes(b"SCE\0" + b"x" * 59)
            rep = self.check(t)
            self.assertEqual([(i.kind, i.state) for i in rep.problems()], [("original", "mismatch")])
        finally:
            t.close()

    def test_firmware(self):
        t = GameTree(firmware=False)
        try:
            fw = Path(t.tmp.name) / "libsre.prx"
            fw.write_bytes(LIBSRE)
            self.assertTrue(self.check(t, firmware=fw).ok)
            fw.write_bytes(b"\x7fELFsrX")
            self.assertFalse(self.check(t, firmware=fw).ok)
        finally:
            t.close()

    def test_firmware_beside_ps3_game(self):
        t = GameTree()
        try:
            rep = self.check(t)
            self.assertTrue(rep.ok, [i.message for i in rep.problems()])
            item = next(i for i in rep.items if i.kind == "firmware")
            self.assertEqual((item.name, item.state, Path(item.found)), ("libsre.prx", "ok", t.root / "libsre.prx"))
            self.assertIn(item, rep.inputs())
        finally:
            t.close()

    def test_firmware_missing(self):
        """Always checked: the build lifts it (a game without it hangs at its loading screen)."""
        t = GameTree(firmware=False)
        try:
            rep = self.check(t)
            self.assertEqual([(i.kind, i.state) for i in rep.problems()], [("firmware", "missing")])
            msg = rep.problems()[0].message
            for words in ("libsre.prx is missing", "dev_flash/sys/external/libsre.sprx", "next to PS3_GAME"):
                self.assertIn(words, msg)
            self.assertEqual([i.kind for i in lift_blockers(rep)], ["firmware"])
        finally:
            t.close()

    def test_firmware_in_a_decrypted_folder(self):
        t = GameTree(firmware=False)
        try:
            dec = Path(t.tmp.name) / "decrypted"
            dec.mkdir()
            (dec / "LIBSRE.PRX").write_bytes(LIBSRE)
            rep = self.check(t, decrypted_dirs=[dec])
            self.assertTrue(rep.ok, [i.message for i in rep.problems()])
            # A wrong copy beside PS3_GAME does not hide a good one elsewhere.
            (t.root / "libsre.prx").write_bytes(b"\x7fELFsrX")
            self.assertTrue(self.check(t, decrypted_dirs=[dec]).ok)
            (dec / "LIBSRE.PRX").unlink()
            self.assertEqual([(i.kind, i.state) for i in self.check(t, decrypted_dirs=[dec]).problems()],
                             [("firmware", "mismatch")])
        finally:
            t.close()

    def test_firmware_still_encrypted(self):
        t = GameTree(firmware=False)
        try:
            (t.root / "libsre.prx").write_bytes(b"SCE\0" + b"q" * 3)
            self.assertEqual([(i.kind, i.state) for i in self.check(t).problems()], [("firmware", "encrypted")])
        finally:
            t.close()

    def test_lift_goes_on_without_the_sdat_files(self):
        t = GameTree()
        try:
            (t.root / "PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat.unedat").unlink()
            rep = self.check(t)
            self.assertFalse(rep.ok)
            self.assertEqual(lift_blockers(rep), [])
            (t.root / "PS3_GAME/USRDIR/EBOOT.elf").unlink()
            rep = self.check(t)
            self.assertEqual([i.name for i in lift_blockers(rep)], ["PS3_GAME/USRDIR/EBOOT.elf"])
        finally:
            t.close()

    def test_stage_copies_and_resumes(self):
        t = GameTree()
        try:
            rep = self.check(t)
            data = Path(t.tmp.name) / "data"
            jr = journal.Journal(data / "setup" / "journal.json")
            dest = stage(rep, data, jr)
            self.assertEqual((dest / "PS3_GAME/USRDIR/EBOOT.elf").read_bytes(), EBOOT_ELF)
            self.assertEqual((dest / "firmware/libsre.prx").read_bytes(), LIBSRE)
            ran, _ = jr.run("stage", jr.state("stage")["inputs"], lambda: self.fail("ran again"))
            self.assertFalse(ran)
        finally:
            t.close()

    def test_real_table_loads(self):
        tb = gamefiles.load_table()
        self.assertEqual(tb["title_id"], "BLUS30443")
        self.assertEqual(len(tb["originals"]), 28)
        self.assertEqual(len(tb["decrypted"]), 27)
        for e in tb["originals"] + tb["decrypted"] + tb["firmware"]:
            self.assertTrue(e["sha256"] is None or len(e["sha256"]) == 64)


class JournalTest(unittest.TestCase):
    def test_run_skip_rerun(self):
        with tempfile.TemporaryDirectory() as d:
            jr = journal.Journal(Path(d) / "j.json")
            calls = []
            fp = journal.fingerprint("a", 1)
            self.assertEqual(jr.run("s", fp, lambda: calls.append(1) or {"n": 1}), (True, {"n": 1}))
            self.assertEqual(journal.Journal(Path(d) / "j.json").run("s", fp, lambda: calls.append(2)),
                             (False, {"n": 1}))
            jr.run("s", journal.fingerprint("a", 2), lambda: calls.append(3))
            self.assertEqual(calls, [1, 3])

    def test_missing_output_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            jr = journal.Journal(Path(d) / "j.json")
            out = Path(d) / "out.bin"
            fp = journal.fingerprint("x")
            jr.run("s", fp, lambda: out.write_bytes(b"1") and None, outputs=[out])
            out.unlink()
            ran, _ = jr.run("s", fp, lambda: out.write_bytes(b"2") and None, outputs=[out])
            self.assertTrue(ran)

    def test_failure_is_recorded(self):
        with tempfile.TemporaryDirectory() as d:
            jr = journal.Journal(Path(d) / "j.json")

            def boom():
                raise ValueError("no")

            with self.assertRaises(journal.StepFailed):
                jr.run("s", "fp", boom)
            self.assertEqual(jr.state("s")["state"], "failed")
            self.assertIn("no", jr.state("s")["message"])


class _Handler(http.server.BaseHTTPRequestHandler):
    blob = b""
    ignore_range = False

    def do_GET(self):
        start = 0
        rng = self.headers.get("Range")
        if rng and not self.ignore_range:
            start = int(rng.split("=")[1].split("-")[0])
            self.send_response(206)
        else:
            self.send_response(200)
        body = self.blob[start:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def _zip(entries) -> bytes:
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        for name, data in entries:
            z.writestr(name, data)
    return b.getvalue()


class FetchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.url = f"http://127.0.0.1:{cls.srv.server_address[1]}/tool.zip"

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def entry(self, blob, **kw):
        _Handler.blob = blob
        e = {"name": "tool", "version": "1.0", "url": self.url, "size": len(blob), "sha256": sha(blob)}
        e.update(kw)
        return e

    def test_fetch_unpack_and_reuse(self):
        blob = _zip([("bin/clang.txt", b"hi")])
        with tempfile.TemporaryDirectory() as d:
            p = fetch.ensure(self.entry(blob), Path(d) / "dl", Path(d) / "tools", allow_http=True)
            self.assertEqual((p / "bin/clang.txt").read_bytes(), b"hi")
            _Handler.blob = b""                       # a second ensure must not download
            self.assertEqual(fetch.ensure(self.entry(blob), Path(d) / "dl", Path(d) / "tools",
                                          allow_http=True), p)

    def test_resume(self):
        blob = _zip([("a.txt", b"x" * 5000)])
        with tempfile.TemporaryDirectory() as d:
            e = self.entry(blob)
            part = fetch.archive_path(e, Path(d))
            part = part.with_name(part.name + ".part")
            part.write_bytes(blob[:1000])
            self.assertEqual(fetch.fetch(e, d, allow_http=True).read_bytes(), blob)

    def test_server_ignores_range(self):
        blob = _zip([("a.txt", b"y" * 5000)])
        with tempfile.TemporaryDirectory() as d:
            e = self.entry(blob)
            part = fetch.archive_path(e, Path(d))
            part.with_name(part.name + ".part").write_bytes(blob[:1000])
            _Handler.ignore_range = True
            try:
                self.assertEqual(fetch.fetch(e, d, allow_http=True).read_bytes(), blob)
            finally:
                _Handler.ignore_range = False

    def test_bad_sum_is_deleted(self):
        blob = _zip([("a.txt", b"z")])
        with tempfile.TemporaryDirectory() as d:
            e = self.entry(blob, sha256="0" * 64)
            with self.assertRaises(fetch.FetchError):
                fetch.fetch(e, d, allow_http=True)
            self.assertEqual(os.listdir(d), [])

    def test_unpinned_and_http_refused(self):
        blob = _zip([("a.txt", b"z")])
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(fetch.FetchError):
                fetch.fetch(self.entry(blob, sha256=None), d, allow_http=True)
            with self.assertRaises(fetch.FetchError):
                fetch.fetch(self.entry(blob), d)          # http:// without allow_http

    def test_unpack_refuses_escape(self):
        with tempfile.TemporaryDirectory() as d:
            a = Path(d) / "evil.zip"
            a.write_bytes(_zip([("../evil.txt", b"no")]))
            with self.assertRaises(fetch.FetchError):
                fetch.unpack(a, Path(d) / "out")
            self.assertFalse((Path(d) / "evil.txt").exists())

    def test_manifest_loads(self):
        m = fetch.load_manifest()
        for e in m["tools"]:
            self.assertTrue(e["url"].startswith("https://"))
            self.assertIn("why", e)

    def test_real_entries_pinned_or_refused(self):
        """Each real entry is pinned (a size and a SHA-256), or both are null and
        fetch refuses it before downloading anything."""
        for e in fetch.load_manifest()["tools"]:
            if e["sha256"] is None:
                self.assertIsNone(e["size"], e["name"])
                with tempfile.TemporaryDirectory() as d:
                    with self.assertRaises(fetch.FetchError):
                        fetch.fetch(e, d)
                    self.assertEqual(os.listdir(d), [])
            else:
                self.assertIsInstance(e["size"], int)
                self.assertRegex(e["sha256"], r"^[0-9a-f]{64}$")

    def test_compiler_entry(self):
        """The compiler download (docs/86): the asset is named as setup caches it,
        and a pinned size is that of a trimmed zip, not LLVM's 862 MB archive."""
        e = next(t for t in fetch.load_manifest()["tools"] if t["name"] == "llvm-win64")
        self.assertRegex(e["url"], r"^https://github\.com/[^/]+/[^/]+/releases/download/[^/]+/"
                         + re.escape(f"{e['name']}-{e['version']}.zip") + "$")
        self.assertTrue(e["version"].startswith("22.1.8-"))
        if e["size"] is not None:
            self.assertTrue(30_000_000 < e["size"] < 150_000_000, e["size"])

    def test_lzma_zip_unpacks(self):
        """The compiler zip's entries are LZMA by default; fetch.ensure reads them."""
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", compression=zipfile.ZIP_LZMA) as z:
            z.writestr("bin/clang.exe", b"MZ" + b"x" * 4000)
        with tempfile.TemporaryDirectory() as d:
            p = fetch.ensure(self.entry(b.getvalue()), Path(d) / "dl", Path(d) / "tools", allow_http=True)
            self.assertEqual((p / "bin/clang.exe").read_bytes(), b"MZ" + b"x" * 4000)

    def test_suffix(self):
        self.assertEqual(fetch.suffix("https://x/y/llvm-linux-x64-22.1.8-1.tar.xz"), ".tar.xz")
        self.assertEqual(fetch.suffix("https://x/20261021/cpython-3.13.17%2B20261021-x86_64-unknown-linux-gnu-"
                                      "install_only_stripped.tar.gz"), ".tar.gz")
        self.assertEqual(fetch.suffix("https://x/y/llvm-win64-22.1.8-1.zip"), ".zip")
        self.assertEqual(fetch.suffix("https://x/y/notes.txt.xz"), ".xz")

    def test_tar_unpacks_and_keeps_the_execute_bit(self):
        """The Linux compiler archive (docs/87): a .tar.xz, unpacked with the owner's execute bit kept."""
        blob = _tar([("bin/clang", b"\x7fELF" + b"x" * 100, 0o755), ("lib/clang/22/include/stddef.h", b"/**/", 0o644)])
        with tempfile.TemporaryDirectory() as d:
            e = self.entry(blob, url=self.url.replace("tool.zip", "tool.tar.xz"))
            p = fetch.ensure(e, Path(d) / "dl", Path(d) / "tools", allow_http=True)
            self.assertTrue((Path(d) / "dl" / "tool-1.0.tar.xz").is_file())
            self.assertEqual((p / "bin/clang").read_bytes()[:4], b"\x7fELF")
            self.assertEqual((p / "lib/clang/22/include/stddef.h").read_bytes(), b"/**/")
            if os.name != "nt":
                self.assertTrue(os.access(p / "bin/clang", os.X_OK))

    def test_tar_keeps_links_inside_the_folder(self):
        """Linux's Python (python-build-standalone) has 1,048 symlinks, all inside its folder (bin/python3 ->
        python3.13). Where links cannot be made (Windows without the right), tarfile puts a copy there."""
        blob = _tar([("python/bin/python3.13", b"\x7fELF" + b"p" * 50, 0o755),
                     ("python/bin/python3", "python3.13", "sym"),
                     ("python/share/a/x", b"term", 0o644), ("python/share/1/y", "../a/x", "sym"),
                     ("python/bin/py", "python/bin/python3.13", "hard")])
        with tempfile.TemporaryDirectory() as d:
            a = Path(d) / "ok.tar.gz"
            a.write_bytes(blob)
            out = fetch.unpack(a, Path(d) / "out")
            self.assertEqual((out / "python/bin/python3").read_bytes()[:4], b"\x7fELF")
            self.assertEqual((out / "python/share/1/y").read_bytes(), b"term")
            self.assertEqual((out / "python/bin/py").read_bytes()[:4], b"\x7fELF")

    def test_tar_refuses_escapes_links_and_devices(self):
        cases = {
            "dotdot": [("../evil.txt", b"no", 0o644)],
            "absolute": [("/tmp/evil.txt", b"no", 0o644)],
            "symlink out": [("bin/ld.lld", "../../lld", "sym")],
            "symlink absolute": [("bin/ld.lld", "/usr/bin/lld", "sym")],
            "hardlink out": [("bin/b", "../a", "hard")],
            "device": [("dev/null", None, "chr")],
        }
        for what, members in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                a = Path(d) / "evil.tar.xz"
                a.write_bytes(_tar(members))
                with self.assertRaises(fetch.FetchError):
                    fetch.unpack(a, Path(d) / "out")
                self.assertFalse((Path(d) / "evil.txt").exists())
                self.assertFalse((Path(d) / "out").exists())
                self.assertFalse((Path(d) / "out.tmp").exists())


def _tar(members) -> bytes:
    """A .tar.xz in memory: (name, data, mode) files, or (name, target, "sym" | "hard") links, or (name, None,
    "chr") a character device."""
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w:xz") as t:
        for name, data, kind in members:
            info = tarfile.TarInfo(name)
            if kind == "sym":
                info.type, info.linkname = tarfile.SYMTYPE, data
                t.addfile(info)
            elif kind == "hard":
                info.type, info.linkname = tarfile.LNKTYPE, data
                t.addfile(info)
            elif kind == "chr":
                info.type, info.devmajor, info.devminor = tarfile.CHRTYPE, 1, 3
                t.addfile(info)
            else:
                info.size, info.mode = len(data), kind
                t.addfile(info, io.BytesIO(data))
    return b.getvalue()


if __name__ == "__main__":
    unittest.main()
