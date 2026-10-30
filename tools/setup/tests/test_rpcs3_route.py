"""Tests for the RPCS3 route's library and import (des_setup/rpcs3_library.py,
rpcs3_import.py and their parts of pipeline.py, docs/90 test T9), and the
firmware's unlock: a fixture RPCS3 around test_unlock's stand-in (games.yml,
vfs.yml, two users, a binary PSF save, a TROPUSR.DAT in RPCS3's layout, the
three YAML files, libsre.sprx). No game data, no RPCS3.

  cd tools/setup && python -I -m unittest discover -s tests
"""
import argparse
import contextlib
import io
import json
import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from des_setup import __main__ as cli, pipeline, protocol, rpcs3, rpcs3_import, rpcs3_library as lib  # noqa: E402
from des_setup import gamefiles, sfo, yaml_subset  # noqa: E402
from test_lift import FakeTree  # noqa: E402
from test_unlock import FAST, Tree, put, sha, snapshot  # noqa: E402

RUN_SETUP = Path(__file__).resolve().parents[1] / "run_setup.py"
T0 = 1792000000                 # the fixture save's times, Unix seconds
LIBSRE_SPRX = b"SCE\0" + b"f" * 44
LIBSRE_PRX = b"\x7fELF" + b"L" * 36


def tropusr(unlocked: dict, count=38) -> bytes:
    """A TROPUSR.DAT in RPCS3's own layout (TROPUSR.h, TROPUSRLoader::Generate): a 0x30-byte header, two table
    headers, table 4 (0x60-byte entries) and table 6 (0x70-byte entries). unlocked: {trophy id: Unix seconds}."""
    t4 = 0x30 + 2 * 0x20
    t6 = t4 + count * 0x60
    out = struct.pack(">IIII", lib.TROPUSR_MAGIC, 0x00010000, 2, 0) + b"\0" * 32
    out += struct.pack(">IIIIQQ", 4, 0x50, 1, count, t4, 0) + struct.pack(">IIIIQQ", 6, 0x60, 1, count, t6, 0)
    out += b"".join(struct.pack(">IIIIIII", 4, 0x50, i, 0, i, 4, 0xFFFFFFFF) + b"\0" * 68 for i in range(count))
    for i in range(count):
        tick = lib.TICK_UNIX + unlocked[i] * 1_000_000 if i in unlocked else 0
        out += struct.pack(">IIIIIIIIQQ", 6, 0x60, i, 0, i, int(i in unlocked), 0, 0, tick, tick) + b"\0" * 64
    return out


def psf_save(files: dict) -> bytes:
    """A save's binary PARAM.SFO, as RPCS3 writes one (cellSaveData.cpp): its keys, one "*<file>" key per file (1 for
    a secure file), and RPCS3_BLIST."""
    v = {"ACCOUNT_ID": "0000000000000000", "ATTRIBUTE": 1, "CATEGORY": "SD", "DETAIL": "line one\nline two",
         "PARAMS": "", "PARAMS2": "", "PARENTAL_LEVEL": 0, "SAVEDATA_DIRECTORY": lib.SAVE_DIR,
         "SAVEDATA_LIST_PARAM": "", "SUB_TITLE": "SAVE DATA", "TITLE": "Demon's Souls",
         "RPCS3_BLIST": "/".join(files) + "/"}
    for name in files:
        v["*" + name] = 0 if name.endswith(".PNG") else 1
    return sfo.build(v)


SAVE_FILES = {"USER.DAT": b"u" * 64, "1USER.DAT": b"1" * 64, "ICON0.PNG": b"\x89PNGicon", "PIC1.PNG": b"\x89PNGpic"}
CONFIG = """Core:
  Libraries Control:
    []
Video:
  Renderer: Vulkan
  Frame limit: Auto
  Second Frame Limit: 0
  Vblank Rate: 60
  Resolution Scale: 100
  Anisotropic Filter Override: 0
  VSync Mode: Full
  Vulkan:
    Adapter: "a GPU's name, never read"
System:
  Console PSID high: 1234
"""
CUSTOM = """Video:
  Frame limit: Infinite
  Second Frame Limit: 0
  Resolution Scale: 150
  Anisotropic Filter Override: 16
  VSync Mode: Disabled
Miscellaneous:
  Window Title Format: "FPS: %F | %R"
"""
PATCHES = """PPU-0123456789abcdef0123456789abcdef01234567:
  Unlock FPS:
    Demon's Souls:
      BLUS30443:
        01.00:
          Enabled: true
  Skip Intro Videos:
    Demon's Souls:
      BLUS30443:
        01.00:
          Enabled: true
  Disable Motion Blur:
    Demon's Souls:
      BLUS30443:
        01.00:
          Enabled: false
  "Enable FreeCam":
    "Demon's Souls":
      BLES00932:
        01.00:
          Enabled: true
"""


