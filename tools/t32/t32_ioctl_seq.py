#!/usr/bin/env python3
"""T32 ioctl trace oracle (#287).

norm  RAW [--header H]   raw t32_ioctl_trace log -> ordered sequence file
diff  REF NEW [--ignore X,..]  compare two sequence files (exit 1 on a difference);
                          --ignore drops requests by name or device (e.g. the
                          IQ-bin requests a later issue adds, or 'rmem')
check SEQ... [--header H] every request of the sequences is in the ABI header,
                          and the header's OI_T32_F_SEEN rows are exactly the
                          requests the sequences contain

Sequence file: '@ <API step>' starts a phase; '= dev NAME req rc=R [xN]' is
an ioctl of the calling thread in order (N consecutive repeats); '~ dev NAME
req rc=R n=N' is a request issued by another libimp thread in that phase
(unordered, with count).  In the streams/frames phase the calling thread is
counted the same way, since the loop length is not part of the ABI.
diff compares the order of '=' lines per phase and the union of the '~'
lines over the run (the phase a worker request lands in is timing); counts
n= are informational.
"""
import argparse
import errno
import os
import re
import sys

HDR = os.path.join(os.path.dirname(__file__), "..", "..", "src", "t32", "openimp_t32_ioctl.h")
UNORDERED_PHASES = ("streams", "frames")


def load_header(path):
    text = open(path).read()
    names = {}
    for m in re.finditer(r"#define OI_T32_(IOC|VPU)_(\w+)\s+0x([0-9a-f]{8})u", text):
        names[int(m.group(3), 16)] = m.group(2)
    seen = set()
    for m in re.finditer(r"X\(\w+, OI_T32_(?:IOC|VPU)_(\w+), [^\n]*?(OI_T32_F_[^)\n]*)\)", text):
        if "OI_T32_F_SEEN" in m.group(2):
            seen.add(m.group(1))
    return names, seen


def rc_text(rc, err):
    if rc >= 0:
        return "rc=%d" % rc
    return "rc=%d:%s" % (rc, errno.errorcode.get(err, str(err)))


def norm(args):
    names, _ = load_header(args.header)
    events = []
    main_tid = None
    for line in open(args.raw, errors="replace"):
        f = line.split()
        if len(f) >= 4 and f[0] == "M":
            if main_tid is None:
                main_tid = f[2]
            events.append((int(f[1]), "M", f[2], " ".join(f[3:])))
        elif len(f) == 7 and f[0] == "I":
            req = int(f[4], 16)
            dev = os.path.basename(f[3])
            events.append((int(f[1]), "I", f[2],
                           (dev, names.get(req, "?"), "%08x" % req, rc_text(int(f[5]), int(f[6])))))
    events.sort(key=lambda e: e[0])
    out = ["# T32 ioctl sequence (tools/t32/t32_ioctl_seq.py norm %s)" % os.path.basename(args.raw)]
    phase, ordered, counted = "start", [], {}

    def flush():
        if not ordered and not counted:
            return
        out.append("@ " + phase)
        last, n = None, 0
        for key in ordered + [None]:
            if key == last:
                n += 1
                continue
            if last is not None:
                out.append("= " + " ".join(last) + (" x%d" % n if n > 1 else ""))
            last, n = key, 1
        for key in sorted(counted):
            out.append("~ " + " ".join(key) + " n=%d" % counted[key])

    for _, kind, tid, data in events:
        if kind == "M":
            if tid == main_tid:
                flush()
                phase, ordered, counted = data, [], {}
            continue
        if tid == main_tid and not phase.startswith(UNORDERED_PHASES):
            ordered.append(data)
        else:
            counted[data] = counted.get(data, 0) + 1
    flush()
    print("\n".join(out))


def parse_seq(path, ignore=()):
    phases = []
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("@ "):
            phases.append([line[2:], [], set()])
        elif line[:2] in ("= ", "~ "):
            if not phases:
                phases.append(["start", [], set()])
            f = line[2:].split()
            if f[0] in ignore or f[1] in ignore:
                continue
            if line[0] == "=":
                phases[-1][1].append(" ".join(f[:4] + [x for x in f[4:] if x.startswith("x")]))
            else:
                phases[-1][2].add(" ".join(f[:4]))
    return phases


def flat(phases):
    """ordered lines per phase, then the union of the other-thread requests
    (which phase a worker request lands in depends on timing)"""
    lines, workers = [], set()
    for name, ordered, counted in phases:
        if ordered:
            lines.append("@ " + name)
            lines += ["= " + x for x in ordered]
        workers |= counted
    lines.append("@ (other threads, whole run)")
    lines += ["~ " + x for x in sorted(workers)]
    return lines


def diff(args):
    import difflib
    ignore = set(x for x in (args.ignore or "").split(",") if x)
    a, b = flat(parse_seq(args.ref, ignore)), flat(parse_seq(args.new, ignore))
    d = list(difflib.unified_diff(a, b, args.ref, args.new, lineterm="", n=2))
    if d:
        print("\n".join(d))
        return 1
    print("identical: %d lines" % len(a))
    return 0


def check(args):
    names, seen = load_header(args.header)
    used, bad = set(), []
    for path in args.seq:
        for _, ordered, counted in parse_seq(path):
            for x in ordered + sorted(counted):
                f = x.split()
                req = int(f[2], 16)
                if names.get(req) != f[1]:
                    bad.append("%s: %s (header: %s)" % (path, x, names.get(req, "missing")))
                used.add(f[1])
    for x in bad:
        print("unknown request: " + x)
    for n in sorted(used - seen):
        print("in the trace but not flagged OI_T32_F_SEEN: " + n)
    for n in sorted(seen - used):
        print("flagged OI_T32_F_SEEN but not in the trace: " + n)
    ok = not bad and used == seen
    print("check %s: %d requests in %d file(s), %d header rows flagged seen"
          % ("ok" if ok else "FAILED", len(used), len(args.seq), len(seen)))
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("norm"); n.add_argument("raw"); n.add_argument("--header", default=HDR)
    d = sub.add_parser("diff"); d.add_argument("ref"); d.add_argument("new"); d.add_argument("--ignore")
    c = sub.add_parser("check"); c.add_argument("seq", nargs="+"); c.add_argument("--header", default=HDR)
    a = p.parse_args()
    return {"norm": norm, "diff": diff, "check": check}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
