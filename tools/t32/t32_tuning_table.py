#!/usr/bin/env python3
"""T32 ISP tuning table (#296).

table RAW               the tuning API steps of a raw t32_ioctl_trace log of t32_tuning_sample (DUMP>=16,
                        payloads armed by oi_trace_tuning_size) -> table file
summary TABLE           markdown rows (step, id, dir, payload bytes, driver answer)
diff  REF NEW [--rc]    compare two tables (exit 1 on a difference): per tuning
                        API step the requests (device, request, VI, dir, id, a
                        non-pointer value), the Set payload bytes and, when both
                        calls returned 0, the Get payload; --rc compares the
                        return codes too (driver served or not)

Table file: '@ <API step>' starts a step; one line per ioctl of it:
  = <dev> <req> rc=<rc> [vi=<n> dir=<set|get> id=<id> val=<ptr|0x..>] [in=<hex>] [out=<hex>]
For a tuning request (0xc0105467) 'in' is the payload behind the value
pointer before the call (Set) and 'out' the bytes after it (Get).
"""
import argparse
import re
import sys

TUNING = 0xC0105467
VALUE_IDS = {0x08000070}        # sensor fps: num << 16 | den in the value word
# bytes of the vendor's driver block it never writes (stack garbage in its local
# buffer): CCM {u8, u8, 2 pad, u32 c[9]}
# CSC: 2 bytes after each 6-byte block
VENDOR_PAD = {"0x08000080": [(2, 4)], "0x08000096": [(46, 48), (90, 92)]}
# the steps compared: tuning API (the ISP lifecycle has its own sequence check)
TUNING_STEP = re.compile(r"Tuning|Csccr|HB_")


def parse_raw(path):
    steps, cur = [], None
    words, pay = {}, {}
    ios = []
    for line in open(path, errors="replace"):
        f = line.split()
        if not f:
            continue
        if f[0] == "M" and len(f) >= 4:
            cur = [" ".join(f[3:]), []]
            steps.append(cur)
        elif f[0] == "D" and len(f) >= 3:
            words[(f[1], f[2])] = [int(x, 16) for x in f[3:]]
        elif f[0] == "P" and len(f) >= 5:
            pay[(f[1], f[2])] = f[4]
        elif f[0] == "I" and len(f) == 7 and cur is not None:
            cur[1].append(f[1])
            ios.append((f[1], f[3], int(f[4], 16), int(f[5]), int(f[6])))
    io = {s: (dev, req, rc, err) for s, dev, req, rc, err in ios}
    return steps, io, words, pay


def table(args):
    steps, io, words, pay = parse_raw(args.raw)
    out = []
    for name, seqs in steps:
        if not TUNING_STEP.search(name):
            continue
        out.append("@ " + name)
        for s in seqs:
            dev, req, rc, err = io[s]
            dev = dev.replace("/dev/", "")
            line = "= %s %08x rc=%d" % (dev, req, rc if rc >= 0 else -err)
            w = words.get((s, "in"))
            if req == TUNING and w and len(w) >= 4:
                pin, pout = pay.get((s, "in")), pay.get((s, "out"))
                ptr = pin is not None and pin != "-" and w[2] not in VALUE_IDS
                wo = words.get((s, "out"), w)
                val = "ptr" if ptr else "0x%08x" % (w[3] if w[1] == 0 else wo[3])
                line += " vi=%d dir=%s id=0x%08x val=%s" % (w[0], "get" if w[1] else "set", w[2], val)
                if ptr:
                    line += " n=%d" % (len(pin) // 2)
                if ptr and w[1] == 0:
                    line += " in=" + pin
                if ptr and w[1] == 1 and pout and pout != "-":
                    line += " out=" + pout
            elif w:
                line += " in=" + "".join("%08x" % x for x in w)
                wo = words.get((s, "out"))
                if wo and rc >= 0:
                    line += " out=" + "".join("%08x" % x for x in wo)
            out.append(line)
    sys.stdout.write("\n".join(out) + "\n")


def load(path):
    steps, cur = [], None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("@ "):
            cur = [line[2:], []]
            steps.append(cur)
        elif line.startswith("= ") and cur is not None:
            f = dict(x.split("=", 1) for x in line[2:].split()[2:] if "=" in x)
            dev, req = line[2:].split()[:2]
            cur[1].append((dev, req, f))
    return steps


def masked(f, key):
    v = f.get(key)
    if v and key in ("in", "out"):
        for lo, hi in VENDOR_PAD.get(f.get("id"), []):
            v = v[:2 * lo] + "xx" * (hi - lo) + v[2 * hi:]
    return v


def diff(args):
    a = [x for x in load(args.ref) if TUNING_STEP.search(x[0])]
    b = [x for x in load(args.new) if TUNING_STEP.search(x[0])]
    bad = 0
    if [s[0] for s in a] != [s[0] for s in b]:
        print("API steps differ")
        bad = 1
    for (na, la), (nb, lb) in zip(a, b):
        if len(la) != len(lb):
            print("%s: %d vs %d requests" % (na, len(la), len(lb)))
            bad = 1
            continue
        for (da, ra, fa), (db, rb, fb) in zip(la, lb):
            keys = ["vi", "dir", "id", "val"]
            if args.rc:
                keys.append("rc")
            if fa.get("dir") != "get":
                keys.append("in")
            if fa.get("rc") == "0" and fb.get("rc") == "0":
                keys.append("out")
            diffs = [k for k in keys if masked(fa, k) != masked(fb, k)]
            if (da, ra) != (db, rb) or diffs:
                print("%s: %s %s vs %s %s differ in %s" % (na, da, ra, db, rb, ",".join(diffs) or "request"))
                bad = 1
    if not bad:
        print("tuning table: %d API steps identical" % len(a))
    return bad


def summary(args):
    """markdown rows: API step, id, dir, payload bytes, driver answer"""
    names = {0: "served", -25: "gap (ENOTTY)", -22: "EINVAL", -19: "ENODEV"}
    for name, lines in load(args.table):
        fn = name.split("(")[0]
        for dev, req, f in lines:
            if "id" in f:
                rc = int(f["rc"])
                print("| %s | %s | %s | %s | %s |" % (fn, f["id"], f["dir"],
                      f.get("n", "value"), names.get(rc, "rc %d" % rc)))
            else:
                rc = int(f["rc"])
                print("| %s | %s %s | - | - | %s |" % (fn, dev, req, names.get(rc, "rc %d" % rc)))


def main():
    p = argparse.ArgumentParser()
    sp = p.add_subparsers(dest="cmd", required=True)
    t = sp.add_parser("table")
    t.add_argument("raw")
    d = sp.add_parser("diff")
    d.add_argument("ref")
    d.add_argument("new")
    d.add_argument("--rc", action="store_true")
    m = sp.add_parser("summary")
    m.add_argument("table")
    args = p.parse_args()
    if args.cmd == "table":
        table(args)
        return 0
    if args.cmd == "summary":
        summary(args)
        return 0
    return diff(args)


if __name__ == "__main__":
    sys.exit(main())
