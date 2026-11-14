#!/usr/bin/env python3
"""Generate docs/port-loop.html (success rate of the paid-API port loop per model) from docs/status/port_loop.json.

    python3 tools/gen_port_loop.py [--check] [out.html]

Data: docs/status/port_loop.json, written locally by tools/status_port_loop.py (the Pages build only reads the JSON).
Inline SVG/HTML, standard library only, light and dark through prefers-color-scheme (same tokens as the start page).
With --check nothing is written: exit status 1 if docs/port-loop.html is not what the JSON produces.
Colours: categorical slots 1-5 of the data-viz reference palette (fixed order = model order), validated light and dark; every
bar carries a visible label and a table view exists (the contrast relief rule).
"""
import html
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "docs", "status", "port_loop.json")
OUT = os.path.join(ROOT, "docs", "port-loop.html")
REPO = "https://github.com/Lu-Fi"
URL = "https://lu-fi.github.io/openimp/port-loop.html"

MODELS = [("deepseek-flash", "DeepSeek Flash"), ("deepseek-v4-pro", "DeepSeek Pro"), ("claude-sonnet-5-5", "Claude Sonnet"),
          ("claude-opus-5-5", "Claude Opus"), ("claude-fable-5-2", "Claude Fable")]
PROJECTS = ["T32", "T33", "T10 de-lift"]
SIZES = [("small", "up to 3 functions"), ("large", "more than 3 functions"), ("none", "size not recorded")]
SOURCES = {"api-deepseek-flash": ("API loop: DeepSeek Flash", 1), "api-deepseek-v4-pro": ("API loop: DeepSeek Pro", 1),
           "api-claude-sonnet-5-5": ("API loop: Claude Sonnet", 1), "api-claude-opus-5-5": ("API loop: Claude Opus", 1),
           "api-claude-fable-5-2": ("API loop: Claude Fable", 1), "api-unknown": ("API loop: model not recorded", 1),
           "agent-opus": ("Claude Code agents: Opus", 3), "agent-sonnet": ("Claude Code agents: Sonnet", 3),
           "agent-fable": ("Claude Code agents: Fable", 3), "agent-unknown": ("Claude Code agents: model not recorded", 3),
           "script": ("Deterministic script rewrite", 2), "no-port": ("Already readable, not ported", 0),
           "unknown": ("Unknown", 0)}
LOW_N = 5   # fewer attempts than this: the bar is flagged "few"

