"""Tests for the compile step (des_setup/compile.py), on a made-up source tree
and lift, with a fake compiler and linker (no game data, no real compiler).

  cd tools/setup && python -m unittest discover -s tests
"""
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from des_setup import compile as cc, fetch, journal  # noqa: E402

REAL = Path(__file__).resolve().parents[3]          # this source tree

# The fake compiler: /Fo<obj> -clang:-MF<dep> ... -- <src>. It reads the
# source's quoted includes (beside it, then the -I folders), writes them as the
# dependency file and the object as the sum of what it read. A source with FAIL
# in it fails; one with OOM_ONCE runs out of memory the first time; one with
# OOM_EXIT ends with Windows' out-of-memory status and prints nothing about it.
FAKECC = r'''import hashlib, os, re, sys
a = sys.argv[1:]
src = a[a.index("--") + 1]
obj = next(x[3:] for x in a if x.startswith("/Fo"))
dep = next(x[len("-clang:-MF"):] for x in a if x.startswith("-clang:-MF"))
inc = [x[2:] for x in a if x.startswith("-I")]
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "cc_calls.txt"), "a").write(os.path.basename(obj) + "\n")
text = open(src, encoding="utf-8").read()
if "FAIL" in text:
    print("error: made to fail"); sys.exit(1)
if "OOM_EXIT" in text:
    sys.exit(-1073741801)   # 0xC0000017 as a signed exit status
mark = obj + ".oom"
if "OOM_ONCE" in text and not os.path.exists(mark):
    open(mark, "w").close(); print("LLVM ERROR: out of memory"); sys.exit(1)
deps, h = [], hashlib.sha256(text.encode())
for name in re.findall(r'#include "([^"]+)"', text):
    for d in [os.path.dirname(src)] + inc:
        p = os.path.normpath(os.path.join(d, name))
        if os.path.isfile(p):
            deps.append(p); h.update(open(p, "rb").read()); break
    else:
        print(f"error: {name} not found"); sys.exit(1)
open(obj, "w").write(h.hexdigest())
open(dep, "w").write(obj.replace(" ", "\\ ") + ": " + " \\\n  ".join(x.replace(" ", "\\ ") for x in [src] + deps) + "\n")
'''
FAKELD = r'''import os, sys
a = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "ld_calls.txt"), "a").write("link\n")
out = next(x[5:] for x in a if x.lower().startswith("/out:"))
objs = [x for x in a if x.endswith(".obj")]
open(out, "w").write("\n".join(os.path.basename(o) + " " + open(o).read() for o in objs))
'''

DESC = {
    "version": 1, "abi": 1,
    "objects": [
        {"name": "ppu", "glob": "{work}/recomp/ppu_recomp_*.cpp", "args": ["/O2", "-I{work}/recomp", "-I{src}/inc"]},
        {"name": "spu", "glob": "{work}/spu/*/spu_recomp.c", "args": ["/O2", "-I{src}/inc"]},
        {"name": "spu_workloads", "src": "{gen}/spu_workloads.cpp", "args": ["-I{src}/inc"]},
        {"name": "des_game_module", "src": "{src}/port/des_game_module.cpp", "args": ["-I{gen}", "-I{src}/inc"]},
    ],
    "link": ["/nologo", "{objects}", "/out:des_game.dll", "/dll"],
    "dll": "des_game.dll",
    "headers": ["{src}/inc/rt.h"],
}

WORKLOADS = b'''/* GENERATED */
#include "../../runtime/spu/spu_workload.h"
extern "C" void des_spu_register_all(void) {}
__attribute__((constructor)) static void des_spu_autoreg(void) { des_spu_register_all(); }
'''


