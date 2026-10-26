"""A stand-in for `rpcs3 --decrypt <file>...` in the unlock tests (test_unlock.py, docs/90 test T8). It works out its
folders as RPCS3 does (File.cpp: a `portable` folder beside it first, then RPCS3_CONFIG_DIR on Windows, cut after its
last separator, or the XDG folders on Linux), writes there what a start of RPCS3 writes (its lock RPCS3.buf, its
config, an empty TTY.log and a dev_hdd0 tree, but no RPCS3.log: measured, docs/90 2.3), then decrypts its inputs in
order and names the outputs as decrypt_binaries.cpp does (EBOOT.BIN -> EBOOT.elf, x.sprx -> x.prx, x.sdat ->
x.sdat.unedat). The bytes come from a fixture folder.

Its settings are fake.json beside it: "fixtures" (the folder of output bytes, by the input's file name), "calls" (a
file outside its folder, where each run adds a JSON line: its inputs, the environment it was given and what stdin
held), "root" (it never writes outside this folder: a run that would stops with exit status 2), "heartbeat" (the
file the hang mode's helper writes) and "mode", comma-separated:
  ok              every input decrypts
  pace=SECONDS    it waits that long after each output, so that a test sees each step
  short=NAME      NAME's output is half as long as it should be
  fail=NAME       NAME does not decrypt: the KLIC prompt, then the end of stdin ends the run, as on Linux (the files
                  after it are not done either)
  batchfail=NAME  the same, but only in a run of more than one file
  nowrite=NAME    "Failed to create" for NAME's output, and the run stops
  hang=NAME       at NAME it starts a helper that adds to the heartbeat file, and both wait for 20 s: the watchdog's case
  old             an RPCS3 without --decrypt: "Unknown option" on stderr, exit status 1
  abort           exit status 3 at once, as RPCS3's fatal errors
  ignore          ignores the redirect and writes in its own folder, as an old build would
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(HERE, "fake.json"), encoding="utf-8") as f:
    CFG = json.load(f)
MODES = dict((m.split("=", 1) + [""])[:2] for m in CFG.get("mode", "ok").split(",") if m)
HEARTBEAT = "import sys, time\nfor _ in range(400):\n    open(sys.argv[1], 'a').write('.')\n    time.sleep(0.05)\n"


def folders():
    """(config, cache), as RPCS3 works them out."""
    portable = os.path.join(HERE, "portable")
    if os.path.isdir(portable):
        config = portable
    elif "ignore" in MODES:
        return HERE, HERE
    elif os.name == "nt":
        v = os.environ.get("RPCS3_CONFIG_DIR", "").replace("\\", "/")
        config = v[:v.rfind("/") + 1] if "/" in v else HERE
    else:
        config = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "rpcs3")
    if os.name == "nt":
        return config, config
    cache = os.environ.get("XDG_CACHE_HOME") or os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.cache")
    return config, os.path.join(cache, "rpcs3")


def inside(path) -> bool:
    root = os.path.normcase(os.path.realpath(CFG["root"]))
    return os.path.normcase(os.path.realpath(path)).startswith(root + os.sep)


def put(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def start(config, cache):
    """What every start of RPCS3 writes."""
    put(os.path.join(cache, "RPCS3.buf"))
    cfg = os.path.join(config, "config") if os.name == "nt" else config
    if not os.path.isfile(os.path.join(cfg, "config.yml")):
        put(os.path.join(cfg, "config.yml"), b"Core: {}\n")
    put(os.path.join(os.path.join(config, "log") if os.name == "nt" else cache, "TTY.log"))
    put(os.path.join(config, "dev_hdd0", "home", "00000001", "localusername"), b"User")


def main():
    argv = sys.argv[1:]
    paths = [argv[i + 1] for i, a in enumerate(argv) if a == "--decrypt" and i + 1 < len(argv)]
    config, cache = folders()
    call = {"inputs": paths, "cwd": os.getcwd(), "stdin": sys.stdin.read() if sys.stdin else None,
            "env": {k: os.environ.get(k) for k in ("RPCS3_CONFIG_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                                    "QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES")},
            "outside": not (inside(config) and inside(cache))}
    with open(CFG["calls"], "a", encoding="utf-8") as f:
        f.write(json.dumps(call) + "\n")
    if "old" in MODES:
        sys.stderr.write("Unknown option 'decrypt'.\n")
        return 1
    if call["outside"]:
        return 2
    start(config, cache)
    if "abort" in MODES:
        return 3
    print("Decrypting binaries...", flush=True)
    batch = len(paths) > 1
    for p in paths:
        name = os.path.basename(p)
        if MODES.get("hang") == name:
            subprocess.Popen([sys.executable, "-I", "-c", HEARTBEAT, CFG["heartbeat"]], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(20)
            return 0
        with open(p, "rb") as f:
            magic = f.read(4)
        if magic == b"SCE\0":
            out = p[:p.rfind(".")] + (".prx" if name.lower().endswith(".sprx") else ".elf")
        elif magic == b"NPD\0":
            out = p + ".unedat"
        else:
            print(f'Failed to decrypt "{p}".', flush=True)
            break
        if MODES.get("fail") == name or (batch and MODES.get("batchfail") == name):
            print(f"Enter KLIC of {name}\nHexadecimal only, 32 characters:", flush=True)
            break                                   # (stdin is at its end: an empty KLIC ends the run)
        if MODES.get("nowrite") == name:
            print(f"Failed to create {out}", flush=True)
            break
        with open(os.path.join(CFG["fixtures"], name), "rb") as f:
            data = f.read()
        if MODES.get("short") == name:
            data = data[:len(data) // 2]
        put(out, data)
        print(f"Decrypted {p} -> {out}", flush=True)
        time.sleep(float(MODES.get("pace") or 0))
    else:
        print("Finished decrypting all binaries.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
