"""Updates (docs/105): a newer release of the port found, downloaded, checked and put in place of this one, with the
version before it kept to go back to. The host runs these at the start screen and from Settings, About.

  update-check --app VERSION [--data DIR]
      The newest stable release (a vX.Y.Z tag, not a draft or a pre-release) newer than VERSION that has a player's
      download for this system: @release <version> <published> <page>, its notes as @note lines, then @done ok.
      Nothing newer: @done ok alone. No answer (offline, or GitHub's limit): @error download - <sentence> and
      @done failed. Only a plain X.Y.Z version is compared, so a developer's build (0.1.0-dev.5+g1234) finds none.
  update-apply --app VERSION --to VERSION --install DIR [--data DIR]
      download: the release's player zip and its SHA256SUMS, the zip checked against them. install: the zip
      unpacked beside the install (DIR/.update), the game build (the data folder's game/ and setup/ready.txt)
      copied into <data>/previous/<VERSION>, then the program files swapped: this version's into
      DIR/.previous/<VERSION>, the new one's in their place. The setup run that follows rebuilds the game.
  update-back --app VERSION --install DIR [--data DIR]
      The kept version back in place: the same swap the other way, so this version is then the one kept.

Only one version is kept: the one kept before goes once a swap is done. The player's files in the install (gamedata/
with the saves, shader_cache/, a portable install's data/ and portable.txt) never move, nor does anything else that
is not a program file. The program files are the names at the top of the install that release-files.txt lists (the
player's zip has it, tools/release/package.py), or, for an install from before it, the names the new release has.

A swap is a few renames in the same folder, each written to DIR/.update/swap.txt first, so a swap that fails is
undone at once, and one cut short (a power cut) is undone at the program's next start (the host reads the same
journal) or by the next update-apply or update-back. The host keeps its window from closing while a swap runs.

DES_UPDATE_FEED: the releases list from a file, or from another URL (the tests); with it the downloads may come from
http://127.0.0.1 too.
"""
import datetime
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from . import fetch, journal, pipeline, protocol

NAME = "DemonsSoulsRecomp"
REPO = "JonathanDC64/DemonsSoulsRecomp"
FEED = f"https://api.github.com/repos/{REPO}/releases?per_page=20"
STABLE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
MANIFEST = "release-files.txt"
SUMS = "SHA256SUMS"
UPDATE, PREVIOUS = ".update", ".previous"
SWAP = "swap.txt"                  # in UPDATE: the swap's journal (Swap)
# Never moved by a swap, whatever a list says: the player's own files, and the swap's folders.
KEEP = {"gamedata", "shader_cache", "data", "portable.txt", UPDATE, PREVIOUS, PREVIOUS + ".old"}
EXE = "des_host.exe" if os.name == "nt" else "des_host"
NOTE_LINES, NOTE_WIDTH = 300, 500
FEED_TIMEOUT = 15


class UpdateFailed(RuntimeError):
    def __init__(self, code, step, message):
        super().__init__(message)
        self.code, self.step, self.message = code, step, message


def version_of(text):
    """(major, minor, patch) of a plain X.Y.Z (a v in front allowed), else None."""
    m = STABLE.match(str(text or "").strip())
    return tuple(int(g) for g in m.groups()) if m else None


def system() -> str:
    return "windows" if os.name == "nt" else "linux"


def player_asset(version) -> str:
    """The player's download of `version` for this system (package.py's names). Linux has none yet (docs/87)."""
    return f"{NAME}-{version}-windows.zip" if system() == "windows" else f"{NAME}-{version}-linux.zip"


# ---- the releases list ----

def _test_feed() -> str:
    return os.environ.get("DES_UPDATE_FEED", "")


