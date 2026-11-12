#!/usr/bin/env python3
# Needs python3-unicorn. Usage: VENDOR_LIBSYSUTILS=path tools/su_time_oracle.py > tests/su_time/vectors.txt
# Unicorn oracle: vendor libsysutils 1.3.0 (T40 SDK) SU_Base_Raw2SUTime / SU_Base_SUTime2Raw
import sys, struct, random
from unicorn import *
from unicorn.mips_const import *
import os
LIB = os.environ.get("VENDOR_LIBSYSUTILS", "libsysutils.so")  # vendor T40 SDK libsysutils 1.3.0 (md5 82fb1afc43ef0f6d8c472c9f74c306c2)
R2S, S2R = 0x1dc8, 0x1c58
data = open(LIB, "rb").read()
mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
mu.mem_map(0, 0x40000)
mu.mem_write(0, data[:0x7374])
mu.mem_write(0x17374, data[0x7374:0x7374+0x108])
mu.mem_map(0x100000, 0x10000)   # stack/data
STOP = 0x100f00
def call(fn, a0, a1):
    mu.mem_write(0x100800, b"\0"*64)
    mu.reg_write(UC_MIPS_REG_SP, 0x10f000)
    mu.reg_write(UC_MIPS_REG_T9, fn)
    mu.reg_write(UC_MIPS_REG_A0, a0)
    mu.reg_write(UC_MIPS_REG_A1, a1)
    mu.reg_write(UC_MIPS_REG_RA, STOP)
    mu.mem_map(STOP & ~0xfff, 0x1000) if False else None
    mu.emu_start(fn, STOP, count=20000)
    return mu.reg_read(UC_MIPS_REG_V0)
mu.mem_write(STOP, b"\0"*4)
def s32(x): return x - (1<<32) if x >= 1<<31 else x
def r2s(raw):
    mu.mem_write(0x100900, struct.pack("<I", raw)); mu.mem_write(0x100a00, b"\xaa"*32)
    rc = s32(call(R2S, 0x100900, 0x100a00))
    return rc, struct.unpack("<6i", bytes(mu.mem_read(0x100a00, 24)))
def s2r(f):
    mu.mem_write(0x100900, struct.pack("<6i", *f)); mu.mem_write(0x100a00, b"\xaa"*8)
    rc = s32(call(S2R, 0x100900, 0x100a00))
    return rc, struct.unpack("<I", bytes(mu.mem_read(0x100a00, 4)))[0]
def gen(seed=321):
    rnd = random.Random(seed)
    raws = [0,1,59,60,86399,86400,951782399,951782400,951868800,1078012800,1078099200,1330473600,1330560000,
            946684800,1582934400,1583020800,4102444799,4102444800,0x7fffffff,0x80000000,0xfffffffe,0xffffffff,
            4107542400,4107628800] + [rnd.getrandbits(32) for _ in range(3000)]
    for v in raws:
        rc, f = r2s(v)
        print("R %u %d %d %d %d %d %d %d" % (v, rc, *f[:6]))
    cases = []
    for y in (1900,1969,1970,1971,1999,2000,2001,2004,2024,2038,2100,2106,2107,2200):
        for m in (-1,0,1,2,3,12,13,99):
            for d in (-1,0,1,28,29,30,31,32,100):
                cases.append((0,0,0,d,m,y))
    cases += [(59,59,23,19,1,2038),(7,28,6,7,2,2106),(15,28,6,7,2,2106),(-1,0,0,1,1,1970)]
    for _ in range(4000):
        k = rnd.random()
        if k < .5: f = (rnd.randrange(60),rnd.randrange(60),rnd.randrange(24),rnd.randrange(1,29),rnd.randrange(1,13),rnd.randrange(1970,2106))
        elif k < .8: f = (rnd.randrange(-5,70),rnd.randrange(-5,70),rnd.randrange(-5,30),rnd.randrange(-3,40),rnd.randrange(-2,16),rnd.randrange(1960,2120))
        else: f = tuple(rnd.randrange(-2**31,2**31) if rnd.random()<.3 else rnd.randrange(-100,200) for _ in range(6))
        cases.append(f)
    for f in cases:
        rc, raw = s2r(f)
        print("S %d %d %d %d %d %d %d %u" % (*f, rc, raw))
if __name__ == "__main__":
    gen()
