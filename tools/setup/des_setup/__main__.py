"""Setup from the command line (the setup screens call the same functions).

  python -m des_setup check GAME [--decrypted DIR]... [--firmware FILE] [--rpcs3 PATH]
  python -m des_setup stage GAME [--decrypted DIR]... [--firmware FILE] [--data DIR]
  python -m des_setup lift GAME [--decrypted DIR]... [--firmware FILE] [--data DIR] [-j N]
  python -m des_setup run [GAME] [--decrypted DIR]... [--firmware FILE] [--rpcs3 PATH] [--data DIR] [-j N]
                          [--app VERSION] [--check-only | --unlock-only] [--cc CLANG-CL --ld LLD-LINK]
  python -m des_setup compile [--data DIR] [-j N] [--cc CLANG-CL --ld LLD-LINK]
  python -m des_setup status [--data DIR] [--json [--app VERSION]]
  python -m des_setup tools
  python -m des_setup fetch NAME [--data DIR]

Run from tools/setup (or with it on PYTHONPATH; the host runs run_setup.py,
docs/88). GAME is the folder of the disc dump that holds PS3_GAME. `run` does
every phase in order with the setup screens' line protocol (pipeline.py,
protocol.py); without GAME it takes the folders the last run used. The data
folder defaults to %LOCALAPPDATA%\\DemonsSoulsRecomp (Linux:
$XDG_DATA_HOME/DemonsSoulsRecomp). A developer's compiler (--cc and --ld, or
DES_SETUP_CC and DES_SETUP_LD in the environment, so the setup screens' runs
use it too) replaces the pinned LLVM download, which is then not fetched.

--rpcs3 is the RPCS3 route (docs/90): the folder that holds rpcs3.exe (Linux:
rpcs3 or an AppImage), or the program. The decrypted files the folders lack
are then made by that RPCS3 (`rpcs3 --decrypt`) in the unlock phase, before
the fetch, on copies in <data>/setup/unlock/ and with a folder of setup's own
for RPCS3's files, so neither the dump nor RPCS3's folder is written to (an
install with a `portable` folder is run only while it is closed). `check
--rpcs3` only says which files RPCS3 would unlock. `run --unlock-only` does
the check and the unlock (the unlock page's run).
Exit status: 0 done, 1 something to fix (the lines say what), 2 usage.
"""
import argparse
import json
import os
import sys
from pathlib import Path

from . import compile as compile_step, fetch, gamefiles, journal, lift
from . import pipeline, rpcs3
from .pipeline import stage  # noqa: F401 (the copy step; the tests import it from here)

NAME = "DemonsSoulsRecomp"


def default_data() -> Path:
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    else:
        base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / NAME


def _print_report(rep, unlocks=False):
    """One line per problem, but the many files that share a state (the disc's
    files that match, decrypted files found or missing) on one line each.
    unlocks: the RPCS3 route, where RPCS3 makes the missing decrypted files."""
    def names(items):
        return ", ".join(i.name.rsplit("/", 1)[-1] for i in items)

    n_orig = [i for i in rep.items if i.kind == "original" and i.state == "ok"]
    unmeasured = [i for i in rep.items if i.state == "unmeasured"]
    missing = [i for i in rep.items if i.kind == "decrypted" and i.state == "missing"]
    for i in rep.items:
        if i in n_orig or i in unmeasured or (len(missing) > 1 and i in missing):
            continue
        print(f"{'ok  ' if i.good else 'next' if unlocks and i.kind == 'decrypted' else 'FIX '} {i.message}")
    if n_orig:
        print(f"ok   {len(n_orig)} of the disc's files match the US disc, version 1.00.")
    if unmeasured:
        print(f"ok?  {len(unmeasured)} decrypted file(s) found; their sums are not in the table yet, so only their "
              f"first bytes were checked: {names(unmeasured)}.")
    if len(missing) > 1 and unlocks:
        print(f"next {len(missing)} decrypted files are missing: {names(missing)}. Your RPCS3 unlocks them in "
              f"`run --rpcs3`.")
    elif len(missing) > 1:
        srcs = sorted({i.name.rsplit("/", 1)[-1].endswith(".elf") and "EBOOT.BIN" or "the .sdat files"
                       for i in missing})
        print(f"FIX  {len(missing)} decrypted files are missing: {names(missing)}. Decrypt {' and '.join(srcs)} "
              f"first (in RPCS3: Utilities, Decrypt PS3 Binaries).")
    probs = pipeline.blocking(rep, unlocks)
    if probs or rep.root is None:
        print(f"{len(probs)} thing(s) to fix.")
    else:
        print("Ready." if rep.ok else f"Ready once your RPCS3 has unlocked {len(rep.problems())} file(s).")


