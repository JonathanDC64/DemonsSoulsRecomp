# 65. Lifter fixes: CTR-and-CR branches, CR1 for FP record forms, mcrfs (static, 2026-10-19)

Status: written statically on 2026-10-19; the patch has since merged as ps3recomp #77. The code is ps3recomp `fork/master` at
4a76fc0. Line numbers are on that commit. A static review found three PPU lifter gaps tonight; this doc checks each
one against the code, the Power ISA (v2.03, Book I) and RPCS3's PPU interpreter (`rpcs3/Emu/Cell/PPUInterpreter.cpp`
at 065b490), and gives one patch with tests. The gaps are old: the code is the same before tonight's CR-fields
change (8fbe2d4).

**Any lifter change needs a re-lift and a regress run on each title before it merges.**

## Short answer

- **All three claims hold.** I checked each one by reading the code and by running master's lifter in Python over
  every encoding the disassembler can produce (a scratch sweep, no compiler).
- **Gap 1, CTR-and-CR branches:** `bdnzt`, `bdnzf`, `bdzt`, `bdzf` and their `lr`/`ctr`/`l`/`a` forms lift to an
  always-taken branch that never decrements CTR. A loop closed by `bdnzf eq,loop` never exits on its count.
- **Gap 2, CR1:** FP record forms (`fadd.` and the rest) never write CR1.
- **Gap 3, mcrfs:** no handler, so its CR field is never written. The disassembler also prints its operands wrong.
- **The patch** fixes all three in `tools/ppu_lifter.py` and `tools/ppu_disasm.py`, and adds tests to
  `tools/test_ppu_lift.py`. It applies to `fork/master` (`git apply --cached --check` passed against a temporary
  index).
- **Tests:** the test file compiles and runs a C++ driver (cl or c++), so I did not run it (no builds tonight). I ran
  only its pure-Python parts, from a scratch copy: all text checks pass, the driver generates with 0 skipped cases,
  and a Python evaluation of all 256 new branch cases' lifted conditions matches the ISA reference. Against master's
  lifter the new checks fail, as they should.
- **Other gaps** found by the same sweep and by reading the branch handlers: `ba`/`bla`, conditional branch-and-link,
  the record forms of `mulld`/`divd`/`divdu`, the OE=1 forms, `lwaux`, `lswi`/`stswi`, `stvlx`/`stvrx` and 36 VMX
  ops. They are not in the patch. Whether they matter depends on whether a title's lift contains them (section 4).
- **Suggested branch:** `fix/lifter-bdnz-cr1` (ps3recomp), based on master.

## 1. Gap 1: combined CTR-and-CR conditional branches

### Confirmed

- The disassembler names the combined forms `dnz`/`dz` plus the condition name
  (`tools/ppu_disasm.py:133-140`). The true conditions are `lt gt eq so`, the false ones `ge le ne ns`. So
  `bdnzf eq,X` comes out as `bdnzne X`, and `bdzt 4*cr7+gt` as `bdzgt cr7, X`.
- `_branch_condition` (`tools/ppu_lifter.py:3222-3253`) knows only `eq ne lt ge gt le so ns dnz dz`. Any other name
  returns `/* cond: <mnemonic> */ 1` (`:3253`).
- All four branch handlers call it: conditional return `b<cond>lr` (`:1701-1712`), `b<cond>lrl` (`:1714-1719`),
  `b<cond>ctr[l]` (`:1721-1734`) and the B-form conditional branch (`:1736-1765`).
- The sweep confirms it. On master, every combined name lifts to the fallback, for example
  `bdnzne cr7, X -> if (/* cond: bdnzne */ 1) goto ...`. That is always taken, and CTR is not touched.
- `tools/test_ppu_lift.py` has no branch case at all. `tools/test_ppu_branches.py` checks the shape of `blr`,
  `blrl`, `bnelrl`, `bctrl` and `bdnz` only.
- These forms are not only theoretical. The disassembler audit (`tools/disasm_audit.py:166-174`) records seeing
  them when it compared this disassembler with Capstone on a real title (BO 0-3 and 8-11, plus the `bdz` ones). It
  does not say whether they were inside real functions or data decoded as code.

### ISA and RPCS3

- Book I 2.4 (bc, bclr): if BO2 is 0, CTR = CTR - 1. Then `ctr_ok = BO2 | ((CTR != 0) ^ BO3)` and
  `cond_ok = BO0 | (CR[BI] == BO1)`. Branch if both. In the 5-bit BO value, BO0 is 0x10, BO1 0x08, BO2 0x04,
  BO3 0x02, and 0x01 is a hint.
- RPCS3's `BC()` (`PPUInterpreter.cpp:3197-3239`) and `BCLR()` (`:3297-3337`) do exactly this.
- bcctr is different. Book I says BO2=0 is an invalid form there. RPCS3's `BCCTR()` (`:3447-3476`) never touches
  CTR: it branches if `BO0` or `CR[BI] == BO1`.
- CTR width: RPCS3 keeps a 64-bit CTR. This lifter keeps it 32-bit: `mtctr` masks it (`tools/ppu_lifter.py:1818-1822`)
  and `dnz`/`dz` decrement as `uint32_t` (`:3249-3250`). The fix keeps the 32-bit model. It differs from the ISA only
  when CTR is decremented from 0 (2^32 loops instead of 2^64).

### Fix

- For `dnz`/`dz` plus a known condition, emit `(<CTR test> && <CR test>)`. The CTR test comes first, so `&&` never
  skips the decrement. Example:
  `bdnzne cr7, X -> if ((((ctx->ctr = (uint32_t)(ctx->ctr - 1)) != 0) && (!(ctx->crf[7] & 2)))) goto loc_X;`
