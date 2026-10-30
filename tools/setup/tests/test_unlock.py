"""Tests for the RPCS3 route's unlock (des_setup/rpcs3.py and its phase in
pipeline.py, docs/90 test T8), with fake_rpcs3.py standing in for RPCS3 and
made-up files with a table of their own (no game data, no RPCS3).

  cd tools/setup && python -m unittest discover -s tests
"""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from des_setup import pipeline, protocol, rpcs3, sfo  # noqa: E402
from test_lift import FakeTree  # noqa: E402

FAKE = Path(__file__).with_name("fake_rpcs3.py")
PROGRAM = "rpcs3.exe" if os.name == "nt" else "rpcs3"
FAST = {"stall": 10.0, "poll": 0.05}          # the watchdog only for the test that hangs


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def npd(plain_size: int, body: bytes) -> bytes:
    """An SDAT's header as RPCS3 reads it: NPD\\0, the SDAT flag at 0x80, the plain size at 0x88."""
    return (b"NPD\0" + b"\0" * 0x7C + rpcs3.SDAT_FLAG.to_bytes(4, "big") + (0x4000).to_bytes(4, "big")
            + plain_size.to_bytes(8, "big") + body)


EBOOT_ELF = b"\x7fELF" + b"E" * 60
M02_DEC = b"DCX\0" + b"C" * 20
M01_DEC = b"DCX\0" + b"S" * 24
# Each original's table path, its bytes, and what RPCS3 makes of it.
FILES = [
    ("PS3_GAME/USRDIR/EBOOT.BIN", b"SCE\0" + b"e" * 60, EBOOT_ELF),
    ("PS3_GAME/USRDIR/chr/m02.chresdbnd.dcx.sdat", npd(len(M02_DEC), b"c" * 40), M02_DEC),
    ("PS3_GAME/USRDIR/script/m01.luabnd.dcx.sdat", npd(len(M01_DEC), b"s" * 40), M01_DEC),
]
M02, M01 = "m02.chresdbnd.dcx.sdat", "m01.luabnd.dcx.sdat"


def decrypted(path: str) -> str:
    """The output's name, as decrypt_binaries.cpp makes it."""
    return path[:path.rfind(".")] + ".elf" if path.endswith(".BIN") else path + ".unedat"


def put(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def snapshot(root: Path) -> dict:
    """Every folder, and every file's size, time and SHA-256, under root."""
    out = {}
    for dirpath, dirs, files in os.walk(root):
        for d in dirs:
            out[os.path.relpath(os.path.join(dirpath, d), root)] = "folder"
        for f in files:
            p = os.path.join(dirpath, f)
            st = os.stat(p)
            out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns, sha(Path(p).read_bytes()))
    return out


