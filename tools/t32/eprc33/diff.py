#!/usr/bin/env python3
"""differential test: vendor T33 eprc (unicorn) vs generated C (libe33.so), per entry call"""
import sys, ctypes, math, os
sys.path.insert(0, '/mnt/NVMe/git/scratch-clones/session-110bf918/wt-eprc33/tools')
import eprc_t33_oracle as O
from eprc_oracle import HEAP
from unicorn import UC_HOOK_CODE
from unicorn.mips_const import UC_MIPS_REG_RA, UC_MIPS_REG_A0

LIB = '/mnt/NVMe/git/scratch-clones/session-110bf918/audit/issue295/gh/libimp-2.2.0.so'
SO = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.environ.get('E33SO','libe33.so')))
PB, PS = HEAP, 0x180000
BB, BS = HEAP + 0x1000000, 0x10000
GB, GS = 0x1b0000, 0x80000
FRAME = HEAP + 0x8000
FN = {'eprc_default_set_H264_T33': 0, 'JZ_VPU_RC_VIDEO_CFG_H264_T33': 1,
      'JZ_VPU_RC_FRAME_START_H264_T33': 2, 'JZ_VPU_RC_FRAME_END_H264_T33': 3}


class Harness:
    def __init__(self, mode, kbps, verbose=2, **kw):
        self.pending = None
        self.ndiff = 0
        self.calls = 0
        self.verbose = verbose
        self.kw = kw
        self.mode, self.kbps = mode, kbps
        # T33.__init__ runs init; we need hooks before: build Lib manually via subclass trick
        orig = O.Lib.call
        H = self

        def call(lib, fn, *a, **k):
            if not getattr(lib, '_h33', False):
                lib._h33 = True
                for n in FN:
                    ad = lib.syms[n]
                    lib.uc.hook_add(UC_HOOK_CODE, H.enter, user_data=n, begin=ad, end=ad)
                lib.uc.hook_add(UC_HOOK_CODE, H.ret, begin=0x78000, end=0x7a000)
                H.L = lib
            return orig(lib, fn, *a, **k)
        O.Lib.call = call
        self.t = O.T33(LIB, mode, kbps, **kw)

    def snap(self):
        L = self.L
        return (bytearray(L.rd(PB, PS)), bytearray(L.rd(BB, BS)), bytearray(L.rd(GB, GS)))

    def enter(self, uc, addr, size, name):
        if self.pending:
            return
        e = uc.reg_read(UC_MIPS_REG_A0)
        assert e == PB + 0x360, hex(e)
        self.pending = (name, uc.reg_read(UC_MIPS_REG_RA), self.snap())

    def ret(self, uc, addr, size, _):
        if not self.pending or addr != self.pending[1]:
            return
        name, ra, before = self.pending
        self.pending = None
        after = self.snap()
        p, b, g = [(ctypes.c_uint8 * len(x)).from_buffer_copy(x) for x in before]
        fault = SO.e33_call(FN[name], p, PB, PS, b, BB, BS, g, GB, GS, FRAME, BB)
        mine = (bytes(p), bytes(b), bytes(g))
        self.calls += 1
        bad = []
        for (nm, base, v, m) in (('P', PB, after[0], mine[0]), ('B', BB, after[1], mine[1]), ('G', GB, after[2], mine[2])):
            if v == m:
                continue
            # P: skip the C build's frame scratch (FRAME .. +0x8000)
            rng = [(0, 0x8000), (0x10000, len(v))] if nm == 'P' else [(0, len(v))]
            d = [i for a0, a1 in rng if v[a0:a1] != m[a0:a1] for i in range(a0, a1) if v[i] != m[i]]
            if d:
                bad.append((nm, base, d, v, m))
        if fault and not bad:
            self.nfault = getattr(self, 'nfault', 0) + 1
        if bad:
            self.ndiff += 1
            if self.verbose and self.ndiff <= self.verbose:
                print('DIFF call %d %s fault=%d' % (self.calls, name, fault))
                for nm, base, d, v, m in bad:
                    # group
                    s = []
                    for i in d[:400]:
                        if s and i - s[-1][1] <= 4:
                            s[-1][1] = i
                        else:
                            s.append([i, i])
                    for a0, a1 in s[:24]:
                        a0 &= ~3
                        off = (base + a0) - (PB + 0x360) if nm == 'P' else a0
                        print('  %s+0x%x: vendor %s  mine %s' % (nm if nm != 'P' else 'E', off,
                              v[a0:a1 + 4 - (a1 & 3)].hex(), m[a0:a1 + 4 - (a1 & 3)].hex()))

    def run(self, pics, cplx_fn=None, out=None):
        t = self.t
        mbs = (O.W // 16) * (O.H // 16)
        seq = []
        for i in range(pics):
            cplx = cplx_fn(i) if cplx_fn else 60.0 * (2.0 if pics // 3 <= i < 2 * pics // 3 else 1.0)
            r, qp, typ = t.start()
            idr = i % O.GOP == 0
            nb = O.model_bytes(qp, idr, mbs, cplx)
            st = O.model_stats(qp, nb, mbs)
            if getattr(self, 'rich', None):
                rr = self.rich
                still = cplx < 3.0
                for j in (39, 40, 41, 42):
                    lo = rr.randint(0, mbs // (2 if still else 6)); hi = rr.randint(0, mbs // (2 if still else 6))
                    st[j] = (lo | hi << 16) & 0xffffffff
                for j in range(22, 39):
                    st[j] = rr.randint(0, 1 << 20)
                # regs 43 + 44: picture complexity sum (i264e_ratecontrol_end -> P+0x5b0, the
                # controller's st+0xa0 = sum * 2048 / (bit rate >> 10)); spread it over all
                # I-share rate classes (st+0xa0 / 10000 up to ~12000)
                cls = cplx / 60.0 * rr.uniform(0.3, 1.7) * 3000.0
                x = min(int(cls * 10000.0 * self.kbps / 2048.0), 0x7fffffff)
                st[43] = x // 2 + rr.randint(0, 1000)
                st[44] = max(0, x - st[43])
                # the still / motion map of P+0x260 (one byte per block, a class 0..7)
                if self.L.r32(t.P + 0x260) == HEAP + 0x30000:
                    lvl = 0 if still else min(7, int(cplx / 40.0))
                    self.L.wr(HEAP + 0x30000, bytes(min(7, max(0, lvl + rr.randint(-1, 1))) for _ in range(0x8000)))
            t.end(nb, st)
            seq.append(qp)
        return seq


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.endswith('.so')]
    mode = int(a[0]) if a else 1
    kbps = int(a[1]) if len(a) > 1 else 2000
    pics = int(a[2]) if len(a) > 2 else 60
    h = Harness(mode, kbps, verbose=int(a[3]) if len(a) > 3 else 2)
    seq = h.run(pics)
    print('mode %d kbps %d: %d calls, %d differ; qp %s' % (mode, kbps, h.calls, h.ndiff, ' '.join(map(str, seq[:20]))))