- For `ctr` forms, drop the CTR part, as RPCS3 does (`bdnzctr` lifts to `1`, `bdnznectr` to the CR test only).
- `bc`, `bclr` and `bcctr` printed raw now lift to `1` without the fallback marker. The disassembler prints them raw
  only when BO is 1z1zz, "branch always" (`tools/ppu_disasm.py:119-121`), so the old `/* cond */ 1` was right by
  luck. After the patch the fallback marks only names that are not conditional branches (section 4).
- The plain forms (`beq`, `bdnz`, ...) emit the same text as before, so unchanged lifts stay byte-identical.

## 2. Gap 2: FP record forms never set CR1

### Confirmed

- The comment at `tools/ppu_lifter.py:1110-1115` says FP dot-forms "set CR1". The wrapper at `:1121-1123` keeps
  mnemonics that start with `f`, `v`, `mt` or `mf` out of the CR0 path. Nothing else writes `crf[1]` for them: master's
  lifter has no `crf[1]` anywhere. The FP handlers strip the dot (`mn_base = mn.rstrip(".")`, `:1930`), so `fadd.`
  lifts the add and drops the CR1 update.

### ISA and RPCS3

- Book I 2.3.1: an FP instruction with Rc=1 copies FPSCR[FX FEX VX OX] (the top nibble of the 32-bit FPSCR) to CR1.
  This includes `mffs.`, `mtfsf.`, `mtfsfi.`, `mtfsb0.` and `mtfsb1.`.
- RPCS3 does not follow the ISA here. Its record forms call `ppu_set_fpcc(result, 0.)` (`PPUInterpreter.cpp:401-446`,
  `FADD()` at `:6537-6547`), which puts the FPCC class of the result (less, greater, equal, unordered against 0) in
  FPSCR[FPCC] and CR1. Its own comment there says "TODO". RPCS3 does not model the exception bits either: `MFFS()`
  returns only FPCC (`:6413-6425`), `MTFSF()` changes nothing but CR1 (`:6428-6439`), and `MCRFS()` clears nothing
  (`:6342-6352`).

### How this lifter models FPSCR

- `ctx->fpscr` is a plain field (`runtime/ppu/ppu_context.h:87`), zeroed by `ppu_context_init` (`:110`).
- `mffs` reads it (`tools/ppu_lifter.py:2163-2165`).
- `mtfsb0`, `mtfsb1` and `mtfsfi` lift to a comment, "FPSCR unmodeled" (`:2174-2175`). `mtfsf` lifts to "ignored for
  now" (`:2177-2178`), and `mtfsf.` falls to the TODO catch-all because the test is `mn == "mtfsf"`.
- `fcmpu`/`fcmpo` write only the CR field, not FPSCR[FPCC] (`:2042-2053`).
- So no lifted instruction writes `ctx->fpscr`. The exception bits are never set, and the register reads as 0.

Under that model, CR1 should be the top nibble of `ctx->fpscr`: what `mffs` would report for FX, FEX, VX and OX. In
practice that is 0. Real hardware differs: FX is sticky. The first inexact result sets XX and FX, and both stay set
until software clears them, so hardware CR1 often has LT (FX) set. The lifter cannot reproduce that without modelling exceptions, and `mffs` already has the same limit.

### Fix

- `_translate` appends `ctx->crf[1] = (uint8_t)((ctx->fpscr >> 28) & 0xFu);` to every record form whose mnemonic
  starts with `f`, and to `mffs.`, `mtfsf.`, `mtfsfi.`, `mtfsb0.` and `mtfsb1.`. It skips lifts that hit the TODO
  catch-all.
- `mtfsf.` now uses the `mtfsf` no-op (`mn.rstrip(".") == "mtfsf"`), so it is no longer a TODO.
- I followed the ISA, not RPCS3's FPCC shortcut. Games were written for hardware, and the ISA is what hardware does.
  Compilers almost never emit FP record forms, so the practical effect should be small either way.

## 3. Gap 3: mcrfs has no handler

### Confirmed

- The disassembler decodes extended opcode 64 as `mcrfs` (`tools/ppu_disasm.py:869`). It prints the operands
  through the generic FP branch (`:891-892`) as `f<FRT>, f<FRB>`. FRB is bits 16-20, which are reserved, so BFA
  (bits 11-13) is lost. `mcrfs cr7, cr4` prints as `mcrfs f28, f0`.
- Master's lifter has no `mcrfs` handler. It falls to `/* TODO: mcrfs f28, f0 */;` (`tools/ppu_lifter.py:3220`), so
  the target CR field keeps its old value.

### ISA and RPCS3

- Book I (mcrfs): CR field BF gets FPSCR field BFA. Then the exception bits that were copied are cleared: FX and OX
  (field 0); UX, ZX, XX, VXSNAN (field 1); VXISI, VXIDI, VXZDZ, VXIMZ (field 2); VXVC (field 3); VXSOFT, VXSQRT, VXCVI
  (field 5). FEX and VX are summary bits and follow the bits they summarize.
- RPCS3's `MCRFS()` (`PPUInterpreter.cpp:6342-6352`) copies the field and clears nothing. It logs a warning unless
  BFA is 4 (FPCC), which hints that field 4 is the usual case in games.

### Fix

- The disassembler prints `mcrfs cr<BF>, cr<BFA>`, like `mcrf`.
- The lifter emits `ctx->crf[BF] = (uint8_t)((ctx->fpscr >> (28 - 4*BFA)) & 0xFu);`.
- It does not clear the exception bits. FPSCR writes are not modeled anywhere (`mtfsf` and friends are no-ops), the
  bits are never set, and RPCS3 skips the clear too. If FPSCR is ever modeled, add the clear with the field masks
  above (in the 32-bit value: 0x90000000, 0x0F000000, 0x00F00000, 0x00080000, 0, 0x00000700, 0, 0 for fields 0-7).

### Open point: mcrfs from field 4

