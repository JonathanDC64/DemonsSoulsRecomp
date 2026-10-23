"""Tests for the lift steps (des_setup/lift.py), on a made-up source tree whose
tools only write marker files (no game data, no real lifter).

  cd tools/setup && python -m unittest discover -s tests
"""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from des_setup import journal, lift  # noqa: E402

REAL = Path(__file__).resolve().parents[3]          # this source tree

# Every fake tool logs its name and arguments to <src>/calls.txt, and fails
# when <src>/fail.txt names it.
PRELUDE = '''import os, sys, json
SRC = os.path.dirname(os.path.abspath(__file__))
while not os.path.isdir(os.path.join(SRC, "scripts")): SRC = os.path.dirname(SRC)
NAME = os.path.basename(__file__)
open(os.path.join(SRC, "calls.txt"), "a").write(NAME + " " + " ".join(sys.argv[1:]) + "\\n")
fail = os.path.join(SRC, "fail.txt")
if os.path.exists(fail) and open(fail).read().strip() == NAME: print("broken"); sys.exit(3)
def arg(k): return sys.argv[sys.argv.index(k) + 1]
def put(p, text="x"):
    os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write(text)
'''
TOOLS = {
    "ps3recomp/tools/ppu_loader.py":
        'o = arg("-o"); put(o + "/EBOOT.functions.json", "[]"); put(o + "/EBOOT.imports.json", "{}")',
    "ps3recomp/tools/ppu_lifter.py":
        'o = arg("--output"); put(o + "/ppu_recomp_000.cpp")\n'
        'if "--raw" not in sys.argv: put(o + "/ppu_recomp.h", "// lifted\\n")\n'
        'print("  ... lifted 5/10 functions (50%)", flush=True)',
    "ps3recomp/tools/gen_hle_nids.py": 'put(arg("--out"))',
    "ps3recomp/tools/lift_prx.py":
        'o = arg("-o"); put(o + "/libsre_image.bin"); put(o + "/libsre_functions.json", "[]")\n'
        'put(o + "/libsre_exports.json", json.dumps({"toc": "0x30039AB0"})); put(o + "/libsre_imports.json", "{}")',
    "scripts/apply_boot_patches.py":
        'h = os.path.join(sys.argv[1], "ppu_recomp.h"); open(h, "a").write("// patched\\n")',
    "scripts/build_spu.py":
        't = json.load(open(os.path.join(SRC, "scripts", "spu_table.json")))\n'
        'if "--eboot" in sys.argv:\n'
        '    for n in t["images"]: put(arg("--elf-dir") + "/" + n + ".elf")\n'
        '    for n in t["jobs"]: put(arg("--job-dir") + "/" + n + ".bin")\n'
        'if "--only" in sys.argv: put(arg("--out-dir") + "/" + arg("--only") + "/spu_recomp.c")\n'
        'put(arg("--out-dir") + "/_gen/spu_workloads.cpp")',
    "scripts/build_libsre.py":
        'put(arg("--work") + "/lifted/ppu_recomp.c"); put(arg("--glue"))',
}


class FakeTree:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.src = Path(self.tmp.name) / "src"
        for rel, body in TOOLS.items():
            p = self.src / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(PRELUDE + body + "\n")
        (self.src / "scripts" / "lift_ppu.json").write_text((REAL / "scripts" / "lift_ppu.json").read_text())
        (self.src / "scripts" / "spu_table.json").write_text(json.dumps({
            "images": {"spu_0000_at_00001000": {"size": 1, "fnv": "0x1"}},
            "jobs": {"spujob_00002000": {"vaddr": "0x00002000", "size": 1, "fnv": "0x2"}}}))
        (self.src / "port").mkdir()
        (self.src / "port" / "insn_hooks.txt").write_text("0x100=hook\n")
        self.data = Path(self.tmp.name) / "data"
        e = self.data / "inputs" / lift.EBOOT
        e.parent.mkdir(parents=True)
        e.write_bytes(b"\x7fELF")
        fw = self.data / "inputs" / "firmware" / "libsre.prx"
        fw.parent.mkdir(parents=True)
        fw.write_bytes(b"\x7fELFsre")
        self.seen = []

    def lift(self):
        jr = journal.Journal(self.data / "setup" / "journal.json")
        return lift.Lift(self.data, jr, src=self.src, workers=2,
                         progress=lambda s, d, t: self.seen.append((s, d, t)))

    def calls(self):
        p = self.src / "calls.txt"
        return [line.split(" ", 1)[0] for line in p.read_text().splitlines()] if p.exists() else []

    def reset_calls(self):
        (self.src / "calls.txt").unlink(missing_ok=True)

    def close(self):
        self.tmp.cleanup()


PPU_STEPS = ["ppu_loader.py", "ppu_lifter.py", "apply_boot_patches.py", "gen_hle_nids.py"]