CSS = """
:root{--bg:#f5f6f8;--fg:#17202a;--mut:#5c6773;--card:#ffffff;--line:#dde2e8;--acc:#3e63dd;--acc-soft:#e8edfd;--shadow:0 1px 2px rgba(16,24,40,.06),0 1px 3px rgba(16,24,40,.08);
  --s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;--s5:#e87ba4;--track:#e6e9ee}
@media (prefers-color-scheme:dark){:root{--bg:#0f1216;--fg:#e6e9ee;--mut:#97a1ad;--card:#171b21;--line:#272d36;--acc:#7c9cff;--acc-soft:#1c2440;--shadow:none;
  --s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--s5:#d55181;--track:#262c35}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 Inter,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
.wrap{max-width:1080px;margin:0 auto;padding:0 16px}
header.top{position:sticky;top:0;z-index:5;background:color-mix(in srgb,var(--bg) 86%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
header.top .wrap{display:flex;align-items:center;gap:16px;height:56px}
.brand{display:flex;align-items:center;gap:10px;font-weight:650;color:var(--fg)}.brand img{width:26px;height:26px}
nav{margin-left:auto;display:flex;gap:4px;overflow-x:auto}
nav a{color:var(--mut);padding:6px 10px;border-radius:8px;font-size:.93em;white-space:nowrap}nav a:hover{background:var(--acc-soft);color:var(--acc);text-decoration:none}
.crumb{color:var(--mut);font-size:.9em;margin:22px 0 4px}h1{font-size:1.6em;margin:0 0 8px;line-height:1.2}
h2{font-size:1.2em;margin:28px 0 4px}.sub,.mut{color:var(--mut)}.sub{margin:0 0 14px;max-width:760px}
.rule{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 16px;box-shadow:var(--shadow);max-width:760px}
.rule p{margin:0 0 6px}.rule p:last-child{margin:0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,440px),1fr));gap:14px}
.pan{margin:0;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px 10px;box-shadow:var(--shadow);min-width:0}
.pan h3{font-size:.98em;margin:0 0 2px}.pan .n{color:var(--mut);font-size:.84em;margin:0 0 6px}
.pan.empty{color:var(--mut)}
svg.bars{display:block;width:100%;height:auto}
.bars .lab{fill:var(--fg);font-size:13px}.bars .val{fill:var(--fg);font-size:13px;font-weight:600;font-variant-numeric:tabular-nums}
.bars .cnt{fill:var(--mut);font-size:12px;font-variant-numeric:tabular-nums}
.bars .trk{fill:var(--track)}.bars .gl{stroke:var(--line);stroke-width:1}.bars .ax{fill:var(--mut);font-size:11px}
.bars .s1{fill:var(--s1)}.bars .s2{fill:var(--s2)}.bars .s3{fill:var(--s3)}.bars .s4{fill:var(--s4)}.bars .s5{fill:var(--s5)}.bars .sg{fill:var(--mut)}
.bars .row:hover .trk{fill:var(--line)}
i.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:baseline}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;color:var(--mut);font-size:.9em;margin:10px 0 0}
.tw{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:12px;box-shadow:var(--shadow);margin-top:8px}
table{border-collapse:collapse;width:100%;font-size:.9em}th,td{padding:7px 12px;border-bottom:1px solid var(--line);text-align:left}
th{color:var(--mut);font-weight:600;font-size:.88em}tr:last-child td{border-bottom:0}td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}
details{margin:12px 0}summary{cursor:pointer;color:var(--acc)}
ul.notes{margin:6px 0 0;padding-left:20px;max-width:760px}ul.notes li{margin:3px 0}
footer{border-top:1px solid var(--line);margin-top:28px;padding:20px 0 40px;color:var(--mut);font-size:.88em}
footer .wrap{display:flex;gap:12px;flex-wrap:wrap;justify-content:space-between}
"""

NOTES = [
    "One attempt is one run of the loop tool for one block of functions with one model; every run is counted, including re-runs "
    "of the same block (T32 ran more than one wave before the one-attempt-per-model rule existed).",
    "A DeepSeek run starts on Flash and moves to Pro inside the same run when the Flash rounds do not go green. So a run that "
    "escalated is counted twice: one failed Flash attempt and one Pro attempt with the result of the run. Flash passes are runs "
    "that went green on Flash alone; Pro attempts are every run that reached Pro, or started there for long reference code.",
    "Pass means the host test of the block went green (our C against the vendor code). Failed means it never did. An independent "
    "review of a passing T32 oracle sometimes listed gaps; those were closed by follow-up fixes, so the attempt stays a pass "
    "(the count is in the table view). No attempt is recorded as rejected in review.",
    "Follow-up runs that fix review gaps (T32) and the T32 runs of new blocks are included; follow-up runs have no function count "
    "and appear under \"size not recorded\".",
]


def load():
    return json.load(open(DATA, encoding="utf-8"))


def size_of(a):
    f = a.get("funcs")
    if f is None:
        return "none"
    return "small" if f <= 3 else "large"


def tally(att):
    """{model: [pass, n]} for the five charted models"""
    t = {m: [0, 0] for m, _ in MODELS}
    for a in att:
        if a["model"] in t:
            t[a["model"]][1] += 1
            if a["result"] == "pass":
                t[a["model"]][0] += 1
    return t


def pct(p, n):
    return "%d %%" % round(100.0 * p / n) if n else "-"


