"""Tests for .github/scripts/llvm_pack.py's pack step (docs/86), on a tiny made-up
LLVM archive: no LLVM bytes, no network. The smoke step needs the real compiler
and runs in the workflow only.

  cd tools/setup && python -m unittest discover -s tests
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import shutil
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from des_setup import fetch  # noqa: E402

REAL = Path(__file__).resolve().parents[3]          # this source tree
TOP = "clang+llvm-22.1.8-x86_64-pc-windows-msvc"
URL = "https://github.com/o/r/releases/download/t/llvm-win64-22.1.8-1.zip"


def load_packer():
    spec = importlib.util.spec_from_file_location("llvm_pack", REAL / ".github" / "scripts" / "llvm_pack.py")
    lp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lp)
    return lp


class PackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lp = load_packer()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.lic = self.work / "licenses"
        for comp, name in (("llvm", "LICENSE.TXT"), ("clang", "LICENSE.TXT"), ("lld", "LICENSE.TXT"),
                           ("libxml2", "Copyright")):
            (self.lic / comp).mkdir(parents=True)
            (self.lic / comp / name).write_text(f"the {comp} licence\n", encoding="utf-8")

    def archive(self, name="a.tar.xz", link_clang=False):
        files = {
            "bin/clang.exe": b"MZ fake clang",
            "bin/clang-cl.exe": b"MZ fake clang",
            "bin/lld-link.exe": b"MZ fake lld",
            "bin/LLVM-C.dll": b"MZ fake dll",
            "lib/LLVMCore.lib": b"x" * 5000,
            "include/llvm/Support/LICENSE.TXT": b"some licence",
            "lib/clang/22/lib/windows/clang_rt.builtins-x86_64.lib": b"rt",
        }
        for h in self.lp.MIN_HEADERS + ("cuda_wrappers/new",):
            files[f"lib/clang/22/include/{h}"] = f"/* {h} */\n".encode()
        path = self.work / name
        with tarfile.open(path, "w:xz") as tf:
            for rel, data in sorted(files.items()):
                ti = tarfile.TarInfo(f"{TOP}/{rel}")
                if link_clang and rel == "bin/clang.exe":
                    ti.type, ti.linkname = tarfile.SYMTYPE, "clang-cl.exe"
                    tf.addfile(ti)
                    continue
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def pack(self, arc, digest, out, method="deflate"):
        with contextlib.redirect_stdout(io.StringIO()):     # its report and ::error:: lines are for the workflow
            return self._pack(arc, digest, out, method)

    def _pack(self, arc, digest, out, method):
        return self.lp.main(["pack", "--archive", str(arc), "--source-url", "https://example.invalid/a.tar.xz",
                             "--source-sha256", digest, "--llvm-version", "22.1.8", "--name", "llvm-win64",
                             "--version", "22.1.8-1", "--licenses", str(self.lic), "--compression", method,
                             "--date", "2026-06-16", "--url", URL, "--checked", "test", "--out", str(out)])

    def test_pack_is_repeatable_and_unpacks(self):
        arc, digest = self.archive()
        for method in ("deflate", "lzma"):
            sums = []
            for run in (1, 2):
                out = self.work / f"out_{method}_{run}"
                self.assertEqual(self.pack(arc, digest, out, method), 0)
                z = out / "llvm-win64-22.1.8-1.zip"
                sums.append(hashlib.sha256(z.read_bytes()).hexdigest())
            self.assertEqual(sums[0], sums[1], f"{method}: two packs differ")
            dest = fetch.unpack(z, self.work / f"unpacked_{method}")
            got = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
            for want in ("bin/clang.exe", "bin/lld-link.exe", "lib/clang/22/include/cuda_wrappers/new",
                         "licenses/llvm/LICENSE.TXT", "licenses/libxml2/Copyright", "README.txt"):
                self.assertIn(want, got)
            for left_out in ("bin/clang-cl.exe", "bin/LLVM-C.dll", "lib/LLVMCore.lib"):
                self.assertNotIn(left_out, got)
            with zipfile.ZipFile(z) as zf:
                self.assertEqual({i.date_time for i in zf.infolist()}, {(2026, 6, 16, 0, 0, 0)})
                self.assertEqual({i.compress_type for i in zf.infolist()}, {self.lp.METHODS[method]})
            rec = json.loads((out / "llvm-win64-22.1.8-1.json").read_text(encoding="utf-8"))
            self.assertEqual(rec["entry"]["sha256"], sums[0])
            self.assertEqual(rec["entry"]["size"], z.stat().st_size)
            self.assertEqual(rec["entry"]["url"], URL)
            self.assertEqual(rec["source"]["dlls_in_bin"], ["bin/LLVM-C.dll"])

    def test_link_refused(self):
        arc, digest = self.archive("bad.tar.xz", link_clang=True)
        self.assertEqual(self.pack(arc, digest, self.work / "out"), 1)

    def test_wrong_source_sum_refused(self):
        arc, _ = self.archive()
        self.assertEqual(self.pack(arc, "0" * 64, self.work / "out"), 1)
        self.assertFalse((self.work / "out" / "llvm-win64-22.1.8-1.zip").exists())

    def test_no_llvm_licence_refused(self):
        shutil.rmtree(self.lic / "llvm")
        arc, digest = self.archive()
        self.assertEqual(self.pack(arc, digest, self.work / "out"), 1)


LTOP = "LLVM-22.1.8-Linux-X64"
LURL = "https://github.com/o/r/releases/download/t/llvm-linux-x64-22.1.8-1.tar.xz"


class PackLinuxTest(unittest.TestCase):
    """--system linux: LLVM's Linux install tree, whose clang and ld.lld are links."""

    @classmethod
    def setUpClass(cls):
        cls.lp = load_packer()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.lic = self.work / "licenses"
        for comp in ("llvm", "clang", "lld"):
            (self.lic / comp).mkdir(parents=True)
            (self.lic / comp / "LICENSE.TXT").write_text(f"the {comp} licence\n", encoding="utf-8")

    def archive(self, name="l.tar.xz", links=None, hard=False, extra=None):
        files = {
            "bin/clang-22": b"\x7fELF fake clang",
            "bin/lld": b"\x7fELF fake lld",
            "bin/llvm-ar": b"\x7fELF fake ar",
            "lib/libLLVM.so.22.1": b"\x7fELF fake lib",
            "lib/clang/22/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a": b"rt",
            "lib/cmake/llvm/LLVMConfig.cmake": b"set(LLVM_ENABLE_ZLIB ON)\nset(LLVM_ENABLE_ZSTD ON)\nset(X 1)\n",
            "include/llvm/Support/LICENSE.TXT": b"some licence",
        }
        for h in self.lp.MIN_HEADERS + ("cuda_wrappers/new",):
            files[f"lib/clang/22/include/{h}"] = f"/* {h} */\n".encode()
        files.update(extra or {})
        if links is None:
            links = {"bin/clang": "clang-22", "bin/clang++": "clang", "bin/ld.lld": "lld", "bin/lld-link": "lld"}
        path = self.work / name
        with tarfile.open(path, "w:xz") as tf:
            for rel, target in sorted(links.items()):          # links first, as an install tree may list them
                ti = tarfile.TarInfo(f"{LTOP}/{rel}")
                if hard:
                    ti.type, ti.linkname = tarfile.LNKTYPE, f"{LTOP}/bin/{target}"
                else:
                    ti.type, ti.linkname = tarfile.SYMTYPE, target
                tf.addfile(ti)
            for rel, data in sorted(files.items()):
                ti = tarfile.TarInfo(f"{LTOP}/{rel}")
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def pack(self, arc, digest, out, extra=()):
        with contextlib.redirect_stdout(io.StringIO()):
            return self.lp.main(["pack", "--system", "linux", "--archive", str(arc),
                                 "--source-url", "https://example.invalid/l.tar.xz", "--source-sha256", digest,
                                 "--llvm-version", "22.1.8", "--name", "llvm-linux-x64", "--version", "22.1.8-1",
                                 "--licenses", str(self.lic), "--date", "2026-06-16", "--url", LURL,
                                 "--checked", "test", "--out", str(out), *extra])

    def test_extra_files(self):
        """--extra: a library the workflow built goes in under its own path; the download's own paths are refused."""
        arc, digest = self.archive()
        ex = self.work / "extra"
        (ex / "lib").mkdir(parents=True)
        (ex / "lib" / "libxml2.so.2").write_bytes(b"\x7fELF fake libxml2")
        out = self.work / "out"
        self.assertEqual(self.pack(arc, digest, out, ["--extra", str(ex), "--extra-note", "Built for lld."]), 0)
        with tarfile.open(out / "llvm-linux-x64-22.1.8-1.tar.xz") as tf:
            m = tf.getmember("lib/libxml2.so.2")
            self.assertEqual((m.mode, tf.extractfile(m).read()), (0o644, b"\x7fELF fake libxml2"))
            readme = tf.extractfile("README.txt").read().decode()
        self.assertIn("Added by the workflow: lib/libxml2.so.2. Built for lld.", readme)
        rec = json.loads((out / "llvm-linux-x64-22.1.8-1.json").read_text(encoding="utf-8"))
        self.assertEqual(rec["added"], {"files": ["lib/libxml2.so.2"], "note": "Built for lld."})
        (ex / "bin").mkdir()
        (ex / "bin" / "clang").write_bytes(b"not LLVM's")
        self.assertEqual(self.pack(arc, digest, self.work / "out2", ["--extra", str(ex)]), 1)

    def test_pack_is_repeatable_and_unpacks(self):
        for hard in (False, True):
            arc, digest = self.archive(f"l{hard}.tar.xz", hard=hard)
            sums = []
            for run in (1, 2):
                out = self.work / f"out_{hard}_{run}"
                self.assertEqual(self.pack(arc, digest, out), 0)
                t = out / "llvm-linux-x64-22.1.8-1.tar.xz"
                sums.append(hashlib.sha256(t.read_bytes()).hexdigest())
            self.assertEqual(sums[0], sums[1], "two packs differ")
            with tarfile.open(t) as tf:
                members = tf.getmembers()
            self.assertTrue(all(m.isfile() for m in members), "the download must hold regular files only")
            self.assertEqual({(m.mtime, m.uid, m.gid, m.uname, m.gname) for m in members},
                             {(1781568000, 0, 0, "", "")})                  # 2026-06-16 00:00 UTC
            modes = {m.name: m.mode for m in members}
            self.assertEqual((modes["bin/clang"], modes["bin/ld.lld"]), (0o755, 0o755))
            self.assertEqual(modes["lib/clang/22/include/stddef.h"], 0o644)
            self.assertEqual([m.name for m in members], sorted(m.name for m in members))
            dest = fetch.unpack(t, self.work / f"unpacked_{hard}")
            got = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
            for want in ("bin/clang", "bin/ld.lld", "lib/clang/22/include/cuda_wrappers/new",
                         "licenses/llvm/LICENSE.TXT", "README.txt"):
                self.assertIn(want, got)
            for left_out in ("bin/clang-22", "bin/lld", "bin/clang++", "bin/llvm-ar", "lib/libLLVM.so.22.1",
                             "lib/cmake/llvm/LLVMConfig.cmake",
                             "lib/clang/22/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a"):
                self.assertNotIn(left_out, got)
            self.assertEqual((dest / "bin" / "clang").read_bytes(), b"\x7fELF fake clang")
            self.assertEqual((dest / "bin" / "ld.lld").read_bytes(), b"\x7fELF fake lld")
            rec = json.loads((out / "llvm-linux-x64-22.1.8-1.json").read_text(encoding="utf-8"))
            self.assertEqual(rec["entry"]["sha256"], sums[0])
            self.assertEqual(rec["entry"]["size"], t.stat().st_size)
            self.assertEqual(rec["entry"]["url"], LURL)
            self.assertEqual(rec["source"]["tools_from"], {"bin/clang": "bin/clang-22", "bin/ld.lld": "bin/lld"})
            self.assertEqual(rec["source"]["llvm_config"], ["set(LLVM_ENABLE_ZLIB ON)", "set(LLVM_ENABLE_ZSTD ON)"])

    def test_plain_files_need_no_links(self):
        arc, digest = self.archive(links={}, extra={"bin/clang": b"\x7fELF plain clang", "bin/ld.lld": b"\x7fELF l"})
        out = self.work / "out"
        self.assertEqual(self.pack(arc, digest, out), 0)
        rec = json.loads((out / "llvm-linux-x64-22.1.8-1.json").read_text(encoding="utf-8"))
        self.assertEqual(rec["source"]["tools_from"], {"bin/clang": "bin/clang", "bin/ld.lld": "bin/ld.lld"})

    def test_bad_links_refused(self):
        for case, links in (("loop", {"bin/clang": "clang-x", "bin/clang-x": "clang", "bin/ld.lld": "lld"}),
                            ("outside", {"bin/clang": "../lib/libLLVM.so.22.1", "bin/ld.lld": "lld"}),
                            ("missing", {"bin/clang": "clang-23", "bin/ld.lld": "lld"}),
                            ("no ld.lld", {"bin/clang": "clang-22"})):
            with self.subTest(case):
                arc, digest = self.archive(f"bad_{case.replace(' ', '_')}.tar.xz", links=links)
                self.assertEqual(self.pack(arc, digest, self.work / f"out_{case}"), 1)

    def test_header_link_refused(self):
        arc, digest = self.archive("hdr.tar.xz", links={"bin/clang": "clang-22", "bin/ld.lld": "lld",
                                                        "lib/clang/22/include/x.h": "stddef.h"})
        self.assertEqual(self.pack(arc, digest, self.work / "out"), 1)

    def test_wrong_source_sum_refused(self):
        arc, _ = self.archive()
        self.assertEqual(self.pack(arc, "0" * 64, self.work / "out"), 1)
        self.assertFalse((self.work / "out" / "llvm-linux-x64-22.1.8-1.tar.xz").exists())


class ElfInfoTest(unittest.TestCase):
    """elf_info on a real ELF file where there is one (the smoke step reads clang, ld.lld and its .so with it)."""

    @classmethod
    def setUpClass(cls):
        cls.lp = load_packer()

    @unittest.skipUnless(Path("/bin/sh").is_file() and sys.platform.startswith("linux"), "Linux only")
    def test_reads_a_system_program(self):
        info = self.lp.elf_info(Path("/bin/sh").resolve())
        self.assertEqual(info["machine"], 62)
        self.assertIn("libc.so.6", info["needed"])
        self.assertGreaterEqual(self.lp.glibc_newest(info), (2, 2))

    def test_refuses_other_files(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "x"
            p.write_bytes(b"MZ not an ELF file")
            with self.assertRaises(self.lp.Error):
                self.lp.elf_info(p)


if __name__ == "__main__":
    unittest.main()
