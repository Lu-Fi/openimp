#!/usr/bin/env python3
"""Refresh docs/status/driver_port.json (kernel driver port progress of the recovered vendor code) from an open-tx-isp checkout.

Local tool (the pages build in CI from this repo only and just read the JSON).  Read-only on the checkout, git commands only, no branch switch.
  total  = number of fn_*.c files under driver/t32/recovered on the recovered-driver branch
  proven = unique fn_*.c names listed in tests/t32_oracle/*.conf on the oracle branch (functions proven bit-exact by a host oracle)
T33 has no recovered driver yet: proven 0, total null (kept as in the JSON, not touched here).
Usage: python3 tools/status_driver_port.py [--repo /path/to/open-tx-isp] [--recovered REF] [--oracle REF] [--print]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.environ.get("OPEN_TX_ISP", "../open-tx-isp"))
    ap.add_argument("--recovered", default="origin/t32/recovered-baseline")
    ap.add_argument("--oracle", default="origin/claude/t32-ds-batch")
    ap.add_argument("--print", action="store_true", help="print, do not write")
    a = ap.parse_args()
    total = len({os.path.basename(p) for p in git(a.repo, "ls-tree", "-r", "--name-only", a.recovered, "driver/t32/recovered").split("\n")
                 if FN.fullmatch(os.path.basename(p))})
    names = set()
    for p in git(a.repo, "ls-tree", "-r", "--name-only", a.oracle, "tests/t32_oracle").split("\n"):
        if p.endswith(".conf"):
            names |= set(FN.findall(git(a.repo, "show", "%s:%s" % (a.oracle, p))))
    day = git(a.repo, "log", "-1", "--format=%cs", a.oracle).strip()
    d = json.load(open(OUT, encoding="utf-8")) if os.path.exists(OUT) else {"socs": {}}
    d["asof"] = day
    d["socs"].setdefault("T33", {"proven": 0, "total": None, "branch": None, "date": day,
                                 "note": "paused, no recovered driver yet"})
    d["socs"]["T32"] = {"proven": len(names), "total": total, "branch": a.oracle.replace("origin/", ""), "date": day,
                        "note": "bit-exact against the recovered vendor C on the host; not merged, waits for the upstream recovered-driver pull request; host-proven, not device-tested"}
    s = json.dumps(d, indent=1) + "\n"
    if a.print:
        sys.stdout.write(s)
    else:
        open(OUT, "w", encoding="utf-8").write(s)
        print("T32 %d/%d (as of %s) -> %s" % (len(names), total, day, OUT))


if __name__ == "__main__":
    main()