def panel(title, att, pid):
    """one horizontal bar panel: rows = models, bar = pass rate, label = passes/attempts"""
    t = tally(att)
    n_all = sum(v[1] for v in t.values())
    if not n_all:
        return '<div class="pan empty"><h3>%s</h3><p class="n">No attempts with a charted model.</p></div>' % html.escape(title)
    x0, x1, rh, top = 118, 330, 30, 6
    h = top + rh * len(MODELS) + 20
    o = ['<svg class="bars" viewBox="0 0 520 %d" role="img" aria-labelledby="%s-t"><title id="%s-t">%s: pass rate per model</title>' % (h, pid, pid, html.escape(title))]
    for g in (0, 50, 100):
        gx = x0 + (x1 - x0) * g / 100.0
        o.append('<line class="gl" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/><text class="ax" x="%.1f" y="%d" text-anchor="middle">%d %%</text>'
                 % (gx, gx, top, h - 18, gx, h - 5, g))
    for i, (m, label) in enumerate(MODELS):
        p, n = t[m]
        y = top + i * rh + 4
        tip = "%s: %d of %d attempts passed" % (label, p, n) if n else "%s: no attempts" % label
        o.append('<g class="row"><title>%s</title><rect x="0" y="%d" width="520" height="%d" fill="transparent"/>' % (html.escape(tip), y - 3, rh))
        o.append('<text class="lab" x="0" y="%d">%s</text>' % (y + 13, html.escape(label)))
        o.append('<rect class="trk" x="%d" y="%d" width="%d" height="14" rx="3"/>' % (x0, y + 2, x1 - x0))
        if n:
            w = (x1 - x0) * p / float(n)
            if w >= 1:
                r = min(4.0, w)   # rounded data end, flat at the baseline
                o.append('<path class="s%d" d="M%d %d h%.1f a%.1f %.1f 0 0 1 %.1f %.1f v%.1f a%.1f %.1f 0 0 1 -%.1f %.1f h-%.1f z"/>'
                         % (i + 1, x0, y + 2, w - r, r, r, r, r, 14 - 2 * r, r, r, r, r, w - r))
            few = " (few)" if n < LOW_N else ""
            o.append('<text class="val" x="%d" y="%d">%s</text><text class="cnt" x="%d" y="%d">%d/%d%s</text>'
                     % (x1 + 10, y + 13, pct(p, n), x1 + 56, y + 13, p, n, few))
        else:
            o.append('<text class="cnt" x="%d" y="%d">no attempts</text>' % (x1 + 10, y + 13))
        o.append('</g>')
    o.append('</svg>')
    return '<figure class="pan"><h3>%s</h3><p class="n">%d attempts with a charted model</p>%s</figure>' % (html.escape(title), n_all, "".join(o))


def contrib_panel(proj, c, pid):
    src = c["sources"]
    tot = sum(src.values())
    rows = [(k, v) for k, v in SOURCES.items() if src.get(k)]
    x0, x1, rh, top = 232, 400, 28, 4
    h = top + rh * len(rows) + 4
    o = ['<svg class="bars" viewBox="0 0 560 %d" role="img" aria-labelledby="%s-t"><title id="%s-t">%s: proven functions per source</title>' % (h, pid, pid, html.escape(proj))]
    for i, (k, (label, g)) in enumerate(rows):
        n = src[k]
        y = top + i * rh + 3
        o.append('<g class="row"><title>%s: %d of %d proven functions</title><rect x="0" y="%d" width="560" height="%d" fill="transparent"/>' % (html.escape(label), n, tot, y - 3, rh))
        o.append('<text class="lab" x="0" y="%d">%s</text><rect class="trk" x="%d" y="%d" width="%d" height="14" rx="3"/>' % (y + 13, html.escape(label), x0, y + 2, x1 - x0))
        w = (x1 - x0) * n / float(tot)
        r = min(4.0, w)
        cls = "sg" if g == 0 else "s%d" % g
        o.append('<path class="%s" d="M%d %d h%.1f a%.1f %.1f 0 0 1 %.1f %.1f v%.1f a%.1f %.1f 0 0 1 -%.1f %.1f h-%.1f z"/>' % (cls, x0, y + 2, w - r, r, r, r, r, 14 - 2 * r, r, r, r, r, w - r))
        o.append('<text class="val" x="%d" y="%d">%d</text><text class="cnt" x="%d" y="%d">%s</text></g>' % (x1 + 10, y + 13, n, x1 + 50, y + 13, pct(n, tot)))
    o.append("</svg>")
    return '<figure class="pan"><h3>%s</h3><p class="n">%d proven functions</p>%s</figure>' % (html.escape(proj), tot, "".join(o))


