#!/usr/bin/env python3
"""Structured implementation status: load, validate, render (docs/status/*).

Source of truth for the hand-maintained part of docs/FEATURE_MATRIX.md:

  docs/status/features.yaml   feature rows x SoCs: symbol, detail text, issue numbers, evidence refs,
                              legend, groups, links from API functions to issues
  docs/status/history.md      the long prose (intro, "Summary" update log, notes under the matrix table), verbatim
  docs/status/issues.json     cache of the public GitHub issues (refresh: tools/status_data.py --refresh-issues)

docs/FEATURE_MATRIX.md and docs/feature-matrix.html are generated from these files plus docs/api_evidence.json
(tools/gen_feature_matrix_html.py); do not edit them by hand.

Usage:
  status_data.py --check             validate the data (unknown symbols, missing SoCs, dangling issue
                                     references, API links); exit status 1 on any problem
  status_data.py --refresh-issues    refresh docs/status/issues.json with `gh api` (needs the gh CLI)
  status_data.py --print-head        print the generated head of FEATURE_MATRIX.md (everything before the API section)

Needs PyYAML (python3-yaml / `pip install pyyaml`); the other generators use the standard library only.
"""
import json
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:                                     # pragma: no cover
    sys.exit("status_data: PyYAML is required (pip install pyyaml)")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDIR = os.path.join(ROOT, "docs", "status")
FEATURES = os.path.join(SDIR, "features.yaml")
HISTORY = os.path.join(SDIR, "history.md")
ISSUES = os.path.join(SDIR, "issues.json")
RELEASE = os.path.join(SDIR, "release.yaml")
SOAK = os.path.join(SDIR, "soak.json")
REPO = "Lu-Fi/openimp"

SOCS = ["T10", "T20", "T21", "T23", "T30", "T31", "T32", "T33", "T40", "T41"]
# status symbols of the matrix (legend in features.yaml); '⚠' and '⚠️' are the same symbol
SYMBOLS = ["✅", "✅+", "⚠️", "🔧", "📋", "❌", "—", "?"]
CELL_KEYS = {"sym", "text", "issues", "evidence", "table_sym"}
FEATURE_KEYS = {"n", "title", "group", "detail_group", "group_head", "anchor", "vendor", "cells", "pre", "extra"}
LABELS = ["soc:" + s for s in SOCS] + [
    "area:" + a for a in ("ISP", "AE-AWB", "Encoder", "FrameSource", "OSD", "IVS", "Audio", "System", "Driver", "Docs")] + [
    "type:gap", "type:bug", "type:wip", "prio:release-gate", "prio:streamer", "prio:long-term",
    "repo:openimp", "repo:open-tx-isp"]


def norm_sym(s):
    return s.replace("️", "")


