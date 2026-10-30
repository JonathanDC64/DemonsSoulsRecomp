"""The RPCS3 route's reading of the player's RPCS3 (docs/90 3.1): where it
keeps its files, Demon's Souls in its library, its users, the save, the
trophies, the settings and patches, and the PS3 system software's libsre.sprx.

Everything here only reads. Files are opened for reading and folders are
listed; nothing is ever written, renamed or locked under RPCS3's folders. A
file that is missing or cannot be read counts as absent, so the check always
gets an answer.

Where things are (Windows; on Linux the .yml files sit in <R> itself):
  <R>                         RPCS3's config folder (rpcs3.player_dirs)
  <R>config/vfs.yml           $(EmulatorDir), /dev_hdd0/ and /dev_flash/ (RPCS3's defaults when absent)
  <R>config/games.yml         the library: BLUS30443's folder or ISO file (older builds: <R>games.yml)
  <R>GuiConfigs/persistent_settings.dat   [Users] active_user, the user RPCS3 uses
  <hdd0>home/<8 digits>/      each user: localusername, savedata/, trophy/
  <hdd0>game/NPUB30910/       the PlayStation Store version, which the port does not support
  <R>config/config.yml, custom_configs/config_BLUS30443.yml, patch_config.yml   the settings and patches
  <dev_flash>sys/external/libsre.sprx     the PS3 system software's file the build lifts
  <dev_flash>vsh/etc/version.txt          its version
"""
import os
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import rpcs3, sfo, yaml_subset

SAVE_DIR = "BLUS30443DEMONSS005"    # the game's save folder: one folder holds the system data and every character
TROPHY_DIR = "NPWR00881_00"         # its trophy set, under home/<user>/trophy/
TROPHY_FILE = "NPWR00881.json"      # the port's own trophy file (ps3recomp sceNpTrophy.c)
TROPHY_COUNT = 38                   # the trophies the set has
STORE_ID = "NPUB30910"              # the US PlayStation Store version
DEFAULT_USER = "00000001"
SPRX = "sys/external/libsre.sprx"   # under dev_flash
VERSION_TXT = "vsh/etc/version.txt" # under dev_flash: the system software's version
# A save's own files, which are not game data (cellSaveData's content types and its system files).
CONTENT = {"PARAM.SFO", "PARAM.PFD", "ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"}
TROPUSR_MAGIC = 0x818F54AD
TICK_UNIX = 62135596800000000       # CellRtcTick (microseconds since 0001-01-01) of 1970-01-01

# The settings this route brings across, by their settings.ini names, in the order the page lists them, with the
# port's defaults ("Original", the game as on the PS3): only values that differ from these are brought across.
# Not V-sync: RPCS3's default is off and most players never change it, so it would turn the port's off for nearly
# everyone.
DEFAULTS = {"DES_FPS": "", "RSX_SCALE": "100", "DES_MOTION_BLUR": "1", "DES_SKIP_INTRO": "0", "RSX_ANISO": "0"}
PATCH_FPS, PATCH_INTRO, PATCH_BLUR = "Unlock FPS", "Skip Intro Videos", "Disable Motion Blur"
FPS_MIN, FPS_MAX = 30, 500          # the port's frame rates (DES_FPS 30-500, or unlocked)
SCALE_MIN, SCALE_MAX = 50, 800      # the port's render resolution (RSX_SCALE)
ANISO = (0, 1, 2, 4, 8, 16)         # the port's anisotropic filtering (RSX_ANISO): 0 is the game's own


class TrophyError(ValueError):
    pass


@dataclass
class Folders:
    root: Path      # <R>
    config: Path    # where its .yml files are
    hdd0: Path
    flash: Path


@dataclass
class User:
    id: str         # 8 digits
    name: str       # its localusername


@dataclass
class Library:
    kind: str           # folder, iso, store or none
    path: Path = None   # the folder that holds PS3_GAME, the ISO file, or the store version's folder


@dataclass
class Save:
    state: str              # ok, none, other-user or busy (the port's own: ok or none)
    folder: Path = None
    saved: int = None       # its newest data file's time, Unix seconds
    characters: int = None  # None: not known (docs/90 I18: the save does not say)


@dataclass
class Trophies:
    state: str                                      # ok (some are unlocked) or none
    earned: int = 0
    total: int = 0
    unlocked: dict = field(default_factory=dict)    # trophy id: Unix seconds (0 when not known)
    path: Path = None