`mcrfs crX, 4` reads FPSCR[FPCC]. RPCS3 sets FPCC on every `fcmpu`/`fcmpo` (`FCMPU()`, `:6442-6453`, through
`ppu_set_fpcc`), and so does hardware. This lifter never sets FPCC, so after the patch `mcrfs crX, 4` reads 0, where
before it left the CR field unchanged. Neither is right. If a title's lift contains `mcrfs` with BFA=4, `fcmpu` and
`fcmpo` should also write FPCC (`ctx->fpscr = (ctx->fpscr & ~0xF000u) | (cr_val << 12)`). I left that out: `fcmpu` is
common in game code, and the CR-fields change exists to remove read-modify-writes from hot paths. Measure before
adding it.

## 4. Other gaps found

I ran every opcode the disassembler decodes through master's lifter, in a scratch script with fixed register fields
(all major opcodes; all extended-opcode values of 4, 19, 31, 59 and 63 with Rc 0 and 1; all 32 BO values of bc,
bclr and bcctr with LK and AA). The forms below lift to the fallback or the TODO catch-all. The branch-and-link row
comes from reading the handler. Reserved encodings (`op31_x*`, `vmx_x*`, `ld?`, `std?`, `rld??`) are left out.

| Form | What happens on master | Matters? |
| --- | --- | --- |
| `ba`, `bla` (I-form, absolute) | The `b`/`bl` handlers match exact names (`tools/ppu_lifter.py:1658`, `:1686`), so these reach the conditional handler and lift to `if (/* cond: ba */ 1) ...`. `ba` works as a jump by luck. `bla` becomes a jump: no LR write, no return. Still true after the patch. | Only if a title has absolute branches, which PS3 user code should not. Search the lift for `/* cond: b`. |
| Conditional branch-and-link (`bcl`, `beql`, `bdnzl`, ...) | The B-form handler (`:1736-1765`) treats LK=1 like LK=0: a goto or tail jump, no LR write, no return. The ISA writes LR = CIA+4 whether or not the branch is taken. The `bcl 20,31,$+4; mflr rX` "get PC" idiom would read a stale LR. | Only if present. Search the disassembly for `bcl` and `b<cond>l`. |
| `mulld.`, `divd.`, `divdu.` | Handlers match exact names (`mulld` `:2059`, `divd` `:2067`), so the dot forms are TODO. The whole instruction is dropped, not just CR0. | Yes, if present. A compiler can emit them when a product or quotient is compared with 0. |
| OE=1 forms: `mullwo(.)`, `mulldo(.)`, `divwo(.)`, `divwuo(.)`, `divdo(.)`, `divduo(.)`, `nego(.)`, `addzeo(.)` | TODO: the instruction is dropped. | Rarely emitted (overflow checks). |
| `lwaux` | TODO: no load, no RA update. | Yes, if present. |
| `lswi`, `stswi` | TODO. | Unlikely on Cell (microcoded), possible in old copy code. |
| `stvlx`, `stvlxl`, `stvrx`, `stvrxl` | TODO: the store is dropped. The load twins `lvlx`/`lvrx` are handled. | Yes, if present: Cell code uses them for unaligned vector stores. |
| VMX: `vaddsws`, `vaddubs`, `vsubsbs`, `vsubshs`, `vsubuhs`, `vsubuws`, `vavgsb`, `vavgsh`, `vavgsw`, `vavguh`, `vavguw`, `vslh`, `vsrb`, `vsrh`, `vsrab`, `vsrah`, `vrlh`, `vmhaddshs`, `vmhraddshs`, `vmladduhm`, `vmsumubm`, `vmsumuhm`, `vmsumuhs`, `vmulesb`, `vmuleub`, `vmulosb`, `vmuloub`, `vpkshus`, `vpkswss`, `vpkswus`, `vpkuhum`, `vsum4sbs`, `vsum4shs`, `vsum4ubs`, `vexptefp`, `vlogefp` | TODO: vD is left unchanged. | Yes, if present: media and audio code uses saturating, averaging and packing ops. |
| `rfi`; `mcrfs.` | TODO. | No: `rfi` is privileged, and `mcrfs.` is an invalid form. |

Check Demon's Souls' lift for these counts. A count of 0 in every title means no action. Any non-zero
count is worth a follow-up patch in the same style.

## 5. The patch

Branch: `fix/lifter-bdnz-cr1` (ps3recomp), based on master. Files: `tools/ppu_disasm.py`, `tools/ppu_lifter.py`,
`tools/test_ppu_lift.py`. It applies to `fork/master` at 4a76fc0.

