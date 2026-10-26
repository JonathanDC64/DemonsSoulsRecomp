"""Check the player's game folder: BLUS30443 version 1.00 only, by SHA-256.

Setup accepts only files the player has already decrypted (docs/51, plan (c)):
EBOOT.elf beside EBOOT.BIN and a .sdat.unedat beside each of the 26 .sdat
files, as RPCS3's Decrypt PS3 Binaries writes them, or all of them in one
folder the player picks. It never decrypts anything itself. On the RPCS3
route (docs/90) the player's own RPCS3 makes the missing ones in the unlock
phase (rpcs3.py), and libsre.prx too when RPCS3 has the PS3 system software,
so the check only says which.

The table (game_table.json) holds sizes and SHA-256 sums only. A decrypted
file whose sum is not in the table yet (null) is checked by its first bytes
instead, and reported as such.
"""
import hashlib
import json
import os
from dataclasses import dataclass, field
from pathlib import Path

from . import firmware as fwver
from . import sfo

TABLE = Path(__file__).with_name("game_table.json")

# First bytes of files that are still encrypted.
ENCRYPTED_MAGIC = {b"SCE\0": "an encrypted SELF", b"NPD\0": "an encrypted SDATA file"}
# The RPCS3 route when RPCS3 has no PS3 system software, so it cannot unlock the firmware's file.
NO_FIRMWARE = ("RPCS3 has no PS3 system software installed. Install it in RPCS3 (File, then Install Firmware), then "
               "press Check again.")


@dataclass
class Item:
    kind: str        # "game", "original", "decrypted", "firmware"
    name: str        # the table's path or name
    state: str       # "ok", "unmeasured", "missing", "mismatch", "encrypted", "wrong"
    message: str     # what to tell the player, in plain words
    found: str = ""  # where the file was found
    sha256: str = "" # its SHA-256, when it was hashed
    unlock: bool = False  # the RPCS3 route: the unlock makes this file, so it is not the player's to fix
    fix: str = ""    # the firmware's file from another system software version: the table's version (hex) to set in
                     # it as it is staged (firmware.py); its sha256 is then the table's

    @property
    def good(self) -> bool:
        return self.state in ("ok", "unmeasured")


@dataclass
class Report:
    root: Path = None
    items: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.root is not None and bool(self.items) and all(i.good for i in self.items)

    def problems(self) -> list:
        return [i for i in self.items if not i.good]

    def inputs(self) -> list:
        """The decrypted files and the firmware file the build reads: their items."""
        return [i for i in self.items if i.kind in ("decrypted", "firmware") and i.good]


def load_table(path=None) -> dict:
    with open(path or TABLE, encoding="utf-8") as f:
        return json.load(f)


