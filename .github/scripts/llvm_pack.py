"""Make setup's compiler download from LLVM's own Windows archive, and test it (docs/86).

Goes in the port as .github/scripts/llvm_pack.py. llvm-archive.yml runs it on a Windows runner:

  python -I llvm_pack.py pack --archive clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz \
      --source-sha256 <sum> --llvm-version 22.1.8 --name llvm-win64 --version 22.1.8-1 \
      --licenses <dir> --url <release asset url> --checked <text> --out <dir>
  python -I llvm_pack.py smoke --tools <unpacked zip> --work <dir> --llvm-version 22.1.8

pack keeps only what compiling and linking the game part needs: bin/clang.exe, bin/lld-link.exe and
clang's own headers (lib/clang/<major>/include), plus the licence files. It writes <name>-<version>.zip
(sorted entries, one date, fixed modes: the same archive gives the same zip) and <name>-<version>.json,
which holds the toolchain.json entry.

smoke compiles and links a tiny freestanding DLL with only the unpacked files (no Windows SDK, no MSVC,
no C runtime), checks it has no imports, loads it and calls it. It also lists the DLLs clang.exe and
lld-link.exe import, and fails if one is not part of Windows.

Standard library only.
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path, PurePosixPath

CHUNK = 1 << 20
TOOLS = {"bin/clang.exe": 0o755, "bin/lld-link.exe": 0o755}   # path in the archive -> mode in the zip
METHODS = {"deflate": zipfile.ZIP_DEFLATED, "lzma": zipfile.ZIP_LZMA}
MIN_HEADERS = ("stddef.h", "stdint.h", "stdatomic.h", "intrin.h", "smmintrin.h")


class Error(RuntimeError):
    pass


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(CHUNK), b""):
            h.update(b)
    return h.hexdigest()


def summary(path, text):
    print(text)
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text + "\n")


# ---------------------------------------------------------------- pack

def select(archive, header_dir):
    """Stream the tar.xz once and yield (relative path, member, file object) for the files we keep.
    Also returns what it saw, for the report."""
    seen = {"tops": set(), "members": 0, "licences": [], "bin_dlls": []}

    def gen():
        with tarfile.open(archive, "r|xz") as tf:
            for m in tf:
                seen["members"] += 1
                name = m.name.replace("\\", "/")
                while name.startswith("./"):
                    name = name[2:]
                parts = PurePosixPath(name).parts
                if not parts:
                    continue
                if name.startswith("/") or ":" in parts[0] or ".." in parts:
                    raise Error(f"{m.name!r}: an entry outside the archive's folder; refusing the archive")
                seen["tops"].add(parts[0])
                rel = "/".join(parts[1:])
                if not rel:
                    continue
                base = PurePosixPath(rel).name.lower()
                if m.isreg() and base.startswith(("license", "copying", "notice")):
                    seen["licences"].append(rel)
                if rel.startswith("bin/") and base.endswith(".dll"):
                    seen["bin_dlls"].append(rel)
                if not (rel in TOOLS or rel.startswith(header_dir)) or m.isdir():
                    continue
                if not m.isreg():
                    raise Error(f"{m.name!r} is not a regular file (tar type {m.type!r}); refusing it")
                with tf.extractfile(m) as f:
                    yield rel, f

    return seen, gen()


def write_zip(path, files, method, when):
    """files: sorted list of (name in zip, mode, source path). One date, fixed attributes."""
    stamp = time.strptime(when, "%Y-%m-%d")[:6]
    with zipfile.ZipFile(path, "w", compression=METHODS[method]) as zf:
        for name, mode, src in files:
            info = zipfile.ZipInfo(name, stamp)
            info.create_system = 3                       # Unix, so the mode below is read
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = METHODS[method]
            info.file_size = src.stat().st_size          # decides zip64 up front, so the header is fixed
            if method == "deflate":
                if hasattr(zipfile.ZipInfo, "compress_level"):   # Python 3.13
                    info.compress_level = 9
                else:
                    info._compresslevel = 9
            with open(src, "rb") as fi, zf.open(info, "w") as fo:
                shutil.copyfileobj(fi, fo, CHUNK)


def cmd_pack(a):
    archive = Path(a.archive)
    got = sha256_file(archive)
    if got != a.source_sha256:
        raise Error(f"{archive.name}: SHA-256 {got}, expected {a.source_sha256}; not packing an unchecked archive")
    major = a.llvm_version.split(".")[0]
    header_dir = f"lib/clang/{major}/include/"
    out = Path(a.out)
    stage = out / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    seen, items = select(archive, header_dir)
    modes = {}
    for rel, f in items:
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        with open(dest, "wb") as o:
            shutil.copyfileobj(f, o, CHUNK)
        modes[rel] = TOOLS.get(rel, 0o644)
    if len(seen["tops"]) != 1:
        raise Error(f"expected one top folder in the archive, found {sorted(seen['tops'])}")
    missing = [t for t in TOOLS if t not in modes] + [h for h in MIN_HEADERS if header_dir + h not in modes]
    if missing:
        raise Error(f"not in the archive: {', '.join(missing)}")

    # Licence files the workflow fetched and checked, under licenses/<component>/.
    lic = Path(a.licenses)
    for p in sorted(lic.rglob("*")):
        if p.is_file():
            rel = "licenses/" + p.relative_to(lic).as_posix()
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, stage / rel)
            modes[rel] = 0o644
    if not any(r.startswith("licenses/llvm/") for r in modes):
        raise Error("no licenses/llvm/ file: LLVM's licence must ship with the zip")
    readme = stage / "README.txt"
    readme.write_text(
        f"clang, lld-link and clang's headers from LLVM {a.llvm_version}, for Demon's Souls setup (docs/86).\n"
        f"\nTaken unchanged from LLVM's own archive:\n  {a.source_url}\n  SHA-256 {a.source_sha256}\n"
        "\nOnly these files were kept: bin/clang.exe, bin/lld-link.exe and "
        f"lib/clang/{major}/include.\nLLVM is under the Apache License v2.0 with LLVM Exceptions: "
        "licenses/llvm/LICENSE.TXT.\nLLVM's Windows build links libxml2 into its tools (MIT licence): "
        "licenses/libxml2/Copyright.\n",
        encoding="utf-8", newline="\n")
    modes["README.txt"] = 0o644

    files = sorted((rel, mode, stage / rel) for rel, mode in modes.items())
    name = f"{a.name}-{a.version}"
    zpath = out / f"{name}.zip"
    write_zip(zpath, files, a.compression, a.date)
    size, digest = zpath.stat().st_size, sha256_file(zpath)
    raw = sum(p.stat().st_size for _, _, p in files)

    record = {
        "entry": {
            "name": a.name, "version": a.version, "url": a.url, "size": size, "sha256": digest,
            "why": f"Compiles and links the game part: clang, lld-link and clang's headers from LLVM {a.llvm_version}.",
            "checked": a.checked,
        },
        "source": {"url": a.source_url, "size": archive.stat().st_size, "sha256": a.source_sha256,
                   "top_folder": sorted(seen["tops"])[0], "members": seen["members"],
                   "licence_files_inside": sorted(seen["licences"]), "dlls_in_bin": sorted(seen["bin_dlls"])},
        "compression": a.compression, "date": a.date, "unpacked_bytes": raw,
        "files": [{"path": rel, "size": p.stat().st_size, "sha256": sha256_file(p)} for rel, _, p in files],
    }
    jpath = out / f"{name}.json"
    jpath.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")

    hdrs = sum(1 for r in modes if r.startswith(header_dir))
    summary(a.summary, "\n".join([
        f"### {zpath.name}", "",
        "| | |", "|---|---|",
        f"| Size | {size:,} bytes ({a.compression}) |",
        f"| SHA-256 | `{digest}` |",
        f"| Unpacked | {raw:,} bytes, {len(files)} files ({hdrs} headers) |",
        f"| From | `{archive.name}`, {archive.stat().st_size:,} bytes, {seen['members']:,} entries |",
        f"| Licence files inside LLVM's archive | {', '.join(sorted(seen['licences'])) or 'none'} |",
        f"| DLLs in LLVM's bin/ | {', '.join(sorted(seen['bin_dlls'])) or 'none'} |",
        "", "toolchain.json entry:", "", "```json", json.dumps(record["entry"], indent=1), "```", ""]))


# ---------------------------------------------------------------- smoke

SMOKE_C = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <intrin.h>
#include <smmintrin.h>

int _fltused = 0;              /* the MSVC target asks for it once floating point is used (no C runtime here) */
static _Atomic int counter;
struct pair { char c; int64_t x; };

__declspec(dllexport) int smoke_add(int a, int b) { return a + b; }
__declspec(dllexport) double smoke_mul(double a, double b) { return a * b; }
__declspec(dllexport) int smoke_sse41(int a, int b)
{
    __m128i v = _mm_mullo_epi32(_mm_set1_epi32(a), _mm_set1_epi32(b));    /* SSE4.1 */
    return _mm_extract_epi32(v, 2);
}
__declspec(dllexport) int smoke_bsf(uint64_t v)
{
    unsigned long i;
    return _BitScanForward64(&i, v) ? (int)i : -1;
}
__declspec(dllexport) int smoke_atomic(void) { return atomic_fetch_add(&counter, 1) + 1; }
__declspec(dllexport) int smoke_offset(void) { return (int)offsetof(struct pair, x); }
"""


