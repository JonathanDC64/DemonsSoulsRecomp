"""Fix VMX lane byte order in our 2026-07-01 PPU lift.

The old lifter keeps ctx->vr as RAW big-endian guest bytes: lvx/stvx are
plain 16-byte memcpys, and vperm/lvsl/vsldoi/logic ops work on that layout.
But its lane arithmetic -- vmaddfp, vaddfp, vrsqrtefp, vcmp*fp, vcfsx,
vctsxs, vadduwm, vslw, vspltisw, ... -- casts the register to float/int lanes
in HOST (little-endian) order. Every VMX float result is therefore computed
on byte-swapped inputs. Demon's Souls' static ctor func_00935688 builds a
projection matrix with VMX and stored 0x0000C0FF (the x86 default NaN,
byte-reversed) into all 16 lanes; the boot screen's vertex shader multiplies
by it and every quad collapses -> black frames.

Upstream fixed this in the lifter (1e30602: ppu_vldf4/ppu_vstf4). The current
lifter doesn't boot this title yet (docs/24 section 8), so this patches the old
lift in place, as fix_jumptables.py does for switch tables:

  * Each emitted block `{ T* x = (T*)&ctx->vr[N]; ... body ... }` whose lane
    type T is float/int32_t/uint32_t/uint16_t and whose body never touches
    ctx->vr directly is rewritten to load every operand into a local lane
    array with byte swapping, run the SAME body, and store `d` back swapped.
    uint8_t/uint64_t blocks (perm, logic, select) are byte-order agnostic and
    are left alone, as are the splat/merge forms that copy whole lanes.
  * vctsxs gets PEM saturation (the plain cast is UB for NaN/out-of-range).
  * lvewx/stvewx place the word at lane (EA & 15), not always lane 0.
  * vmrghw: emitted as a HALFWORD merge (`"h" in mn[4:]` is true for "hw").
    DeS has 1500 vmrghw and no vmrghh, so every uint16 high merge is
    rewritten as the word merge. This is half of the matrix-transpose idiom.
  * vrfin (`vmx_x522`), vsumsws and vcmpgtuh were never lowered (TODO
    comments that left vD stale); they get real implementations.

The old lift's FMA operand order is correct (checked against the encoding at
0x009357D0: vmaddfp vD=13,vA=6,vB=7,vC=13 is emitted as d=a*c+b with b=v7).

Run from the repo root:  python scripts/fix_vmx_byteorder.py work/recomp [--dry]
Idempotent: rewritten blocks no longer match, and the header helper is
guarded by a marker.
"""
import glob
import os
import re
import sys

LANE = {"float": 4, "int32_t": 4, "uint32_t": 4, "uint16_t": 8}

# A whole emitted block on one line: declarations, then a body.
DECL = re.compile(r'(float|int32_t|uint32_t|uint16_t)\* ([a-d]) ?= ?\(\1\*\)&ctx->vr\[(\d+)\]; ')
BLOCK = re.compile(r'\{ ((?:(?:float|int32_t|uint32_t|uint16_t)\* [a-d] ?= ?\((?:float|int32_t|uint32_t|uint16_t)\*\)&ctx->vr\[\d+\]; )+)(.*) \}$')

LVEWX = re.compile(r'\{ uint64_t ea = (\([^;]*\)|ctx->gpr\[\d+\]); memset\(&ctx->vr\[(\d+)\], 0, 16\); '
                   r'memcpy\(&ctx->vr\[\2\], vm_base \+ \(uint32_t\)ea, 4\); \}$')
STVEWX = re.compile(r'\{ uint64_t ea = (\([^;]*\)|ctx->gpr\[\d+\]); '
                    r'memcpy\(vm_base \+ \(uint32_t\)ea, &ctx->vr\[(\d+)\], 4\); \}$')