class LiftTest(unittest.TestCase):
    def setUp(self):
        self.t = FakeTree()

    def tearDown(self):
        self.t.close()

    def assertCalls(self, expected):
        """The same calls; the PPU steps in their order, and the SPU steps' in theirs (the two sides run side by
        side, run_all)."""
        calls = self.t.calls()
        self.assertEqual(sorted(calls), sorted(expected))
        for side in (PPU_STEPS, ["build_spu.py", "build_libsre.py"]):
            self.assertEqual([c for c in calls if c in side], [c for c in expected if c in side])

    def test_full_then_nothing_to_do(self):
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertCalls(PPU_STEPS + ["build_spu.py", "build_spu.py", "build_spu.py", "build_libsre.py"])
        w = self.t.data / "work"
        self.assertEqual((w / "recomp" / "ppu_recomp.h").read_text(), "// lifted\n// patched\n")
        self.assertEqual((w / "recomp_raw" / "ppu_recomp.h").read_text(), "// lifted\n")
        self.assertIn(("ppu", 5, 10), self.t.seen)
        self.assertIn(("spu", 2, 2), self.t.seen)
        self.t.reset_calls()
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertEqual(self.t.calls(), [])
        self.assertEqual((w / "recomp" / "ppu_recomp.h").read_text(), "// lifted\n// patched\n")

    def test_no_firmware_no_libsre(self):
        self.t.lift().run_all("e" * 64, None)
        self.assertNotIn("build_libsre.py", self.t.calls())

    def test_libsre_image_from_the_module(self):
        # The glue gets the image's path from the game module's folder (<data>/game), not a folder of this PC.
        self.t.lift().run_all("e" * 64, "f" * 64)
        calls = (self.t.src / "calls.txt").read_text().splitlines()
        args = next(c for c in calls if c.startswith("build_libsre.py ")).split(" ")
        self.assertEqual(args[args.index("--runtime-image") + 1], "@module/../work/libsre/libsre_image.bin")

    def test_new_eboot_redoes_the_lift(self):
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.t.reset_calls()
        self.t.lift().run_all("d" * 64, "f" * 64)
        # Everything that reads the EBOOT; the NID table and libsre do not.
        self.assertCalls(PPU_STEPS + ["build_spu.py"])

    def test_changed_tool_redoes_everything(self):
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.t.reset_calls()
        p = self.t.src / "ps3recomp" / "tools" / "ppu_lifter.py"
        p.write_text(p.read_text() + "# a new release\n")
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertEqual(len(self.t.calls()), 8)

    def test_missing_output_redoes_that_step(self):
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.t.reset_calls()
        (self.t.data / "work" / "spu" / "spujob_00002000" / "spu_recomp.c").unlink()
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertEqual(self.t.calls(), ["build_spu.py"])

    def test_failure_is_recorded_then_resumes(self):
        (self.t.src / "fail.txt").write_text("ppu_lifter.py")
        with self.assertRaises(journal.StepFailed) as cm:
            self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertIn("ppu_lifter.py failed (exit 3)", cm.exception.message)
        self.assertIn("broken", cm.exception.message)
        jr = journal.Journal(self.t.data / "setup" / "journal.json")
        self.assertEqual(jr.state("loader")["state"], "done")
        self.assertEqual(jr.state("ppu")["state"], "failed")
        (self.t.src / "fail.txt").unlink()
        self.t.reset_calls()
        self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertEqual([c for c in self.t.calls() if c in PPU_STEPS][0], "ppu_lifter.py")

    def test_spu_progress_after_the_ppu_steps(self):
        # The screens show one lift row at a time: the SPU side's progress is told once the PPU side is done.
        self.t.lift().run_all("e" * 64, "f" * 64)
        steps = [s for s, _, _ in self.t.seen]
        self.assertEqual(steps, sorted(steps, key=lambda s: s != "ppu"), steps)
        self.assertEqual(self.t.seen[-1], ("spu", 2, 2))

    def test_failure_beside_the_ppu_steps(self):
        (self.t.src / "fail.txt").write_text("build_spu.py")
        with self.assertRaises(journal.StepFailed) as cm:
            self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertIn("build_spu.py failed (exit 3)", cm.exception.message)
        jr = journal.Journal(self.t.data / "setup" / "journal.json")
        self.assertEqual(jr.state("nids")["state"], "done")         # the PPU side went on to its end
        self.assertEqual(jr.state("spu-cut")["state"], "failed")

    def test_ppu_failure_stops_the_side(self):
        (self.t.src / "fail.txt").write_text("ppu_loader.py")
        with self.assertRaises(journal.StepFailed) as cm:
            self.t.lift().run_all("e" * 64, "f" * 64)
        self.assertIn("ppu_loader.py failed", cm.exception.message)  # the PPU side's failure, not a stop
        self.assertNotIn("spu", [s for s, _, _ in self.t.seen])     # nothing told for a side that stopped

    def test_missing_eboot(self):
        (self.t.data / "inputs" / lift.EBOOT).unlink()
        with self.assertRaises(journal.StepFailed):
            self.t.lift().run_all("e" * 64)


class SharedFilesTest(unittest.TestCase):
    def test_readme_matches_lift_ppu_json(self):
        flags = json.loads((REAL / "scripts" / "lift_ppu.json").read_text(encoding="utf-8"))
        readme = (REAL / "README.md").read_text(encoding="utf-8")
        m = re.search(r'--override-funcs "([^"]+)"', readme)
        self.assertIsNotNone(m, "the README has no --override-funcs")
        self.assertEqual(m.group(1), ",".join(flags["override_funcs"]))
        self.assertIn("--insn-hooks @" + flags["insn_hooks"], readme)
        self.assertEqual("--shadow-stack" in readme, bool(flags["shadow_stack"]))
        m = re.search(r"--chunk-lines (\d+)", readme)
        self.assertEqual(int(m.group(1)) if m else None, flags.get("chunk_lines"))

    def test_spu_table(self):
        t = json.loads((REAL / "scripts" / "spu_table.json").read_text(encoding="utf-8"))
        self.assertEqual(len(t["images"]), 12)
        for name, e in t["images"].items():
            self.assertRegex(name, r"^spu_\d{4}_at_[0-9A-F]{8}$")
            self.assertRegex(e["fnv"], r"^0x[0-9A-F]{16}$")
        for name, e in t["jobs"].items():
            self.assertEqual(name, "spujob_" + e["vaddr"][2:])
            self.assertRegex(e["fnv"], r"^0x[0-9A-F]{16}$")


if __name__ == "__main__":
    unittest.main()
