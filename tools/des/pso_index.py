#!/usr/bin/env python3
"""pso_index.py: the pipeline index (docs/97), one file per game version in port/data/pso_index/.

  pso_index.py merge INDEX SEEN...
      Adds the rows of each SEEN that INDEX lacks to the end of INDEX, in their order, and says how many.
      SEEN: a pso_index_seen.tsv the game wrote into its shader cache, or a folder to look for those in (every
      one under it, oldest first: a test package's shader_cache and the ones its play.sh moved to backups/).
      A SEEN file of another game version (its first line) is left out. INDEX is made if it is not there.
  pso_index.py check INDEX...
      Each row has its 14 fields in hex and is there once, and the first line names the game version the
      file is named for. Exit status 1 when one does not hold.
  pso_index.py stats INDEX...
      Rows, programs, and the rows in each form.

A row, tab-separated, in hex: vp vstart vfetch fp exp32 cube d2c clamp blend nrt rtfmt cmask cull depth. vp and
fp are fingerprints of the game's two shader programs (64-bit hashes of their code), the rest the draw state
the pipeline is made with; ps3recomp's libs/video/rsx_d3d12_backend.c ("The pipeline index") says what each
is. Nothing in a row is the game's. Standard library only.
"""
import os
import re
import sys

FIELDS = "vp vstart vfetch fp exp32 cube d2c clamp blend nrt rtfmt cmask cull depth".split()
HEADER = re.compile(r"^# pso_index 1 game=(\S+)\s*$")
HEX = re.compile(r"^[0-9a-f]+$")
SEEN_NAME = "pso_index_seen.tsv"
NOTE = [
    "# Pipelines the game asks for, found in play, and built ahead at a start (docs/97). Kept with",
    "# tools/des/pso_index.py; new rows go at the end. A row, tab-separated, in hex:",
    "#   " + " ".join(FIELDS),
    "# vp and fp are fingerprints of the game's two shader programs, the rest is draw state: nothing in a row",
    "# is the game's.",
]


class Bad(Exception):
    pass


def parse_row(line, where):
    """The row's fields as numbers; Bad if it is not a row."""
    parts = line.split("\t")
    if len(parts) != len(FIELDS):
        raise Bad(f"{where}: {len(parts)} fields, not {len(FIELDS)}")
    for name, p in zip(FIELDS, parts):
        if not HEX.match(p) or len(p) > (16 if name in ("vp", "fp") else 8):
            raise Bad(f"{where}: {name} is {p!r}, not hex")
    v = tuple(int(p, 16) for p in parts)
    row = dict(zip(FIELDS, v))
    for flag in ("vfetch", "exp32", "clamp"):
        if row[flag] > 1:
            raise Bad(f"{where}: {flag} is {row[flag]}")
    if row["nrt"] > 4 or row["cmask"] > 0xF:
        raise Bad(f"{where}: nrt {row['nrt']}, cmask {row['cmask']:x}")
    return v


def format_row(v):
    """As the game writes it (rsx_d3d12_backend.c, pso_index_note)."""
    return "\t".join(f"{x:016x}" if name in ("vp", "fp") else f"{x:x}" for name, x in zip(FIELDS, v))


def read(path):
    """(game, rows in order, problems) of an index or a seen file."""
    game, rows, bad, seen = None, [], [], set()
    with open(path, encoding="ascii", errors="replace", newline="") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                m = HEADER.match(line)
                if m and game is None:
                    game = m.group(1)
                continue
            try:
                v = parse_row(line, f"{path}:{n}")
            except Bad as e:
                bad.append(str(e))
                continue
            if v in seen:
                bad.append(f"{path}:{n}: the row is there before")
                continue
            seen.add(v)
            rows.append(v)
    return game, rows, bad


def seen_files(arg):
    if os.path.isfile(arg):
        return [arg]
    found = []
    for top, _dirs, files in os.walk(arg):
        if SEEN_NAME in files:
            found.append(os.path.join(top, SEEN_NAME))
    return sorted(found, key=os.path.getmtime)


def game_of(index_path):
    return os.path.splitext(os.path.basename(index_path))[0]


def merge(index, sources):
    game = game_of(index)
    if os.path.exists(index):
        have_game, rows, bad = read(index)
        if bad:
            raise Bad("; ".join(bad[:5]))
        if have_game != game:
            raise Bad(f"{index}: its first line says {have_game}, not {game}")
    else:
        rows = []
    have = set(rows)
    added = 0
    files = [f for s in sources for f in seen_files(s)]
    if not files:
        raise Bad(f"no {SEEN_NAME} in {' '.join(sources)}")
    for path in files:
        g, new, bad = read(path)
        for b in bad:
            print(f"  skipped {b}")
        if g != game:
            print(f"  left out {path}: game {g}")
            continue
        n = 0
        for v in new:
            if v not in have:
                have.add(v)
                rows.append(v)
                n += 1
        added += n
        print(f"  {n:5d} new of {len(new):5d} in {path}")
    if not added and not os.path.exists(index):
        print(f"{index}: no rows for {game}, not made")
        return
    os.makedirs(os.path.dirname(os.path.abspath(index)), exist_ok=True)
    with open(index, "w", encoding="ascii", newline="\n") as f:
        f.write(f"# pso_index 1 game={game}\n")
        for line in NOTE:
            f.write(line + "\n")
        for v in rows:
            f.write(format_row(v) + "\n")
    print(f"{index}: {added} rows added, {len(rows)} in all")


def check(paths):
    ok = True
    for path in paths:
        game, rows, bad = read(path)
        if game != game_of(path):
            bad.insert(0, f"{path}: the first line says game={game}, the file is named for {game_of(path)}")
        for b in bad:
            print(b)
        print(f"{path}: {len(rows)} rows, {'ok' if not bad else f'{len(bad)} problems'}")
        ok = ok and not bad
    return ok


def stats(paths):
    for path in paths:
        game, rows, _bad = read(path)
        col = {name: [r[i] for r in rows] for i, name in enumerate(FIELDS)}
        print(f"{path} ({game}): {len(rows)} rows")
        print(f"  programs: {len(set(col['vp']))} vertex, {len(set(col['fp']))} fragment, "
              f"{len(set(zip(col['vp'], col['fp'])))} pairs")
        print(f"  forms: {sum(col['vfetch'])} fetch, {sum(col['clamp'])} clamp, "
              f"{sum(1 for c, d in zip(col['cube'], col['d2c']) if c or d)} with cube, shadow or d2c units, "
              f"{sum(1 for x in col['exp32'] if not x)} half-precision exports")
        print(f"  draw states: {len(set(r[8:] for r in rows))}")


def main(argv):
    if len(argv) >= 3 and argv[1] == "merge":
        merge(argv[2], argv[3:])
        return 0
    if len(argv) >= 3 and argv[1] == "check":
        return 0 if check(argv[2:]) else 1
    if len(argv) >= 3 and argv[1] == "stats":
        stats(argv[2:])
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Bad as e:
        sys.stderr.write(f"pso_index.py: {e}\n")
        sys.exit(1)
