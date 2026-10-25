"""Make setup's compiler download from LLVM's own Windows or Linux archive, and test it (docs/86, docs/87).

Goes in the port as .github/scripts/llvm_pack.py. llvm-archive.yml runs it on a Windows or a Linux runner:

  python -I llvm_pack.py pack --system windows --archive clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz \
      --source-sha256 <sum> --llvm-version 22.1.8 --name llvm-win64 --version 22.1.8-1 \
      --licenses <dir> --url <release asset url> --checked <text> --out <dir>
  python -I llvm_pack.py smoke --tools <unpacked download> --work <dir> --llvm-version 22.1.8

pack keeps only what compiling and linking the game part needs, plus the licence files:
  - Windows: bin/clang.exe, bin/lld-link.exe and clang's own headers (lib/clang/<major>/include). It writes
    <name>-<version>.zip (sorted entries, one date, fixed modes: the same archive gives the same zip).
  - Linux (--system linux, from LLVM-<version>-Linux-X64.tar.xz): bin/clang, bin/ld.lld and the same headers.
    The install tree's clang and ld.lld are links (to clang-<major> and lld); the real files go in under the
    names setup runs, so the download holds no links. --extra adds files the workflow made under their own
    paths (lib/libxml2.so.2, which LLVM's lld needs). It writes <name>-<version>.tar.xz (sorted entries, one
    date, owner 0, fixed modes, xz in one thread: the same files give the same tar.xz). A tar, because a zip
    unpacked by Python loses the execute bit.
Both also write <name>-<version>.json, which holds the toolchain.json entry.

smoke builds a tiny freestanding library with only the unpacked files, checks it, loads it and calls it:
  - Windows: a DLL (no Windows SDK, no MSVC, no C runtime) with no imports. It also lists the DLLs clang.exe
    and lld-link.exe import, and fails if one is not part of Windows.
  - Linux: a .so built with the game part's own flags (docs/87), with no NEEDED entries, only its own
    exports and an initial-exec thread-local. It also lists the libraries clang, ld.lld and the download's
    own libraries need, and fails on one a desktop Linux may not have that the download does not carry, or on
    a glibc symbol newer than GLIBC_FLOOR.

Standard library only.
"""
import argparse
import calendar
import ctypes
import hashlib
import json
import lzma
import os
import posixpath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import threading
import time
import zipfile
from pathlib import Path, PurePosixPath

CHUNK = 1 << 20
TOOLS = {"bin/clang.exe": 0o755, "bin/lld-link.exe": 0o755}   # path in the archive -> mode in the zip
METHODS = {"deflate": zipfile.ZIP_DEFLATED, "lzma": zipfile.ZIP_LZMA}
MIN_HEADERS = ("stddef.h", "stdint.h", "stdatomic.h", "intrin.h", "smmintrin.h")

# Linux: the names setup runs (compile.py) -> mode in the tar, and the bin/ files those names can lead to.
LINUX_TOOLS = {"bin/clang": 0o755, "bin/ld.lld": 0o755}
LINUX_BIN = re.compile(r"clang|clang-\d+|lld|ld\.lld")
LLVM_CONFIG = "lib/cmake/llvm/LLVMConfig.cmake"     # read for the report: which libraries LLVM was built with
# The oldest glibc the Linux build supports (docs/87): the ubuntu-22.04 runner's.
GLIBC_FLOOR = (2, 35)
GLIBC_LIBS = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libresolv.so.2",
              "libutil.so.1", "ld-linux-x86-64.so.2"}
# What every desktop Linux has besides glibc: GCC's runtime, zlib and zstd. The tools may need nothing else.
SYSTEM_LIBS = GLIBC_LIBS | {"libstdc++.so.6", "libgcc_s.so.1", "libz.so.1", "libzstd.so.1"}


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
    if a.system == "linux":
        return cmd_pack_linux(a)
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


