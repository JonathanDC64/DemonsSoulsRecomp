#!/usr/bin/env python3
"""check_game_files.py: fails when game-derived files are in the repository.

Game-derived means made from a game disc or from the PS3 firmware: the lift (the C and C++ the lifters
write), the decrypted EBOOT, firmware modules (libsre, fw_spu), game assets, saves and gamedata, shader
caches and shader dumps built from the game's shaders, frame captures and screenshots, and Ghidra
output. None of it may be committed or shipped (docs/05; the plan is in docs/54).

Modes (one or more):
  --tree [REV]    every file in REV (default HEAD)
  --range A..B    every file each commit in A..B adds or changes. A file added and deleted again inside
                  a pull request is not in its last tree, but a merge would still publish it.
  --dir PATH      every file under PATH (a release folder before it is zipped)
  --self-test     checks the rules against made-up samples, so a green run cannot mean "checked nothing"

Exceptions: .github/game-files-allow.txt, one glob per line (matched against the path from the repository
or folder root, with "/"), each with a comment saying why the file is not game-derived.
Exit status: 0 nothing found, 1 something found, 2 usage or git error. Only the standard library is used.
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

MAX_BYTES = 2 * 1024 * 1024  # the largest file either repository tracks today is 1.2 MB (a font)

# A folder with any of these names, anywhere in the path.
DIR_NAMES = {
    "work": "the port's work/ folder: the decrypted EBOOT, the lift, the SPU images, libsre",
    "gamedata": "the game's PS3 file system (saves, /dev_hdd0)",
    "dev_hdd0": "the game's PS3 file system", "dev_hdd1": "the game's cache", "dev_flash": "PS3 firmware files",
    "ps3_game": "a game disc folder", "usrdir": "a game disc folder", "savedata": "game saves",
    "fw_spu": "lifted firmware SPU modules", "work_spu": "lifted SPU images of the game",
    "shader_cache": "shaders compiled from the game's own", "shots": "screenshots",
}

# File names (case does not matter; * and ? as in a shell).
NAME_GLOBS = [
    ("eboot.bin", "the game's executable"), ("eboot.elf", "the decrypted game executable"),
    ("param.sfo", "disc or save metadata"), ("param.pfd", "save metadata"),
    ("icon0.png", "the game's icon"), ("icon1.pam", "a game movie"), ("pic?.png", "game artwork"),
    ("snd0.at3", "game audio"), ("ps3updat.pup", "PS3 firmware"),
    ("libsre*.prx", "a firmware module"), ("libsre*.sprx", "a firmware module"),
    ("libsre_image.bin", "lifted firmware (libsre)"), ("libsre_glue.c", "firmware-derived glue (port/gen)"),
    ("taskset_policy_blob.c", "firmware bytes (port/gen)"), ("spu_workloads.cpp", "generated from the game's SPU images"),
    ("ppu_recomp_*.c*", "the PPU lift"), ("ppu_recomp.h", "the PPU lift's header"),
    ("spu_recomp.c", "an SPU lift"), ("spu_recomp.h", "an SPU lift's header"),
    ("*.functions.json", "the EBOOT's function list"), ("*.imports.json", "the EBOOT's import list"),
    ("eboot.*.json", "the EBOOT's layout (ppu_loader.py output)"), ("libsre_*.json", "libsre's layout (firmware)"),
    ("des_boot.map", "the game exe's map"), ("ghidra_*.txt", "Ghidra output"),
    ("vp_pick*.hlsl", "a dumped game shader"), ("fp_pick*.hlsl", "a dumped game shader"), ("duck_fp*.hlsl", "a dumped game shader"),
]

# File extensions (case does not matter).
EXTENSIONS = {
    # PS3 executables, modules, firmware, packages and keys
    ".elf": "an executable", ".self": "a PS3 executable", ".sprx": "a PS3 module", ".prx": "a PS3 module",
    ".pup": "PS3 firmware", ".pkg": "a PS3 package", ".rap": "a licence key", ".edat": "PS3 DRM data",
    ".sdat": "PS3 DRM data", ".sfo": "PS3 metadata", ".pfd": "save metadata", ".bin": "a raw binary",
    # Game assets (FromSoftware and PS3 formats)
    ".flver": "a game model", ".tpf": "game textures", ".hkx": "game physics", ".dcx": "a game archive",
    ".param": "game data", ".paramdef": "game data", ".esd": "game scripts", ".bnd": "a game archive",
    ".bhd": "a game archive", ".bdt": "a game archive", ".msb": "a game map", ".fsb": "game audio",
    ".at3": "game audio", ".pam": "a game movie", ".gtf": "a PS3 texture", ".dds": "a texture", ".tga": "an image",
    # Frame captures, screenshots and dumps
    ".rcap": "a frame capture", ".bmp": "a screenshot", ".png": "an image", ".jpg": "an image",
    ".jpeg": "an image", ".gif": "an image", ".webp": "an image", ".dmp": "a memory dump", ".mdmp": "a memory dump",
    # Shaders dumped from or compiled from the game
    ".hlsl": "a dumped shader", ".cso": "a compiled shader", ".dxbc": "a compiled shader", ".dxil": "a compiled shader",
    # Build output: the exe holds the compiled game
    ".exe": "a built program", ".dll": "a built library", ".pdb": "debug symbols", ".obj": "an object file",
    ".o": "an object file", ".lib": "a library", ".a": "a library", ".map": "a linker map", ".ilk": "a linker file",
    # Archives can hold any of the above
    ".zip": "an archive", ".7z": "an archive", ".rar": "an archive", ".gz": "an archive", ".xz": "an archive",
    ".zst": "an archive", ".tar": "an archive", ".iso": "a disc image",
}

# The first bytes of a file.
MAGIC = [
    (b"\x7fELF", "an ELF executable (EBOOT, SPU image, PRX)"),
    (b"SCE\x00", "a PS3 SELF or SPRX"),
    (b"SCEUF", "PS3 firmware (PUP)"),
    (b"\x00PSF", "PS3 metadata (PARAM.SFO)"),
    (b"\x89PNG\r\n\x1a\n", "a PNG image"),
    (b"DDS ", "a DDS texture"),
    (b"MZ", "a Windows executable"),  # binary files only, see check_bytes
]

# Headers the lifters and the firmware generators write on their first line.
HEADER = re.compile(rb"^\s*/\*\s*(Auto-generated by (ppu|spu)_lifter\.py|GENERATED by scripts/gen_\w+\.py)")
LIFTED = re.compile(rb"ctx->gpr\[")  # hand-written code has at most ~200; a lift chunk has hundreds of thousands
LIFTED_LIMIT = 1000
GHIDRA = re.compile(rb"\b(FUN|DAT|LAB)_[0-9a-fA-F]{6,}\b|\bundefined[1248]\b")
GHIDRA_LIMIT = 60


def load_allow(paths):
    """Globs from the exceptions files: one per line, comments after #."""
    globs = []
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                pattern = line.split("#", 1)[0].strip()
                if pattern:
                    globs.append(pattern)
    return globs