def cmd_check(a) -> int:
    if not a.rpcs3:
        rep = gamefiles.check(a.game, a.decrypted, a.firmware)
        _print_report(rep)
        return 0 if rep.ok else 1
    exe = rpcs3.find(a.rpcs3)
    if exe is None:
        print(f"FIX  There is no RPCS3 in {a.rpcs3}.")
        return 1
    t = gamefiles.load_table()
    print(f"ok   RPCS3: {exe}" + (" (a portable install: setup runs it only while it is closed)"
                                  if (exe.parent / "portable").is_dir() else ""))
    also = {lift.EBOOT: rpcs3.ppu_debug_eboots(exe, t["title_id"])}
    rep = gamefiles.check(a.game, a.decrypted, a.firmware, table=t, unlocks=True, also=also)
    _print_report(rep, unlocks=True)
    return 1 if pipeline.blocking(rep, True) or rep.root is None else 0


def cmd_stage(a) -> int:
    rep = gamefiles.check(a.game, a.decrypted, a.firmware)
    if not rep.ok:
        _print_report(rep)
        return 1
    data = Path(a.data)
    jr = journal.Journal(data / "setup" / "journal.json")
    try:
        dest = stage(rep, data, jr)
    except journal.StepFailed as e:
        print(f"FIX  {e.message}")
        return 1
    print(f"ok   {len(rep.inputs())} files in {dest}")
    return 0


def lift_blockers(rep) -> list:
    """The problems that stop the lift. The decrypted .sdat files are only the
    game's run-time inputs (docs/52), so the lift goes on without them."""
    return [i for i in rep.problems() if not (i.kind == "decrypted" and i.name != lift.EBOOT)]


def cmd_lift(a) -> int:
    """Check, stage, then the lift steps (lift.py), each skipped when done."""
    rep = gamefiles.check(a.game, a.decrypted, a.firmware)
    if lift_blockers(rep):
        _print_report(rep)
        return 1
    if not rep.ok:
        _print_report(rep)
        print("     The lift needs only EBOOT.elf (and libsre.prx); the game needs the rest to run. Lifting now.")
    data = Path(a.data)
    jr = journal.Journal(data / "setup" / "journal.json")
    shown = {}

    def prog(step, done, total):
        pct = done * 100 // max(1, total)
        if shown.get(step) != pct:
            shown[step] = pct
            print(f"@progress {step} {done}/{total}", flush=True)

    sums = {i.name: i.sha256 for i in rep.inputs()}
    try:
        stage(rep, data, jr)
        lift.Lift(data, jr, workers=a.jobs, progress=prog).run_all(sums.get(lift.EBOOT), sums.get("libsre.prx"))
    except journal.StepFailed as e:
        print(f"FIX  {e}" + (f" (log: {e.log})" if e.log else ""))
        return 1
    print(f"ok   the lift is in {data / 'work'}")
    return 0


def cmd_run(a) -> int:
    """Every phase in order, with the setup screens' protocol (pipeline.py, docs/88)."""
    req = pipeline.make_request(a.game, a.decrypted, a.firmware, a.rpcs3) if a.game else None
    return pipeline.run(Path(a.data), req, app=a.app, jobs=a.jobs, check_only=a.check_only, cc=a.cc, ld=a.ld,
                        unlock_only=a.unlock_only, rpcs3_path=a.rpcs3)


def cmd_compile(a) -> int:
    """The lift compiled and linked into des_game.dll (compile.py), each object
    skipped when done. The lift must have run."""
    data = Path(a.data)
    jr = journal.Journal(data / "setup" / "journal.json")
    shown = {}

    def prog(step, done, total):
        if shown.get(step) != done:
            shown[step] = done
            print(f"@progress {step} {done}/{total}", flush=True)

    try:
        cc, ld = compile_step.toolchain(data, a.cc, a.ld)
        dll = compile_step.Compile(data, jr, cc, ld, jobs=a.jobs, progress=prog).run_all()
    except fetch.FetchError as e:
        print(f"FIX  {e}")
        return 1
    except journal.StepFailed as e:
        print(f"FIX  {e}" + (f" (log: {e.log})" if e.log else ""))
        return 1
    print(f"ok   {dll}")
    return 0


def cmd_status(a) -> int:
    if a.json:
        print(json.dumps(pipeline.status(Path(a.data), a.app), indent=1, sort_keys=True))
        return 0
    jr = journal.Journal(Path(a.data) / "setup" / "journal.json")
    steps = jr.data["steps"]
    if not steps:
        print("No setup step has run yet.")
    for name, s in steps.items():
        line = f"{name:12s} {s.get('state')}"
        if s.get("message"):
            line += f": {s['message']}"
        print(line)
    return 0


