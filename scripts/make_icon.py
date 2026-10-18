#!/usr/bin/env python3
"""make_icon.py <ICON0.PNG> <out.ico> -- the exe's icon from the game's own icon.

The disc's PS3_GAME/ICON0.PNG is 320x176. Windows icons are square, so the
picture is centred on a transparent square, then scaled (area average, with
premultiplied alpha) to 256, 64, 48, 32, 24 and 16 pixels. Each size is stored
as a PNG inside the .ico (Windows Vista and later read them). Only the Python
standard library is used, so the build needs no extra packages.

The icon is game-derived: it is written into the build directory and never
committed.
"""
import struct
import sys
import zlib

SIZES = (256, 64, 48, 32, 24, 16)


def read_png(path):
    """Returns (width, height, rows of RGBA bytes)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, palette, trns = 8, [], None, None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError(f'{path}: unsupported PNG (depth {depth}, colour type {ctype}, interlace {interlace})')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        rgba = bytearray(width * 4)
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 6:
                rgba[x * 4:x * 4 + 4] = px
            elif ctype == 2:
                rgba[x * 4:x * 4 + 4] = bytes(px) + b'\xff'
            elif ctype == 0:
                rgba[x * 4:x * 4 + 4] = bytes((px[0], px[0], px[0], 255))
            elif ctype == 4:
                rgba[x * 4:x * 4 + 4] = bytes((px[0], px[0], px[0], px[1]))
            else:
                k = px[0]
                alpha = trns[k] if trns is not None and k < len(trns) else 255
                rgba[x * 4:x * 4 + 4] = palette[k * 3:k * 3 + 3] + bytes((alpha,))
        rows.append(rgba)
    return width, height, rows


def square(width, height, rows):
    """Centres the picture on a transparent square."""
    side = max(width, height)
    left, top = (side - width) // 2, (side - height) // 2
    out = [bytearray(side * 4) for _ in range(side)]
    for y in range(height):
        out[top + y][left * 4:(left + width) * 4] = rows[y]
    return side, out


def spans(src, dst):
    """For each output index: the source indices it covers and their weights."""
    scale = src / dst
    result = []
    for o in range(dst):
        a, b = o * scale, (o + 1) * scale
        parts, i = [], int(a)
        while i < b and i < src:
            w = min(b, i + 1) - max(a, i)
            if w > 0:
                parts.append((i, w / scale))
            i += 1
        result.append(parts)
    return result


def scale(side, rows, size):
    """Area-average scale of a square RGBA image, with premultiplied alpha."""
    pm = []
    for row in rows:
        line = []
        for x in range(side):
            r, g, b, a = row[x * 4:x * 4 + 4]
            line.append((r * a, g * a, b * a, a))
        pm.append(line)
    sp = spans(side, size)
    horiz = []
    for line in pm:
        out = []
        for parts in sp:
            acc = [0.0, 0.0, 0.0, 0.0]
            for i, w in parts:
                px = line[i]
                for c in range(4):
                    acc[c] += px[c] * w
            out.append(acc)
        horiz.append(out)
    result = []
    for parts in sp:
        row = bytearray(size * 4)
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for i, w in parts:
                px = horiz[i][x]
                for c in range(4):
                    acc[c] += px[c] * w
            a = acc[3]
            if a > 0:
                row[x * 4:x * 4 + 4] = bytes((min(255, round(acc[0] / a)), min(255, round(acc[1] / a)),
                                              min(255, round(acc[2] / a)), min(255, round(a))))
        result.append(row)
    return result


def write_png(size, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.splitlines()[0])
    width, height, rows = read_png(sys.argv[1])
    side, sq = square(width, height, rows)
    images = [(s, write_png(s, scale(side, sq, s))) for s in SIZES]
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b'', b''
    for s, png in images:
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(png), offset + len(blobs))
        blobs += png
    with open(sys.argv[2], 'wb') as f:
        f.write(header + entries + blobs)
    print(f'make_icon: {sys.argv[2]} ({width}x{height} -> {", ".join(str(s) for s in SIZES)})')


if __name__ == '__main__':
    main()
