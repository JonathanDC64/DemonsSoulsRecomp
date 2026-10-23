"""The setup screens' line protocol (docs/88 section 4).

The engine prints its news on standard output, one line each, for the host's
setup screens to read (port/ui/src/setup_protocol.cpp). A line that starts
with "@" and a keyword is for the screens; any other line is for people, and
the screens show the last ones under Details.

  @setup <version>                  the first line: this protocol's version
  @plan <phase>...                  the phases this run goes through, in order
  @step <phase> start|done|skip     a phase starts, ends, or had nothing to do
  @progress <step> <n>/<m>          the current step's own count
  @overall <n>/1000                 the whole run so far
  @check <state> <name> <text>      one checked file (gamefiles.Item: its state, table name and message)
  @file <step> <path>               the file the step works on now (the unlock: its path under USRDIR)
  @rpcs3 <state> <path>             the RPCS3 in use (docs/90): own (it runs with setup's folder),
                                    shared (a portable install: starting it changes its log) or missing
  @user <id> <users> <name>         RPCS3's active user: its 8-digit ID, how many users RPCS3 has, its name
  @library <kind> <path>            Demon's Souls in RPCS3's library: folder, iso, store, or none (path -)
  @found <what> <state> <value>...  what RPCS3 has that can come across (FOUND: the states and values of each):
                                    save <state> <characters> <saved>, current-save <state> <saved> (the port's own
                                    save, which an import replaces), trophies <state> <earned>/<total>,
                                    settings <state> <n> (n @setting lines follow); - where not known, times in
                                    Unix seconds
  @setting <key> <value>            one setting that comes across, by its settings.ini name
  @awake                            the PC is kept from sleeping while this runs
  @memory short|ok                  the compile runs fewer jobs at once than the PC has processors, because memory is
                                    short (so closing other programs makes it faster), or no longer does
  @log <path>                       the log of the step that failed
  @error <code> <step> <text>       what failed, in a sentence for the player (codes: ERRORS)
  @done ok|fix|failed               the last line

Names (phases, steps, states, codes) have no spaces; <text> and <path> are the
rest of the line. The host ignores keywords it does not know, so a new kind of
line needs no new version; a changed meaning does. Version 2 added the RPCS3
route (docs/90 3.8): the unlock and import phases, @file, @rpcs3, @user,
@library, @found, @setting, and the rpcs3 and import codes.

Every line also goes to the run's log (setup/logs/run.log), so a run from a
terminal leaves the same record as one from the screens.
"""
import sys

VERSION = 2
KINDS = ("setup", "plan", "step", "progress", "overall", "check", "file", "rpcs3", "user", "library", "found",
         "setting", "awake", "memory", "log", "error", "done")
# game: the game folder or its files; busy: another setup is running; disk: not enough free space;
# download: a download stopped or did not match; memory: out of memory; step: a build step failed;
# rpcs3: the player's RPCS3 could not unlock the files; import: bringing the save, trophies or settings across from
# RPCS3 failed (the game is built); internal: an error of the engine's own.
ERRORS = ("game", "busy", "disk", "download", "memory", "step", "rpcs3", "import", "internal")
STEP_STATES = ("start", "done", "skip")
RPCS3_STATES = ("own", "shared", "missing")
LIBRARY_KINDS = ("folder", "iso", "store", "none")
# @found: each item's states, and how many values follow the state.
FOUND = {"save": (("ok", "none", "other-user", "busy"), 2), "current-save": (("ok", "none"), 1),
         "trophies": (("ok", "none"), 1), "settings": (("ok", "none"), 1)}
RESULTS = ("ok", "fix", "failed")


def word(s) -> str:
    """A name field: no spaces, never empty."""
    s = "_".join(str(s).split())
    return s or "-"


def one_line(s) -> str:
    return " ".join(str(s).split())


class Emitter:
    """Writes the protocol to `out` (standard output by default) and every line,
    plain ones too, to the log file opened with open_log."""

    def __init__(self, out=None):
        self.out = out if out is not None else sys.stdout
        self.log = None
        self._shown = {}            # progress: the last per cent printed; overall: the last per mille

    def open_log(self, path):
        """The run's log; the last run's is kept as <name>.prev.log."""
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.is_file():
            path.replace(path.with_name(path.stem + ".prev" + path.suffix))
        self.log = open(path, "w", encoding="utf-8", errors="replace")

    def close(self):
        if self.log:
            self.log.close()
            self.log = None

    def _put(self, line):
        for f in (self.out, self.log):
            if f is not None:
                f.write(line + "\n")
                f.flush()

    def _line(self, kind, *fields):
        assert kind in KINDS, kind
        self._put(" ".join(["@" + kind] + [str(f) for f in fields if f != ""]))

    def text(self, s):
        """Lines for people. One that starts with '@' gets a space first, so it is never read as the protocol."""
        for line in str(s).splitlines() or [""]:
            self._put(" " + line if line.startswith("@") else line)

    def setup(self):
        self._line("setup", VERSION)

    def plan(self, phases):
        self._line("plan", *[word(p) for p in phases])

    def step(self, phase, state):
        assert state in STEP_STATES, state
        self._line("step", word(phase), state)

    def progress(self, step, n, m):
        """Printed when the per cent changes, and at the end."""
        m = max(1, int(m))
        n = max(0, min(int(n), m))
        pc = n * 100 // m
        key = ("progress", step)
        if self._shown.get(key) == pc and n != m:
            return
        self._shown[key] = pc
        self._line("progress", word(step), f"{n}/{m}")

    def overall(self, fraction):
        pm = max(0, min(1000, int(fraction * 1000)))
        if self._shown.get("overall") == pm:
            return
        self._shown["overall"] = pm
        self._line("overall", f"{pm}/1000")

    def check(self, item):
        self._line("check", word(item.state), word(item.name), one_line(item.message))

    def file(self, step, path):
        """Printed when the step's file changes."""
        key = ("file", step)
        if self._shown.get(key) == path:
            return
        self._shown[key] = path
        self._line("file", word(step), one_line(path))

    def rpcs3(self, state, path):
        assert state in RPCS3_STATES, state
        self._line("rpcs3", state, one_line(path or "-"))

    def user(self, uid, users, name):
        self._line("user", word(uid), int(users), one_line(name) or "-")

    def library(self, kind, path):
        assert kind in LIBRARY_KINDS, kind
        self._line("library", kind, one_line(path) if path and kind != "none" else "-")

    def found(self, what, state, *values):
        """One @found line: values are printed as they are, None as '-'."""
        states, n = FOUND[what]
        assert state in states and len(values) == n, (what, state, values)
        self._line("found", what, state, *["-" if v is None or v == "" else word(v) for v in values])

    def setting(self, key, value):
        self._line("setting", word(key), one_line(value) or '""')

    def awake(self):
        self._line("awake")

    def memory(self, short):
        """Printed when it changes (compile.py tells, from its own thread)."""
        state = "short" if short else "ok"
        if self._shown.get("memory", "ok") == state:
            return
        self._shown["memory"] = state
        self._line("memory", state)

    def log_path(self, path):
        self._line("log", one_line(path))

    def error(self, code, step, text):
        assert code in ERRORS, code
        self._line("error", code, word(step or "-"), one_line(text))

    def done(self, result):
        assert result in RESULTS, result
        self._line("done", result)