class Library:
    """The player's RPCS3 files, in the stand-in RPCS3's folder, where rpcs3_library looks for them."""

    def __init__(self, t: Tree):
        self.t = t
        self.exe = rpcs3.find(t.rpcs3)
        self.root = rpcs3.player_dirs(self.exe)["config"]
        self.conf = self.root / "config" if os.name == "nt" else self.root
        self.hdd0 = self.root / "dev_hdd0"

    def games(self, text):
        put(self.conf / "games.yml", text.encode("utf-8"))

    def user(self, uid, name):
        put(self.hdd0 / "home" / uid / "localusername", name.encode("utf-8"))

    def active(self, uid):
        put(self.root / "GuiConfigs" / "persistent_settings.dat",
            f"[LastPlayed]\nBLUS30443=2026-10-22T19:02:09\n\n[Users]\nactive_user={uid}\n".encode("utf-8"))

    def save_folder(self, uid) -> Path:
        return self.hdd0 / "home" / uid / "savedata" / lib.SAVE_DIR

    def save(self, uid, files=None):
        files = dict(SAVE_FILES if files is None else files)
        folder = self.save_folder(uid)
        for k, (name, data) in enumerate(files.items()):
            put(folder / name, data)
            os.utime(folder / name, (T0 + 100 * k, T0 + 100 * k))
        put(folder / "PARAM.SFO", psf_save(files))
        return folder

    def trophies(self, uid, unlocked, count=38):
        put(self.hdd0 / "home" / uid / "trophy" / lib.TROPHY_DIR / "TROPUSR.DAT", tropusr(unlocked, count))

    def settings(self, config=CONFIG, custom=CUSTOM, patches=PATCHES):
        put(self.conf / "config.yml", config.encode("utf-8"))
        put(self.conf / "custom_configs" / "config_BLUS30443.yml", custom.encode("utf-8"))
        put(self.conf / "patch_config.yml", patches.encode("utf-8"))

    def firmware(self, data=LIBSRE_SPRX):
        put(self.root / "dev_flash" / "sys" / "external" / "libsre.sprx", data)

    def standard(self):
        """The usual player: the game in the library, two users (the second active), a save and trophies for the
        second, settings and patches."""
        self.games(f"BLUS30027: C:/somewhere else/\nBLUS30443: {self.t.dump.as_posix()}/\n")
        self.user("00000001", "User")
        self.user("00000002", "Second")
        self.active("00000002")
        self.save("00000002")
        self.trophies("00000002", {0: T0 - 50, 5: T0 - 40, 37: T0 - 30})
        self.settings()


class Base(unittest.TestCase):
    ENV = ("RPCS3_CONFIG_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "PS3_SAVEDATA_ROOT", "PS3_TROPHY_DIR")

    def setUp(self):
        self._env = {k: os.environ.pop(k, None) for k in self.ENV}
        self.t = Tree()
        os.environ["XDG_CONFIG_HOME"] = str(self.t.base / "xdg-config")
        os.environ["XDG_CACHE_HOME"] = str(self.t.base / "xdg-cache")
        self.lib = Library(self.t)
        self.gamedata = self.t.base / "install" / "gamedata"
        self._free = pipeline.free_bytes
        pipeline.free_bytes = lambda p: 1 << 50
        self.src = None

    def tearDown(self):
        pipeline.free_bytes = self._free
        for k, v in self._env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        if self.src:
            self.src.close()
        self.t.close()

    def run_route(self, phases=("check",), library=True, **kw):
        """A run on the RPCS3 route: the game from the library (or the request's dump), with the stand-in RPCS3.
        phases None: the plan the run makes itself, as the host's runs (the whole build, with the import when one is
        pending)."""
        req = pipeline.make_request(None if library else self.t.dump, rpcs3_path=self.t.rpcs3)
        kw.setdefault("gamedata", self.gamedata)
        if phases is None or "lift" in phases:
            if self.src is None:
                self.src = FakeTree()
            kw.setdefault("src", self.src.src)
            kw.setdefault("compile_fn", fake_compile)
        out = io.StringIO()
        rc = pipeline.run(self.t.data, req, app="1.0", emit=protocol.Emitter(out), table=self.t.table,
                          manifest={"tools": []}, phases=list(phases) if phases else None, quiet=False,
                          unlock_kw=dict(FAST, launch=[sys.executable, "-I", str(self.t.rpcs3 / "fake_rpcs3.py")]),
                          from_library=library, **kw)
        return rc, out.getvalue().splitlines()

    def port_save(self) -> Path:
        return self.gamedata / "dev_hdd0" / "home" / "00000001" / "savedata" / lib.SAVE_DIR


def fake_compile(data, jr, jobs, progress):
    progress("compile", 1, 1)
    out = data / "game" / "des_game.dll"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"MZ module")
    return out


class YamlTest(unittest.TestCase):
    def test_subset(self):
        text = ("# a comment\nTop:\n  plain: a value # not this\n  'quoted key': 'it''s'\n  \"dq\": \"a\\tb \\\"c\\\" "
                "\\u00e9\"\n  empty:\n  flow:\n    []\n  list:\n    - one\n    - \"two: 2\"\n  inner:\n    deep: 1\n"
                "  after: yes\nkey at col 0: 01.00\nlist2:\n- a\n- b\nlast: z\n")
        self.assertEqual(yaml_subset.load(text), {
            "Top": {"plain": "a value", "quoted key": "it's", "dq": 'a\tb "c" \u00e9', "empty": "", "flow": "[]",
                    "list": ["one", "two: 2"], "inner": {"deep": "1"}, "after": "yes"},
            "key at col 0": "01.00", "list2": ["a", "b"], "last": "z"})

    def test_lines_it_cannot_read_are_skipped(self):
        self.assertEqual(yaml_subset.load("a: 1\n  stray text\n{flow: map}\nb: 2\n"), {"a": "1", "b": "2"})


