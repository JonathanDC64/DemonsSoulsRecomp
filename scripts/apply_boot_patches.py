#!/usr/bin/env python3
"""Re-apply boot-critical source patches to the (gitignored) lifted output.

The lifter regenerates work/recomp/ppu_recomp_*.cpp from scratch on every run, so
hand fixes don't survive a re-lift. This script re-injects the small, well-understood
fixes that get Demon's Souls past its C++ static-init memory bootstrap. Run it after
every `ppu_lifter.py` invocation, before building.

Fixes (both in chunk 005):

1) func_00AC8078 (the DL segregated-allocator bin search): when called with a NULL
   heap descriptor (r3==0) it would loop forever in its size-class doubling loop
   (r9<<=1 from 0). The game's heap managers are bootstrapped lazily and their pools
   are transiently NULL during static init, so route those allocations to our backed
   ds_heap instead of spinning. (r5 = requested size.)  Clears boot blockers #4/#5.

2) func_00B0AF28 (a Meyers-style lazy singleton accessor for the heap-manager object
   X=0x01B92CF8): it returns X without constructing it, so the init runner
   func_00A35780 dispatches a virtual method on a NULL vtable. Lazily build X via its
   real ctor func_00B25CB8 (on a context copy so only guest memory is built, not the
   accessor's own register state) when X's vtable is still 0.

Idempotent: running twice is a no-op. See docs/07-m2-boot-debug.md.
"""
import sys, pathlib

CHUNK = pathlib.Path(__file__).resolve().parent.parent / "work" / "recomp" / "ppu_recomp_005.cpp"

# (description, anchor_that_must_exist_once, old, new)
PATCHES = [
    (
        "func_00AC8078 NULL-heap -> ds_heap",
        "void func_00AC8078(ppu_context* ctx) {",
        "void func_00AC8078(ppu_context* ctx) {\n        uint64_t _cs_24 = ctx->gpr[24];",
        "void func_00AC8078(ppu_context* ctx) {\n"
        "        if ((uint32_t)ctx->gpr[3] == 0) { /* boot-patch: NULL heap pool -> ds_heap */\n"
        "            uint32_t _sz = (uint32_t)ctx->gpr[5]; if (_sz < 16) _sz = 16;\n"
        "            ctx->gpr[3] = _sz; ds_malloc(ctx); return; }\n"
        "        uint64_t _cs_24 = ctx->gpr[24];",
    ),
    (
        "func_00B0AF28 lazy-build singleton X",
        "void func_00B0AF28(ppu_context* ctx) {",
        "void func_00B0AF28(ppu_context* ctx) {\n        uint64_t _cs_27 = ctx->gpr[27];",
        "void func_00B0AF28(ppu_context* ctx) {\n"
        "        { /* boot-patch: lazily construct singleton X if its vtable is still 0 */\n"
        "            uint32_t _r2 = (uint32_t)ctx->gpr[2]; uint32_t _r30 = vm_read32(_r2 - 0x6AB8);\n"
        "            uint32_t _base = vm_read32(_r30 - 0x7FCC);\n"
        "            uint32_t _obj = (((uint32_t)(-(int32_t)_base)) & 7u) + _base;\n"
        "            static __declspec(thread) int _building = 0;\n"
        "            if (vm_read32(_obj) == 0 && !_building) {\n"
        "                _building = 1; ppu_context _t = *ctx; _t.gpr[3] = _obj; func_00B25CB8(&_t); _building = 0;\n"
        "            } }\n"
        "        uint64_t _cs_27 = ctx->gpr[27];",
    ),
]

def main():
    if not CHUNK.exists():
        print(f"ERROR: {CHUNK} not found (run the lifter first)", file=sys.stderr)
        return 1
    text = CHUNK.read_text(encoding="utf-8", errors="surrogateescape")
    applied = 0
    for desc, anchor, old, new in PATCHES:
        if "boot-patch:" in text and new.split("boot-patch:")[1][:30] in text:
            print(f"  [skip] already applied: {desc}")
            continue
        if anchor not in text:
            print(f"  [WARN] anchor not found for: {desc} -- lifter output changed?", file=sys.stderr)
            continue
        if old not in text:
            print(f"  [WARN] pattern not found for: {desc} -- lifter output changed?", file=sys.stderr)
            continue
        text = text.replace(old, new, 1)
        applied += 1
        print(f"  [ok]   applied: {desc}")
    CHUNK.write_text(text, encoding="utf-8", errors="surrogateescape")
    print(f"apply_boot_patches: {applied} patch(es) applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
