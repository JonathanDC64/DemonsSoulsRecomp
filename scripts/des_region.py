"""The two discs the port supports, and the European disc's addresses, for the lift's scripts and setup.

The port's guest addresses are the US disc's (BLUS30443). port/regions/BLES00932.inc gives the European
disc's (BLES00932) address for each, as DES_REGION_ADDR(0xUS, 0xEU) lines; the port's C++ reads it through
port/des_region.h. To lift the European EBOOT, the lifter gets:

  - the instruction hooks at their European addresses, each called with its US address as its key
    (ppu_lifter.py's ADDR=SYMBOL@KEY), so the port's handlers compare the addresses they always did;
  - the function overrides at their European addresses;
  - the boot patch on the European name of its function.

Addresses only: nothing here comes from the game's bytes.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "port" / "regions" / "BLES00932.inc"

# TITLE_ID -> the region's short name, its save folder and its trophy set (the game's own, from its EBOOT).
TITLES = {
    "BLUS30443": {"region": "US", "save_dir": "BLUS30443DEMONSS005", "trophy": "NPWR00881"},
    "BLES00932": {"region": "EU", "save_dir": "BLES00932DEMONSS005", "trophy": "NPWR01249"},
}
US, EU = "BLUS30443", "BLES00932"

_ROW = re.compile(r"^\s*DES_REGION_ADDR\(\s*0x([0-9A-Fa-f]{1,8})\s*,\s*0x([0-9A-Fa-f]{1,8})\s*\)")


class RegionError(Exception):
    pass


def load_table(path=TABLE) -> dict:
    """{US address: European address} from the .inc file; sorted, one line per address."""
    rows, last = {}, -1
    for n, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        m = _ROW.match(line)
        if not m:
            continue
        us, eu = int(m.group(1), 16), int(m.group(2), 16)
        if us <= last:
            raise RegionError(f"{path}:{n}: 0x{us:08X} is not after 0x{last:08X} (sorted, one line each)")
        rows[us], last = eu, us
    return rows


def eu_of(table: dict, us: int) -> int:
    if us not in table:
        raise RegionError(f"0x{us:08X} is not in {TABLE.name}: add its European address "
                          f"(tools/region/region_table.py find)")
    return table[us]


def insn_hooks(text: str, table: dict) -> str:
    """The instruction hook list (port/insn_hooks.txt) for the European EBOOT: each ADDR=SYMBOL line becomes
    EU=SYMBOL@US. Comments and blank lines are kept."""
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        body, sep, comment = line.partition("#")
        s = body.strip()
        if not s:
            out.append(line)
            continue
        addr, eq, sym = s.partition("=")
        if not eq or "@" in sym:
            raise RegionError(f"insn_hooks line {n}: {s!r} is not ADDR=SYMBOL")
        us = int(addr, 0)
        out.append(f"0x{eu_of(table, us):08X}={sym.strip()}@0x{us:08X}" + (f"   #{comment}" if sep else ""))
    return "\n".join(out)


def override_funcs(items: list, table: dict) -> list:
    """lift_ppu.json's override_funcs (ADDR=SYMBOL) at the European addresses."""
    out = []
    for item in items:
        addr, _, sym = item.partition("=")
        out.append(f"0x{eu_of(table, int(addr, 0)):X}={sym}")
    return out


def func_name(table: dict, us: int, title: str) -> str:
    """The lift's name of the function at the US address `us` on the disc `title`."""
    return f"func_{(eu_of(table, us) if title == EU else us):08X}"
