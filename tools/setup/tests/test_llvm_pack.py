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


if __name__ == "__main__":
    unittest.main()