class Tree:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.src, self.data, self.tools = root / "src with space", root / "data", root / "tools"
        for rel, text in {
            "inc/rt.h": "// runtime header\n",
            "ps3recomp/runtime/spu/spu_workload.h": "// the workload registry\n",
            "port/des_game_module.cpp": '#include "des_game_title_funcs.h"\n#include "rt.h"\n',
            "port/insn_hooks.txt": "# hooks\n0x00100000 = des_hook_a\n  0x00100004=des_hook_b ; note\n",
            "scripts/lift_ppu.json": json.dumps({"override_funcs": ["0x001=des_over", "0x002 = des_hook_a"]}),
        }.items():
            self.put(self.src / rel, text)
        for rel, text in {
            "work/recomp/ppu_recomp.h": "// lifted header\n",
            "work/recomp/ppu_recomp_000.cpp": '#include "ppu_recomp.h"\n#include "rt.h"\n' + "x" * 5000,
            "work/recomp/ppu_recomp_001.cpp": '#include "ppu_recomp.h"\n',
            "work/spu/spu_0000_at_01760280/spu_recomp.c": '#include "rt.h"\n',
            "work/spu/spujob_0177B480/spu_recomp.c": "// job\n",
        }.items():
            self.put(self.data / rel, text)
        (self.data / "work/spu/_gen").mkdir(parents=True)
        (self.data / "work/spu/_gen/spu_workloads.cpp").write_bytes(WORKLOADS)
        self.put(self.tools / "fakecc.py", FAKECC)
        self.put(self.tools / "fakeld.py", FAKELD)
        self.put(self.tools / "desc.json", json.dumps(DESC))
        self.jpath = self.data / "setup" / "journal.json"

    @staticmethod
    def put(p, text):
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def make(self, jobs=2, desc=None):
        if desc is not None:
            self.put(self.tools / "desc.json", json.dumps(desc))
        jr = journal.Journal(self.jpath)
        return cc.Compile(self.data, jr, [sys.executable, str(self.tools / "fakecc.py")],
                          [sys.executable, str(self.tools / "fakeld.py")], src=self.src,
                          desc=self.tools / "desc.json", jobs=jobs)

    def calls(self, name):
        p = self.tools / f"{name}_calls.txt"
        if not p.exists():
            return []
        out = p.read_text().split()
        p.unlink()
        return out