class ReaderTest(Base):
    """rpcs3_library on the fixture, piece by piece."""

    def test_library_entries(self):
        d = self.t.dump.as_posix()
        for text, kind, path in ((f"{d}/", "folder", self.t.dump), (f"{d}/PS3_GAME/./", "folder", self.t.dump),
                                 (f"'{d}/PS3_GAME/.'", "folder", self.t.dump), (f'"{d}"', "folder", self.t.dump),
                                 ("D:/isos/Demon's Souls.iso", "iso", Path("D:/isos/Demon's Souls.iso"))):
            with self.subTest(text=text):
                self.lib.games(f"BLUS30443: {text}\n")
                got = lib.library(lib.folders(self.lib.exe), "BLUS30443")
                self.assertEqual((got.kind, got.path), (kind, path))
        self.lib.games("BLUS30027: C:/x/\n")
        self.assertEqual(lib.library(lib.folders(self.lib.exe), "BLUS30443").kind, "none")
        (self.lib.hdd0 / "game" / lib.STORE_ID).mkdir(parents=True)
        got = lib.library(lib.folders(self.lib.exe), "BLUS30443")
        self.assertEqual((got.kind, got.path), ("store", self.lib.hdd0 / "game" / lib.STORE_ID))

    def test_older_games_yml_place(self):
        put(self.lib.root / "games.yml", f"BLUS30443: {self.t.dump.as_posix()}/\n".encode())
        self.assertEqual(lib.library(lib.folders(self.lib.exe), "BLUS30443").path, self.t.dump)

    def test_vfs_moves_the_folders(self):
        elsewhere = self.t.base / "emu"
        put(self.lib.conf / "vfs.yml", f"$(EmulatorDir): \"{elsewhere.as_posix()}/\"\n/dev_hdd0/: "
                                       f"$(EmulatorDir)hdd/\n/dev_flash/: \"\"\n".encode())
        f = lib.folders(self.lib.exe)
        self.assertEqual((f.hdd0, f.flash), (elsewhere / "hdd", elsewhere / "dev_flash"))
        os.remove(self.lib.conf / "vfs.yml")
        f = lib.folders(self.lib.exe)
        self.assertEqual((f.hdd0, f.flash), (self.lib.root / "dev_hdd0", self.lib.root / "dev_flash"))

    def test_users_and_the_active_one(self):
        f = lib.folders(self.lib.exe)
        self.assertEqual(lib.active_user(f, lib.users(f)), "00000001")       # no users at all yet
        self.lib.user("00000002", "Second")
        self.lib.user("00000003", "x" * 20)
        put(self.lib.hdd0 / "home" / "00000004" / "no-name.txt", b"")         # no localusername: not a user
        put(self.lib.hdd0 / "home" / "notauser" / "localusername", b"no")
        us = lib.users(f)
        self.assertEqual([(u.id, u.name) for u in us], [("00000002", "Second"), ("00000003", "x" * 16)])
        self.assertEqual(lib.active_user(f, us), "00000002")    # no setting, no 00000001: the first user
        self.lib.user("00000001", "User")
        us = lib.users(f)
        self.assertEqual(lib.active_user(f, us), "00000001")    # the default
        self.lib.active("00000003")
        self.assertEqual(lib.active_user(f, us), "00000003")
        self.lib.active("00000009")                              # a user whose folder is gone
        self.assertEqual(lib.active_user(f, us), "00000001")

    def test_save_states(self):
        f = lib.folders(self.lib.exe)
        for uid in ("00000001", "00000002"):
            self.lib.user(uid, uid)
        us = lib.users(f)
        self.assertEqual(lib.save(f, "00000001", us).state, "none")
        self.lib.save("00000002")
        self.assertEqual(lib.save(f, "00000001", us).state, "other-user")
        s = lib.save(f, "00000002", us)
        self.assertEqual((s.state, s.saved, s.characters), ("ok", T0 + 100, None))   # the newest data file
        (self.lib.save_folder("00000002").parent / (".working_" + lib.SAVE_DIR)).mkdir()
        self.assertEqual(lib.save(f, "00000002", us).state, "busy")
        put(self.lib.save_folder("00000001") / "PARAM.SFO", b"not a PSF")
        self.assertEqual(lib.save(f, "00000001", us).state, "other-user")

    def test_port_save(self):
        self.assertEqual(lib.port_save(None).state, "none")
        self.assertEqual(lib.port_save(self.gamedata).state, "none")
        put(self.port_save() / "PARAM.SFO", b"TITLE=x\n")
        put(self.port_save() / "USER.DAT", b"o")
        os.utime(self.port_save() / "USER.DAT", (T0 - 5, T0 - 5))
        self.assertEqual((lib.port_save(self.gamedata).state, lib.port_save(self.gamedata).saved), ("ok", T0 - 5))
        os.environ["PS3_SAVEDATA_ROOT"] = "elsewhere/savedata"
        self.assertEqual(lib.port_save_folder(self.gamedata), self.gamedata.parent / "elsewhere" / "savedata" /
                         lib.SAVE_DIR)

    def test_tropusr(self):
        data = tropusr({1: T0, 30: T0 + 9, 2: 0})
        self.assertEqual(len(data), 0x70 + 38 * 0x60 + 38 * 0x70)    # 8016 bytes, as RPCS3's own for 38 trophies
        total, unlocked = lib.read_tropusr(data)
        self.assertEqual((total, unlocked), (38, {1: T0, 2: 0, 30: T0 + 9}))
        for bad in (b"", b"\0" * 0x40, data[:0x200]):
            with self.assertRaises(lib.TrophyError):
                lib.read_tropusr(bad)

    def test_settings(self):
        f = lib.folders(self.lib.exe)
        self.lib.settings()
        # The custom config over config.yml; Unlock FPS and Skip Intro on, Disable Motion Blur off; FreeCam is
        # another serial's.
        self.assertEqual(lib.settings(f, "BLUS30443", "01.00"),
                         {"DES_FPS": "60", "RSX_SCALE": "150", "DES_SKIP_INTRO": "1", "RSX_ANISO": "16"})
        self.assertEqual(lib.patches(f, "BLUS30443", "01.00"), {"unlock fps", "skip intro videos"})
        self.lib.settings(custom="", patches="")
        self.assertEqual(lib.settings(f, "BLUS30443", "01.00"), {})    # all the game's own (V-sync is never brought across)

    def test_frame_rate_rules(self):
        for video, want in (({}, "60"), ({"Frame limit": "30"}, "30"), ({"Frame limit": "Off"}, "60"),
                            ({"Frame limit": "Infinite", "Vblank Rate": "144"}, "144"),
                            ({"Frame limit": "Display", "Vblank Rate": "1000"}, "unlocked"),
                            ({"Frame limit": "120", "Vblank Rate": "240", "Second Frame Limit": "99.6"}, "100"),
                            ({"Frame limit": "50", "Vblank Rate": "240"}, "50"),
                            ({"Vblank Rate": "25"}, None), ({"Frame limit": "PS3 Native", "Vblank Rate": "x"}, "60")):
            with self.subTest(video=video):
                self.assertEqual(lib.frame_rate(video), want)
        self.assertEqual([lib.scale({"Resolution Scale": v}) for v in ("25", "300", "900", "x")],
                         ["50", "300", "100", "100"])
        self.assertEqual([lib.aniso({"Anisotropic Filter Override": v}) for v in ("0", "1", "3", "12", "16", "99")],
                         ["0", "1", "2", "8", "16", "16"])


