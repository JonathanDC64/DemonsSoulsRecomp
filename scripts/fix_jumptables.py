"""Restore the jump-table cases our 2026-07-01 PPU lift dropped.

The lift in work/recomp predates the lifter's jump-table fixes. For a GCC PIC
switch (`lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr` with the table of
table-relative words right after the bctr) it often recovered only the FIRST
case, emitted the table as `/* TODO: .word */` lines, and left the remaining
case bodies as unlabelled code after it. Every other case then falls into
`default: ps3_indirect_call` -> "[ppu] unresolved indirect call" into the
middle of a function. HighGraphics' command encoder (func_00ACA890, table at
0x00ACAEA4) hit exactly that after the GCM fix.

A full re-lift with the current lifter would fix this but changes other
semantics and does not boot yet (docs/24), so this patches the old lift in
place. The old lifter emits exactly one C line per PPC instruction, so every
line can be given an address, anchored on `loc_XXXXXXXX:` labels and
`ctx->cia = 0x...;` call lines; an anchor that disagrees with the count marks
the stretch unreliable and nothing derived from it is used.

Run from the repo root:  python scripts/fix_jumptables.py work/recomp [--dry]
Idempotent: cases already present are not added twice.
"""
import glob
import re
import sys
import time

FUNC = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')
CIA = re.compile(r'^\s*ctx->cia = 0x([0-9A-F]{8});')
SWITCH = re.compile(r'^\s*switch \(\(uint32_t\)ctx->ctr\) \{')
CASE = re.compile(r'case 0x([0-9A-F]{8})u: goto loc_([0-9A-F]{8});')
WORD = re.compile(r'/\* TODO: \.word 0x([0-9A-F]{8}) \*/;')


def is_insn(s):
    t = s.strip()
    if not t or t == '}' or t.startswith('uint64_t _cs_') or t.startswith('DS_FRAME('):
        return False
    if LABEL.match(s) or t.startswith('case ') or t.startswith('default:'):
        return False
    if SWITCH.match(s):
        return True          # the bctr
    return True


def fix_function(lines, start, end, stats):
    """lines[start] is the `void func_...{` line, lines[end] its closing '}'."""
    fa = int(FUNC.match(lines[start]).group(1), 16)
    addr_of = {}             # line index -> address (reliable lines only)
    line_at = {}             # address -> line index
    labels = set()
    addr = fa
    i = start + 1
    in_switch = False
    while i < end:
        s = lines[i]
        m = LABEL.match(s)
        if m:
            a = int(m.group(1), 16)
            labels.add(a)
            if addr is not None and addr != a:
                stats['mismatch'] += 1
            addr = a
            i += 1
            continue
        if in_switch:
            if s.strip() == '}':
                in_switch = False
            i += 1
            continue
        if not is_insn(s):
            i += 1
            continue
        m = CIA.match(s)
        if m:
            a = int(m.group(1), 16)
            if addr is not None and addr != a:
                stats['mismatch'] += 1
                addr = None
                # Invalidate back to the previous anchor: we cannot tell where
                # the count went wrong.
            addr = a
        if addr is not None:
            addr_of[i] = addr
            line_at.setdefault(addr, i)
            addr += 4
        if SWITCH.match(s):
            in_switch = True
        i += 1
    # Anchors confirm the count; require one at or after a target's stretch.
    edits = []               # (switch line index, [new case lines], {label addr: line idx})
    i = start + 1
    while i < end:
        if not SWITCH.match(lines[i]):
            i += 1
            continue
        j = i + 1
        have = set()
        while j < end and lines[j].strip() != '}':
            m = CASE.search(lines[j])
            if m:
                have.add(int(m.group(1), 16))
            j += 1
        k = j + 1
        words = []
        while k < end and WORD.search(lines[k]):
            words.append((k, int(WORD.search(lines[k]).group(1), 16)))
            k += 1
        stats['switches'] += 1
        if not words or words[0][0] not in addr_of:
            i = k
            continue
        base = addr_of[words[0][0]]
        # Sanity: the recovered case(s) must be base-relative entries.
        rel = [(base + (w if w < 0x80000000 else w - 0x100000000)) & 0xFFFFFFFF
               for _, w in words]
        if have and not have & set(rel):
            stats['skip_not_relative'] += 1
            i = k
            continue
        new_cases = []
        new_labels = {}
        for t in sorted(set(rel)):
            if t in have:
                continue
            if t not in line_at or not (fa <= t):
                stats['target_unresolved'] += 1
                continue
            new_cases.append(t)
            if t not in labels:
                new_labels[t] = line_at[t]
        if new_cases:
            edits.append((i, new_cases, new_labels))
            stats['cases_added'] += len(new_cases)
            stats['switches_fixed'] += 1
        i = k
    return edits


def apply_edits(lines, all_edits):
    """Apply in descending line order so indices stay valid."""
    inserts = []             # (line index, text) inserted BEFORE that line
    for sw_line, cases, labs in all_edits:
        indent = lines[sw_line][:len(lines[sw_line]) - len(lines[sw_line].lstrip())]
        text = [f'{indent}case 0x{t:08X}u: goto loc_{t:08X};  /* fix_jumptables */'
                for t in cases]
        inserts.append((sw_line + 1, text))
        for t, li in labs.items():
            inserts.append((li, [f'loc_{t:08X}:']))
    for li, text in sorted(inserts, key=lambda x: -x[0]):
        lines[li:li] = text


def write_retry(p, s):
    for _ in range(30):
        try:
            with open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                f.write(s)
            return True
        except PermissionError:
            time.sleep(2)
    return False


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'work/recomp'
    dry = '--dry' in sys.argv
    total = {}
    for p in sorted(glob.glob(root + '/ppu_recomp_*.cpp')):
        src = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
        nl = '\r\n' if '\r\n' in src else '\n'
        lines = src.split(nl)
        stats = dict(switches=0, switches_fixed=0, cases_added=0, mismatch=0,
                     target_unresolved=0, skip_not_relative=0)
        edits = []
        i = 0
        while i < len(lines):
            if FUNC.match(lines[i]):
                j = i + 1
                while j < len(lines) and lines[j] != '}':
                    j += 1
                edits += fix_function(lines, i, j, stats)
                i = j + 1
            else:
                i += 1
        if edits and not dry:
            apply_edits(lines, edits)
            if not write_retry(p, nl.join(lines)):
                print('FAILED write', p)
        print(p, stats)
        for k, v in stats.items():
            total[k] = total.get(k, 0) + v
    print('TOTAL', total)


if __name__ == '__main__':
    main()