def select_linux(archive, header_dir, stage):
    """Stream LLVM's Linux tar.xz once. Saves under stage/ the headers, LLVMConfig.cmake and the bin/ files that
    bin/clang and bin/ld.lld can lead to, and records bin/'s links on the way (clang is a link to clang-<major>).
    Returns what it saw and {path in the archive: saved file}."""
    seen = {"tops": set(), "members": 0, "licences": [], "links": {}}
    files = {}
    with tarfile.open(archive, "r|xz") as tf:
        for m in tf:
            seen["members"] += 1
            name = m.name.replace("\\", "/")
            while name.startswith("./"):
                name = name[2:]
            parts = PurePosixPath(name).parts
            if not parts:
                continue
            if name.startswith("/") or ".." in parts:
                raise Error(f"{m.name!r}: an entry outside the archive's folder; refusing the archive")
            seen["tops"].add(parts[0])
            rel = "/".join(parts[1:])
            if not rel or m.isdir():
                continue
            base = PurePosixPath(rel).name
            if m.isreg() and base.lower().startswith(("license", "copying", "notice")):
                seen["licences"].append(rel)
            in_bin = rel.startswith("bin/") and "/" not in rel[4:]
            if in_bin and (m.issym() or m.islnk()):
                # A symlink's target is from its own folder, a hard link's from the archive's top.
                target = (posixpath.join("bin", m.linkname) if m.issym()
                          else "/".join(PurePosixPath(m.linkname.replace("\\", "/")).parts[1:]))
                seen["links"][rel] = posixpath.normpath(target)
                continue
            if not ((in_bin and LINUX_BIN.fullmatch(base)) or rel.startswith(header_dir) or rel == LLVM_CONFIG):
                continue
            if not m.isreg():
                raise Error(f"{m.name!r} is not a regular file (tar type {m.type!r}); refusing it")
            dest = stage / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(m) as f, open(dest, "wb") as o:
                shutil.copyfileobj(f, o, CHUNK)
            files[rel] = dest
    return seen, files


def resolve_link(rel, links, files):
    """The kept file a bin/ name leads to, through the archive's links."""
    path = [rel]
    while path[-1] in links:
        if len(path) > 8 or links[path[-1]] in path:
            raise Error(f"{rel}: the links loop ({' -> '.join(path)})")
        path.append(links[path[-1]])
    if path[-1] not in files:
        raise Error(f"{' -> '.join(path)}: no such file in the archive's bin/")
    return path[-1]