class CheckLinesTest(Base):
    """The check's RPCS3 lines (docs/90 3.8) and the game from the library."""

    def test_found_lines(self):
        self.lib.standard()
        rc, lines = self.run_route(check_only=True)
        self.assertEqual(rc, 0, lines)
        want = ["@setup 2", "@plan check", "@overall 0/1000", "@step check start", f"@rpcs3 own {self.t.rpcs3}",
                "@user 00000002 2 Second",
                f"@library folder {self.t.dump}", f"@found save ok - {T0 + 100}", "@found current-save none -",
                "@found trophies ok 3/38", "@found settings ok 4", "@setting DES_FPS 60", "@setting RSX_SCALE 150",
                "@setting DES_SKIP_INTRO 1", "@setting RSX_ANISO 16"]
        self.assertEqual(lines[:len(want)], want)
        first_check = next(i for i, x in enumerate(lines) if x.startswith("@check "))
        self.assertLess(len(want), first_check)             # all of them before the @check lines
        self.assertEqual(lines[-1], "@done ok")
        self.assertEqual(pipeline.load_request(self.t.data)["game"], str(self.t.dump))

    def test_nothing_to_bring_across(self):
        """A save and a port save, no trophies, every setting the port's own (V-sync on)."""
        self.lib.games(f"BLUS30443: {self.t.dump.as_posix()}/PS3_GAME/./\n")
        self.lib.settings(config="Video:\n  VSync Mode: Full\n", custom="", patches="")
        self.lib.user("00000001", "User")
        self.lib.save("00000001")
        put(self.port_save() / "PARAM.SFO", b"TITLE=Demon's Souls\n")
        put(self.port_save() / "USER.DAT", b"o")
        os.utime(self.port_save() / "USER.DAT", (T0 + 7, T0 + 7))
        rc, lines = self.run_route(check_only=True)
        self.assertEqual(rc, 0, lines)
        for want in ("@user 00000001 1 User", f"@library folder {self.t.dump}", f"@found current-save ok {T0 + 7}",
                     "@found trophies none 0/0", "@found settings none 0"):
            self.assertIn(want, lines)
        self.assertFalse(any(x.startswith("@setting ") for x in lines))

    def test_other_user_and_busy(self):
        self.lib.standard()
        self.lib.active("00000001")
        rc, lines = self.run_route(check_only=True)
        self.assertIn("@found save other-user - -", lines)
        self.lib.active("00000002")
        (self.lib.save_folder("00000002").parent / (".backup_" + lib.SAVE_DIR)).mkdir()
        rc, lines = self.run_route(check_only=True)
        self.assertIn(f"@found save busy - {T0 + 100}", lines)

    def test_iso_store_and_not_in_library(self):
        self.lib.user("00000001", "User")
        cases = [("BLUS30443: D:/isos/DeS.iso\n", "@library iso D:/isos/DeS.iso", "Setup cannot use ISO files yet."),
                 ("BLUS30027: C:/x/\n", "@library none -", "Demon's Souls is not in RPCS3's library yet.")]
        for games, line, sentence in cases:
            with self.subTest(games=games):
                self.lib.games(games)
                rc, lines = self.run_route(check_only=True)
                self.assertEqual(rc, 1)
                self.assertIn(str(Path(line.split(" ", 2)[2])) if "iso" in line else "-", lines[-3])
                self.assertEqual(lines[-2:], [f"@error game check {sentence}", "@done fix"])
                self.assertFalse(any(x.startswith(("@found", "@check")) for x in lines))
        (self.lib.hdd0 / "game" / lib.STORE_ID).mkdir(parents=True)
        rc, lines = self.run_route(check_only=True)
        self.assertEqual(rc, 1)
        sentence = gamefiles.other_title(self.t.table, lib.STORE_ID)
        self.assertEqual(lines[-4:], [f"@library store {self.lib.hdd0 / 'game' / lib.STORE_ID}",
                                      f"@check wrong TITLE_ID {sentence}", f"@error game check {sentence}",
                                      "@done fix"])

    def test_game_given_keeps_its_folder(self):
        """With GAME (the player chose the folder), the library is reported but the folder is the one given."""
        self.lib.games("BLUS30443: D:/isos/DeS.iso\n")
        rc, lines = self.run_route(library=False, check_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertIn(f"@library iso {Path('D:/isos/DeS.iso')}", lines)
        self.assertEqual(lines[-1], "@done ok")

    def test_rpcs3_gone(self):
        rc, lines = self.t.run(phases=("check",), request=pipeline.make_request(None, rpcs3_path=self.t.base / "gone"),
                               from_library=True, check_only=True)
        self.assertEqual(rc, 1)
        self.assertEqual(lines[-3:], [f"@rpcs3 missing {self.t.base / 'gone'}",
                                      f"@error rpcs3 check RPCS3 is not in {self.t.base / 'gone'} any more.",
                                      "@done fix"])

    def test_check_command(self):
        """`check --rpcs3` without GAME: human lines, the game from the library, nothing written."""
        self.lib.standard()
        before = snapshot(self.t.rpcs3), snapshot(self.t.dump)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = cli.main(["check", "--rpcs3", str(self.t.rpcs3), "--gamedata", str(self.gamedata)])
        text = out.getvalue()
        self.assertEqual(rc, 1, text)        # the real table: this fake dump is not the US disc
        for want in ("RPCS3 user: Second (00000002); RPCS3 has 2 users.", f"in RPCS3's library: {self.t.dump}",
                     "Trophies: 3 of 38 unlocked.", "DES_FPS=60, RSX_SCALE=150", "The port's own save: none"):
            self.assertIn(want, text)
        self.assertEqual((snapshot(self.t.rpcs3), snapshot(self.t.dump)), before)


class ImportTest(Base):
    """The import phase (docs/90 3.5): the save, the trophies and the settings, once, at the end of the build."""

    def setUp(self):
        super().setUp()
        self.lib.standard()

    def build(self, imports=("save", "trophies", "settings"), **kw):
        return self.run_route(phases=None, imports=list(imports) if imports else None, **kw)

    def own_save(self, marker=b"the port's own"):
        put(self.port_save() / "PARAM.SFO", b"TITLE=Demon's Souls\nSUB_TITLE=SAVE DATA\nDETAIL=\nATTRIBUTE=1\n"
                                             b"LIST_PARAM=\n")
        put(self.port_save() / "USER.DAT", marker)

    def test_whole_build_with_an_import(self):
        self.own_save()
        trophy_json = self.gamedata / "trophies" / lib.TROPHY_FILE
        put(trophy_json, b'{\n  "trophies": [\n    { "id": 5, "timestamp": 100 },\n    { "id": 7, "timestamp": 0 }\n'
                         b'  ]\n}\n')
        before = snapshot(self.t.rpcs3), snapshot(self.t.dump)
        rc, lines = self.build()
        self.assertEqual(rc, 0, lines)
        self.assertIn("@plan check unlock fetch stage lift compile import shaders", lines)
        at = lines.index("@step import done")
        ready = next(i for i, x in enumerate(lines) if x.startswith("ok   the game is ready"))
        self.assertLess(lines.index("@step compile skip"), at)
        self.assertLess(at, ready)                           # ready.txt only once the import is done
        self.assertLess(ready, lines.index("@step shaders start"))
        self.assertIsNotNone(pipeline.read_ready(self.t.data))

        # The save: RPCS3's files raw, PARAM.SFO and PARAM.PFD in the port's text formats.
        dest = self.port_save()
        for name, data in SAVE_FILES.items():
            self.assertEqual((dest / name).read_bytes(), data)
        self.assertEqual((dest / "PARAM.SFO").read_bytes(), b"TITLE=Demon's Souls\nSUB_TITLE=SAVE DATA\n"
                                                            b"DETAIL=line one\nATTRIBUTE=1\nLIST_PARAM=\n")
        self.assertEqual((dest / "PARAM.PFD").read_bytes(), b"1USER.DAT\nUSER.DAT\n")
        self.assertEqual(sorted(p.name for p in dest.iterdir()), sorted(list(SAVE_FILES) + ["PARAM.SFO", "PARAM.PFD"]))
        backups = list((self.gamedata / "save-backups").iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / lib.SAVE_DIR / "USER.DAT").read_bytes(), b"the port's own")
        self.assertFalse((self.gamedata / "import").exists())       # (the staging folder goes too)

        # The trophies: a union, the earlier time wins; the old file kept aside.
        self.assertEqual(rpcs3_import.read_port_trophies(trophy_json.read_text(encoding="utf-8")),
                         {0: T0 - 50, 5: 100, 7: 0, 37: T0 - 30})
        self.assertIn(b'"id": 5, "timestamp": 100', (backups[0] / "trophies" / lib.TROPHY_FILE).read_bytes())

        # The settings, and what came across.
        self.assertEqual(sum(x.startswith("ok   the game is ready") for x in lines), 1)
        settings = (self.t.data / "setup" / rpcs3_import.SETTINGS_FILE).read_text(encoding="utf-8").splitlines()
        self.assertEqual(settings[1:], ["DES_FPS=60", "RSX_SCALE=150", "DES_SKIP_INTRO=1", "RSX_ANISO=16"])
        imported = dict(x.split("=", 1) for x in (self.t.data / "setup" / rpcs3_import.IMPORTED)
                        .read_text(encoding="utf-8").splitlines() if not x.startswith("#"))
        self.assertEqual(imported, {"format": "1", "rpcs3": str(self.t.rpcs3), "user": "00000002", "save": "1",
                                    "save_characters": "-", "save_saved": str(T0 + 100),
                                    "save_backup": str(backups[0] / lib.SAVE_DIR), "trophies": "1",
                                    "trophies_earned": "4", "trophies_total": "38", "trophies_added": "2",
                                    "trophies_backup": str(backups[0] / "trophies" / lib.TROPHY_FILE),
                                    "settings": "1", "settings_count": "4"})
        self.assertNotIn("import", pipeline.load_request(self.t.data))       # done: dropped
        # RPCS3's folder and the dump: byte for byte the same.
        self.assertEqual((snapshot(self.t.rpcs3), snapshot(self.t.dump)), before)

    def test_a_later_run_imports_nothing(self):
        self.assertEqual(self.build()[0], 0)
        put(self.port_save() / "USER.DAT", b"progress made in the port")
        rc, lines = self.run_route(phases=None)               # an Update or Repair: no --import
        self.assertEqual(rc, 0, lines)
        self.assertIn("@plan check unlock fetch stage lift compile shaders", lines)
        self.assertEqual((self.port_save() / "USER.DAT").read_bytes(), b"progress made in the port")
        self.assertFalse((self.gamedata / "save-backups").exists())       # (the first had nothing of the port's)
        self.assertIsNotNone(pipeline.read_ready(self.t.data))

    def test_settings_only_needs_no_gamedata(self):
        rc, lines = self.build(imports=["settings"], gamedata=None)
        self.assertEqual(rc, 0, lines)
        self.assertTrue((self.t.data / "setup" / rpcs3_import.SETTINGS_FILE).is_file())
        self.assertFalse(self.gamedata.exists())

    def test_busy_stops_and_changes_nothing(self):
        self.own_save()
        (self.lib.save_folder("00000002").parent / (".working_" + lib.SAVE_DIR)).mkdir()
        rc, lines = self.build()
        self.assertEqual(rc, 1)
        self.assertIn("@error import import-save RPCS3 is saving the game right now. Close the game in RPCS3.", lines)
        self.assertTrue(any(x.startswith("@log ") and x.endswith("import.log") for x in lines), lines)
        self.assertEqual(lines[-1], "@done failed")
        self.assertEqual((self.port_save() / "USER.DAT").read_bytes(), b"the port's own")
        self.assertIsNone(pipeline.read_ready(self.t.data))  # not Ready while the import is pending
        self.assertIn("import", pipeline.load_request(self.t.data))

    def test_rpcs3_saves_during_the_copy(self):
        self.own_save()
        real = rpcs3_import.stage

        def stage_while_rpcs3_saves(folder, vals, staging):
            n = real(folder, vals, staging)
            p = folder / "1USER.DAT"
            p.write_bytes(b"a newer save")
            os.utime(p, (T0 + 999, T0 + 999))
            return n

        rpcs3_import.stage = stage_while_rpcs3_saves
        try:
            rc, lines = self.build(imports=["save"])
        finally:
            rpcs3_import.stage = real
        self.assertEqual(rc, 1)
        self.assertIn("@error import import-save RPCS3 saved the game while setup was copying it.", lines)
        self.assertEqual((self.port_save() / "USER.DAT").read_bytes(), b"the port's own")
        self.assertFalse((self.gamedata / "import").exists())

    def test_try_again_carries_on_then_no_import(self):
        """A failed trophy import: the save step is kept; Try again (the same --import) skips it; a later failure's
        Go on without it (--no-import) drops the import, and ready.txt comes."""
        self.lib.trophies("00000002", {1: T0}, count=37)      # not this game's set: the trophy step fails
        rc, lines = self.build(imports=["save", "trophies"])
        self.assertEqual(rc, 1)
        self.assertIn("@error import import-trophies Bringing your trophies across failed.", lines)
        self.assertTrue((self.port_save() / "PARAM.PFD").is_file())     # the save came across
        self.lib.trophies("00000002", {1: T0})
        put(self.port_save() / "USER.DAT", b"changed after the save came across")
        rc, lines = self.build(imports=["save", "trophies"])
        self.assertEqual(rc, 0, lines)
        self.assertEqual((self.port_save() / "USER.DAT").read_bytes(), b"changed after the save came across")
        self.assertEqual(rpcs3_import.read_port_trophies((self.gamedata / "trophies" / lib.TROPHY_FILE)
                                                         .read_text(encoding="utf-8")), {1: T0})

        self.lib.trophies("00000002", {}, count=3)
        rc, lines = self.run_route(phases=None, imports=["trophies"])
        self.assertEqual(rc, 1)
        self.assertIsNone(pipeline.read_ready(self.t.data))
        rc, lines = self.run_route(phases=None, no_import=True)
        self.assertEqual(rc, 0, lines)
        self.assertNotIn("import", " ".join(x for x in lines if x.startswith("@plan")))
        self.assertNotIn("import", pipeline.load_request(self.t.data))
        self.assertIsNotNone(pipeline.read_ready(self.t.data))

    def test_a_new_import_is_not_the_old_one(self):
        """A second Import from RPCS3 later on brings the save again, though nothing changed in RPCS3."""
        self.assertEqual(self.build(imports=["save"])[0], 0)
        put(self.port_save() / "USER.DAT", b"played in the port")
        rc, lines = self.build(imports=["save"])
        self.assertEqual(rc, 0, lines)
        self.assertEqual((self.port_save() / "USER.DAT").read_bytes(), SAVE_FILES["USER.DAT"])
        self.assertEqual(len(list((self.gamedata / "save-backups").iterdir())), 1)   # the first had no save to keep
        backup = next((self.gamedata / "save-backups").iterdir())
        self.assertEqual((backup / lib.SAVE_DIR / "USER.DAT").read_bytes(), b"played in the port")

    def test_a_folder_a_stopped_swap_left_is_put_back(self):
        aside = self.gamedata / "import" / (lib.SAVE_DIR + ".old")
        put(aside / "PARAM.SFO", b"TITLE=x\n")
        put(aside / "USER.DAT", b"moved aside by a stopped run")
        rc, lines = self.build(imports=["save"])
        self.assertEqual(rc, 0, lines)
        backup = next((self.gamedata / "save-backups").iterdir())
        self.assertEqual((backup / lib.SAVE_DIR / "USER.DAT").read_bytes(), b"moved aside by a stopped run")
        self.assertFalse(aside.exists())

    def test_trophy_file_layout(self):
        p = self.t.base / "t" / lib.TROPHY_FILE
        rpcs3_import.write_port_trophies(p, {})
        self.assertEqual(p.read_bytes(), b'{\n  "trophies": [\n  ]\n}\n')
        rpcs3_import.write_port_trophies(p, {3: 30, 1: 10})
        self.assertEqual(p.read_bytes(), b'{\n  "trophies": [\n    { "id": 1, "timestamp": 10 },\n'
                                         b'    { "id": 3, "timestamp": 30 }\n  ]\n}\n')
        self.assertEqual(rpcs3_import.merge_trophies({1: 10, 2: 0, 4: 50}, {1: 5, 2: 7, 3: 0, 4: 60, 999: 1}),
                         {1: 5, 2: 7, 3: 0, 4: 50})
        self.assertIsNone(rpcs3_import.read_port_trophies("not a trophy file"))

    def test_command_line(self):
        """--import's items, and what it needs."""
        for argv, words in ((["run", "--import", "save,bogus"], "not one of save, trophies, settings"),
                            (["run", "--import", "save"], "--import needs --rpcs3"),
                            (["run", "--rpcs3", "x", "--import", "trophies"], "needs --gamedata"),
                            (["run", "--import", "save", "--no-import"], "not allowed with")):
            with self.subTest(argv=argv):
                err = io.StringIO()
                with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                    cli.main(argv)
                self.assertEqual(cm.exception.code, 2)
                self.assertIn(words, err.getvalue())
        self.assertEqual(cli.import_items("settings, save"), ["settings", "save"])
        a = argparse.Namespace(gamedata=None, host=str(self.t.base / "install" / "des.exe"), imports=["save"],
                               rpcs3="x")
        cli._check_run_args(None, a)
        self.assertEqual(a.gamedata, str(self.t.base / "install" / "gamedata"))    # beside the host