```diff
diff --git a/tools/ppu_disasm.py b/tools/ppu_disasm.py
index 67dfd3f..1abd76e 100644
--- a/tools/ppu_disasm.py
+++ b/tools/ppu_disasm.py
@@ -888,6 +888,8 @@ def decode(insn: int, addr: int = 0) -> Instruction:
             elif xo_full == 711:
                 fm = bits(insn, 7, 14)
                 result.operands = f"0x{fm:02X}, f{frb}"
+            elif xo_full == 64:   # mcrfs BF, BFA: CR field, FPSCR field
+                result.operands = f"cr{bits(insn, 6, 8)}, cr{bits(insn, 11, 13)}"
             else:
                 result.operands = f"f{frd}, f{frb}"
             return result
diff --git a/tools/ppu_lifter.py b/tools/ppu_lifter.py
index 50f30be..e0b14bf 100644
--- a/tools/ppu_lifter.py
+++ b/tools/ppu_lifter.py
@@ -1112,9 +1112,17 @@ class PPULifter:
     # arithmetic; the _translate wrapper appends the CR0 update for any
     # dot-form whose first operand is the destination GPR. Excluded:
     # stwcx./stdcx. (their handlers set CR0 themselves), FP dot-forms
-    # (those set CR1), VMX dot-forms (those set CR6), mt*/mf* specials.
+    # (those set CR1, below), VMX dot-forms (those set CR6), mt*/mf* specials.
     _CR0_SELF_HANDLED = ("stwcx.", "stdcx.")
 
+    # Record-form FP ops (fadd. ... fcfid., and the FPSCR moves below) copy
+    # FPSCR[FX FEX VX OX], its top nibble, to CR1 (Book I 2.3.1). Nothing
+    # lifted writes ctx->fpscr -- the exception bits are not modeled, and
+    # mtfsf/mtfsb0/mtfsb1/mtfsfi are no-ops -- so CR1 gets what mffs would
+    # read there, 0 from a zeroed context. RPCS3 puts the result's FPCC
+    # class in CR1 instead (marked TODO there); this follows the ISA.
+    _CR1_FPSCR_OPS = ("mffs.", "mtfsf.", "mtfsfi.", "mtfsb0.", "mtfsb1.")
+
     def _translate(self, insn: Instruction, func: LiftedFunction) -> str:
         code = self._translate_op(insn, func)
         mn = insn.mnemonic
@@ -1128,6 +1136,9 @@ class PPULifter:
                          f"uint32_t _c = (_r < 0) ? 8u : (_r > 0) ? 4u : 2u; "
                          f"_c |= (uint32_t)((ctx->xer >> 31) & 1u); "
                          f"ctx->crf[0] = (uint8_t)_c; }}")
+        if (mn.endswith(".") and code and not code.startswith("/* TODO")
+                and (mn.startswith("f") or mn in self._CR1_FPSCR_OPS)):
+            code += " ctx->crf[1] = (uint8_t)((ctx->fpscr >> 28) & 0xFu);"
         return code
 
     def _translate_op(self, insn: Instruction, func: LiftedFunction) -> str:
@@ -2164,6 +2175,16 @@ class PPULifter:
             frd = _reg_idx(ops[0])
             return f"{{ uint64_t fpscr64 = ctx->fpscr; memcpy(&ctx->fpr[{frd}], &fpscr64, 8); }}"
 
+        # mcrfs crD, crS — copy FPSCR field crS to CR field crD. The ISA also
+        # clears the exception bits it copies (FX OX, UX ZX XX VXSNAN, VXISI
+        # VXIDI VXZDZ VXIMZ, VXVC, VXSOFT VXSQRT VXCVI in fields 0, 1, 2, 3,
+        # 5); FPSCR writes are not modeled (see mtfsf below), so only the copy
+        # is lifted. RPCS3's MCRFS clears nothing either.
+        if mn == "mcrfs":
+            bf = int(ops[0][2:]) if ops[0].startswith("cr") else int(ops[0])
+            bfa = int(ops[1][2:]) if ops[1].startswith("cr") else int(ops[1])
+            return f"ctx->crf[{bf}] = (uint8_t)((ctx->fpscr >> {28 - 4 * bfa}) & 0xFu);"
+
         # mtfsf — move to FPSCR fields
         # mtfsb0/mtfsb1 clear/set a single FPSCR bit; mtfsfi sets a 4-bit field.
         # Those bits are the rounding mode, the exception enables and the sticky
@@ -2174,7 +2195,7 @@ class PPULifter:
         if mn.rstrip(".") in ("mtfsb0", "mtfsb1", "mtfsfi"):
             return f"/* {mn} {insn.operands}: FPSCR unmodeled */;"
 
-        if mn == "mtfsf":
+        if mn.rstrip(".") == "mtfsf":   # mtfsf. sets CR1 too (_translate)
             return f"/* mtfsf: FPSCR update -- ignored for now */;"
 
         # ------- Store/load with update indexed -------
@@ -3225,6 +3246,7 @@ class PPULifter:
         if mn.startswith("b"):
             mn = mn[1:]  # strip leading 'b'
 
+        via_ctr = mn.endswith("ctr")
         # strip trailing 'lr', 'ctr'
         mn = mn.replace("lr", "").replace("ctr", "")
 
@@ -3250,6 +3272,23 @@ class PPULifter:
             "dz": "((ctx->ctr = (uint32_t)(ctx->ctr - 1)) == 0)",
         }
 
+        # bcctr never decrements CTR: its BO2=0 forms are invalid (Book I
+        # 2.4) and RPCS3's BCCTR tests only BO0/BO1, so drop the CTR part.
+        if via_ctr and mn.startswith(("dnz", "dz")):
+            mn = mn[3:] if mn.startswith("dnz") else mn[2:]
+            if not mn:
+                return "1"
+        # bdnzt/bdnzf/bdzt/bdzf come through as dnz/dz plus the CR condition
+        # (bdnzf eq -> dnzne). Branch only if both hold; the CTR test goes
+        # first so && never skips the decrement (Book I 2.4, RPCS3 BC/BCLR).
+        for dec in ("dnz", "dz"):
+            if mn.startswith(dec) and mn[len(dec):] in cond_map:
+                return f"({cond_map[dec]} && {cond_map[mn[len(dec):]]})"
+        if mn == "c":
+            # bc/bclr/bcctr printed raw: the disassembler does that only for
+            # BO = 1z1zz, branch always.
+            return "1"
+
         return cond_map.get(mn, f"/* cond: {mnemonic} */ 1")
 
     # ------------------------------------------------------------------ #
diff --git a/tools/test_ppu_lift.py b/tools/test_ppu_lift.py
index 911fed8..1b3c1ff 100644
--- a/tools/test_ppu_lift.py
+++ b/tools/test_ppu_lift.py
@@ -274,15 +274,20 @@ CASES = []   # dicts: name, word, in_regs {idx:val}, in_ca, expects [(reg, val,
              # IEEE-754 pattern via memcpy, not value).
 
 def case(name, word, in_regs, expects, in_ca=None, exp_ca=None, exp_cr=None, may_trap=False,
-         in_mem=None, exp_mem=None, in_fpr=None, exp_fpr=None, in_cr=None, exp_crw=None):
+         in_mem=None, exp_mem=None, in_fpr=None, exp_fpr=None, in_cr=None, exp_crw=None,
+         in_fpscr=None, in_ctr=None, exp_ctr=None, exp_taken=None):
     # in_cr / exp_crw: the whole condition register as the guest sees it
     # (mfcr's 32-bit value, CR0 in bits 31:28), set before the instruction and
     # compared after it in full, SO bits included.
+    # in_fpscr / in_ctr seed FPSCR and CTR; exp_ctr is CTR after the
+    # instruction. exp_taken marks a conditional branch: emit_c runs it in a
+    # helper function and checks whether it was taken.
     CASES.append(dict(name=name, word=word, in_regs=in_regs, expects=expects,
                       in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap,
                       in_mem=in_mem or {}, exp_mem=exp_mem or {},
                       in_fpr=in_fpr or {}, exp_fpr=exp_fpr or [],
-                      in_cr=in_cr, exp_crw=exp_crw))
+                      in_cr=in_cr, exp_crw=exp_crw, in_fpscr=in_fpscr,
+                      in_ctr=in_ctr, exp_ctr=exp_ctr, exp_taken=exp_taken))
 
 # VMX/vector cases: unlike the GPR CASES above, a vupk-family op reads/writes
 # ctx->vr[] directly (no memory load/store instructions needed to exercise
@@ -562,6 +567,78 @@ def build_cases():
                      (31 << 26) | (R[1] << 21) | (fxm << 12) | (144 << 1),
                      {R[1]: v}, [(R[1], v, MASK64)], in_cr=cr, exp_crw=want)
 
+    # --- conditional branches: CTR and CR together --------------------------
+    # Every BO value of bc and bclr, with the CR bit set and clear and a CTR
+    # that does and does not reach zero. bdnzt/bdnzf/bdzt/bdzf (BO2 clear,
+    # BO0 clear) lifted to an always-taken branch that never decremented CTR,
+    # so a loop closed by `bdnzf eq,loop` ran on past its count. Book I 2.4
+    # and RPCS3's BC/BCLR: CTR -= 1 unless BO2, then branch if
+    # (BO2 | ((CTR != 0) ^ BO3)) & (BO0 | (CR[BI] == BO1)). CTR stays 32-bit,
+    # as mtctr writes it. The mnemonic is the decoder's own (all 32 BO values
+    # map to some name); the expectation comes from BO alone.
+    def ref_bc(bo, crbit, ctr):
+        if not bo & 0x04:
+            ctr = (ctr - 1) & MASK32
+        ctr_ok = bool(bo & 0x04) or ((ctr != 0) != bool(bo & 0x02))
+        cond_ok = bool(bo & 0x10) or crbit == (bo >> 3) & 1
+        return ctr_ok and cond_ok, ctr
+    for bo in range(32):
+        for bclr, bi in ((False, 30), (True, 1)):   # bc on cr7.eq, bclr on cr0.gt
+            if bclr:
+                word = (19 << 26) | (bo << 21) | (bi << 16) | (16 << 1)
+            else:
+                word = (16 << 26) | (bo << 21) | (bi << 16) | 8   # target $+8
+            mn = ppu_disasm.decode(word, 0).mnemonic
+            for crbit in (0, 1):
+                cr = crb_set(0x2468ACE1, bi, crbit)
+                for ctr in (1, 2):
+                    taken, ctr_out = ref_bc(bo, crbit, ctr)
+                    case(f"{mn} bo={bo} bi={bi} crbit={crbit} ctr={ctr}", word,
+                         {}, [], in_cr=cr, exp_crw=cr, in_ctr=ctr, exp_ctr=ctr_out,
+                         exp_taken=taken)
+
+    # --- FP record forms set CR1 -------------------------------------------
+    # Rc=1 FP ops copy FPSCR[FX FEX VX OX], its top nibble, to CR1 (Book I
+    # 2.3.1); nothing wrote CR1 before. No lifted op sets an FPSCR bit, so
+    # FPSCR is seeded here, and CR1 must follow it as mffs does.
+    def dbits(v):
+        return struct.unpack(">Q", struct.pack(">d", v))[0]
+    fa, fb = 1.5, 2.25   # exact in single and double, and so are a+b and a*b
+    for fpscr in (0x00000000, 0xA0000000, 0x5F0F0000):
+        for cr in (0xFFFFFFFF, 0x2468ACE1):
+            want = (cr & ~0x0F000000 & 0xFFFFFFFF) | ((fpscr >> 28) << 24)
+            fin = {1: dbits(fa), 2: dbits(fb)}
+            sfx = f"fpscr={fpscr:#010x} cr={cr:#010x}"
+            case(f"fadd. {sfx}", (63 << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (21 << 1) | 1,
+                 {}, [], in_fpr=fin, exp_fpr=[(3, dbits(fa + fb))],
+                 in_fpscr=fpscr, in_cr=cr, exp_crw=want)
+            case(f"fmuls. {sfx}", (59 << 26) | (3 << 21) | (1 << 16) | (2 << 6) | (25 << 1) | 1,
+                 {}, [], in_fpr=fin, exp_fpr=[(3, dbits(fa * fb))],
+                 in_fpscr=fpscr, in_cr=cr, exp_crw=want)
+            case(f"fmr. {sfx}", (63 << 26) | (3 << 21) | (2 << 11) | (72 << 1) | 1,
+                 {}, [], in_fpr=fin, exp_fpr=[(3, dbits(fb))],
+                 in_fpscr=fpscr, in_cr=cr, exp_crw=want)
+            case(f"mffs. {sfx}", (63 << 26) | (3 << 21) | (583 << 1) | 1,
+                 {}, [], exp_fpr=[(3, fpscr)], in_fpscr=fpscr, in_cr=cr, exp_crw=want)
+        case(f"fadd fpscr={fpscr:#010x} leaves cr1 alone",
+             (63 << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (21 << 1),
+             {}, [], in_fpr={1: dbits(fa), 2: dbits(fb)}, exp_fpr=[(3, dbits(fa + fb))],
+             in_fpscr=fpscr, in_cr=0xFFFFFFFF, exp_crw=0xFFFFFFFF)
+
+    # --- mcrfs: an FPSCR field into a CR field -------------------------------
+    # It had no handler (and the decoder printed its fields as FPRs, losing
+    # BFA), so the CR field was never written. The ISA then clears the
+    # exception bits it copied; FPSCR writes are not modeled, so only the CR
+    # side is checked.
+    for fpscr in (0x9F3C5A61, 0x00000000, 0xFFFFFFFF):
+        for cr in (0x2468ACE1, 0xFFFFFFFF):
+            for bf, bfa in ((7, 4), (1, 0), (0, 5), (6, 6), (3, 7)):
+                f = (fpscr >> (28 - 4 * bfa)) & 0xF
+                want = (cr & ~(0xF << (28 - 4 * bf)) & 0xFFFFFFFF) | (f << (28 - 4 * bf))
+                case(f"mcrfs cr{bf},cr{bfa} fpscr={fpscr:#010x} cr={cr:#010x}",
+                     (63 << 26) | (bf << 23) | (bfa << 18) | (64 << 1),
+                     {}, [], in_fpscr=fpscr, in_cr=cr, exp_crw=want)
+
     # --- FP D-form load/store-with-update RA writeback ---------------------
     # lfsu/lfdu/stfsu/stfdu (opcodes 49/51/53/55) sat in the plain lfs/lfd/
     # stfs/stfd handling, so the access happened but RA never got EA written
@@ -863,6 +940,68 @@ def check_barrier_lowering():
     print(f"[barriers] {n - fails} checks passed, {fails} FAILED")
     return fails == 0
 
+# ---------------------------------------------------------------------------
+# Branch conditions and the FPSCR moves, as text.
+#
+# Every BO value of every conditional-branch form has to lift to a real
+# condition: the "/* cond */ 1" fallback is a branch that is always taken and
+# never touches CTR, which is what bdnzt/bdnzf/bdzt/bdzf lifted to. bcctr and
+# b<cond>lrl call through ps3_indirect_call, which the CASES driver cannot
+# run, so they are only checked here. bcctr must not decrement CTR: BO2=0 is
+# an invalid form there, and RPCS3 tests only BO0/BO1.
+# ---------------------------------------------------------------------------
+
+def check_branch_and_fpscr_lifts():
+    lifter = PPULifter()
+    dummy = LiftedFunction(name="conf", start_addr=0x30000, end_addr=0x30100)
+    fails = n = 0
+    for bo in range(32):
+        for bi in (0, 2, 29, 31):
+            words = [(16 << 26) | (bo << 21) | (bi << 16) | 8 | (aa << 1) | lk
+                     for aa in (0, 1) for lk in (0, 1)]
+            words += [(19 << 26) | (bo << 21) | (bi << 16) | (xo << 1) | lk
+                      for xo in (16, 528) for lk in (0, 1)]
+            for word in words:
+                insn = ppu_disasm.decode(word, 0x30010)
+                code = lifter._translate(insn, dummy)
+                n += 1
+                bcctr = word >> 26 == 19 and (word >> 1) & 0x3FF == 528
+                if code.startswith("/*") or "/* cond" in code:
+                    why = "no condition"
+                elif bcctr and "ctx->ctr - 1" in code:
+                    why = "bcctr decrements CTR"
+                else:
+                    continue
+                print(f"FAIL branch {insn.mnemonic} {insn.operands} "
+                      f"(word {word:#010x}): {why}: {code}")
+                fails += 1
+
+    # mcrfs names both fields as CR fields (it printed FPRs, which lost the
+    # FPSCR field), and lifts to a copy of that FPSCR field.
+    insn = ppu_disasm.decode((63 << 26) | (7 << 23) | (4 << 18) | (64 << 1), 0x30000)
+    code = lifter._translate(insn, dummy)
+    n += 1
+    if (insn.mnemonic, insn.operands) != ("mcrfs", "cr7, cr4") or \
+            code != "ctx->crf[7] = (uint8_t)((ctx->fpscr >> 12) & 0xFu);":
+        print(f"FAIL mcrfs: {insn.mnemonic} {insn.operands} -> {code}")
+        fails += 1
+
+    # The FPSCR moves' record forms keep their lift and set CR1 (mtfsf. was
+    # reaching the TODO catch-all).
+    for word, want_mn in (((63 << 26) | (0xFF << 17) | (4 << 11) | (711 << 1) | 1, "mtfsf."),
+                          (fp_x_form(70, 5, rc=1), "mtfsb0."),
+                          ((63 << 26) | (3 << 21) | (583 << 1) | 1, "mffs.")):
+        insn = ppu_disasm.decode(word, 0x30000)
+        code = lifter._translate(insn, dummy)
+        n += 1
+        if (insn.mnemonic != want_mn or "TODO" in code
+                or "ctx->crf[1] = (uint8_t)((ctx->fpscr >> 28) & 0xFu);" not in code):
+            print(f"FAIL {want_mn}: {insn.mnemonic} {insn.operands} -> {code}")
+            fails += 1
+
+    print(f"[branches/fpscr] {n - fails} checks passed, {fails} FAILED")
+    return fails == 0
+
 # ---------------------------------------------------------------------------
 # C driver generation
 # ---------------------------------------------------------------------------
@@ -911,6 +1050,20 @@ static void check_crw(const char* name, uint32_t got, uint32_t want) {
     if (got != want) { printf("FAIL %s: CR = %08X want %08X\\n", name, got, want); g_fail++; }
     else g_pass++;
 }
+static void check_br(const char* name, int got, int want) {
+    if (got != want) {
+        printf("FAIL %s: branch %s, want %s\\n", name, got ? "taken" : "not taken",
+               want ? "taken" : "not taken");
+        g_fail++;
+    } else g_pass++;
+}
+static void check_ctr(const char* name, uint64_t got, uint64_t want) {
+    if (got != want) {
+        printf("FAIL %s: CTR = 0x%016llX want 0x%016llX\\n", name,
+               (unsigned long long)got, (unsigned long long)want);
+        g_fail++;
+    } else g_pass++;
+}
 static void check_vr(const char* name, const uint8_t* got, const uint8_t* want) {
     if (memcmp(got, want, 16) != 0) {
         printf("FAIL %s: vr =", name);
@@ -997,6 +1150,10 @@ extern "C" void vm_write32(uint64_t a, uint32_t v) { v = CONF_BSWAP32(v); memcpy
 extern "C" uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, g_vm_stub + VMOFF(a), 8); return CONF_BSWAP64(v); }
 extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy(g_vm_stub + VMOFF(a), &v, 8); }
 """)
+    # A branch case runs in a function of its own, emitted ahead of main(): a
+    # taken bclr form returns from it, a taken bc form jumps to the label after
+    # it, and falling out of the statement marks the branch not taken.
+    helpers, main_at = [], len(out)
     out.append("int main(void) {")
     out.append("    ppu_context* ctx = &g_ctx;")
     n_encoding_skipped = 0
@@ -1009,7 +1166,10 @@ extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy
                   f"{insn.mnemonic} {insn.operands} (word {c['word']:#010x}) -- case skipped")
             n_encoding_skipped += 1
             continue
-        code = lifter._translate(insn, dummy)
+        func = dummy
+        if c["exp_taken"] is not None:   # lift inside a function that holds the target
+            func = LiftedFunction(name="conf", start_addr=insn.addr, end_addr=insn.addr + 0x10)
+        code = lifter._translate(insn, func)
         if code.startswith("/*"):
             print(f"UNHANDLED by lifter: {c['name']!r} -> {code[:60]} -- case skipped")
             n_encoding_skipped += 1
@@ -1025,6 +1185,10 @@ extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy
             out.append("      ctx->xer |= (1u << 29);")
         if c["in_cr"] is not None:
             out.append(f"      ppu_cr_unpack(ctx, 0x{c['in_cr']:08X}u);")
+        if c["in_fpscr"] is not None:
+            out.append(f"      ctx->fpscr = 0x{c['in_fpscr']:08X}u;")
+        if c["in_ctr"] is not None:
+            out.append(f"      ctx->ctr = 0x{c['in_ctr']:016X}ULL;")
         for addr, blob in c["in_mem"].items():
             byts = ", ".join(f"0x{b:02X}" for b in blob)
             out.append(f"      {{ static const uint8_t _m[{len(blob)}] = {{ {byts} }}; "
@@ -1032,6 +1196,17 @@ extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy
         for reg, bits in c["in_fpr"].items():
             out.append(f"      {{ uint64_t _b = 0x{bits:016X}ULL; memcpy(&ctx->fpr[{reg}], &_b, 8); }}")
         body = [f"        {code}"]
+        if c["exp_taken"] is not None:
+            fn = f"conf_branch_{i}"
+            helpers += [f"static void {fn}(ppu_context* ctx, int* fell, int* jumped) {{",
+                        f"    {code}", "    *fell = 1; return;"]
+            if "goto loc_" in code:
+                helpers.append(f"{code.split('goto ', 1)[1].split(';', 1)[0]}: *jumped = 1;")
+            helpers.append("}")
+            body = [f"        {{ int _fell = 0, _jumped = 0; {fn}(ctx, &_fell, &_jumped); "
+                    f'check_br("{nm}", _jumped || !_fell, {int(bool(c["exp_taken"]))}); }}']
+        if c["exp_ctr"] is not None:
+            body.append(f'        check_ctr("{nm}", ctx->ctr, 0x{c["exp_ctr"]:016X}ULL);')
         for reg, val, mask in c["expects"]:
             body.append(f'        check_reg("{nm}", {reg}, ctx->gpr[{reg}], '
                         f"0x{val:016X}ULL, 0x{mask:016X}ULL);")
@@ -1142,6 +1317,7 @@ extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy
     return g_fail ? 1 : 0;
 }
 """)
+    out[main_at:main_at] = helpers
     text = "\n".join(out)
     with open(path, "w") as f:
         f.write(text)
@@ -1174,6 +1350,7 @@ def main():
 
     decode_ok = check_decode_and_lift()
     decode_ok = check_barrier_lowering() and decode_ok
+    decode_ok = check_branch_and_fpscr_lifts() and decode_ok
 
     os.makedirs(os.path.join(ROOT, "scratch"), exist_ok=True)
     cpath = os.path.join(ROOT, "scratch", "ppu_conformance.cpp")   # preamble is C++ (extern "C")
```

