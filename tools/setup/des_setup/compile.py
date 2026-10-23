"""The compile: the lift built into des_game.dll, as journal steps (docs/81, docs/57).

  gen          des_game_title_funcs.h (from scripts/lift_ppu.json and port/insn_hooks.txt) and
               the SPU workload table without its static constructor  -> build/gen
  cc:<name>    one per object, in parallel                            -> build/obj/<name>.obj
  lib:<name>   the import libraries the link reads (lld-link /lib)    -> build/gen/<name>.lib
  link         the linker                                             -> game/des_game.dll

The commands are CMake's own for des_game (DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON:
no Visual C++ headers, Windows SDK or C runtime, docs/84), read from
game_build.json, which scripts/game_build_desc.py writes from such a build. The
placeholders in it: {src} the source tree (the install mirrors it), {work} the
lift, {gen} build/gen.

An object's fingerprint covers its command, the compiler, its source's sum and
the sums of the headers it read the last time (clang's dependency file), so a
changed header rebuilds only the objects that read it. The largest sources
start first. The job count follows the free memory (docs/52 4.4: 3 GB a job,
2 GB kept free), and a job that runs out of memory is tried once more with one
job fewer.

Everything written is game-derived and stays in the data folder.
"""
import ctypes
import glob as globmod
import hashlib
import json
import os
import re
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import fetch, journal

SRC = Path(__file__).resolve().parents[3]          # the source tree: tools/setup/des_setup/compile.py
DESC = Path(__file__).resolve().parent / "game_build.json"
GB = 1 << 30
OOM = re.compile(r"out of memory|not enough memory|bad_alloc|std::bad_alloc", re.I)
OOM_CODES = {0xC0000017, 0xC000012D}               # STATUS_NO_MEMORY, STATUS_COMMITMENT_LIMIT
LLVM_ENTRY = "llvm-win64"                          # toolchain.json's compiler (docs/86)


