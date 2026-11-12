#!/usr/bin/env python3
"""Issues #357/#397 (development aid): is the vendor watermark in a coded stream?

Decodes the I pictures of an H.264/H.265 elementary stream (ffmpeg), and runs the vendor's own detector on the
top-left 64x16 luma of each: check_watermark() of a vendor libimp.so (it estimates the mark of each 8x8 block from the
coefficients the embedder set and correlates it with the 32-value pattern; the vendor treats a correlation of 0.42 or
more as marked) under the unicorn CPU emulator (loader: eprc_oracle.py).  Nothing of the vendor library is copied; it
is read from the path you pass.  The pictures stay local (only numbers are printed).

    tools/watermark_check.py T31/lib/1.1.6/uclibc/5.4.0/libimp.so stream.h264 [--dump DIR]

Per I picture: 'I <n> corr <c> marked <0|1> mean <luma mean of the 64x16 area>'; then a summary line.  --dump writes
the decoded 64x16 areas as PGM files into DIR (local inspection).
"""
import os
import struct
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eprc_oracle as O
from unicorn.mips_const import UC_MIPS_REG_F0, UC_MIPS_REG_CP0_STATUS


def i_pictures(path):
    p = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
                        '-of', 'csv=p=0', path], capture_output=True, text=True, check=True)
    w, h = (int(x) for x in p.stdout.strip().split(',')[:2])
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-vf', "select='eq(pict_type,I)'", '-vsync', '0',
                          '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True, check=True).stdout
    n = w * h
    return w, h, [raw[i:i + n] for i in range(0, len(raw) - n + 1, n)]


def main():
    lib, stream = sys.argv[1], sys.argv[2]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else None
    L = O.Lib(lib, generic=True)
    if '64-bit FPU' in subprocess.run(['readelf', '-A', lib], capture_output=True, text=True,
                                      env=dict(os.environ, LC_ALL='C')).stdout:
        L.uc.reg_write(UC_MIPS_REG_CP0_STATUS, L.uc.reg_read(UC_MIPS_REG_CP0_STATUS) | (1 << 26))
    ctx = L.call('watermark_init')
    w, h, pics = i_pictures(stream)
    marked = 0
    corrs = []
    for k, y in enumerate(pics):
        H = O.HEAP + 0x100000
        L.wr(H, y)
        r = L.call('check_watermark', ctx, H, w, h) & 0xffffffff
        c = struct.unpack('<f', struct.pack('<I', L.uc.reg_read(UC_MIPS_REG_F0) & 0xffffffff))[0]
        area = b''.join(y[r0 * w:r0 * w + 64] for r0 in range(16))
        print('I %d corr %.4f marked %d mean %.1f' % (k, c, r, sum(area) / len(area)))
        marked += r == 1
        corrs.append(c)
        if dump:
            os.makedirs(dump, exist_ok=True)
            with open(os.path.join(dump, 'i%02d.pgm' % k), 'wb') as f:
                f.write(b'P5 64 16 255\n' + area)
    print('summary %s: %dx%d, %d I pictures, %d marked, corr min %.3f max %.3f' % (
        os.path.basename(stream), w, h, len(pics), marked, min(corrs) if corrs else 0, max(corrs) if corrs else 0))


if __name__ == '__main__':
    main()
