"""The firmware's libsre.prx from another PS3 system software version (des_setup/firmware.py): set back to the table's
version, checked against the table's SHA-256, and staged as the table's bytes."""
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from des_setup import firmware, gamefiles, journal, pipeline  # noqa: E402

V486, V489, V492 = 0x00486000, 0x00489000, 0x00492000


def lis(r, hi):
    return (15 << 26) | (r << 21) | hi


def ori(r, lo):
    return (24 << 26) | (r << 21) | (r << 16) | lo


def elf(version, extra=b"", places=10):
    """A 64-bit big-endian ELF whose one loadable segment holds `places` version constants (lis, two other
    instructions, ori), some filler, and `extra` at its end."""
    code = b""
    for k in range(places):
        r = (5, 6, 8)[k % 3]        # (r4 and r7 are written between: not these)
        code += struct.pack(">4I", lis(r, version >> 16), 0x80828004, 0x38E0003D, ori(r, version & 0xFFFF))
        code += struct.pack(">2I", 0x60000000, 0x4E800020)          # nop, blr
    code += struct.pack(">I", ori(3, 0x9000))                          # an ori with no lis before it: not a place
    code += extra
    off = 0x100
    head = bytearray(off)
    head[:6] = b"\x7fELF\x02\x02"
    struct.pack_into(">Q", head, 0x20, 0x40)                          # e_phoff
    struct.pack_into(">H", head, 0x38, 1)                              # e_phnum
    struct.pack_into(">IIQQQQQQ", head, 0x40, 1, 5, off, 0, 0, len(code), len(code), 0x10)
    return bytes(head) + code


class VersionTest(unittest.TestCase):
    def test_places_and_version(self):
        data = elf(V489)
        self.assertEqual(len(firmware.places(data)), 10)
        self.assertEqual(firmware.version_of(data), V489)
        self.assertEqual(firmware.name(V489), "4.89")
        self.assertEqual(firmware.name(V492), "4.92")

    def test_set_back(self):
        self.assertEqual(firmware.as_version(elf(V489), V486), elf(V486))
        self.assertEqual(firmware.as_version(elf(V492), V486), elf(V486))   # the high half too
        self.assertEqual(firmware.as_version(elf(V486), V486), elf(V486))
        self.assertIsNone(firmware.version_of(elf(V489, places=3)))        # too few to be the version
        self.assertIsNone(firmware.version_of(b"not an elf"))

    def test_matched(self):
        good = elf(V486)
        entry = {"size": len(good), "sha256": hashlib.sha256(good).hexdigest(), "version": hex(V486)}
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "libsre.prx"
            p.write_bytes(elf(V489))
            fixed, own = firmware.matched(p, entry)
            self.assertEqual((fixed, own), (good, V489))
            p.write_bytes(elf(V489, extra=b"\0\0\0\1")[:len(good) - 4] + b"\0\0\0\1")    # another byte differs too
            self.assertIsNone(firmware.matched(p, entry))
            self.assertIsNone(firmware.matched(p, dict(entry, version=None)))


class CheckAndStageTest(unittest.TestCase):
    """The check takes the other version's file as good, and the stage writes the table's bytes."""

    def test_check_and_stage(self):
        good = elf(V486)
        entry = {"name": "libsre.prx", "size": len(good), "sha256": hashlib.sha256(good).hexdigest(),
                 "magic": "7f454c46", "from": "dev_flash/sys/external/libsre.sprx, decrypted", "version": hex(V486)}
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            p = d / "libsre.prx"
            p.write_bytes(elf(V489))
            item = gamefiles._check_file("firmware", entry, p, None, missing="missing", mismatch="mismatch")
            self.assertEqual((item.state, item.sha256, item.fix), ("ok", entry["sha256"], hex(V486)))
            self.assertIn("4.89", item.message)
            self.assertIn("4.86", item.message)
            # A decrypted game file is never changed this way.
            self.assertEqual(gamefiles._check_file("decrypted", dict(entry, path="x"), p, None, missing="missing",
                                                   mismatch="mismatch").state, "mismatch")
            # The stage writes the table's bytes, and leaves the player's file as it was.
            rep = gamefiles.Report(root=d, items=[item])
            data = d / "data"
            jr = journal.Journal(data / "setup" / "journal.json")
            inputs = pipeline.stage(rep, data, jr)
            self.assertEqual((inputs / "firmware" / "libsre.prx").read_bytes(), good)
            self.assertEqual(p.read_bytes(), elf(V489))


if __name__ == "__main__":
    unittest.main()