def sha_file(p) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def parse_depfile(text: str) -> list:
    """The prerequisites of a make-style dependency file, as clang writes it:
    'target: dep dep \\' with continuation lines, spaces escaped as '\\ ' and
    '$' as '$$'. Drive letters keep their colon (the target ends at ': ')."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    m = re.search(r":(?:\s|$)", text)
    if not m:
        return []
    body, deps, cur, i = text[m.end():], [], [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body) and body[i + 1] in " #":
            cur.append(body[i + 1])
            i += 2
            continue
        if c == "$" and body[i + 1:i + 2] == "$":
            cur.append("$")
            i += 2
            continue
        if c.isspace():
            if cur:
                deps.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        deps.append("".join(cur))
    return deps


def free_memory() -> int:
    """Available physical memory in bytes, or 0 when unknown."""
    try:
        if os.name == "nt":
            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("tpage", ctypes.c_ulonglong),
                            ("apage", ctypes.c_ulonglong), ("tvirt", ctypes.c_ulonglong),
                            ("avirt", ctypes.c_ulonglong), ("aext", ctypes.c_ulonglong)]
            ms = MS()
            ms.len = ctypes.sizeof(MS)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
                return int(ms.avail)
        else:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemAvailable:"):
                        return int(line.split()[1]) * 1024
    except (OSError, AttributeError, ValueError):
        pass
    return 0


def default_jobs(cpus=None, free=None) -> int:
    """docs/52 4.4: min(logical CPUs, (free RAM - 2 GB) / 3 GB), at least 1."""
    cpus = cpus or os.cpu_count() or 4
    free = free_memory() if free is None else free
    if not free:
        return max(1, cpus // 2)
    return max(1, min(cpus, (free - 2 * GB) // (3 * GB)))


def title_funcs(src: Path) -> str:
    """des_game_title_funcs.h, as port/CMakeLists.txt writes it (DES_SPLIT): the
    port functions the lift calls by name, from the overrides and the hooks."""
    names = []
    flags = json.loads((src / "scripts" / "lift_ppu.json").read_text(encoding="utf-8"))
    for ov in flags.get("override_funcs", []):
        m = re.search(r"=[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*$", ov)
        if m:
            names.append(m.group(1))
    for line in (src / "port" / "insn_hooks.txt").read_text(encoding="utf-8").split("\n"):
        m = re.match(r"^[ \t]*0x[0-9A-Fa-f]+[ \t]*=[ \t]*([A-Za-z_][A-Za-z0-9_]*)", line)
        if m:
            names.append(m.group(1))
    names = list(dict.fromkeys(names))
    text = ("/* Generated by tools/setup (des_setup/compile.py), as port/CMakeLists.txt does for DES_SPLIT, "
            "from scripts/lift_ppu.json and port/insn_hooks.txt. */\n")
    text += "#pragma once\n#define DES_GAME_TITLE_FUNCS(X)"
    for n in names:
        text += f" \\\n    X({n})"
    return text + "\n"


def split_workloads(text: bytes, ps3recomp: Path) -> bytes:
    """The SPU workload table as port/CMakeLists.txt copies it for the DLL: no
    static constructor (des_game_init hands des_spu_register_all to the exe),
    and the runtime header included by absolute path."""
    text = text.replace(b"__attribute__((constructor)) static void des_spu_autoreg(void)",
                        b"[[maybe_unused]] static void des_spu_autoreg(void)")
    text = text.replace(b'#include "../../runtime/spu/',
                        b'#include "' + ps3recomp.as_posix().encode("utf-8") + b"/runtime/spu/")
    if b"constructor" in text:
        raise journal.StepFailed("gen", "the SPU workload table still has a static constructor")
    return text


def local_toolchain(cc=None, ld=None):
    """A developer's compiler, (clang-cl, lld-link): cc and ld when given, else
    DES_SETUP_CC and DES_SETUP_LD from the environment; (None, None) for the
    pinned download. The fetch phase skips the download when there is one."""
    return (cc or os.environ.get("DES_SETUP_CC") or None), (ld or os.environ.get("DES_SETUP_LD") or None)


def toolchain(data: Path, cc=None, ld=None):
    """(clang-cl command, lld-link): a developer's (local_toolchain, both or
    neither), else the pinned LLVM download unpacked under data/tools (fetched
    first if it is not). The download has clang.exe only, which is clang-cl with
    --driver-mode=cl."""
    cc, ld = local_toolchain(cc, ld)
    if cc or ld:
        if not (cc and ld):
            raise fetch.FetchError("--cc and --ld (DES_SETUP_CC and DES_SETUP_LD) go together")
        return [str(cc)], Path(ld)
    e = next((t for t in fetch.load_manifest()["tools"] if t["name"] == LLVM_ENTRY), None)
    if e is None:
        raise fetch.FetchError("no compiler is pinned yet: pass --cc and --ld")
    root = fetch.ensure(e, Path(data) / "downloads", Path(data) / "tools")
    return [str(root / "bin" / "clang.exe"), "--driver-mode=cl"], root / "bin" / "lld-link.exe"


class Obj:
    def __init__(self, name, src: Path, args: list):
        self.name, self.src, self.args = name, src, args
        self.step = f"cc:{name}"


class Compile:
    def __init__(self, data: Path, jr: journal.Journal, cc, ld, src: Path = SRC, desc: Path = DESC, jobs: int = 0,
                 progress=None):
        """cc and ld: the compiler and the linker, each a path or a command list."""
        self.data, self.jr, self.src = Path(data), jr, Path(src)
        self.cc = [str(cc)] if isinstance(cc, (str, Path)) else [str(c) for c in cc]
        self.ld = [str(ld)] if isinstance(ld, (str, Path)) else [str(c) for c in ld]
        self.desc = json.loads(Path(desc).read_text(encoding="utf-8"))
        self.work = self.data / "work"
        self.gen = self.data / "build" / "gen"
        self.objdir = self.data / "build" / "obj"
        self.out = self.data / "game"
        self.logs = self.data / "setup" / "logs"
        self.jobs = jobs or default_jobs()
        self.progress = progress or (lambda step, done, total: None)
        self._sums, self._sums_lock = {}, threading.Lock()

    # ---- the description ----
    def expand(self, s: str) -> str:
        return (s.replace("{src}", self.src.as_posix()).replace("{work}", self.work.as_posix())
                .replace("{gen}", self.gen.as_posix()))

    def plan(self) -> list:
        """The objects, in the description's order (sources of a glob sorted by
        name, as CMake's glob is). A missing lift output is an error: run the lift."""
        objs = []
        for o in self.desc["objects"]:
            args = [self.expand(a) for a in o["args"]]
            if "glob" in o:
                found = sorted(globmod.glob(self.expand(o["glob"])))
                if not found:
                    raise journal.StepFailed("compile", f"no {o['name']} sources ({o['glob']}): run the lift first")
                for f in found:
                    p = Path(f)
                    n = p.stem if o["name"] == "ppu" else p.parent.name
                    objs.append(Obj(n, p, args))
            else:
                p = Path(self.expand(o["src"]))
                if not p.is_file() and not p.is_relative_to(self.gen):
                    raise journal.StepFailed("compile", f"{p} is missing: run the lift first")
                objs.append(Obj(o["name"], p, args))
        if len({o.name for o in objs}) != len(objs):
            raise journal.StepFailed("compile", "two objects with the same name")
        return objs

    def missing_headers(self) -> list:
        return [h for h in self.desc.get("headers", []) if not Path(self.expand(h)).is_file()]

    # ---- sums ----
    def _sum(self, p) -> str:
        p = os.path.normcase(os.path.abspath(p))
        with self._sums_lock:
            if p in self._sums:
                return self._sums[p]
        try:
            s = sha_file(p)
        except OSError:
            s = None
        with self._sums_lock:
            self._sums[p] = s
        return s

    def _tool_id(self, cmd) -> str:
        """The archive a downloaded tool was unpacked from (fetch.ensure's stamp),
        or the sum of its executable: the command's last part that is a file
        (clang.exe in [clang.exe, --driver-mode=cl]; the script a launcher runs)."""
        exe = next((Path(c) for c in reversed(cmd) if Path(c).is_file()), Path(cmd[0]))
        for d in (exe.parent.parent, exe.parent):
            st = d / ".des-setup-sha256"
            if st.is_file():
                return "archive:" + st.read_text().strip()
        return "file:" + (self._sum(exe) or "missing")

    # ---- the steps ----
    def run_all(self) -> Path:
        miss = self.missing_headers()
        if miss:
            raise journal.StepFailed("compile", f"the install is missing {len(miss)} header(s): {', '.join(miss[:5])}")
        self._gen()
        objs = self.plan()
        cc_id, ld_id = self._tool_id(self.cc), self._tool_id(self.ld)
        self._compile_all(objs, cc_id)
        self._libs(ld_id)
        return self._link(objs, ld_id)

    def _gen(self):
        wl_in = self.work / "spu" / "_gen" / "spu_workloads.cpp"
        tables = [self.src / "scripts" / "lift_ppu.json", self.src / "port" / "insn_hooks.txt"]
        if not wl_in.is_file():
            raise journal.StepFailed("gen", f"{wl_in} is missing: run the lift first")
        fp = journal.fingerprint("gen", 1, [self._sum(p) for p in tables + [wl_in]], self.src.as_posix())
        outs = [self.gen / "des_game_title_funcs.h", self.gen / "spu_workloads.cpp"]

        def run():
            self.gen.mkdir(parents=True, exist_ok=True)
            wl = split_workloads(wl_in.read_bytes(), self.src / "ps3recomp")
            for p, data in ((outs[0], title_funcs(self.src).encode("utf-8")), (outs[1], wl)):
                tmp = p.with_name(p.name + ".tmp")
                tmp.write_bytes(data)
                os.replace(tmp, p)
            with self._sums_lock:
                self._sums.clear()
            return {}
        self.jr.run("gen", fp, run, outputs=outs)

    def _obj_fp(self, o, cc_id, deps):
        return journal.fingerprint("cc", o.name, self.cc, o.args, cc_id, self._sum(o.src),
                                   [(d, self._sum(d)) for d in deps])

    def _compile_all(self, objs, cc_id):
        self.objdir.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        todo = []
        for o in objs:
            prev = self.jr.state(o.step).get("results", {}).get("deps", [])
            fp = self._obj_fp(o, cc_id, prev)
            if not self.jr.is_done(o.step, fp, [self.objdir / f"{o.name}.obj"]):
                todo.append((o, fp))
        total, done = len(objs), len(objs) - len(todo)
        self.progress("compile", done, total)
        if not todo:
            return
        todo.sort(key=lambda t: -(t[0].src.stat().st_size if t[0].src.is_file() else 0))
        slots = _Slots(min(self.jobs, len(todo)))
        stop, lock, failed = threading.Event(), threading.Lock(), []
        count = [done]

        def one(o, fp):
            if stop.is_set():
                return
            try:
                self.jr.run(o.step, fp, lambda: self._cc(o, slots, stop),
                            outputs=[self.objdir / f"{o.name}.obj"], log=self._log(o.step),
                            refingerprint=lambda res: self._obj_fp(o, cc_id, res["deps"]))
            except journal.StepFailed as e:
                stop.set()
                with lock:
                    failed.append(e)
                return
            with lock:
                count[0] += 1
                self.progress("compile", count[0], total)

        with ThreadPoolExecutor(max_workers=min(self.jobs, len(todo))) as ex:
            for o, fp in todo:
                ex.submit(one, o, fp)
        if failed:
            raise next((e for e in failed if not e.message.startswith("stopped")), failed[0])

    def _log(self, step) -> Path:
        return self.logs / (step.replace(":", "_") + ".log")

    def _cc(self, o, slots, stop):
        obj = self.objdir / f"{o.name}.obj"
        dep = self.objdir / f"{o.name}.obj.d"
        cmd = self.cc + o.args + [f"/Fo{obj}", "-clang:-MD", f"-clang:-MF{dep}", "-clang:-fno-caret-diagnostics",
                                  "-c", "--", str(o.src)]
        self._log(o.step).write_text("", encoding="utf-8")
        for attempt in (0, 1):
            with slots:
                if stop.is_set():
                    raise journal.StepFailed(o.step, "stopped: another object failed", self._log(o.step))
                rc, out = self._exec(o.step, cmd, self.objdir)
            if rc == 0:
                break
            oom = bool(OOM.search(out)) or (rc & 0xFFFFFFFF) in OOM_CODES
            if attempt == 0 and oom:
                slots.shrink()
                continue
            tail = " | ".join(out.strip().splitlines()[-5:])
            why = "the compiler ran out of memory" if oom else "the compiler failed"
            raise journal.StepFailed(o.step, f"{why} (exit {rc}): {tail}", self._log(o.step))
        try:
            deps = parse_depfile(dep.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            raise journal.StepFailed(o.step, f"the compiler wrote no dependency file {dep}", self._log(o.step))
        src = os.path.normcase(os.path.abspath(o.src))
        return {"deps": sorted({os.path.abspath(d) for d in deps if os.path.normcase(os.path.abspath(d)) != src})}

    def _exec(self, step, cmd, cwd):
        log = self._log(step)
        p = subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace")
        with open(log, "a", encoding="utf-8", errors="replace") as lf:
            lf.write("+ " + subprocess.list2cmdline(cmd) + "\n" + p.stdout + f"(exit {p.returncode})\n")
        return p.returncode, p.stdout

    def _link(self, objs, ld_id) -> Path:
        dll = self.out / self.desc.get("dll", "des_game.dll")
        args = []
        for a in self.desc["link"]:
            if a == "{objects}":
                args += [str(self.objdir / f"{o.name}.obj") for o in objs]
            else:
                args.append(self.expand(a))
        fps = [(o.name, self.jr.state(o.step).get("inputs")) for o in objs] + self._lib_fps()
        fp = journal.fingerprint("link", self.ld, args, ld_id, fps)

        def run():
            self.out.mkdir(parents=True, exist_ok=True)
            self.logs.mkdir(parents=True, exist_ok=True)
            self._log("link").write_text("", encoding="utf-8")
            rc, out = self._exec("link", self.ld + args, self.out)
            if rc != 0:
                tail = " | ".join(out.strip().splitlines()[-5:])
                raise journal.StepFailed("link", f"the linker failed (exit {rc}): {tail}", self._log("link"))
            return {"dll": str(dll), "size": dll.stat().st_size}
        self.jr.run("link", fp, run, outputs=[dll], log=self._log("link"))
        return dll

    def _libs(self, ld_id):
        """The import libraries the link reads, each made by the linker from a .def file (lld-link /lib), so the
        link needs no Windows SDK (docs/84 3.2): a journal step each, redone when its command or .def changes."""
        for lib in self.desc.get("libs", []):
            step = f"lib:{lib['name']}"
            args = [self.expand(a) for a in lib["args"]]
            out = next((Path(a[5:]) for a in args if a.lower().startswith("/out:")), None)
            defs = [Path(a[5:]) for a in args if a.lower().startswith("/def:")]
            if out is None:
                raise journal.StepFailed(step, "the description names no /out: for it")
            miss = [d.as_posix() for d in defs if not d.is_file()]
            if miss:
                raise journal.StepFailed(step, f"the install is missing {', '.join(miss)}")
            fp = journal.fingerprint("lib", self.ld, args, ld_id, [self._sum(d) for d in defs])

            def run(step=step, args=args, out=out):
                out.parent.mkdir(parents=True, exist_ok=True)
                self.logs.mkdir(parents=True, exist_ok=True)
                self._log(step).write_text("", encoding="utf-8")
                rc, text = self._exec(step, self.ld + args, out.parent)[:2]
                if rc != 0:
                    tail = " | ".join(text.strip().splitlines()[-5:])
                    raise journal.StepFailed(step, f"the linker failed to make {out.name} (exit {rc}): {tail}",
                                             self._log(step))
                return {}
            self.jr.run(step, fp, run, outputs=[out], log=self._log(step))

    def _lib_fps(self) -> list:
        return [(lib["name"], self.jr.state(f"lib:{lib['name']}").get("inputs")) for lib in self.desc.get("libs", [])]


class _Slots:
    """A semaphore whose size can only go down (a job that ran out of memory)."""
    def __init__(self, n):
        self.n, self.used, self.cond = max(1, n), 0, threading.Condition()

    def __enter__(self):
        with self.cond:
            while self.used >= self.n:
                self.cond.wait()
            self.used += 1

    def __exit__(self, *exc):
        with self.cond:
            self.used -= 1
            self.cond.notify_all()

    def shrink(self):
        with self.cond:
            self.n = max(1, self.n - 1)
