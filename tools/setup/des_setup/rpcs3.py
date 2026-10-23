"""The RPCS3 route's unlock (docs/90 sections 2 and 3.4): the player's own RPCS3
decrypts copies of the files the build needs, with `rpcs3 --decrypt`.

Setup ships no keys and no decryptor. The unlock copies each original into
<data>/setup/unlock/in/ (nothing is ever decrypted in the dump), starts RPCS3
once with one `--decrypt <file>` pair for each copy, and gives it a folder of
setup's own, <data>/setup/unlock/rpcs3/, for everything a start of RPCS3
writes (its log, lock, config and dev_hdd0 tree). So nothing is written in the
player's RPCS3 folder, and an RPCS3 the player has open is not disturbed.
RPCS3's exit status is 0 after a failure too, so success is each output's
size and SHA-256 against game_table.json. A file that did not come out right
gets one more run on its own, which also names the one that fails.

A `portable` folder beside the program wins over that redirect (RPCS3 checks
it first), so such an install is "shared": setup starts it in its own folder,
and only while it is closed (docs/90 section 6, question 1 (a)).

The firmware's libsre.prx comes the same way when it is missing: a copy of
the player's <dev_flash>/sys/external/libsre.sprx goes in with the game's
files, and RPCS3 writes libsre.prx beside it (decrypt_binaries.cpp names the
output of a .sprx <name>.prx).

Of the player's RPCS3 files, only the first line of its log, PPU Debug's
dumps of the EBOOT and libsre.sprx are read here (rpcs3_library.py reads the
rest), and nothing is written there.
"""
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from . import firmware, gamefiles, journal

STALL = 30.0                  # seconds without a new or growing output before the watchdog ends RPCS3 (docs/90 2.7)
POLL = 0.25                   # how often the outputs are looked at
ABORTS = (3, 0xC0000409, -6)  # abort() and a fast fail on Windows, SIGABRT on Linux: RPCS3's fatal errors
SDAT_FLAG = 0x01000000        # in the big-endian flags word at 0x80 of an NPD file
SHARED = "rpcs3_shared.txt"   # in setup/: the RPCS3 programs that ignored the redirect once (docs/90 2.5)
OUTPUT = "rpcs3-output.txt"   # in the unlock folder: what RPCS3 printed before it took the console
# Qt then prints "Unknown option" on stderr instead of showing a message box (Windows).
NO_MESSAGE_BOXES = {"QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES": "1"}


class UnlockFailed(journal.StepFailed):
    """RPCS3 could not unlock the files. `message` is the sentence for the player (docs/90 1.10)."""


@dataclass
class Job:
    """One file to unlock: the original in the dump, and its two entries in game_table.json (for the firmware's
    file: the player's libsre.sprx, and its "firmware" entry)."""
    src: Path           # the original, where the check found it
    orig: dict          # its "originals" entry
    dec: dict           # the "decrypted" entry made from it
    copy: Path = None   # its copy in the unlock folder, and the output RPCS3 writes beside it (Unlock sets them)
    out: Path = None
    kind: str = "decrypted"     # or "firmware"

    @property
    def name(self) -> str:
        """The original's path under USRDIR, as the lines and sentences name it."""
        return usrdir(self.orig["path"])

    @property
    def out_name(self) -> str:
        return usrdir(self.dec["path"])


def usrdir(path: str) -> str:
    """A table path under USRDIR: "PS3_GAME/USRDIR/chr/m02.chresdbnd.dcx.sdat" is "chr/m02.chresdbnd.dcx.sdat"."""
    return path.split("USRDIR/", 1)[-1]


def different(name: str, kind="decrypted") -> str:
    """The sentence for an output that is not the known good one."""
    if kind == "firmware":
        return (f"{name} came out different from the known good version: your RPCS3's PS3 system software may be "
                f"another version.")
    return f"{name} came out different from the known good version."


