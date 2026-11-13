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
        if mode == 0 and x:
            iqp = 30
    except RuntimeError:
        print('scen %d: vendor rejects the attributes (no channel)' % k)
        return 0
    cfgp = [f for f in CFGF if r.random() < 0.25] if r.random() < 0.4 else []
    cfgp = [(o, fm, r.choice(v)) for o, fm, v in cfgp]
    rev = r.choice([6, 6, 7, 7])
    p54 = r.choice([0, 0, 1, 1, 2]); p7f = r.choice([None, None, 0, 1, 2, 3]); p7d = r.choice([None, 0, 1])
    pk30 = r.choice([0, 0, 0.05, 0.2]); pk53 = r.choice([0, 1]); p398 = r.choice([0, 0, 0.05, 0.15])
    sizes = r.random() < 0.5
    # seeds >= 3: FRAME_START knobs on top (own random stream, so seeds 1 / 2 stay as they were):
    # FIXQP (mode 0), the still map (cur+0x320.. -> the 64x64 CTU vote), motion mode cur+0x340 / 0x341,
    # forced intra cur+0x342 / hold 0x344, intra refresh e+0x35d with cur+0x32a .. 0x338
    x = random.Random(seed * 1000 + k + 777777) if seed >= 3 else None
    if x:
        if x.random() < 0.2:
            mode = 0
        still = x.random() < 0.5
        sbs = x.choice([8, 12, 16, 32]); scl = x.choice([2, 3, 4]); shy = x.choice([0, 1])   # <= 4 classes (4 counters)
        mmode = x.choice([None, 0, 1, 2, 3, 4]); m41 = x.choice([0, 1])
        fint = x.choice([0, 0, 0.03, 0.1]); hold = x.choice([-1, 0, 2, 5])
        refr = x.random() < 0.4
        rf = (x.choice([0, 1]), x.choice([0, 1, 3, 12, 20]), x.choice([0, 1]), x.choice([0, 1, 3, 8]),
              x.choice([0, 2, 5, 30, 70000]))
        # slice-block knobs (own stream, the x stream above unchanged): the ROI QP table cur+0x2ac / map
        # cur+0x2b0 (one byte per 16x16 block, vpi_emc_qpt_cfg) and the motion search cur+0x348 / level 0x349
        y = random.Random(seed * 1000 + k + 999999)
        qpt = y.random() < 0.3; mce = y.choice([None, None, 0, 1, 2, 3, 4, 5])
        if w * h > 1280 * 720:   # the slice block per 32x32 unit is slow under the emulator's per-instruction hook
            qpt, mce = False, None

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
            if x:
                c = e + 0x34
                if still:
                    m = HEAP + 0xa0000; bw, bh = w // sbs, h // sbs
                    if L.r32(c + 0x320) != m:
                        L.wr(m, bytes(x.randint(0, scl) for _ in range(bw * bh)))
                    L.wr(c + 0x324, bytes([1, sbs, scl, shy])); L.w32(c + 0x320, m)
                    if x.random() < 0.05:   # the map changes
                        L.wr(m + x.randint(0, bw * bh - 64), bytes(x.randint(0, scl) for _ in range(64)))
                if mmode is not None:
                    L.wr(c + 0x340, bytes([mmode, m41]))
                if fint:
                    L.wr(c + 0x342, bytes([x.random() < fint])); L.w32(c + 0x344, hold & 0xffffffff)
                if refr:
                    L.wr(e + 0x35d, b'\x01'); L.wr(c + 0x32a, bytes([rf[0]])); L.w32(c + 0x32c, rf[1])
                    L.wr(c + 0x330, bytes([rf[2]])); L.w32(c + 0x334, rf[3]); L.w32(c + 0x338, rf[4])
                if qpt:
                    m = HEAP + 0xc0000; n = ((w + 15) // 16) * ((h + 15) // 16)
                    if L.r32(c + 0x2b0) != m or y.random() < 0.05:
                        L.wr(m, bytes(y.randint(0, 255) for _ in range(n)))
                    L.wr(c + 0x2ac, b'\x01'); L.w32(c + 0x2b0, m)
                if mce is not None:
                    L.wr(c + 0x348, bytes([1, mce]))
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
