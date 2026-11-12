#!/usr/bin/env python3
"""Issues #357/#397 (development aid): reference vectors for src/watermark from the OEM embed_watermark.

IMP_Encoder_SetFisheyeEnableStatus is, in the vendor libraries, an invisible luma watermark: the hwicodec H.264 (T30
also H.265) encoders call watermark_init() once and embed_watermark(ctx, luma, stride, height) on the source picture
of every I picture while the flag is set.  This script runs the real vendor watermark_init + embed_watermark of a
libimp.so under the unicorn CPU emulator on random luma planes and prints the rewritten top-left 64x16 pixels.
tests/watermark/watermark_test.c checks src/watermark against the output.  Nothing of the vendor library is copied
into OpenIMP; it is read from the path you pass.

    pip install unicorn pyelftools
    tools/watermark_oracle.py T31/lib/1.1.6/uclibc/5.4.0/libimp.so 357 120 > tests/watermark/watermark_vectors.txt

The same command on T10/T20 3.12.0, T21 1.0.33 (both gcc builds), T23 1.3.0, T30 1.0.5 (both), T31 1.1.6 (both) and T40
1.3.1 prints the same lines apart from the header (T40: o32 FP64, the script sets Status.FR; its madd.s/madd.d are the
unfused MIPS32 R2 ones).  T41 1.2.6 has no watermark.  A mipsel shared object built from src/watermark/watermark.c
(it exports watermark_embed) runs in place of the vendor library, which checks the cross-compiled code under the same
CPU model:

    mipsel-linux-gcc -O2 -fPIC -shared -Isrc -o wm.so src/watermark/watermark.c
    tools/watermark_oracle.py wm.so 357 120 | tail -n +2 | cmp - <(tail -n +2 tests/watermark/watermark_vectors.txt)

Lines: 'W stride height ret : <in 64x16 hex> : <out 64x16 hex>' (rows of 64 bytes, concatenated).  The plane around
the 64x16 block is checked for untouched bytes by the script itself (it raises otherwise).
"""
import os
import random
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eprc_oracle as O
from unicorn.mips_const import UC_MIPS_REG_CP0_STATUS


def gen_block(rng, kind):
    """64x16 luma: mixes of flat dark, flat bright, noise, gradients, edges (dark blocks hit the fixed-block path)."""
    b = bytearray(64 * 16)
    for by in range(2):
        for bx in range(8):
            k = kind if kind >= 0 else rng.randrange(8)
            base = rng.randrange(256)
            for y in range(8):
                for x in range(8):
                    if k == 0:
                        v = rng.randrange(256)
                    elif k == 1:
                        v = rng.randrange(12)                       # dark (DC < 80 -> fixed block)
                    elif k == 2:
                        v = base
                    elif k == 3:
                        v = (base + 9 * x + 5 * y) & 255
                    elif k == 4:
                        v = 235 if x + y > 7 else 16
                    elif k == 5:
                        v = min(255, max(0, base + rng.randrange(-6, 7)))
                    elif k == 6:
                        v = rng.choice((0, 255))
                    else:
                        v = min(255, 240 + rng.randrange(16))       # near white (wrap-around after the IDCT)
                    b[(by * 8 + y) * 64 + bx * 8 + x] = v
    return bytes(b)


def embed(L, ctx, luma, stride, height):
    """The vendor routine, or watermark_embed() of a mipsel build of src/watermark (--ours)."""
    if ctx is None:
        r = L.call('watermark_embed', luma, stride, height)
    else:
        r = L.call('embed_watermark', ctx, luma, stride, height)
    r &= 0xffffffff
    return r - (1 << 32) if r >> 31 else r


def run(L, ctx, blk, stride, height, rng):
    H = O.HEAP + 0x100000
    plane = bytearray(rng.randrange(256) for _ in range(stride * height))
    for y in range(16):
        plane[y * stride:y * stride + 64] = blk[y * 64:(y + 1) * 64]
    L.wr(H, plane)
    ret = embed(L, ctx, H, stride, height)
    out = L.rd(H, stride * height)
    for y in range(height):
        lo = 64 if y < 16 else 0
        if out[y * stride + lo:(y + 1) * stride] != plane[y * stride + lo:(y + 1) * stride]:
            raise RuntimeError('vendor wrote outside the 64x16 block (row %d)' % y)
    res = b''.join(out[y * stride:y * stride + 64] for y in range(16))
    return ret, res


def main():
    lib = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 357
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    L = O.Lib(lib, generic=True)
    # T40/T41 (gcc 7.2, -mfp64): o32 FP64, the FPU runs with Status.FR = 1
    if '64-bit FPU' in subprocess.run(['readelf', '-A', lib], capture_output=True, text=True,
                                      env=dict(os.environ, LC_ALL='C')).stdout:
        st = L.uc.reg_read(UC_MIPS_REG_CP0_STATUS)
        L.uc.reg_write(UC_MIPS_REG_CP0_STATUS, st | (1 << 26))
    rng = random.Random(seed)
    if 'watermark_embed' in L.syms:
        ctx = None
        print('# OpenIMP watermark_embed seed %d' % seed)
    else:
        ctx = L.call('watermark_init')
        words = [L.r32(ctx + 4 * i) for i in range(5)]
        print('# vendor embed_watermark seed %d ctx %s' % (seed, ' '.join('%08x' % w for w in words)))
    cases = [(64, 16), (64, 15), (63, 16), (80, 16), (128, 24)]
    for i in range(n):
        if i < len(cases):
            stride, height = cases[i]
        else:
            stride = rng.choice((64, 72, 96, 128, 200))
            height = rng.choice((16, 17, 32))
        blk = gen_block(rng, i % 9 - 1 if i % 9 else -1)
        if stride < 64:
            print('W %d %d %d : - : -' % (stride, height, embed(L, ctx, O.HEAP + 0x100000, stride, height)))
            continue
        ret, res = run(L, ctx, blk, stride, height, rng)
        if ret:
            print('W %d %d %d : - : -' % (stride, height, ret))
            continue
        print('W %d %d %d : %s : %s' % (stride, height, ret, blk.hex(), res.hex()))


if __name__ == '__main__':
    main()