def read_feed(data=None, timeout=FEED_TIMEOUT) -> list:
    """The releases list (GitHub's JSON), newest first as GitHub gives it. The last answer is kept in
    <data>/setup/update/feed.json with its ETag, so an unchanged list costs nothing of GitHub's hourly limit.
    Raises UpdateFailed when there is no answer."""
    src = _test_feed() or FEED
    if not re.match(r"^https?://", src):
        try:
            return json.loads(Path(src).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise UpdateFailed("download", "-", f"The releases list could not be read ({e}).") from e
    cache = Path(data) / "setup" / "update" / "feed.json" if data else None
    kept = {}
    if cache and cache.is_file():
        try:
            kept = json.loads(cache.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            kept = {}
    req = urllib.request.Request(src, headers={"User-Agent": "des-setup", "Accept": "application/vnd.github+json",
                                               "X-GitHub-Api-Version": "2022-11-28"})
    if kept.get("etag") and kept.get("url") == src:
        req.add_header("If-None-Match", kept["etag"])
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = json.loads(r.read().decode("utf-8"))
            etag = r.headers.get("ETag")
    except urllib.error.HTTPError as e:
        if e.code == 304 and isinstance(kept.get("body"), list):
            return kept["body"]
        raise UpdateFailed("download", "-", f"GitHub did not answer the update check (HTTP {e.code}).") from e
    except (OSError, ValueError) as e:
        raise UpdateFailed("download", "-", "There is no connection to GitHub for the update check.") from e
    if not isinstance(body, list):
        raise UpdateFailed("download", "-", "GitHub's answer to the update check was not a list of releases.")
    if cache and etag:
        try:
            cache.parent.mkdir(parents=True, exist_ok=True)
            tmp = cache.with_name(cache.name + ".tmp")
            tmp.write_text(json.dumps({"url": src, "etag": etag, "body": body}), encoding="utf-8")
            os.replace(tmp, cache)
        except OSError:
            pass
    return body


def stable_releases(feed) -> list:
    """The feed's published stable releases that have this system's download and SHA256SUMS, newest first:
    (version tuple, version text, release)."""
    out = []
    for r in feed if isinstance(feed, list) else []:
        if not isinstance(r, dict) or r.get("draft") or r.get("prerelease"):
            continue
        v = version_of(r.get("tag_name"))
        if v is None:
            continue
        text = ".".join(map(str, v))
        names = {a.get("name") for a in r.get("assets") or [] if isinstance(a, dict)}
        if player_asset(text) in names and SUMS in names:
            out.append((v, text, r))
    return sorted(out, key=lambda x: x[0], reverse=True)


def newer(feed, app):
    """The newest stable release newer than `app` as (version text, release), or None (also for a version that is
    not a plain X.Y.Z: a developer's build)."""
    mine = version_of(app)
    if mine is None:
        return None
    for v, text, r in stable_releases(feed):
        return (text, r) if v > mine else None
    return None


def published(release) -> int:
    try:
        when = datetime.datetime.fromisoformat(str(release.get("published_at")).replace("Z", "+00:00"))
        return int(when.timestamp())
    except (TypeError, ValueError):
        return 0


def check(data, app, emit) -> int:
    emit.setup()
    if version_of(app) is None:
        emit.text(f"note {app or 'this build'} is not a release, so it is not compared with the releases")
        emit.done("ok")
        return 0
    try:
        found = newer(read_feed(data), app)
    except UpdateFailed as e:
        emit.error(e.code, e.step, e.message)
        emit.done("failed")
        return 1
    if found:
        text, r = found
        emit.release(text, published(r), r.get("html_url") or f"https://github.com/{REPO}/releases")
        lines = str(r.get("body") or "").replace("\r\n", "\n").split("\n")
        for line in lines[:NOTE_LINES]:
            emit.note(line[:NOTE_WIDTH])
        if len(lines) > NOTE_LINES:
            emit.note("...")
    else:
        emit.text(f"ok   {app} is the newest release")
    emit.done("ok")
    return 0


# ---- the swap ----

def _rename(src: Path, dst: Path, tries=40):
    """os.replace, tried again for a while on Windows' sharing errors (a virus scanner reading a new file)."""
    for k in range(tries):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if k == tries - 1:
                raise
            time.sleep(0.1)


def _clear_empty(path: Path):
    """Removes `path` if it is a folder of empty folders."""
    if not path.is_dir() or path.is_symlink():
        return
    for root, dirs, files in os.walk(path, topdown=False):
        if files:
            return
        try:
            os.rmdir(root)
        except OSError:
            return


class Swap:
    """Renames, each written to the journal before it is made, so they can be undone in reverse, with the folders
    they made. The journal is lines of tab-separated fields, "made <folder>" and "move <from> <to>", in the order they
    happened, so the host can undo a swap cut short without Python (port/ui/src/update.cpp, recover_swap): the
    program files may be half swapped then, the engine with them."""

    HEAD = "# A swap of des_setup's update (docs/105): undone from the last line up if it did not end."

    def __init__(self, path: Path):
        self.path = Path(path)
        self.steps = []                     # ("made", folder) or ("move", from, to)

    def _save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_name(self.path.name + ".tmp")
        lines = [self.HEAD] + ["\t".join(s) for s in self.steps]
        tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
        os.replace(tmp, self.path)

    def move(self, src: Path, dst: Path):
        if dst.exists():
            raise FileExistsError(f"{dst} is in the way")
        if any(c in str(p) for p in (src, dst) for c in "\t\n\r"):
            raise OSError(f"{src}: a name with a tab or a line break")
        top = dst.parent
        while not top.parent.exists() and top.parent != top:
            top = top.parent
        n = len(self.steps)
        if not top.exists():
            self.steps.append(("made", str(top)))
        self.steps.append(("move", str(src), str(dst)))
        self._save()
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            _rename(src, dst)
        except OSError:
            del self.steps[n:]
            _clear_empty(top)
            self._save()
            raise

    def undo(self):
        for s in reversed(self.steps):
            if s[0] == "made":
                _clear_empty(Path(s[1]))
                continue
            src, dst = Path(s[1]), Path(s[2])
            _clear_empty(src)               # (a folder a later move made, emptied by the moves undone before)
            if dst.exists() and not src.exists():
                _rename(dst, src)
        self.steps = []
        self.done()

    def done(self):
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass

    @classmethod
    def read(cls, path: Path):
        s = cls(path)
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            f = line.split("\t")
            if (f[0] == "made" and len(f) == 2) or (f[0] == "move" and len(f) == 3):
                s.steps.append(tuple(f))
        return s


def recover(install: Path, emit=None) -> bool:
    """A swap cut short (its journal is still there): undone. True if there was one."""
    path = Path(install) / UPDATE / SWAP
    if not path.is_file():
        return False
    try:
        s = Swap.read(path)
    except OSError:
        s = Swap(path)
    n = sum(1 for x in s.steps if x[0] == "move")
    s.undo()
    if emit:
        emit.text(f"ok   the swap that was cut short is undone ({n} renames)")
    return True


def listed(root: Path):
    """The names at the top of a release's folder that its release-files.txt lists, or None without one."""
    m = Path(root) / MANIFEST
    if not m.is_file():
        return None
    names = {line.strip().split("/", 1)[0] for line in m.read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.startswith("#")}
    return {n for n in names if n and n not in KEEP and n not in (".", "..")}


def program_names(root: Path) -> set:
    """The program files at the top of a new release's folder: its list's names, else all that is there."""
    names = listed(root)
    if names is None:
        names = {p.name for p in Path(root).iterdir() if p.name not in KEEP} if Path(root).is_dir() else set()
    return names


def _order(names, first_out):
    """The exe last out and first in: the install has none for the shortest time."""
    rest = sorted(n for n in names if n != EXE)
    return rest + [EXE] if first_out and EXE in names else ([EXE] if EXE in names else []) + rest


def swap_program(sw: Swap, install: Path, staged: Path, keep_as: Path):
    """install's program files into keep_as, then staged's in their place (journaled in sw). An install from before
    release-files.txt: the names the new release has (what else is there may be the player's)."""
    out = (listed(install) or set()) | program_names(staged)
    for n in _order(out, True):
        if (install / n).exists() or (install / n).is_symlink():
            sw.move(install / n, keep_as / n)
    for n in _order(program_names(staged), False):
        if (staged / n).exists():
            sw.move(staged / n, install / n)


def release_shaped(install: Path) -> bool:
    """A player's install (release-info.txt and the exe at its top), not a developer's tree (a git checkout)."""
    return (install / "release-info.txt").is_file() and (install / EXE).is_file() and not (install / ".git").exists()


def kept_version(install):
    """The version kept to go back to (its folder under .previous has the exe), or None."""
    p = Path(install) / PREVIOUS
    if not p.is_dir():
        return None
    for d in sorted(p.iterdir(), reverse=True):
        if d.is_dir() and version_of(d.name) and (d / EXE).is_file():
            return d.name
    return None


def _writable(folder: Path) -> bool:
    probe = folder / UPDATE / ".probe"
    try:
        probe.parent.mkdir(parents=True, exist_ok=True)
        probe.write_bytes(b"")
        probe.unlink()
        return True
    except OSError:
        return False


def _copy_build(data: Path, dest: Path):
    """The game build (game/ and setup/ready.txt) copied into dest, a new folder."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    if (data / "game").is_dir():
        shutil.copytree(data / "game", dest / "game")
    ready = data / "setup" / pipeline.READY
    if ready.is_file():
        shutil.copy2(ready, dest / pipeline.READY)


def _drop(path: Path):
    shutil.rmtree(path, ignore_errors=True)


def _drop_others(folder: Path, keep: str):
    """The folders under `folder` but `keep`: only one version is kept."""
    if folder.is_dir():
        for d in folder.iterdir():
            if d.name == keep:
                continue
            if d.is_dir():
                _drop(d)
            else:
                d.unlink()


# ---- update-apply ----

def _sums(url, allow_http, limit=1 << 16) -> dict:
    if not (url.startswith("https://") or (allow_http and url.startswith("http://127.0.0.1"))):
        raise UpdateFailed("download", "download", "The release's checksums are not on an https:// address.")
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "des-setup"}),
                                    timeout=60) as r:
            text = r.read(limit + 1)
    except OSError as e:
        raise UpdateFailed("download", "download", "The download did not finish. Check the connection, then "
                                                   "try again.") from e
    if len(text) > limit:
        raise UpdateFailed("download", "download", "The release's checksums file is too large.")
    out = {}
    for line in text.decode("utf-8", "replace").splitlines():
        w = line.split()
        if len(w) == 2 and re.fullmatch(r"[0-9a-f]{64}", w[0]):
            out[w[1].lstrip("*")] = w[0]
    return out


def _staged_root(unpacked: Path, version) -> Path:
    """The release's folder in the unpacked zip (DemonsSoulsRecomp-<version>/), checked."""
    root = unpacked / f"{NAME}-{version}"
    if not root.is_dir():
        dirs = [d for d in unpacked.iterdir() if d.is_dir()]
        root = dirs[0] if len(dirs) == 1 else root
    if not (root / EXE).is_file() or not (root / "tools" / "setup" / "run_setup.py").is_file():
        raise UpdateFailed("download", "install", "The download is not a release of the game (the program is "
                                                  "missing from it).")
    return root


def apply(install, data, app, to, emit, timeout=60) -> int:
    install, data = Path(install).resolve(), Path(data)
    held = pipeline.lock(data / "setup" / "run.lock")
    emit.setup()
    if held is None:
        emit.error("busy", "-", "Setup is running. Wait for it to end, then update.")
        emit.done("failed")
        return 1
    try:
        emit.plan(["download", "install"])
        return _apply(install, data, app, to, emit, timeout)
    except UpdateFailed as e:
        emit.error(e.code, e.step, e.message)
    except OSError as e:
        emit.text(f"FIX  {type(e).__name__}: {e}")
        if getattr(e, "errno", None) == 28:
            emit.error("disk", "install", "The disk is full.")
        else:
            emit.error("internal", "install", "The update stopped on an unexpected error. Nothing was changed.")
    finally:
        held.close()
    emit.done("failed")
    return 1


def _apply(install, data, app, to, emit, timeout) -> int:
    if not release_shaped(install):
        raise UpdateFailed("internal", "-", f"{install} is not a player's install of the game, so it is not updated.")
    if not _writable(install):
        raise UpdateFailed("internal", "-", f"Setup cannot write to the game's folder ({install}). Move the game "
                                            f"to a folder you can write to, such as Documents, then update.")
    recover(install, emit)
    found = [(v, t, r) for v, t, r in stable_releases(read_feed(data)) if t == ".".join(map(str, version_of(to) or ()))]
    if not found:
        raise UpdateFailed("download", "-", f"Version {to} is not among the releases.")
    _v, text, rel = found[0]
    assets = {a.get("name"): a for a in rel.get("assets") or [] if isinstance(a, dict)}
    zip_asset = assets[player_asset(text)]
    allow_http = bool(_test_feed())

    emit.step("download", "start")
    emit.overall(0.0)
    sums = _sums(assets[SUMS].get("browser_download_url", ""), allow_http)
    want = sums.get(zip_asset["name"])
    if not want:
        raise UpdateFailed("download", "download", "The release's checksums do not list its download.")
    entry = {"name": NAME, "version": text, "url": zip_asset.get("browser_download_url", ""),
             "size": int(zip_asset.get("size") or 0), "sha256": want}
    need = 3 * entry["size"] + pipeline.used_bytes(data / "game")
    for where in {install, data}:
        if pipeline.free_bytes(where) < need:
            raise UpdateFailed("disk", "download", f"The update needs about {need / pipeline.GB:.1f} GB free on "
                                                   f"{Path(where).drive or where}.")

    def prog(done, total):
        emit.progress("download", done, total)
        emit.overall(0.8 * done / max(1, total))

    try:
        archive = fetch.fetch(entry, data / "downloads", prog, allow_http=allow_http, timeout=timeout)
    except fetch.FetchError as e:
        emit.text(f"FIX  {e}")
        raise UpdateFailed("download", "download", "The download did not finish or did not match its checksum. "
                                                   "Try again.") from e
    emit.step("download", "done")

    emit.step("install", "start")
    emit.overall(0.8)
    unpacked = install / UPDATE / text
    try:
        fetch.unpack(archive, unpacked)
    except fetch.FetchError as e:
        raise UpdateFailed("download", "install", str(e)) from e
    staged = _staged_root(unpacked, text)
    emit.overall(0.9)
    kept = data / "previous" / (app + ".tmp")
    _copy_build(data, kept)
    sw = Swap(install / UPDATE / SWAP)
    try:
        if (install / PREVIOUS).exists():
            sw.move(install / PREVIOUS, install / (PREVIOUS + ".old"))
        swap_program(sw, install, staged, install / PREVIOUS / app)
    except OSError as e:
        sw.undo()
        _drop(kept)
        busy = isinstance(e, PermissionError)
        emit.text(f"FIX  {type(e).__name__}: {e}")
        raise UpdateFailed("busy" if busy else "internal", "install",
                           "A file in the game's folder is in use. Close the programs that use it, then try again."
                           if busy else "The new version could not be put in place. Nothing was changed.") from e
    sw.done()
    # Done: what the swap left behind goes, and the version before this one with it (one is kept).
    _drop(install / (PREVIOUS + ".old"))
    _drop(unpacked)
    _drop_others(data / "previous", kept.name)
    os.replace(kept, data / "previous" / app)
    try:
        archive.unlink()
    except OSError:
        pass
    emit.step("install", "done")
    emit.text(f"ok   version {text} is in place; {app} is kept to go back to")
    emit.overall(1.0)
    emit.done("ok")
    return 0


# ---- update-back ----

def back(install, data, app, emit) -> int:
    install, data = Path(install).resolve(), Path(data)
    held = pipeline.lock(data / "setup" / "run.lock")
    emit.setup()
    if held is None:
        emit.error("busy", "-", "Setup is running. Wait for it to end first.")
        emit.done("failed")
        return 1
    try:
        emit.plan(["install"])
        return _back(install, data, app, emit)
    except UpdateFailed as e:
        emit.error(e.code, e.step, e.message)
    except OSError as e:
        emit.text(f"FIX  {type(e).__name__}: {e}")
        emit.error("internal", "install", "Going back stopped on an unexpected error. Nothing was changed.")
    finally:
        held.close()
    emit.done("failed")
    return 1


def _back(install, data, app, emit) -> int:
    if not release_shaped(install):
        raise UpdateFailed("internal", "-", f"{install} is not a player's install of the game.")
    recover(install, emit)
    old = kept_version(install)
    if old is None:
        raise UpdateFailed("internal", "-", "There is no earlier version to go back to.")
    emit.step("install", "start")
    staged = install / UPDATE / old
    _drop(staged)
    build = data / "previous" / old
    here = data / "previous" / (app + ".tmp")
    _drop(here)
    sw = Swap(install / UPDATE / SWAP)
    try:
        sw.move(install / PREVIOUS / old, staged)
        # The game build: this version's aside, the kept one's in its place (ready.txt with it, which names it).
        here.mkdir(parents=True, exist_ok=True)
        if (data / "game").is_dir():
            sw.move(data / "game", here / "game")
        if (data / "setup" / pipeline.READY).is_file():
            sw.move(data / "setup" / pipeline.READY, here / pipeline.READY)
        if (build / "game").is_dir():
            sw.move(build / "game", data / "game")
        if (build / pipeline.READY).is_file():
            sw.move(build / pipeline.READY, data / "setup" / pipeline.READY)
        swap_program(sw, install, staged, install / PREVIOUS / app)
    except OSError as e:
        sw.undo()
        _drop(here)
        busy = isinstance(e, PermissionError)
        emit.text(f"FIX  {type(e).__name__}: {e}")
        raise UpdateFailed("busy" if busy else "internal", "install",
                           "A file in the game's folder is in use. Close the programs that use it, then try again."
                           if busy else "The earlier version could not be put back. Nothing was changed.") from e
    sw.done()
    # The journal's link made the module that is now kept aside: a later build links again rather than keep this one.
    journal.Journal(data / "setup" / "journal.json").forget("link")
    _drop(staged)
    _drop_others(data / "previous", here.name)
    os.replace(here, data / "previous" / app)
    emit.step("install", "done")
    emit.text(f"ok   version {old} is back in place; {app} is kept")
    emit.done("ok")
    return 0


def main_check(a) -> int:
    return check(Path(a.data), a.app, protocol.Emitter())


def main_apply(a) -> int:
    return apply(a.install, a.data, a.app, a.to, protocol.Emitter())


def main_back(a) -> int:
    return back(a.install, a.data, a.app, protocol.Emitter())


if __name__ == "__main__":
    sys.exit("run through des_setup: python -m des_setup update-check ...")
