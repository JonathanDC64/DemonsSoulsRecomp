"""The RPCS3 route's import (docs/90 3.5, docs/44 8.4 and 8.5): at the end of
the build, once, what the player ticked comes across from their RPCS3.

  import-save      the save, copied and converted into the folder the game reads
  import-trophies  RPCS3's unlocked trophies, merged into the port's trophy file
  (settings)       <data>/setup/rpcs3_settings.txt, for the options page

The two journal steps carry the import's own ID (request.json's import
"asked"), so a resumed run skips what is done, and a new import is never
mistaken for an old one. The phase runs only while request.json has a pending
import; the pipeline drops it once the import is done, so a later Update or
Repair never brings an old RPCS3 save over newer progress.

RPCS3's files are only ever opened for reading, and are never moved. The
port's own save is copied to <gamedata>/save-backups/<date-time>/ before
anything changes (backups are never deleted), then swapped with the staged
one by two renames. At the end <data>/setup/imported.txt says what came
across, for the Ready to play page (docs/90 1.9).
"""
import os
import re
import shutil
import time
from pathlib import Path

from . import gamefiles, journal, rpcs3, rpcs3_library as lib, sfo

ITEMS = ("save", "trophies", "settings")
SETTINGS_FILE = "rpcs3_settings.txt"    # in setup/
IMPORTED = "imported.txt"               # in setup/
MAX_TROPHIES = 128                      # the trophy IDs the port keeps (sceNpTrophy.c)
# The player's sentences (docs/90 1.10); the host adds its own second one for the import code.
FAILED = {"import-save": "Bringing your save across failed.",
          "import-trophies": "Bringing your trophies across failed.",
          "import-settings": "Bringing your settings across failed."}
SAVE_BUSY = "RPCS3 is saving the game right now. Close the game in RPCS3."
SAVE_CHANGED = "RPCS3 saved the game while setup was copying it."
NO_SAVE = "This RPCS3 user has no Demon's Souls save."
NO_TROPHIES = "This RPCS3 user has no Demon's Souls trophies."


class ImportFailed(journal.StepFailed):
    """Bringing something across failed. `message` is the sentence for the player; the game is built."""


def sentence(step) -> str:
    return FAILED.get(step, "Bringing your save across failed.")


def items(req) -> list:
    """The items of request.json's pending import, in the order they come across ([] when none is pending)."""
    imp = req.get("import") or {}
    return [i for i in ITEMS if i in (imp.get("items") or [])]


def trophy_file(gamedata) -> Path:
    """The port's trophy file (sceNpTrophy.c): PS3_TROPHY_DIR, else gamedata/trophies; a relative folder is the game's
    working folder's (the gamedata's parent)."""
    gamedata = Path(gamedata)
    d = os.environ.get("PS3_TROPHY_DIR")
    return (gamedata.parent / d if d else gamedata / "trophies") / lib.TROPHY_FILE


