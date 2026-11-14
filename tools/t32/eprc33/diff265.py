#!/usr/bin/env python3
"""differential test (H.265): vendor T33 eprc (unicorn, tools/eprc_t33_h265_oracle.py) vs a C build
(E33SO, e33_call(fn, ..., e)), per entry call; memory compared byte for byte (handle / picture /
slice block / maps, the controller block B, the library globals)"""
import sys, ctypes, os, random
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import eprc_t33_h265_oracle as O
from eprc_oracle import HEAP
from unicorn import UC_HOOK_CODE
from unicorn.mips_const import UC_MIPS_REG_RA, UC_MIPS_REG_A0

LIB = os.environ.get('E33LIB', '/mnt/NVMe/git/scratch-clones/session-110bf918/audit/issue295/gh/libimp-2.2.0.so')
SO = ctypes.CDLL(os.path.abspath(os.environ.get('E33SO', 'libe265.so')))
PB, PS = HEAP, 0x180000
BB, BS = HEAP + 0x1000000, 0x10000
GB, GS = 0x1b0000, 0x80000
FRAME = HEAP + 0x8000
EA = HEAP + O.E
FN = {'eprc_default_set_T33': 0, 'JZ_VPU_RC_VIDEO_CFG_T33': 1,
      'JZ_VPU_RC_FRAME_START_T33': 2, 'JZ_VPU_RC_FRAME_END_T33': 3}


