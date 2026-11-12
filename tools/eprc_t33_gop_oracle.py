#!/usr/bin/env python3
"""T32 IMP frame-rate / GOP glue reference (#295, development aid).

Runs the IMP layer of a vendor T32 libimp 2.2.0 under the unicorn CPU
emulator: IMP_Encoder_CreateChn (up to i264e_init) sets up an H.264 channel,
then per picture the vendor encoder_get_fps_mask (called by
on_encoder_group_data_update before every picture) with IMP_Encoder_SetChnFrmRate
/ IMP_Encoder_SetGOPSize called at random pictures.  i264e_set_param is
hooked: parameter 5 (frame rate) and 7 (GOP) are what the encoder gets
(i264e_reconfig_fps_set / _gop_set, taken at the next IDR by
i264e_idr_reconfig with i264e_ratecontrol_init); channel +0x14c is the
"next picture is an IDR" flag the group thread hands to the picture
(+0xddc) and clears.  Nothing from the vendor library is copied; it is read
from the path you pass.

    tools/eprc_t33_gop_oracle.py LIB FIRST COUNT
    (tests/t32/vendor-2.2.0-gop33.txt.gz: LIB 0 300 | gzip -9)

'S <seed> <src num> <src den> <fps num> <fps den> <gop>' (the channel as created; the
source rate is the FrameSource channel's), 'C <gop> <fps num> <fps den>' (after
CreateChn: what the channel starts with), then per picture
'P <pic> <call> <set5 num/den or -> <set7 gop or -> <idr flag> <gop> <num> <den>'
with <call> 'F<n>/<d>' (SetChnFrmRate before the picture), 'G<g>' (SetGOPSize) or '-'.
tests/t32/gop33_test.c checks OpenIMP's src/t32/t32_gop33.h against them.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eprc_oracle import Lib, HEAP  # noqa: E402
from eprc_t33_imp_oracle import TBL, ATTR, ASIZE, HND, GOT_KERN_ENC, H264_HIGH, xorshift32  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.mips_const import UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_V0, UC_MIPS_REG_PC, UC_MIPS_REG_RA  # noqa: E402

GD = HEAP + 0xa00000    # the group's frame data handed to encoder_get_fps_mask


class Glue:
    def __init__(self, path):
        L = self.L = Lib(path, generic=True)
        self.g = L.r32(GOT_KERN_ENC)
        self.sets = []

        def set_param(uc, addr, size, _):
            pid = uc.reg_read(UC_MIPS_REG_A1)
            a = uc.reg_read(UC_MIPS_REG_A2)
            self.sets.append((pid, struct.unpack('<3I', bytes(uc.mem_read(a, 12)))))
            ret(uc, 0)

        def ret(uc, v):
            uc.reg_write(UC_MIPS_REG_V0, v)
            uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))

        # i264e_init: no encoder, a handle; the rest of CreateChn runs (threads are stubs)
        for name, fn in (('i264e_set_param', set_param),
                         ('i264e_init', lambda uc, a, s, _: ret(uc, HND)),
                         ('dump_encoder_chn_attr', lambda uc, a, s, _: ret(uc, 0))):
            a = L.syms[name]
            L.uc.hook_add(UC_HOOK_CODE, fn, begin=a, end=a)

    def create(self, fn, fd, gop, kbps=1000):
        """IMP_Encoder_SetDefaultParam + IMP_Encoder_CreateChn(0): CreateChn's return"""
        L, g = self.L, self.g
        L.wr(ATTR, bytes(ASIZE))
        L.call('IMP_Encoder_SetDefaultParam', ATTR, H264_HIGH, 1, 1280, 720, fn, fd, gop, 0, 30, kbps)
        L.wr(TBL, bytes(0x1698))
        L.w32(TBL, 0xffffffff)
        L.wr(HEAP + 0x500000, bytes(0x1000))
        L.w32(g + 0x511c, HEAP + 0x500000)
        L.w32(g + 0x5120, TBL)
        L.wr(HND, bytes(0x5000))
        L.brk = HEAP + 0x1000000
        return L.call('IMP_Encoder_CreateChn', 0, ATTR)

    def f(self, i): return self.L.r32(TBL + 4 * i)
    def setf(self, i, v): self.L.w32(TBL + 4 * i, v)

    def picture(self, sn, sd):
        L = self.L
        L.wr(GD, bytes(0x100))
        L.w32(GD + 0x28, 1000)              # the same timestamp: no rate check by time
        L.w32(GD + 0x30, sn)
        L.w32(GD + 0x34, sd)
        self.sets = []
        L.call('encoder_get_fps_mask', TBL, GD)
        idr = self.f(0x53)
        self.setf(0x53, 0)                  # the group thread hands it to the picture
        n = self.f(0x42) + 1                # pictures coded (+0x108, 64 bit)
        self.setf(0x42, n)
        return idr


def scenario(g, seed, out):
    x = xorshift32(seed * 2654435761 + 1)

    def rnd(n):
        nonlocal x
        x = xorshift32(x)
        return x % n

    src = [(25, 1), (30, 1), (15, 1), (20, 1), (30000, 1001), (24, 1)][rnd(6)]
    fn, fd = [(25, 1), (15, 1), (10, 1), (30, 1), (12, 1), (5, 1), (25, 2)][rnd(7)]
    gop = [30, 25, 50, 60, 1, 7, 100, 2 * fn // fd + 1][rnd(8)]
    out.append('S %d %d %d %d %d %d' % (seed, src[0], src[1], fn, fd, gop))
    r = g.create(fn, fd, gop)
    if r:
        out.append('X create %x' % r)
        return
    out.append('C %d %d %d' % (g.f(0x10), g.f(0xe), g.f(0xf)))
    for pic in range(int(60 + rnd(60))):
        call = '-'
        k = rnd(10)
        if k == 0:
            n, d = [(10, 1), (5, 1), (15, 1), (25, 1), (30, 1), (12, 1), (7, 2), (20, 1), (1, 1)][rnd(9)]
            g.L.w32(GD + 0x40, n)
            g.L.w32(GD + 0x44, d)
            r = g.L.call('IMP_Encoder_SetChnFrmRate', 0, GD + 0x40)
            call = 'F%d/%d:%x' % (n, d, r)
        elif k == 1:
            v = [25, 30, 50, 1, 7, 10, 60, 100, 13][rnd(9)]
            g.L.w32(GD + 0x40, v)
            r = g.L.call('IMP_Encoder_SetGOPSize', 0, GD + 0x40)
            call = 'G%d:%x' % (v, r)
        idr = g.picture(*src)
        s5 = s7 = '-'
        for pid, a in g.sets:
            if pid == 5:
                s5 = '%d/%d' % (a[1], a[2])
            elif pid == 7:
                s7 = '%d' % a[0]
        out.append('P %d %s %s %s %d %d %d %d' % (pic, call, s5, s7, idr, g.f(0x10), g.f(0xe), g.f(0xf)))


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    g = Glue(sys.argv[1])
    for seed in range(int(sys.argv[2]), int(sys.argv[2]) + int(sys.argv[3])):
        out = []
        scenario(g, seed, out)
        print('\n'.join(out), flush=True)


if __name__ == '__main__':
    main()
