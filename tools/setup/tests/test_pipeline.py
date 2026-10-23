"""Tests for `run` (des_setup/pipeline.py) and the setup screens' line protocol
(protocol.py), on test_setup's made-up dump and test_lift's fake tools (no game
data, no real lifter). The last two check that the host's C++ agrees with them.

  cd tools/setup && python -m unittest discover -s tests
"""
import io
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from des_setup import compile as compile_step, fetch, journal, pipeline, protocol  # noqa: E402
from test_lift import REAL, FakeTree  # noqa: E402
from test_setup import GameTree  # noqa: E402

PHASES = ["check", "fetch", "stage", "lift"]
UI = REAL / "port" / "ui" / "src"


class Setup:
    """A fake dump, a fake source tree and a data folder; run() returns the exit status and the lines."""

    def __init__(self, **game):
        self.game = GameTree(**game)
        self.tree = FakeTree()
        self.data = Path(self.game.tmp.name) / "data"

    def request(self):
        return pipeline.make_request(self.game.root)

    def run(self, request="mine", **kw):
        out = io.StringIO()
        kw.setdefault("phases", PHASES)
        kw.setdefault("manifest", {"tools": []})
        rc = pipeline.run(self.data, self.request() if request == "mine" else request, app="1.0",
                          emit=protocol.Emitter(out), src=self.tree.src, table=self.game.table,
                          quiet=False, **kw)
        return rc, out.getvalue().splitlines()

    def close(self):
        self.tree.close()
        self.game.close()


def fake_compile(data, jr, jobs, progress):
    """A compile phase that writes a module (as compile.py's link would)."""
    progress("compile", 1, 1)
    out = data / "game" / "des_game.dll"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"MZ module")
    return out


