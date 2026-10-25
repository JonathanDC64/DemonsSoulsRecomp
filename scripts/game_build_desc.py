#!/usr/bin/env python3
"""Write the game DLL's build description for setup (tools/setup/des_setup/game_build.json).

Setup compiles des_game.dll on the player's PC with the commands CMake uses for
it (DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON, docs/57 and docs/84: no Visual C++
headers, Windows SDK or C runtime). This script reads them from such a build
directory (ninja's compile database, the link command and the import libraries'
commands), turns the paths into placeholders, and checks that the placeholders
expand back to CMake's commands:

  {cc} {ld}   the compiler and the linker (any launcher before the compiler is dropped)
  {src}       the source tree; the install folder mirrors it for these files
  {work}      the lift: setup's data folder work/ (recomp, spu/<image>, libsre, gen)
  {gen}       the files the build makes from the lift and the tables (CMake's split_gen)

The libs list is the linker's /lib commands that make the import libraries the
link reads (kernel32's, from ps3recomp's .def file), so the link needs no SDK.

The headers list is every file under {src} that an object of des_game read
(ninja's recorded dependencies), so the build must have compiled des_game once.
It is what the release has to ship for setup's compile.

  python scripts/game_build_desc.py BUILD_DIR [--ninja PATH] [--out FILE] [--check [--commands-only]]

--check compares with the file instead of writing it: exit 1 when they differ.
--commands-only leaves the headers out of the check, for a build that has only
been configured (CI, with placeholder lift files: ninja has recorded no headers).

A Linux build (clang, des_game.so, docs/87) gives game_build_linux.json, with
"system": "linux": clang's commands, the link by the clang driver (no import
libraries), and a check that no object read a header from outside the source
tree, the lift and the compiler's own (no libc headers: the player has none).
"""
import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTS = {"windows": os.path.join(ROOT, "tools", "setup", "des_setup", "game_build.json"),
        "linux": os.path.join(ROOT, "tools", "setup", "des_setup", "game_build_linux.json")}
DROP = re.compile(r"^(?:/Fo|/Fd|-clang:-MT|-clang:-MF)|^(?:-clang:-MD|-c|--)$")
ABS = re.compile(r"(?:^|[=:,])(?:[A-Za-z]:[\\/]|\\\\)|^/[a-z]+/")
# Linux: a path from the root, alone or after an option's letters (-I/usr/include) or an = : ,.
ABS_ELF = re.compile(r"(?:^|^-[A-Za-z][A-Za-z-]*|[=:,])/")
LINUX_CC = re.compile(r"^clang(?:\+\+)?(?:-\d+)?$")
ELF_DROP = {"-MD": 0, "-MT": 1, "-MF": 1, "-o": 1, "-c": 0}   # option -> how many words after it go too


