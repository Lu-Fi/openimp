#!/usr/bin/env python3
"""T32 H.265 rate-control reference runs (#295, development aid).

Runs the H.265 rate control of a vendor T32 libimp 2.2.0 (the "eprc" T33
revision: I265eRatectrl_rcInit / _rcStart / _rcEnd, which drive
eprc_default_set_T33 and JZ_VPU_RC_*_T33) under the unicorn CPU emulator on a
synthetic picture-size model and prints the picture type / QP / size
sequence.  The i265e parameter block is made by the vendor IMP layer
(IMP_Encoder_SetDefaultParam + IMP_Encoder_CreateChn up to i265e_init, see
tools/eprc_t33_imp_oracle.py) and run through the vendor I265e_configure, as
I265e_create does before I265eRatectrl_rcInit.  Nothing from the vendor
library is copied; it is read from the path you pass.  The emulator has no
HERA: statistics are synthetic.

    tools/eprc_t33_h265_oracle.py LIB MODE [KBPS] [PICTURES]   'F' lines
    tools/eprc_t33_h265_oracle.py LIB --scenarios FIRST COUNT  random scenarios ('S' + 'F' lines)

MODE: the T32 IMPEncoderRcMode (1 CBR, 2 VBR, 3 SMART, 4 CVBR, 5 AVBR).
'F picture type qp bytes' (type 2 = I).

Layout (vendor 2.2.0, I265e_create / I265e_rcStart / I265eRatectrl_*): the
handle H (calloc 0x26e0) holds the parameters at +0 (0x378 bytes) and the
rate control at +0x2030 (its controller block E at +0x58, the register
window pointer at +0x490); I265e_rcStart passes H+0x390, whose +0x28 is the
current picture F: F+0 the parameters, F+0x778 the slice type (2 I), F+0x77c
the picture QP, F+0xd8c the HERA statistics (register i at +8 i + 4),
F+0x1120 the picture bytes, F+0xd9c the slice block the controller fills,
F+0x2210.. the per-CTU maps, F+0x2378.. the VPU buffers (the first is the
work buffer VPI_set_sliceinfo lays its maps out in).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eprc_oracle import Lib, HEAP  # noqa: E402
from eprc_t33_oracle import Scene, xorshift32  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402

W, H, GOP, FPS = 1920, 1080, 50, 25
PSIZE = 0x378
RC = 0x2030            # the rate control in the handle
E = RC + 0x58          # the controller block


def model_bytes(qp, idr, ctus, cplx):
    """bits = cplx * 4 * CTUs * 2^((26-qp)/6), I 5x (a 64x64 CTU = 16 MBs, H.265 ~ 0.25 of H.264)"""
    bits = cplx * 4.0 * ctus * 2.0 ** ((26.0 - qp) / 6.0) * (5.0 if idr else 1.0)
    return int(bits / 8.0) + 16


def model_stats(qp, nbytes, ctus, rr=None):
    """HERA statistics registers (synthetic; I265eRatectrl_rcEnd reads 0..8, 29, 30, 32, 33)"""
    st = [0] * 48
    st[0] = qp * ctus * 16
    st[1] = ctus * 16
    st[2] = nbytes * 3
    st[3] = (nbytes // 2) | (nbytes // 3) << 16
    st[4] = ctus * 4
    for j in (5, 6, 7, 8):
        st[j] = (ctus * 2) | (ctus * 3) << 16
    st[29] = nbytes * 6
    st[30] = nbytes * 2
    st[32] = (ctus * 16) << 11 | 3 << 30
    st[33] = qp * ctus * 4
    if rr is not None:
        for j in range(len(st)):
            if rr.random() < 0.5:
                st[j] = rr.randint(0, 1 << rr.choice((8, 16, 24, 31)))
    return [v & 0xffffffff for v in st]


class T33H265:
    def __init__(self, path, block, rows=False):
        L = self.L = Lib(path, generic=True)
        lo = L.syms['librc_h264_get_version']
        hi = L.syms['librc_h265_get_version'] + 0x100
        # the OEM unaligned-access fix-up (Lib._track) over the controller
        L.code = {a: L.r32(a) for a in range(lo & ~3, hi & ~3, 4)}
        L.uc.hook_add(UC_HOOK_CODE, L._track, begin=lo & ~3, end=hi & ~3)

        def abort(uc, addr, size, _):
            raise RuntimeError('vendor abort (picture type mismatch?)')
        a = L.syms['abort'] if 'abort' in L.syms else None
        if a:
            L.uc.hook_add(UC_HOOK_CODE, abort, begin=a, end=a)
        # HEAP+0x8000..0x10000 stays free (the scratch of the C differential build)
        self.H = HEAP; self.F = HEAP + 0x10000; self.TB = HEAP + 0x14000
        self.REG = HEAP + 0x18000
        self.SL = HEAP + 0x20000; self.MAPS = HEAP + 0x40000
        L.wr(HEAP, bytes(0x40000))
        Hh = self.H
        L.wr(Hh, block[:PSIZE])
        L.w32(Hh + 0x370, 0)                       # no parameter callback
        L.call('I265e_configure', Hh)
        L.w32(Hh + RC + 0x48c, 1)                  # register window open (no /dev/mem)
        L.w32(Hh + RC + 0x490, self.REG)
        L.call('I265eRatectrl_rcInit', Hh + RC, Hh)
        L.w32(Hh + 0x390 + 0x28, self.F)
        L.w32(self.F, Hh)
        L.w32(self.F + 0x363 * 4, self.TB)
        # the slice block the controller fills (F+0xd9c -> rc+0x234; without it the controller
        # writes through a NULL pointer) and the per-CTU maps (F+0x2210.. -> rc+0x90..0x9c)
        L.w32(self.F + 0x367 * 4, self.SL)
        for k in range(4):
            L.w32(self.F + (0x884 + k) * 4, self.MAPS + 0x4000 * k)
        # the VPU buffers (F+0x2378.. -> rc+0x344..0x35c); the first is the work buffer the slice
        # block maps are laid out in (VPI_set_sliceinfo: about 0x60 bytes per 32x32 block + maps)
        self.WB = HEAP + 0x100000
        for k in range(7):
            L.w32(self.F + (0x8de + k) * 4, self.WB if k == 0 else HEAP + 0x170000 + 0x1000 * k)
        self.gop = L.r32(Hh + 0x20)
        self.ctus = ((L.r32(Hh + 0x2c) + 63) // 64) * ((L.r32(Hh + 0x30) + 63) // 64)
        self.n = 0
        self.rows = rows

    def start(self):
        L = self.L
        idr = self.n % self.gop == 0 if self.gop else self.n == 0
        L.w32(self.F + 0x1de * 4, 2 if idr else 1)
        L.call('I265eRatectrl_rcStart', self.H + RC, self.H + 0x390, self.H + 0x378)
        return idr

    def qp(self):
        """the picture QP the controller chose (rcStart: F+0x77c = rc+0x3d8)"""
        return self.L.r32(self.F + 0x1df * 4)

    def end(self, nbytes, regs, rows=()):
        L = self.L
        for i, v in enumerate(regs):
            L.w32(self.TB + 8 * i + 4, v)
        for i, v in enumerate(rows):
            L.w32(self.REG + (0x201 + 2 * i) * 4, v)
        L.w32(self.F + 0x448 * 4, nbytes)
        L.call('I265eRatectrl_rcEnd', self.H + RC, self.H + 0x390)
        self.n += 1


def imp_block(path, mode, kbps, w=W, h=H, fps=FPS, gop=GOP, iqp=30, mutate=None):
    from eprc_t33_imp_oracle import Imp, H265_MAIN
    imp = Imp(path)
    L = imp.L
    cap = {}

    def at_init(uc, addr, size, _):
        cap['p'] = bytes(uc.mem_read(uc.reg_read(UC_MIPS_REG_A0), PSIZE))
        uc.emu_stop()
    from unicorn.mips_const import UC_MIPS_REG_A0
    a = L.syms['i265e_init']
    L.uc.hook_add(UC_HOOK_CODE, at_init, begin=a, end=a)
    r, attr = imp.default(H265_MAIN, mode, w, h, fps, 1, gop, 0, iqp, kbps)
    if mutate:
        attr = mutate(attr)
    imp.param(attr)
    if 'p' not in cap:
        raise RuntimeError('vendor channel set-up failed')
    return cap['p']


def run(path, mode, kbps, pics, out=sys.stdout):
    t = T33H265(path, imp_block(path, mode, kbps))
    seq = []
    for i in range(pics):
        cplx = 60.0 * (2.0 if pics // 3 <= i < 2 * pics // 3 else 1.0)
        idr = t.start()
        qp = t.qp()
        nb = model_bytes(qp, idr, t.ctus, cplx)
        t.end(nb, model_stats(qp, nb, t.ctus))
        seq.append((i, 2 if idr else 1, qp, nb))
        if out:
            print('F %d %d %d %d' % seq[-1], file=out)
    return seq


def scenarios(path, first, count, out=sys.stdout):
    """'S k mode kbps w h gop fps iqp pictures' + 'F' lines; complexity from Scene(k + 1)"""
    for k in range(first, first + count):
        x = xorshift32((k * 2654435761 + 7) & 0xffffffff or 1)
        p = []
        for _ in range(7):
            x = xorshift32(x)
            p.append(x)
        mode = 1 + p[0] % 5
        kbps = (256, 500, 1000, 2000, 4000, 8000)[p[1] % 6]
        w, h = ((1920, 1080), (1280, 720), (640, 360), (2560, 1440), (2304, 1296))[p[2] % 5]
        gop = (10, 25, 30, 50, 60)[p[3] % 5]
        fps = (10, 15, 20, 25, 30)[p[4] % 5]
        iqp = (-1, 25, 30, 35)[p[5] % 4]
        pics = (60, 120, 200)[p[6] % 3]
        t = T33H265(path, imp_block(path, mode, kbps, w, h, fps, gop, iqp))
        sc = Scene(k + 1)
        print('S %d %d %d %d %d %d %d %d %d' % (k, mode, kbps, w, h, gop, fps, iqp, pics), file=out)
        for i in range(pics):
            cplx = sc.next()
            idr = t.start()
            qp = t.qp()
            nb = model_bytes(qp, idr, t.ctus, cplx)
            t.end(nb, model_stats(qp, nb, t.ctus))
            print('F %d %d %d %d' % (i, 2 if idr else 1, qp, nb), file=out)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    if a[1] == '--scenarios':
        scenarios(a[0], int(a[2]), int(a[3]))
        return
    run(a[0], int(a[1]), int(a[2]) if len(a) > 2 else 2000, int(a[3]) if len(a) > 3 else 60)


if __name__ == '__main__':
    main()
