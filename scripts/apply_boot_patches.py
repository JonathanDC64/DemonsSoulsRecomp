#!/usr/bin/env python3
"""Re-apply boot-critical source patches to the (gitignored) lifted output.

The lifter regenerates work/recomp/ppu_recomp_*.cpp from scratch on every run, so
hand fixes don't survive a re-lift. This script re-injects the small, well-understood
fixes that get Demon's Souls past its C++ static-init memory bootstrap. Run it after
every `ppu_lifter.py` invocation, before building.

Usage: python scripts/apply_boot_patches.py [lift dir, default work/recomp]

Fixes:

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

If another program holds the chunk open, the write is retried for up to 30 s.
"""
import sys, pathlib, time

LIFT = pathlib.Path(__file__).resolve().parent.parent / "work" / "recomp"

# OBSOLETE (2026-06-29): Both patches below were workarounds for symptoms of a single
# root cause -- a rotate-by-zero undefined-behavior bug in the lifter's ppc_rotl64/
# ppc_rlwinm/ppc_rlwimi helpers (fixed in ps3recomp 58fd9d8). That UB miscompiled the
# C++ thread-safe-static guard logic (bit math via rldicl/rlwinm), which is what left
# singleton X's vtable NULL (#4) and the DL allocator with a NULL heap (#5). With the UB
# fixed, the guards construct correctly on their own, so these patches are not only
# unnecessary but HARMFUL: the force-build of X (#2) builds it on a context copy and
# corrupts a downstream Meyers singleton (func_00A98AB0 -> returned a garbage 0xB8 'this'
# -> infinite *(0x78) poll near cellSysutilCheckCallback). Disabling them lets boot reach
# the same point and advance further. Kept here (disabled) as a record; re-enable only if
# a genuine post-UB regression is proven. See docs/07, memory demons-souls-rotl-ub-bug.
# Patch 1 (func_00AC8078 NULL-heap -> ds_heap) is a LEGITIMATE shim, kept enabled: the DL
# segregated allocator is sometimes invoked with a NULL heap descriptor (r3==0) for a
# registry/object whose backing heap isn't wired up in our runtime; route those allocations
# to the backed ds_heap instead of spinning in the bin-doubling loop. (Distinct from the
# rotate-by-zero UB -- that's fixed separately; this NULL-heap case persists.) Size is r4
# (confirmed via probe: 0x180), fall back to r5 if larger, min 16.
# Patch 2 (force-build singleton X) stays DISABLED -- it was obsoleted by the rotl-UB fix
# and corrupts a downstream singleton (see _OBSOLETE_PATCHES / commit history).
PATCHES = [
    (
        "func_00AC8078 NULL-heap -> ds_heap",
        "void func_00AC8078(ppu_context* ctx) {",
        # Insert right after the opening brace (robust to a leading DS_FRAME(...)
        # guard line from a --shadow-stack lift, which used to break the old
        # signature+_cs_24 match). The NULL-heap check returns early before
        # DS_FRAME is constructed, so the shadow stack stays balanced.
        "void func_00AC8078(ppu_context* ctx) {\n",
        "void func_00AC8078(ppu_context* ctx) {\n"
        "        if ((uint32_t)ctx->gpr[3] == 0) { /* boot-patch: NULL heap pool -> ds_heap */\n"
        "            uint32_t _sz = (uint32_t)ctx->gpr[4]; uint32_t _al = (uint32_t)ctx->gpr[5];\n"
        "            if (_al > _sz) _sz = _al; if (_sz < 16) _sz = 16;\n"
        "            ctx->gpr[3] = _sz; ds_malloc(ctx); return; }\n",
    ),
]
_OBSOLETE_PATCHES = [
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

# VS Code's C/C++ extension opens a chunk about 3 s after the lifter writes it
# and keeps it open for about 10 s without write sharing. A write in that window
# fails with PermissionError, so wait for the file instead of leaving the chunk
# unpatched.
WRITE_WAIT_S = 30


def write_chunk(chunk, text):
    """Write the patched chunk, retrying while another program holds it open.
    Returns False if it is still locked after WRITE_WAIT_S seconds."""
    deadline = time.monotonic() + WRITE_WAIT_S
    waiting = False
    while True:
        try:
            chunk.write_text(text, encoding="utf-8", errors="surrogateescape")
            return True
        except PermissionError as e:
            if time.monotonic() >= deadline:
                print(f"ERROR: could not write {chunk}: {e.strerror or e}. Another program "
                      f"kept it open for {WRITE_WAIT_S} s, so the patch is not applied. Close "
                      f"that program or exclude the lift folder from it (VS Code: "
                      f"C_Cpp.files.exclude), then run this script again.", file=sys.stderr)
                return False
            if not waiting:
                print(f"  [wait] {chunk.name} is open in another program; "
                      f"retrying for up to {WRITE_WAIT_S} s")
                waiting = True
            time.sleep(0.5)


def main():
    # Optional argument: the lift directory (default work/recomp). The patched
    # function moves between chunks from one lift to the next, so find it.
    lift = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else LIFT
    chunks = sorted(lift.glob("ppu_recomp_*.cpp"))
    if not chunks:
        print(f"ERROR: no chunks in {lift} (run the lifter first)", file=sys.stderr)
        return 1
    applied = 0
    for desc, anchor, old, new in PATCHES:
        CHUNK = next((c for c in chunks if anchor in
                      c.read_text(encoding="utf-8", errors="surrogateescape")), chunks[0])
        text = CHUNK.read_text(encoding="utf-8", errors="surrogateescape")
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
        if not write_chunk(CHUNK, text):
            return 1
        applied += 1
        print(f"  [ok]   applied: {desc} ({CHUNK.name})")
    print(f"apply_boot_patches: {applied} patch(es) applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