def write_tar_xz(path, files, when):
    """files: sorted list of (name in the tar, mode, source path). Plain ustar entries with one date, owner 0 and
    fixed modes, and xz preset 9 in one thread, so the same files give the same bytes."""
    mtime = calendar.timegm(time.strptime(when, "%Y-%m-%d"))
    with lzma.open(path, "wb", format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=9) as xz:
        with tarfile.open(fileobj=xz, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            for name, mode, src in files:
                ti = tarfile.TarInfo(name)
                ti.size = src.stat().st_size
                ti.mode = mode
                ti.mtime = mtime
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ""
                with open(src, "rb") as f:
                    tf.addfile(ti, f)


def cmd_pack_linux(a):
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

    seen, files = select_linux(archive, header_dir, stage / "archive")
    if len(seen["tops"]) != 1:
        raise Error(f"expected one top folder in the archive, found {sorted(seen['tops'])}")
    chosen = {}                                     # name in the tar -> (mode, file, path in LLVM's archive)
    for name, mode in LINUX_TOOLS.items():
        real = resolve_link(name, seen["links"], files)
        chosen[name] = (mode, files[real], real)
    for rel, p in files.items():
        if rel.startswith(header_dir):
            chosen[rel] = (0o644, p, rel)
    missing = [header_dir + h for h in MIN_HEADERS if header_dir + h not in chosen]
    if missing:
        raise Error(f"not in the archive: {', '.join(missing)}")

    lic = Path(a.licenses)
    for p in sorted(lic.rglob("*")):
        if p.is_file():
            chosen["licenses/" + p.relative_to(lic).as_posix()] = (0o644, p, None)
    if not any(r.startswith("licenses/llvm/") for r in chosen):
        raise Error("no licenses/llvm/ file: LLVM's licence must ship with the download")
    # Files the workflow made itself (lib/libxml2.so.2, for ld.lld), under their own paths.
    extra = []
    if a.extra:
        root = Path(a.extra)
        for p in sorted(root.rglob("*")):
            if p.is_file():
                rel = p.relative_to(root).as_posix()
                if rel in chosen or rel.startswith(("bin/", "licenses/", header_dir)):
                    raise Error(f"--extra {rel}: that path is the download's own")
                chosen[rel] = (0o644, p, None)
                extra.append(rel)

    # Which libraries LLVM was built with, for the report: a library built in may bring a licence to ship.
    config = []
    if LLVM_CONFIG in files:
        config = [ln.strip() for ln in files[LLVM_CONFIG].read_text(encoding="utf-8", errors="replace").splitlines()
                  if re.match(r"\s*set\((LLVM_ENABLE_(ZLIB|ZSTD|LIBXML2|LIBEDIT|TERMINFO|LIBCXX|FFI|CURL|HTTPLIB)|"
                              r"LLVM_USE_STATIC_ZSTD|LLVM_LINK_LLVM_DYLIB|LLVM_BUILD_LLVM_DYLIB)\b", ln)]
    # LLVM's Linux build links zstd into its tools (LLVM_USE_STATIC_ZSTD in its release cache, which
    # LLVMConfig.cmake does not repeat), so a build with zstd ships zstd's licence.
    zstd = any(re.fullmatch(r"set\(LLVM_ENABLE_ZSTD\s+(ON|TRUE|YES|1|FORCE_ON)\)", c, re.I) for c in config)
    if zstd and not any(r.startswith("licenses/zstd/") for r in chosen):
        raise Error("LLVM was built with zstd, which its Linux build links into clang and lld: zstd's licence must "
                    "ship, under licenses/zstd/")
    readme = stage / "README.txt"
    readme.write_text(
        f"clang, ld.lld and clang's headers from LLVM {a.llvm_version}, for Demon's Souls setup (docs/86, docs/87).\n"
        f"\nTaken unchanged from LLVM's own archive:\n  {a.source_url}\n  SHA-256 {a.source_sha256}\n"
        f"\nOnly these files were kept: bin/clang (LLVM's {chosen['bin/clang'][2]}), bin/ld.lld (LLVM's "
        f"{chosen['bin/ld.lld'][2]}) and lib/clang/{major}/include.\nLLVM is under the Apache License v2.0 with "
        "LLVM Exceptions: licenses/llvm/LICENSE.TXT.\n"
        + ("LLVM's Linux build links zstd into clang and ld.lld (BSD licence): licenses/zstd/.\n" if zstd else "")
        + (f"\nAdded by the workflow: {', '.join(extra)}. {a.extra_note}\n" if extra else ""),
        encoding="utf-8", newline="\n")
    chosen["README.txt"] = (0o644, readme, None)

    tar = sorted((rel, mode, p) for rel, (mode, p, _) in chosen.items())
    name = f"{a.name}-{a.version}"
    tpath = out / f"{name}.tar.xz"
    write_tar_xz(tpath, tar, a.date)
    size, digest = tpath.stat().st_size, sha256_file(tpath)
    raw = sum(p.stat().st_size for _, _, p in tar)

    record = {
        "entry": {
            "name": a.name, "version": a.version, "url": a.url, "size": size, "sha256": digest,
            "why": f"Compiles and links the game part on Linux: clang, ld.lld and clang's headers from LLVM "
                   f"{a.llvm_version} (docs/86).",
            "checked": a.checked,
        },
        "source": {"url": a.source_url, "size": archive.stat().st_size, "sha256": a.source_sha256,
                   "top_folder": sorted(seen["tops"])[0], "members": seen["members"],
                   "licence_files_inside": sorted(seen["licences"]),
                   "tools_from": {n: chosen[n][2] for n in LINUX_TOOLS},
                   "links_in_bin": dict(sorted(seen["links"].items())), "llvm_config": config},
        "added": {"files": extra, "note": a.extra_note if extra else None},
        "compression": "xz", "date": a.date, "unpacked_bytes": raw,
        "files": [{"path": rel, "size": p.stat().st_size, "sha256": sha256_file(p)} for rel, _, p in tar],
    }
    jpath = out / f"{name}.json"
    jpath.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")

    hdrs = sum(1 for r in chosen if r.startswith(header_dir))
    summary(a.summary, "\n".join([
        f"### {tpath.name}", "",
        "| | |", "|---|---|",
        f"| Size | {size:,} bytes (xz) |",
        f"| SHA-256 | `{digest}` |",
        f"| Unpacked | {raw:,} bytes, {len(tar)} files ({hdrs} headers) |",
        f"| From | `{archive.name}`, {archive.stat().st_size:,} bytes, {seen['members']:,} entries |",
        f"| Tools | bin/clang from {chosen['bin/clang'][2]}, bin/ld.lld from {chosen['bin/ld.lld'][2]} |",
        f"| Added by the workflow | {', '.join(extra) or 'nothing'} |",
        f"| Licence files inside LLVM's archive | {', '.join(sorted(seen['licences'])) or 'none'} |",
        f"| LLVMConfig.cmake | {'; '.join(config) or 'not in the archive'} |",
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
        return smoke_linux(a)
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


SMOKE_LINUX_C = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <smmintrin.h>

#define EXPORT __attribute__((visibility("default")))
static _Atomic int counter;
static __thread int per_thread;          /* initial-exec (-ftls-model), as the game part's thread-locals */
struct pair { char c; int64_t x; };

EXPORT int smoke_add(int a, int b) { return a + b; }
EXPORT double smoke_mul(double a, double b) { return a * b; }
EXPORT int smoke_sse41(int a, int b)
{
    __m128i v = _mm_mullo_epi32(_mm_set1_epi32(a), _mm_set1_epi32(b));    /* SSE4.1 */
    return _mm_extract_epi32(v, 2);
}
EXPORT int smoke_ctz(uint64_t v) { return v ? __builtin_ctzll(v) : -1; }
EXPORT int smoke_atomic(void) { return atomic_fetch_add(&counter, 1) + 1; }
EXPORT int smoke_offset(void) { return (int)offsetof(struct pair, x); }
EXPORT int smoke_tls(void) { return ++per_thread; }
int smoke_hidden(void) { return 7; }     /* hidden by -fvisibility=hidden: must not be exported */
"""

SMOKE_LINUX_CPP = r"""
#include <stdint.h>

template <typename T> constexpr T twice(T v) { return v + v; }
struct shape { virtual int sides() const { return 0; } };
struct square : shape { int sides() const override { return 4; } };
static int sides_of(const shape &s) { return s.sides(); }

extern "C" __attribute__((visibility("default"))) int smoke_cpp(int v)
{
    square sq;
    auto add = [v](int x) { return x + v; };
    return add(twice<int>(sides_of(sq)));      /* 8 + v */
}
"""

# The game part compiles hosted with its own small C headers (ps3recomp's game_libc/), and so does the smoke test:
# clang's <mm_malloc.h>, which <smmintrin.h> reaches, wants malloc and free from <stdlib.h>.
SMOKE_LINUX_STDLIB = r"""
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t size);
void free(void *block);
#ifdef __cplusplus
}
#endif
"""

# The game part's flags on Linux (game_build_linux.json, docs/87), without its -D, -I and -include.
LINUX_CFLAGS = ["-fPIC", "-O2", "-fwrapv", "-msse4.1", "-fno-strict-aliasing", "-ffp-contract=off", "-nostdlibinc",
                "-fvisibility=hidden", "-ftls-model=initial-exec", "-fno-stack-protector", "-Wall"]
LINUX_LDFLAGS = ["-fPIC", "-nostdlib", "-fuse-ld=lld", "-Wl,-z,defs", "-Wl,-z,now", "-Wl,--build-id=sha1", "-shared"]
SMOKE_EXPORTS = {"smoke_add", "smoke_mul", "smoke_sse41", "smoke_ctz", "smoke_atomic", "smoke_offset", "smoke_tls",
                 "smoke_cpp"}


def elf_info(path):
    """An x86-64 ELF file's type, its dynamic section (NEEDED, SONAME, RUNPATH, RPATH and the flags), the symbol
    versions it needs from each library, the symbols it exports and whether it has thread-locals."""
    d = Path(path).read_bytes()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        raise Error(f"{path}: not a 64-bit little-endian ELF file")
    etype, machine = struct.unpack_from("<HH", d, 16)
    phoff, shoff = struct.unpack_from("<QQ", d, 32)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", d, 54)
    phdrs = [struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize) for i in range(phnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]

    def cstr(sec, off):
        o = secs[sec][4] + off
        return d[o:d.index(b"\0", o)].decode("utf-8", "replace")

    info = {"type": etype, "machine": machine, "needed": [], "soname": None, "runpath": None, "rpath": None,
            "flags": 0, "flags_1": 0, "versions": {}, "exports": [], "tls": any(p[0] == 7 for p in phdrs)}
    for s in secs:
        stype, off, size, link, count = s[1], s[4], s[5], s[6], s[7]
        if stype == 6:                                   # SHT_DYNAMIC
            for o in range(off, off + size, 16):
                tag, val = struct.unpack_from("<qQ", d, o)
                if tag == 0:
                    break
                if tag == 1:
                    info["needed"].append(cstr(link, val))
                elif tag in (14, 15, 29):
                    info[{14: "soname", 15: "rpath", 29: "runpath"}[tag]] = cstr(link, val)
                elif tag == 30:
                    info["flags"] = val
                elif tag == 0x6FFFFFFB:
                    info["flags_1"] = val
        elif stype == 0x6FFFFFFE:                        # SHT_GNU_verneed: the versions needed, per library
            o = off
            for _ in range(count):
                _, cnt, file_, aux, nxt = struct.unpack_from("<HHIII", d, o)
                names, a = [], o + aux
                for _ in range(cnt):
                    _, _, _, vname, anext = struct.unpack_from("<IHHII", d, a)
                    names.append(cstr(link, vname))
                    a += anext
                info["versions"].setdefault(cstr(link, file_), []).extend(names)
                if not nxt:
                    break
                o += nxt
        elif stype == 11:                                # SHT_DYNSYM: defined global or weak, default visibility
            for o in range(off + 24, off + size, 24):
                name, st_info, other, shndx = struct.unpack_from("<IBBH", d, o)
                if shndx != 0 and (st_info >> 4) in (1, 2) and (other & 3) in (0, 3) and (st_info & 0xF) in (0, 1, 2, 6):
                    info["exports"].append(cstr(link, name))
    return info


def glibc_newest(info):
    """The newest GLIBC_x.y the file needs, as (x, y)."""
    vs = [tuple(int(n) for n in v[6:].split(".")) for lib, names in info["versions"].items() if lib in GLIBC_LIBS
          for v in names if re.fullmatch(r"GLIBC_\d+(\.\d+)*", v)]
    return max(vs, default=(0,))


def newest_others(info):
    """The newest version the file needs of each family outside glibc (GLIBCXX_, CXXABI_, GCC_ ...)."""
    best = {}
    for lib, names in info["versions"].items():
        for v in names:
            m = re.fullmatch(r"([A-Za-z_]+?)_?(\d+(?:\.\d+)*)", v)
            if lib not in GLIBC_LIBS and m:
                key = tuple(int(n) for n in m.group(2).split("."))
                if key > best.get(m.group(1), ((), ""))[0]:
                    best[m.group(1)] = (key, v)
    return sorted(v for _, v in best.values())


def smoke_linux(a):
    tools = Path(a.tools).resolve()
    clang, lld = tools / "bin" / "clang", tools / "bin" / "ld.lld"
    work = Path(a.work).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    # Only the system's own folders on PATH, and no compiler, include or library variables.
    env = {"PATH": "/usr/bin:/bin", "HOME": str(work), "TMPDIR": str(work), "LC_ALL": "C"}
    lines, ldd, problems = [], [], []

    def needs(path, rel):
        """Report what a program or library of the download needs; note what a player's PC may not have."""
        info = elf_info(path)
        if info["machine"] != 62 or info["type"] not in (2, 3):
            raise Error(f"{rel} is not x86-64 ELF code: {info['type']}, {info['machine']}")
        newest, other = glibc_newest(info), newest_others(info)
        lines.append(f"| {rel} needs | {', '.join(info['needed']) or 'nothing'}; newest GLIBC_"
                     f"{'.'.join(map(str, newest))}{'; ' + ', '.join(other) if other else ''}"
                     f"{'; RUNPATH ' + info['runpath'] if info['runpath'] else ''}"
                     f"{'; RPATH ' + info['rpath'] if info['rpath'] else ''} |")
        # A library outside SYSTEM_LIBS must be one the download carries in lib/, where the program's own search
        # path leads (LLVM's tools have $ORIGIN/../lib).
        search = (info["runpath"] or info["rpath"] or "").split(":")
        for n in info["needed"]:
            if n in SYSTEM_LIBS:
                continue
            if not (tools / "lib" / n).is_file():
                problems.append(f"{rel} needs {n}, which a desktop Linux may not have and the download does not carry")
            elif rel.startswith("bin/") and not {"$ORIGIN/../lib", "${ORIGIN}/../lib"} & set(search):
                problems.append(f"{rel} needs the download's lib/{n}, but its RUNPATH does not lead to lib/")
        if newest > GLIBC_FLOOR:
            problems.append(f"{rel} needs GLIBC_{'.'.join(map(str, newest))}, newer than the floor "
                            f"{'.'.join(map(str, GLIBC_FLOOR))} (docs/87)")

    for exe in (clang, lld):
        needs(exe, f"bin/{exe.name}")
        r = subprocess.run(["ldd", str(exe)], capture_output=True, text=True, errors="replace")
        ldd += [f"$ ldd bin/{exe.name}", (r.stdout + r.stderr).rstrip()]
    for lib in sorted(p for p in (tools / "lib").glob("*.so*") if p.is_file()):
        needs(lib, f"lib/{lib.name}")
    if problems:
        summary(a.summary, "\n".join(["### Smoke test", "", "| | |", "|---|---|", *lines, "",
                                      "```", *ldd, "```", ""]))
        raise Error("; ".join(problems))

    v = run([str(clang), "--version"], env, work).splitlines()[0]
    if f"clang version {a.llvm_version}" not in v:
        raise Error(f"clang says {v!r}, expected version {a.llvm_version}")
    lv = run([str(lld), "--version"], env, work).strip()
    if f"LLD {a.llvm_version}" not in lv:
        raise Error(f"ld.lld says {lv!r}, expected LLD {a.llvm_version}")
    lines += [f"| clang --version | {v} |", f"| ld.lld --version | {lv} |"]

    major = a.llvm_version.split(".")[0]
    rdir = Path(run([str(clang), "-print-resource-dir"], env, work).strip()).resolve()
    if rdir != (tools / "lib" / "clang" / major):
        raise Error(f"clang's resource folder is {rdir}, not the one in the download")

    inc = work / "inc"
    inc.mkdir()
    (inc / "stdlib.h").write_text(SMOKE_LINUX_STDLIB, encoding="utf-8")
    objs = []
    for src_name, text, lang in (("smoke.c", SMOKE_LINUX_C, ["-std=gnu17"]),
                                 ("smoke.cpp", SMOKE_LINUX_CPP, ["-std=gnu++20", "-fno-rtti"])):
        src = work / src_name
        src.write_text(text, encoding="utf-8")
        flags = [str(clang), *lang, *LINUX_CFLAGS, f"-I{inc}"]
        # Every header must come from the download or the smoke test's own folder: read clang's search list.
        out = run(flags + ["-v", "-E", str(src), "-o", str(work / (src_name + ".i"))], env, work).splitlines()
        try:
            search = out[out.index("#include <...> search starts here:") + 1:out.index("End of search list.")]
        except ValueError:
            raise Error("clang -v printed no include search list:\n" + "\n".join(out))
        search = [Path(s.strip()).resolve() for s in search]
        outside = [str(s) for s in search if s != inc and not s.is_relative_to(tools)]
        if outside or tools / "lib" / "clang" / major / "include" not in search:
            raise Error(f"clang searches headers outside the download for {src_name}: {outside or 'not its own'}")
        objs.append(str(work / (src_name + ".o")))
        run(flags + ["-c", str(src), "-o", objs[-1]], env, work)

    link = [str(clang), *LINUX_LDFLAGS, "-Wl,-soname,smoke.so", "-o", str(work / "smoke.so"), *objs]
    # The link must run the download's ld.lld, not one on PATH: clang -### prints the command it would run.
    jobs = [ln for ln in run(link + ["-###"], env, work).splitlines() if ln.startswith(' "')]
    linker = Path(jobs[-1].split('"')[1]).resolve() if jobs else None
    if linker != lld.resolve():
        raise Error(f"clang links with {linker}, not the download's ld.lld")
    run(link, env, work)

    info = elf_info(work / "smoke.so")
    if info["type"] != 3 or info["machine"] != 62:
        raise Error(f"smoke.so is not an x86-64 shared object: {info['type']}, {info['machine']}")
    if info["needed"]:
        raise Error(f"smoke.so needs {info['needed']}; it must need nothing")
    if set(info["exports"]) != SMOKE_EXPORTS:
        raise Error(f"smoke.so exports {sorted(info['exports'])}, expected {sorted(SMOKE_EXPORTS)}")
    if not info["tls"] or not info["flags"] & 0x10:              # PT_TLS, DF_STATIC_TLS
        raise Error("smoke.so has no initial-exec thread-locals (PT_TLS and DF_STATIC_TLS)")
    lib = ctypes.CDLL(str(work / "smoke.so"))
    lib.smoke_mul.restype = ctypes.c_double
    lib.smoke_mul.argtypes = (ctypes.c_double, ctypes.c_double)
    lib.smoke_ctz.argtypes = (ctypes.c_uint64,)
    other_thread = []
    t = threading.Thread(target=lambda: other_thread.append(lib.smoke_tls()))
    checks = {"smoke_add(2, 3)": (lib.smoke_add(2, 3), 5),
              "smoke_mul(1.5, 4.0)": (lib.smoke_mul(1.5, 4.0), 6.0),
              "smoke_sse41(7, 6)": (lib.smoke_sse41(7, 6), 42),
              "smoke_ctz(0x100)": (lib.smoke_ctz(0x100), 8),
              "smoke_atomic() twice": ((lib.smoke_atomic(), lib.smoke_atomic()), (1, 2)),
              "smoke_offset()": (lib.smoke_offset(), 8),
              "smoke_tls() twice": ((lib.smoke_tls(), lib.smoke_tls()), (1, 2)),
              "smoke_cpp(5)": (lib.smoke_cpp(5), 13)}
    t.start()
    t.join()
    checks["smoke_tls() on another thread"] = (other_thread, [1])
    wrong = {k: v for k, v in checks.items() if v[0] != v[1]}
    if wrong:
        raise Error(f"smoke.so gave wrong answers: {wrong}")
    lines.append(f"| smoke.so | {(work / 'smoke.so').stat().st_size:,} bytes, C and C++ with the game part's flags, "
                 f"no NEEDED, {len(SMOKE_EXPORTS)} exports only, initial-exec TLS, {len(checks)} calls right; "
                 "headers only from the download, linked by its ld.lld |")
    summary(a.summary, "\n".join(["### Smoke test", "", "| | |", "|---|---|", *lines, "",
                                  "```", *ldd, "```", ""]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("pack")
    k.add_argument("--system", choices=("windows", "linux"), default="windows",
                   help="which LLVM archive: windows makes a zip, linux a tar.xz (--compression is for the zip)")
    k.add_argument("--archive", required=True)
    k.add_argument("--source-url", required=True)
    k.add_argument("--source-sha256", required=True)
    k.add_argument("--llvm-version", required=True)
    k.add_argument("--name", required=True)
    k.add_argument("--version", required=True)
    k.add_argument("--licenses", required=True)
    k.add_argument("--extra", help="linux: a folder of files the workflow made, packed under their own paths")
    k.add_argument("--extra-note", default="", help="linux: a line for the README about --extra's files")
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