def pe_info(path):
    """Machine, DLL flag, entry point, imported and delay-imported DLLs, and whether it exports."""
    d = Path(path).read_bytes()
    if d[:2] != b"MZ":
        raise Error(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise Error(f"{path}: no PE header")
    machine, nsec, _, _, _, optsize, chars = struct.unpack_from("<HHIIIHH", d, pe + 4)
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        raise Error(f"{path}: not PE32+")
    entry = struct.unpack_from("<I", d, opt + 16)[0]
    ndirs = struct.unpack_from("<I", d, opt + 108)[0]
    dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
    secs = []
    for i in range(nsec):
        _, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", d, opt + optsize + 40 * i)
        secs.append((va, vsize or rsize, rptr))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return rva - va + raw
        raise Error(f"{path}: RVA {rva:#x} is in no section")

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b"\0", o)].decode("ascii")

    def table(index, step, name_field):
        names = []
        rva = dirs[index][0] if len(dirs) > index else 0
        if rva:
            o = off(rva)
            while True:
                fields = struct.unpack_from("<" + "I" * (step // 4), d, o)
                if not any(fields):
                    break
                names.append(cstr(fields[name_field]))
                o += step
        return names

    return {"machine": machine, "dll": bool(chars & 0x2000), "entry": entry,
            "imports": table(1, 20, 3), "delay_imports": table(13, 32, 1),
            "exports": len(dirs) > 0 and dirs[0][1] > 0}


def is_windows_dll(name):
    low = name.lower()
    if low.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    return (Path(os.environ["SystemRoot"]) / "System32" / name).is_file()


def run(cmd, env, cwd):
    r = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise Error(f"{Path(cmd[0]).name} failed ({r.returncode:#x}):\n{r.stdout}\n{r.stderr}")
    return r.stdout + r.stderr


def cmd_smoke(a):
    if os.name != "nt":
        raise Error("the smoke test is for Windows only (docs/87 has the Linux plan)")
    tools = Path(a.tools).resolve()
    clang, lld = tools / "bin" / "clang.exe", tools / "bin" / "lld-link.exe"
    work = Path(a.work).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    root = os.environ["SystemRoot"]
    # Only Windows' own folders: no other LLVM, no MSVC, no Windows SDK, no INCLUDE or LIB.
    env = {"SystemRoot": root, "windir": root, "SystemDrive": os.environ.get("SystemDrive", "C:"),
           "PATH": f"{root}\\System32;{root}", "TEMP": str(work), "TMP": str(work)}
    lines = []

    for exe in (clang, lld):
        info = pe_info(exe)
        bad = [n for n in info["imports"] + info["delay_imports"] if not is_windows_dll(n)]
        lines.append(f"| {exe.name} imports | {', '.join(info['imports'])}"
                     f"{'; delay-loaded: ' + ', '.join(info['delay_imports']) if info['delay_imports'] else ''} |")
        if bad:
            raise Error(f"{exe.name} needs DLLs that are not part of Windows: {', '.join(bad)}")

    v = run([str(clang), "--version"], env, work).splitlines()[0]
    if f"clang version {a.llvm_version}" not in v:
        raise Error(f"clang.exe says {v!r}, expected version {a.llvm_version}")
    lv = run([str(lld), "--version"], env, work).strip()
    if f"LLD {a.llvm_version}" not in lv:
        raise Error(f"lld-link.exe says {lv!r}, expected LLD {a.llvm_version}")
    lines += [f"| clang --version | {v} |", f"| lld-link --version | {lv} |"]

    major = a.llvm_version.split(".")[0]
    rdir = Path(run([str(clang), "-print-resource-dir"], env, work).strip()).resolve()
    if rdir != (tools / "lib" / "clang" / major):
        raise Error(f"clang's resource folder is {rdir}, not the one in the zip")

    src = work / "smoke.c"
    src.write_text(SMOKE_C, encoding="utf-8")
    cflags = ["--target=x86_64-pc-windows-msvc", "-O2", "-ffreestanding", "-nostdlibinc",
              "-fno-stack-protector", "-fno-math-errno", "-fwrapv", "-msse4.1", "-Wall"]
    # Every header must come from the zip: read clang's own include search list.
    out = run([str(clang), *cflags, "-v", "-E", str(src), "-o", str(work / "smoke.i")], env, work).splitlines()
    try:
        start = out.index("#include <...> search starts here:") + 1
        search = out[start:out.index("End of search list.")]
    except ValueError:
        raise Error("clang -v printed no include search list:\n" + "\n".join(out))
    outside = [s.strip() for s in search if not str(Path(s.strip()).resolve()).lower().startswith(str(tools).lower())]
    if outside or not search:
        raise Error(f"clang searches headers outside the zip: {outside or 'no folder at all'}")
    run([str(clang), *cflags, "-c", str(src), "-o", str(work / "smoke.obj")], env, work)
    run([str(lld), "/dll", "/noentry", "/nodefaultlib", "/lldignoreenv", "/machine:x64",
         f"/out:{work / 'smoke.dll'}", str(work / "smoke.obj")], env, work)

    info = pe_info(work / "smoke.dll")
    if not info["dll"] or info["machine"] != 0x8664 or info["entry"] != 0 or not info["exports"]:
        raise Error(f"smoke.dll is not a /noentry x64 DLL with exports: {info}")
    if info["imports"] or info["delay_imports"]:
        raise Error(f"smoke.dll imports {info['imports'] + info['delay_imports']}; it must import nothing")
    lib = ctypes.CDLL(str(work / "smoke.dll"))
    lib.smoke_mul.restype = ctypes.c_double
    lib.smoke_mul.argtypes = (ctypes.c_double, ctypes.c_double)
    lib.smoke_bsf.argtypes = (ctypes.c_uint64,)
    checks = {"smoke_add(2, 3)": (lib.smoke_add(2, 3), 5),
              "smoke_mul(1.5, 4.0)": (lib.smoke_mul(1.5, 4.0), 6.0),
              "smoke_sse41(7, 6)": (lib.smoke_sse41(7, 6), 42),
              "smoke_bsf(0x100)": (lib.smoke_bsf(0x100), 8),
              "smoke_atomic() twice": ((lib.smoke_atomic(), lib.smoke_atomic()), (1, 2)),
              "smoke_offset()": (lib.smoke_offset(), 8)}
    wrong = {k: v for k, v in checks.items() if v[0] != v[1]}
    if wrong:
        raise Error(f"smoke.dll gave wrong answers: {wrong}")
    lines.append(f"| smoke.dll | {(work / 'smoke.dll').stat().st_size:,} bytes, no imports, "
                 f"{len(checks)} calls right; headers only from the zip |")

    # Again in cl mode, as setup's compile step runs clang (clang.exe --driver-mode=cl with CMake's clang-cl
    # commands; the zip has no clang-cl.exe). /X: only clang's own headers. Without it clang-cl finds Visual
    # Studio's headers through the registry, even with no INCLUDE (measured). The smoke file is freestanding
    # here because mm_malloc.h, which intrin.h reaches, wants <stdlib.h> in hosted mode. The game part stays
    # hosted, with its own small headers, because -ffreestanding also turns memcpy and sqrt into calls.
    cl = [str(clang), "--driver-mode=cl", "/nologo", "/X", "/O2", "/GS-", "/clang:-msse4.1", "/clang:-ffreestanding"]
    out = run(cl + ["-v", "/E", str(src)], env, work).splitlines()
    try:
        search = out[out.index("#include <...> search starts here:") + 1:out.index("End of search list.")]
    except ValueError:
        raise Error("clang in cl mode printed no include search list:\n" + "\n".join(out[-20:]))
    outside = [s.strip() for s in search if not str(Path(s.strip()).resolve()).lower().startswith(str(tools).lower())]
    if outside or not search:
        raise Error(f"clang in cl mode searches headers outside the zip: {outside or 'no folder at all'}")
    run(cl + ["/c", str(src), f"/Fo{work / 'smoke_cl.obj'}"], env, work)
    run([str(lld), "/dll", "/noentry", "/nodefaultlib", "/lldignoreenv", "/machine:x64",
         f"/out:{work / 'smoke_cl.dll'}", str(work / "smoke_cl.obj")], env, work)
    info = pe_info(work / "smoke_cl.dll")
    if info["imports"] or info["delay_imports"] or info["entry"] != 0:
        raise Error(f"smoke_cl.dll is not a /noentry DLL without imports: {info}")
    lib = ctypes.CDLL(str(work / "smoke_cl.dll"))
    if (lib.smoke_add(2, 3), lib.smoke_sse41(7, 6)) != (5, 42):
        raise Error("smoke_cl.dll gave wrong answers")
    lines.append("| smoke_cl.dll | cl mode (`--driver-mode=cl /X`): headers only from the zip, no imports, calls right |")
    summary(a.summary, "\n".join(["### Smoke test", "", "| | |", "|---|---|", *lines, ""]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("pack")
    k.add_argument("--archive", required=True)
    k.add_argument("--source-url", required=True)
    k.add_argument("--source-sha256", required=True)
    k.add_argument("--llvm-version", required=True)
    k.add_argument("--name", required=True)
    k.add_argument("--version", required=True)
    k.add_argument("--licenses", required=True)
    k.add_argument("--compression", choices=sorted(METHODS), default="lzma")
    k.add_argument("--date", required=True, help="the zip entries' date, YYYY-MM-DD")
    k.add_argument("--url", required=True)
    k.add_argument("--checked", required=True)
    k.add_argument("--out", required=True)
    k.add_argument("--summary")
    s = sub.add_parser("smoke")
    s.add_argument("--tools", required=True)
    s.add_argument("--work", required=True)
    s.add_argument("--llvm-version", required=True)
    s.add_argument("--summary")
    a = p.parse_args(argv)
    try:
        (cmd_pack if a.cmd == "pack" else cmd_smoke)(a)
    except Error as e:
        print(f"::error::{e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
