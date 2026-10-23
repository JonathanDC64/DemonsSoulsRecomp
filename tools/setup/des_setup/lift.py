"""The lift: the game's code converted to C, as journal steps.

  loader       ps3recomp/tools/ppu_loader.py     inputs/.../EBOOT.elf -> work/loader
  ppu          ps3recomp/tools/ppu_lifter.py     -> work/recomp_raw (flags: scripts/lift_ppu.json)
  patches      scripts/apply_boot_patches.py     work/recomp_raw copied to work/recomp, then patched
  nids         ps3recomp/tools/gen_hle_nids.py   -> work/recomp/hle_nid_table.cpp
  spu-cut      scripts/build_spu.py --eboot      -> work/spu_images, work/spu_jobs (checked by scripts/spu_table.json)
  spu:<name>   scripts/build_spu.py --only       -> work/spu/<name>, one step per image or job
  libsre       scripts/build_libsre.py           -> work/libsre, work/gen/libsre_glue.c (with the firmware file only)

The SPU steps and libsre run on a thread beside the first four (run_all).
Everything written is game- or firmware-derived and stays in the data folder.
Each step's fingerprint covers the sums of its input files, of every script
of the source tree that does the lifting, and its arguments; so a new release
redoes the lift, and a lift interrupted anywhere carries on at the step it
was in. The patches run on a copy, so a rerun never patches twice.

The tools run under this Python in UTF-8 mode (-X utf8): the lifter opens its
outputs without an encoding, so the code page would otherwise pick it
(docs/52). Each step's output goes to setup/logs/<step>.log.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
from pathlib import Path

from . import journal

SRC = Path(__file__).resolve().parents[3]          # the source tree: tools/setup/des_setup/lift.py
EBOOT = "PS3_GAME/USRDIR/EBOOT.elf"                  # its place under inputs/ (gamefiles' name)
PROGRESS = re.compile(r"lifted (\d+)/(\d+) functions")


def _sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def toolset(src: Path = SRC) -> str:
    """One sum over the scripts and tables that do the lifting: ps3recomp's
    tools, this tree's scripts and the instruction hooks."""
    files = sorted(list((src / "ps3recomp" / "tools").rglob("*.py")) + list((src / "scripts").glob("*.py")) +
                   list((src / "scripts").glob("*.json")) + [src / "port" / "insn_hooks.txt"])
    files = [f for f in files if "__pycache__" not in f.parts and f.is_file()]
    return journal.fingerprint([(str(f.relative_to(src)).replace("\\", "/"), _sha_file(f)) for f in files])


class _Stopped(Exception):
    """The PPU side failed: the steps beside it end."""


class _Side:
    """The steps beside the PPU steps (run_all): their progress held until go_live, then told as it comes; stopped
    between steps when the PPU side fails; their own failure kept for run_all to raise."""

    def __init__(self, progress):
        self._progress, self._lock = progress, threading.Lock()
        self._live, self._last, self._stop = False, None, False
        self.failed = None

    def progress(self, step, done, total):
        with self._lock:
            if self._stop:
                raise _Stopped()
            if self._live:
                self._progress(step, done, total)
            else:
                self._last = (step, done, total)

    def go_live(self):
        with self._lock:
            self._live = True
            if self._last and not self._stop:
                self._progress(*self._last)

    def stop(self):
        with self._lock:
            self._stop = True

    def run(self, fn):
        try:
            fn()
        except _Stopped:
            pass
        except Exception as e:  # (raised again on run_all's thread)
            self.failed = e