CTS_OLD = 'd[i]=(int32_t)(b[i]);'
CTS_NEW = ('d[i]=(b[i]!=b[i])?0:(b[i]>=2147483648.0f)?0x7FFFFFFF:'
           '(b[i]<-2147483648.0f)?(int32_t)0x80000000:(int32_t)b[i];')

HELPER_MARK = '/* fix_vmx_byteorder helpers */'
HELPER = HELPER_MARK + r'''
#include <string.h>
static inline void ppu_vl32(const void* v, void* o) { uint32_t t[4]; memcpy(t, v, 16);
    for (int i = 0; i < 4; i++) t[i] = __builtin_bswap32(t[i]); memcpy(o, t, 16); }
static inline void ppu_vs32(void* v, const void* o) { uint32_t t[4]; memcpy(t, o, 16);
    for (int i = 0; i < 4; i++) t[i] = __builtin_bswap32(t[i]); memcpy(v, t, 16); }
static inline void ppu_vl16(const void* v, void* o) { uint16_t t[8]; memcpy(t, v, 16);
    for (int i = 0; i < 8; i++) t[i] = __builtin_bswap16(t[i]); memcpy(o, t, 16); }
static inline void ppu_vs16(void* v, const void* o) { uint16_t t[8]; memcpy(t, o, 16);
    for (int i = 0; i < 8; i++) t[i] = __builtin_bswap16(t[i]); memcpy(v, t, 16); }
'''


# vmrghw lifted as a HALFWORD merge: the lifter picked the element size with
# `"h" in mn[4:]`, which is true for "hw". DeS has 1500 vmrghw and no vmrghh
# at all (counted in the EBOOT), so every uint16 high merge here is a vmrghw.
# It is half of the matrix-transpose idiom, so every transposed matrix had
# rows like 0x80003F80 (a -0.0 / 1.0 halfword interleave).
MRGHW = re.compile(r'\{ uint16_t tmp\[8\]; uint16_t\* a=\(uint16_t\*\)&ctx->vr\[(\d+)\]; '
                   r'uint16_t\* b=\(uint16_t\*\)&ctx->vr\[(\d+)\]; for\(int i=0;i<4;i\+\+\) '
                   r'\{ tmp\[i\*2\]=a\[0\+i\]; tmp\[i\*2\+1\]=b\[0\+i\]; \} memcpy\(&ctx->vr\[(\d+)\], tmp, 16\); \}$')

# Ops the old lifter never lowered: they became comments and left vD stale.
TODO_OP = re.compile(r'/\* TODO: (vmx_x522|vsumsws|vcmpgtuh) v(\d+), v(\d+), v(\d+) \*/;$')


def lower_todo(op, vd, va, vb):
    if op == 'vmx_x522':   # vrfin: round to FP integer, nearest-even
        return (f'{{ float b[4]; ppu_vl32(&ctx->vr[{vb}], b); for(int i=0;i<4;i++) b[i]=nearbyintf(b[i]); '
                f'ppu_vs32(&ctx->vr[{vd}], b); }}')
    if op == 'vsumsws':    # sum of vA's 4 signed words + vB word 3, saturated, into word 3
        return (f'{{ int32_t a[4], b[4], o[4] = {{0,0,0,0}}; ppu_vl32(&ctx->vr[{va}], a); ppu_vl32(&ctx->vr[{vb}], b); '
                f'int64_t acc = (int64_t)a[0]+a[1]+a[2]+a[3]+b[3]; '
                f'if (acc > 2147483647LL) acc = 2147483647LL; if (acc < -2147483648LL) acc = -2147483648LL; '
                f'o[3] = (int32_t)acc; ppu_vs32(&ctx->vr[{vd}], o); }}')
    if op == 'vcmpgtuh':
        return (f'{{ uint16_t a[8], b[8], d[8]; ppu_vl16(&ctx->vr[{va}], a); ppu_vl16(&ctx->vr[{vb}], b); '
                f'for(int i=0;i<8;i++) d[i]=a[i]>b[i]?0xFFFFu:0; ppu_vs16(&ctx->vr[{vd}], d); }}')
    return None