@dataclass
class Found:
    """What the RPCS3 route's check reports (docs/90 3.8)."""
    folders: Folders
    users: list
    user: str               # the active user's ID
    library: Library
    save: Save
    current: Save           # the port's own save, which an import replaces
    trophies: Trophies
    settings: dict          # {settings.ini name: value}, only those that differ from the port's defaults
    firmware: Path = None   # libsre.sprx, or None when RPCS3 has no PS3 system software

    @property
    def user_name(self) -> str:
        return next((u.name for u in self.users if u.id == self.user), "")


def read(exe, table, gamedata=None) -> Found:
    """Everything the check reports about this RPCS3, for the table's title. gamedata: the port's gamedata folder (its
    own save), or None."""
    f = folders(exe)
    us = users(f)
    user = active_user(f, us)
    return Found(f, us, user, library(f, table["title_id"]), save(f, user, us), port_save(gamedata),
                 trophies(f, user), settings(f, table["title_id"], table["app_ver"]), firmware(f))


# ---- where things are ----

def folders(exe) -> Folders:
    """RPCS3's folders, as vfs_config.cpp works them out: $(EmulatorDir) is vfs.yml's, else the config folder."""
    root = rpcs3.player_dirs(exe)["config"]
    conf = root / "config" if os.name == "nt" else root
    vfs = yaml_subset.load_file(conf / "vfs.yml")

    def text(key):
        v = vfs.get(key)
        return v if isinstance(v, str) else ""

    emu = text("$(EmulatorDir)") or str(root)
    if not emu.endswith(("/", "\\")):
        emu += "/"

    def dev(key, default):
        return Path((text(key) or default).replace("$(EmulatorDir)", emu))

    return Folders(root, conf, dev("/dev_hdd0/", "$(EmulatorDir)dev_hdd0/"),
                   dev("/dev_flash/", "$(EmulatorDir)dev_flash/"))


def firmware(f: Folders):
    p = f.flash / SPRX
    return p if p.is_file() else None


def firmware_version(f: Folders) -> str:
    """The PS3 system software version RPCS3 has, as "4.93" (its version.txt's "release:04.9300:"), or ""."""
    try:
        text = (f.flash / VERSION_TXT).read_text(encoding="ascii", errors="replace")
    except OSError:
        return ""
    m = re.search(r"^release:(\d+)\.(\d\d)", text, re.M)
    return f"{int(m.group(1))}.{m.group(2)}" if m else ""


# ---- the library ----

def library(f: Folders, title_id) -> Library:
    """The title's games.yml entry (System.cpp, game_enumeration.h): a disc root, a PS3_GAME folder marked by a
    trailing "/./", or an ISO file. With none, the store version's folder if RPCS3 has it."""
    for p in (f.config / "games.yml", f.root / "games.yml"):
        v = yaml_subset.load_file(p).get(title_id)
        if isinstance(v, str) and v.strip():
            return entry(v.strip())
    store = f.hdd0 / "game" / STORE_ID
    return Library("store", store) if store.is_dir() else Library("none")


def entry(text: str) -> Library:
    """One games.yml path, as the folder that holds PS3_GAME (or the ISO file)."""
    p = text.replace("\\", "/")
    if p.lower().endswith(".iso") or Path(p).is_file():
        return Library("iso", Path(p))
    p = p.rstrip("/")
    while p.endswith("/."):
        p = p[:-2].rstrip("/")
    folder = Path(p)
    return Library("folder", folder.parent if folder.name.upper() == "PS3_GAME" else folder)


# ---- users ----

def users(f: Folders) -> list:
    """RPCS3's users (user_account.cpp): the 8-digit folders under home/ that have a localusername, in ID order."""
    home = f.hdd0 / "home"
    try:
        names = sorted(os.listdir(home))
    except OSError:
        return []
    return [User(n, _name(home / n / "localusername")) for n in names
            if len(n) == 8 and n.isdigit() and int(n) > 0 and (home / n / "localusername").is_file()]


def _name(p: Path) -> str:
    try:
        return p.read_bytes().decode("utf-8", "replace").split("\0", 1)[0][:16]   # RPCS3 keeps 16 characters
    except OSError:
        return ""


def active_user(f: Folders, found: list) -> str:
    """[Users] active_user of persistent_settings.dat (default 00000001). When that user has no folder: 00000001 if
    RPCS3 has it, else its first user."""
    want = ini_value(f.root / "GuiConfigs" / "persistent_settings.dat", "Users", "active_user")
    ids = [u.id for u in found]
    if want in ids:
        return want
    return DEFAULT_USER if DEFAULT_USER in ids or not ids else ids[0]


