"""shader_extract.py <USRDIR/shader dir> <out dir>

Unpack the game's shader archives (*.shaderbnd: BND3, big-endian) into plain
Cg binaries (.vpo/.fpo) for ps3recomp's rsx_shader_precompile. Each entry is a
FromSoftware header (its size in the second word) followed by the
CgBinaryProgram the game uploads. The output is game data: keep it out of the
repository (precompile_shaders.sh writes it to a temporary directory).
"""
import os
import struct
import sys


def bnd3_entries(b):
    if b[:4] != b"BND3":
        raise ValueError("not a BND3 archive")
    count = struct.unpack(">I", b[0x10:0x14])[0]
    for i in range(count):
        o = 0x20 + i * 0x18
        _flags, size, off, _id, name_off, _usize = struct.unpack(">6I", b[o:o + 0x18])
        name = b[name_off:b.index(b"\0", name_off)].decode("latin-1")
        yield name, b[off:off + size]


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    n = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".shaderbnd"):
            continue
        with open(os.path.join(src, fn), "rb") as f:
            data = f.read()
        for i, (name, d) in enumerate(bnd3_entries(data)):
            if not name.lower().endswith((".vpo", ".fpo")) or len(d) < 8:
                continue
            hdr = struct.unpack(">I", d[4:8])[0]
            base = name.replace("\\", "/").rsplit("/", 1)[-1]
            with open(os.path.join(out, "%s_%04d_%s" % (fn.split(".")[0], i, base)), "wb") as f:
                f.write(d[hdr:])
            n += 1
    print("shader_extract: %d programs from %s" % (n, src))


if __name__ == "__main__":
    main()
