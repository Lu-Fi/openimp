#!/usr/bin/env python3
"""T33 software OSD reference runs (#675, development aid).

Runs the CPU OSD drawing of a vendor T33 libimp 2.0.2.1 (md5 818b0f11) under
the unicorn CPU emulator on random NV12 frames and regions and prints one line
per case: the inputs and an FNV-1a hash of the frame afterwards.
tests/t33/osd_sw_test.c redraws every case with OpenIMP's
src/t32/openimp_t33_osd_sw.c and compares the hashes.  Nothing from the vendor
library is copied; it is read from the path you pass.

    tools/t33_osd_oracle.py LIB SEED COUNT > tests/t33/vendor-2.0.2.1-osd-sw.txt

Kinds:
  P  osd_draw_pic (exported, 0x170108): NV12 and the seven RGB formats
  C  the cover of the OSD group update, type-1 (T33) path, run from its case
     label 0x170d7c to the common tail 0x170a30 with the update's registers
  B  the bitmap of the same update, 0x17105c .. 0x170a30

Line: KIND w h x0 y0 x1 y1 fmt offx offy sx sy aen falpha colour fseed dseed rc hash
(sx/sy as float bit patterns; frame bytes and picture bytes from xorshift32 of
fseed / dseed, see fill()).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eprc_oracle import Lib, HEAP  # noqa: E402
from unicorn.mips_const import (UC_MIPS_REG_GP, UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2,  # noqa: E402
                                UC_MIPS_REG_SP, UC_MIPS_REG_F20)

FRAME, RGN, GRP, VIRT, PIC = HEAP + 0x100, HEAP + 0x400, HEAP + 0x800, HEAP + 0x10000, HEAP + 0x800000
GP = 0x20c160                       # gp of the OSD update (0x170794)
COVER_PC, BITMAP_PC, TAIL_PC = 0x170d7c, 0x17105c, 0x170a30
FMTS = (10, 14, 15, 16, 17, 19, 21, 25)


def xs(s):
    s ^= (s << 13) & 0xffffffff
    s ^= s >> 17
    s ^= (s << 5) & 0xffffffff
    return s


def fill(seed, n, zero_share=0):
    """n bytes of xorshift32(seed) (seed 0 -> 1); zero_share/256 of them forced 0"""
    s = seed or 1
    out = bytearray(n)
    for i in range(n):
        s = xs(s)
        v = s & 0xff
        if zero_share and (s >> 8) & 0xff < zero_share:
            v = 0
        out[i] = v
    return bytes(out)


def fnv(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h


def al16(v):
    return (v + 15) & ~15


def fbits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def setup(lib, c):
    w, h = c['w'], c['h']
    size = al16(w) * al16(h) * 3 // 2
    lib.wr(VIRT, fill(c['fseed'], size))
    lib.wr(PIC, fill(c['dseed'], 0x40000, 64 if c['kind'] == 'B' else 0))
    lib.wr(FRAME, bytes(0x40))
    lib.w32(FRAME + 8, w)
    lib.w32(FRAME + 12, h)
    lib.w32(FRAME + 0x1c, VIRT)
    lib.wr(RGN, bytes(0x110))
    for i, v in enumerate((0, c['x0'], c['y0'], c['x1'], c['y1'], 0, 0, c['fmt'])):
        lib.w32(RGN + 4 * i, v)
    lib.w32(RGN + 0x20, c['colour'] if c['kind'] == 'C' else PIC)
    lib.wr(GRP, bytes(0x40))
    for i, v in enumerate((1, c['offx'], c['offy'], c['sx'], c['sy'], c['aen'], c['falpha'], 0, 0)):
        lib.w32(GRP + 4 * i, v)
    return size


def run(lib, c):
    size = setup(lib, c)
    uc = lib.uc
    if c['kind'] == 'P':
        rc = lib.call('osd_draw_pic', FRAME, RGN, GRP)
    else:
        sp = 0x5ff80000
        lib.wr(sp, bytes(256))
        lib.w32(sp + 40, GP)
        lib.w32(sp + 68, 1)                         # system_get_cpu_type() = 1 (T33)
        lib.w32(sp + 88, lib.r32(GP - 32720))       # the float constant base (1.0 at -9788)
        lib.w32(sp + 72, lib.r32(GP - 32636))
        uc.reg_write(UC_MIPS_REG_SP, sp)
        uc.reg_write(UC_MIPS_REG_GP, GP)
        uc.reg_write(UC_MIPS_REG_S0, RGN)
        uc.reg_write(UC_MIPS_REG_S1, FRAME)
        uc.reg_write(UC_MIPS_REG_S2, GRP - 8)
        uc.reg_write(UC_MIPS_REG_F20, fbits(1.0))
        uc.emu_start(COVER_PC if c['kind'] == 'C' else BITMAP_PC, TAIL_PC, count=200_000_000)
        rc = 0
    return rc & 0xffffffff, fnv(lib.rd(VIRT, size) + lib.rd(RGN, 0x20) + lib.rd(GRP, 0x24))


def cases(seed, count):
    s = seed or 1

    def r(n):
        nonlocal s
        s = xs(s)
        return s % n
    for i in range(count):
        kind = 'PPPCB'[r(5)]
        w = 16 * (1 + r(8)) + (r(2) * r(16))
        h = 2 * (4 + r(40)) + r(2)
        x0, y0 = r(w + 8) - 4, r(h + 8) - 4
        x1, y1 = x0 + r(40) - 2, y0 + r(30) - 2
        scales = (0.0, 1.0, 1.0, 1.0, 0.5, 1.5, 0.75)
        c = dict(kind=kind, w=w, h=h, x0=x0, y0=y0, x1=x1, y1=y1,
                 fmt=FMTS[r(len(FMTS))] if kind == 'P' else 0,
                 offx=r(9) - 4, offy=r(9) - 4,
                 sx=fbits(scales[r(len(scales))]), sy=fbits(scales[r(len(scales))]),
                 aen=r(2), falpha=(0, 1, 64, 128, 200, 255, 300)[r(7)],
                 colour=(0xffffffff, 0xff000000, 0xffff0000, 0xff00ff00, 0xff0000ff, 0x12345678, 0)[r(7)],
                 fseed=s, dseed=xs(s) ^ i)
        if kind == 'B':
            c['sx'] = c['sy'] = fbits(1.0)
            c['x0'], c['x1'] = c['x0'] & 0xffffffff, c['x1'] & 0xffffffff
        yield c


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    lib = Lib(sys.argv[1], generic=True)
    print('# vendor libimp 2.0.2.1 T33 software OSD (tools/t33_osd_oracle.py LIB %s %s)' % (sys.argv[2], sys.argv[3]))
    print('# KIND w h x0 y0 x1 y1 fmt offx offy sx sy aen falpha colour fseed dseed rc hash')
    for c in cases(int(sys.argv[2], 0), int(sys.argv[3], 0)):
        rc, hsh = run(lib, c)
        print('%s %d %d %d %d %d %d %d %d %d %08x %08x %d %d %08x %08x %08x %08x %08x' % (
            c['kind'], c['w'], c['h'], c['x0'] if c['x0'] < 0x80000000 else c['x0'] - (1 << 32),
            c['y0'], c['x1'] if c['x1'] < 0x80000000 else c['x1'] - (1 << 32), c['y1'], c['fmt'],
            c['offx'], c['offy'], c['sx'], c['sy'], c['aen'], c['falpha'], c['colour'],
            c['fseed'], c['dseed'], rc, hsh))
    return 0


if __name__ == '__main__':
    sys.exit(main())