class Tree:
    """A fake dump, a fake RPCS3 folder (the stand-in, its settings and some of the
    player's files), the stand-in's fixtures, and a data folder."""

    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.dump = self.base / "dump"
        good = sfo.build({"TITLE_ID": "BLUS30443", "APP_VER": "01.00", "VERSION": "01.00", "CATEGORY": "DG"})
        put(self.dump / "PS3_GAME" / "PARAM.SFO", good)
        for p, orig, dec in FILES:
            put(self.dump / p, orig)
            put(self.base / "fixtures" / p.rsplit("/", 1)[-1], dec)
        self.rpcs3 = self.base / "RPCS3"
        put(self.rpcs3 / PROGRAM, b"a stand-in: the tests run fake_rpcs3.py")
        shutil.copyfile(FAKE, self.rpcs3 / "fake_rpcs3.py")
        put(self.rpcs3 / "config" / "config.yml", b"Core: {}\n")
        put(self.rpcs3 / "dev_hdd0" / "tmp" / "keep.txt", b"the player's")
        self.calls_file, self.heartbeat = self.base / "calls.jsonl", self.base / "heartbeat.txt"
        self.mode("ok")
        self.data = self.base / "data"
        self.work = self.data / "setup" / "unlock"
        self.table = {
            "title_id": "BLUS30443", "app_ver": "01.00", "version": "01.00", "other_titles": {},
            "originals": [{"path": "PS3_GAME/PARAM.SFO", "size": len(good), "sha256": sha(good)}]
            + [{"path": p, "size": len(o), "sha256": sha(o)} for p, o, _ in FILES],
            "decrypted": [{"path": decrypted(p), "of": p, "size": len(d), "sha256": sha(d), "magic": d[:4].hex()}
                          for p, _, d in FILES],
            "firmware": []}

    def mode(self, mode):
        (self.rpcs3 / "fake.json").write_text(json.dumps({
            "mode": mode, "fixtures": str(self.base / "fixtures"), "calls": str(self.calls_file),
            "root": str(self.base), "heartbeat": str(self.heartbeat)}), encoding="utf-8")

    def run(self, phases=("check", "unlock", "stage"), request="mine", unlock_kw=None, **kw):
        out = io.StringIO()
        req = pipeline.make_request(self.dump, rpcs3_path=self.rpcs3) if request == "mine" else request
        opts = dict(FAST, launch=[sys.executable, "-I", str(self.rpcs3 / "fake_rpcs3.py")])
        opts.update(unlock_kw or {})
        rc = pipeline.run(self.data, req, app="1.0", emit=protocol.Emitter(out), table=self.table,
                          manifest={"tools": []}, phases=list(phases), quiet=False, unlock_kw=opts, **kw)
        return rc, out.getvalue().splitlines()

    def calls(self) -> list:
        if not self.calls_file.is_file():
            return []
        return [json.loads(x) for x in self.calls_file.read_text(encoding="utf-8").splitlines()]

    def log(self) -> str:
        return (self.data / "setup" / "logs" / "unlock.log").read_text(encoding="utf-8")

    def close(self):
        self.tmp.cleanup()


