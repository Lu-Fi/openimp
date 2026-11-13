#!/usr/bin/env python3
"""Make src/t32/openimp_t32_hera_h265_tab.h from vendor libimp 2.2.0 H.265 traces (#294).

The HERA H.265 command list of vendor libimp 2.2.0 is a fixed register
sequence per picture type (I: 692 pairs, P: 840; P adds the MCE block), the
same at every QP and size measured.  The order and the default values come
from one I and one P list of a 1080p FIXQP trace (the first trace, jobs 0
and 2: P after P); registers openimp_t32_hera_h265.c computes (COMPUTED:
sizes, addresses, QP fields, CABAC states) are 0.  The other registers that
change with the QP (the analysis / mode-decision defaults) become QP bands:
for each picture type the runs of equal value over QP 0..51, measured from
the 1080p traces of a FIXQP sweep (every trace given, T32_ENC_QPS chunks).

usage: hera_h265_table.py MAIN.LOG SWEEP.LOG... > src/t32/openimp_t32_hera_h265_tab.h
       hera_h265_table.py --t33 MAIN.LOG SWEEP.LOG... > src/t32/openimp_t33_hera_h265_tab.h

--t33 (#391): vendor libimp 2.0.2.1 traces (QEMU -M ingenic-t33, HERA v1.6;
I 591 pairs, P 626): BC context run of 33, COMPUTED_T33 (openimp_t33_hera_h265.c).
       (input: "V" lines of tools/t32/t32_ioctl_trace.c, T32_TRACE_VPU=12288)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hera_list import jobs  # noqa: E402
from hera_h264_table import merge  # noqa: E402

# registers openimp_t32_hera_h265.c fills in (h265_field); their table value is 0
COMPUTED = {
    0x60000, 0x60004, 0x68000, 0x68004, 0x68008, 0x70004, 0x7000c, 0x3807c,
    0x40004, 0x40008, 0x40010, 0x40280, 0x50000, 0x78000, 0x78004, 0x79004, 0x58028,
    0x10004, 0x10008, 0x1000c, 0x10030, 0x10034, 0x10038, 0x1003c,
    0x28004, 0x28014, 0x28018, 0x28020, 0x28024, 0x28028, 0x2802c,
    0x30000, 0x30060, 0x30064, 0x30070, 0x30074, 0x30078, 0x30800, 0x30804,
    0x90000, 0x90008, 0x9000c, 0x90018, 0x9001c, 0x90024, 0x90028, 0x9002c, 0x90034,
    0x90038, 0x90050, 0x9005c, 0x90064,
    0x20000, 0x20008, 0x2000c, 0x20010, 0x20014, 0x2001c, 0x2003c, 0x20040,
    0x20054, 0x20058, 0x2005c, 0x20060,
}
# T33 (HERA v1.6): what openimp_t33_hera_h265.c fills in (t33_h265_field)
COMPUTED_T33 = (COMPUTED - {0x20054, 0x20058, 0x2005c, 0x20060}) | {
    0x10020, 0x10024, 0x10028, 0x1002c, 0x10174, 0x10178, 0x1017c, 0x10180, 0x10184, 0x10188,
    0x28008, 0x2800c, 0x3007c, 0x90030, 0x90034,
}
BC_CTX, SDE_CTX = 0x61000, 0x69000
CTX_N = {BC_CTX: 145, SDE_CTX: 152}
FRM_SIZE_1080P = 0x04380780     # EFE 0x20008 of a 1920x1080 list
MCE_FRM_SIZE = 0x30060


def regs_of(job):
    return [(v, c & 0xffffc) for v, c in job]


def first(job, reg):
    for v, r in job:
        if r == reg:
            return v
    return None


def sweep(paths):
    """{(type, qp): list} of the 1080p lists, type 1 I / 2 P (the first P
    after an I differs from the others only in COMPUTED registers)"""
    out = {}
    for path in paths:
        for job in jobs(path):
            j = regs_of(job)
            if first(j, 0x20008) != FRM_SIZE_1080P:
                continue
            p = first(j, MCE_FRM_SIZE) is not None
            qp = first(j, 0x60004) >> 1 & 0x3f
            out.setdefault((2 if p else 1, qp), j)
    return out


def bands(lists, t, k):
    """[(qp_from, value)] of list position k over the QPs measured"""
    qps = sorted(q for tt, q in lists if tt == t)
    if len(qps) != 52:
        raise SystemExit("type %d: QPs %s missing" % (t, sorted(set(range(52)) - set(qps))))
    runs = []
    for q in qps:
        v = lists[(t, q)][k][0]
        if not runs or runs[-1][1] != v:
            runs.append((q, v))
    return runs


def main():
    global COMPUTED
    args = sys.argv[1:]
    t33 = bool(args) and args[0] == "--t33"
    if t33:
        args = args[1:]
        COMPUTED = COMPUTED_T33
        CTX_N[BC_CTX] = 33
    main_jobs = jobs(args[0])
    i_list, p_list = regs_of(main_jobs[0]), regs_of(main_jobs[2])
    lists = sweep(args[1:] + [args[0]])
    m = merge(i_list, p_list)
    # list position of each merged entry per type, to read the sweep lists
    pos, ki, kp = [], 0, 0
    for reg, vi, vp, when in m:
        pos.append((ki if when & 1 else None, kp if when & 2 else None))
        ki += when & 1
        kp += (when >> 1) & 1
    for (t, q), j in lists.items():
        ref = i_list if t == 1 else p_list
        if [r for _, r in j] != [r for _, r in ref]:
            raise SystemExit("type %d qp %d: other register order" % (t, q))
    rows, band_tab, band_at, k = [], [], {}, 0
    while k < len(m):
        reg, vi, vp, when = m[k]
        if reg in (BC_CTX, SDE_CTX):
            n = 0
            while k + n < len(m) and m[k + n][0] == reg + 4 * n:
                n += 1
            assert n == CTX_N[reg], n
            rows.append((reg, 0, 0, when, n, 0, 0, "BC" if reg == BC_CTX else "SDE"))
            k += n
            continue
        bi = bp = 0
        if reg in COMPUTED:
            vi = vp = 0
        else:
            for t, idx in ((1, pos[k][0]), (2, pos[k][1])):
                if idx is None:
                    continue
                b = tuple(bands(lists, t, idx))
                if len(b) > 1:
                    if b not in band_at:            # equal runs share one entry
                        band_at[b] = len(band_tab) + 1
                        band_tab.extend(b)
                    if t == 1:
                        bi = band_at[b]
                    else:
                        bp = band_at[b]
        rows.append((reg, vi if when & 1 else 0, vp if when & 2 else 0, when, 1, bi, bp, None))
        k += 1
    pre = "t33" if t33 else "t32"
    if t33:
        print("/* Generated by tools/t32/hera_h265_table.py --t33 from vendor libimp 2.0.2.1")
        print(" * FIXQP traces (QEMU -M ingenic-t33, HERA v1.6, #391): the H.265 command list's")
        print(" * register order and default values per picture type; registers")
        print(" * openimp_t33_hera_h265.c computes are 0 here.  {reg, I value, P value, n,")
    else:
        print("/* Generated by tools/t32/hera_h265_table.py from vendor libimp 2.2.0 FIXQP")
        print(" * traces (QEMU -M ingenic-t32, #294): the HERA H.265 command list's register")
        print(" * order and default values per picture type; registers")
        print(" * openimp_t32_hera_h265.c computes are 0 here.  {reg, I value, P value, n,")
    print(" * when: 1 I, 2 P, 3 both, I band, P band}: a band (1-based index into")
    print(" * %s_hera_h265_band) replaces the default by QP: runs {first QP, value} from" % pre)
    print(" * QP 0, up to the next run starting at QP 0 or the end. */")
    print("static const T32Hera5Ent %s_hera_h265_tab[] = {" % pre)
    for reg, vi, vp, when, n, bi, bp, note in rows:
        line = "    { 0x%05x, 0x%08x, 0x%08x, %3d, %d, %3d, %3d }," % (reg, vi, vp, n, when, bi, bp)
        if note:
            line += "  /* %s CABAC context states */" % note
        print(line)
    print("};")
    print("/* pairs: I %d, P %d */" % (sum(r[4] for r in rows if r[3] & 1), sum(r[4] for r in rows if r[3] & 2)))
    print("static const T32HeraBand %s_hera_h265_band[] = {" % pre)
    for q, v in band_tab:
        print("    { %2d, 0x%08x }," % (q, v))
    print("    { 0, 0 },")
    print("};")


if __name__ == "__main__":
    main()