def contrib_table(cs):
    rows = []
    for proj in PROJECTS:
        c = cs.get(proj)
        if not c:
            continue
        for k, (label, _) in SOURCES.items():
            if c["sources"].get(k):
                rows.append("<tr><td>%s</td><td>%s</td><td class='num'>%d</td><td class='num'>%s</td></tr>" % (
                    html.escape(proj), html.escape(label), c["sources"][k], c.get("blocks", {}).get(k, "-")))
    return ('<div class="tw"><table><thead><tr><th>Project</th><th>Source</th><th class="num">Proven functions</th>'
            '<th class="num">Oracle blocks</th></tr></thead><tbody>%s</tbody></table></div>' % "".join(rows))


def table(att):
    rows = []
    for proj in PROJECTS:
        for m, label in MODELS:
            sub = [a for a in att if a["project"] == proj and a["model"] == m]
            if not sub:
                continue
            cells = []
            for sz, _ in SIZES:
                s = [a for a in sub if size_of(a) == sz]
                cells.append("%d/%d" % (sum(a["result"] == "pass" for a in s), len(s)) if s else "-")
            rev = sum(1 for a in sub if a.get("review") == "gaps")
            rows.append("<tr><td>%s</td><td>%s</td><td class='num'>%d/%d</td>%s<td class='num'>%d</td></tr>" % (
                html.escape(proj), html.escape(label), sum(a["result"] == "pass" for a in sub), len(sub),
                "".join("<td class='num'>%s</td>" % c for c in cells), rev))
    head = "<tr><th>Project</th><th>Model</th><th class='num'>All (passed/attempts)</th>%s<th class='num'>Passes with review gaps</th></tr>" % "".join(
        "<th class='num'>%s</th>" % html.escape(l) for _, l in SIZES)
    return '<div class="tw"><table><thead>%s</thead><tbody>%s</tbody></table></div>' % (head, "".join(rows))


def render(d):
    att = d["attempts"]
    dates = sorted(a["date"] for a in att if a.get("date"))
    charted = [a for a in att if a["model"] in dict(MODELS)]
    other = len(att) - len(charted)
    parts = [panel("All projects, all block sizes", charted, "all")]
    secs = []
    for proj in PROJECTS:
        pa = [a for a in charted if a["project"] == proj]
        if not pa:
            secs.append('<div class="pan empty"><h3>%s</h3><p class="n">No model attempts yet. The de-lift of T10 so far used a '
                        'deterministic rewrite script, which is not part of the paid loop.</p></div>' % html.escape(proj))
            continue
        for k, (sz, lab) in enumerate(SIZES):
            sa = [a for a in pa if size_of(a) == sz]
            if sa:
                secs.append(panel("%s, %s" % (proj, lab), sa, "p%s%d" % (proj[:3].lower().replace(" ", ""), k)))
    legend = "".join('<span><i class="sw" style="background:var(--s%d)"></i>%s</span>' % (i + 1, html.escape(l)) for i, (_, l) in enumerate(MODELS))
    notes = list(NOTES)
    if other:
        notes.append("%d attempts could not be assigned to a charted model (the log does not say whether a DeepSeek pass came from "
                     "Flash or Pro, or the model is not recorded). They are not charted and not guessed." % other)
    cs = d.get("contributors", {})
    contrib = "".join(contrib_panel(p, cs[p], "c%d" % i) for i, p in enumerate(PROJECTS) if p in cs and cs[p]["sources"])
    return PAGE % dict(css=CSS, contrib=contrib, ctable=contrib_table(cs), legend=legend, all=parts[0], secs="".join(secs), table=table(charted), repo=REPO,
                       n=len(att), d0=dates[0] if dates else "-", d1=dates[-1] if dates else "-", gen=html.escape(d.get("generated", "")),
                       notes="".join("<li>%s</li>" % html.escape(n) for n in notes), url=URL, low=LOW_N)


PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Port loop success rate</title>
<meta name="description" content="Success rate of the paid-API port loop per model (DeepSeek Flash/Pro, Claude Sonnet/Opus/Fable), split by project and block size.">
<link rel="icon" href="assets/logo.svg" type="image/svg+xml">
<style>%(css)s</style></head><body>
<header class="top"><div class="wrap">
  <a class="brand" href="./"><img src="assets/logo.svg" alt="">OpenIMP</a>
  <nav><a href="./">Start</a><a href="feature-matrix.html">Matrix</a><a href="#who">Who contributed</a><a href="#table">Table</a><a href="%(repo)s/openimp">GitHub</a></nav>
</div></header>
<main class="wrap">
<p class="crumb"><a href="./">Start</a> &rsaquo; Port loop</p>
<h1>Port loop: success rate per model</h1>
<p class="sub">Parts of the T32 and T33 kernel driver are ported from the recovered vendor code by a loop that asks a language model for the port and
runs a host test on it. This page shows how often each model's attempt passed that test: passes divided by attempts, with the attempt count next to every bar.
%(n)s attempts in total, %(d0)s to %(d1)s. Counts and rates only.</p>
<div class="rule"><p><b>Escalation rule.</b> Cheap first, one try per model: DeepSeek only for small blocks (about 3 functions or fewer), then Claude Sonnet,
then Claude Opus, then Claude Fable. A model never gets a second try on the same block; the next model starts from the files the previous one left behind.</p>
<p class="mut">The T32 waves before this rule was set ran DeepSeek on larger blocks too, which is why the large-block DeepSeek bars are mostly failures.</p></div>
<h2>All attempts</h2>
<div class="grid">%(all)s</div>
<div class="legend">%(legend)s</div>
<h2>By project and block size</h2>
<p class="sub">T32 is the recovered-C port with host oracles, T33 uses the binary oracle, T10 de-lift is the readable-C rework of the T10 module.
Blocks are split at 3 functions. Bars with fewer than %(low)d attempts are marked "few": read them as anecdotes, not rates.</p>
<div class="grid">%(secs)s</div>
<h2 id="who">Who contributed</h2>
<p class="sub">Proven functions per source: a function counts once, for the oldest merged oracle block that proves it (host test green, merged into the driver).
The API loop above is split by model. Claude Code agents (hand ports on the plan budget) are split by the model named in their commit; scripts are deterministic
rewrites. <b>Agents get no success rate</b> because they keep working on a block until its test passes, so every agent block ends as a pass and there is no failed attempt to count.
A block whose source the logs do not settle is listed as not recorded or unknown, never guessed.</p>
<div class="grid">%(contrib)s</div>
<details><summary>Table view: proven functions per source</summary>%(ctable)s</details>
<h2 id="table">Table view</h2>
<details open><summary>Passed / attempts per project, model and block size</summary>%(table)s</details>
<h2>How the numbers were counted</h2>
<ul class="notes">%(notes)s</ul>
</main>
<footer><div class="wrap"><span>Generated from <a href="%(repo)s/openimp/blob/next/docs/status/port_loop.json">docs/status/port_loop.json</a> (data of %(gen)s, written by tools/status_port_loop.py).</span>
<span><a href="./">Start page</a> &middot; <a href="feature-matrix.html">Feature matrix</a></span></div></footer>
</body></html>
"""


def main():
    a = sys.argv[1:]
    check = "--check" in a
    a = [x for x in a if x != "--check"]
    out = a[0] if a else OUT
    page = render(load())
    if check:
        cur = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
        if cur != page:
            print("docs/port-loop.html is stale: run python3 tools/gen_port_loop.py")
            sys.exit(1)
        return
    with open(out, "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", out)


if __name__ == "__main__":
    main()