## 6. Tests

All new tests are in `tools/test_ppu_lift.py`, in its existing style.

- **Branch cases (C driver, 256 cases).** Every BO value (0-31) of bc (on cr7.eq, target $+8) and bclr (on cr0.gt),
  with the CR bit set and clear, and CTR 1 (reaches zero) and 2 (does not). The expectation comes from BO alone,
  through `ref_bc`, written from the ISA. Each case checks taken or not taken, CTR afterwards, and that CR is
  unchanged. That covers all four combined forms taken and not taken, and CTR reaching zero, in both bc and bclr.
- **How a branch runs in the driver.** `emit_c` lifts a branch case inside a function range that holds its target and
  emits it as a small helper ahead of `main()`. A taken bclr returns from the helper; a taken bc jumps to a label the
  helper defines; falling out of the statement marks it not taken. New `case()` fields: `in_fpscr`, `in_ctr`,
  `exp_ctr`, `exp_taken`. New C checks: `check_br`, `check_ctr`.
- **CR1 cases (C driver, 27 cases).** `fadd.`, `fmuls.`, `fmr.` and `mffs.` with FPSCR seeded to 0, 0xA0000000 and
  0x5F0F0000, and two CR words. Each checks the FP result and the whole CR: CR1 equals FPSCR's top nibble, the other
  fields are unchanged. Plain `fadd` must leave CR1 alone.
