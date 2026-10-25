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

# The fake compiler: /Fo<obj> -clang:-MF<dep> ... -- <src>, or as clang on
# Linux, -o <obj> -MF <dep> ... -c <src>. It reads the
# source's quoted includes (beside it, then the -I folders), writes them as the
# dependency file and the object as the sum of what it read. A source with FAIL
# in it fails; one with OOM_ONCE runs out of memory the first time; one with
# OOM_EXIT ends with Windows' out-of-memory status and prints nothing about it.
# SLEEP=<s> makes it take that long. Each call is noted in a file of its own in
# cc_calls (ld_calls for the linker), and its start and end in cc_times:
# appends from processes running side by side can lose lines on Windows.
NOTE = r'''
def note(folder, text):
    os.makedirs(folder, exist_ok=True)
    fd, _ = tempfile.mkstemp(dir=folder, suffix=".txt")
    os.write(fd, text.encode()); os.close(fd)
'''
FAKECC = r'''import hashlib, os, re, sys, tempfile, time
''' + NOTE + r'''
t0 = time.time()
t0_ns = time.time_ns()
a = sys.argv[1:]
if "--" in a:
    src = a[a.index("--") + 1]
    obj = next(x[3:] for x in a if x.startswith("/Fo"))
    dep = next(x[len("-clang:-MF"):] for x in a if x.startswith("-clang:-MF"))
else:
    src, obj, dep = a[a.index("-c") + 1], a[a.index("-o") + 1], a[a.index("-MF") + 1]
inc = [x[2:] for x in a if x.startswith("-I")]
here = os.path.dirname(os.path.abspath(__file__))
note(os.path.join(here, "cc_calls"), f"{t0_ns} {os.path.basename(obj)}")
text = open(src, encoding="utf-8").read()
m = re.search(r"SLEEP=([0-9.]+)", text)
if m:
    time.sleep(float(m.group(1)))
note(os.path.join(here, "cc_times"), f"{os.path.basename(obj)} {t0} {time.time()}\n")
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
FAKELD = r'''import os, sys, tempfile, time
''' + NOTE + r'''
t0_ns = time.time_ns()
a = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
if "-o" in a:   # the clang driver linking des_game.so (Linux)
    note(os.path.join(here, "ld_calls"), f"{t0_ns} link")
    objs = [x for x in a if x.endswith(".o")]
    open(a[a.index("-o") + 1], "w").write("\n".join(os.path.basename(o) + " " + open(o).read() for o in objs))
    sys.exit(0)
out = next(x[5:] for x in a if x.lower().startswith("/out:"))
if a[0] == "/lib":   # an import library from a .def file; one with FAIL in it fails
    note(os.path.join(here, "ld_calls"), f"{t0_ns} lib")
    text = open(next(x[5:] for x in a if x.lower().startswith("/def:"))).read()
    if "FAIL" in text:
        print("error: made to fail"); sys.exit(1)
    open(out, "w").write(text); sys.exit(0)
note(os.path.join(here, "ld_calls"), f"{t0_ns} link")
objs = [x for x in a if x.endswith(".obj") or x.endswith(".lib")]
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

    def make(self, jobs=2, desc=None, **kw):
        if desc is not None:
            self.put(self.tools / "desc.json", json.dumps(desc))
        jr = journal.Journal(self.jpath)
        return cc.Compile(self.data, jr, [sys.executable, str(self.tools / "fakecc.py")],
                          [sys.executable, str(self.tools / "fakeld.py")], src=self.src,
                          desc=self.tools / "desc.json", jobs=jobs, **kw)

    def times(self):
        """Each compiler call's (object, start, end), from cc_times (a file each: appends from processes running
        side by side can mix on Windows)."""
        out = []
        for p in sorted((self.tools / "cc_times").glob("*.txt")):
            n, a, b = p.read_text().split()
            out.append((n, float(a), float(b)))
            p.unlink()
        return out

    def calls(self, name):
        """The tool's calls in the order they started, then forgotten (a file each, as cc_times)."""
        rows = []
        for p in (self.tools / f"{name}_calls").glob("*.txt"):
            t, text = p.read_text().split(" ", 1)
            rows.append((int(t), text))
            p.unlink()
        return [text for _, text in sorted(rows)]