class CommandLineRunTest(Base):
    """`run --rpcs3 PATH` without GAME, as the host starts it (its own process: a run lowers its priority). It reads
    the real table, which this fake dump does not match, so the game it finds stops at the check."""

    def cli(self, *args):
        p = subprocess.run([sys.executable, "-I", str(RUN_SETUP), "run", "--rpcs3", str(self.t.rpcs3), "--data",
                            str(self.t.data), *args], capture_output=True, text=True, encoding="utf-8", timeout=120)
        return p.returncode, p.stdout.splitlines()

    def test_the_game_from_the_library(self):
        self.lib.user("00000001", "User")
        self.lib.games("BLUS30443: D:/isos/DeS.iso\n")
        rc, lines = self.cli("--check-only")
        self.assertEqual(rc, 1)
        self.assertEqual(lines[-3:], [f"@library iso {Path('D:/isos/DeS.iso')}",
                                      "@error game check Setup cannot use ISO files yet.", "@done fix"])
        self.lib.games(f"BLUS30443: {self.t.dump.as_posix()}/\n")
        for opt in ("--check-only", "--unlock-only"):
            with self.subTest(opt=opt):
                rc, lines = self.cli(opt, "--import", "settings")
                self.assertEqual(rc, 1)
                self.assertIn(f"@library folder {self.t.dump}", lines)
                self.assertIn("@found settings none 0", lines)    # (no config: all RPCS3's defaults, none brought across)
                self.assertIn("@check missing libsre.prx " + gamefiles.NO_FIRMWARE, lines)
                self.assertTrue(lines[-2].startswith("@error game check "), lines[-2])
                self.assertEqual(lines[-1], "@done fix")
        self.assertFalse(self.t.calls())                  # nothing unlocked: the check stopped the run


