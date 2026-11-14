#!/usr/bin/env python3
"""Refresh docs/status/driver_port.json (kernel driver port progress of the recovered vendor code) from an open-tx-isp checkout.

Local tool (the pages build in CI from this repo only and just read the JSON).  Read-only on the checkout, git commands only, no branch switch.
  total  = number of fn_*.c files under driver/t32/recovered on the recovered-driver branch
  proven = unique fn_*.c names listed in tests/t32_oracle/*.conf on the oracle branch (functions proven bit-exact by a host oracle)
T33 has no recovered driver: its row counts the T33-only functions of the vendor tx-isp-t33.ko (no T32 namesake; total and
'ported by name' come from the local inventory, Lu-Fi audit/t33-port, kept from the JSON unless --t33-total / --t33-ported):
  proven = unique vendor function names in FUNCS (first column) and PROVES of tests/t33_binoracle/*.conf on --t33-oracle
           (functions that pass the binary oracle: the vendor instructions in Unicorn against our C)
Readable-C (de-lift) metric per SoC: --readable DIR reads DIR/<soc>/summary.json (+ summary_base.json = before the de-lift),
written by Lu-Fi tools/delift_inventory.py (rules: DIR/t10/README.md); proven = functions that are real C and proven
(vendor SDK / own code, or a de-lift oracle), total = every C function compiled into the module.  --readable-only updates
only that section (T32/T33 rows untouched).
Usage: python3 tools/status_driver_port.py [--repo /path/to/open-tx-isp] [--recovered REF] [--oracle REF] [--t33-oracle REF]
       [--t33-total N] [--t33-ported N] [--print]
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "status", "driver_port.json")
FN = re.compile(r"fn_[0-9a-f]+_[A-Za-z0-9_]+\.c")


def git(repo, *a):
    return subprocess.run(["git", "-C", repo] + list(a), check=True, capture_output=True, text=True).stdout


def readable(d, top):
    """readable-C rows from the de-lift inventories (one summary.json per SoC)"""
    if not top or not os.path.isdir(top):
        return
    r = d.setdefault("readable", {})
    for soc in sorted(os.listdir(top)):
        f = os.path.join(top, soc, "summary.json")
        if not os.path.isfile(f):
            continue
        cur = json.load(open(f, encoding="utf-8"))
        bf = os.path.join(top, soc, "summary_base.json")
        base = json.load(open(bf, encoding="utf-8")) if os.path.isfile(bf) else cur
        r[soc.upper()] = {"proven": cur["proven_readable"], "total": cur["functions"],
                          "machine_lines": cur["machine_lines"], "lines": cur["lines"],
                          "pct_machine_lines": cur["pct_machine_lines"],
                          "base_proven": base["proven_readable"], "base_pct_machine_lines": base["pct_machine_lines"],
                          "date": cur.get("date", ""), "issue": cur.get("issue", ""), "note": cur.get("note", "")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.environ.get("OPEN_TX_ISP", "../open-tx-isp"))
    ap.add_argument("--recovered", default="origin/t32/recovered-baseline")
    ap.add_argument("--oracle", default="origin/claude/t32-ds-batch")
    ap.add_argument("--t33-oracle", default="origin/next")
    ap.add_argument("--t33-total", type=int, default=None)
    ap.add_argument("--t33-ported", type=int, default=None)
    ap.add_argument("--print", action="store_true", help="print, do not write")
    ap.add_argument("--readable", default=os.environ.get("DELIFT_DIR"), help="dir with <soc>/summary.json (de-lift inventory)")
    ap.add_argument("--readable-only", action="store_true", help="update only the readable-C section")
    a = ap.parse_args()
    if a.readable_only:
        d = json.load(open(OUT, encoding="utf-8"))
        readable(d, a.readable)
        s = json.dumps(d, indent=1) + "\n"
        sys.stdout.write(s) if a.print else open(OUT, "w", encoding="utf-8").write(s)
        print("readable C: %s" % ", ".join("%s %d/%d" % (k, v["proven"], v["total"]) for k, v in sorted(d.get("readable", {}).items())))
        return
    total = len({os.path.basename(p) for p in git(a.repo, "ls-tree", "-r", "--name-only", a.recovered, "driver/t32/recovered").split("\n")
                 if FN.fullmatch(os.path.basename(p))})
    names = set()
    for p in git(a.repo, "ls-tree", "-r", "--name-only", a.oracle, "tests/t32_oracle").split("\n"):
        if p.endswith(".conf"):
            names |= set(FN.findall(git(a.repo, "show", "%s:%s" % (a.oracle, p))))
    day = git(a.repo, "log", "-1", "--format=%cs", a.oracle).strip()
    d = json.load(open(OUT, encoding="utf-8")) if os.path.exists(OUT) else {"socs": {}}
    d["asof"] = day
    t33 = set()
    for p in git(a.repo, "ls-tree", "-r", "--name-only", a.t33_oracle, "tests/t33_binoracle").split("\n"):
        if p.endswith(".conf"):
            c = git(a.repo, "show", "%s:%s" % (a.t33_oracle, p))
            m = re.search(r"^FUNCS='(.*?)'", c, re.S | re.M)
            for ln in (m.group(1) if m else "").splitlines():
                w = ln.split()
                if w and not w[0].startswith("#") and w[0] not in ("VEC", "GLOBAL", "CALLS"):
                    t33.add(w[0])
            m = re.search(r"^PROVES='(.*?)'", c, re.S | re.M)
            t33 |= set((m.group(1) if m else "").split())
    o = d["socs"].get("T33", {})
    tot = a.t33_total or o.get("total") or 409
    ported = a.t33_ported if a.t33_ported is not None else o.get("ported", 0)
    t33day = git(a.repo, "log", "-1", "--format=%cs", a.t33_oracle).strip()
    d["socs"]["T33"] = {"proven": len(t33), "ported": ported, "total": tot, "branch": a.t33_oracle.replace("origin/", ""), "date": t33day,
                        "note": "counting basis differs from T32 (no recovered T33 C): of the vendor tx-isp-t33.ko (1571 functions) 1162 have a "
                                "T32 namesake and run on the shared driver/t32 port; the bar counts the %d T33-only functions; proven = %d that "
                                "pass the binary oracle (vendor instructions in Unicorn against our C, tests/t33_binoracle, openimp #99); "
                                "%d more are ported by name in the T33 files but not oracle-proven yet (QEMU apitest only); "
                                "not device-tested" % (tot, len(t33), max(ported - len(t33), 0))}
    d["asof"] = max(day, t33day)
    d["socs"]["T32"] = {"proven": len(names), "total": total, "branch": a.oracle.replace("origin/", ""), "date": day,
                        "note": "bit-exact against the recovered vendor C on the host; merged into open-tx-isp next; reference-C oracles stay on the T32 branch until upstream PR #15; host-proven, not device-tested"}
    if a.readable:
        readable(d, a.readable)
    s = json.dumps(d, indent=1) + "\n"
    if a.print:
        sys.stdout.write(s)
    else:
        open(OUT, "w", encoding="utf-8").write(s)
        print("T32 %d/%d, T33 %d/%d (as of %s) -> %s" % (len(names), total, len(t33), tot, day, OUT))


if __name__ == "__main__":
    main()
