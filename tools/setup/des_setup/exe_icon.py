"""The program's icon in Explorer, on Windows (docs/112 section 7).

A released program comes without one, as a picture made from the game cannot be in the download (docs/51). So setup
gives it one on the player's PC, at the end of a run, once the game's files are there:
  1. The picture. The host draws one of the game's swords (`--icon`, port/ui/src/souls_icon.cpp). If it cannot, the
     game's own icon, PS3_GAME/ICON0.PNG (scripts/make_icon.py).
  2. A copy of the program gets it as its icon resource 1, which the window takes as well (ps3recomp's
     rsx_window_icon.c). The copy is started once (`--program-check`), to see that it still runs.
  3. The copy takes the program's place. The program may be running (it started setup), and Windows lets a running
     exe be renamed but not overwritten, so the old file goes to .update/, and the next start removes it
     (updates.cpp's drop_old_program).

It never fails setup: without an icon the game plays the same. It runs once for each download, as a program with an
icon is left alone, and an update's new program gets its icon from the setup run that follows the update. Only a
player's install is touched (setup/release-info.txt beside the exe), not a developer's build.

DES_SETUP_ICON=0 leaves the program as it is.
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

RT_ICON, RT_GROUP_ICON = 3, 14
GROUP = 1                       # the group's resource number
AS_DATA = 0x00000002 | 0x00000020   # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE
UPDATE = ".update"              # (update.py's)
OLD = "program-before-icon"     # the old file's name there (updates.cpp's drop_old_program)
SRC = Path(__file__).resolve().parents[3]   # the source tree: tools/setup/des_setup/exe_icon.py
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class IconError(Exception):
    pass


def read_ico(path) -> list:
    """An .ico's frames: (width, height, colours, planes, bits, data)."""
    data = Path(path).read_bytes()
    if len(data) < 6:
        raise IconError(f"{path} is too short")
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved or kind != 1 or not count or len(data) < 6 + 16 * count:
        raise IconError(f"{path} is not an icon")
    frames = []
    for i in range(count):
        w, h, colours, _r, planes, bits, size, at = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        body = data[at:at + size]
        if len(body) != size or not size:
            raise IconError(f"{path}: frame {i} is cut short")
        frames.append((w, h, colours, planes, bits, body))
    return frames


def _kernel32():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k.BeginUpdateResourceW.restype = wintypes.HANDLE
    k.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_char_p,
                                  wintypes.DWORD]
    k.UpdateResourceW.restype = wintypes.BOOL
    k.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    k.EndUpdateResourceW.restype = wintypes.BOOL
    k.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    k.LoadLibraryExW.restype = wintypes.HMODULE
    k.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    k.FindResourceW.restype = wintypes.HANDLE
    k.FreeLibrary.argtypes = [wintypes.HMODULE]
    return k


def has_icon(exe) -> bool:
    k = _kernel32()
    h = k.LoadLibraryExW(str(exe), None, AS_DATA)
    if not h:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return bool(k.FindResourceW(h, GROUP, RT_GROUP_ICON))
    finally:
        k.FreeLibrary(h)


def put_icon(exe, frames):
    """frames (read_ico's) into the file `exe` as its icon: one icon resource each, and the group that lists them."""
    k = _kernel32()
    h = k.BeginUpdateResourceW(str(exe), False)
    if not h:
        raise ctypes.WinError(ctypes.get_last_error())
    written = False
    try:
        group = struct.pack("<HHH", 0, 1, len(frames))
        for n, (w, height, colours, planes, bits, body) in enumerate(frames, 1):
            group += struct.pack("<BBBBHHIH", w, height, colours, 0, planes, bits, len(body), n)
            if not k.UpdateResourceW(h, RT_ICON, n, 0, body, len(body)):
                raise ctypes.WinError(ctypes.get_last_error())
        if not k.UpdateResourceW(h, RT_GROUP_ICON, GROUP, 0, group, len(group)):
            raise ctypes.WinError(ctypes.get_last_error())
        written = True
    finally:
        ended = k.EndUpdateResourceW(h, not written)
    if not ended:
        raise ctypes.WinError(ctypes.get_last_error())