- **mcrfs cases (C driver, 30 cases).** Five (BF, BFA) pairs, three FPSCR words, two CR words. Only the CR is checked,
  since FPSCR writes are not modeled.
- **Text checks (`check_branch_and_fpscr_lifts`, 1028 checks).** Every BO value of bc (with AA and LK), bclr, bclrl,
  bcctr and bcctrl, on four BI values: none may lift to the fallback, and no bcctr form may decrement CTR. Also the
  `mcrfs` operand text and lift, and that `mtfsf.`, `mtfsb0.` and `mffs.` keep their lift and set CR1. These cover the
  `ctr` and `lrl` forms, which call `ps3_indirect_call` and so cannot run in the driver.

What ran tonight: only the pure-Python parts, from a scratch copy of the patched `tools/` (so the generated file went
to the scratch dir, not the repo). `check_decode_and_lift`, `check_barrier_lowering` and the new text checks pass;
`emit_c` writes 1727 cases with 0 skipped (1414 before); and a small Python evaluator ran the lifted condition of all
256 branch cases and matched every expected outcome and CTR. With master's lifter, the text checks fail, the mcrfs
cases are skipped as unhandled, and the branch conditions are the fallback. The two existing pure-Python branch
tests, `tools/test_ppu_branches.py` and `tools/test_ppu_fallthrough.py`, also pass against the patched lifter (run
from the same scratch copy). `tools/test_lifter_output_ascii.py:51` expects the `mtfsf` comment text, which the patch
keeps for the plain form.

