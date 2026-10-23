"""The firmware's libsre.prx from any PS3 system software version (docs/90 section 8).

libsre writes the system software's version into its code as a constant made by two instructions, `lis rX,hi` then
`ori rX,rX,lo`: 0x00486000 is 4.86, 0x00489000 is 4.89. libsre.prx has 86 such places. Apart from them, the file
decrypted from 4.89 is the same as the one from 4.86, byte for byte (measured 10-23).

The port is built and tested with 4.86's copy, the one game_table.json's sum names. So a copy from another version is
made into that one: the version is set back to the table's at each place, and then the whole file must match the
table's SHA-256. A copy that differs anywhere else still does not match, so this can never let a wrong file through."""
import hashlib
import struct
from collections import Counter
from pathlib import Path

LOOKBACK = 16      # instructions before an `ori` searched for its `lis`
MIN_PLACES = 8     # the most common version must appear at least this often to count as the version


def _words(data: bytes):
    """The ELF's first loadable segment (libsre's code): its file offset and size, or None for a file that is not a
    64-bit big-endian ELF."""
    if len(data) < 0x40 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        return None
    phoff, = struct.unpack_from(">Q", data, 0x20)
    phnum, = struct.unpack_from(">H", data, 0x38)
    for k in range(phnum):
        o = phoff + k * 56
        if o + 56 > len(data):
            return None
        ptype, _flags, off, _va, _pa, filesz = struct.unpack_from(">IIQQQQ", data, o)
        if ptype == 1 and off + filesz <= len(data):
            return off, filesz
    return None


def places(data: bytes):
    """The version's places: [(lis offset, ori offset, value)] for each `ori rX,rX,lo` whose `lis rX,hi` comes shortly
    before it and whose value looks like a version (4.00 to 4.99: 0x0040xxxx to 0x0049xxxx, with lo's last 12 bits
    zero)."""
    seg = _words(data)
    if seg is None:
        return []
    off, size = seg
    out = []
    for o in range(off, off + size - 3, 4):
        w, = struct.unpack_from(">I", data, o)
        if w >> 26 != 24:                                   # ori rA,rS,UIMM
            continue
        rs, ra, lo = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if rs != ra or lo & 0xFFF:
            continue
        for back in range(1, LOOKBACK + 1):
            p = o - 4 * back
            if p < off:
                break
            v, = struct.unpack_from(">I", data, p)
            if v >> 26 == 15 and (v >> 21) & 31 == ra and (v >> 16) & 31 == 0:     # lis rA,hi (addis rA,0,hi)
                hi = v & 0xFFFF
                if 0x0040 <= hi <= 0x0049:
                    out.append((p, o, (hi << 16) | lo))
                break
            if (v >> 21) & 31 == ra and v >> 26 not in (36, 37, 38, 44, 45, 62):     # rA written by something else
                break
    return out


def version_of(data: bytes):
    """The system software version the file was decrypted from (0x00489000 for 4.89), or None."""
    counts = Counter(v for _, _, v in places(data))
    if not counts:
        return None
    value, n = counts.most_common(1)[0]
    return value if n >= MIN_PLACES else None


def as_version(data: bytes, version: int) -> bytes:
    """`data` with its version set to `version` at every place that holds its own version."""
    own = version_of(data)
    if own is None or own == version:
        return data
    out = bytearray(data)
    for lis, ori, v in places(data):
        if v != own:
            continue
        w, = struct.unpack_from(">I", out, lis)
        struct.pack_into(">I", out, lis, (w & 0xFFFF0000) | (version >> 16))
        w, = struct.unpack_from(">I", out, ori)
        struct.pack_into(">I", out, ori, (w & 0xFFFF0000) | (version & 0xFFFF))
    return bytes(out)


def name(version) -> str:
    """0x00489000 as "4.89"."""
    if version is None:
        return "?"
    return f"{(version >> 20) & 0xF}.{(version >> 16) & 0xF}{(version >> 12) & 0xF}"


def matched(path: Path, entry: dict):
    """The table's bytes made from `path` when it is the table's file from another system software version: (bytes,
    its version), or None. entry: game_table.json's firmware entry, with "version" (the version its sum is for)."""
    want = entry.get("version")
    if not want or not entry.get("sha256"):
        return None
    data = Path(path).read_bytes()
    if entry.get("size") is not None and len(data) != entry["size"]:
        return None
    own = version_of(data)
    if own is None:
        return None
    fixed = as_version(data, int(want, 16))
    if hashlib.sha256(fixed).hexdigest() != entry["sha256"]:
        return None
    return fixed, own