def _rename(src, dst, tries=40):
    """A rename that waits a little for a virus scanner to let go of a new file."""
    for i in range(tries):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(0.25)


def swap_in(exe: Path, new: Path):
    """new takes exe's place, exe running or not. The two renames are one after the other: the program is missing
    only for that moment, as in an update's swap (update.py's _order)."""
    keep = exe.parent / UPDATE
    keep.mkdir(exist_ok=True)
    old = keep / f"{OLD}-{os.getpid()}"
    _rename(exe, old)
    try:
        _rename(new, exe)
    except OSError:
        _rename(old, exe)
        raise
    try:
        old.unlink()    # (it stays if it is running: the next start removes it)
    except OSError:
        pass


def _starts(new: Path):
    p = subprocess.run([str(new), "--program-check"], cwd=str(new.parent), stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, creationflags=NO_WINDOW)
    if p.returncode != 0:
        raise IconError(f"with the icon the program ends with {p.returncode}")


def stamp(exe: Path, frames, started=_starts):
    new = exe.with_name(f"{exe.stem}.new{exe.suffix}")
    try:
        shutil.copyfile(exe, new)
        put_icon(new, frames)
        if not has_icon(new):
            raise IconError("the icon did not go in")
        started(new)
        swap_in(exe, new)
    finally:
        try:
            new.unlink(missing_ok=True)
        except OSError:
            pass


def _drawn(prog, game, ico: Path) -> bool:
    """The sword, drawn by the host."""
    try:
        p = subprocess.run(prog + ["--icon", str(game), str(ico)], cwd=str(Path(prog[-1]).parent),
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300,
                           creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return False
    return p.returncode == 0 and ico.is_file()


def _games_own(game, ico: Path) -> bool:
    """The game's own icon."""
    png = Path(game) / "PS3_GAME" / "ICON0.PNG"
    script = SRC / "scripts" / "make_icon.py"
    if not png.is_file() or not script.is_file():
        return False
    try:
        p = subprocess.run([sys.executable, "-X", "utf8", str(script), str(png), str(ico)], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300, creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return False
    return p.returncode == 0 and ico.is_file()


def _tell_explorer(exe: Path):
    """Explorer keeps its own copy of a file's icon: told that the file changed."""
    try:
        shell = ctypes.WinDLL("shell32")
        shell.SHChangeNotify.argtypes = [wintypes.LONG, wintypes.UINT, ctypes.c_wchar_p, ctypes.c_void_p]
        shell.SHChangeNotify(0x00002000, 0x0005, str(exe), None)      # SHCNE_UPDATEITEM, SHCNF_PATHW
        shell.SHChangeNotify(0x08000000, 0x0000, None, None)          # SHCNE_ASSOCCHANGED, SHCNF_IDLIST
    except (OSError, AttributeError, ctypes.ArgumentError):
        pass


def apply(host, game, data, emit) -> bool:
    """The icon, if the program has none. host: the program, or a command list ending in it (pipeline's _shaders). True
    when it was given one."""
    if os.name != "nt" or os.environ.get("DES_SETUP_ICON") == "0":
        return False
    prog = [str(c) for c in host] if isinstance(host, (list, tuple)) else [str(host)] if host else []
    if not prog or not Path(prog[-1]).is_file():
        return False
    exe = Path(prog[-1])
    if not (exe.parent / "setup" / "release-info.txt").is_file():
        return False
    try:
        if has_icon(exe):
            return False
        ico = Path(data) / "setup" / "program.ico"
        ico.parent.mkdir(parents=True, exist_ok=True)
        ico.unlink(missing_ok=True)
        if _drawn(prog, game, ico):
            what = "the sword"
        elif _games_own(game, ico):
            what = "the game's own"
        else:
            emit.text("note the program has no icon (no picture for it)")
            return False
        stamp(exe, read_ico(ico))
        _tell_explorer(exe)
        emit.text(f"ok   the program's icon is in place ({what})")
        return True
    except Exception as e:      # (whatever it was: the game plays the same without an icon)
        emit.text(f"note the program has no icon ({type(e).__name__}: {e})")
        return False
