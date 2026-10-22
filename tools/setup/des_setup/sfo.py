"""Read a PS3 PARAM.SFO.

Layout (little-endian): a 20-byte header (magic "\\0PSF", version, key table
offset, data table offset, entry count), then one 16-byte index entry per key
(key offset, data format, data length, data maximum length, data offset).
Formats: 0x0004 and 0x0204 are UTF-8 strings (0x0204 is NUL-terminated),
0x0404 a 32-bit integer.
"""
import struct

MAGIC = b"\0PSF"
FMT_UTF8_SPECIAL = 0x0004
FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404


class SfoError(ValueError):
    pass


def parse(data: bytes) -> dict:
    """Return the SFO's keys and values (str or int). Raises SfoError."""
    if len(data) < 20 or data[:4] != MAGIC:
        raise SfoError("not a PARAM.SFO (bad magic)")
    _ver, key_off, data_off, count = struct.unpack_from("<IIII", data, 4)
    if 20 + count * 16 > len(data) or key_off > len(data) or data_off > len(data):
        raise SfoError("PARAM.SFO is cut short")
    out = {}
    for i in range(count):
        k_off, fmt, d_len, _d_max, d_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        k_start = key_off + k_off
        k_end = data.find(b"\0", k_start)
        if k_end < 0:
            raise SfoError("PARAM.SFO key table is cut short")
        key = data[k_start:k_end].decode("ascii", "replace")
        start = data_off + d_off
        raw = data[start:start + d_len]
        if len(raw) != d_len:
            raise SfoError(f"PARAM.SFO value of {key} is cut short")
        if fmt == FMT_INT32:
            out[key] = struct.unpack("<I", raw)[0] if d_len == 4 else int.from_bytes(raw, "little")
        else:
            out[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
    return out


def build(values: dict) -> bytes:
    """Make a PARAM.SFO from {key: str | int}: for tests, and the inverse of parse."""
    keys = sorted(values)
    key_table = b""
    key_offs = []
    for k in keys:
        key_offs.append(len(key_table))
        key_table += k.encode("ascii") + b"\0"
    while len(key_table) % 4:
        key_table += b"\0"
    data_table = b""
    entries = []
    for k, k_off in zip(keys, key_offs):
        v = values[k]
        if isinstance(v, int):
            raw, fmt = struct.pack("<I", v), FMT_INT32
            d_max = 4
        else:
            raw, fmt = v.encode("utf-8") + b"\0", FMT_UTF8
            d_max = (len(raw) + 3) & ~3
        entries.append(struct.pack("<HHIII", k_off, fmt, len(raw), d_max, len(data_table)))
        data_table += raw.ljust(d_max, b"\0")
    key_start = 20 + 16 * len(keys)
    data_start = key_start + len(key_table)
    header = MAGIC + struct.pack("<IIII", 0x0101, key_start, data_start, len(keys))
    return header + b"".join(entries) + key_table + data_table
