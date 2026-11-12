# rand265p.py SEED N [K0 [VB]]: H.265 poke scenarios (diff265.py harness), the H.264 rand4/rand5 knobs on the
# H.265 block layout.  Inputs i265e would not produce are poked at function entry (Harness.poke, before the
# snapshot), so both sides get the same input:
#  - VIDEO_CFG: out-of-range configuration values (the H.264 rand4 list, e+0x82x.. -> e+0x39x.., e+0x89x.. -> e+0x40x..)
#  - FRAME_START: the HW revision e+0x34 (6 / 7), the size-based reference QP (inst+0x54), the SMART motion
#    level inst+0x7f, the per-picture flags e+0x30 / e+0x53
#  - FRAME_START: last picture bits / last IDR slice sizes (cur+0x88, +0x98.., +0x198, cur = e+0x34)
#  - FRAME_END: picture position st+0x398 back to 0 (a forced IDR outside the GOP grid)
#  - IDR periods that differ from the GOP (Harness.t.gop), GOP 1 / 2, random statistics
import sys, random, os, struct
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'diff265.py')).read().split("if __name__")[0])

CFGF = [(0x10, 'I', [0, 70000]), (0x18, 'I', [0, 300]), (0x14, 'I', [0, 500]), (0x1c, 'B', [60]), (0x1d, 'B', [55]),
        (0xc, 'B', [3]), (0x1e, 'B', [2]), (0x396, 'B', [9]), (0x395, 'b', [-5, 60, 0, 40]), (0x397, 'B', [0, 1, 3]),
        (0x398, 'B', [1, 4]), (0x399, 'B', [7]), (0x39c, 'B', [60]), (0x39d, 'B', [70]), (0x39e, 'B', [99]),
        (0x3a4, 'I', [0, 12, 3000]), (0x3a8, 'I', [0, 1, 0x20000]), (0x3ac, 'I', [0, 0x30000]),
        (0x3b0, 'B', [0, 20, 120, 200]), (0x3b1, 'B', [0, 150, 90]), (0x3b2, 'B', [0, 200, 5]), (0x3b3, 'B', [0, 9, 60]),
        (0x3b4, 'B', [9]), (0x3b5, 'b', [-20, 20, 5]), (0x3b6, 'H', [0, 1200, 200, 60]), (0x3b7, 'B', [0, 60]),
        (0x3b8, 'H', [0, 2000]), (0x3ba, 'B', [0, 60]), (0x3bb, 'B', [0, 60]), (0x3c0, 'i', [0, 70, -3, 40]),
        (0x3c1, 'b', [-3, 40]), (0x3c4, 'b', [-3, 40]), (0x3c5, 'b', [-3, 40]), (0x3c6, 'b', [-3, 40]),
        (0x3cc, 'i', [0xc350001 + 5, -5]), (0x3d0, 'i', [0xc350001 + 5, -7]), (0x40d, 'B', [5]), (0x410, 'I', [100]),
        (0x414, 'I', [0x20000]), (0x41c, 'I', [0xffffffff]), (0x420, 'I', [0xffffffff])]


def scen(seed, k, VB):
    r = random.Random(seed * 1000 + k)
    mode = r.choice([1, 2, 3, 4, 5, 3, 5]); kbps = r.choice([256, 500, 1000, 2000, 4000, 8000, 20000])
    w, h = r.choice([(1920, 1080), (1280, 720), (640, 360), (2560, 1440), (2304, 1296)])
    gop = r.choice([2, 10, 25, 30, 50, 60]); fps = r.choice([5, 10, 15, 25, 30])
    iqp = r.choice([-1, 25, 30, 35])
    try:
        blk = O.imp_block(LIB, mode, kbps, w, h, fps, gop, iqp)
    except RuntimeError:
        print('scen %d: vendor rejects the attributes (no channel)' % k)
        return 0
    cfgp = [f for f in CFGF if r.random() < 0.25] if r.random() < 0.4 else []
    cfgp = [(o, fm, r.choice(v)) for o, fm, v in cfgp]
    rev = r.choice([6, 6, 7, 7])
    p54 = r.choice([0, 0, 1, 1, 2]); p7f = r.choice([None, None, 0, 1, 2, 3]); p7d = r.choice([None, 0, 1])
    pk30 = r.choice([0, 0, 0.05, 0.2]); pk53 = r.choice([0, 1]); p398 = r.choice([0, 0, 0.05, 0.15])
    sizes = r.random() < 0.5

    def poke(name, H):
        L = H.L; e = EA
        if name == 'JZ_VPU_RC_VIDEO_CFG_T33':
            for o, fm, v in cfgp:
                L.wr(e + o, struct.pack('<' + fm, v))
            L.wr(e + 0x34, bytes([rev]))
        elif name == 'JZ_VPU_RC_FRAME_START_T33':
            L.wr(e + 0x34, bytes([rev]))
            if sizes:    # last picture bits, last IDR slice sizes and their sum (cur = e+0x34)
                L.w32(e + 0x34 + 0x88, r.choice([0, r.randint(1, 1 << 24)]))
                for i in range(0x40): L.w32(e + 0x34 + 0x98 + 4 * i, r.randint(0, 1 << 20))
                L.w32(e + 0x34 + 0x198, r.choice([0, r.randint(1, 1 << 24), 0x80000000 | r.randint(0, 1 << 20)]))
            inst = L.r32(e + 0x3d4)
            if inst:
                L.wr(inst + 0x54, bytes([p54]))
                if p7f is not None: L.wr(inst + 0x7f, bytes([p7f]))
                if p7d is not None: L.wr(inst + 0x7d, bytes([p7d]))
            if pk30:
                on = r.random() < pk30
                L.wr(e + 0x30, bytes([on])); L.wr(e + 0x53, bytes([pk53 if on else 0]))
        elif name == 'JZ_VPU_RC_FRAME_END_T33':
            if p398 and r.random() < p398:
                st = L.r32(e + 0x3d8)
                if st: L.w32(st + 0x398, 0)
    hh = Harness(blk, verbose=VB)
    hh.poke = poke
    idr = r.choice([gop, gop, gop * 2, max(1, gop // 2), gop + 3, max(1, gop - 1)]) if gop > 2 else gop
    hh.t.gop = idr
    pics = r.choice([60, 120, 200])
    lvl = [60.0]; still = [0]

    def cpl(i):
        if r.random() < 0.03: lvl[0] = r.choice([5.0, 20.0, 60.0, 120.0, 250.0])
        if r.random() < 0.03: still[0] = r.randint(10, 60)
        if still[0] > 0:
            still[0] -= 1; return 1.0 + r.random()
        return max(0.5, lvl[0] * (0.8 + 0.4 * r.random()))
    try:
        seq = hh.run(pics, cpl, random.Random(seed * 7 + k) if seed % 2 == 0 else None)
    except Exception as ex_:
        print('scen %d EXC %r' % (k, ex_)); seq = []
    print('scen %d mode %d %dk %dx%d gop %d idr %d fps %d init %d rev %d p54 %d p7f %s cfg %d p398 %s pics %d: '
          'calls %d diff %d fault %d qp[-5:] %s' % (k, mode, kbps, w, h, gop, idr, fps, iqp, rev, p54, p7f, len(cfgp),
                                                    p398, pics, hh.calls, hh.ndiff, hh.nfault, seq[-5:]), flush=True)
    return hh.ndiff


if __name__ == '__main__':
    seed, n = int(sys.argv[1]), int(sys.argv[2])
    K0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    VB = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    bad = sum(scen(seed, k, VB) > 0 for k in range(K0, K0 + n))
    print('TOTAL', n, 'bad', bad)
