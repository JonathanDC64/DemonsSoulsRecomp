"""tools/region: the address table's check (region_table.py check) on small made-up trees, and the lift's
translation of the hook list and overrides (scripts/des_region.py). Addresses only, all made up.

  python -m unittest discover -s tools/region/tests
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import region_table  # noqa: E402
import des_region    # noqa: E402  (region_table puts scripts/ on the path)

TABLE_HEAD = "/* made up */\n"


def tree(root, table, sources, hooks="0x00200000=hook_a\n", overrides=("0x00300000=over_a",)):
    """A port tree with just what the check reads."""
    (root / "port" / "regions").mkdir(parents=True)
    (root / "scripts").mkdir()
    (root / "port" / "regions" / "BLES00932.inc").write_text(
        TABLE_HEAD + "".join(f"DES_REGION_ADDR(0x{us:08X}, 0x{eu:08X})  /* x */\n" for us, eu in table),
        encoding="utf-8")
    (root / "port" / "insn_hooks.txt").write_text("# hooks\n" + hooks, encoding="utf-8")
    (root / "scripts" / "lift_ppu.json").write_text(
        json.dumps({"insn_hooks": "port/insn_hooks.txt", "override_funcs": list(overrides)}), encoding="utf-8")
    (root / "scripts" / "apply_boot_patches.py").write_text(
        '"""patches 0x01B92CF8 in a docstring: not an address the port uses"""\nADDR = 0x00400000\n',
        encoding="utf-8")
    for name, text in sources.items():
        (root / "port" / name).write_text(text, encoding="utf-8")


BASE = [(0x00200000, 0x00201000), (0x00300000, 0x00301000), (0x00400000, 0x00401000)]


class CheckTest(unittest.TestCase):
    def run_check(self, table, sources, **kw):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            tree(root, table, sources, **kw)
            return region_table.check(root)

    def test_clean_tree(self):
        src = ("#include \"des_region.h\"\n"
               "constexpr uint32_t kThing = 0x01900000u;   /* 0x01A00000 in a comment */\n"
               "void f(ppu_context* ctx, uint32_t addr) {\n"
               "    vm_read32(DES_AT(kThing));\n"
               "    ctx->lr = DES_AT(0x00500000u);\n"
               "    if (addr == 0x00200000u) return;\n"
               "    switch (addr) { case 0x00300000u: break; }\n"
               "    DES_REGISTER_FUNCTION(0x00600000u, f);\n"
               "    func_00700000(ctx);\n"
               "    const char* s = \"0x01A00000\";\n"
               "    bool ok = a >= 0x00010000u;   // the guest memory's floor\n"
               "}\n")
        table = BASE + [(0x00500000, 0x00501000), (0x00600000, 0x00601000), (0x00700000, 0x00701000),
                        (0x01900000, 0x01900230)]
        self.assertEqual(self.run_check(sorted(table), {"des_x.cpp": src}), [])

    def test_raw_address_is_caught(self):
        errors = self.run_check(BASE + [(0x00500000, 0x00501000)],
                                {"des_x.cpp": "void f(ppu_context* ctx) { ctx->lr = 0x00500000u; }\n"})
        self.assertEqual(len(errors), 1)
        self.assertIn("des_x.cpp:1: 0x00500000 is used as it is", errors[0])

    def test_raw_use_of_a_named_address_is_caught(self):
        src = ("constexpr uint32_t kVt = 0x01900000u;\n"
               "bool is(uint32_t o) { return vm_read32(o) == kVt; }\n"
               "bool ok(uint32_t o) { return vm_read32(o) == DES_AT(kVt); }\n")
        errors = self.run_check(BASE + [(0x01900000, 0x01900230)], {"des_x.cpp": src})
        self.assertEqual(len(errors), 1)
        self.assertIn("des_x.cpp:2: kVt", errors[0])

    def test_missing_address(self):
        errors = self.run_check(BASE, {"des_x.cpp": "void f(ppu_context* ctx) { ctx->lr = DES_AT(0x00500000u); }\n"})
        self.assertEqual(len(errors), 1)
        self.assertIn("0x00500000 is not in BLES00932.inc", errors[0])

    def test_missing_hook_override_and_call(self):
        errors = self.run_check([(0x00400000, 0x00401000)], {"des_x.cpp": "void g() { func_00700000(0); }\n"})
        text = "\n".join(errors)
        for addr in ("0x00200000", "0x00300000", "0x00700000"):
            self.assertIn(f"{addr} is not in BLES00932.inc", text)
        self.assertNotIn("0x01B92CF8", text)   # (a docstring, not code)

    def test_unused_line(self):
        errors = self.run_check(BASE + [(0x00800000, 0x00801000)], {})
        self.assertEqual(errors, ["BLES00932.inc: 0x00800000 is not used by the port: remove its line"])

    def test_unsorted_and_doubled(self):
        errors = self.run_check([(0x00300000, 0x00301000), (0x00200000, 0x00201000)], {})
        self.assertEqual(len(errors), 1)
        self.assertIn("is not after", errors[0])
        errors = self.run_check(BASE + [(0x00500000, 0x00401000)],
                                {"des_x.cpp": "void f(ppu_context* ctx) { ctx->lr = DES_AT(0x00500000u); }\n"})
        self.assertEqual(errors, ["BLES00932.inc: European address 0x00401000 is on 2 lines"])

    def test_hook_reading_its_key_is_caught(self):
        src = ("static void helper(ppu_context* ctx, uint32_t addr) { ctx->fpr[1] = vm_readf(addr + 4); }\n"
               "extern \"C\" void hook_a(ppu_context* ctx, uint32_t addr) {\n"
               "    if (addr == 0x00200000u) helper(ctx, addr);\n"
               "    const uint32_t insn = vm_read32(addr);\n"
               "    const uint32_t ok = vm_read32(des_region_addr(addr));\n"
               "}\n")
        errors = self.run_check(BASE, {"des_x.cpp": src})
        self.assertEqual(len(errors), 2, errors)
        self.assertIn("des_x.cpp:4: hook_a uses its key addr as a guest address", errors[0])
        self.assertIn("des_x.cpp:1: helper uses its key addr", "\n".join(errors))

    def test_the_port_itself(self):
        self.assertEqual(region_table.check(), [])


class LiftInputsTest(unittest.TestCase):
    TABLE = {0x00200000: 0x00201000, 0x00300000: 0x00301000}

    def test_insn_hooks(self):
        text = "# head\n\n0x00200000=hook_a   # why\n0x300000 = hook_b\n"
        self.assertEqual(des_region.insn_hooks(text, self.TABLE).split("\n"),
                         ["# head", "", "0x00201000=hook_a@0x00200000   # why", "0x00301000=hook_b@0x00300000", ""])

    def test_insn_hooks_missing(self):
        with self.assertRaises(des_region.RegionError):
            des_region.insn_hooks("0x00400000=hook_c\n", self.TABLE)

    def test_overrides_and_names(self):
        self.assertEqual(des_region.override_funcs(["0x200000=ds_malloc"], self.TABLE), ["0x201000=ds_malloc"])
        self.assertEqual(des_region.func_name(self.TABLE, 0x00300000, des_region.EU), "func_00301000")
        self.assertEqual(des_region.func_name(self.TABLE, 0x00300000, des_region.US), "func_00300000")

    def test_the_real_table_loads(self):
        table = des_region.load_table()
        self.assertGreater(len(table), 100)
        self.assertEqual(list(table), sorted(table))


if __name__ == "__main__":
    unittest.main()
