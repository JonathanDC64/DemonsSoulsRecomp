"""A stand-in for tools/setup/run_setup.py in screenshot tests of the setup screens (docs/89 section 5): it prints the
line protocol (tools/setup/des_setup/protocol.py, docs/88 section 4) on a fixed clock and does no work at all. Copy it
to tools/setup/run_setup.py beside the exe a test runs (a test run's staged copy: <DES_SCRATCH>/bin), where the host
looks first, and run with DES_UI_OPEN=setup, DES_SETUP_PYTHON=python and a fresh DES_SETUP_DATA.

FAKE_SETUP_MODE (from the environment the host passes down):
  ok      every phase, about FAKE_SETUP_SECONDS (default 60) in all, then @done ok
  fail    the same until the compile is about half done, then a failed object and @done failed
  hold    the same, but it stops at 40% of the compile and waits there (until the host ends it)
  check   `run --check-only`: a few @check lines (3 decrypted files missing) and @done fix
FAKE_SETUP_MODES ("hold,fail,ok"): one mode for each run in turn, counted in fake_runs.txt in DES_SETUP_DATA (the last
one repeats), so that one test goes through a stop, a failure and a finished run. A failed run writes its step's log
in DES_SETUP_DATA/setup/logs. FAKE_SETUP_APP (the host's version, des_version.h's DES_VERSION): a finished run leaves
setup/ready.txt for it, so the host finds the game Ready. The run's arguments are accepted and ignored."""
import os
import sys
import time

DATA = os.environ.get("DES_SETUP_DATA") or os.getcwd()


def pick_mode():
    modes = [m for m in os.environ.get("FAKE_SETUP_MODES", "").split(",") if m]
    if not modes:
        return os.environ.get("FAKE_SETUP_MODE", "ok")
    counter = os.path.join(DATA, "fake_runs.txt")
    try:
        with open(counter, encoding="utf-8") as f:
            n = int(f.read().strip() or 0)
    except (OSError, ValueError):
        n = 0
    with open(counter, "w", encoding="utf-8") as f:
        f.write(str(n + 1))
    return modes[min(n, len(modes) - 1)]


MODE = pick_mode()
TOTAL = float(os.environ.get("FAKE_SETUP_SECONDS", "60"))
# Each phase's share of the run (pipeline.WEIGHTS) and its own steps.
PHASES = [("check", 2), ("fetch", 4), ("stage", 1), ("lift", 13), ("compile", 80)]
done_w = 0.0


def say(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def overall(f):
    say(f"@overall {int(max(0.0, min(1.0, f)) * 1000)}/1000")


def count(step, n, m, phase_w, part=(0.0, 1.0), seconds=1.0, stop_at=None, fail_at=None):
    """A step's own count from 0 to m over `seconds`, the overall bar moving through `part` of its phase."""
    ticks = max(1, int(seconds * 4))
    for t in range(ticks + 1):
        k = n + (m - n) * t // ticks
        say(f"@progress {step} {k}/{m}")
        overall((done_w + phase_w * (part[0] + (part[1] - part[0]) * t / ticks)) / 100.0)
        if stop_at is not None and t >= ticks * stop_at:
            while True:
                time.sleep(1)
        if fail_at is not None and t >= ticks * fail_at:
            return k
        time.sleep(seconds / ticks)
    return m


def main():
    global done_w
    if "--check-only" in sys.argv or MODE == "check":
        say("@setup 1")
        say("@plan check")
        say("@step check start")
        say("@check ok PS3_GAME The game folder holds PS3_GAME.")
        say("@check ok TITLE_ID BLUS30443, version 01.00.")
        say("@check ok EBOOT.elf EBOOT.elf matches.")
        for name in ("chr/c5320.luabnd.dcx.sdat.unedat", "script/talk/m02_00.talkesdbnd.dcx.sdat.unedat",
                     "menu/menu.esdbnd.dcx.sdat.unedat"):
            say(f"@check missing USRDIR/{name} Not found.")
        say("@error game check 3 of the game files have a problem.")
        say("@done fix")
        return 1
    say("@setup 1")
    say("@plan " + " ".join(p for p, _ in PHASES))
    say("@awake")
    s = TOTAL / 100.0
    for phase, w in PHASES:
        say(f"@step {phase} start")
        say(f"[{phase}] started")
        if phase == "check":
            count("check", 0, 5100 << 20, w, seconds=2 * w * s)
        elif phase == "fetch":
            say("@step fetch skip")
            done_w += w
            overall(done_w / 100.0)
            continue
        elif phase == "stage":
            time.sleep(w * s)
        elif phase == "lift":
            count("ppu", 0, 94017, w, (0.05, 0.75), seconds=0.7 * w * s)
            count("spu", 0, 14, w, (0.75, 0.97), seconds=0.25 * w * s)
        elif phase == "compile":
            stop = 0.4 if MODE == "hold" else None
            fail = 0.55 if MODE == "fail" else None
            got = count("compile", 0, 33, w, (0.0, 0.97), seconds=w * s, stop_at=stop, fail_at=fail)
            if MODE == "fail":
                say("ppu_recomp_008.cpp:120544:5: error: use of undeclared identifier 'ppc_rotl64'")
                say("1 error generated.")
                say(f"[compile] stopped after {got} of 33 files; the finished ones are kept")
                logs = os.path.join(DATA, "setup", "logs")
                os.makedirs(logs, exist_ok=True)
                log = os.path.join(logs, "cc_ppu_recomp_008.log").replace("\\", "/")
                with open(log, "w", encoding="utf-8") as f:
                    f.write("clang-cl ppu_recomp_008.cpp\n"
                            "ppu_recomp_008.cpp:120544:5: error: use of undeclared identifier 'ppc_rotl64'\n"
                            "1 error generated.\n")
                say(f"@log {log}")
                say("@error step cc:ppu_recomp_008 Building the game failed on ppu_recomp_008.")
                say("@done failed")
                return 1
            say("[compile] link des_game.dll")
        done_w += w
        say(f"@step {phase} done")
        overall(done_w / 100.0)
    # setup/ready.txt as the engine leaves it (setup_engine.cpp's launch_state reads it), when the test says which
    # version the host is (FAKE_SETUP_APP): the host then finds the game Ready.
    if os.environ.get("FAKE_SETUP_APP"):
        game = os.path.join(DATA, "fakegame")
        module = os.path.join(DATA, "setup", "game", "des_game.dll")
        os.makedirs(game, exist_ok=True)
        os.makedirs(os.path.dirname(module), exist_ok=True)
        with open(os.path.join(game, "EBOOT.elf"), "wb") as f:
            f.write(b"\0" * 16)
        with open(module, "wb") as f:
            f.write(b"\0" * 4096)
        with open(os.path.join(DATA, "setup", "ready.txt"), "w", encoding="utf-8") as f:
            f.write(f"format=1\napp={os.environ['FAKE_SETUP_APP']}\ngame={game}\n"
                    f"eboot={os.path.join(game, 'EBOOT.elf')}\nmodule={module}\nmodule_size=4096\n")
    say("ok   the game is ready")
    say("@done ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