What did not run: the compile and run of the C++ driver (`main()` builds it with cl or c++), and the other tests that
compile C (`tools/test_ppu_vcmp_record_cr6.py`). Run them before merging: `py -3 tools\test_ppu_lift.py`. The helper
functions and checks are plain C, but they have not met a compiler yet.

## 7. Risks for other titles

- **Combined branches change behaviour wherever they occur.** Before, every `bdnzt`/`bdnzf`/`bdzt`/`bdzf` was an
  unconditional branch that kept CTR. A title that runs fine today either has none on its live paths, or has them
  only where "always taken" happened to work (for example a loop that also has its own exit branch). After the patch
  both the count and the CR bit decide the branch. That is correct, but it is a real change in control flow, so every
  title needs a re-lift and a regress run.
- **bcctr with BO2=0** no longer decrements CTR, and its combined forms now test the CR bit instead of always
  branching. This is an invalid form that compilers do not emit; it should only appear in data decoded as code.
- **CR1** is now written by FP record forms. A title that reads CR1 after one now sees FPSCR's top nibble (0) instead
  of a stale value. Hardware would often show FX set, so a title that tests FX this way still differs from hardware.
- **mcrfs** now writes its CR field (from FPSCR, 0 in practice) instead of leaving it alone. For field 4, see the open
  point in section 3.