def allowed(path, allow):
    """True when a glob matches the path, or the path with leading folders removed (a copy of a repository
    inside a release folder: ps3recomp/runtime/... matches runtime/...)."""
    parts = path.split("/")
    subs = ["/".join(parts[i:]) for i in range(len(parts))]
    return any(fnmatch.fnmatchcase(sub, g) for g in allow for sub in subs)


def check_name(path):
    """The reason a path is game-derived by its name alone, or None."""
    parts = path.lower().split("/")
    for part in parts[:-1]:
        if part in DIR_NAMES:
            return f"in a {part}/ folder ({DIR_NAMES[part]})"
    name = parts[-1]
    for glob, why in NAME_GLOBS:
        if fnmatch.fnmatchcase(name, glob):
            return f"named like {glob} ({why})"
    ext = os.path.splitext(name)[1]
    if ext in EXTENSIONS:
        return f"a {ext} file ({EXTENSIONS[ext]})"
    return None


def check_bytes(data, size):
    """The reason the contents look game-derived, or None. `data` is the whole file unless it is too big."""
    if size > MAX_BYTES:
        return f"{size:,} bytes, over the {MAX_BYTES:,}-byte limit (lifts and game assets are large)"
    head = data[:4096]
    binary = b"\x00" in head
    for magic, why in MAGIC:
        if head.startswith(magic) and (magic != b"MZ" or binary):
            return f"starts like {why}"
    if HEADER.match(head):
        return "has a lifter or firmware-generator header on its first line"
    if not binary:
        n = len(LIFTED.findall(data))
        if n >= LIFTED_LIMIT:
            return f"looks like lifted code ({n:,} ctx->gpr[ uses)"
        n = len(GHIDRA.findall(data))
        if n >= GHIDRA_LIMIT:
            return f"looks like Ghidra output ({n:,} FUN_/DAT_/undefinedN names)"
    return None