class Harness:
    def __init__(self, block, verbose=2):
        self.pending = None
        self.ndiff = self.calls = self.nfault = 0
        self.verbose = verbose
        self.poke = None
        orig = O.Lib.call
        H = self

        def call(lib, fn, *a, **k):
            if not getattr(lib, '_h33', False) and fn == 'I265eRatectrl_rcInit':
                lib._h33 = True
                for n in FN:
                    ad = lib.syms[n]
                    lib.uc.hook_add(UC_HOOK_CODE, H.enter, user_data=n, begin=ad, end=ad)
                lib.uc.hook_add(UC_HOOK_CODE, H.ret, begin=0x83000, end=0x85000)
                H.L = lib
            return orig(lib, fn, *a, **k)
        O.Lib.call = call
        try:
            self.t = O.T33H265(LIB, block)
        finally:
            O.Lib.call = orig

    def snap(self):
        L = self.L
        return (bytearray(L.rd(PB, PS)), bytearray(L.rd(BB, BS)), bytearray(L.rd(GB, GS)))

    def enter(self, uc, addr, size, name):
        if self.pending:
            return
        assert uc.reg_read(UC_MIPS_REG_A0) == EA, hex(uc.reg_read(UC_MIPS_REG_A0))
        # the vendor's bump allocator: the state block (the first calloc, at configuration) stays at B as the
        # C build assumes; the per-picture scratch (vpi_emc_qpt_cfg's padded ROI map) goes past the compared ranges
        if name in ('JZ_VPU_RC_FRAME_START_T33', 'JZ_VPU_RC_FRAME_END_T33'):
            self.L.brk = max(self.L.brk, HEAP + 0x2000000)
        if self.poke:
            self.poke(name, self)
        self.pending = (name, uc.reg_read(UC_MIPS_REG_RA), self.snap())

    def ret(self, uc, addr, size, _):
        if not self.pending or addr != self.pending[1]:
            return
        name, ra, before = self.pending
        self.pending = None
        after = self.snap()
        p, b, g = [(ctypes.c_uint8 * len(x)).from_buffer_copy(x) for x in before]
        fault = SO.e33_call(FN[name], p, PB, PS, b, BB, BS, g, GB, GS, FRAME, BB, EA)
        mine = (bytes(p), bytes(b), bytes(g))
        self.calls += 1
        bad = []
        for (nm, base, v, m) in (('P', PB, after[0], mine[0]), ('B', BB, after[1], mine[1]), ('G', GB, after[2], mine[2])):
            if v == m:
                continue
            rng = [(0, 0x8000), (0x10000, len(v))] if nm == 'P' else [(0, len(v))]
            d = [i for a0, a1 in rng if v[a0:a1] != m[a0:a1] for i in range(a0, a1) if v[i] != m[i]]
            if d:
                bad.append((nm, base, d, v, m))
        if fault:
            self.nfault += 1
        if bad:
            self.ndiff += 1
            if self.verbose and self.ndiff <= self.verbose:
                print('DIFF call %d %s fault=%d' % (self.calls, name, fault))
                for nm, base, d, v, m in bad:
                    s = []
                    for i in d[:400]:
                        if s and i - s[-1][1] <= 4:
                            s[-1][1] = i
                        else:
                            s.append([i, i])
                    for a0, a1 in s[:24]:
                        a0 &= ~3
                        off = (base + a0) - EA if nm == 'P' else a0
                        print('  %s%+#x: vendor %s  mine %s' % (nm if nm != 'P' else 'E', off,
                              v[a0:a1 + 4 - (a1 & 3)].hex(), m[a0:a1 + 4 - (a1 & 3)].hex()))

    def run(self, pics, cplx_fn=None, rich=None):
        t = self.t
        seq = []
        for i in range(pics):
            cplx = cplx_fn(i) if cplx_fn else 60.0 * (2.0 if pics // 3 <= i < 2 * pics // 3 else 1.0)
            idr = t.start()
            qp = t.qp()
            nb = O.model_bytes(qp, idr, t.ctus, cplx)
            st = O.model_stats(qp, nb, t.ctus, rich)
            rows = None
            if rich is not None and idr:
                n = max(1, t.ctus // 30)
                rows = [rich.randint(0, nb * 8 // n + 1) for _ in range(n)]
            t.end(nb, st, rows or ())
            seq.append(qp)
        return seq


def scenario(seed, k, VB=1):
    r = random.Random(seed * 1000 + k)
    mode = r.choice([1, 2, 3, 4, 5]); kbps = r.choice([256, 500, 1000, 2000, 3000, 4000, 6000, 8000])
    w, h = r.choice([(1920, 1080), (1280, 720), (640, 360), (2560, 1440), (2304, 1296)])
    gop = r.choice([10, 25, 30, 50, 60]); fps = r.choice([10, 15, 20, 25, 30])
    iqp = r.choice([-1, 25, 30, 35])
    mut = None
    if seed >= 3:
        # seed 3+: every rate-control attribute of the mode randomised as tools/eprc_t33_imp_oracle.py
        # --random does (keep / 0 / in range / raw), through the vendor IMP checks
        import eprc_t33_imp_oracle as I
        mut = lambda a: I.mutate(a, (seed * 2654435761 + k * 40503) & 0xffffffff or 1)[0]
    try:
        blk = O.imp_block(LIB, mode, kbps, w, h, fps, gop, iqp, mutate=mut)
    except RuntimeError:
        print('scen %d: vendor rejects the attributes (no channel)' % k)
        return 0
    hh = Harness(blk, verbose=VB)
    pics = r.choice([80, 150, 250])
    lvl = [60.0]; still = [0]

    def cpl(i):
        if r.random() < 0.03: lvl[0] = r.choice([5.0, 20.0, 60.0, 120.0, 250.0])
        if r.random() < 0.02: still[0] = r.randint(10, 60)
        if still[0] > 0:
            still[0] -= 1; return 1.0 + r.random()
        return max(0.5, lvl[0] * (0.8 + 0.4 * r.random()))
    seq = hh.run(pics, cpl, random.Random(seed * 7 + k) if seed % 2 == 0 or seed >= 3 else None)
    print('scen %d mode %d %dk %dx%d gop %d fps %d init %d pics %d: calls %d diff %d fault %d qp[-5:] %s' % (
        k, mode, kbps, w, h, gop, fps, iqp, pics, hh.calls, hh.ndiff, hh.nfault, seq[-5:]), flush=True)
    return hh.ndiff


if __name__ == '__main__':
    seed, n = int(sys.argv[1]), int(sys.argv[2])
    K0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    VB = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    bad = sum(scenario(seed, k, VB) > 0 for k in range(K0, K0 + n))
    print('TOTAL', n, 'bad', bad)