def ini_value(path, section, key) -> str:
    """One value of a QSettings INI file, or ''."""
    try:
        text = Path(path).read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""
    here = ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            here = line[1:-1].strip()
        elif here == section and "=" in line:
            k, v = line.split("=", 1)
            if k.strip() == key:
                return v.strip().strip('"')
    return ""


# ---- the save ----

def save(f: Folders, user, found: list) -> Save:
    """The user's save: busy while a .working_ or .backup_ folder sits beside it (RPCS3 is mid-save, cellSaveData.cpp),
    ok when its PARAM.SFO reads, else other-user when another user has one, else none."""
    base = f.hdd0 / "home" / user / "savedata"
    folder = base / SAVE_DIR
    if any((base / (pre + SAVE_DIR)).exists() for pre in (".working_", ".backup_")):
        return Save("busy", folder, newest(folder))
    if psf_save(folder):
        return Save("ok", folder, newest(folder))
    others = [u.id for u in found if u.id != user and psf_save(f.hdd0 / "home" / u.id / "savedata" / SAVE_DIR)]
    return Save("other-user" if others else "none")


def psf_save(folder: Path) -> bool:
    """An RPCS3 save: its PARAM.SFO is a PSF that reads."""
    try:
        sfo.parse((folder / "PARAM.SFO").read_bytes())
        return True
    except (OSError, sfo.SfoError):
        return False


def port_save_folder(gamedata) -> Path:
    """Where the game keeps its save (ps3recomp cellSaveData.c): PS3_SAVEDATA_ROOT, else
    gamedata/dev_hdd0/home/00000001/savedata. A relative root is the game's working folder's (the gamedata's
    parent)."""
    gamedata = Path(gamedata)
    root = os.environ.get("PS3_SAVEDATA_ROOT")
    base = (gamedata.parent / root) if root else gamedata / "dev_hdd0" / "home" / DEFAULT_USER / "savedata"
    return base / SAVE_DIR


def port_save(gamedata) -> Save:
    """The port's own save: ok when it has a PARAM.SFO (the game's own test, dir_has_save), else none."""
    if gamedata is None:
        return Save("none")
    folder = port_save_folder(gamedata)
    return Save("ok", folder, newest(folder)) if (folder / "PARAM.SFO").is_file() else Save("none", folder)


def files(folder: Path) -> list:
    """The save's files (not folders), as os.DirEntry, by name."""
    try:
        with os.scandir(folder) as it:
            return sorted((e for e in it if e.is_file()), key=lambda e: e.name)
    except OSError:
        return []


def newest(folder: Path):
    """The newest data file's time, Unix seconds (the save's own files count only when it has no data file)."""
    entries = files(folder)
    data = [e for e in entries if e.name.upper() not in CONTENT] or entries
    times = []
    for e in data:
        try:
            times.append(int(e.stat().st_mtime))
        except OSError:
            pass
    return max(times) if times else None


def stamp(folder: Path) -> list:
    """Each file's name, size and time: what changes when RPCS3 saves (it writes the files it changed anew)."""
    out = []
    for e in files(folder):
        try:
            st = e.stat()
            out.append((e.name, st.st_size, st.st_mtime_ns))
        except OSError:
            out.append((e.name, -1, -1))
    return out


# ---- trophies ----

def trophies(f: Folders, user) -> Trophies:
    p = f.hdd0 / "home" / user / "trophy" / TROPHY_DIR / "TROPUSR.DAT"
    try:
        total, unlocked = read_tropusr(p.read_bytes())
    except (OSError, TrophyError):
        return Trophies("none")
    return Trophies("ok" if unlocked else "none", len(unlocked), total, unlocked, p)


def read_tropusr(data: bytes):
    """(the trophy count, {id: Unix seconds} of the unlocked ones) from a TROPUSR.DAT (TROPUSR.h): a 0x30-byte header
    (magic, ?, the table count), 0x20-byte table headers from 0x30 (type, entry size, ?, count, offset), and in table 6
    entries of 0x10 + entry size bytes: type, size, entry id, ?, then the trophy id, its state (1: unlocked), two words
    and two CellRtcTick times; RPCS3 reports the second."""
    if len(data) < 0x30 or struct.unpack_from(">I", data, 0)[0] != TROPUSR_MAGIC:
        raise TrophyError("not a TROPUSR.DAT")
    count = struct.unpack_from(">I", data, 8)[0]
    if 0x30 + count * 0x20 > len(data):
        raise TrophyError("its table headers are cut short")
    for i in range(count):
        kind, size, _one, n, off = struct.unpack_from(">IIIIQ", data, 0x30 + i * 0x20)
        if kind != 6:
            continue
        stride = 0x10 + size
        if size < 0x20 or off + n * stride > len(data):
            raise TrophyError("its trophy table is cut short")
        unlocked = {}
        for k in range(n):
            e = off + k * stride
            etype, _size, _eid, _unk, tid, state = struct.unpack_from(">IIIIII", data, e)
            if etype == 6 and state:
                tick = struct.unpack_from(">Q", data, e + 0x28)[0]
                unlocked[tid] = (tick - TICK_UNIX) // 1_000_000 if tick > TICK_UNIX else 0
        return n, unlocked
    raise TrophyError("it has no trophy table")


