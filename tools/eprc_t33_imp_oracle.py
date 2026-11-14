#!/usr/bin/env python3
"""T32 IMP -> i264e rate-control parameter reference (#295, development aid).

Drives the IMP layer of a vendor T32 libimp 2.2.0 under the unicorn CPU
emulator: IMP_Encoder_SetDefaultParam fills an IMPEncoderChnAttr, the attribute
is (optionally) mutated at random, channel_i264e_encoder_init turns it into the
i264e parameter block and i264e_validate_parameters clamps it, exactly as
i264e_init does before i264e_ratecontrol_init.  The attribute goes in through
IMP_Encoder_CreateChn (which sizes the stream buffer, encAttr.bufSize, that
becomes the VBV size); the emulation stops at the i264e_init entry (no HERA,
no threads).  Nothing from the vendor library is
copied; it is read from the path you pass.

    tools/eprc_t33_imp_oracle.py LIB --defaults            SetDefaultParam per mode ('D' lines)
    tools/eprc_t33_imp_oracle.py LIB --random FIRST COUNT  random attributes ('A' + 'P' lines)
    tools/eprc_t33_imp_oracle.py LIB --reconfig FIRST COUNT  + a random IMP_Encoder_SetChnAttrRcMode
                                                       at run time ('R' + 'Q' lines)

'D <profile> <mode> <initqp> <kbps> <ret> <attr hex (184 bytes)>'
'A <seed> <attr hex (184 bytes)>' then 'P <seed> <ret> <validate ret> <param hex (0x360 bytes)>'
(ret: channel_i264e_encoder_init reached i264e_init = 0, else its return code).
'R <seed> <IMPEncoderAttrRcMode hex (64 bytes)>' then 'Q <seed> <ret> <param hex>': the channel of
'A <seed>' gets this rate control by IMP_Encoder_SetChnAttrRcMode (i264e_set_param ->
i264e_reconfig_rc_set); the block is what i264e_idr_reconfig hands to i264e_ratecontrol_init at
the next IDR (after i264e_validate_parameters).
tests/t32/rc_imp_test.c reads both and checks OpenIMP's SetDefaultParam and
attribute -> parameter mapping against them.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eprc_oracle import Lib, HEAP  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.mips_const import UC_MIPS_REG_A0, UC_MIPS_REG_V0, UC_MIPS_REG_PC, UC_MIPS_REG_RA  # noqa: E402

PSIZE = 0x360           # i264e_param_default clears 0x360 bytes, i264e_init copies 0x360
ASIZE = 0xb8            # IMPEncoderChnAttr (SetDefaultParam clears 0xb8)
CHN = HEAP + 0x100000   # the channel block (IMP_Encoder_CreateChn: 0x1698 bytes, attr at +8)
TBL = HEAP + 0x400000   # g_kern_enc_chn's channel table (+0x5120) for IMP_Encoder_CreateChn
ATTR = HEAP + 0x600000  # the caller's attribute
HND = HEAP + 0x800000   # the i264e handle (i264e_init: calloc 0x41e0, the parameters at +0)
FB = HEAP + 0x900000    # the handle's current picture (+0x41d8) and its block (+0x3f0)
VP = HEAP + 0x200000    # the copy i264e_validate_parameters works on (i264e_init: calloc 0x41e0)
GOT_KERN_ENC = 0x201b24  # libimp 2.2.0 (md5 98071ecd): the GOT word with the encoder state block
H264_MAIN, H264_HIGH, H265_MAIN = 0x0100004d, 0x01000064, 0x02000001


def xorshift32(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x


class Imp:
    def __init__(self, path):
        L = self.L = Lib(path, generic=True)
        self.cap = None

        def at_init(uc, addr, size, _):
            self.cap = bytes(uc.mem_read(uc.reg_read(UC_MIPS_REG_A0), PSIZE))
            uc.emu_stop()

        def skip(uc, addr, size, _):
            uc.reg_write(UC_MIPS_REG_V0, 0)
            uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))

        # the encoder module state (GOT entry of g_kern_enc_chn's block, vendor 2.2.0)
        self.g = L.r32(GOT_KERN_ENC)
        a = L.syms['i264e_init']
        L.uc.hook_add(UC_HOOK_CODE, at_init, begin=a, end=a)
        a = L.syms['dump_encoder_chn_attr']
        L.uc.hook_add(UC_HOOK_CODE, skip, begin=a, end=a)

        def at_rcinit(uc, addr, size, _):
            self.cap = bytes(uc.mem_read(uc.reg_read(UC_MIPS_REG_A0), PSIZE))
            uc.emu_stop()

        a = L.syms['i264e_ratecontrol_init']
        L.uc.hook_add(UC_HOOK_CODE, at_rcinit, begin=a, end=a)

    def default(self, profile, mode, w, h, fn, fd, gop, bufsize, iqp, kbps):
        L = self.L
        L.wr(CHN, bytes(0x1698))
        r = L.call('IMP_Encoder_SetDefaultParam', CHN + 8, profile, mode, w, h, fn, fd, gop, bufsize,
                   iqp & 0xffffffff, kbps)
        return r, L.rd(CHN + 8, ASIZE)

    def param(self, attr):
        """IMP_Encoder_CreateChn(0) up to i264e_init + i264e_validate_parameters: (ret, vret, block)"""
        L = self.L
        # the encoder module state CreateChn needs (IMP_System_Init / CreateGroup): the
        # channel table, channel 0 not created (-1)
        g = self.g
        L.wr(TBL, bytes(0x1698))
        L.w32(TBL, 0xffffffff)
        L.w32(g + 0x511c, HEAP + 0x500000)
        L.w32(g + 0x5120, TBL)
        L.wr(ATTR, attr)
        L.brk = HEAP + 0x1000000            # CreateChn's allocations, freed each time
        self.cap = None
        try:
            r = L.call('IMP_Encoder_CreateChn', 0, ATTR)
        except RuntimeError:
            r = 0                           # stopped at i264e_init
        if self.cap is None:
            return r, 0, None
        L.wr(VP, bytes(0x41e0))
        L.wr(VP, self.cap)
        v = L.call('i264e_validate_parameters', VP)
        return 0, v, L.rd(VP, PSIZE)


    def reconfig(self, block, rc):
        """IMP_Encoder_SetChnAttrRcMode(0, rc) on the channel param() set up (its i264e handle
        holding @block), then i264e_idr_reconfig: (ret, block at i264e_ratecontrol_init)"""
        L = self.L
        L.wr(HND, bytes(0x5000))
        L.wr(HND, block)
        L.wr(FB, bytes(0x2000))
        L.w32(HND + 0x41d8, FB)
        L.w32(FB + 0x3f0, FB + 0x1000)
        L.w32(TBL + 0x372 * 4, HND)
        L.call('i264e_reconfig_init', HND)  # i264e_init: the reconfiguration copy of the parameters
        L.wr(ATTR, rc)
        r = L.call('IMP_Encoder_SetChnAttrRcMode', 0, ATTR)
        if r:
            return r, None
        self.cap = None
        try:
            L.call('i264e_idr_reconfig', HND, 0, 0, 0)
        except RuntimeError:
            pass                            # stopped at i264e_ratecontrol_init
        return 0, self.cap


def defaults(imp, out):
    for prof in (H264_MAIN, H264_HIGH, H265_MAIN):
        for mode in range(6):
            for iqp, kbps in ((30, 2000), (-1, 777), (60, 0), (0, 123457)):
                r, a = imp.default(prof, mode, 1920, 1080, 25, 1, 50, 0, iqp, kbps)
                out.write('D %08x %d %d %d %x %s\n' % (prof, mode, iqp, kbps, r, a.hex()))


# field layout of the T32 rate-control attributes (include/imp/imp_encoder.h):
# (offset in the union, struct format, kind); kinds pick the random values
F_QP = ((0, 'B', 'qp'), (1, 'B', 'qp'), (2, 'B', 'bool'), (3, 'B', 'qp'), (4, 'B', 'qp'), (5, 'B', 'qp'))
FIELDS = {
    1: F_QP + ((8, 'I', 'kbps'), (0xc, 'b', 'bias'), (0xe, 'H', 'step'), (0x10, 'H', 'step'),
              (0x14, 'I', 'time'), (0x18, 'B', 'lvl'), (0x1c, 'I', 'lvl'), (0x20, 'I', 'pct'),
              (0x24, 'I', 'pct'), (0x28, 'I', 'size'), (0x2c, 'I', 'size')),
    2: F_QP + ((8, 'I', 'kbps'), (0xc, 'b', 'bias'), (0x10, 'I', 'pct'), (0x14, 'I', 'time'),
              (0x18, 'I', 'lvl'), (0x1c, 'I', 'pct'), (0x20, 'I', 'pct'), (0x24, 'I', 'size'),
              (0x28, 'I', 'size')),
    3: F_QP + ((8, 'I', 'kbps'), (0xc, 'b', 'bias'), (0x10, 'I', 'pct'), (0x14, 'I', 'time'),
              (0x18, 'I', 'lvl'), (0x1c, 'I', 'pct'), (0x20, 'I', 'pct'), (0x24, 'B', 'pct'),
              (0x25, 'B', 'qp'), (0x26, 'B', 'bool'), (0x27, 'B', 'lvl'), (0x28, 'B', 'lvl'),
              (0x29, 'B', 'pct'), (0x2c, 'I', 'size'), (0x30, 'I', 'size')),
    4: F_QP + ((8, 'I', 'kbps'), (0xc, 'I', 'kbps'), (0x10, 'I', 'kbps'), (0x14, 'b', 'bias'),
              (0x18, 'I', 'pct'), (0x1c, 'I', 'time'), (0x20, 'I', 'time'), (0x24, 'I', 'lvl'),
              (0x28, 'I', 'pct'), (0x2c, 'I', 'pct'), (0x30, 'H', 'size'), (0x34, 'I', 'size'),
              (0x38, 'I', 'size')),
    5: F_QP + ((8, 'I', 'kbps'), (0xc, 'b', 'bias'), (0x10, 'I', 'pct'), (0x14, 'I', 'time'),
              (0x18, 'I', 'lvl'), (0x1c, 'I', 'pct'), (0x20, 'I', 'pct'), (0x24, 'B', 'pct'),
              (0x25, 'B', 'qp'), (0x28, 'I', 'size'), (0x2c, 'I', 'size')),
}
RANGES = dict(qp=(0, 60), bool=(0, 2), kbps=(1, 20000), bias=(-14, 14), step=(0, 20), time=(0, 130),
              lvl=(0, 12), pct=(0, 130), size=(0, 20000))


def mutate(attr, x):
    """randomise every rate-control field: keep / 0 / in range / raw"""
    a = bytearray(attr)
    mode = struct.unpack_from('<I', a, 0x3c)[0]
    for off, fmt, kind in FIELDS.get(mode, ()):
        x = xorshift32(x)
        pick = x % 100
        if pick < 30:
            continue
        x = xorshift32(x)
        if pick < 55:
            v = 0
        elif pick < 90:
            lo, hi = RANGES[kind]
            v = lo + x % (hi - lo + 1)
        else:
            n = struct.calcsize(fmt)
            v = x & ((1 << (8 * n)) - 1)
            if fmt.islower() and v >= 1 << (8 * n - 1):
                v -= 1 << (8 * n)
        struct.pack_into('<' + fmt, a, 0x40 + off, v)
    return bytes(a), x


def random_cases(imp, first, count, out, reconfig=False):
    for seed in range(first, first + count):
        x = xorshift32((seed * 2654435761) & 0xffffffff or 1)
        p = []
        for _ in range(8):
            x = xorshift32(x)
            p.append(x)
        prof = (H264_MAIN, H264_HIGH)[p[0] & 1]
        mode = 1 + p[1] % 5
        w, h = ((1920, 1080), (1280, 720), (640, 360), (2560, 1440), (2304, 1296), (352, 288))[p[2] % 6]
        fps = (5, 10, 15, 20, 25, 30)[p[3] % 6]
        gop = (1, 10, 25, 30, 50, 60, 120)[p[4] % 7]
        iqp = (-1, 0, 25, 30, 35, 51, 60)[p[5] % 7]
        kbps = (64, 256, 500, 1000, 2000, 4000, 8000, 15000)[p[6] % 8]
        x = xorshift32(x)
        bufsize = 0 if x % 4 else (x >> 8) % 3000000
        r, a = imp.default(prof, mode, w, h, fps, 1 + (p[7] & 1), gop, bufsize, iqp, kbps)
        a, x = mutate(a, x)
        r, v, blk = imp.param(a)
        out.write('A %d %s\n' % (seed, a.hex()))
        out.write('P %d %x %x %s\n' % (seed, r, v, blk.hex() if blk else '-'))
        if not reconfig or blk is None:
            continue
        # a new rate control of a random mode: SetDefaultParam's, mutated
        x = xorshift32(x)
        mode2 = 1 + x % 5
        x = xorshift32(x)
        r2, a2 = imp.default(prof, mode2, w, h, fps, 1, gop, 0, iqp, (64, 500, 2000, 8000)[x % 4])
        a2, x = mutate(a2, x)
        rc = a2[0x3c:0x7c]
        r2, blk2 = imp.reconfig(blk, rc)
        out.write('R %d %s\n' % (seed, rc.hex()))
        out.write('Q %d %x %s\n' % (seed, r2, blk2.hex() if blk2 else '-'))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    imp = Imp(sys.argv[1])
    if sys.argv[2] == '--defaults':
        defaults(imp, sys.stdout)
    elif sys.argv[2] == '--random':
        random_cases(imp, int(sys.argv[3]), int(sys.argv[4]), sys.stdout)
    elif sys.argv[2] == '--reconfig':
        random_cases(imp, int(sys.argv[3]), int(sys.argv[4]), sys.stdout, reconfig=True)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