def rewrite(line):
    s = line.strip()
    m = MRGHW.match(s)
    if m:
        indent = line[:len(line) - len(line.lstrip())]
        return (f'{indent}{{ uint32_t tmp[4]; uint32_t* a=(uint32_t*)&ctx->vr[{m.group(1)}]; '
                f'uint32_t* b=(uint32_t*)&ctx->vr[{m.group(2)}]; for(int i=0;i<2;i++) '
                f'{{ tmp[i*2]=a[0+i]; tmp[i*2+1]=b[0+i]; }} memcpy(&ctx->vr[{m.group(3)}], tmp, 16); }}\n')
    m = TODO_OP.match(s)
    if m:
        indent = line[:len(line) - len(line.lstrip())]
        return indent + lower_todo(m.group(1), *m.group(2, 3, 4)) + '\n'
    m = BLOCK.match(s)
    if m:
        decls, body = m.group(1), m.group(2)
        if 'ctx->vr' in body or 'd' not in [x[1] for x in DECL.findall(decls)]:
            return None
        ds = DECL.findall(decls)
        if CTS_OLD in body:
            body = body.replace(CTS_OLD, CTS_NEW)
        pre, post = [], ''
        for ty, name, reg in ds:
            n = LANE[ty]
            w = '16' if ty == 'uint16_t' else '32'
            pre.append(f'{ty} {name}[{n}]; ppu_vl{w}(&ctx->vr[{reg}], {name});')
            if name == 'd':
                post = f' ppu_vs{w}(&ctx->vr[{reg}], d);'
        indent = line[:len(line) - len(line.lstrip())]
        return f'{indent}{{ {" ".join(pre)} {body}{post} }}\n'
    m = LVEWX.match(s)
    if m:
        indent = line[:len(line) - len(line.lstrip())]
        return (f'{indent}{{ uint64_t ea = {m.group(1)} & ~3ULL; memset(&ctx->vr[{m.group(2)}], 0, 16); '
                f'memcpy((uint8_t*)&ctx->vr[{m.group(2)}] + (ea & 15), vm_base + (uint32_t)ea, 4); }}\n')
    m = STVEWX.match(s)
    if m:
        indent = line[:len(line) - len(line.lstrip())]
        return (f'{indent}{{ uint64_t ea = {m.group(1)} & ~3ULL; '
                f'memcpy(vm_base + (uint32_t)ea, (const uint8_t*)&ctx->vr[{m.group(2)}] + (ea & 15), 4); }}\n')
    return None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'work/recomp'
    dry = '--dry' in sys.argv
    hdr = os.path.join(root, 'ppu_recomp.h')
    h = open(hdr, encoding='latin-1', newline='').read()
    if HELPER_MARK not in h:
        if not dry:
            open(hdr, 'w', encoding='latin-1', newline='').write(h.rstrip('\r\n') + '\n\n' + HELPER)
        print('header: helpers added')
    total = 0
    for path in sorted(glob.glob(os.path.join(root, 'ppu_recomp_*.cpp'))):
        # The lift is CRLF and not valid UTF-8 (stray bytes in string
        # comments); latin-1 round-trips every byte, newline='' keeps CRLF.
        lines = open(path, encoding='latin-1', newline='').readlines()
        n = 0
        for i, ln in enumerate(lines):
            if 'ctx->vr[' not in ln and 'TODO: v' not in ln:
                continue
            body = ln.rstrip('\r\n')
            r = rewrite(body)
            if r is not None:
                lines[i] = r.rstrip('\n') + ln[len(body):]
                n += 1
        if n and not dry:
            open(path, 'w', encoding='latin-1', newline='').writelines(lines)
        total += n
        print(f'{os.path.basename(path)}: {n} block(s)')
    print(f'total {total} block(s){" (dry run)" if dry else ""}')


if __name__ == '__main__':
    main()