def firmware_job(sprx: Path, entry: dict) -> Job:
    """The firmware's file as a job: a copy of the player's libsre.sprx in, and the libsre.prx RPCS3 writes beside it
    out (decrypt_binaries.cpp: a .sprx gives <name>.prx). Its own size and SHA-256, taken now, check the copy."""
    sprx = Path(sprx)
    out = sprx.name[:sprx.name.rfind(".")] + ".prx"
    orig = {"path": sprx.name, "size": sprx.stat().st_size, "sha256": gamefiles.sha256_file(sprx)}
    return Job(sprx, orig, dict(entry, path=out), kind="firmware")


# ---- the player's RPCS3 (read only) ----

def find(path):
    """The RPCS3 program: `path` itself, the one in that folder, or a command on
    PATH. None when there is none."""
    if not path:
        return None
    p = Path(path)
    if p.is_file():
        return p.absolute()
    if p.is_dir():
        hit = next((p / n for n in _program_names(p) if (p / n).is_file()), None)
        return hit.absolute() if hit else None
    hit = shutil.which(str(path))
    return Path(hit).absolute() if hit else None


def _program_names(folder: Path) -> list:
    """What RPCS3's program is called in its folder: rpcs3.exe on Windows; rpcs3, or an AppImage, on Linux."""
    if os.name == "nt":
        return ["rpcs3.exe"]
    try:
        images = sorted(n for n in os.listdir(folder) if n.lower().startswith("rpcs3") and n.endswith(".AppImage"))
    except OSError:
        images = []
    return ["rpcs3"] + images


def where(path) -> str:
    """What the lines and sentences name: the folder of rpcs3.exe (or rpcs3), else the path itself (an AppImage)."""
    p = Path(path)
    return str(p.parent if p.name.lower() in ("rpcs3.exe", "rpcs3") else p)


def is_shared(data, exe) -> bool:
    """Setup cannot give this RPCS3 a folder of its own: a `portable` folder
    beside the program wins over the environment (File.cpp get_config_dir), or
    this program ignored the redirect before (an old build). Starting it then
    replaces its last log, as any start does (docs/90 2.5)."""
    return (Path(exe).parent / "portable").is_dir() or _key(exe) in _shared_list(data)


def remember_shared(data, exe):
    if _key(exe) not in _shared_list(data):
        p = Path(data) / "setup" / SHARED
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "a", encoding="utf-8") as f:
            f.write(_key(exe) + "\n")


def _shared_list(data) -> set:
    try:
        return set((Path(data) / "setup" / SHARED).read_text(encoding="utf-8").splitlines())
    except OSError:
        return set()


def _key(p) -> str:
    return os.path.normcase(os.path.realpath(p))


def player_dirs(exe) -> dict:
    """Where the player's RPCS3 keeps its config, its cache (PPU Debug's dumps)
    and its log, worked out as File.cpp does: `portable/` beside the program
    first; on Windows then RPCS3_CONFIG_DIR, cut after its last separator, then
    the program's folder; on Linux the XDG folders."""
    home = Path(exe).parent
    portable = home / "portable"
    if os.name == "nt":
        cfg = portable
        if not cfg.is_dir():
            v = os.environ.get("RPCS3_CONFIG_DIR", "").replace("\\", "/")
            cfg = Path(v[:v.rfind("/") + 1]) if "/" in v else home
        return {"config": cfg, "cache": cfg, "log": cfg / "log"}
    env, user = os.environ, Path.home()
    cfg = portable if portable.is_dir() else Path(env.get("XDG_CONFIG_HOME") or user / ".config") / "rpcs3"
    cache = Path(env.get("XDG_CACHE_HOME") or env.get("XDG_CONFIG_HOME") or user / ".cache") / "rpcs3"
    return {"config": cfg, "cache": cache, "log": cache}