class RunTest(unittest.TestCase):
    def setUp(self):
        self.s = Setup()
        self._free = pipeline.free_bytes
        pipeline.free_bytes = lambda p: 1 << 50

    def tearDown(self):
        pipeline.free_bytes = self._free
        self.s.close()

    def test_full_run_then_nothing_to_do(self):
        rc, lines = self.s.run()
        self.assertEqual(rc, 0, lines)
        self.assertEqual(lines[0], "@setup 2")
        self.assertEqual(lines[-1], "@done ok")
        for want in ("@plan check fetch stage lift", "@step check done", "@step fetch skip", "@step stage done",
                     "@step lift done", "@progress ppu 5/10", "@progress spu 2/2", "@overall 1000/1000"):
            self.assertIn(want, lines)
        self.assertIn("@check ok PS3_GAME/USRDIR/EBOOT.elf EBOOT.elf matches.", lines)
        overall = [int(x.split()[1].split("/")[0]) for x in lines if x.startswith("@overall ")]
        self.assertEqual(overall, sorted(overall))
        self.assertFalse((self.s.data / "setup" / pipeline.READY).exists())    # no compile phase: not ready
        log = (self.s.data / "setup" / "logs" / "run.log").read_text(encoding="utf-8").splitlines()
        self.assertEqual(log, lines)

        self.s.tree.reset_calls()
        rc, lines = self.s.run()
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step stage skip", lines)
        self.assertIn("@step lift skip", lines)
        self.assertEqual(self.s.tree.calls(), [])
        self.assertTrue((self.s.data / "setup" / "logs" / "run.prev.log").exists())

    def test_check_only(self):
        rc, lines = self.s.run(check_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertIn("@plan check", lines)
        self.assertFalse((self.s.data / "inputs").exists())

    def test_game_problem(self):
        s = Setup(decrypted=False)
        try:
            rc, lines = s.run()
            self.assertEqual(rc, 1)
            self.assertEqual(lines[-1], "@done fix")
            self.assertTrue(any(x.startswith("@error game check 2 decrypted files are missing.") for x in lines), lines)
            self.assertTrue(any(x.startswith("@check missing PS3_GAME/USRDIR/EBOOT.elf ") for x in lines))
        finally:
            s.close()

    def test_saved_request(self):
        self.assertEqual(self.s.run()[0], 0)
        rc, lines = self.s.run(request=None)
        self.assertEqual(rc, 0, lines)
        self.assertEqual(pipeline.load_request(self.s.data)["game"], os.path.abspath(self.s.game.root))

    def test_no_request(self):
        rc, lines = self.s.run(request=None)
        self.assertEqual(rc, 1)
        self.assertEqual(lines[-2:], ["@error game check Choose the game folder first.", "@done fix"])

    def test_step_failure_then_resume(self):
        (self.s.tree.src / "fail.txt").write_text("ppu_lifter.py")
        rc, lines = self.s.run()
        self.assertEqual(rc, 1)
        self.assertIn("@error step ppu Converting the game's code failed.", lines)
        self.assertTrue(any(x.startswith("@log ") and x.endswith("ppu.log") for x in lines), lines)
        self.assertEqual(lines[-1], "@done failed")
        (self.s.tree.src / "fail.txt").unlink()
        self.s.tree.reset_calls()
        rc, lines = self.s.run()
        self.assertEqual(rc, 0, lines)
        self.assertEqual(self.s.tree.calls()[0], "ppu_lifter.py")

    def test_busy(self):
        held = pipeline.lock(self.s.data / "setup" / "run.lock")
        try:
            rc, lines = self.s.run()
        finally:
            held.close()
        self.assertEqual(rc, 1)
        self.assertEqual(lines, ["@setup 2", "@error busy - Setup is already running.", "@done failed"])
        self.assertEqual(self.s.run()[0], 0)

    def test_short_of_space(self):
        pipeline.free_bytes = lambda p: 0
        rc, lines = self.s.run()
        self.assertEqual(rc, 1)
        self.assertTrue(any(x.startswith("@error disk fetch Setup needs about 0.5 GB free on ") for x in lines), lines)

    def test_ready_then_status(self):
        rc, lines = self.s.run(phases=PHASES + ["compile"], compile_fn=fake_compile)
        self.assertEqual(rc, 0, lines)
        ready = pipeline.read_ready(self.s.data)
        self.assertEqual((ready["app"], ready["module_size"]), ("1.0", str(len(b"MZ module"))))
        self.assertEqual(ready["eboot"], str(self.s.data / "inputs" / "PS3_GAME" / "USRDIR" / "EBOOT.elf"))
        self.assertEqual(pipeline.status(self.s.data, "1.0")["state"], "ready")
        self.assertEqual(pipeline.status(self.s.data, "2.0")["state"], "outdated")
        (self.s.data / "game" / "des_game.dll").write_bytes(b"MZ")
        self.assertEqual(pipeline.status(self.s.data, "1.0")["state"], "damaged")

    def test_a_run_that_fails_leaves_no_ready_file(self):
        self.assertEqual(self.s.run(phases=PHASES + ["compile"], compile_fn=fake_compile)[0], 0)

        def broken(data, jr, jobs, progress):
            raise ValueError("a bug")

        rc, lines = self.s.run(phases=PHASES + ["compile"], compile_fn=broken)
        self.assertEqual(rc, 1)
        self.assertIn("@error internal compile Setup stopped on an unexpected error.", lines)
        self.assertIsNone(pipeline.read_ready(self.s.data))
        self.assertEqual(pipeline.status(self.s.data)["state"], "partial")

    def test_status_interrupted_and_missing(self):
        self.assertEqual(pipeline.status(self.s.data)["state"], "missing")
        jr = journal.Journal(self.s.data / "setup" / "journal.json")
        jr.data["steps"]["ppu"] = {"state": "running", "inputs": "x"}
        jr._save()
        self.assertEqual(pipeline.status(self.s.data)["state"], "interrupted")


# A stand-in for `des_host --shaders <game folder>` (port/des_prework.cpp's des_prework_shaders): it writes its
# arguments beside itself, then prints the lines that mode.txt there asks for and exits with its status.
FAKE_HOST = r'''
import os, sys
from pathlib import Path
here = Path(__file__).resolve().parent
(here / "args.txt").write_text("\n".join(sys.argv[1:]), encoding="utf-8")
ready = os.environ.get("FAKE_READY", "")
(here / "ready_seen.txt").write_text(str(bool(ready) and Path(ready).exists()), encoding="utf-8")
mode = (here / "mode.txt").read_text(encoding="utf-8").strip()
print("[des] shaders: a line for the log", file=sys.stderr, flush=True)
if mode == "done":
    print("shaders 10 100", flush=True)
    print("shaders 100 100", flush=True)
    print("shaders done 90 10 0 1.5", flush=True)
elif mode == "up-to-date":
    print("shaders up-to-date 0 0 0 0.0", flush=True)
elif mode == "failed":
    print("shaders 50 100", flush=True)
    print("shaders failed 40 0 10 2.0", flush=True)
    sys.exit(1)
elif mode == "crash":
    sys.exit(3)
'''


class ShadersTest(unittest.TestCase):
    """The shaders phase (docs/52 section 5, docs/88 section 13): the host's `--shaders` pass after the compile."""

    def setUp(self):
        self.s = Setup()
        self._free = pipeline.free_bytes
        pipeline.free_bytes = lambda p: 1 << 50
        self.host = Path(self.s.game.tmp.name) / "host"
        self.host.mkdir()
        (self.host / "des_host.py").write_text(FAKE_HOST, encoding="utf-8")

    def tearDown(self):
        pipeline.free_bytes = self._free
        self.s.close()

    def run_with(self, mode, host="fake"):
        (self.host / "mode.txt").write_text(mode, encoding="utf-8")
        cmd = [sys.executable, str(self.host / "des_host.py")] if host == "fake" else host
        os.environ["FAKE_READY"] = str(self.s.data / "setup" / pipeline.READY)
        try:
            return self.s.run(phases=PHASES + ["compile", "shaders"], compile_fn=fake_compile, host=cmd)
        finally:
            os.environ.pop("FAKE_READY", None)

    def test_runs_the_host_after_the_compile(self):
        rc, lines = self.run_with("done")
        self.assertEqual(rc, 0, lines)
        self.assertEqual((self.host / "args.txt").read_text(encoding="utf-8").split("\n"),
                         ["--shaders", str(self.s.game.root)])
        for want in ("@plan check fetch stage lift compile shaders", "@step shaders start", "@progress shaders 10/100",
                     "@progress shaders 100/100", "@step shaders done", "@done ok"):
            self.assertIn(want, lines)
        # (the fake compile runs no journal step: the phase reads as skipped)
        self.assertLess(lines.index("@step compile skip"), lines.index("@step shaders start"))
        self.assertTrue(any("90 compiled, 10 already prepared" in x for x in lines), lines)
        self.assertIsNotNone(pipeline.read_ready(self.s.data))
        # ready.txt is written before the pass: one that is stopped leaves a game that plays.
        self.assertEqual((self.host / "ready_seen.txt").read_text(encoding="utf-8"), "True")
        log = (self.s.data / "setup" / "logs" / "shaders.log").read_text(encoding="utf-8")
        self.assertIn("--shaders", log)
        self.assertIn("[des] shaders: a line for the log", log)

    def test_cache_up_to_date_is_a_skip(self):
        rc, lines = self.run_with("up-to-date")
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step shaders skip", lines)

    def test_a_failed_pass_does_not_fail_the_run(self):
        for mode in ("failed", "crash"):
            rc, lines = self.run_with(mode)
            self.assertEqual(rc, 0, lines)
            self.assertIn("@step shaders done", lines)
            self.assertIn("@done ok", lines)
            self.assertFalse(any(x.startswith("@error") for x in lines), lines)
            self.assertTrue(any("the game prepares the rest at its start" in x for x in lines), lines)
            self.assertIsNotNone(pipeline.read_ready(self.s.data))

    def test_no_host(self):
        for host in (None, [str(self.host / "missing.exe")]):
            rc, lines = self.s.run(phases=PHASES + ["compile", "shaders"], compile_fn=fake_compile, host=host)
            self.assertEqual(rc, 0, lines)
            self.assertIn("@step shaders skip", lines)
            self.assertIsNotNone(pipeline.read_ready(self.s.data))

    def test_in_the_whole_build(self):
        self.assertEqual(pipeline.default_phases()[-2:], ["compile", "shaders"])


class LocalCompilerTest(unittest.TestCase):
    """A developer's compiler (--cc and --ld, or DES_SETUP_CC and DES_SETUP_LD) replaces the LLVM download."""
    LLVM = {"tools": [{"name": "llvm-win64", "version": "0", "url": "https://example.invalid/llvm.zip",
                       "size": None, "sha256": None}]}

    def setUp(self):
        self.s = Setup()

    def tearDown(self):
        self.s.close()

    def test_unpinned_download_fails_without_one(self):
        rc, lines = self.s.run(manifest=self.LLVM)
        self.assertEqual(rc, 1)
        self.assertIn("@error download fetch A download did not finish.", lines)

    def test_given_compiler_skips_the_download(self):
        rc, lines = self.s.run(manifest=self.LLVM, cc="bin/clang-cl", ld="bin/lld-link")
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step fetch skip", lines)

    def test_environment_compiler_skips_the_download(self):
        old = {k: os.environ.get(k) for k in ("DES_SETUP_CC", "DES_SETUP_LD")}
        os.environ.update(DES_SETUP_CC="bin/clang-cl", DES_SETUP_LD="bin/lld-link")
        try:
            rc, lines = self.s.run(manifest=self.LLVM)
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        self.assertEqual(rc, 0, lines)
        self.assertIn("@step fetch skip", lines)


class EmitterTest(unittest.TestCase):
    def lines(self, fn):
        out = io.StringIO()
        fn(protocol.Emitter(out))
        return out.getvalue().splitlines()

    def test_lines(self):
        def fn(e):
            e.setup()
            e.plan(["check", "lift"])
            e.step("lift", "start")
            e.progress("ppu", 1, 3)
            e.progress("ppu", 1, 3)          # the same per cent: not again
            e.progress("ppu", 3, 3)
            e.overall(0.5)
            e.error("step", "spu:a b", "It failed\non two lines.")
            e.text("@not the protocol")
            e.done("failed")
        self.assertEqual(self.lines(fn), [
            "@setup 2", "@plan check lift", "@step lift start", "@progress ppu 1/3", "@progress ppu 3/3",
            "@overall 500/1000", "@error step spu:a_b It failed on two lines.", " @not the protocol", "@done failed"])

    def test_unknown_code_refused(self):
        with self.assertRaises(AssertionError):
            self.lines(lambda e: e.error("oops", "-", "x"))

    def test_memory_lines(self):
        """@memory only when it changes, and nothing for ok at the start (docs/81 section 8)."""
        def fn(e):
            e.memory(False)
            e.memory(True)
            e.memory(True)
            e.memory(False)
        self.assertEqual(self.lines(fn), ["@memory short", "@memory ok"])

    def test_rpcs3_lines(self):
        """Version 2's lines (docs/90 3.8): @file only when the file changes, @rpcs3 with a path that has spaces."""
        def fn(e):
            e.setup()
            e.rpcs3("own", r"C:\Games\RPCS3 nightly")
            e.file("unlock", "EBOOT.BIN")
            e.file("unlock", "EBOOT.BIN")
            e.file("unlock", "chr/c0000/c0000.esd.dcx.sdat")
            e.error("rpcs3", "unlock", "RPCS3 could not unlock script/m03.luabnd.dcx.sdat.")
        self.assertEqual(protocol.VERSION, 2)
        self.assertEqual(self.lines(fn), [
            "@setup 2", r"@rpcs3 own C:\Games\RPCS3 nightly", "@file unlock EBOOT.BIN",
            "@file unlock chr/c0000/c0000.esd.dcx.sdat",
            "@error rpcs3 unlock RPCS3 could not unlock script/m03.luabnd.dcx.sdat."])
        with self.assertRaises(AssertionError):
            self.lines(lambda e: e.rpcs3("borrowed", "-"))


class HostAgreesTest(unittest.TestCase):
    """The host's C++ (port/ui/src) reads what this side writes."""

    def test_parser_knows_every_kind(self):
        cpp = (UI / "setup_protocol.cpp").read_text(encoding="utf-8")
        kinds = set(re.findall(r'\{"(\w+)",\s*Line::Kind::', cpp))
        self.assertEqual(kinds, set(protocol.KINDS))
        self.assertIn(f"kProtocol = {protocol.VERSION};", (UI / "setup_protocol.hpp").read_text(encoding="utf-8"))
        codes = set(re.findall(r'\{"(\w+)",\s*Fix::', (UI / "setup_window.cpp").read_text(encoding="utf-8")))
        self.assertEqual(codes, set(protocol.ERRORS))

    def test_every_phase_has_a_task_line(self):
        """The progress page names each phase of the plan (the lift as its two steps, ppu and spu)."""
        ids = set(re.findall(r'\{"(\w+)",\s*"[^"]+",\s*"', (UI / "setup_window.cpp").read_text(encoding="utf-8")))
        for phase in pipeline.default_phases(rpcs3_route=True, imports=True):
            for step in (["ppu", "spu"] if phase == "lift" else [phase]):
                self.assertIn(step, ids)

    def test_shaders_lines(self):
        """The host's `--shaders` lines (port/des_prework.cpp) are the ones the shaders phase reads."""
        cpp = (REAL / "port" / "des_prework.cpp").read_text(encoding="utf-8")
        self.assertIn('std::printf("shaders %u %u\\n"', cpp)
        self.assertIn('std::printf("shaders %s %u %u %u %.1f\\n"', cpp)
        words = re.search(r"kResult\[\]\s*=\s*\{([^}]*)\}", cpp).group(1)
        self.assertEqual(set(re.findall(r'"([\w-]+)"', words)), {"none", "up-to-date", "failed", "skipped", "done"})
        self.assertIn('"--shaders"', (REAL / "port" / "des_options.cpp").read_text(encoding="utf-8"))
        self.assertIn('"--host"', (UI / "setup_engine.cpp").read_text(encoding="utf-8"))

    def test_python_pin(self):
        hpp = (UI / "setup_engine.hpp").read_text(encoding="utf-8")
        e = next(t for t in fetch.load_manifest()["tools"] if t["name"] == "python-embed")

        def const(name, text=hpp):
            m = re.search(name + r'\s*=\s*"?([^";]+)"?;', text)
            self.assertIsNotNone(m, name)
            return m.group(1).rstrip("ull")

        self.assertEqual(const("kPythonName"), e["name"])
        self.assertEqual(const("kPythonVersion"), e["version"])
        self.assertEqual(const("kPythonUrl"), e["url"])
        self.assertEqual(int(const("kPythonSize")), e["size"])
        self.assertEqual(const("kPythonSha256"), e["sha256"])

    def test_first_python_lands_where_fetch_looks(self):
        """The host's own first download (setup_python.hpp): fetch.py's archive name and folders, the stamp that
        fetch.unpacked accepts, and the fetch phase's free-space figure."""
        hpp = (UI / "setup_python.hpp").read_text(encoding="utf-8")
        e = next(t for t in fetch.load_manifest()["tools"] if t["name"] == "python-embed")

        def const(name):
            m = re.search(name + r'\s*=\s*"?([^";]+)"?;', hpp)
            self.assertIsNotNone(m, name)
            return m.group(1).rstrip("ull")

        downloads, tools = const("kDownloadsFolder"), const("kToolsFolder")
        self.assertEqual((downloads, tools), pipeline.OUTPUTS["fetch"])
        # The host names the archive <name>-<version>.zip (setup_python.cpp relative_archive).
        self.assertEqual(fetch.archive_path(e, Path(downloads)), Path(downloads) / f"{e['name']}-{e['version']}.zip")
        self.assertEqual(int(const("kFetchBytes")), pipeline.NEED["fetch"])
        # The host writes the SHA-256 and "\n" in text mode: "\r\n" on Windows, "\n" elsewhere.
        for end in ("\r\n", "\n"):
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp) / f"{e['name']}-{e['version']}"
                folder.mkdir()
                (folder / const("kStampName")).write_bytes((e["sha256"] + end).encode("ascii"))
                self.assertTrue(fetch.unpacked(e, tmp), repr(end))

    def test_estimate_counts_jobs_as_the_compile(self):
        """The start screen's time estimate counts the chunks the compile can run at once (docs/81 section 8): a chunk
        at compile.py's size rule for the 53 chunks' sources (6.7 to 10.7 MB, docs/53 16.3), and its KEEP."""
        cpp = (UI / "setup_pc.cpp").read_text(encoding="utf-8")
        job = re.search(r"kJobBytes = (\d+)ull \* \(1ull << 30\) / 10;", cpp)
        kept = re.search(r"kKeptBytes = (\d+)ull << 30;", cpp)
        self.assertTrue(job and kept)

        def rule(mb):  # compile.py's _need for a PPU chunk it has not built yet
            return min(compile_step.NEED["ppu"], int(compile_step.PPU_BASE + compile_step.PPU_PER_MB * mb))

        job_bytes = int(job.group(1)) * compile_step.GB // 10
        self.assertLessEqual(rule(6.7), job_bytes)
        self.assertLessEqual(job_bytes, rule(10.7))
        self.assertEqual(int(kept.group(1)) * compile_step.GB, compile_step.KEEP)


if __name__ == "__main__":
    unittest.main()