- **Text churn:** raw `bc`/`bclr`/`bcctr` lifts change from `/* cond: bc */ 1` to `1`. Same behaviour, but those
  output files are rewritten on the next lift.
- **The disassembler's `mcrfs` text changes.** Any tool that parsed `mcrfs fN, f0` would see `crN, crM`. Nothing in
  `tools/` does.

## 8. Commit message

The commit was not built or run when this was written, so the draft message ends with a line that says so. It follows the subject style of
`git log fork/master -- tools/ppu_lifter.py`.

```text
ppu_lifter: bdnzf-style branches test CTR, FP dot-forms set CR1, mcrfs

Three gaps a static review found. All of them predate the CR-fields change.

bdnzt, bdnzf, bdzt and bdzf, and their lr and ctr forms, reach
_branch_condition under combined names (bdnzf eq comes out as bdnzne).
It did not know them, so they fell to "/* cond */ 1": always taken, and
CTR never decremented. A loop closed by bdnzf ran on past its count.
They now decrement CTR, test it, and then test the CR bit, as Book I 2.4
and RPCS3's BC and BCLR do. CTR stays 32-bit, as mtctr writes it. The
bcctr forms drop the CTR part: BO2=0 is an invalid form there, and RPCS3
ignores it. Raw bc, bclr and bcctr, which the disassembler prints only
for "branch always", now lift to 1 without the fallback marker.

FP record forms (fadd. and the rest, plus mffs., mtfsf., mtfsfi.,
mtfsb0. and mtfsb1.) never wrote CR1, though a comment said they did.
They now copy FPSCR[FX FEX VX OX] to CR1. The lifter does not model the
FPSCR exception bits and nothing lifted writes ctx->fpscr, so CR1 gets
what mffs reads there: 0 in practice. mtfsf. no longer falls to the TODO
catch-all.

mcrfs had no handler, and the disassembler printed its fields as FPRs,
which lost the FPSCR field number. It now prints "crBF, crBFA" and lifts
to a copy of that FPSCR field into the CR field. The ISA's clear of the
copied exception bits is not lifted: FPSCR writes are not modeled, and
RPCS3 skips the clear too.

test_ppu_lift.py: every BO value of bc and bclr, with the CR bit set and
clear and a CTR that does and does not reach zero, run in helper
functions so a taken branch can be seen; CR1 cases for fadd., fmuls.,
fmr. and mffs. with a seeded FPSCR, and a check that plain fadd leaves
CR1 alone; mcrfs cases; and a text sweep over every BO value of bc,
bclr and bcctr that fails on the fallback or on a CTR decrement in
bcctr.

Any lifter change needs a re-lift and a regress run on each title before
it merges.

Not built or run yet: written statically on 2026-10-19.
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```
