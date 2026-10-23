"""Setup in one go, for the setup screens (docs/88): the phases in order, each
made of journal steps, with the screens' line protocol on standard output.

  check     the game folder and the decrypted files (gamefiles.py)
  unlock    on the RPCS3 route only: the missing decrypted files, made by the player's RPCS3 (rpcs3.py, docs/90)
  fetch     the pinned tools the build needs (fetch.py); Python is the host's to fetch
  stage     the decrypted files copied into the data folder
  lift      the game's code converted to C (lift.py)
  compile   the lift built into the game module (compile.py, docs/81 section 7), when this tree has it

A phase whose steps are all done with the same inputs is skipped, so a run
stopped anywhere (Cancel, a crash, a power cut) carries on where it stopped.
At the end, with the game module built, setup/ready.txt tells the host where
the game is (docs/88 section 3). While a run is under way there is no
ready.txt, so the host never starts a half-built game.

Only one run at a time: setup/run.lock is held while it runs, and the system
drops it when the process ends, however it ends.
"""
import errno
import json
import os
import re
import shutil
import traceback
from pathlib import Path

from . import compile as compile_step
from . import fetch, gamefiles, journal, lift, protocol, rpcs3

READY = "ready.txt"
REQUEST = "request.json"
UNLOCK = "unlock"                  # setup/unlock: the unlock's copies, RPCS3's folder, then its outputs until the stage
GB = 1 << 30
# Each phase's share of a whole run, from its CPU time (docs/52 4.3: the compile is most of it).
WEIGHTS = {"check": 2, "unlock": 1, "fetch": 4, "stage": 1, "lift": 13, "compile": 80}
# Where a step's own count falls in its phase; the phase's small steps fill the rest.
PARTS = {"ppu": (0.05, 0.75), "spu": (0.75, 0.97), "compile": (0.0, 0.97)}
# The free space a phase needs from scratch (docs/52 10.2), less what its folders already hold.
NEED = {"fetch": GB // 2, "lift": GB, "compile": 2 * GB}
OUTPUTS = {"fetch": ("downloads", "tools"), "lift": ("work",), "compile": ("build", "game")}
# The tools the build downloads: the compiler (docs/86), once toolchain.json pins it.
BUILD_TOOLS = (compile_step.LLVM_ENTRY,)
OOM = re.compile(r"out of memory|not enough memory|bad_alloc|MemoryError", re.I)
NOSPACE = re.compile(r"no space left|not enough space on the disk|disk is full", re.I)
FAILED = {"unlock": "Copying the game files for RPCS3 failed.", "stage": "Copying the game files failed.",
          "lift": "Converting the game's code failed.", "compile": "Building the game failed."}


def default_phases(rpcs3_route=False) -> list:
    """The whole build. On the RPCS3 route the unlock comes before the fetch, so
    an RPCS3 problem shows before the large download (docs/90 3.2)."""
    return ["check"] + (["unlock"] if rpcs3_route else []) + ["fetch", "stage", "lift", "compile"]


# ---- the copy step (also `python -m des_setup stage`) ----

def stage(rep, data: Path, jr: journal.Journal) -> Path:
    """Copy the decrypted inputs into the data folder, so a later rebuild needs
    nothing from the player. Returns the inputs folder."""
    dest = data / "inputs"
    items = rep.inputs()
    fp = journal.fingerprint("stage", sorted((i.name, i.sha256) for i in items))
    outs = [dest / _rel(i) for i in items]

    def run_copy():
        for i, o in zip(items, outs):
            o.parent.mkdir(parents=True, exist_ok=True)
            tmp = o.with_name(o.name + ".tmp")
            shutil.copyfile(i.found, tmp)
            if gamefiles.sha256_file(tmp) != i.sha256:
                tmp.unlink()
                raise journal.StepFailed("stage", f"{i.name} changed while it was copied")
            os.replace(tmp, o)
        return {"files": len(items)}

    jr.run("stage", fp, run_copy, outputs=outs)
    return dest


def _rel(item) -> str:
    return item.name if item.kind == "decrypted" else f"firmware/{item.name}"


# ---- what a run keeps in setup/ ----

def make_request(game, decrypted=(), firmware=None, rpcs3_path=None) -> dict:
    """What the player chose, with absolute paths (a later run may start elsewhere).
    rpcs3_path: the RPCS3 that unlocks the missing decrypted files (the RPCS3 route)."""
    return {"game": os.path.abspath(game), "decrypted": [os.path.abspath(d) for d in decrypted],
            "firmware": os.path.abspath(firmware) if firmware else None,
            "rpcs3": os.path.abspath(rpcs3_path) if rpcs3_path else None}


def save_request(data: Path, req: dict):
    p = data / "setup" / REQUEST
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps(req, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, p)


def load_request(data: Path):
    try:
        return json.loads((data / "setup" / REQUEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def write_ready(data: Path, fields: dict):
    """setup/ready.txt: key=value lines, the host's to read without Python (docs/88 section 3)."""
    p = data / "setup" / READY
    tmp = p.with_name(p.name + ".tmp")
    lines = ["# Written by des_setup when setup finished; the host reads it at every start (docs/88)."]
    lines += [f"{k}={v}" for k, v in fields.items()]
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(tmp, p)


def read_ready(data: Path):
    try:
        text = (data / "setup" / READY).read_text(encoding="utf-8")
    except OSError:
        return None
    out = {}
    for line in text.splitlines():
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


class _Held:
    """A held lock; close() lets it go."""

    def __init__(self, f):
        self.f = f

    def close(self):
        if os.name == "nt":                 # (Windows drops a lock left at close only "in time")
            try:
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
        self.f.close()


def lock(path: Path):
    """An exclusive lock on `path`, or None when another run holds it. The system
    drops it when this process ends, so a killed run leaves no lock behind."""
    path.parent.mkdir(parents=True, exist_ok=True)
    f = open(path, "a+b")
    try:
        if os.name == "nt":
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return _Held(f)


def quiet_and_awake(emit):
    """Below-normal priority for this process and everything it starts (docs/52 4.1),
    and on Windows a request that keeps the PC from sleeping until this process
    ends. Off Windows only the priority: keeping Linux awake needs the desktop's
    session bus (docs/88 section 8)."""
    try:
        if os.name == "nt":
            import ctypes
            k = ctypes.windll.kernel32
            k.GetCurrentProcess.restype = ctypes.c_void_p
            k.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)             # BELOW_NORMAL_PRIORITY_CLASS
            k.SetThreadExecutionState.argtypes = [ctypes.c_uint32]
            k.SetThreadExecutionState.restype = ctypes.c_uint32
            if k.SetThreadExecutionState(0x80000000 | 0x00000001):            # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
                emit.awake()
        else:
            os.nice(5)
    except (OSError, AttributeError, ValueError):
        pass


# ---- disk ----

def free_bytes(path: Path) -> int:
    p = Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    return shutil.disk_usage(p).free


def used_bytes(path: Path) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def _short_of_space(data: Path, phase: str):
    """None, or the sentence for the player when the phase may not fit on the disk."""
    need = NEED.get(phase, 0) - sum(used_bytes(data / o) for o in OUTPUTS.get(phase, ()))
    if need <= 0:
        return None
    free = free_bytes(data)
    if free >= need:
        return None
    where = Path(data).drive or str(data)
    return f"Setup needs about {need / GB:.1f} GB free on {where}. {free / GB:.1f} GB is free."


# ---- progress ----

class Overall:
    """The whole run's bar: the phases done, plus the current one's share."""

    def __init__(self, plan, emit):
        self.plan, self.emit = list(plan), emit
        self.total = sum(WEIGHTS.get(p, 1) for p in self.plan) or 1
        self.base, self.weight = 0, 0

    def start(self, phase):
        i = self.plan.index(phase)
        self.base = sum(WEIGHTS.get(p, 1) for p in self.plan[:i])
        self.weight = WEIGHTS.get(phase, 1)
        self.part(0.0)

    def part(self, fraction):
        self.emit.overall((self.base + self.weight * max(0.0, min(1.0, fraction))) / self.total)

    def step(self, step, done, total):
        """A step's own count (lift.py's and compile.py's progress callback)."""
        self.emit.progress(step, done, total)
        if step in PARTS:
            a, b = PARTS[step]
            self.part(a + (b - a) * done / max(1, total))


def _ends(jr) -> dict:
    return {n: s.get("end") for n, s in jr.data["steps"].items()}


# ---- the phases ----

def _check(req, emit, total, table, data=None, unlocks=False):
    t = table or gamefiles.load_table()
    size = sum(e.get("size") or 0 for e in t["originals"] + t["decrypted"]) or 1
    seen = [0]

    def prog(n):
        seen[0] += n
        emit.progress("check", min(seen[0], size), size)
        total.part(seen[0] / size)

    rep = _game_check(req, t, data, unlocks, progress=prog)
    for i in rep.items:
        emit.check(i)
    return rep


def _game_check(req, t, data, unlocks, progress=None, unlocked=False):
    """gamefiles.check on the request's folders. On the RPCS3 route the files an
    earlier run kept in inputs/ count too (docs/90 3.3), as does PPU Debug's
    dump of the EBOOT; after an unlock (`unlocked`), its outputs."""
    dirs, also = list(req.get("decrypted") or []), None
    if unlocks:
        dirs.append(str(data / "inputs"))
        if unlocked:
            dirs.append(str(data / "setup" / UNLOCK / "in"))
        exe = rpcs3.find(req.get("rpcs3"))
        if exe:
            also = {lift.EBOOT: rpcs3.ppu_debug_eboots(exe, t["title_id"])}
    return gamefiles.check(req["game"], dirs, req.get("firmware"), table=t, progress=progress, unlocks=unlocks,
                           also=also)


def blocking(rep, unlocks=False) -> list:
    """The check's problems that stop a run. On the RPCS3 route a decrypted file
    that is missing or wrong is the unlock's to make, not the player's to fix."""
    return [i for i in rep.problems() if not (unlocks and i.kind == "decrypted")]


def _rpcs3_state(req, data):
    """The RPCS3 of the request, and what @rpcs3 says about it: own, shared or missing."""
    exe = rpcs3.find(req.get("rpcs3"))
    if exe is None:
        return None, "missing"
    return exe, "shared" if rpcs3.is_shared(data, exe) else "own"


def _unlock(rep, req, data, jr, emit, total, table, kw):
    """The unlock phase (rpcs3.py, docs/90 3.4): the decrypted files the check did
    not find, made by the player's RPCS3 from copies of the originals. Returns
    the check again, which now finds them, and whether there was nothing to do.
    The journal skips the step while its originals and outputs are unchanged."""
    need = [i for i in rep.items if i.kind == "decrypted" and not i.good]
    if not need:
        return rep, True
    t = table or gamefiles.load_table()
    exe, state = _rpcs3_state(req, data)
    if exe is None:
        raise rpcs3.UnlockFailed("unlock", f"RPCS3 is not in {rpcs3.where(req['rpcs3'])} any more.")
    if state == "shared" and rpcs3.running(exe):
        raise rpcs3.UnlockFailed("unlock", "RPCS3 is open. Close it so setup can start it.")
    found = {i.name: i.found for i in rep.items if i.kind == "original"}
    origs = {e["path"]: e for e in t["originals"]}
    decs = {e["path"]: e for e in t["decrypted"]}
    jobs = [rpcs3.Job(Path(found[decs[i.name]["of"]]), origs[decs[i.name]["of"]], decs[i.name]) for i in need]
    u = rpcs3.Unlock(exe, data / "setup" / UNLOCK, jobs, emit, data / "setup" / "logs" / "unlock.log",
                     shared=state == "shared", progress=total.part, **(kw or {}))
    fp = journal.fingerprint("unlock", sorted((j.orig["path"], j.orig.get("sha256")) for j in jobs))
    try:
        ran, _ = jr.run("unlock", fp, u.run, outputs=u.outputs(), log=u.log)
        rep = _game_check(req, t, data, True, unlocked=True)
        if not ran and [i for i in rep.problems() if i.kind == "decrypted"]:
            jr.forget("unlock")                      # a kept output went bad since: unlock again
            ran, _ = jr.run("unlock", fp, u.run, outputs=u.outputs(), log=u.log)
            rep = _game_check(req, t, data, True, unlocked=True)
    finally:
        if u.ignored:
            rpcs3.remember_shared(data, exe)
    left = [i for i in rep.problems() if i.kind == "decrypted"]
    if left:
        raise rpcs3.UnlockFailed("unlock", f"{rpcs3.usrdir(left[0].name)} came out different from the known good "
                                           f"version.", u.log)
    emit.text(f"ok   {len(jobs)} file(s) unlocked by RPCS3 and checked" if ran else
              f"ok   the {len(jobs)} file(s) RPCS3 unlocked last time are unchanged")
    return rep, not ran


def _drop_unlock(data):
    """After the stage: the unlock's outputs are in inputs/ now, so its folder goes (docs/90 3.4 step 11)."""
    shutil.rmtree(data / "setup" / UNLOCK, ignore_errors=True)


def _game_problem(probs) -> str:
    if len(probs) == 1:
        return probs[0].message
    if all(i.kind == "decrypted" and i.state == "missing" for i in probs):
        return (f"{len(probs)} decrypted files are missing. Decrypt them first (in RPCS3: Utilities, "
                f"Decrypt PS3 Binaries).")
    return f"{len(probs)} of the game files have a problem."


def _fetch(data, emit, total, manifest, cc=None, ld=None) -> bool:
    """The build's tools; True if anything was downloaded or unpacked. A
    developer's compiler (compile.local_toolchain) replaces the download."""
    m = manifest or fetch.load_manifest()
    local = compile_step.local_toolchain(cc, ld) != (None, None)
    tools = [e for e in m["tools"] if e["name"] in BUILD_TOOLS and not (local and e["name"] == compile_step.LLVM_ENTRY)]
    did = False
    for k, e in enumerate(tools):
        if fetch.unpacked(e, data / "tools"):
            continue
        did = True

        def prog(done, size, k=k):
            emit.progress("download", done, size)
            total.part((k + done / max(1, size)) / len(tools))

        fetch.ensure(e, data / "downloads", data / "tools", prog)
    return did


def _compile(data, jr, jobs, progress, cc=None, ld=None, memory=None):
    """The compile phase (compile.py): the downloaded compiler, unpacked by the fetch phase, or a developer's.
    memory(short): the screens' hint that closing other programs makes it faster (@memory)."""
    cc, ld = compile_step.toolchain(data, cc, ld)
    return compile_step.Compile(data, jr, cc, ld, jobs=jobs, progress=progress, memory=memory).run_all()


def _classify(phase, e: journal.StepFailed):
    if NOSPACE.search(e.message):
        return "disk", e.step, "The disk is full."
    if OOM.search(e.message):
        return "memory", e.step, "The PC ran out of memory."
    if e.step.startswith("cc:"):
        return "step", e.step, f"Building the game failed on {e.step[3:]}."
    return "step", e.step, FAILED.get(phase, "A setup step failed.")


def run(data, request=None, app="", jobs=0, check_only=False, emit=None, src=None, table=None,
        manifest=None, phases=None, compile_fn=None, quiet=True, cc=None, ld=None, unlock_only=False,
        rpcs3_path=None, unlock_kw=None) -> int:
    """Every phase in order (or only the check), with the protocol on `emit`.
    request: make_request's dict, or None for the one the last run saved. app: the
    host's version, recorded in ready.txt (a new version is set up again). cc, ld:
    a developer's compiler (compile.local_toolchain), else the pinned download.
    rpcs3_path: the RPCS3 route's RPCS3 (its folder or program), over the
    request's. unlock_only: the check and the unlock (the unlock page's run).
    unlock_kw: rpcs3.Unlock's launch, stall and poll (the tests' stand-in).
    Returns the exit status: 0 done, 1 something to fix or a failure."""
    data = Path(data)
    emit = emit or protocol.Emitter()
    if compile_fn is None:
        def compile_fn(d, j, n, p):
            return _compile(d, j, n, p, cc, ld, emit.memory)
    held = lock(data / "setup" / "run.lock")
    if held is None:
        emit.setup()
        emit.error("busy", "-", "Setup is already running.")
        emit.done("failed")
        return 1
    try:
        emit.open_log(data / "setup" / "logs" / "run.log")
        emit.setup()
        return _run(data, request, app, jobs, check_only, emit, src, table, manifest, phases,
                    compile_fn, quiet, cc, ld, unlock_only, rpcs3_path, unlock_kw)
    finally:
        emit.close()
        held.close()


def _run(data, request, app, jobs, check_only, emit, src, table, manifest, phases, compile_fn, quiet, cc, ld,
         unlock_only, rpcs3_path, unlock_kw) -> int:
    if quiet:
        quiet_and_awake(emit)
    req = request or load_request(data)
    if not req or not req.get("game"):
        emit.error("game", "check", "Choose the game folder first.")
        emit.done("fix")
        return 1
    if rpcs3_path:
        req = dict(req, rpcs3=os.path.abspath(rpcs3_path))
    route = bool(req.get("rpcs3"))
    plan = (["check"] if check_only else ["check", "unlock"] if unlock_only
            else list(phases or default_phases(route)))
    assert plan[0] == "check", plan
    unlocks = route and (check_only or "unlock" in plan)   # the unlock makes what the check misses
    emit.plan(plan)
    if not check_only and (data / "setup" / READY).exists():
        (data / "setup" / READY).unlink()            # not ready while a run is under way
    jr = journal.Journal(data / "setup" / "journal.json")
    total = Overall(plan, emit)
    rep, module, phase = None, None, plan[0]
    try:
        for phase in plan:
            short = None if phase == "check" else _short_of_space(data, phase)
            if short:
                emit.error("disk", phase, short)
                emit.done("failed")
                return 1
            total.start(phase)
            emit.step(phase, "start")
            before = _ends(jr)
            skipped = False
            if phase == "check":
                exe = None
                if unlocks:
                    exe, state = _rpcs3_state(req, data)
                    emit.rpcs3(state, rpcs3.where(exe or req["rpcs3"]))
                rep = _check(req, emit, total, table, data, unlocks)
                probs = blocking(rep, unlocks)
                if probs or rep.root is None:
                    for i in probs:
                        emit.text(f"FIX  {i.message}")
                    emit.error("game", "check", _game_problem(probs))
                    emit.done("fix")
                    return 1
                if unlocks and exe is None and rep.problems():
                    emit.error("rpcs3", "check", f"RPCS3 is not in {rpcs3.where(req['rpcs3'])} any more.")
                    emit.done("fix")
                    return 1
                save_request(data, req)
            elif phase == "unlock":
                if unlocks:
                    rep, skipped = _unlock(rep, req, data, jr, emit, total, table, unlock_kw)
                else:
                    skipped = True
            elif phase == "fetch":
                skipped = not _fetch(data, emit, total, manifest, cc, ld)
            elif phase == "stage":
                stage(rep, data, jr)
                _drop_unlock(data)
            elif phase == "lift":
                sums = {i.name: i.sha256 for i in rep.inputs()}
                lift.Lift(data, jr, src=src or lift.SRC, workers=jobs, progress=total.step).run_all(
                    sums.get(lift.EBOOT), sums.get("libsre.prx"))
            elif phase == "compile":
                module = compile_fn(data, jr, jobs, total.step)
            if phase in ("stage", "lift", "compile") and _ends(jr) == before:
                skipped = True
            total.part(1.0)
            emit.step(phase, "skip" if skipped else "done")
        if module and Path(module).is_file():
            write_ready(data, {"format": 1, "app": app, "game": str(rep.root),
                               "eboot": str(data / "inputs" / lift.EBOOT), "module": str(module),
                               "module_size": Path(module).stat().st_size,
                               "module_sha256": gamefiles.sha256_file(module)})
            emit.text(f"ok   the game is ready: {module}")
        elif not check_only:
            emit.text("ok   every phase is done; the compile was not in this run, so the game is not built yet")
        emit.overall(1.0)
        emit.done("ok")
        return 0
    except fetch.FetchError as e:
        emit.text(f"FIX  {e}")
        emit.error("download", phase, "A download did not finish.")
    except rpcs3.UnlockFailed as e:
        emit.text(f"FIX  {e.message}")
        if e.log:
            emit.log_path(e.log)
        emit.error("rpcs3", e.step, e.message)
    except journal.StepFailed as e:
        emit.text(f"FIX  {e}")
        if e.log:
            emit.log_path(e.log)
        emit.error(*_classify(phase, e))
    except OSError as e:
        emit.text(traceback.format_exc())
        if e.errno == errno.ENOSPC:
            emit.error("disk", phase, "The disk is full.")
        else:
            emit.error("internal", phase, "Setup stopped on an unexpected error.")
    except Exception:                                    # the engine's own
        emit.text(traceback.format_exc())
        emit.error("internal", phase, "Setup stopped on an unexpected error.")
    emit.done("failed")
    return 1


# ---- status ----

def status(data, app=None) -> dict:
    """What `status --json` prints: the start screen's state (docs/88 section 3)
    and the journal's steps, from the data folder alone."""
    data = Path(data)
    jr = journal.Journal(data / "setup" / "journal.json")
    steps = {n: {k: s[k] for k in ("state", "message", "log") if s.get(k)} for n, s in jr.data["steps"].items()}
    running = False
    if (data / "setup" / "run.lock").exists():
        held = lock(data / "setup" / "run.lock")
        running = held is None
        if held:
            held.close()
    ready = read_ready(data)
    return {"protocol": protocol.VERSION, "data": str(data), "state": _state(ready, steps, running, app),
            "ready": ready, "request": load_request(data), "steps": steps}


def _state(ready, steps, running, app) -> str:
    if running:
        return "running"
    if ready:
        module = Path(ready.get("module", ""))
        if not module.is_file() or str(module.stat().st_size) != ready.get("module_size"):
            return "damaged"
        if app and ready.get("app") != app:
            return "outdated"
        if not Path(ready.get("game", "")).is_dir():
            return "game-moved"
        return "ready"
    if any(s.get("state") == "running" for s in steps.values()):
        return "interrupted"
    if any(s.get("state") == "failed" for s in steps.values()):
        return "failed"
    return "partial" if steps else "missing"