def sha256_file(path, progress=None, bufsize=1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
            if progress:
                progress(len(b))
    return h.hexdigest()


def find_root(folder) -> Path:
    """The folder that holds PS3_GAME, given that folder, PS3_GAME itself, or None."""
    p = Path(folder)
    if _find_ci(p, "PS3_GAME") and _find_ci(p / _find_ci(p, "PS3_GAME"), "PARAM.SFO"):
        return p
    if p.name.upper() == "PS3_GAME" and _find_ci(p, "PARAM.SFO"):
        return p.parent
    return None


def _find_ci(folder: Path, name: str) -> str:
    """`name` in `folder`, ignoring case (a folder dump may say EBOOT.ELF or EBOOT.elf)."""
    if (folder / name).exists():
        return name
    try:
        low = name.lower()
        for e in os.listdir(folder):
            if e.lower() == low:
                return e
    except OSError:
        pass
    return ""


def _resolve(root: Path, rel: str) -> Path:
    """`rel` (a table path, "/"-separated) under root, matching each part ignoring case."""
    p = root
    for part in rel.split("/"):
        hit = _find_ci(p, part)
        if not hit:
            return None
        p = p / hit
    return p


def _find_decrypted(folder: Path, rel: str) -> Path:
    """A decrypted file in a folder the player picked: by its name at the top, or at
    its own path under the folder (RPCS3 writes beside each input, so a decrypted
    copy of USRDIR, PS3_GAME or the whole dump keeps the game's subfolders)."""
    hit = _find_ci(folder, rel.rsplit("/", 1)[-1])
    if hit:
        return folder / hit
    for cut in (rel, rel.split("PS3_GAME/", 1)[-1], rel.split("USRDIR/", 1)[-1]):
        p = _resolve(folder, cut)
        if p is not None:
            return p
    return None


def _magic(path: Path, n=4) -> bytes:
    with open(path, "rb") as f:
        return f.read(n)


def other_title(t, tid) -> str:
    """The sentence for a copy of another title or version of the game, by its TITLE_ID."""
    what = t["other_titles"].get(tid, f"a game with the ID {tid or 'none'}")
    return f"This is {what} ({tid}). The port supports the US disc, {t['title_id']}, only."


def check(folder, decrypted_dirs=(), firmware=None, table=None, progress=None, unlocks=False, also=None,
          firmware_unlock=None) -> Report:
    """Check a game folder. decrypted_dirs: more folders to look in for the
    decrypted files, by file name. firmware: the decrypted libsre.prx, or None
    to look for it by name in the game's folder and in decrypted_dirs (and
    also's places). progress(bytes) is called while hashing.
    unlocks: the RPCS3 route, where the unlock makes a missing decrypted file
    (the sentences say so). also: {table path or firmware name: [files]}, more
    files to try (PPU Debug's dump of the EBOOT, a copy an earlier run kept).
    A file is good when any of the places has a good copy.
    firmware_unlock, on the RPCS3 route: True when RPCS3 has the PS3 system
    software, so the unlock makes a missing or wrong libsre.prx; False when it
    has none, so the player installs it first. None: the folder route."""
    t = table or load_table()
    rep = Report()
    root = find_root(folder)
    if root is None:
        rep.items.append(Item("game", "PS3_GAME", "missing",
                              "This folder has no PS3_GAME folder with a PARAM.SFO in it. Pick the folder "
                              "of your disc dump that holds PS3_GAME."))
        return rep
    rep.root = root

    # The title and version, from PARAM.SFO.
    try:
        vals = sfo.parse(_resolve(root, "PS3_GAME/PARAM.SFO").read_bytes())
    except (OSError, sfo.SfoError) as e:
        rep.items.append(Item("game", "PARAM.SFO", "wrong", f"PARAM.SFO cannot be read ({e})."))
        return rep
    tid = vals.get("TITLE_ID", "")
    if tid != t["title_id"]:
        rep.items.append(Item("game", "TITLE_ID", "wrong", other_title(t, tid)))
        return rep
    ver = vals.get("APP_VER", ""), vals.get("VERSION", "")
    if ver != (t["app_ver"], t["version"]):
        rep.items.append(Item("game", "APP_VER", "wrong",
                              f"This copy is version {ver[0] or '?'}. The port supports version "
                              f"{t['app_ver']} only."))
        return rep
    rep.items.append(Item("game", "TITLE_ID", "ok", f"{t['title_id']}, version {t['app_ver']}."))

    # The disc's own files: proof that the dump is this version and intact.
    for e in t["originals"]:
        rep.items.append(_check_file("original", e, _resolve(root, e["path"]), progress,
                                     missing=f"{e['path']} is missing from the game folder.",
                                     mismatch=f"{e['path']} does not match the US disc, version 1.00. The dump may "
                                              f"be damaged: dump the disc again."))

    # The decrypted files the build reads: the first good copy, else what is wrong with the first one found.
    for e in t["decrypted"]:
        base = e["path"].rsplit("/", 1)[-1]
        src = e["of"].rsplit("/", 1)[-1]
        if unlocks:
            texts = dict(missing=f"{base} is missing: your RPCS3 unlocks it next.",
                         mismatch=f"{base} does not match: your RPCS3 unlocks it again.")
        else:
            texts = dict(missing=f"{base} is missing: decrypt {src} first (in RPCS3: Utilities, "
                                 f"Decrypt PS3 Binaries).",
                         mismatch=f"{base} does not match: decrypt {src} again.")
        places = [_resolve(root, e["path"])] + [_find_decrypted(Path(d), e["path"]) for d in decrypted_dirs]
        places += [Path(p) for p in (also or {}).get(e["path"], ())]
        first = None
        for p in places:
            if p is None or not p.is_file():
                continue
            item = _check_file("decrypted", e, p, progress, **texts)
            if item.good:
                break
            first = first or item
        else:
            item = first or _check_file("decrypted", e, None, progress, **texts)
        item.unlock = unlocks
        rep.items.append(item)

    # The firmware's file: the one given, else one by its name in the game's folder (beside PS3_GAME) or in a
    # folder of decrypted files. The build needs it (the lifted libsre), so it is always checked.
    for e in t.get("firmware", []):
        mismatch = f"{e['name']} does not match the copy the port was tested with ({e['from']})."
        unlockable = firmware is None and firmware_unlock is True
        if firmware is not None:
            places = [Path(firmware)]
            missing = f"{e['name']} was not found at {firmware}."
        else:
            places = [d / _find_ci(d, e["name"]) for d in [root] + [Path(x) for x in decrypted_dirs]
                      if _find_ci(d, e["name"])]
            places += [Path(p) for p in (also or {}).get(e["name"], ())]
            missing = (f"{e['name']} is missing. It is one file of the PS3 system software: decrypt "
                       f"{e['from'].split(',')[0]} with RPCS3 (Utilities, Decrypt PS3 Binaries), then put "
                       f"{e['name']} in the game's folder, next to PS3_GAME.")
            if unlockable:
                missing = f"{e['name']} is missing: your RPCS3 unlocks it next."
                mismatch = f"{e['name']} does not match: your RPCS3 unlocks it again."
            elif firmware_unlock is False:
                missing = mismatch = NO_FIRMWARE
        first = None
        for p in places:
            if not p.is_file():
                continue
            item = _check_file("firmware", e, p, progress, missing=missing, mismatch=mismatch)
            if item.good:
                break
            first = first or item
        else:
            item = first or _check_file("firmware", e, None, progress, missing=missing, mismatch=mismatch)
        item.unlock = unlockable
        rep.items.append(item)
    return rep


def _check_file(kind, e, path, progress, missing, mismatch) -> Item:
    name = e.get("path") or e["name"]
    if path is None or not path.is_file():
        return Item(kind, name, "missing", missing)
    size = path.stat().st_size
    head = _magic(path)
    if head in ENCRYPTED_MAGIC and kind in ("decrypted", "firmware"):
        return Item(kind, name, "encrypted", f"{path.name} is still {ENCRYPTED_MAGIC[head]}: decrypt it first.",
                    str(path))
    if e.get("magic") and head != bytes.fromhex(e["magic"]):
        return Item(kind, name, "mismatch", mismatch, str(path))
    if e.get("size") is not None and size != e["size"]:
        return Item(kind, name, "mismatch", mismatch, str(path))
    got = sha256_file(path, progress)
    if e.get("sha256") is None:
        return Item(kind, name, "unmeasured", f"{path.name} found. Its sum is not in the table yet, so only its "
                                              f"first bytes were checked.", str(path), got)
    if got != e["sha256"] and kind == "firmware":
        fixed = fwver.matched(path, e)
        if fixed:
            want = int(e["version"], 16)
            return Item(kind, name, "ok", f"{path.name} matches. It is from PS3 system software "
                        f"{fwver.name(fixed[1])}; setup sets its version to {fwver.name(want)}, the one the port is "
                        f"tested with.", str(path), e["sha256"], fix=e["version"])
    if got != e["sha256"]:
        return Item(kind, name, "mismatch", mismatch, str(path), got)
    return Item(kind, name, "ok", f"{path.name} matches.", str(path), got)