class Lift:
    def __init__(self, data: Path, jr: journal.Journal, src: Path = SRC, workers: int = 0, progress=None):
        self.data, self.jr, self.src = Path(data), jr, Path(src)
        self.work = self.data / "work"
        self.logs = self.data / "setup" / "logs"
        self.inputs = self.data / "inputs"
        self.workers = workers or min(16, os.cpu_count() or 4)
        self.progress = progress or (lambda step, done, total: None)
        self.tools = toolset(self.src)

    # ---- running a tool ----
    def _run(self, step, args, cwd=None, parse=None):
        """Run one of the source tree's scripts under this Python; its output to
        the step's log. parse(line) is called on every line."""
        self.logs.mkdir(parents=True, exist_ok=True)
        log = self.logs / (step.replace(":", "_") + ".log")
        cmd = [sys.executable, "-X", "utf8"] + [str(a) for a in args]
        with open(log, "w", encoding="utf-8", errors="replace") as lf:
            lf.write("+ " + " ".join(cmd) + "\n")
            lf.flush()
            with subprocess.Popen(cmd, cwd=str(cwd or self.src), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding="utf-8", errors="replace") as p:
                for line in p.stdout:
                    lf.write(line)
                    if parse:
                        parse(line)
                rc = p.wait()
        if rc != 0:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-5:]
            raise journal.StepFailed(step, f"{Path(str(args[0])).name} failed (exit {rc}): " + " | ".join(tail), log)

    def _step(self, name, inputs, args, fn, outputs):
        fp = journal.fingerprint(name, self.tools, sorted((str(k), v) for k, v in inputs.items()), args)
        return self.jr.run(name, fp, fn, outputs=outputs, log=self.logs / (name.replace(":", "_") + ".log"))

    # ---- the steps ----
    def run_all(self, eboot_sha: str, libsre_sha: str = None):
        """Every step. eboot_sha (and libsre_sha) are the staged files' sums
        from the game check, so the inputs are not hashed again.

        The SPU steps and libsre read only the EBOOT and the firmware, so they
        run on a thread of their own beside the PPU steps (loader, ppu,
        patches, nids), which they used to wait for: the PPU lift's 35 s are
        half one thread's analysis. Their progress is told once the PPU steps
        are done, so the screens show one lift row at a time, as before. When
        both sides fail, the PPU side's failure is the one raised."""
        eboot = self.inputs / EBOOT
        if not eboot.is_file():
            raise journal.StepFailed("lift", f"{eboot} is missing: run the game check first")
        side = _Side(self.progress)
        thread = threading.Thread(target=side.run, args=(lambda: self._side_steps(eboot, eboot_sha, libsre_sha,
                                                                                   side.progress),), daemon=True)
        thread.start()
        try:
            self._ppu_steps(eboot, eboot_sha)
        except BaseException:
            side.stop()                            # (it ends after the step it is in)
            raise
        finally:
            side.go_live()
            thread.join()
        if side.failed is not None:
            raise side.failed

    def _ppu_steps(self, eboot, eboot_sha):
        ps3r, w = self.src / "ps3recomp", self.work
        loader = w / "loader"
        self._step("loader", {"eboot": eboot_sha}, [], lambda: self._run(
            "loader", [ps3r / "tools" / "ppu_loader.py", eboot, "-o", loader]),
            [loader / "EBOOT.functions.json", loader / "EBOOT.imports.json"])

        flags = json.loads((self.src / "scripts" / "lift_ppu.json").read_text(encoding="utf-8"))
        raw = w / "recomp_raw"
        args = [ps3r / "tools" / "ppu_lifter.py", eboot,
                "--functions", loader / "EBOOT.functions.json", "--hle-stubs", loader / "EBOOT.imports.json",
                "--override-funcs", ",".join(flags["override_funcs"]),
                "--insn-hooks", "@" + str(self.src / flags["insn_hooks"])] + \
               (["--shadow-stack"] if flags.get("shadow_stack") else []) + \
               ["--output", raw, "-j", self.workers]

        def parse(line):
            m = PROGRESS.search(line)
            if m:
                self.progress("ppu", int(m.group(1)), int(m.group(2)))

        # The flags, not the command line: the worker count does not change the output.
        self._step("ppu", {"eboot": eboot_sha}, flags,
                   lambda: self._run("ppu", args, parse=parse), [raw / "ppu_recomp.h"])

        rec = w / "recomp"

        def patch():
            if rec.exists():
                shutil.rmtree(rec)
            shutil.copytree(raw, rec)
            self._run("patches", [self.src / "scripts" / "apply_boot_patches.py", rec])
        self._step("patches", {"eboot": eboot_sha}, [], patch, [rec / "ppu_recomp.h"])

        nid = rec / "hle_nid_table.cpp"
        self._step("nids", {}, [], lambda: self._run(
            "nids", [ps3r / "tools" / "gen_hle_nids.py", "--all", "--out", nid]), [nid])

    def _side_steps(self, eboot, eboot_sha, libsre_sha, progress):
        ps3r, w = self.src / "ps3recomp", self.work
        imgs, jobs = w / "spu_images", w / "spu_jobs"
        build_spu = self.src / "scripts" / "build_spu.py"
        table = json.loads((self.src / "scripts" / "spu_table.json").read_text(encoding="utf-8"))
        names = sorted(table["images"]) + sorted(table["jobs"])
        self._step("spu-cut", {"eboot": eboot_sha}, [], lambda: self._run(
            "spu-cut", [build_spu, "--eboot", eboot, "--elf-dir", imgs, "--job-dir", jobs, "--ps3recomp", ps3r,
                        "--out-dir", w / "spu", "--gen-only"]),
            [imgs / f"{n}.elf" for n in table["images"]] + [jobs / f"{n}.bin" for n in table["jobs"]] +
            [w / "spu" / "_gen" / "spu_workloads.cpp"])
        for k, n in enumerate(names):
            progress("spu", k, len(names))
            fp_in = table["images"].get(n) or table["jobs"][n]
            self._step(f"spu:{n}", {"image": fp_in["fnv"]}, [], lambda n=n: self._run(
                f"spu:{n}", [build_spu, "--elf-dir", imgs, "--job-dir", jobs, "--ps3recomp", ps3r,
                             "--out-dir", w / "spu", "--only", n]),
                [w / "spu" / n / "spu_recomp.c"])
        progress("spu", len(names), len(names))

        fw = self.inputs / "firmware" / "libsre.prx"
        if libsre_sha and fw.is_file():
            lw, glue = w / "libsre", w / "gen" / "libsre_glue.c"
            self._step("libsre", {"libsre": libsre_sha}, [str(lw)], lambda: self._run(
                "libsre", [self.src / "scripts" / "build_libsre.py", fw, "--ps3recomp", ps3r, "--work", lw,
                           "--glue", glue, "--runtime-image", lw / "libsre_image.bin"]),
                [lw / "lifted" / "ppu_recomp.c", glue])
