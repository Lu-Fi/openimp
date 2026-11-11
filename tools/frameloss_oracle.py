#!/usr/bin/env python3
"""T23 IMP_Encoder_Setframelossthd (development aid): reference vectors for src/t23/t23_frameloss.h.

The OEM T23 libimp 1.3.0 stream thread (update_h264_one_frmstrm) drops an H.264 picture of `threshold` bytes or more
and asks get_idr_frameqp (0x4d508) for the QP of the IDR it requests.  get_idr_frameqp reads the channel record:
+0x44 rate-control mode, +0x48 FIXQP QP / maxQp, +0x4c minQp, +0x5e8 slice_qp_delta of the last picture (slice QP -
pic_init_qp, written back by i264e_encode), +0xa18 the threshold.  This script runs the real function under the unicorn
CPU emulator (tools/eprc_oracle.py, `log` from the host libm) and prints one line per case:

    F mode qp min max delta bytes threshold : result

tests/t23/frameloss_test.c checks t23_frameloss_idr_qp() against the output (OpenIMP's PPS has pic_init_qp 26, so its
`slice_qp` argument is delta + 26).  Nothing of the vendor library is copied into OpenIMP; it is read from the path you pass.

    tools/frameloss_oracle.py T23/lib/1.3.0/uclibc/libimp.so | cmp - <(tail -n +3 tests/t23/frameloss_vectors.txt)
"""
import os
import random
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eprc_oracle as O

REC = O.HEAP + 0x100000


def cases():
    r = random.Random(11)
    for mode in (0, 1, 2, 3, 4, 7):
        for delta in (-26, -10, 0, 9, 25):
            for thr in (1, 1000, 30000, 100000):
                for mul in (0, 1, 2, 3, 7, 8, 20, 21, 54, 55, 148, 149, 1000):
                    for bytes_ in {thr * mul, thr * mul + thr // 2, max(thr * mul - 1, 0)}:
                        yield mode, 30, 15, 45, delta, bytes_, thr
    for _ in range(3000):
        mode = r.randint(0, 3)
        mn = r.randint(0, 51)
        mx = r.randint(0, 51)
        thr = r.randint(1, 1 << 20)
        yield mode, r.randint(0, 51), mn, mx, r.randint(-26, 25), r.randint(0, 1 << 24), thr


def main():
    L = O.Lib(sys.argv[1])
    out = set()
    for mode, qp, mn, mx, delta, bytes_, thr in cases():
        L.wr(REC, bytes(0x1000))
        L.w32(REC + 0x44, mode)
        L.w32(REC + 0x48, qp if mode == 0 else mx)
        L.w32(REC + 0x4c, mn)
        L.w32(REC + 0x5e8, delta)
        L.w32(REC + 0xa18, thr)
        v = L.call('get_idr_frameqp', REC, bytes_)
        v -= (v >> 31) << 32
        out.add('F %d %d %d %d %d %d %d : %d' % (mode, qp, mn, mx, delta, bytes_, thr, v))
    for line in sorted(out):
        print(line)


if __name__ == '__main__':
    main()