def ppu_debug_eboots(exe, title_id) -> list:
    """PPU Debug's dumps of the game's EBOOT (System.cpp dump_executable):
    <cache>/ppu_progs/<title>-<module's SHA-1 in base57>-EBOOT.BIN/exec.elf.
    The check takes one whose size and SHA-256 are the table's EBOOT.elf."""
    root = player_dirs(exe)["cache"] / "ppu_progs"
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return []
    pre = f"{title_id}-".lower()
    return [root / n / "exec.elf" for n in names
            if n.lower().startswith(pre) and n.lower().endswith("-eboot.bin") and (root / n / "exec.elf").is_file()]


def version(exe) -> str:
    """RPCS3's version, from the first line of the player's own log (rpcs3.exe's version resource is empty), or ''."""
    try:
        with open(player_dirs(exe)["log"] / "RPCS3.log", "rb") as f:
            return f.readline(300).decode("utf-8", "replace").strip()
    except OSError:
        return ""


def running(exe) -> bool:
    """A process runs from this program, judged by its image path, so none of
    RPCS3's files is opened (docs/90 I15)."""
    want = _key(exe)
    return any(_key(p) == want for p in _images())


def _images() -> list:
    """The image path of every process this user may ask about: Windows'
    QueryFullProcessImageNameW, Linux's /proc/<pid>/exe."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        k = _kernel32()
        n = 1024
        while True:
            pids, got = (wintypes.DWORD * n)(), wintypes.DWORD()
            if not k.K32EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got)):
                return []
            if got.value < ctypes.sizeof(pids):
                break
            n *= 2
        out, buf = [], ctypes.create_unicode_buffer(32768)
        for pid in pids[:got.value // ctypes.sizeof(wintypes.DWORD)]:
            h = k.OpenProcess(0x1000, False, pid)            # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                continue
            try:
                size = wintypes.DWORD(len(buf))
                if k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                    out.append(buf.value)
            finally:
                k.CloseHandle(h)
        return out
    out = []
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except OSError:
        return out
    for pid in pids:
        try:
            out.append(os.readlink(f"/proc/{pid}/exe"))
        except OSError:
            pass
    return out


_K32 = []


def _kernel32():
    """kernel32 with the prototypes this module uses (Windows)."""
    if not _K32:
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.K32EnumProcesses.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        k.OpenProcess.restype = wintypes.HANDLE
        k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                 ctypes.POINTER(wintypes.DWORD)]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.CreateJobObjectW.restype = wintypes.HANDLE
        k.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        _K32.append(k)
    return _K32[0]


# ---- running RPCS3 ----

def redirect(home: Path):
    """The child's environment that gives RPCS3 setup's folder for everything it
    writes, and the lock file that shows it did (docs/90 2.3, I12)."""
    home = Path(home)
    if os.name == "nt":
        # RPCS3 cuts the value after its last separator, so it must end in one.
        return {"RPCS3_CONFIG_DIR": str(home) + os.sep}, home / "RPCS3.buf"
    return ({"XDG_CONFIG_HOME": str(home / "config"), "XDG_CACHE_HOME": str(home / "cache")},
            home / "cache" / "rpcs3" / "RPCS3.buf")


class _Child:
    """RPCS3, started as a plain child of the engine (docs/90 2.6): stdin from
    the null device, its output to `out` (never the protocol's stdout), no
    window, and in a group of its own, so that end() ends whatever it started."""

    def __init__(self, cmd, env, cwd, out):
        self.job = None
        if os.name == "nt":
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 0                      # SW_HIDE: a console of its own, if it opens one, stays hidden
            # Not DETACHED_PROCESS: RPCS3 attaches to the engine's console, which the host starts without a window.
            # CREATE_NO_WINDOW is ignored for a GUI program such as rpcs3.exe; a console one gets no window either.
            self.p = subprocess.Popen(cmd, env=env, cwd=cwd, stdin=subprocess.DEVNULL, stdout=out,
                                      stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW,
                                      startupinfo=si)
            k = _kernel32()
            job = k.CreateJobObjectW(None, None)
            if job and k.AssignProcessToJobObject(job, int(self.p._handle)):
                self.job = job
            elif job:
                k.CloseHandle(job)
        else:
            self.p = subprocess.Popen(cmd, env=env, cwd=cwd, stdin=subprocess.DEVNULL, stdout=out,
                                      stderr=subprocess.STDOUT, start_new_session=True,
                                      preexec_fn=_parent_death_signal())

    def wait(self, timeout):
        """The exit status, or None while it runs after `timeout` seconds."""
        try:
            return self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def end(self):
        """End RPCS3 and what it started: its job (Windows) or its process group (Linux), never anything by name."""
        if os.name == "nt":
            if self.job:
                _kernel32().TerminateJobObject(self.job, 1)
        else:
            try:
                os.killpg(self.p.pid, 9)            # SIGKILL
            except OSError:
                pass
        if self.p.poll() is None:
            self.p.kill()

    def close(self):
        if self.p.poll() is None:
            self.end()
        self.p.wait()
        if self.job:
            _kernel32().CloseHandle(self.job)
            self.job = None


def _parent_death_signal():
    """Linux: what the child runs before RPCS3, so the kernel ends RPCS3 when the
    engine ends. Its own process group is out of reach of the host's Cancel."""
    try:
        import ctypes
        prctl = ctypes.CDLL(None, use_errno=True).prctl
    except (OSError, AttributeError):
        return None
    return lambda: prctl(1, 9)                      # PR_SET_PDEATHSIG, SIGKILL


@dataclass
class _Ran:
    jobs: list
    code: int
    text: str
    stalled: Job = None


class Unlock:
    """One unlock (docs/90 3.4): the checked copies, one RPCS3 process for all of
    them, then each file that did not come out right once more on its own.

    work: <data>/setup/unlock. jobs: the files, in table order. emit: the
    protocol (protocol.Emitter). log: the step log. shared: run RPCS3 in its own
    folder (no redirect). progress(fraction): the phase's share of the whole
    bar. launch: the command before the arguments (default: the program; the
    tests put a stand-in there). stall: the watchdog's seconds."""

    def __init__(self, exe, work, jobs, emit, log, shared=False, progress=None, launch=None, stall=STALL,
                 poll=POLL):
        self.exe, self.work = Path(exe), Path(work).absolute()
        self.inp, self.home = self.work / "in", self.work / "rpcs3"
        self.jobs = list(jobs)
        for j in self.jobs:
            j.copy, j.out = self.inp / j.orig["path"], self.inp / j.dec["path"]
        self.emit, self.log, self.shared, self.progress = emit, Path(log), shared, progress
        self.launch = list(launch or [str(self.exe)])
        self.stall, self.poll = stall, poll
        self.env, self.lock = redirect(self.home)
        self.ignored = False                        # RPCS3 wrote somewhere other than setup's folder
        self._log = None

    def outputs(self) -> list:
        return [j.out for j in self.jobs]

    def run(self) -> dict:
        """The journal step's work: its results, or UnlockFailed with the player's sentence."""
        self.log.parent.mkdir(parents=True, exist_ok=True)
        self._log = open(self.log, "w", encoding="utf-8", errors="replace")
        try:
            self._note(f"RPCS3: {self.exe}")
            self._note(f"its version: {version(self.exe) or 'unknown (no log of its own)'}")
            self._note("its folder: " + ("its own (a portable install, or one that ignored the redirect)"
                                         if self.shared else f"setup's, {self.home}"))
            self._prepare()
            res = self._unlock()
        except BaseException:
            self.clean(everything=True)
            raise
        finally:
            self._log.close()
        self.clean()
        return res

    def clean(self, everything=False):
        """Delete the encrypted copies and RPCS3's folder (its config, log and
        dev_hdd0 tree); after a failure, the whole unlock folder. The outputs
        stay for the stage, which copies them into inputs/."""
        if everything:
            shutil.rmtree(self.work, ignore_errors=True)
            return
        shutil.rmtree(self.home, ignore_errors=True)
        (self.work / OUTPUT).unlink(missing_ok=True)
        for j in self.jobs:
            j.copy.unlink(missing_ok=True)

    def _note(self, text):
        self._log.write(text.rstrip("\n") + "\n")
        self._log.flush()

    def _fail(self, sentence):
        self._note(f"FAILED: {sentence}")
        raise UnlockFailed("unlock", sentence, self.log)

    def _prepare(self):
        """A fresh folder for RPCS3, and a copy of each original that is checked
        again: its sum, then the header bytes RPCS3 decides by, so RPCS3 never
        reaches its KLIC prompt with these files (docs/90 2.7)."""
        if self.home.exists():
            shutil.rmtree(self.home)
        self.home.mkdir(parents=True)
        for j in self.jobs:
            j.out.unlink(missing_ok=True)
            j.copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(j.src, j.copy)
            why = _not_ready(j)
            if why:
                self._note(why)
                raise journal.StepFailed("unlock", why, self.log)

    def _unlock(self) -> dict:
        why = self._why(self._rpcs3(self.jobs), first=True)
        if why:
            self._fail(why)
        for j in self.jobs:
            if self._good(j):
                continue
            self._note(f"{j.out_name} did not come out right: once more, on its own")
            j.out.unlink(missing_ok=True)
            why = self._why(self._rpcs3([j]))
            if why:
                self._fail(why)
            if not j.out.is_file():
                self._fail(f"RPCS3 could not unlock {j.name}.")
            if not self._good(j):
                self._fail(different(j.out_name, j.kind))
        for j in self.jobs:
            self._note(f"{j.out_name}: {j.out.stat().st_size} bytes, matches")
        return {"files": len(self.jobs), "redirect": "ignored" if self.ignored else "shared" if self.shared else "ok"}

    def _why(self, r: _Ran, first=False) -> str:
        """The sentence for a run of RPCS3 that went wrong as a whole, or '' (then
        each file is looked at). After the first run it also checks that RPCS3
        used setup's folder: its lock is there (docs/90 2.5)."""
        nothing = not any(self._done(j) for j in r.jobs)
        if r.code != 0 and "Unknown option" in r.text:
            return "This RPCS3 is too old to unlock the files. Update RPCS3."
        if nothing and r.code in ABORTS:
            if _key(self.work).startswith(_key(tempfile.gettempdir()) + os.sep):
                self._note("RPCS3 stops when its folder is in a temporary location (docs/90 2.4), and setup's data "
                           "folder is under the temp folder")
            return "RPCS3 could not start."
        if first and not self.shared and not self.lock.is_file():
            self.ignored = True
            self._note(f"RPCS3 ignored {' and '.join(self.env)}: it did not use setup's folder, so it is treated as "
                       f"a shared install from now on")
            if nothing:                     # most likely the "Another instance" dialog of the RPCS3 the player has open
                return "RPCS3 is open. Close it so setup can start it."
        if r.stalled is not None:
            return f"RPCS3 stopped answering while it unlocked {r.stalled.name}."
        if nothing and "Failed to create" in r.text:
            return "RPCS3 could not write in setup's folder. Your antivirus may be blocking it."
        return ""

    def _rpcs3(self, jobs) -> _Ran:
        """One RPCS3 process for `jobs`, in order, watched until it ends. One
        `--decrypt <file>` pair each: the `--decrypt=` form misses RPCS3's
        headless mode (rpcs3.cpp find_arg)."""
        cmd = self.launch + [a for j in jobs for a in ("--decrypt", str(j.copy))]
        env = dict(os.environ, **NO_MESSAGE_BOXES)
        if not self.shared:
            env.update(self.env)
        self._note("$ " + subprocess.list2cmdline(cmd))
        for k in sorted(NO_MESSAGE_BOXES) + ([] if self.shared else sorted(self.env)):
            self._note(f"  {k}={env[k]}")
        out = self.work / OUTPUT
        with open(out, "wb") as f:
            child = _Child(cmd, env, self.work, f)
        try:
            code, stalled = self._watch(child, jobs)
        finally:
            child.close()
        text = out.read_text(encoding="utf-8", errors="replace")
        if text:
            self._note(text)
        self._note(f"exit status {code}" + (f"; stalled on {stalled.name}" if stalled else ""))
        return _Ran(jobs, code, text, stalled)

    def _watch(self, child, jobs):
        """Wait for RPCS3 to end, with the progress lines. RPCS3 handles its
        inputs in order and writes no log in this mode, so progress is a new or
        growing output. After `stall` seconds without any the watchdog ends it,
        and the first file with no output is the one it stalled on."""
        seen, since = None, time.monotonic()
        while True:
            self._report(jobs)
            code = child.wait(self.poll)
            if code is not None:
                self._report(jobs)
                return code, None
            now = tuple(j.out.stat().st_size if j.out.is_file() else -1 for j in jobs)
            if now != seen:
                seen, since = now, time.monotonic()
            elif time.monotonic() - since >= self.stall:
                self._note(f"no new output for {self.stall:g} s: RPCS3 ended")
                child.end()
                return child.wait(None), next((j for j in jobs if not self._done(j)), jobs[-1])

    def _report(self, jobs):
        done = sum(1 for j in self.jobs if self._done(j))
        now = next((j for j in jobs if not self._done(j)), None)
        if now is not None:
            self.emit.file("unlock", now.name)
        self.emit.progress("unlock", done, len(self.jobs))
        if self.progress:
            self.progress(done / max(1, len(self.jobs)))

    def _done(self, j) -> bool:
        """The output is all there: the table's size (or any, when the table has none)."""
        if not j.out.is_file():
            return False
        size = j.out.stat().st_size
        return size == j.dec["size"] if j.dec.get("size") is not None else size > 0

    def _good(self, j) -> bool:
        """The output is the table's: its size and SHA-256, or its first bytes when the table has no sum yet. The
        firmware's file may be from another system software version (firmware.py): the stage makes it the table's."""
        if not self._done(j):
            return False
        if j.dec.get("sha256"):
            if gamefiles.sha256_file(j.out) == j.dec["sha256"]:
                return True
            return j.kind == "firmware" and firmware.matched(j.out, j.dec) is not None
        with open(j.out, "rb") as f:
            return not j.dec.get("magic") or f.read(4) == bytes.fromhex(j.dec["magic"])


def _not_ready(j) -> str:
    """Why a copy is not what RPCS3 must be given, or ''. Its sum is the
    original's in the table. A SELF starts with SCE\\0. An SDAT starts with
    NPD\\0, has the SDAT flag, and its plain size (a big-endian u64 at 0x88) is
    the table's decrypted size."""
    if j.orig.get("sha256") and gamefiles.sha256_file(j.copy) != j.orig["sha256"]:
        return f"The copy of {j.name} for RPCS3 does not match the original."
    with open(j.copy, "rb") as f:
        head = f.read(0x90)
    if not j.orig["path"].lower().endswith(".sdat"):
        return "" if head[:4] == b"SCE\0" else f"{j.name} is not a SELF file."
    if head[:4] != b"NPD\0" or len(head) < 0x90:
        return f"{j.name} is not an SDATA file."
    if not int.from_bytes(head[0x80:0x84], "big") & SDAT_FLAG:
        return f"{j.name} has no SDATA flag."
    size = int.from_bytes(head[0x88:0x90], "big")
    if j.dec.get("size") is not None and size != j.dec["size"]:
        return f"{j.name} says it holds {size} bytes; the table says {j.dec['size']}."
    return ""