class FirmwareUnlockTest(Base):
    """libsre.prx on the RPCS3 route: RPCS3 unlocks a copy of its own libsre.sprx."""

    def setUp(self):
        super().setUp()
        self.lib.games(f"BLUS30443: {self.t.dump.as_posix()}/\n")
        self.lib.user("00000001", "User")
        self.t.table["firmware"] = [{"name": "libsre.prx", "size": len(LIBSRE_PRX), "sha256": sha(LIBSRE_PRX),
                                     "magic": "7f454c46", "from": "dev_flash/sys/external/libsre.sprx, decrypted"}]
        put(self.t.base / "fixtures" / "libsre.sprx", LIBSRE_PRX)

    def test_unlocked_with_the_game_files(self):
        self.lib.firmware()
        rc, lines = self.run_route(check_only=True)
        self.assertEqual(rc, 0, lines)
        self.assertIn("@check missing libsre.prx libsre.prx is missing: your RPCS3 unlocks it next.", lines)
        rc, lines = self.run_route(phases=("check", "unlock", "stage"))
        self.assertEqual(rc, 0, lines)
        self.assertIn("@progress unlock 4/4", lines)
        self.assertEqual([[Path(i).name for i in c["inputs"]] for c in self.t.calls()],
                         [["EBOOT.BIN", "m02.chresdbnd.dcx.sdat", "m01.luabnd.dcx.sdat", "libsre.sprx"]])
        self.assertEqual((self.t.data / "inputs" / "firmware" / "libsre.prx").read_bytes(), LIBSRE_PRX)
        self.assertFalse(self.t.work.exists())
        rc, lines = self.run_route(phases=("check", "unlock", "stage"))     # a rebuild: kept in inputs/
        self.assertEqual(rc, 0, lines)
        self.assertIn("@check ok libsre.prx libsre.prx matches.", lines)
        self.assertIn("@step unlock skip", lines)
        self.assertEqual(len(self.t.calls()), 1)

    def test_rpcs3_without_system_software(self):
        rc, lines = self.run_route(check_only=True)
        self.assertEqual(rc, 1)
        sentence = ("RPCS3 has no PS3 system software installed. Install it in RPCS3 (File, then Install Firmware), "
                    "then press Check again.")
        self.assertIn(f"@check missing libsre.prx {sentence}", lines)
        self.assertEqual(lines[-2:], [f"@error game check {sentence}", "@done fix"])

    def test_another_version_of_the_system_software(self):
        self.lib.firmware()
        put(self.t.base / "fixtures" / "libsre.sprx", b"\x7fELF" + b"O" * 36)
        rc, lines = self.run_route(phases=("check", "unlock"))
        self.assertEqual(rc, 1)
        self.assertIn("@error rpcs3 unlock libsre.prx came out different from the known good version. Your RPCS3's "
                      "PS3 system software may be another version; setup works with 4.93, the one Sony offers now. "
                      "Install it in RPCS3 (File, then Install Firmware), then try again.", lines)
        put(self.lib.root / "dev_flash" / "vsh" / "etc" / "version.txt",
            b"release:04.9000:\nbuild:00000,20230101:x\n")        # (made up beyond the first line)
        rc, lines = self.run_route(phases=("check", "unlock"))
        self.assertEqual(rc, 1)
        self.assertIn("@error rpcs3 unlock libsre.prx came out different from the known good version. Your RPCS3 "
                      "has PS3 system software 4.90; setup works with 4.93, the one Sony offers now. Install it in "
                      "RPCS3 (File, then Install Firmware), then try again.", lines)
        put(self.lib.root / "dev_flash" / "vsh" / "etc" / "version.txt", b"release:04.9300:\n")
        rc, lines = self.run_route(phases=("check", "unlock"))
        self.assertEqual(rc, 1)
        self.assertIn("@error rpcs3 unlock libsre.prx came out different from the known good version.", lines)

    def test_a_good_copy_beside_the_game_needs_no_unlock(self):
        self.lib.firmware()
        put(self.t.dump / "libsre.prx", LIBSRE_PRX)
        rc, lines = self.run_route(phases=("check", "unlock"))
        self.assertEqual(rc, 0, lines)
        self.assertNotIn("libsre.sprx", json.dumps(self.t.calls()))