class TestPieces(unittest.TestCase):
    def test_depfile(self):
        text = "C:\\b\\x.obj: C:\\src\\a.c \\\n  C:\\Program\\ Files\\inc\\b.h C:/c$$d.h \\\r\n  rel/e.h\n"
        self.assertEqual(cc.parse_depfile(text),
                         ["C:\\src\\a.c", "C:\\Program Files\\inc\\b.h", "C:/c$d.h", "rel/e.h"])
        self.assertEqual(cc.parse_depfile("x.obj:\n"), [])

    def test_jobs(self):
        self.assertEqual(cc.default_jobs(16, 31 * cc.GB), 9)      # (31 - 2) / 3
        self.assertEqual(cc.default_jobs(4, 31 * cc.GB), 4)
        self.assertEqual(cc.default_jobs(16, 3 * cc.GB), 1)
        self.assertEqual(cc.default_jobs(16, 0), 8)               # unknown: half the CPUs

    def test_workloads(self):
        out = cc.split_workloads(WORKLOADS, Path("C:/inst/ps3recomp"))
        self.assertIn(b'#include "C:/inst/ps3recomp/runtime/spu/spu_workload.h"', out)
        self.assertIn(b"[[maybe_unused]] static void des_spu_autoreg(void)", out)
        self.assertNotIn(b"constructor", out)
        with self.assertRaises(journal.StepFailed):
            cc.split_workloads(WORKLOADS.replace(b"static void des_spu_autoreg", b"static void other"), Path("C:/i"))

    def test_title_funcs(self):
        t = Tree()
        self.addCleanup(t.tmp.cleanup)
        text = cc.title_funcs(t.src)
        self.assertTrue(text.endswith("#pragma once\n#define DES_GAME_TITLE_FUNCS(X) \\\n    X(des_over) \\\n"
                                      "    X(des_hook_a) \\\n    X(des_hook_b)\n"), text)

    def test_title_funcs_match_cmake(self):
        """The same names, in the same order, as port/CMakeLists.txt's split
        section takes from this tree's lift flags and hooks."""
        cm = (REAL / "port" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('"=[ \\t]*([A-Za-z_][A-Za-z0-9_]*)[ \\t]*$"', cm)
        self.assertIn('"^[ \\t]*0x[0-9A-Fa-f]+[ \\t]*=[ \\t]*([A-Za-z_][A-Za-z0-9_]*)"', cm)
        self.assertIn('"^[ \\t]*0x[0-9A-Fa-f]+[ \\t]*="', cm)
        names = re.findall(r"X\((\w+)\)", cc.title_funcs(REAL))
        self.assertGreater(len(names), 10)
        self.assertEqual(len(names), len(set(names)))


class TestDescription(unittest.TestCase):
    """scripts/game_build_desc.py's pieces (the whole script needs a split build)."""
    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location("game_build_desc", REAL / "scripts" / "game_build_desc.py")
        cls.g = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.g)

    def test_split(self):
        s = self.g.split_cmdline
        self.assertEqual(s(r'a  "b c" d\e "f\"g" h\\"i j"'), ["a", "b c", r"d\e", 'f"g', r"h\i j"])
        self.assertEqual(s('x ""'), ["x", ""])

    def test_roots(self):
        r = self.g.Roots([("C:\\t\\port\\..", "{src}"), ("C:\\t\\port", "{src}/port"), ("D:\\l\\ps3r", "{src}/ps3recomp"),
                          ("D:\\l\\ps3r\\work_spu", "{work}/spu"), ("C:\\t\\port\\gen", "{work}/gen")])
        self.assertEqual(r.to_ph(r"-ID:\L\PS3R\include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph(r"D:/l/ps3r/work_spu\spu_0000\spu_recomp.c"), "{work}/spu/spu_0000/spu_recomp.c")
        self.assertEqual(r.to_ph(r"C:\t\port\gen\libsre_glue.c"), "{work}/gen/libsre_glue.c")
        self.assertEqual(r.to_ph(r"-IC:\t\port\compat"), "-I{src}/port/compat")
        self.assertEqual(r.to_ph(r"C:\t\portable\x.h"), "{src}/portable/x.h")       # not port's: a prefix only
        self.assertEqual(r.to_ph(r"E:\elsewhere\x.h"), r"E:\elsewhere\x.h")
        self.assertEqual(r.to_ph(r"C:\t\scripts\a.json"), "{src}/scripts/a.json")
        self.assertEqual(os.path.normcase(r.from_ph("-I{src}/ps3recomp/include")), os.path.normcase(r"-ID:\l\ps3r/include"))

    def test_kinds(self):
        k = self.g.kind_of
        self.assertEqual(k("{work}/recomp/ppu_recomp_013.cpp"), ("ppu", "{work}/recomp/ppu_recomp_*.cpp"))
        self.assertEqual(k("{work}/spu/spujob_0177B480/spu_recomp.c"), ("spu", "{work}/spu/*/spu_recomp.c"))
        self.assertEqual(k("{work}/libsre/lifted/ppu_recomp.c"), ("libsre", None))
        self.assertEqual(k("{src}/port/des_game_module.cpp"), ("des_game_module", None))


class TestCompile(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.tmp.cleanup)

    def test_plan(self):
        c = self.t.make()
        c._gen()
        names = [o.name for o in c.plan()]
        self.assertEqual(names, ["ppu_recomp_000", "ppu_recomp_001", "spu_0000_at_01760280", "spujob_0177B480",
                                 "spu_workloads", "des_game_module"])
        o = c.plan()[0]
        self.assertEqual(o.args, ["/O2", "-I" + (self.t.data / "work/recomp").as_posix(),
                                  "-I" + (self.t.src / "inc").as_posix()])

    def test_build_then_nothing_to_do(self):
        dll = self.t.make().run_all()
        self.assertTrue(dll.is_file())
        self.assertEqual(dll, self.t.data / "game" / "des_game.dll")
        self.assertEqual(len(self.t.calls("cc")), 6)
        self.assertEqual(self.t.calls("ld"), ["link"])
        self.assertEqual(len(dll.read_text().splitlines()), 6)
        gen = (self.t.data / "build/gen/spu_workloads.cpp").read_bytes()
        self.assertNotIn(b"constructor", gen)
        self.t.make().run_all()
        self.assertEqual(self.t.calls("cc"), [])
        self.assertEqual(self.t.calls("ld"), [])
        steps = json.loads(self.t.jpath.read_text())["steps"]
        self.assertEqual(steps["link"]["state"], "done")
        self.assertIn(str(self.t.src / "inc" / "rt.h"), steps["cc:ppu_recomp_000"]["results"]["deps"])

    def test_header_change_rebuilds_its_readers(self):
        self.t.make().run_all()
        self.t.calls("cc"), self.t.calls("ld")
        Tree.put(self.t.data / "work/recomp/ppu_recomp.h", "// lifted header, changed\n")
        self.t.make().run_all()
        self.assertEqual(sorted(self.t.calls("cc")), ["ppu_recomp_000.obj", "ppu_recomp_001.obj"])
        self.assertEqual(self.t.calls("ld"), ["link"])
        Tree.put(self.t.src / "inc/rt.h", "// runtime header, changed\n")
        self.t.make().run_all()
        self.assertEqual(sorted(self.t.calls("cc")),
                         ["des_game_module.obj", "ppu_recomp_000.obj", "spu_0000_at_01760280.obj"])

    def test_table_change_rebuilds_the_module(self):
        self.t.make().run_all()
        self.t.calls("cc")
        Tree.put(self.t.src / "port/insn_hooks.txt", "0x00100000 = des_hook_a\n0x00100008 = des_hook_c\n")
        self.t.make().run_all()
        self.assertEqual(self.t.calls("cc"), ["des_game_module.obj"])

    def test_failure_then_resume(self):
        Tree.put(self.t.data / "work/spu/spujob_0177B480/spu_recomp.c", "FAIL\n")
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make(jobs=1).run_all()
        self.assertEqual(e.exception.step, "cc:spujob_0177B480")
        self.assertIn("made to fail", e.exception.message)
        self.assertTrue(Path(e.exception.log).is_file())
        self.assertFalse((self.t.data / "game" / "des_game.dll").exists())
        first = self.t.calls("cc")
        Tree.put(self.t.data / "work/spu/spujob_0177B480/spu_recomp.c", "// fixed\n")
        self.t.make(jobs=1).run_all()
        again = self.t.calls("cc")
        self.assertIn("spujob_0177B480.obj", again)
        self.assertEqual(set(first) & set(again) - {"spujob_0177B480.obj"}, set())

    def test_out_of_memory_retried_once(self):
        Tree.put(self.t.data / "work/recomp/ppu_recomp_001.cpp", "OOM_ONCE\n")
        self.t.make(jobs=3).run_all()
        self.assertEqual(self.t.calls("cc").count("ppu_recomp_001.obj"), 2)

    @unittest.skipUnless(os.name == "nt", "Windows' exit status")
    def test_out_of_memory_by_exit_status_says_so(self):
        """Only the exit status tells: tried once more, then the message says out
        of memory, so `run` reports the memory error (pipeline.OOM)."""
        Tree.put(self.t.data / "work/recomp/ppu_recomp_001.cpp", "OOM_EXIT\n")
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make(jobs=2).run_all()
        self.assertEqual(self.t.calls("cc").count("ppu_recomp_001.obj"), 2)
        self.assertIn("out of memory", e.exception.message)

    def test_no_source_lines_in_messages(self):
        self.t.make(jobs=1).run_all()
        log = (self.t.data / "setup/logs/cc_ppu_recomp_000.log").read_text(encoding="utf-8")
        self.assertIn("-clang:-fno-caret-diagnostics", log)

    def test_toolchain_given(self):
        self.assertEqual(cc.toolchain(self.t.data, "bin/clang-cl", "bin/lld-link"),
                         (["bin/clang-cl"], Path("bin/lld-link")))
        with self.assertRaises(fetch.FetchError):
            cc.toolchain(self.t.data, "bin/clang-cl", None)

    def test_missing_header_in_install(self):
        (self.t.src / "inc/rt.h").unlink()
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make().run_all()
        self.assertIn("missing 1 header", e.exception.message)

    def test_no_lift(self):
        import shutil
        shutil.rmtree(self.t.data / "work/recomp")
        c = self.t.make()
        c._gen()
        with self.assertRaises(journal.StepFailed) as e:
            c.plan()
        self.assertIn("run the lift first", e.exception.message)

    def test_real_description(self):
        """The checked-in description: every path is a placeholder, and it names
        the objects CMake's split build has."""
        d = json.loads(cc.DESC.read_text(encoding="utf-8"))
        names = [o["name"] for o in d["objects"]]
        for n in ("ppu", "spu", "libsre", "libsre_glue", "spu_workloads", "game_module", "des_game_module"):
            self.assertIn(n, names)
        for o in d["objects"]:
            for a in o["args"] + [o.get("src", ""), o.get("glob", "")]:
                self.assertNotRegex(a, r"[A-Za-z]:[\\/]|\\\\", a)
        self.assertIn("{objects}", d["link"])
        self.assertTrue(all(h.startswith("{src}/") for h in d["headers"]))


if __name__ == "__main__":
    unittest.main()
