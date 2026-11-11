#!/usr/bin/env python3
"""Decode the HERA command lists of a T32 trace (#293).

The recorder (tools/t32/t32_ioctl_trace.c, T32_TRACE_VPU=<bytes>) logs each
CHANNEL_START's job image as "V <seq> <pa> <words>".  A HERA list is pairs
(value, VLD bit 31 | TERM bit 30 | checksum 29:20 | register offset 19:0);
the checksum is the byte sum of the value and of the offset's low 16 bits,
10 bits.

usage: hera_list.py RAW.LOG list          one "job n idx reg value" line per pair
       hera_list.py RAW.LOG check         pairs, TERM, checksum errors per job
       hera_list.py RAW.LOG regs [n]      the register image after job n (last write wins)
       hera_list.py RAW.LOG vec NAME n..  "V NAME-<n> <words>" test vectors (tests/t32/hera_list_test.c)
"""
import sys


def jobs(path):
    out = []
    for line in open(path, errors="replace"):
        if line.startswith("V "):
            w = [int(x, 16) for x in line.split()[3:]]
            pairs = []
            for i in range(0, len(w) - 1, 2):
                pairs.append((w[i], w[i + 1]))
                if w[i + 1] & (1 << 30):
                    break
            out.append(pairs)
    return out


def csum(val, reg):
    s = sum((val >> k) & 0xff for k in (0, 8, 16, 24)) + (reg & 0xff) + ((reg >> 8) & 0xff)
    return s & 0x3ff


def main():
    path, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "check"
    js = jobs(path)
    if mode == "list":
        for n, p in enumerate(js):
            for i, (v, c) in enumerate(p):
                print("job %d %4d %05x %08x%s%s" % (n, i, c & 0xffffc, v, "" if c >> 31 else " !VLD",
                                                   " TERM" if c & (1 << 30) else ""))
    elif mode == "check":
        for n, p in enumerate(js):
            bad = sum(1 for v, c in p if (c >> 20) & 0x3ff != csum(v, c & 0xffffc))
            print("job %d: %d pairs, term %s, checksum errors %d" % (n, len(p), bool(p and p[-1][1] & (1 << 30)), bad))
    elif mode == "vec":
        for n in sys.argv[4:]:
            p = js[int(n)]
            print("V %s-%s %s" % (sys.argv[3], n, " ".join("%08x %08x" % vc for vc in p)))
    elif mode == "regs":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        regs = {}
        for v, c in js[n]:
            regs[c & 0xffffc] = v
        for r in sorted(regs):
            print("%05x %08x" % (r, regs[r]))


if __name__ == "__main__":
    main()