class TestPieces(unittest.TestCase):
    def test_depfile(self):
        text = "C:\\b\\x.obj: C:\\src\\a.c \\\n  C:\\Program\\ Files\\inc\\b.h C:/c$$d.h \\\r\n  rel/e.h\n"
        self.assertEqual(cc.parse_depfile(text),
                         ["C:\\src\\a.c", "C:\\Program Files\\inc\\b.h", "C:/c$d.h", "rel/e.h"])
        self.assertEqual(cc.parse_depfile("x.obj:\n"), [])

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

    @unittest.skipIf(os.name == "nt", "POSIX paths")
    def test_roots_posix(self):
        r = self.g.Roots([("/t/port/..", "{src}"), ("/t/port", "{src}/port"), ("/l/ps3r", "{src}/ps3recomp"),
                          ("/l/ps3r/work_spu", "{work}/spu"), ("/t/port/gen", "{work}/gen")])
        self.assertEqual(r.to_ph("-I/l/ps3r/include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph("/l/ps3r/work_spu/spu_0000/spu_recomp.c"), "{work}/spu/spu_0000/spu_recomp.c")
        self.assertEqual(r.to_ph("/t/port/gen/libsre_glue.c"), "{work}/gen/libsre_glue.c")
        self.assertEqual(r.to_ph("/t/portable/x.h"), "{src}/portable/x.h")       # not port's: a prefix only
        self.assertEqual(r.to_ph("/elsewhere/x.h"), "/elsewhere/x.h")
        self.assertEqual(r.from_ph("-I{src}/ps3recomp/include"), "-I/l/ps3r/include")
        # ps3recomp beside port/, where CMake's default PS3RECOMP_DIR names it port/../ps3recomp.
        r = self.g.Roots([("/t/port/..", "{src}"), ("/t/port", "{src}/port"), ("/t/port/../ps3recomp", "{src}/ps3recomp")])
        self.assertEqual(r.to_ph("-I/t/port/../ps3recomp/include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph("-I/t/ps3recomp/include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph("/t/port/des_game_module.cpp"), "{src}/port/des_game_module.cpp")

    @unittest.skipUnless(os.name == "nt", "Windows paths")
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
        # ps3recomp beside port/, where CMake's default PS3RECOMP_DIR names it port/../ps3recomp.
        r = self.g.Roots([("C:\\t\\port\\..", "{src}"), ("C:\\t\\port", "{src}/port"),
                          ("C:/t/port/../ps3recomp", "{src}/ps3recomp")])
        self.assertEqual(r.to_ph("-IC:/t/port/../ps3recomp/include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph("/FIC:/t/port/../ps3recomp/include/x.h"), "/FI{src}/ps3recomp/include/x.h")
        self.assertEqual(r.to_ph(r"-IC:\t\ps3recomp\include"), "-I{src}/ps3recomp/include")
        self.assertEqual(r.to_ph(r"C:\t\port\des_game_module.cpp"), "{src}/port/des_game_module.cpp")

    def test_kinds(self):
        k = self.g.kind_of
        self.assertEqual(k("{work}/recomp/ppu_recomp_013.cpp"), ("ppu", "{work}/recomp/ppu_recomp_*.cpp"))
        self.assertEqual(k("{work}/spu/spujob_0177B480/spu_recomp.c"), ("spu", "{work}/spu/*/spu_recomp.c"))
        self.assertEqual(k("{work}/libsre/lifted/ppu_recomp.c"), ("libsre", None))
        self.assertEqual(k("{src}/port/des_game_module.cpp"), ("des_game_module", None))

    def test_lib_command(self):
        """The import library's command as ninja writes it for DesGameFreestanding.cmake's custom command."""
        t = self.g.lib_tokens
        line = (r'C:\WINDOWS\system32\cmd.exe /C "cd /D C:\b\build && "C:\Program Files\LLVM\bin\lld-link.exe" '
                r'/lib /nologo /machine:x64 /def:C:/b/ps3r/runtime/ppu/game_libc_kernel32.def '
                r'/out:C:/b/build/split_gen/game_libc_kernel32.lib"')
        self.assertEqual(t(line), ["/lib", "/nologo", "/machine:x64", "/def:C:/b/ps3r/runtime/ppu/game_libc_kernel32.def",
                                   "/out:C:/b/build/split_gen/game_libc_kernel32.lib"])
        self.assertEqual(t(line[:-1] + ' && cd ."')[-1], "/out:C:/b/build/split_gen/game_libc_kernel32.lib")
        self.assertIsNone(t(r'C:\PROGRA~1\LLVM\bin\lld-link.exe /nologo a.obj /out:des_game.dll /dll'))
        self.assertIsNone(t(r'C:\PROGRA~1\LLVM\bin\clang-cl.exe /nologo /lib -c -- x.c'))

    def test_linux_compile_command(self):
        """A Linux compile line as ninja writes it (docs/87): the object, dependency and source words go."""
        r = self.g.Roots([("/l/port/port/..", "{src}"), ("/l/port/port", "{src}/port"), ("/l/ps3r", "{src}/ps3recomp"),
                          ("/l/port/work/recomp", "{work}/recomp")])
        line = ("/usr/bin/clang++ -DPS3_GAME_FREESTANDING -I/l/port/work/recomp -I/l/ps3r/include -O2 -fwrapv "
                "-nostdlibinc -include/l/ps3r/include/ps3emu/game_freestanding.h -fPIC -MD -MT "
                "CMakeFiles/des_game.dir/l/port/work/recomp/ppu_recomp_000.cpp.o -MF "
                "CMakeFiles/des_game.dir/l/port/work/recomp/ppu_recomp_000.cpp.o.d -o "
                "CMakeFiles/des_game.dir/l/port/work/recomp/ppu_recomp_000.cpp.o -c /l/port/work/recomp/ppu_recomp_000.cpp")
        args, src, want = self.g.template_elf(line, r)
        self.assertEqual(src, "{work}/recomp/ppu_recomp_000.cpp")
        self.assertEqual(args, ["-DPS3_GAME_FREESTANDING", "-I{work}/recomp", "-I{src}/ps3recomp/include", "-O2",
                                "-fwrapv", "-nostdlibinc", "-include{src}/ps3recomp/include/ps3emu/game_freestanding.h",
                                "-fPIC"])
        self.assertEqual(want[-1], "/l/port/work/recomp/ppu_recomp_000.cpp")
        self.assertTrue(self.g.ABS_ELF.search("-I/usr/include"))
        self.assertTrue(self.g.ABS_ELF.search("/usr/include/stdio.h"))
        self.assertFalse(self.g.ABS_ELF.search("-I{src}/ps3recomp/include"))
        self.assertFalse(self.g.ABS_ELF.search("-Wl,-z,defs"))
        self.assertEqual(self.g.system_of({"CMAKE_C_COMPILER": "/usr/bin/clang"}), "linux")
        self.assertEqual(self.g.system_of({"CMAKE_C_COMPILER": "C:/LLVM/bin/clang-cl.exe"}), "windows")

    def test_linux_link_command(self):
        r = self.g.Roots([("/l/port/port/..", "{src}"), ("/l/port/port", "{src}/port")])
        line = (": && /usr/bin/clang++ -fPIC -nostdlib -fuse-ld=lld -Wl,-z,defs -Wl,-Map=des_game.map -shared "
                "-Wl,-soname,des_game.so -o des_game.so CMakeFiles/des_game.dir/a.cpp.o CMakeFiles/des_game.dir/b.c.o "
                "&& :")
        largs, out = self.g.link_elf(line, "/b", r)
        self.assertEqual(out, "des_game.so")
        self.assertEqual(largs, ["-fPIC", "-nostdlib", "-fuse-ld=lld", "-Wl,-z,defs", "-Wl,-Map=des_game.map", "-shared",
                                 "-Wl,-soname,des_game.so", "-o", "des_game.so", "{objects}"])


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

    def test_linux_description(self):
        """game_build_linux.json's shape (docs/87): clang's spellings, .o objects, the clang driver linking
        des_game.so with -o, no import libraries."""
        desc = dict(DESC, system="linux", libs=[], link=["-shared", "-nostdlib", "-fuse-ld=lld", "-o", "des_game.so",
                                                         "{objects}"], dll="des_game.so")
        desc["objects"] = [dict(o, args=[a.replace("/O2", "-O2") for a in o["args"]]) for o in DESC["objects"]]
        so = self.t.make(desc=desc).run_all()
        self.assertEqual(so, self.t.data / "game" / "des_game.so")
        self.assertTrue((self.t.data / "build/obj/ppu_recomp_000.o").is_file())
        self.assertTrue((self.t.data / "build/obj/ppu_recomp_000.o.d").is_file())
        self.assertEqual(len(so.read_text().splitlines()), 6)
        self.assertEqual(self.t.calls("ld"), ["link"])
        log = (self.t.data / "setup/logs/cc_ppu_recomp_000.log").read_text()
        self.assertIn(" -MF ", log)
        self.assertNotIn("/Fo", log)

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

    def test_toolchain_from_environment(self):
        old = {k: os.environ.get(k) for k in ("DES_SETUP_CC", "DES_SETUP_LD")}
        try:
            os.environ.update(DES_SETUP_CC="bin/clang-cl", DES_SETUP_LD="bin/lld-link")
            self.assertEqual(cc.toolchain(self.t.data), (["bin/clang-cl"], Path("bin/lld-link")))
            self.assertEqual(cc.toolchain(self.t.data, "x/clang-cl", "x/lld-link"),
                             (["x/clang-cl"], Path("x/lld-link")))     # the arguments win
            os.environ["DES_SETUP_LD"] = ""
            with self.assertRaises(fetch.FetchError):
                cc.toolchain(self.t.data)
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_missing_header_in_install(self):
        (self.t.src / "inc/rt.h").unlink()
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make().run_all()
        self.assertIn("missing 1 header", e.exception.message)

    LIBS = dict(DESC, libs=[{"name": "k32", "args": ["/lib", "/nologo", "/def:{src}/inc/k32.def", "/out:{gen}/k32.lib"]}],
                link=DESC["link"] + ["{gen}/k32.lib"])

    def test_import_library_before_the_link(self):
        Tree.put(self.t.src / "inc/k32.def", "LIBRARY kernel32.dll\nEXPORTS\n  GetCurrentThreadId\n")
        dll = self.t.make(desc=self.LIBS).run_all()
        self.assertEqual(self.t.calls("ld"), ["lib", "link"])
        self.assertEqual(len(self.t.calls("cc")), 6)
        self.assertIn("GetCurrentThreadId", (self.t.data / "build/gen/k32.lib").read_text())
        self.assertIn("k32.lib", dll.read_text())
        self.t.make(desc=self.LIBS).run_all()
        self.assertEqual(self.t.calls("ld"), [])
        # A changed .def makes the library and the DLL again, and compiles nothing.
        Tree.put(self.t.src / "inc/k32.def", "LIBRARY kernel32.dll\nEXPORTS\n  GetCurrentThreadId\n  Sleep\n")
        self.t.make(desc=self.LIBS).run_all()
        self.assertEqual(self.t.calls("ld"), ["lib", "link"])
        self.assertEqual(self.t.calls("cc"), [])
        steps = json.loads(self.t.jpath.read_text())["steps"]
        self.assertEqual(steps["lib:k32"]["state"], "done")

    def test_import_library_def_missing(self):
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make(desc=self.LIBS).run_all()
        self.assertEqual(e.exception.step, "lib:k32")
        self.assertIn("the install is missing", e.exception.message)
        self.assertEqual(self.t.calls("ld"), [])

    def test_import_library_fails(self):
        Tree.put(self.t.src / "inc/k32.def", "FAIL\n")
        with self.assertRaises(journal.StepFailed) as e:
            self.t.make(desc=self.LIBS).run_all()
        self.assertEqual(e.exception.step, "lib:k32")
        self.assertIn("failed to make k32.lib", e.exception.message)
        self.assertIn("made to fail", e.exception.message)
        self.assertEqual(self.t.calls("ld"), ["lib"])

    def test_no_lift(self):
        import shutil
        shutil.rmtree(self.t.data / "work/recomp")
        c = self.t.make()
        c._gen()
        with self.assertRaises(journal.StepFailed) as e:
            c.plan()
        self.assertIn("run the lift first", e.exception.message)

    def test_real_linux_description(self):
        """The checked-in Linux description (docs/87): every path is a placeholder, it names the objects CMake's split
        build has, each built freestanding, and the link has no system library."""
        d = json.loads(cc.DESC.with_name("game_build_linux.json").read_text(encoding="utf-8"))
        self.assertEqual(d["system"], "linux")
        names = [o["name"] for o in d["objects"]]
        for n in ("ppu", "spu", "libsre", "libsre_glue", "spu_workloads", "game_module", "des_game_module",
                  "game_libc"):
            self.assertIn(n, names)
        absolute = re.compile(r"(?:^|^-[A-Za-z][A-Za-z-]*|[=:,])/")
        for o in d["objects"]:
            for a in o["args"] + [o.get("src", ""), o.get("glob", "")]:
                self.assertNotRegex(a, absolute, a)
            for a in ("-nostdlibinc", "-fvisibility=hidden", "-ftls-model=initial-exec", "-fno-stack-protector",
                      "-fPIC", "-DPS3_GAME_FREESTANDING"):
                self.assertIn(a, o["args"], o["name"])
        for a in ("-nostdlib", "-fuse-ld=lld", "-Wl,-z,defs", "-shared", "{objects}"):
            self.assertIn(a, d["link"])
        self.assertFalse([a for a in d["link"] if a.startswith("-l")], "a system library by name")
        self.assertEqual(d["dll"], "des_game.so")
        self.assertEqual(d["libs"], [])
        self.assertTrue(all(h.startswith("{src}/") for h in d["headers"]))

    def test_real_description(self):
        """The checked-in Windows description: every path is a placeholder, and it names
        the objects CMake's split build has."""
        d = json.loads(cc.DESC.with_name("game_build.json").read_text(encoding="utf-8"))
        names = [o["name"] for o in d["objects"]]
        for n in ("ppu", "spu", "libsre", "libsre_glue", "spu_workloads", "game_module", "des_game_module",
                  "game_libc"):
            self.assertIn(n, names)
        for o in d["objects"]:
            for a in o["args"] + [o.get("src", ""), o.get("glob", "")]:
                self.assertNotRegex(a, r"[A-Za-z]:[\\/]|\\\\", a)
            # docs/84: no Visual C++ or SDK headers, no default libraries, no C runtime's stack cookies.
            for a in ("/X", "/Zl", "/GS-", "-DPS3_GAME_FREESTANDING"):
                self.assertIn(a, o["args"], o["name"])
        self.assertIn("{objects}", d["link"])
        for a in ("/NODEFAULTLIB", "/NOENTRY", "/MANIFEST:NO"):
            self.assertIn(a, d["link"])
        self.assertFalse([a for a in d["link"] if re.fullmatch(r"(?i)[a-z0-9_]+\.lib", a)], "an SDK library by name")
        self.assertTrue(d["libs"])
        for lib in d["libs"]:
            out = next(a[5:] for a in lib["args"] if a.lower().startswith("/out:"))
            self.assertTrue(out.startswith("{gen}/"), out)
            self.assertIn(out, d["link"])
            self.assertEqual(lib["args"][0], "/lib")
            for a in lib["args"]:
                self.assertNotRegex(a, r"[A-Za-z]:[\\/]|\\\\", a)
        self.assertTrue(all(h.startswith("{src}/") for h in d["headers"]))


def overlap(a, b) -> bool:
    return a[1] < b[2] and b[1] < a[2]


def most_at_once(times) -> int:
    edges = sorted([(s, 1) for _, s, _ in times] + [(e, -1) for _, _, e in times], key=lambda x: (x[0], x[1]))
    n = best = 0
    for _, d in edges:
        n += d
        best = max(best, n)
    return best


class TestMemory(unittest.TestCase):
    """The jobs follow the memory (compile.py's docstring, docs/81 section 8), with a stand-in for the free memory
    and, but in the test of the measured peak, no process's memory read: each kind's expected peak (NEED) against
    what is free less the 2 GB kept for the PC."""
    PPU, SMALL = 0.6, 0.2                                  # how long the fake compiler takes for each

    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.tmp.cleanup)
        # The fake chunks are a few bytes, so their size would give them almost no memory: they get the kind's figure,
        # but in the tests of the size rule, which put the real figures back.
        self.base = cc.PPU_BASE
        cc.PPU_BASE = 1 << 50
        self.addCleanup(setattr, cc, "PPU_BASE", self.base)
        for rel in ("work/recomp/ppu_recomp_000.cpp", "work/recomp/ppu_recomp_001.cpp"):
            p = self.t.data / rel
            Tree.put(p, p.read_text(encoding="utf-8") + f"\n// SLEEP={self.PPU}\n")
        for rel in ("work/spu/spu_0000_at_01760280/spu_recomp.c", "work/spu/spujob_0177B480/spu_recomp.c"):
            p = self.t.data / rel
            Tree.put(p, p.read_text(encoding="utf-8") + f"\n// SLEEP={self.SMALL}\n")

    def run_with(self, free_gb, measure=False, **kw):
        c = self.t.make(jobs=kw.pop("jobs", 0), free=lambda: int(free_gb * cc.GB), **kw)
        c.cpus = 8
        old = cc.proc_memory
        if not measure:
            cc.proc_memory = lambda p: (0, 0)
        try:
            c.run_all()
        finally:
            cc.proc_memory = old
        return self.t.times()

    def test_short_memory_runs_one_at_a_time(self):
        times = self.run_with(2.9)                         # 0.9 GB over what is kept: one job at a time
        self.assertEqual(len(times), 6)
        self.assertEqual(most_at_once(times), 1, times)

    def test_small_jobs_beside_a_big_one(self):
        times = self.run_with(2 + 2.6 + 1.1)               # a chunk, and 1.1 GB beside it
        by = {n: (n, s, e) for n, s, e in times}
        self.assertFalse(overlap(by["ppu_recomp_000.obj"], by["ppu_recomp_001.obj"]), times)
        beside = [n for n in by if not n.startswith("ppu_") and
                  (overlap(by[n], by["ppu_recomp_000.obj"]) or overlap(by[n], by["ppu_recomp_001.obj"]))]
        self.assertTrue(beside, times)

    def test_plenty_of_memory_runs_side_by_side(self):
        times = self.run_with(64)
        self.assertGreaterEqual(most_at_once(times), 3, times)

    def test_progress_counts_source_bytes(self):
        calls = []
        self.run_with(64, progress=lambda step, n, m: calls.append((step, n, m)))
        total = sum(max(1, o.src.stat().st_size) for o in self.t.make(jobs=0).plan())
        self.assertEqual(calls[0], ("compile", 0, total))
        self.assertEqual(calls[-1], ("compile", total, total))
        self.assertEqual(len(calls), 7)                    # the start, then one as each object ends

    def test_need_by_kind(self):
        c = self.t.make(jobs=0)
        objs = {o.name: o for o in c.plan()}
        ppu = objs["ppu_recomp_001"]
        self.assertEqual(c._need(ppu), cc.NEED["ppu"])
        c._seen["ppu"] = cc.GB                             # a small chunk measured: the others keep the kind's figure
        self.assertEqual(c._need(ppu), cc.NEED["ppu"])
        c._seen["ppu"] = 3 * cc.GB                         # a bigger one: the others are expected to take as much
        self.assertEqual(c._need(ppu), int(3 * cc.GB * cc.ROOM))
        self.assertEqual(c._need(objs["spu_0000_at_01760280"]), cc.NEED["spu"])
        self.assertEqual(c._need(objs["des_game_module"]), cc.NEED_OTHER)

    def pad(self, rel, size):
        """The chunk's source made `size` bytes long (spaces after a comment mark; the fake compiler reads it)."""
        p = self.t.data / rel
        data = p.read_bytes() + b"\n//"
        p.write_bytes(data + b" " * (size - len(data) - 1) + b"\n")

    def test_need_by_size(self):
        cc.PPU_BASE = self.base
        c = self.t.make(jobs=0)
        o = next(o for o in c.plan() if o.name == "ppu_recomp_001")
        self.pad("work/recomp/ppu_recomp_001.cpp", 8_000_000)          # a chunk of 150,000 lines
        self.assertEqual(c._need(o), int(cc.PPU_BASE + cc.PPU_PER_MB * 8))
        c._rates["ppu"] = [cc.GB // 5]                     # a chunk of this run took more for its size
        self.assertAlmostEqual(c._need(o), cc.PPU_BASE + cc.GB // 5 * cc.ROOM * 8, delta=2)
        # One chunk in eight that takes far more: the others are not counted at its rate.
        c._rates["ppu"] = [cc.GB * 6 // 100] * 7 + [cc.GB * 12 // 100]
        self.assertEqual(c._need(o), int(cc.PPU_BASE + cc.PPU_PER_MB * 8))
        # Most of them take more: they are.
        c._rates["ppu"] = [cc.GB * 6 // 100] * 2 + [cc.GB * 12 // 100] * 6
        self.assertAlmostEqual(c._need(o), cc.PPU_BASE + cc.GB * 12 // 100 * cc.ROOM * 8, delta=2)
        c._rates.clear()
        c._seen["ppu"] = 2 * cc.GB                         # a large one measured: says nothing about this one
        self.assertEqual(c._need(o), int(cc.PPU_BASE + cc.PPU_PER_MB * 8))
        self.pad("work/recomp/ppu_recomp_001.cpp", 34_000_000)         # a chunk of 600,000 lines: the kind's figure
        c._seen.clear()
        self.assertEqual(c._need(o), cc.NEED["ppu"])

    def test_small_chunks_side_by_side(self):
        # Two 8 MB chunks are expected at 1.27 GB each, so both fit in 2.7 GB over what is kept; at the kind's 2.6 GB
        # each they ran one at a time (test_small_jobs_beside_a_big_one).
        cc.PPU_BASE = self.base
        for rel in ("work/recomp/ppu_recomp_000.cpp", "work/recomp/ppu_recomp_001.cpp"):
            self.pad(rel, 8_000_000)
        times = self.run_with(2 + 2.7)
        by = {n: (n, s, e) for n, s, e in times}
        self.assertTrue(overlap(by["ppu_recomp_000.obj"], by["ppu_recomp_001.obj"]), times)

    def test_memory_not_known(self):
        times = self.run_with(0)                           # half the 8 processors, whatever the jobs need
        self.assertEqual(len(times), 6)
        self.assertGreaterEqual(most_at_once(times), 2, times)
        self.assertLessEqual(most_at_once(times), 4, times)

    def test_given_jobs_ignore_the_memory(self):
        times = self.run_with(0.5, jobs=2)
        self.assertEqual(most_at_once(times), 2, times)

    def test_memory_hint(self):
        said, old = [], cc.SHORT_AFTER
        cc.SHORT_AFTER = 0.1
        try:
            self.run_with(2.9, memory=said.append)
        finally:
            cc.SHORT_AFTER = old
        self.assertEqual(said, [True, False])
        said.clear()
        self.run_with(64, memory=said.append)              # (nothing to build: no word either)
        self.assertEqual(said, [])

    @unittest.skipUnless(os.name == "nt", "the peak is read from the process's handle on Windows")
    def test_peak_measured_and_kept(self):
        self.run_with(64, measure=True)
        steps = json.loads(self.t.jpath.read_text())["steps"]
        peak = steps["cc:ppu_recomp_000"]["results"]["peak"]
        self.assertGreater(peak, 1 << 20)                  # a Python process takes a few MB
        c = self.t.make(jobs=0)
        o = next(o for o in c.plan() if o.name == "ppu_recomp_000")
        self.assertEqual(c._need(o), int(peak * cc.ROOM))
        self.assertLess(c._need(o), cc.NEED["ppu"])

    def test_out_of_memory_runs_alone(self):
        p = self.t.data / "work/recomp/ppu_recomp_001.cpp"
        Tree.put(p, p.read_text(encoding="utf-8") + "\nOOM_ONCE\n")
        times = self.run_with(64)
        retry = sorted((x for x in times if x[0] == "ppu_recomp_001.obj"), key=lambda x: x[1])
        self.assertEqual(len(retry), 2)
        others = [x for x in times if x[0] != "ppu_recomp_001.obj"]
        self.assertFalse([x for x in others if overlap(x, retry[-1])], times)


if __name__ == "__main__":
    unittest.main()