class EmitterLinesTest(unittest.TestCase):
    def test_lines(self):
        out = io.StringIO()
        e = protocol.Emitter(out)
        e.user("00000002", 3, "Player One")
        e.user("00000001", 1, "")
        e.library("folder", r"C:\Games\RPCS3\games\Demon's Souls")
        e.library("none", None)
        e.found("save", "ok", None, 1792000100)
        e.found("save", "busy", None, None)
        e.found("current-save", "none", None)
        e.found("trophies", "ok", "14/38")
        e.found("settings", "ok", 2)
        e.setting("DES_FPS", "60")
        e.setting("RSX_SCALE", "150")
        e.error("import", "import-save", "RPCS3 saved the game while setup was copying it.")
        self.assertEqual(out.getvalue().splitlines(), [
            "@user 00000002 3 Player One", "@user 00000001 1 -", r"@library folder C:\Games\RPCS3\games\Demon's Souls",
            "@library none -", "@found save ok - 1792000100", "@found save busy - -", "@found current-save none -",
            "@found trophies ok 14/38", "@found settings ok 2", "@setting DES_FPS 60", "@setting RSX_SCALE 150",
            "@error import import-save RPCS3 saved the game while setup was copying it."])
        for bad in (lambda: e.found("save", "maybe", None, None), lambda: e.found("trophies", "ok"),
                    lambda: e.library("cloud", "-")):
            with self.assertRaises(AssertionError):
                bad()

    def test_phases(self):
        self.assertEqual(pipeline.default_phases(True, True),
                         ["check", "unlock", "fetch", "stage", "lift", "compile", "import", "shaders"])
        self.assertEqual(pipeline.default_phases(False, True), pipeline.default_phases())


if __name__ == "__main__":
    unittest.main()
