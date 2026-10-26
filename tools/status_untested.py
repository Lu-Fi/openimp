#!/usr/bin/env python3
"""docs/status/untested.json: why an implemented function (cell host/aud) has no device test yet.

    python3 tools/status_untested.py RESULT_DIR_OR_JSON ...

Reads the result.json files of the apitest/imgfx device runs (tools/apitest/results2json.py output; the directory
name or the file's "soc" field gives the SoC) and keeps, per SoC, the functions whose best verdict was PART
(test ran, result unclear), SKIP or NA (not run).  tools/gen_api_coverage.py derives the reason of every
untested cell from it: PART -> "test ran, result unclear (PART: reason)", SKIP/NA -> "not run on this SoC",
a function without an entry on a SoC with a run -> "no test case yet", a SoC without a run -> "not run on this SoC".
"""
import glob
import json
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "status", "untested.json")
RANK = {"PASS": 0, "RT": 1, "PART": 2, "SKIP": 3, "NA": 4, "FAIL": 5}


def main(paths):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, "result.json"))) if os.path.isdir(p) else [p]
    socs = {}
    for f in files:
        r = json.load(open(f, encoding="utf-8"))
        if not r.get("funcs"):
            continue
        s = socs.setdefault(r["soc"], {"runs": [], "funcs": {}})
        s["runs"].append("%s %s" % (r.get("image", ""), r.get("date", "")))
        for k, v in r["funcs"].items():
            old = s["funcs"].get(k)
            if old is None or RANK.get(v["v"], 9) < RANK.get(old[0], 9):
                s["funcs"][k] = [v["v"], v.get("detail", "")]
    res = {"_comment": "Device-run verdicts that are not PASS, per SoC (tools/status_untested.py). Used for the reason of "
                       "'implemented, not device-tested' cells in the API tables.",
           "socs": {k: {"runs": sorted(set(v["runs"])), "funcs": {n: x for n, x in sorted(v["funcs"].items())
                                                                    if x[0] in ("PART", "SKIP", "NA")}} for k, v in sorted(socs.items())}}
    json.dump(res, open(OUT, "w", encoding="utf-8"), indent=0, ensure_ascii=False)
    print("wrote %s: %s" % (os.path.normpath(OUT), ", ".join("%s %d non-pass" % (k, len(v["funcs"])) for k, v in res["socs"].items())))


if __name__ == "__main__":
    main(sys.argv[1:])