def slug_anchor(heading):
    """GitHub heading anchor: lower case, punctuation dropped, spaces to hyphens."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s, flags=re.U)
    return s.replace(" ", "-")


# ------------------------------------------------------------------ load
def load_features(path=FEATURES):
    with open(path, encoding="utf-8") as f:
        return yaml.load(f, Loader=getattr(yaml, "CSafeLoader", yaml.SafeLoader))


def load_release(path=RELEASE):
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def load_history(path=HISTORY):
    """docs/status/history.md -> {part name: text}; parts start with a line '<!-- part: NAME -->'."""
    parts, cur = {}, None
    for ln in open(path, encoding="utf-8").read().split("\n"):
        m = re.match(r"^<!-- part: (\S+) -->$", ln)
        if m:
            cur = m.group(1)
            parts[cur] = []
        elif cur is not None:
            parts[cur].append(ln)
    return {k: "\n".join(v).strip("\n") for k, v in parts.items()}


def load_issues(path=ISSUES):
    """Cache of the GitHub issues -> {number: {title, state, labels, url, created}}."""
    if not os.path.exists(path):
        return {}
    d = json.load(open(path, encoding="utf-8"))
    return {int(i["number"]): i for i in d.get("issues", [])}


# ------------------------------------------------------------------ render
def render_head(data, hist):
    """The hand-maintained part of docs/FEATURE_MATRIX.md (everything before '## API coverage per function')."""
    socs = data["socs"]
    o = ["# %s" % data["title"], "",
         "<!-- docs/feature-matrix.html and this file are generated: python3 tools/gen_feature_matrix_html.py "
         "from docs/status/features.yaml, docs/status/history.md and docs/api_evidence.json (do not edit by hand; "
         "edit the data files, regenerate after every change) -->", "",
         hist["intro"], "", "## Summary", "", hist["summary"], "", "## Legend", ""]
    o += ["- " + t for t in data["legend"]]
    o += ["", "## Matrix", "", "| # | Feature | %s |" % " | ".join(socs),
          "|--:|---|%s|" % "|".join(["---"] * len(socs))]
    grp = None
    for f in data["features"]:
        if f["group"] != grp:
            grp = f["group"]
            o.append("|  | **%s** |%s|" % (grp, "|".join(["  "] * len(socs))))
        anchor = f.get("anchor") or slug_anchor("%d. %s" % (f["n"], f["title"]))
        syms = " | ".join(f["cells"][s].get("table_sym", f["cells"][s]["sym"]) for s in socs)
        o.append("| %d | [%s](#%s) | %s |" % (f["n"], f["title"], anchor, syms))
    o += ["", hist["matrix-notes"], "", "### Matrix details", "", data["details_intro"], "", ""]
    dgrp = None
    for f in data["features"]:
        g = f.get("detail_group", f["group"])
        if g != dgrp:
            dgrp = g
            o += ["#### %s" % g]
            if f.get("group_head"):
                o.append(f["group_head"])
            o.append("")
        o += ["##### %d. %s" % (f["n"], f["title"]), "", "- **Vendor stack:** %s" % f["vendor"]]
        if f.get("pre"):
            o.append(f["pre"])
        for s in socs:
            c = f["cells"][s]
            o.append("- **%s:** %s%s" % (s, c["sym"], (" " + c["text"]) if c.get("text") else ""))
        if f.get("extra"):
            o.append(f["extra"])
        o.append("")
    o.append("")
    return "\n".join(o)


# ------------------------------------------------------------------ issues in the generators
def cell_issue_links(data, issues):
    """{(feature number, soc): [issue dict ...]} for the cells that reference issues (unknown numbers skipped)."""
    res = {}
    for f in data["features"]:
        for s, c in f["cells"].items():
            for n in c.get("issues") or []:
                if n in issues:
                    res.setdefault((f["n"], s), []).append(dict(issues[n], number=n))
    return res


def is_wip(issue):
    """Open issue in progress: label type:wip (the project column "In progress" mirrors it)."""
    return issue.get("state") == "open" and "type:wip" in issue.get("labels", [])


def wip_items(data, issues):
    """Work-in-progress items in the shape the generators used for docs/matrix_wip.json (construction sign)."""
    items = []
    for f in data["features"]:
        for s, c in f["cells"].items():
            for n in c.get("issues") or []:
                i = issues.get(n)
                if i and is_wip(i):
                    items.append({"soc": s, "feature": f["n"], "branch": "#%d" % n, "since": (i.get("created") or "")[:10],
                                  "note": i.get("title", ""), "url": i.get("url", "")})
    for lk in data.get("api_links") or []:
        for n in lk.get("issues") or []:
            i = issues.get(n)
            if i and is_wip(i):
                for fn in lk.get("functions") or []:
                    items.append({"soc": lk["soc"], "function": fn, "branch": "#%d" % n,
                                  "since": (i.get("created") or "")[:10], "note": i.get("title", ""), "url": i.get("url", "")})
    return items


def func_issue_links(data, issues):
    """{(API row name, soc): [issue dict ...]} from the api_links section."""
    res = {}
    for lk in data.get("api_links") or []:
        for n in lk.get("issues") or []:
            if n in issues:
                for fn in lk.get("functions") or []:
                    res.setdefault((fn, lk["soc"]), []).append(dict(issues[n], number=n))
    return res


# ------------------------------------------------------------------ validate
def validate(data=None, hist=None, issues=None, functions=None):
    """List of problems (strings); empty = ok."""
    E = []
    data = data if data is not None else load_features()
    hist = hist if hist is not None else load_history()
    issues = issues if issues is not None else load_issues()
    for k in ("title", "socs", "legend", "details_intro", "features"):
        if k not in data:
            E.append("features.yaml: missing key %s" % k)
    if E:
        return E
    if data["socs"] != SOCS:
        E.append("features.yaml: socs must be %s" % SOCS)
    for k in ("intro", "summary", "matrix-notes"):
        if not hist.get(k):
            E.append("history.md: missing part '%s'" % k)
    known = {norm_sym(s) for s in SYMBOLS}
    legend_syms = set()
    for t in data["legend"]:
        m = re.match(r"^(\S+)", t)
        if m:
            legend_syms.add(norm_sym(m.group(1)))
    for s in known - legend_syms:
        E.append("legend: no entry for symbol %s" % s)
    seen = set()
    for f in data["features"]:
        n = f.get("n")
        where = "feature %s" % n
        for k in f:
            if k not in FEATURE_KEYS:
                E.append("%s: unknown key %s" % (where, k))
        if n in seen:
            E.append("%s: duplicate number" % where)
        seen.add(n)
        for k in ("title", "group", "vendor", "cells"):
            if k not in f:
                E.append("%s: missing %s" % (where, k))
        cells = f.get("cells", {})
        for s in SOCS:
            if s not in cells:
                E.append("%s: missing SoC %s" % (where, s))
        for s in cells:
            if s not in SOCS:
                E.append("%s: unknown SoC %s" % (where, s))
        for s, c in cells.items():
            for k in c:
                if k not in CELL_KEYS:
                    E.append("%s %s: unknown key %s" % (where, s, k))
            if norm_sym(c.get("sym", "")) not in known:
                E.append("%s %s: unknown symbol %r" % (where, s, c.get("sym")))
            for i in c.get("issues") or []:
                if i not in issues:
                    E.append("%s %s: issue #%s is not in docs/status/issues.json (dangling reference; refresh the cache)" % (where, s, i))
    nums = [f["n"] for f in data["features"] if "n" in f]
    if nums != sorted(nums):
        E.append("features are not in ascending order")
    for lk in data.get("api_links") or []:
        if lk.get("soc") not in SOCS:
            E.append("api_links: unknown SoC %s" % lk.get("soc"))
        for fn in lk.get("functions") or []:
            if functions is not None and fn not in functions:
                E.append("api_links %s: unknown function %s" % (lk.get("soc"), fn))
        for i in lk.get("issues") or []:
            if i not in issues:
                E.append("api_links %s: issue #%s is not in docs/status/issues.json" % (lk.get("soc"), i))
    for n, i in issues.items():
        for lab in i.get("labels", []):
            if lab not in LABELS and lab.startswith(("soc:", "area:", "type:", "prio:", "repo:")):
                E.append("issues.json: #%d has the unknown label %s" % (n, lab))
    return E


def validate_release(rel=None, soak=None):
    """Problems in docs/status/release.yaml and docs/status/soak.json (empty list = ok)."""
    E = []
    try:
        rel = rel if rel is not None else load_release()
    except (OSError, yaml.YAMLError) as e:
        return ["release.yaml: %s" % e]
    for k in ("version", "socs", "fleet", "gates"):
        if k not in rel:
            E.append("release.yaml: missing key %s" % k)
    if E:
        return E
    socs = rel["socs"]
    for s in socs:
        if s not in SOCS:
            E.append("release.yaml: unknown SoC %s" % s)
    for cam, s in rel["fleet"].items():
        if not re.fullmatch(r"\.\d{1,3}", str(cam)):
            E.append("release.yaml: fleet camera %r must be the '.N' shorthand (no full LAN address)" % cam)
        if s not in socs:
            E.append("release.yaml: fleet camera %s has SoC %s that is not a gate SoC" % (cam, s))
    for s in socs:
        if s not in rel["fleet"].values():
            E.append("release.yaml: gate SoC %s has no camera in the fleet" % s)
    ids, gids = set(), set()
    for g in rel["gates"]:
        gids.add(g.get("id"))
        for k in ("id", "title", "milestone", "rules"):
            if k not in g:
                E.append("release.yaml: gate %s: missing %s" % (g.get("id"), k))
        for r in g.get("rules", []):
            if r.get("id") in ids:
                E.append("release.yaml: duplicate rule id %s" % r.get("id"))
            ids.add(r.get("id"))
            if not r.get("text"):
                E.append("release.yaml: rule %s has no text" % r.get("id"))
            for k in ("min_stable_days", "min_cameras_per_soc", "max_unplanned_reboots", "max_oops"):
                if k in r and (not isinstance(r[k], int) or r[k] < 0):
                    E.append("release.yaml: rule %s: %s must be a non-negative integer" % (r.get("id"), k))
    rc3 = next((x for x in rel["gates"] if x.get("id") == "rc3"), None)
    if not rc3 or not rc3.get("works_cells"):
        E.append("release.yaml: gate rc3 with works_cells missing")
    r1 = next((x for x in rel["gates"] if x.get("id") == "release1"), None)
    if not r1:
        E.append("release.yaml: gate release1 missing")
    else:
        have = {k for r in r1.get("rules", []) for k in r}
        for k in ("min_stable_days", "min_cameras_per_soc", "max_unplanned_reboots", "max_oops", "planned_marker"):
            if k not in have:
                E.append("release.yaml: gate release1: threshold %s missing" % k)
        if r1.get("requires") not in gids:
            E.append("release.yaml: gate release1: requires an existing gate")
    if soak is None and os.path.exists(SOAK):
        try:
            soak = json.load(open(SOAK, encoding="utf-8"))
        except ValueError as e:
            return E + ["soak.json: %s" % e]
    if soak is not None:
        for k in ("image", "asof", "cameras", "days"):
            if k not in soak:
                E.append("soak.json: missing key %s" % k)
        for c in soak.get("cameras", []):
            w = "soak.json %s" % c.get("camera")
            for k in ("camera", "soc", "image", "first_seen", "uptime_s", "unplanned_reboots", "oops", "last_reset", "stable_days", "events"):
                if k not in c:
                    E.append("%s: missing %s" % (w, k))
            if rel["fleet"].get(c.get("camera")) != c.get("soc"):
                E.append("%s: camera/SoC does not match release.yaml fleet" % w)
            if re.search(r"192\.168\.|\b10\.\d+\.\d+\.\d+", json.dumps(c)):
                E.append("%s: contains a full LAN address (use the .N shorthand)" % w)
    return E


# ------------------------------------------------------------------ gh
def gh(*args):
    return subprocess.run(["gh"] + list(args), check=True, capture_output=True, text=True).stdout


def refresh_issues(path=ISSUES):
    """Fetch all issues (open and closed, no pull requests) of the repository with the gh CLI."""
    out = gh("api", "--paginate", "repos/%s/issues?state=all&per_page=100" % REPO)
    # --paginate prints one JSON array per page
    arrs = [json.loads(x) for x in re.split(r"(?<=\])\s*(?=\[)", out.strip())] if out.strip() else []
    res = []
    for a in arrs:
        for i in a:
            if "pull_request" in i:
                continue
            res.append({"number": i["number"], "title": i["title"], "state": i["state"], "url": i["html_url"],
                        "labels": sorted(l["name"] for l in i["labels"]), "created": i["created_at"],
                        "closed": i.get("closed_at"),
                        "milestone": (i.get("milestone") or {}).get("title")})
    res.sort(key=lambda x: x["number"])
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"_comment": "Cache of the public GitHub issues of %s (refresh: python3 tools/status_data.py "
                               "--refresh-issues). Read by the matrix generators; do not edit by hand." % REPO,
                   "issues": res}, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("wrote %s (%d issues, %d open)" % (path, len(res), sum(1 for i in res if i["state"] == "open")))


def main():
    a = sys.argv[1:]
    if "--refresh-issues" in a:
        refresh_issues()
        return
    if "--print-head" in a:
        sys.stdout.write(render_head(load_features(), load_history()))
        return
    funcs = None
    ev = os.path.join(ROOT, "docs", "api_evidence.json")
    if os.path.exists(ev):
        funcs = set(json.load(open(ev, encoding="utf-8")).get("functions", {}))
    E = validate(functions=funcs) + validate_release()
    for e in E:
        sys.stderr.write("status_data: %s\n" % e)
    if E:
        sys.exit(1)
    d = load_features()
    print("status data ok: %d features x %d SoCs, %d issues in the cache" % (len(d["features"]), len(d["socs"]), len(load_issues())))


if __name__ == "__main__":
    main()
