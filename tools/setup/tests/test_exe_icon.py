"""The program's icon (des_setup/exe_icon.py), with a small DLL of Python's standing in for the program, and a copy of
python.exe for one that is running. Windows only."""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from des_setup import exe_icon, protocol  # noqa: E402


def ico_bytes():
    """An .ico of two frames (their pictures are not looked at)."""
    frames = [(32, 32, b"\x28" + b"a" * 100), (0, 0, b"\x89PNG" + b"b" * 200)]
    out = struct.pack("<HHH", 0, 1, len(frames))
    at = 6 + 16 * len(frames)
    for w, h, body in frames:
        out += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(body), at)
        at += len(body)
    return out + b"".join(f[2] for f in frames)


class ReadIcoTest(unittest.TestCase):
    def test_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "a.ico"
            p.write_bytes(ico_bytes())
            frames = exe_icon.read_ico(p)
        self.assertEqual([f[0] for f in frames], [32, 0])
        self.assertEqual(frames[1][5][:4], b"\x89PNG")

    def test_not_an_icon(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "a.ico"
            for bad in (b"", b"\x89PNG\r\n\x1a\n", struct.pack("<HHH", 0, 1, 3), ico_bytes()[:60]):
                p.write_bytes(bad)
                with self.assertRaises(exe_icon.IconError):
                    exe_icon.read_ico(p)


@unittest.skipUnless(os.name == "nt", "Windows only")
class StampTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        dll = Path(sys.base_prefix) / "python3.dll"
        if not dll.is_file():
            self.skipTest("no python3.dll here")
        self.exe = self.dir / "Game.exe"
        shutil.copyfile(dll, self.exe)
        (self.dir / "x.ico").write_bytes(ico_bytes())
        self.frames = exe_icon.read_ico(self.dir / "x.ico")

    def tearDown(self):
        self.tmp.cleanup()

    def test_goes_in_and_is_seen(self):
        self.assertFalse(exe_icon.has_icon(self.exe))
        exe_icon.stamp(self.exe, self.frames, started=lambda new: None)
        self.assertTrue(exe_icon.has_icon(self.exe))
        self.assertEqual([p.name for p in self.dir.iterdir() if p.name.startswith("Game.")], ["Game.exe"])

    def test_a_copy_that_does_not_start_changes_nothing(self):
        before = self.exe.read_bytes()

        def fails(new):
            raise exe_icon.IconError("no")

        with self.assertRaises(exe_icon.IconError):
            exe_icon.stamp(self.exe, self.frames, started=fails)
        self.assertEqual(self.exe.read_bytes(), before)
        self.assertEqual([p.name for p in self.dir.iterdir() if p.name.startswith("Game.")], ["Game.exe"])


@unittest.skipUnless(os.name == "nt", "Windows only")
class RunningProgramTest(unittest.TestCase):
    def test_takes_the_place_of_a_running_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            exe = d / "Game.exe"
            shutil.copyfile(sys.executable, exe)
            for dll in Path(sys.base_prefix).glob("python3*.dll"):
                shutil.copyfile(dll, d / dll.name)
            env = dict(os.environ, PYTHONHOME=sys.base_prefix)
            p = subprocess.Popen([str(exe), "-c", "import time; time.sleep(60)"], env=env)
            try:
                time.sleep(0.5)
                if p.poll() is not None:
                    self.skipTest("a copy of python.exe does not run here")
                new = d / "Game.new.exe"
                new.write_bytes(b"the new one")
                exe_icon.swap_in(exe, new)
                self.assertEqual(exe.read_bytes(), b"the new one")
                self.assertFalse(new.exists())
                self.assertTrue(list((d / exe_icon.UPDATE).glob(exe_icon.OLD + "-*")))
            finally:
                p.kill()
                p.wait()


class ApplyTest(unittest.TestCase):
    def run_apply(self, exe, data):
        out = io.StringIO()
        return exe_icon.apply(exe, data / "game", data, protocol.Emitter(out)), out.getvalue()

    def test_leaves_a_developers_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            exe = d / "des_host.exe"
            exe.write_bytes(b"x")
            done, _out = self.run_apply(exe, d)
            self.assertFalse(done)
            self.assertEqual(exe.read_bytes(), b"x")

    def test_knob_leaves_it(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {"DES_SETUP_ICON": "0"}):
            d = Path(tmp)
            (d / "setup").mkdir()
            (d / "setup" / "release-info.txt").write_text("exe=Game.exe\n")
            exe = d / "Game.exe"
            exe.write_bytes(b"x")
            done, _out = self.run_apply(exe, d)
            self.assertFalse(done)

    @unittest.skipUnless(os.name == "nt", "Windows only")
    def test_a_program_that_cannot_take_one_is_only_noted(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "setup").mkdir()
            (d / "setup" / "release-info.txt").write_text("exe=Game.exe\n")
            exe = d / "Game.exe"
            exe.write_bytes(b"not a program")
            done, out = self.run_apply(exe, d)
            self.assertFalse(done)
            self.assertIn("note the program has no icon", out)
            self.assertEqual(exe.read_bytes(), b"not a program")

    @unittest.skipUnless(os.name == "nt", "Windows only")
    def test_the_games_own_when_the_sword_cannot_be_drawn(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            dll = Path(sys.base_prefix) / "python3.dll"
            if not dll.is_file():
                self.skipTest("no python3.dll here")
            (d / "setup").mkdir()
            (d / "setup" / "release-info.txt").write_text("exe=Game.exe\n")
            exe = d / "Game.exe"
            shutil.copyfile(dll, exe)
            calls = []

            def own(game, ico):
                calls.append("own")
                ico.write_bytes(ico_bytes())
                return True

            real = exe_icon.stamp
            with mock.patch.object(exe_icon, "_drawn", lambda prog, game, ico: False), \
                    mock.patch.object(exe_icon, "_games_own", own), \
                    mock.patch.object(exe_icon, "stamp", lambda e, f: real(e, f, started=lambda new: None)):
                done, out = self.run_apply(exe, d)
            self.assertTrue(done, out)
            self.assertEqual(calls, ["own"])
            self.assertIn("the game's own", out)
            self.assertTrue(exe_icon.has_icon(exe))
            done, out = self.run_apply(exe, d)      # (it has one now: left alone)
            self.assertFalse(done)
            self.assertEqual(calls, ["own"])


if __name__ == "__main__":
    unittest.main()