def cmd_tools(a) -> int:
    m = fetch.load_manifest()
    for e in m["tools"]:
        pinned = "pinned" if e.get("sha256") else "NOT PINNED (no SHA-256 yet)"
        size = f"{e['size']:,} bytes" if e.get("size") else "size not set"
        print(f"{e['name']} {e['version']} ({size}): {pinned}")
    return 0


def cmd_fetch(a) -> int:
    m = fetch.load_manifest()
    e = next((t for t in m["tools"] if t["name"] == a.name), None)
    if e is None:
        print(f"FIX  no tool named {a.name} in the manifest")
        return 2
    data = Path(a.data)

    def prog(done, total):
        print(f"\r{a.name}: {done * 100 // total}%", end="", flush=True)

    try:
        p = fetch.ensure(e, data / "downloads", data / "tools", prog)
    except fetch.FetchError as err:
        print(f"\nFIX  {err}")
        return 1
    print(f"\nok   {p}")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="des_setup", description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    def game_args(sp):
        sp.add_argument("game", help="the folder of the disc dump that holds PS3_GAME")
        sp.add_argument("--decrypted", action="append", default=[], metavar="DIR",
                        help="also look here for the decrypted files, by name (repeatable)")
        sp.add_argument("--firmware", metavar="FILE", help="the decrypted libsre.prx")

    def rpcs3_arg(sp):
        sp.add_argument("--rpcs3", metavar="PATH", help="the RPCS3 route: this RPCS3 (its folder or program) unlocks "
                        "the decrypted files the folders lack (docs/90)")

    def data_arg(sp):
        sp.add_argument("--data", default=str(default_data()), help="setup's data folder")

    def toolchain_args(sp):
        sp.add_argument("--cc", metavar="CLANG-CL", help="this clang-cl instead of the pinned download "
                        "(default: DES_SETUP_CC)")
        sp.add_argument("--ld", metavar="LLD-LINK", help="this lld-link instead of the pinned download "
                        "(default: DES_SETUP_LD)")

    sp = sub.add_parser("check", help="check the game folder")
    game_args(sp)
    rpcs3_arg(sp)
    sp.set_defaults(fn=cmd_check)
    sp = sub.add_parser("stage", help="check, then copy the decrypted files into the data folder")
    game_args(sp)
    data_arg(sp)
    sp.set_defaults(fn=cmd_stage)
    sp = sub.add_parser("lift", help="check, stage, then convert the game's code (resumes where it stopped)")
    game_args(sp)
    data_arg(sp)
    sp.add_argument("-j", "--jobs", type=int, default=0, help="lifter workers (default: up to 16)")
    sp.set_defaults(fn=cmd_lift)
    sp = sub.add_parser("run", help="every phase in order, for the setup screens (resumes where it stopped)")
    sp.add_argument("game", nargs="?", help="the folder of the disc dump (default: the last run's)")
    sp.add_argument("--decrypted", action="append", default=[], metavar="DIR",
                    help="also look here for the decrypted files, by name (repeatable)")
    sp.add_argument("--firmware", metavar="FILE", help="the decrypted libsre.prx")
    rpcs3_arg(sp)
    data_arg(sp)
    sp.add_argument("-j", "--jobs", type=int, default=0, help="workers for the lift and the compile (default: auto)")
    sp.add_argument("--app", default="", metavar="VERSION", help="the host's version, kept in ready.txt")
    only = sp.add_mutually_exclusive_group()
    only.add_argument("--check-only", action="store_true", help="only check the game files")
    only.add_argument("--unlock-only", action="store_true", help="check, then unlock with RPCS3 (the unlock page)")
    toolchain_args(sp)
    sp.set_defaults(fn=cmd_run)
    sp = sub.add_parser("compile", help="compile and link the lift into des_game.dll (resumes where it stopped)")
    data_arg(sp)
    sp.add_argument("-j", "--jobs", type=int, default=0,
                    help="compiler jobs at a time, whatever the memory (default: as many as the memory holds)")
    toolchain_args(sp)
    sp.set_defaults(fn=cmd_compile)
    sp = sub.add_parser("status", help="show the journal")
    data_arg(sp)
    sp.add_argument("--json", action="store_true", help="the state and the journal as JSON (docs/88)")
    sp.add_argument("--app", default=None, metavar="VERSION", help="with --json: the host's version")
    sp.set_defaults(fn=cmd_status)
    sp = sub.add_parser("tools", help="list the pinned tools")
    sp.set_defaults(fn=cmd_tools)
    sp = sub.add_parser("fetch", help="download and unpack one pinned tool")
    sp.add_argument("name")
    data_arg(sp)
    sp.set_defaults(fn=cmd_fetch)
    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