class _Log:
    """The phase's step log, setup/logs/import.log."""

    def __init__(self, path: Path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(path, "w", encoding="utf-8", errors="replace")

    def note(self, text):
        self.f.write(str(text).rstrip("\n") + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


class _Backups:
    """<gamedata>/save-backups/<date-time>/, made when it is first needed, never deleted."""

    def __init__(self, gamedata):
        self.base = Path(gamedata) / "save-backups" if gamedata else None
        self.made = None

    def folder(self) -> Path:
        if self.made is None:
            stamp = time.strftime("%Y-%m-%d_%H-%M-%S")
            p, n = self.base / stamp, 1
            while p.exists():
                n += 1
                p = self.base / f"{stamp}_{n}"
            p.mkdir(parents=True)
            self.made = p
        return self.made


def run(req, data, jr, table, progress=None) -> bool:
    """The import phase: each ticked item, then setup/imported.txt. Returns True when every journal step was already
    done (a resumed run). Raises ImportFailed with the player's sentence."""
    data = Path(data)
    todo = items(req)
    imp = req["import"]
    gamedata = Path(imp["gamedata"]) if imp.get("gamedata") else None
    log = _Log(data / "setup" / "logs" / "import.log")
    try:
        log.note(f"import of {', '.join(todo)} (asked {imp.get('asked')}); gamedata: {gamedata}")
        step = f"import-{todo[0]}" if todo else "import-save"
        exe = rpcs3.find(req.get("rpcs3"))
        if exe is None:
            raise ImportFailed(step, f"RPCS3 is not in {rpcs3.where(req.get('rpcs3') or '-')} any more.", log.path)
        if gamedata is None and set(todo) & {"save", "trophies"}:
            log.note("no gamedata folder: run with --gamedata")
            raise ImportFailed(step, sentence(step), log.path)
        (data / "setup" / IMPORTED).unlink(missing_ok=True)
        found = lib.read(exe, table, gamedata)
        log.note(f"RPCS3: {exe}; user {found.user}; save {found.save.state}; trophies {found.trophies.state} "
                 f"{found.trophies.earned}/{found.trophies.total}; settings: {', '.join(found.settings) or 'none'}")
        backups, results, ran = _Backups(gamedata), {}, False
        for k, item in enumerate(todo):
            if item == "save":
                results[item], did = _save_step(found, imp, gamedata, jr, log, backups)
            elif item == "trophies":
                results[item], did = _trophies_step(found, imp, gamedata, jr, log, backups)
            else:
                results[item], did = _settings(found, data, log), True
            ran = ran or did
            if progress:
                progress((k + 1) / len(todo))
        write_imported(data, exe, found, results)
        return not ran
    finally:
        log.close()


# ---- the save ----

def _save_step(found, imp, gamedata, jr, log, backups):
    src, dest = found.save, lib.port_save_folder(gamedata)
    fp = journal.fingerprint("import-save", imp.get("asked"), str(src.folder or ""),
                             lib.stamp(src.folder) if src.folder else None, str(dest))
    ran, res = jr.run("import-save", fp, lambda: import_save(src, dest, Path(gamedata), log, backups),
                      outputs=[dest / "PARAM.SFO"], log=log.path)
    return res, ran


def import_save(src: lib.Save, dest: Path, gamedata: Path, log, backups) -> dict:
    """docs/90 3.5: stop on a busy save; back up the port's own; stage the copy in <gamedata>/import/ with the port's
    PARAM.SFO and PARAM.PFD; check that RPCS3 did not save meanwhile; then swap the folders by two renames."""
    step = "import-save"
    if src.state == "busy":
        raise ImportFailed(step, SAVE_BUSY, log.path)
    if src.state != "ok":
        raise ImportFailed(step, NO_SAVE, log.path)
    staging = gamedata / "import" / lib.SAVE_DIR
    aside = gamedata / "import" / (lib.SAVE_DIR + ".old")
    try:
        before = lib.stamp(src.folder)
        log.note(f"RPCS3's save: {src.folder}, {len(before)} files, newest {src.saved}")
        vals = sfo.parse((src.folder / "PARAM.SFO").read_bytes())
        _put_back(aside, dest, log, backups)
        backup = None
        if dest.is_dir() and any(dest.iterdir()):
            backup = backups.folder() / lib.SAVE_DIR
            copy_checked(dest, backup)
            log.note(f"the port's own save is backed up in {backup}")
        n = stage(src.folder, vals, staging)
        log.note(f"staged {n} files and the port's PARAM.SFO and PARAM.PFD in {staging}")
    except (OSError, sfo.SfoError) as e:
        log.note(f"FAILED: {e}")
        shutil.rmtree(staging, ignore_errors=True)
        _drop_empty(staging.parent)
        raise ImportFailed(step, sentence(step), log.path) from e
    if lib.stamp(src.folder) != before or _busy(src.folder):
        log.note("RPCS3's save changed while it was copied: nothing was changed here")
        shutil.rmtree(staging, ignore_errors=True)
        _drop_empty(staging.parent)
        raise ImportFailed(step, SAVE_CHANGED, log.path)
    try:
        swap(staging, dest, aside)
    except OSError as e:
        log.note(f"FAILED to swap the folders: {e}")
        raise ImportFailed(step, sentence(step), log.path) from e
    _drop_empty(staging.parent)
    log.note(f"the save is in {dest}")
    return {"saved": src.saved, "characters": src.characters, "backup": str(backup or ""), "files": n}


def _drop_empty(folder: Path):
    """<gamedata>/import/ goes once nothing is left in it (a folder still in it stays, for the next run)."""
    try:
        folder.rmdir()
    except OSError:
        pass


def _busy(folder: Path) -> bool:
    return any((folder.parent / (pre + folder.name)).exists() for pre in (".working_", ".backup_"))


def stage(folder: Path, vals: dict, staging: Path) -> int:
    """The new save folder: RPCS3's files copied raw (the data files, ICON0.PNG, PIC1.PNG), then PARAM.SFO and
    PARAM.PFD in the port's text formats. Returns how many files were copied."""
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    n = 0
    for e in lib.files(folder):
        if e.name.upper() in ("PARAM.SFO", "PARAM.PFD"):
            continue
        shutil.copy2(e.path, staging / e.name)
        n += 1
    (staging / "PARAM.SFO").write_bytes(port_sfo(vals))
    pfd = port_pfd(vals)
    if pfd:
        (staging / "PARAM.PFD").write_bytes(pfd)
    return n


def port_sfo(vals: dict) -> bytes:
    """The port's PARAM.SFO (ps3recomp cellSaveData.c write_param_sfo and read_param_sfo): five key=value lines, from
    the PSF's TITLE, SUB_TITLE, DETAIL, ATTRIBUTE and SAVEDATA_LIST_PARAM. A value ends at a line break, as the port
    reads it back."""
    def text(key):
        v = vals.get(key, "")
        return re.split(r"[\r\n]", v if isinstance(v, str) else str(v), maxsplit=1)[0]

    attr = vals.get("ATTRIBUTE", 0)
    attr = attr if isinstance(attr, int) else int(attr) if str(attr).strip().isdigit() else 0
    lines = [f"TITLE={text('TITLE')}", f"SUB_TITLE={text('SUB_TITLE')}", f"DETAIL={text('DETAIL')}",
             f"ATTRIBUTE={attr & 0xFFFFFFFF}", f"LIST_PARAM={text('SAVEDATA_LIST_PARAM')}"]
    return ("\n".join(lines) + "\n").encode("utf-8")


def port_pfd(vals: dict):
    """The port's PARAM.PFD (cellSaveData.c pfd_add): the secure files, one name a line, from RPCS3's "*<file>" keys
    whose value is 1 (cellSaveData.cpp). None when there are none."""
    names = [k[1:] for k, v in vals.items() if k.startswith("*") and len(k) > 1 and isinstance(v, int) and v]
    return ("\n".join(names) + "\n").encode("utf-8") if names else None


def copy_checked(src: Path, dst: Path):
    """A copy of a folder whose every file has the original's SHA-256, or OSError."""
    shutil.copytree(src, dst, copy_function=shutil.copy2)
    for root, _dirs, names in os.walk(src):
        for name in names:
            a = Path(root) / name
            b = dst / a.relative_to(src)
            if gamefiles.sha256_file(a) != gamefiles.sha256_file(b):
                raise OSError(f"the copy of {a} does not match it")


def _put_back(aside: Path, dest: Path, log, backups):
    """A folder a stopped swap left aside: put back when the save folder is missing, else kept with the backups."""
    if not aside.exists():
        return
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.rename(aside, dest)
        log.note(f"put back the save a stopped import had moved to {aside}")
    else:
        keep = backups.folder() / (lib.SAVE_DIR + ".old")
        os.rename(aside, keep)
        log.note(f"kept a folder a stopped import left in {aside} as {keep}")


def swap(staging: Path, dest: Path, aside: Path):
    """The port's folder out of the way, the staged one in: two renames on one drive. If the second fails, the first
    is undone. The folder moved aside goes once the new one is in: the backup holds the same files."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    moved = dest.exists()
    if moved:
        os.rename(dest, aside)
    try:
        os.rename(staging, dest)
    except OSError:
        if moved:
            os.rename(aside, dest)
        raise
    if moved:
        shutil.rmtree(aside, ignore_errors=True)


# ---- trophies ----

def _trophies_step(found, imp, gamedata, jr, log, backups):
    t, path = found.trophies, trophy_file(gamedata)
    src_sum = gamefiles.sha256_file(t.path) if t.path and t.path.is_file() else None
    fp = journal.fingerprint("import-trophies", imp.get("asked"), str(t.path or ""), src_sum, str(path))
    ran, res = jr.run("import-trophies", fp, lambda: import_trophies(t, path, log, backups), outputs=[path],
                      log=log.path)
    return res, ran


def import_trophies(t: lib.Trophies, path: Path, log, backups) -> dict:
    """docs/44 8.5: the port's file copied aside first, then RPCS3's unlocked trophies merged in as a union, where the
    earlier time wins, written through a temporary file and a rename."""
    step = "import-trophies"
    if t.path is None:
        raise ImportFailed(step, NO_TROPHIES, log.path)
    try:
        total, theirs = lib.read_tropusr(t.path.read_bytes())
        if total != lib.TROPHY_COUNT:
            log.note(f"{t.path} has {total} trophies; Demon's Souls has {lib.TROPHY_COUNT}")
            raise ImportFailed(step, sentence(step), log.path)
        ours, backup = {}, None
        if path.is_file():
            backup = backups.folder() / "trophies" / path.name
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, backup)
            if gamefiles.sha256_file(backup) != gamefiles.sha256_file(path):
                raise OSError(f"the copy of {path} does not match it")
            got = read_port_trophies(path.read_bytes().decode("utf-8", "replace"))
            if got is None:
                log.note(f"{path} is not a trophy file: it is kept in {backup}, and the merge starts empty")
            ours = got or {}
        merged = merge_trophies(ours, theirs)
        write_port_trophies(path, merged)
    except (OSError, lib.TrophyError) as e:
        log.note(f"FAILED: {e}")
        raise ImportFailed(step, sentence(step), log.path) from e
    added = len(set(merged) - set(ours))
    log.note(f"trophies: {len(theirs)} in RPCS3, {len(ours)} here before, {added} added, {len(merged)} now; {path}")
    return {"earned": len(merged), "total": lib.TROPHY_COUNT, "added": added, "backup": str(backup or "")}


def read_port_trophies(text: str):
    """{id: Unix seconds} from the port's trophy file, read as sceNpTrophy.c's trophy_load_file reads it (the objects
    of the "trophies" list, each with "id" and "timestamp"), or None when it is not one."""
    at = text.find('"trophies"')
    start = text.find("[", at) if at >= 0 else -1
    if start < 0:
        return None
    out = {}
    for m in re.finditer(r"\s*,?\s*\{([^}]*)\}", text[start + 1:]):
        tid = re.search(r'"id"\s*:\s*(\d+)', m.group(1))
        ts = re.search(r'"timestamp"\s*:\s*(\d+)', m.group(1))
        if tid and int(tid.group(1)) < MAX_TROPHIES:
            out[int(tid.group(1))] = int(ts.group(1)) if ts else 0
    return out


def merge_trophies(ours: dict, theirs: dict) -> dict:
    """The union; a trophy in both keeps the earlier known time (0 means not known)."""
    out = dict(ours)
    for tid, ts in theirs.items():
        if not 0 <= tid < MAX_TROPHIES:
            continue
        times = [t for t in (out.get(tid, 0), ts) if t > 0]
        out[tid] = min(times) if times else 0
    return out


def write_port_trophies(path: Path, unlocked: dict):
    """The port's trophy file, laid out as sceNpTrophy.c's trophy_save writes it, through a temporary file."""
    body = ",".join(f'\n    {{ "id": {tid}, "timestamp": {unlocked[tid]} }}' for tid in sorted(unlocked))
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".setup.tmp")
    tmp.write_bytes(('{\n  "trophies": [' + body + "\n  ]\n}\n").encode("utf-8"))
    os.replace(tmp, path)


# ---- settings, and what came across ----

def _settings(found, data: Path, log) -> dict:
    try:
        write_keys(data / "setup" / SETTINGS_FILE, "Written by des_setup from the player's RPCS3 settings and "
                   "patches: the options page starts Custom from them (docs/90 3.5).", found.settings)
    except OSError as e:
        log.note(f"FAILED: {e}")
        raise ImportFailed("import-settings", sentence("import-settings"), log.path) from e
    log.note(f"settings: {found.settings or 'none differ from the port defaults'}")
    return {"count": len(found.settings)}


def write_imported(data: Path, exe, found, results: dict):
    """setup/imported.txt: what came across, for the Ready to play page (docs/90 1.9). 0 or 1 for each item, then its
    facts: the save's characters ('-': not known), its time and the backup of the port's own save ('' when there was
    none), the trophies unlocked here now and how many were new, the number of settings."""
    s, t, c = results.get("save"), results.get("trophies"), results.get("settings")
    fields = {"format": 1, "rpcs3": rpcs3.where(exe), "user": found.user, "save": int(bool(s))}
    if s:
        fields.update(save_characters=s.get("characters") or "-", save_saved=s.get("saved") or "-",
                      save_backup=s.get("backup", ""))
    fields["trophies"] = int(bool(t))
    if t:
        fields.update(trophies_earned=t["earned"], trophies_total=t["total"], trophies_added=t["added"],
                      trophies_backup=t.get("backup", ""))
    fields["settings"] = int(bool(c))
    if c:
        fields["settings_count"] = c["count"]
    write_keys(data / "setup" / IMPORTED, "Written by des_setup when it brought these across from RPCS3; the host "
               "reads it for Ready to play (docs/90 1.9).", fields)


def write_keys(path: Path, comment: str, fields: dict):
    """key=value lines like ready.txt's, so the host reads them without a JSON parser; written whole, then renamed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    lines = ["# " + comment] + [f"{k}={v}" for k, v in fields.items()]
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(tmp, path)
