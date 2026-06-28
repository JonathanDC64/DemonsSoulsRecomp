# 03 — Environment Setup

Tracks prerequisites and procedures. Check items off as installed/verified.

## Current machine state (verified at project start)

| Tool | Status | Notes |
|---|---|---|
| Python | ✅ 3.13.14 | `C:\Users\jonat\AppData\Local\Programs\Python\Python313` |
| git | ✅ | mingw64 |
| Python tool deps | ✅ | pycryptodome/capstone/construct/tabulate/tqdm installed |
| CMake | ✅ 4.3.3 | `C:\Program Files\CMake\bin\cmake.exe` |
| MSVC + Windows SDK | ✅ | BuildTools 2022; MSVC **14.44.35207** (Hostx64/x64); Win SDK **10.0.26100.0** (has `d3d12.h`) |
| Ninja | ✅ 1.13.2 | winget pkg dir; alias `ninja` after shell restart |
| RPCS3 | ✅ | `D:\Users\jonat\Documents\roms\rpcs3` — used for decryption (M0 done) |

**Build environment note:** `cl.exe` needs the VS developer environment on PATH. Before building, run from a "x64 Native Tools" shell or source:
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
MSVC path: `...\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`

## 1. Python tool dependencies

```powershell
python -m pip install -r ps3recomp\requirements.txt
# pulls: pycryptodome, capstone, construct, tabulate, tqdm
```

## 2. C/C++ toolchain (for M1+)

- **Visual Studio 2022 Community** (or Build Tools) with:
  - "Desktop development with C++" workload
  - Windows 10/11 SDK (provides `d3d12.h`, `dxgi`, WASAPI, XInput)
- **CMake ≥ 3.20** and **Ninja** (VS bundles both; or install standalone).

Not needed for M0 (analysis is pure Python).

## 3. Getting a decrypted `EBOOT.ELF`  ← required before analysis

The disc's `EBOOT.BIN` is an **encrypted retail SELF** (magic `SCE\0`). ps3recomp ships **no decryption keys**, so its `elf_parser.py --decrypt` only helps for fSELF or if you supply keys. Two clean options:

### Option A (preferred) — RPCS3 decrypted dump
RPCS3 already has the public keys and decrypts on load. The user has RPCS3 working.
- Add the game folder to RPCS3, let it create its caches; the decrypted executable is produced during boot. Enable the executable-dump / decrypt option in RPCS3 (Debug settings) and retrieve the decrypted ELF.
- *(Exact RPCS3 menu path to confirm when we do this — RPCS3 versions differ.)*

### Option B — public keys + scetool
The PS3 `lv0`/`metldr` keys have been public since 2012/2013. With a keys file, **`scetool`** (external tool) decrypts the retail SELF to a PowerPC64 big-endian ELF. We supply the keys ourselves; they are **not** committed here.

> **Correction (verified in source):** `elf_parser.py` has **no `--decrypt` flag and no key/AES handling** — it only parses *already-decrypted* or fSELF ELFs, contrary to the porting guide's `elf_parser.py --decrypt` example. Decryption of a retail SELF must be done by RPCS3 (Option A) or scetool (Option B) first.

**Verify success:** the output should be `ELF64`, **big-endian**, machine **PowerPC** (e.g. `readelf -h` shows `EM_PPC64`, `MSB`).

## 4. Working directory (never committed)

All game-derived files live under `work/` and are gitignored:
```
work/
├── game/        # decrypted EBOOT.ELF, PARAM.SFO, any .sprx
├── analysis/    # elf_info.json, imports.json, functions.json, spu images
└── recomp/      # generated C/C++ (gigabytes)
```

## Legal note
Decryption is done on the user's own legally-owned copy, locally. Decrypted code/keys/assets are never distributed or committed. See [`05-legal.md`](05-legal.md).
