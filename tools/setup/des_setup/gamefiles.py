"""Check the player's game folder: BLUS30443 version 1.00 only, by SHA-256.

Setup accepts only files the player has already decrypted (docs/51, plan (c)):
EBOOT.elf beside EBOOT.BIN and a .sdat.unedat beside each of the 26 .sdat
files, as RPCS3's Decrypt PS3 Binaries writes them, or all of them in one
folder the player picks. It never decrypts anything itself.

The table (game_table.json) holds sizes and SHA-256 sums only. A decrypted
file whose sum is not in the table yet (null) is checked by its first bytes
instead, and reported as such.
"""
import hashlib
import json
import os
from dataclasses import dataclass, field
from pathlib import Path

from . import sfo

TABLE = Path(__file__).with_name("game_table.json")

# First bytes of files that are still encrypted.
ENCRYPTED_MAGIC = {b"SCE\0": "an encrypted SELF", b"NPD\0": "an encrypted SDATA file"}


@dataclass
class Item:
    kind: str        # "game", "original", "decrypted", "firmware"
    name: str        # the table's path or name
    state: str       # "ok", "unmeasured", "missing", "mismatch", "encrypted", "wrong"
    message: str     # what to tell the player, in plain words
    found: str = ""  # where the file was found
    sha256: str = "" # its SHA-256, when it was hashed

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


def _magic(path: Path, n=4) -> bytes:
    with open(path, "rb") as f:
        return f.read(n)


def check(folder, decrypted_dirs=(), firmware=None, table=None, progress=None) -> Report:
    """Check a game folder. decrypted_dirs: more folders to look in for the
    decrypted files, by file name. firmware: the decrypted libsre.prx, or None
    to leave it out of the check. progress(bytes) is called while hashing."""
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
        what = t["other_titles"].get(tid, f"a game with the ID {tid or 'none'}")
        rep.items.append(Item("game", "TITLE_ID", "wrong",
                              f"This is {what} ({tid}). The port supports the US disc, {t['title_id']}, only."))
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

    # The decrypted files the build reads.
    for e in t["decrypted"]:
        base = e["path"].rsplit("/", 1)[-1]
        p = _resolve(root, e["path"])
        for d in decrypted_dirs:
            if p is not None:
                break
            hit = _find_ci(Path(d), base)
            p = Path(d) / hit if hit else None
        src = e["of"].rsplit("/", 1)[-1]
        rep.items.append(_check_file("decrypted", e, p, progress,
                                     missing=f"{base} is missing: decrypt {src} first (in RPCS3: Utilities, "
                                             f"Decrypt PS3 Binaries).",
                                     mismatch=f"{base} does not match: decrypt {src} again."))

    # The firmware file, when asked for.
    for e in t.get("firmware", []):
        if firmware is None:
            continue
        p = Path(firmware)
        rep.items.append(_check_file("firmware", e, p if p.is_file() else None, progress,
                                     missing=f"{e['name']} was not found at {firmware}.",
                                     mismatch=f"{e['name']} does not match the copy the port was tested with "
                                              f"({e['from']})."))
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
    if got != e["sha256"]:
        return Item(kind, name, "mismatch", mismatch, str(path), got)
    return Item(kind, name, "ok", f"{path.name} matches.", str(path), got)
