#!/usr/bin/env python3
"""T32 rate-control reference runs (#295, development aid).

Runs the H.264 rate control of a vendor T32 libimp 2.2.0 (the "eprc" T33
revision: i264e_param_default, i264e_ratecontrol_init / _start /
_is_reenc, which drive eprc_default_set_H264_T33 and JZ_VPU_RC_*_H264_T33)
under the unicorn CPU emulator on a synthetic picture-size model and prints
the picture type / QP / size sequence.  The same model drives OpenIMP's T32
controller (tests/t32/rc_test.c 'trace'), so the two sequences can be
compared.  Nothing from the vendor library is copied; it is read from the
path you pass.  The emulator has no HERA: statistics are synthetic.

    tools/eprc_t33_oracle.py LIB MODE [KBPS] [PICTURES]      'F' lines
    tools/eprc_t33_oracle.py LIB --compare MODE KBPS PICS OPENIMP_TRACE
    tools/eprc_t33_oracle.py LIB --defaults                  i264e defaults
    tools/eprc_t33_oracle.py LIB --scenarios FIRST COUNT     random scenarios ('S' + 'F' lines)

MODE: the T32 IMPEncoderRcMode (1 CBR, 2 VBR, 3 SMART, 4 CVBR, 5 AVBR).
i264e parameter offsets (vendor 2.2.0, from i264e_param_default and the
i264e_ratecontrol_init mapping): +0x2c GOP, +0x3c/+0x40 size, +0xdc mode,
+0xec/+0xf0 min/max QP, +0xf8 initial QP, +0xfc/+0x100 max/min I QP,
+0x110 bit rate (kbit/s), +0x2a4/+0x2a8 frame rate.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eprc_oracle import Lib, HEAP  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402

W, H, GOP, FPS = 1920, 1088, 50, 25


def model_bytes(qp, idr, mbs, cplx):
    """the size model of tests/t32/rc_test.c (bits = cplx * MBs * 2^((26-qp)/6), I 5x)"""
    bits = cplx * mbs * math.pow(2.0, (26.0 - qp) / 6.0) * (5.0 if idr else 1.0)
    return int(bits / 8.0) + 16


def model_stats(qp, nbytes, mbs):
    st = [0] * 48
    st[8] = (qp * mbs) & 0xffff
    st[10] = mbs & 0xffff
    st[12] = nbytes * 3
    st[15] = mbs * 4
    st[20] = nbytes // 2
    st[21] = nbytes // 3
    return [v & 0xffffffff for v in st]


class T33:
    def __init__(self, path, mode, kbps, rev=6, w=W, h=H, gop=GOP, fps=FPS, minqp=15, maxqp=48,
                 initqp=30):
        L = self.L = Lib(path, generic=True)
        lo = L.syms['get_history_iqp'] if 'get_history_iqp' in L.syms else 0
        hi = L.syms['librc_h264_get_version'] + 0x100
        # the OEM unaligned-access fix-up (Lib._track) over the controller
        L.code = {a: L.r32(a) for a in range(lo & ~3, hi & ~3, 4)}
        L.uc.hook_add(UC_HOOK_CODE, L._track, begin=lo & ~3, end=hi & ~3)
        self.P = HEAP; self.Hh = HEAP + 0x10000; self.X = HEAP + 0x14000
        self.S = HEAP + 0x15000; self.TB = HEAP + 0x16000; self.REG = HEAP + 0x18000
        L.wr(HEAP, bytes(0x40000))
        P = self.P
        L.call('i264e_param_default', P)
        if mode is None:
            return
        L.w32(P + 4, rev)
        L.w32(P + 0xc20, 1); L.w32(P + 0xc24, self.REG)     # the HERA register window (no /dev/mem)
        L.w32(P + 0x2c, gop); L.w32(P + 0x3c, w); L.w32(P + 0x40, h)
        L.w32(P + 0x2a4, fps); L.w32(P + 0x2a8, 1)
        L.w32(P + 0xdc, mode); L.w32(P + 0x110, kbps)
        L.w32(P + 0xec, minqp); L.w32(P + 0xf0, maxqp); L.w32(P + 0xf8, initqp)
        L.w32(P + 0xfc, maxqp); L.w32(P + 0x100, minqp)
        if mode == 4:                                     # CVBR long-term bounds
            L.w32(P + 0x178, kbps); L.w32(P + 0x17c, kbps // 4)
        L.call('i264e_ratecontrol_init', P)
        Hh = self.Hh
        L.w32(Hh + 0x1540, P); L.w32(Hh + 0x3f0, self.X); L.w32(Hh + 0x400, self.S)
        L.w32(Hh + 0xbac, self.TB)
        L.w32(self.X + 0x4c, 0xffffffff)                  # no FRAME_REPEATE_JUDGE (no re-encoding)
        # the i264e slice block the controller fills (Hh+0xf9c -> P+0x6cc); without it the
        # controller writes through a NULL pointer into low memory
        self.SL = HEAP + 0x20000
        L.w32(Hh + 0xf9c, self.SL)
        self.n = 0

    def start(self):
        L = self.L
        L.w32(self.Hh, self.n)
        r = L.call('i264e_ratecontrol_start', self.Hh)
        return r, L.r32(self.Hh + 0x354), L.rd(self.P + 0xb71, 1)[0]

    def end(self, nbytes, regs):
        L = self.L
        for i, v in enumerate(regs):
            L.w32(self.TB + 8 * i + 4, v)
        L.w32(self.Hh + 0xfa8, nbytes); L.w32(self.Hh + 0x350, 0)
        L.call('i264e_ratecontrol_is_reenc', self.Hh)
        self.n += 1


def run(path, mode, kbps, pics, out=sys.stdout):
    t = T33(path, mode, kbps)
    mbs = (W // 16) * (H // 16)
    seq = []
    for i in range(pics):
        cplx = 60.0 * (2.0 if pics // 3 <= i < 2 * pics // 3 else 1.0)
        r, qp, typ = t.start()
        idr = i % GOP == 0
        nb = model_bytes(qp, idr, mbs, cplx)
        t.end(nb, model_stats(qp, nb, mbs))
        seq.append((i, 2 if idr else 0, qp, nb))
        if out:
            print('F %d %d %d %d' % seq[-1], file=out)
    return seq


def xorshift32(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x


class Scene:
    """deterministic complexity sequence (scene cuts, still periods); tests/t32/rc_test.c has the same"""
    LEVELS = (5.0, 20.0, 60.0, 120.0, 250.0)

    def __init__(self, seed):
        self.x = seed or 1
        self.lvl = 60.0
        self.still = 0

    def next(self):
        self.x = u = xorshift32(self.x)
        if u % 100 < 3:
            self.lvl = self.LEVELS[(u >> 8) % 5]
        if (u >> 16) % 100 < 2:
            self.still = 10 + (u >> 20) % 50
        if self.still > 0:
            self.still -= 1
            return 1.0 + ((u >> 4) & 255) / 256.0
        return max(0.5, self.lvl * (0.8 + 0.4 * ((u >> 12) & 1023) / 1024.0))


def scenario(path, seed):
    """one random scenario: 'S' header line and its 'F' lines"""
    x = xorshift32(seed * 2654435761 & 0xffffffff or 1)
    pick = []
    for _ in range(10):
        x = xorshift32(x)
        pick.append(x)
    mode = 1 + pick[0] % 5
    kbps = (256, 500, 1000, 2000, 3000, 4000, 6000, 8000)[pick[1] % 8]
    w, h = ((1920, 1088), (1280, 720), (640, 368), (2560, 1440), (2304, 1296))[pick[2] % 5]
    gop = (10, 25, 30, 50, 60)[pick[3] % 5]
    fps = (10, 15, 20, 25, 30)[pick[4] % 5]
    minqp = (10, 15, 20, 25)[pick[5] % 4]
    maxqp = (38, 42, 45, 48, 51)[pick[6] % 5]
    initqp = (25, 30, 35)[pick[7] % 3]
    pics = (80, 150, 250)[pick[8] % 3]
    t = T33(path, mode, kbps, w=w, h=h, gop=gop, fps=fps, minqp=minqp, maxqp=maxqp, initqp=initqp)
    sc = Scene(pick[9])
    mbs = (w // 16) * (h // 16)
    out = ['S %d %d %d %d %d %d %d %d %d %d %u' % (mode, kbps, w, h, gop, fps, minqp, maxqp, initqp, pics, pick[9])]
    for i in range(pics):
        cplx = sc.next()
        r, qp, typ = t.start()
        idr = i % gop == 0
        nb = model_bytes(qp, idr, mbs, cplx)
        t.end(nb, model_stats(qp, nb, mbs))
        out.append('F %d %d %d %d' % (i, 2 if idr else 0, qp, nb))
    return out


def summary(name, seq):
    bits = sum(s[3] for s in seq) * 8
    p = [s[2] for s in seq if s[1] != 2]
    i = [s[2] for s in seq if s[1] == 2]
    return '%-8s %6.0f kbit/s  P qp %d..%d (mean %.1f)  I qp %d..%d' % (
        name, bits / 1000.0 / (len(seq) / FPS), min(p), max(p), sum(p) / len(p), min(i), max(i))


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[1] == '--defaults':
        t = T33(a[0], None, 0)
        print(' '.join('+0x%x=%d' % (o, t.L.r32(t.P + o)) for o in range(0, 0x360, 4) if t.L.r32(t.P + o)))
        return
    if len(a) >= 4 and a[1] == '--scenarios':
        for k in range(int(a[2]), int(a[2]) + int(a[3])):
            print('\n'.join(scenario(a[0], k)), flush=True)
        return
    if len(a) >= 6 and a[1] == '--compare':
        mode, kbps, pics = int(a[2]), int(a[3]), int(a[4])
        v = run(a[0], mode, kbps, pics, out=None)
        o = [tuple(int(x) for x in l.split()[1:]) for l in open(a[5]) if l.startswith('F ')]
        n = min(len(v), len(o))
        d = [abs(v[k][2] - o[k][2]) for k in range(n)]
        print(summary('vendor', v))
        print(summary('openimp', o[:n]))
        print('QP |diff| mean %.2f max %d, equal %d/%d' % (sum(d) / n, max(d), d.count(0), n))
        return
    mode = int(a[1]); kbps = int(a[2]) if len(a) > 2 else 2000; pics = int(a[3]) if len(a) > 3 else 250
    run(a[0], mode, kbps, pics)


if __name__ == '__main__':
    main()