class UnlockTest(unittest.TestCase):
    def setUp(self):
        # The player's RPCS3 folders come from the environment: keep this PC's out of the tests.
        self._env = {k: os.environ.pop(k, None) for k in ("RPCS3_CONFIG_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME")}
        self.t = Tree()
        os.environ["XDG_CONFIG_HOME"] = str(self.t.base / "xdg-config")
        os.environ["XDG_CACHE_HOME"] = str(self.t.base / "xdg-cache")
        self._running = rpcs3.running

    def tearDown(self):
        rpcs3.running = self._running
        for k, v in self._env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.t.close()

    def assertInOrder(self, want, lines):
        at = [lines.index(w) if w in lines else -1 for w in want]
        self.assertNotIn(-1, at, [w for w, i in zip(want, at) if i < 0])
        self.assertEqual(at, sorted(at), lines)

    def test_unlock_then_stage(self):
        exe = rpcs3.find(self.t.rpcs3)
        put(rpcs3.player_dirs(exe)["log"] / "RPCS3.log", b"RPCS3 v0.0.0-fake Alpha | test\n")
        rc, lines = self.t.run()
        self.assertEqual(rc, 0, lines)
        self.assertEqual(lines[0], "@setup 2")
        self.assertInOrder(["@plan check unlock stage", "@step check start", f"@rpcs3 own {self.t.rpcs3}",
                            "@check missing PS3_GAME/USRDIR/EBOOT.elf EBOOT.elf is missing: your RPCS3 unlocks it "
                            "next.", "@step check done", "@step unlock start", "@file unlock EBOOT.BIN",
                            "@progress unlock 0/3", "@progress unlock 3/3", "@step unlock done", "@step stage done",
                            "@done ok"], lines)
        for p, _, dec in FILES:
            self.assertEqual((self.t.data / "inputs" / decrypted(p)).read_bytes(), dec)
        self.assertFalse(self.t.work.exists())          # the copies, RPCS3's folder and the outputs: all gone

        calls = self.t.calls()
        self.assertEqual(len(calls), 1)
        c = calls[0]
        self.assertEqual(c["inputs"], [str(self.t.work / "in" / p) for p, _, _ in FILES])
        self.assertEqual(os.path.normcase(c["cwd"]), os.path.normcase(str(self.t.work)))
        self.assertEqual(c["stdin"], "")                 # the null device: a KLIC prompt would end at once
        self.assertFalse(c["outside"])
        self.assertEqual(c["env"]["QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES"], "1")
        home = self.t.work / "rpcs3"
        if os.name == "nt":
            self.assertEqual(c["env"]["RPCS3_CONFIG_DIR"], str(home) + "\\")
        else:
            self.assertEqual((c["env"]["XDG_CONFIG_HOME"], c["env"]["XDG_CACHE_HOME"]),
                             (str(home / "config"), str(home / "cache")))
        log = self.t.log()
        self.assertIn("RPCS3 v0.0.0-fake Alpha | test", log)
        self.assertIn("EBOOT.elf: 64 bytes, matches", log)
        self.assertIn("Decrypted ", log)                 # what RPCS3 printed is in the step log, not the protocol
        self.assertFalse(any("Decrypted" in x for x in lines))

    def test_progress_follows_each_file(self):
        self.t.mode("pace=0.3")
        rc, lines = self.t.run(unlock_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertInOrder(["@plan check unlock", "@file unlock EBOOT.BIN", "@progress unlock 0/3",
                            f"@file unlock chr/{M02}", "@progress unlock 1/3", f"@file unlock script/{M01}",
                            "@progress unlock 2/3", "@progress unlock 3/3", "@step unlock done", "@done ok"], lines)
        overall = [int(x.split()[1].split("/")[0]) for x in lines if x.startswith("@overall ")]
        self.assertEqual(overall, sorted(overall))

    def test_second_run_skips_the_unlock(self):
        rc, lines = self.t.run(unlock_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step unlock done", lines)
        kept = sorted(str(p.relative_to(self.t.work)).replace("\\", "/") for p in self.t.work.rglob("*") if p.is_file())
        self.assertEqual(kept, sorted("in/" + decrypted(p) for p, _, _ in FILES))   # only the outputs, until the stage

        rc, lines = self.t.run()                         # the build: the journal skips the unlock
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step unlock skip", lines)
        self.assertIn("@step stage done", lines)
        self.assertFalse(self.t.work.exists())
        rc, lines = self.t.run()                         # a rebuild: the kept files are found in inputs/
        self.assertEqual(rc, 0, lines)
        self.assertIn("@check ok PS3_GAME/USRDIR/EBOOT.elf EBOOT.elf matches.", lines)
        self.assertIn("@step unlock skip", lines)
        self.assertEqual(len(self.t.calls()), 1)

    def test_a_changed_output_is_unlocked_again(self):
        self.assertEqual(self.t.run(unlock_only=True)[0], 0)
        put(self.t.work / "in" / decrypted(FILES[2][0]), b"DCX\0" + b"X" * 24)
        rc, lines = self.t.run()
        self.assertEqual(rc, 0, lines)
        self.assertEqual(len(self.t.calls()), 2)
        self.assertEqual((self.t.data / "inputs" / decrypted(FILES[2][0])).read_bytes(), M01_DEC)

    def test_a_file_that_fails_in_the_batch_is_tried_alone(self):
        self.t.mode(f"batchfail={M02}")                  # RPCS3 stops at m02: m01 after it is not done either
        rc, lines = self.t.run()
        self.assertEqual(rc, 0, lines)
        self.assertEqual([[Path(i).name for i in c["inputs"]] for c in self.t.calls()],
                         [["EBOOT.BIN", M02, M01], [M02], [M01]])
        self.assertIn("once more, on its own", self.t.log())

    def test_a_wrong_output_names_the_file(self):
        self.t.mode(f"short={M01}")
        rc, lines = self.t.run()
        self.assertEqual(rc, 1)
        self.assertIn(f"@error rpcs3 unlock script/{M01}.unedat came out different from the known good version.",
                      lines)
        self.assertEqual([[Path(i).name for i in c["inputs"]] for c in self.t.calls()][1:], [[M01]])
        self.assertTrue(any(x.startswith("@log ") and x.endswith("unlock.log") for x in lines), lines)
        self.assertEqual(lines[-1], "@done failed")
        self.assertFalse(self.t.work.exists())           # a failure leaves nothing decrypted behind
        self.assertFalse((self.t.data / "inputs").exists())

    def test_a_missing_output_names_the_file(self):
        self.t.mode(f"fail={M01}")
        rc, lines = self.t.run()
        self.assertEqual(rc, 1)
        self.assertIn(f"@error rpcs3 unlock RPCS3 could not unlock script/{M01}.", lines)
        self.assertEqual(len(self.t.calls()), 2)

    def test_the_watchdog_ends_rpcs3_and_what_it_started(self):
        self.t.mode(f"hang={M02}")
        t0 = time.monotonic()
        rc, lines = self.t.run(unlock_kw={"stall": 1.0})
        self.assertLess(time.monotonic() - t0, 10.0)     # not the stand-in's 20 s
        self.assertEqual(rc, 1)
        self.assertIn(f"@error rpcs3 unlock RPCS3 stopped answering while it unlocked chr/{M02}.", lines)
        self.assertEqual(len(self.t.calls()), 1)         # no retry after a hang
        self.assertTrue(self.t.heartbeat.is_file())
        size = self.t.heartbeat.stat().st_size
        for _ in range(3):                               # the helper RPCS3 started was ended with it
            time.sleep(0.15)
            self.assertEqual(self.t.heartbeat.stat().st_size, size)
        self.assertFalse(self.t.work.exists())

    def test_too_old(self):
        self.t.mode("old")
        rc, lines = self.t.run()
        self.assertEqual(rc, 1)
        self.assertIn("@error rpcs3 unlock This RPCS3 is too old to unlock the files. Update RPCS3.", lines)
        self.assertEqual(len(self.t.calls()), 1)
        self.assertIn("Unknown option", self.t.log())

    def test_rpcs3_could_not_start_or_write(self):
        for mode, sentence in (("abort", "RPCS3 could not start."),
                               ("nowrite=EBOOT.BIN", "RPCS3 could not write in setup's folder. Your antivirus may "
                                                     "be blocking it.")):
            with self.subTest(mode=mode):
                self.t.mode(mode)
                rc, lines = self.t.run(unlock_only=True)
                self.assertEqual(rc, 1)
                self.assertIn(f"@error rpcs3 unlock {sentence}", lines)

    def test_rpcs3_set_to_run_as_administrator(self):
        """Windows will not start it (WinError 740): the player is told why, not that copying failed."""
        def refused(*_a, **_k):
            e = OSError("The requested operation requires elevation")
            e.winerror = rpcs3.ERROR_ELEVATION_REQUIRED
            raise e
        child, rpcs3._Child = rpcs3._Child, refused
        try:
            rc, lines = self.t.run(unlock_only=True)
        finally:
            rpcs3._Child = child
        self.assertEqual(rc, 1)
        self.assertIn(f"@error rpcs3 unlock {rpcs3.NEEDS_ADMIN}", lines)
        self.assertEqual(self.t.calls(), [])
        self.assertIn("RPCS3 did not start", self.t.log())

    def test_rpcs3_folder_and_dump_stay_the_same(self):
        """The whole build, the lift too: neither the player's RPCS3 folder nor the dump changes, byte for byte."""
        tree = FakeTree()
        try:
            before = snapshot(self.t.rpcs3), snapshot(self.t.dump)
            rc, lines = self.t.run(phases=("check", "unlock", "fetch", "stage", "lift"), src=tree.src)
            self.assertEqual(rc, 0, lines)
            self.assertEqual((snapshot(self.t.rpcs3), snapshot(self.t.dump)), before)
            self.assertIn("@step lift done", lines)
        finally:
            tree.close()

    def test_portable_install(self):
        """A `portable` folder: setup says so, refuses while that RPCS3 runs, and otherwise runs it as it is."""
        (self.t.rpcs3 / "portable").mkdir()
        rc, lines = self.t.run(check_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertIn(f"@rpcs3 shared {self.t.rpcs3}", lines)
        asked = []
        rpcs3.running = lambda exe: asked.append(Path(exe)) or True
        rc, lines = self.t.run(unlock_only=True)
        self.assertEqual(rc, 1)
        self.assertIn("@error rpcs3 unlock RPCS3 is open. Close it so setup can start it.", lines)
        self.assertEqual(asked, [self.t.rpcs3 / PROGRAM])
        self.assertEqual(self.t.calls(), [])
        rpcs3.running = lambda exe: False
        rc, lines = self.t.run(unlock_only=True)
        self.assertEqual(rc, 0, lines)
        env = self.t.calls()[0]["env"]
        self.assertNotIn(str(self.t.work), json.dumps(env))     # no redirect: it would not work
        self.assertTrue((self.t.rpcs3 / "portable" / "config" if os.name == "nt" else
                         self.t.rpcs3 / "portable").is_dir())

    def test_an_ignored_redirect_is_remembered(self):
        self.t.mode("ignore")
        rc, lines = self.t.run(unlock_only=True)
        self.assertEqual(rc, 0, lines)                   # the files came out right all the same
        self.assertIn("RPCS3 ignored", self.t.log())
        rc, lines = self.t.run(check_only=True)
        self.assertIn(f"@rpcs3 shared {self.t.rpcs3}", lines)

    def test_ppu_debug_eboot_counts(self):
        exe = rpcs3.find(self.t.rpcs3)
        dumps = rpcs3.player_dirs(exe)["cache"] / "ppu_progs"
        put(dumps / "BLUS30443-A1-EBOOT.BIN" / "exec.elf", b"\x7fELF" + b"Q" * 60)
        put(dumps / "BLUS30443-K2dX9-EBOOT.BIN" / "exec.elf", EBOOT_ELF)
        rc, lines = self.t.run()
        self.assertEqual(rc, 0, lines)
        self.assertIn("@check ok PS3_GAME/USRDIR/EBOOT.elf exec.elf matches.", lines)
        self.assertIn("@progress unlock 2/2", lines)
        self.assertEqual([[Path(i).name for i in c["inputs"]] for c in self.t.calls()], [[M02, M01]])
        self.assertEqual((self.t.data / "inputs" / "PS3_GAME" / "USRDIR" / "EBOOT.elf").read_bytes(), EBOOT_ELF)

    def test_rpcs3_gone(self):
        gone = self.t.base / "old RPCS3"
        rc, lines = self.t.run(request=pipeline.make_request(self.t.dump, rpcs3_path=gone), check_only=True)
        self.assertEqual(rc, 1)
        self.assertIn(f"@rpcs3 missing {gone}", lines)
        self.assertEqual(lines[-2:], [f"@error rpcs3 check RPCS3 is not in {gone} any more.", "@done fix"])

    def test_a_copy_that_is_not_what_rpcs3_needs(self):
        """The header check before RPCS3 runs: here the SDAT's plain size disagrees with the table."""
        self.t.table["decrypted"][2]["size"] += 1
        rc, lines = self.t.run()
        self.assertEqual(rc, 1)
        self.assertIn("@error step unlock Copying the game files for RPCS3 failed.", lines)
        self.assertEqual(self.t.calls(), [])
        self.assertIn("says it holds 28 bytes; the table says 29", self.t.log())

    def test_find_and_running(self):
        self.assertEqual(rpcs3.find(self.t.rpcs3), (self.t.rpcs3 / PROGRAM).absolute())
        self.assertEqual(rpcs3.find(self.t.rpcs3 / PROGRAM), (self.t.rpcs3 / PROGRAM).absolute())
        self.assertIsNone(rpcs3.find(self.t.base / "nothing here"))
        self.assertEqual(rpcs3.where(self.t.rpcs3 / PROGRAM), str(self.t.rpcs3))
        self.assertFalse(rpcs3.running(self.t.rpcs3 / PROGRAM))
        p = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(10)"], stdin=subprocess.DEVNULL)
        try:
            self.assertTrue(rpcs3.running(sys.executable))
        finally:
            p.kill()
            p.wait()


if __name__ == "__main__":
    unittest.main()