def split_cmdline(s):
    """A Windows command line split as CommandLineToArgvW does."""
    out, cur, i, quoted, have = [], [], 0, False, False
    while i < len(s):
        c = s[i]
        if c == "\\":
            n = 0
            while i < len(s) and s[i] == "\\":
                n, i = n + 1, i + 1
            if i < len(s) and s[i] == '"':
                cur.append("\\" * (n // 2))
                if n % 2:
                    cur.append('"')
                    i += 1
            else:
                cur.append("\\" * n)
            have = True
            continue
        if c == '"':
            quoted, have, i = not quoted, True, i + 1
            continue
        if c in " \t" and not quoted:
            if have:
                out.append("".join(cur))
            cur, have, i = [], False, i + 1
            continue
        cur.append(c)
        have, i = True, i + 1
    if have:
        out.append("".join(cur))
    return out


def norm(p):
    return os.path.normcase(os.path.normpath(p))


class Roots:
    """CMake's folders and the placeholders they become, longest first."""
    def __init__(self, pairs):
        self.pairs = sorted(((os.path.normpath(r), ph) for r, ph in pairs), key=lambda x: -len(x[0]))
        self.rx = [(re.compile("".join(r"[\\/]" if ch in "\\/" else re.escape(ch) for ch in r) + r"(?=$|[\\/])",
                               re.I), ph, r) for r, ph in self.pairs]

    def to_ph(self, tok):
        for rx, ph, _ in self.rx:
            m = rx.search(tok)
            if m:
                return tok[:m.start()] + ph + tok[m.end():].replace("\\", "/")
        return tok

    def from_ph(self, tok):
        for r, ph in sorted(self.pairs, key=lambda x: -len(x[1])):
            if ph in tok:
                return tok.replace(ph, r)
        return tok


def cache_vars(build):
    v = {}
    with open(os.path.join(build, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^([A-Za-z_0-9]+):[A-Z]+=(.*)$", line.rstrip("\n"))
            if m:
                v[m.group(1)] = m.group(2)
    return v


def roots_for(build, v):
    port = v["CMAKE_HOME_DIRECTORY"]
    ps3r = v["PS3RECOMP_DIR"]
    return Roots([
        (os.path.join(port, ".."), "{src}"),
        (port, "{src}/port"),
        (ps3r, "{src}/ps3recomp"),
        (v["RECOMP_DIR"], "{work}/recomp"),
        (os.path.join(ps3r, "work_spu"), "{work}/spu"),
        (os.path.join(port, "..", "work", "libsre"), "{work}/libsre"),
        (os.path.join(port, "gen"), "{work}/gen"),       # the libsre glue (setup's lift writes it to work/gen)
        (os.path.join(build, "split_gen"), "{gen}"),
    ])


def kind_of(src):
    if re.fullmatch(r"\{work\}/recomp/ppu_recomp_\d+\.cpp", src):
        return "ppu", "{work}/recomp/ppu_recomp_*.cpp"
    if re.fullmatch(r"\{work\}/spu/[^/]+/spu_recomp\.c", src):
        return "spu", "{work}/spu/*/spu_recomp.c"
    if src == "{work}/libsre/lifted/ppu_recomp.c":
        return "libsre", None
    return os.path.splitext(src.rsplit("/", 1)[-1])[0], None


def system_of(v):
    """windows (clang-cl) or linux (clang), from the build's compiler."""
    return "windows" if os.path.basename(v.get("CMAKE_C_COMPILER", "")).lower().startswith("clang-cl") else "linux"


def template(cmd, roots):
    """(args, source, CMake's words less the dropped ones) of one compile command, args and source as placeholders."""
    toks = split_cmdline(cmd)
    k = next((i for i, t in enumerate(toks) if os.path.basename(t).lower() == "clang-cl.exe"), None)
    if k is None:
        raise SystemExit(f"no clang-cl in: {cmd[:200]}")
    toks = toks[k + 1:]
    if len(toks) < 2 or toks[-2] != "--":
        raise SystemExit(f"the source is not after '--' in: {cmd[:200]}")
    src = roots.to_ph(toks[-1])
    args = [roots.to_ph(t) for t in toks[:-1] if not DROP.search(t)]
    return args, src, [t for t in toks if not DROP.search(t)]


def template_elf(cmd, roots):
    """template for a Linux command, as ninja writes it: <clang> ... -MD -MT <obj> -MF <dep> -o <obj> -c <source>."""
    toks = shlex.split(cmd)
    k = next((i for i, t in enumerate(toks) if LINUX_CC.match(os.path.basename(t))), None)
    if k is None:
        raise SystemExit(f"no clang in: {cmd[:200]}")
    toks = toks[k + 1:]
    if len(toks) < 2 or toks[-2] != "-c":
        raise SystemExit(f"the source is not after '-c' in: {cmd[:200]}")
    keep, skip = [], 0
    for t in toks[:-2]:
        if skip:
            skip -= 1
        elif t in ELF_DROP:
            skip = ELF_DROP[t]
        else:
            keep.append(t)
    return [roots.to_ph(t) for t in keep], roots.to_ph(toks[-1]), keep + [toks[-1]]


def link_elf(cmd, build, roots):
    """(link arguments as placeholders, the module's name) of ninja's link line for des_game.so:
    : && <clang++> <flags> -shared ... -o des_game.so <objects> && :"""
    toks = shlex.split(cmd)
    k = next((i for i, t in enumerate(toks) if LINUX_CC.match(os.path.basename(t))), None)
    if k is None:
        raise SystemExit(f"no clang in the link command: {cmd[:200]}")
    end = next((i for i in range(k, len(toks)) if toks[i] == "&&"), len(toks))
    largs, have_objs, out, after_o = [], False, None, False
    for t in toks[k + 1:end]:
        if after_o:   # the module, named relative to the folder the link runs in
            out, after_o = t, False
            largs.append(t)
            continue
        if t.endswith(".o") and re.search(r"des_game\.dir", t):
            if not have_objs:
                largs.append("{objects}")
                have_objs = True
            continue
        after_o = t == "-o"
        largs.append(roots.to_ph(t))
    return largs, out


def elf_resource_dir(v):
    """The compiler's own header folder's parent (clang -print-resource-dir), to tell its headers from the system's."""
    cc = v.get("CMAKE_C_COMPILER", "clang")
    try:
        return os.path.normpath(subprocess.run([cc, "-print-resource-dir"], capture_output=True, text=True,
                                               check=True).stdout.strip())
    except (OSError, subprocess.CalledProcessError) as err:
        raise SystemExit(f"{cc} -print-resource-dir failed: {err}")


def lib_tokens(cmd):
    """The arguments of an import library's command (lld-link /lib), as ninja runs it:
    cmd /C "cd /D <build> && "<lld-link>" /lib ... /out:<lib>". None for any other command."""
    m = re.search(r'lld-link(?:\.exe)?"?\s+(/lib\s.*)$', cmd, re.I)
    if not m:
        return None
    rest = m.group(1)
    rest = rest[:rest.find(" && ")] if " && " in rest else rest.rstrip('"')
    return split_cmdline(rest)


def run(ninja, build, *args):
    return subprocess.run([ninja, "-C", build] + list(args), capture_output=True, text=True, check=True).stdout


def link_vars(build):
    """The variables of des_game's link in build.ninja (LINK_FLAGS, LINK_PATH, LINK_LIBRARIES, ...), unescaped."""
    out, inside = {}, False
    with open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("build "):
                if inside:
                    break
                inside = bool(re.search(r":\s*\w*SHARED_LIBRARY_LINKER__des_game_\s", line + " "))
            elif inside and line.startswith("  ") and " = " in line:
                name, value = line.strip().split(" = ", 1)
                out[name] = re.sub(r"\$([$ :])", r"\1", value)
            elif inside and line.strip():
                break
    return out


def _link_arg(t, build, roots):
    """One argument of the link, as a placeholder: a file named relative to the build directory (the import
    library: split_gen\\...) first becomes a full path."""
    if not t.startswith(("/", "-")) and not os.path.isabs(t):
        t = os.path.join(build, t)
    return roots.to_ph(t)


def describe(build, ninja, headers_too=True):
    v = cache_vars(build)
    if v.get("DES_SPLIT", "").upper() not in ("ON", "TRUE", "1"):
        raise SystemExit(f"{build} is not a DES_SPLIT=ON build")
    if v.get("DES_SPLIT_FREESTANDING", "").upper() not in ("ON", "TRUE", "1"):
        raise SystemExit(f"{build} is not a DES_SPLIT_FREESTANDING=ON build: setup builds the DLL with no "
                         "Visual C++ headers or Windows SDK (docs/84)")
    elf = system_of(v) == "linux"
    absolute = ABS_ELF if elf else ABS
    roots = roots_for(build, v)
    db = json.loads(run(ninja, build, "-t", "compdb"))
    db = [e for e in db if re.search(r"[\\/]des_game\.dir[\\/]", e["output"])]
    if not db:
        raise SystemExit("no des_game objects in the compile database")
    objects, seen, problems = [], {}, []
    for e in db:
        args, src, want = (template_elf if elf else template)(e["command"], roots)
        name, glob = kind_of(src)
        if name in seen:
            if seen[name]["args"] != args:
                problems.append(f"{src}: its command differs from the other {name} sources")
            continue
        ent = {"name": name}
        ent.update({"glob": glob} if glob else {"src": src})
        ent["args"] = args
        seen[name] = ent
        objects.append(ent)
        # Back to CMake's paths: the same command, apart from the dropped object and dependency names.
        back = [roots.from_ph(a) for a in args] + [roots.from_ph(src)]
        if [norm(t) for t in back] != [norm(t) for t in want]:
            problems.append(f"{src}: the placeholders do not expand back to CMake's command")
        for a in args + [src]:
            if absolute.search(a):
                problems.append(f"{src}: a path no placeholder covers: {a}")

    cmds = run(ninja, build, "-t", "commands", "des_game").splitlines()
    if elf:
        return _describe_elf(build, ninja, v, roots, db, objects, problems, cmds[-1], headers_too)
    link = cmds[-1]
    # Ninja runs it as cmd /C "cd . && cmake -E vs_link_dll ... -- <linker> <args> && cd .": the linker's part.
    i = link.find(" -- ", link.find("vs_link_dll"))
    if i >= 0:
        link = link[i + 4:]
        link = link[:link.find(" && ")] if " && " in link else link.rstrip('"')
    toks = split_cmdline(link)
    k = next((i for i, t in enumerate(toks) if os.path.basename(t).lower() == "lld-link.exe"), None)
    if k is None:
        raise SystemExit(f"no lld-link in the link command: {link[:200]}")
    end = next((i for i in range(k, len(toks)) if toks[i] in ("&&", "&")), len(toks))
    largs, have_objs, tail = [], False, []
    for t in toks[k + 1:end]:
        if t.lower().endswith(".obj") and re.search(r"des_game\.dir", t):
            if not have_objs:
                largs.append("{objects}")
                have_objs = True
            continue
        if t.startswith("@") and t.lower().endswith(".rsp"):
            # A long build path: CMake links through a response file, which ninja writes only while it links. It
            # holds the objects, then the link's LINK_PATH and LINK_LIBRARIES, which the plain command has last.
            if not have_objs:
                largs.append("{objects}")
                have_objs = True
            lv = link_vars(build)
            tail = split_cmdline(lv.get("LINK_PATH", "")) + split_cmdline(lv.get("LINK_LIBRARIES", ""))
            continue
        largs.append(_link_arg(t, build, roots))
    largs += [_link_arg(t, build, roots) for t in tail]
    for a in largs:
        if ABS.search(a):
            problems.append(f"link: a path no placeholder covers: {a}")
    out = next((a[5:] for a in largs if a.lower().startswith("/out:")), None)

    libs = []
    for c in cmds[:-1]:
        toks = lib_tokens(c)
        if toks is None:
            continue
        args = [roots.to_ph(t) for t in toks]
        lib = next((a[5:] for a in args if a.lower().startswith("/out:")), None)
        if not lib or lib not in largs:
            problems.append(f"lib: {lib or c[:200]}: the link does not read it")
        for a in args:
            if ABS.search(a):
                problems.append(f"lib: a path no placeholder covers: {a}")
        libs.append({"name": os.path.splitext((lib or "lib").rsplit("/", 1)[-1])[0], "args": args})

    headers = _headers(build, ninja, roots, db, headers_too, None, problems)
    abi = _abi(v)
    if problems:
        raise SystemExit("game_build_desc:\n  " + "\n  ".join(problems))
    srcs = {o.get("src") for o in objects}
    return {
        "about": "des_game.dll's build, as CMake does it with DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON. Written by "
                 "scripts/game_build_desc.py; read by tools/setup/des_setup/compile.py.",
        "version": 1,
        "abi": abi,
        "objects": objects,
        "libs": libs,
        "link": largs,
        "dll": out,
        "headers": sorted(h for h in headers if h not in srcs),
    }


def _headers(build, ninja, roots, db, headers_too, own, problems):
    """Every file under {src} an object read (ninja's recorded dependencies). own: on Linux, the compiler's resource
    folder; a file read from outside it, the source tree, the lift and {gen} is a problem (a libc header)."""
    headers = set()
    for e in (db if headers_too else []):
        dep = run(ninja, build, "-t", "deps", e["output"]).splitlines()
        if len(dep) < 2 or "#deps 0" in dep[0]:
            raise SystemExit(f"no recorded dependencies for {e['output']}: build des_game first")
        for d in dep[1:]:
            raw = d.strip()
            if not raw:
                continue
            # (A relative include keeps its '..' on Linux: the libsre glue's ../../ps3recomp/... from port/gen.)
            d = roots.to_ph(os.path.normpath(raw) if os.path.isabs(raw) else raw)
            if d.startswith("{src}/"):
                headers.add(d)
            elif own and not d.startswith(("{work}/", "{gen}/")) and not norm(os.path.join(build, raw)).startswith(own):
                problems.append(f"{e['output']}: reads {raw}, outside the source tree, the lift and the compiler's "
                                "own headers")
    return headers


def _abi(v):
    with open(os.path.join(v["PS3RECOMP_DIR"], "include", "ps3emu", "game_module.h"), encoding="utf-8") as f:
        m = re.search(r"^#define PS3_GAME_ABI (\d+)", f.read(), re.M)
        return int(m.group(1)) if m else None


def _describe_elf(build, ninja, v, roots, db, objects, problems, link, headers_too):
    """describe's Linux half: the link by the clang driver, no import libraries, the header check (module docstring)."""
    largs, out = link_elf(link, build, roots)
    for a in largs:
        if ABS_ELF.search(a):
            problems.append(f"link: a path no placeholder covers: {a}")
    if not out or "{objects}" not in largs:
        problems.append("link: no -o or no des_game objects in the link command")
    own = norm(elf_resource_dir(v)) + os.sep
    headers = _headers(build, ninja, roots, db, headers_too, own, problems)
    if problems:
        raise SystemExit("game_build_desc:\n  " + "\n  ".join(problems))
    srcs = {o.get("src") for o in objects}
    return {
        "about": "des_game.so's build, as CMake does it on Linux with DES_SPLIT=ON DES_SPLIT_FREESTANDING=ON "
                 "(docs/87). Written by scripts/game_build_desc.py; read by tools/setup/des_setup/compile.py.",
        "version": 1,
        "system": "linux",
        "abi": _abi(v),
        "objects": objects,
        "libs": [],
        "link": largs,
        "dll": out,
        "headers": sorted(h for h in headers if h not in srcs),
    }


def find_ninja(arg):
    for c in (arg, shutil.which("ninja")):
        if c and os.path.isfile(c):
            return c
    raise SystemExit("ninja not found: pass --ninja")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build", help="a CMake build directory configured with DES_SPLIT=ON, des_game built once")
    ap.add_argument("--ninja", help="ninja.exe (default: the one on PATH)")
    ap.add_argument("--out", help="default: tools/setup/des_setup/game_build.json, or game_build_linux.json for a "
                    "Linux build")
    ap.add_argument("--check", action="store_true", help="compare with --out instead of writing it")
    ap.add_argument("--commands-only", action="store_true", help="with --check: leave the headers out")
    a = ap.parse_args(argv)
    if a.commands_only and not a.check:
        ap.error("--commands-only goes with --check")
    d = describe(a.build, find_ninja(a.ninja), headers_too=not a.commands_only)
    a.out = a.out or OUTS[d.get("system", "windows")]
    text = json.dumps(d, indent=1) + "\n"
    if a.check:
        old = open(a.out, encoding="utf-8").read() if os.path.isfile(a.out) else ""
        if a.commands_only and old:
            o = json.loads(old)
            o["headers"] = []
            old = json.dumps(o, indent=1) + "\n"
        if old != text:
            print(f"{a.out} differs from the build's commands; run without --check to update it")
            return 1
        print(f"{a.out} matches the build")
        return 0
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"{a.out}: {len(d['objects'])} object kinds, {len(d['link'])} link arguments, {len(d['headers'])} headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