def git(*args, data=None):
    r = subprocess.run(["git", *args], input=data, capture_output=True, check=False)
    if r.returncode != 0:
        sys.stderr.write(f"git {' '.join(args)}: {r.stderr.decode(errors='replace')}")
        sys.exit(2)
    return r.stdout


def read_blobs(entries):
    """entries: [(path, sha, size)]. Yields (path, size, data) with data empty for blobs over the limit."""
    small = [e for e in entries if e[2] <= MAX_BYTES]
    out = git("cat-file", "--batch", data="".join(sha + "\n" for _, sha, _ in small).encode()) if small else b""
    pos = 0
    for path, _, size in small:
        nl = out.index(b"\n", pos)
        pos = nl + 1
        yield path, size, out[pos:pos + size]
        pos += size + 1
    for path, _, size in (e for e in entries if e[2] > MAX_BYTES):
        yield path, size, b""


def tree_entries(rev):
    """[(path, sha, size)] for every blob in rev (submodules and symlinks skipped)."""
    entries = []
    for rec in git("ls-tree", "-r", "-z", "-l", rev).split(b"\x00"):
        if not rec:
            continue
        meta, path = rec.split(b"\t", 1)
        mode, kind, sha, size = meta.split()
        if kind == b"blob" and mode != b"120000":
            entries.append((path.decode("utf-8", "replace"), sha.decode(), int(size)))
    return entries


def range_entries(span):
    """[(path, sha, size)] for every file a non-merge commit in span adds or changes."""
    entries, seen = [], set()
    for commit in git("rev-list", "--no-merges", "--reverse", span).decode().split():
        recs = git("diff-tree", "-r", "-z", "--no-commit-id", "--root", "--no-renames", "--diff-filter=AMT", commit).split(b"\x00")
        for meta, path in zip(recs[0::2], recs[1::2]):
            fields = meta.split()
            if len(fields) < 5 or fields[1] in (b"120000", b"160000"):
                continue
            sha = fields[3].decode()
            key = (path, sha)
            if key not in seen:
                seen.add(key)
                entries.append((path.decode("utf-8", "replace"), sha, 0))
    if entries:
        sizes = git("cat-file", "--batch-check=%(objectsize)", data="".join(s + "\n" for _, s, _ in entries).encode()).split()
        entries = [(p, s, int(n)) for (p, s, _), n in zip(entries, sizes)]
    return entries


def dir_files(root):
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(base, name)
            size = os.path.getsize(full)
            data = b""
            if size <= MAX_BYTES:
                with open(full, "rb") as f:
                    data = f.read()
            yield os.path.relpath(full, root).replace(os.sep, "/"), size, data


def scan(files, allow, where):
    found = []
    for path, size, data in files:
        if allowed(path, allow):
            continue
        why = check_name(path) or check_bytes(data, size)
        if why:
            found.append((path, why))
    for path, why in found:
        print(f"{where}: {path}: {why}")
        if os.environ.get("GITHUB_ACTIONS") == "true" and where != "folder":
            print(f"::error file={path}::Game-derived file? {why} (docs/54; exceptions in .github/game-files-allow.txt)")
    return found