# ---- settings and patches ----

def settings(f: Folders, title_id, app_ver) -> dict:
    """The settings that come across (docs/90 3.5), by their settings.ini names, in DEFAULTS' order: config.yml, the
    title's custom config over it, and the patches enabled in patch_config.yml. Only values that differ from the
    port's defaults. The other keys of these files (a console ID, the GPU's name) are never kept or logged."""
    cfg = _merge(yaml_subset.load_file(f.config / "config.yml"),
                 yaml_subset.load_file(f.config / "custom_configs" / f"config_{title_id}.yml"))
    video = cfg.get("Video") if isinstance(cfg.get("Video"), dict) else {}
    on = patches(f, title_id, app_ver)
    values = {"DES_FPS": frame_rate(video) if PATCH_FPS.lower() in on else "",
              "RSX_SCALE": scale(video),
              "DES_MOTION_BLUR": "0" if PATCH_BLUR.lower() in on else "1",
              "DES_SKIP_INTRO": "1" if PATCH_INTRO.lower() in on else "0",
              "RSX_ANISO": aniso(video)}
    return {k: values[k] for k in DEFAULTS if values[k] is not None and values[k] != DEFAULTS[k]}


def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        out[k] = _merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def patches(f: Folders, title_id, app_ver) -> set:
    """The names (lower case) of the patches enabled for this serial and version, under any hash and title
    (bin_patch.cpp: PPU-<hash> > patch > title > serial > version > Enabled)."""
    on = set()
    for by_hash in yaml_subset.load_file(f.config / "patch_config.yml").values():
        for name, titles in _maps(by_hash):
            for _title, serials in _maps(titles):
                for serial, versions in _maps(serials):
                    if serial not in (title_id, "All"):
                        continue
                    for version, node in _maps(versions):
                        if version in (app_ver, "All") and isinstance(node, dict) and _true(node.get("Enabled")):
                            on.add(name.lower())
    return on


def _maps(d):
    return [(k, v) for k, v in d.items() if isinstance(v, dict)] if isinstance(d, dict) else []


def _true(v) -> bool:
    return isinstance(v, str) and v.strip().lower() in ("true", "yes", "on", "y")


def _number(v, default):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def frame_rate(video: dict):
    """DES_FPS with the Unlock FPS patch on: the lowest limit that is set (RSXThread.cpp). The frame limit's 30, 50,
    60 or 120 (Off, Infinite and Display add none; Auto and PS3 Native are the vblank rate), the second frame limit
    when set, and the vblank rate (60 by default). Above 500: unlocked. Below 30, which the port does not run: None."""
    limits = [_number(video.get("Vblank Rate"), 60) or 60]
    fl = str(video.get("Frame limit", "Auto")).strip()
    if fl in ("30", "50", "60", "120"):
        limits.append(float(fl))
    second = _number(video.get("Second Frame Limit"), 0)
    if second >= 0.1:
        limits.append(second)
    rate = int(round(min(limits)))
    if rate < FPS_MIN:
        return None
    return "unlocked" if rate > FPS_MAX else str(rate)


def scale(video: dict) -> str:
    """RSX_SCALE: Resolution Scale (RPCS3 takes 25-800, else its 100), kept within the port's 50-800."""
    v = int(_number(video.get("Resolution Scale"), 100))
    v = v if 25 <= v <= 800 else 100
    return str(min(SCALE_MAX, max(SCALE_MIN, v)))


def aniso(video: dict) -> str:
    """RSX_ANISO: Anisotropic Filter Override (0 is the game's own), as the port's nearest level at or below it."""
    v = int(_number(video.get("Anisotropic Filter Override"), 0))
    return str(max(a for a in ANISO if a <= min(16, max(0, v))))


