#!/usr/bin/env python3
"""Completeness check (part of `make check`): every vendor function of a SoC (tools/export-check/vendor-<SoC>.txt) has a test
case in tools/apitest (its name appears in the case sources, incl. the picture/devtest sources) or an entry in
tools/apitest/untestable.yaml (category + reason).  Exit 1 on a function with neither, a bad entry, or an entry for a
function that has a test case.  Prints the number of entries per category (backlog = testable, no case yet; a SoC in STRICT fails on any backlog)."""
import fnmatch, glob, os, re, sys
import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SOCS = ["T10", "T20", "T21", "T23", "T31", "T40", "T41"]
STRICT = ["T40", "T41"]     # SoCs whose backlog is closed: a function that is still category "backlog" fails the check (T41/T40 since 2026-10-28)
CATS = {"sound", "reboot", "power", "hardware", "wedges-isp", "no-signature", "own-topology", "not-in-soc-api", "backlog"}

srcs = [f for f in glob.glob(os.path.join(HERE, "*.c"))] + glob.glob(os.path.join(ROOT, "tests", "t40", "devtest", "*.c"))
text = "".join(open(f, errors="replace").read() for f in srcs)
toks = set(re.findall(r"\b[A-Za-z]\w+", text))
entries = yaml.safe_load(open(os.path.join(HERE, "untestable.yaml")))["entries"]
bad = []
for e in entries:
    if e.get("category") not in CATS or not e.get("reason") or not e.get("match"):
        bad.append("bad entry: %s" % (e.get("match"),))
    socs = e.get("socs", "all")
    if socs != "all" and not set(socs) <= set(SOCS + ["T30", "T32", "T33"]):
        bad.append("bad socs in %s" % (e.get("match"),))
missing, double, stats = [], [], {}
for soc in SOCS:
    for name in (l.strip() for l in open(os.path.join(ROOT, "tools", "export-check", "vendor-%s.txt" % soc))):
        if not name:
            continue
        ent = [e for e in entries if (e.get("socs", "all") == "all" or soc in e["socs"]) and any(fnmatch.fnmatchcase(name, m) for m in e["match"])]
        if name in toks:
            if ent:
                double.append((soc, name))
            continue
        if not ent:
            missing.append((soc, name)); continue
        stats.setdefault(soc, {}).setdefault(ent[0]["category"], 0)
        stats[soc][ent[0]["category"]] += 1
for soc in SOCS:
    print("apitest coverage %s: %s" % (soc, ", ".join("%s %d" % (k, v) for k, v in sorted(stats.get(soc, {}).items())) or "all vendor functions have a case"))
for soc in STRICT:
    if stats.get(soc, {}).get("backlog"):
        bad.append("%s is strict: %d function(s) are still category backlog (write a case or classify them with a concrete reason)" % (soc, stats[soc]["backlog"]))
for m in bad:
    print("ERROR", m)
for soc, n in missing:
    print("ERROR %s: %s has neither a test case in tools/apitest nor an entry in tools/apitest/untestable.yaml" % (soc, n))
for soc, n in (double[:20] if "-v" in sys.argv else []):
    print("note %s: %s has a test case and also matches an untestable.yaml entry" % (soc, n))
sys.exit(1 if bad or missing else 0)