def self_test():
    lift = b"/* Auto-generated by ppu_lifter.py -- do not edit by hand. */\nvoid f(void) {}\n"
    glue = b"/* GENERATED by scripts/gen_libsre_exports.py -- do not edit, do not commit.\n"
    dense = b"void g(ppu_context* ctx) {\n" + b"  ctx->gpr[3] = ctx->gpr[4];\n" * 600 + b"}\n"
    ghidra = b"void FUN_00123456(void) {\n" + b"  undefined8 uVar1 = DAT_00abcdef;\n" * 40 + b"}\n"
    cases = [
        ("name", "work/game/EBOOT.ELF", b"x", True), ("name", "port/gen/taskset_policy_blob.c", b"x", True),
        ("name", "ps3recomp/work_spu/img/spu_recomp.c", b"x", True), ("name", "shots/capture_001.rcap", b"x", True),
        ("name", "gamedata/dev_hdd0/x.dat", b"x", True), ("name", "vp_pick.hlsl", b"x", True),
        ("name", "screenshot.PNG", b"x", True), ("name", "port/ui/src/app.cpp", b"int x;", False),
        ("bytes", "a.c", b"\x7fELF\x02\x02\x01", True), ("bytes", "a.c", b"SCE\x00\x00\x00\x00\x02", True),
        ("bytes", "a.c", lift, True), ("bytes", "a.c", glue, True), ("bytes", "a.c", dense, True),
        ("bytes", "a.c", ghidra, True), ("bytes", "a.c", b"MZ is a postcode area.\n", False),
        ("bytes", "a.c", b"MZ\x90\x00\x03\x00", True), ("bytes", "a.c", b"int main(void) { return 0; }\n", False),
        ("bytes", "big.cpp", b"", True),
    ]
    bad = 0
    for kind, path, data, want in cases:
        size = MAX_BYTES + 1 if path == "big.cpp" else len(data)
        got = (check_name(path) if kind == "name" else check_bytes(data, size)) is not None
        if got != want:
            bad += 1
            print(f"self-test: {kind} {path!r}: expected {'a finding' if want else 'nothing'}, got the opposite")
    print(f"self-test: {len(cases) - bad} of {len(cases)} cases right")
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tree", nargs="?", const="HEAD", help="check every file in this revision (default HEAD)")
    ap.add_argument("--range", dest="span", help="check every file the commits in A..B add or change")
    ap.add_argument("--dir", help="check every file under this folder")
    ap.add_argument("--allow", action="append", help="an exceptions file (repeatable; default "
                    ".github/game-files-allow.txt when it exists)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if not (a.tree or a.span or a.dir or a.self_test):
        ap.error("give --tree, --range, --dir or --self-test")
    ok = True
    if a.self_test:
        ok = self_test() and ok
    default = ".github/game-files-allow.txt"
    allow = load_allow(a.allow if a.allow else [default] if os.path.exists(default) else [])
    found = []
    if a.tree:
        entries = tree_entries(a.tree)
        found += scan(read_blobs(entries), allow, "tree")
        print(f"tree {a.tree}: {len(entries)} files checked")
    if a.span:
        entries = range_entries(a.span)
        found += scan(read_blobs(entries), allow, "commits")
        print(f"commits {a.span}: {len(entries)} added or changed files checked")
    if a.dir:
        files = list(dir_files(a.dir))
        found += scan(files, allow, "folder")
        print(f"folder {a.dir}: {len(files)} files checked")
    if found:
        print(f"{len(found)} file(s) look game-derived. Remove them (and from history, before merging), or, if one "
              "is ours, add it to .github/game-files-allow.txt with the reason.")
    return 0 if ok and not found else 1


if __name__ == "__main__":
    sys.exit(main())
