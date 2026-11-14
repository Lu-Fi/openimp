#!/usr/bin/env python3
"""Generate docs/FEATURE_MATRIX.md and docs/feature-matrix.html from the status data.

    python3 tools/gen_feature_matrix_html.py [--check] [--baseline old.md] [in.md [out.html]]

Inputs: docs/status/features.yaml, docs/status/history.md, docs/status/issues.json (see tools/status_data.py)
and docs/api_evidence.json / docs/api_exports.json (see tools/gen_api_coverage.py).  The Markdown is written
first (head from the status data + the generated API section), the HTML is rendered from it.  With --check
nothing is written: exit status 1 if docs/FEATURE_MATRIX.md is not what the data produces, or on warnings.
With explicit in.md the HTML is rendered from that Markdown file only (baseline tags etc.).

Python 3 standard library only (no markdown module): the small subset of
Markdown used by FEATURE_MATRIX.md is parsed here (headings, paragraphs,
bullet/numbered lists, pipe tables, code fences, <details> blocks, inline
code/bold/italic/links).  The look (style block, legend, sticky table, light
and dark tokens) is the same as the former hand-made HTML.

Special handling:
  * "## Matrix" table + "### Matrix details" bullets are merged into one
    table (vendor column + one cell per SoC, class s-ok/s-plus/... from the
    symbol in the table); extra text of a detail section goes into a
    <details id="slug"> block below the table.
  * Function tables (first column "Vendor function") get cell classes from
    the cell codes (dev, host, aud, ...), data-gap rows and a "only rows with
    gaps" filter.
  * "Progress per SoC" gets a bar column computed from the counts.
  * Release badges: RELEASES is a list of (tag, label), oldest first (a new release is one line).
    Every matrix cell symbol is compared with the matrix at each tag (`git show TAG:docs/FEATURE_MATRIX.md`;
    --baseline FILE or the env var BASELINE_MD replaces the first one).  A cell that is better than at
    the previous release (rank ? < X < planned < in progress < defect < supported < beyond vendor;
    "—" -> anything counts as better) gets the badge of the release in which it improved (the label of
    the interval's end tag); one that improved after the last tag gets "new".  The latest improvement
    wins.  Cells that changed in some interval without improving get a muted "changed" badge.  Before
    the first tag there is nothing to compare with, so there is no badge for it.  A select filters the
    rows by badge and the summary table counts per SoC and interval.  Without baselines nothing is marked.
  * Date badges (on top of the release ones): "today" = the cell changed since the last commit of
    docs/FEATURE_MATRIX.md before today 00:00 (local time, env TZ), "yday" = it changed between the last commit
    before yesterday 00:00 and the one before today 00:00.  Filled badge = improved, outlined = changed without
    improving.  Computed at generation time from git history (`git rev-list -1 --before=...`), so the hourly Pages
    rebuild keeps them current (the workflow checks out the full history).  The API function tables mark cells whose
    code in docs/api_evidence.json differs between the same commits (filter "changed today / yesterday").
"""
import html
import json
import os
import re
import datetime
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_api_coverage as api

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "docs", "FEATURE_MATRIX.md")
DST = os.path.join(ROOT, "docs", "feature-matrix.html")
# (tag, label), oldest first.  RC4 / Release 1 later: append one line.
RELEASES = [("v2026.10.11-rc1", "RC1"), ("v2026.10.25-rc2", "RC2"), ("v2026.11.07-rc3", "RC3")]
MATRIX_COUNTS = {}
SYMS = []          # per release: feature title -> {soc: symbol} at that tag
TXTS = []          # per release: feature number -> {soc: detail text without the symbol}
DSYMS = []         # date baselines [before yesterday, before today]: feature title -> {soc: symbol}
DTXTS = []         # same for the detail texts
DREVS = []         # the two commits (None if the history has none)
RANK = {"?": 0, "❌": 1, "📋": 2, "🔧": 3, "⚠": 4, "✅": 5, "✅+": 6}

CSS = api.API_CSS + r"""
:root{--bg:#f6f7f9;--fg:#1c2128;--muted:#5b6573;--card:#fff;--line:#d9dee5;--head:#eef1f5;--accent:#2457c5;
--ok:#e3f4e8;--plus:#cdeed8;--bug:#fde8c8;--wip:#dfe8fb;--plan:#ebe4f7;--miss:#fbdada;--na:#eceff2;--unk:#f4f4f4;}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#14171c;--fg:#e6e9ee;--muted:#9aa4b2;--card:#1c2027;--line:#333a45;--head:#232932;--accent:#8fb2ff;
--ok:#1d3326;--plus:#18432b;--bug:#4a3818;--wip:#1f2d4d;--plan:#34294d;--miss:#4a2323;--na:#262b32;--unk:#22262c;}}
:root[data-theme="dark"]{--bg:#14171c;--fg:#e6e9ee;--muted:#9aa4b2;--card:#1c2027;--line:#333a45;--head:#232932;--accent:#8fb2ff;
--ok:#1d3326;--plus:#18432b;--bug:#4a3818;--wip:#1f2d4d;--plan:#34294d;--miss:#4a2323;--na:#262b32;--unk:#22262c;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:none;margin:0 auto;padding:16px}
h1{font-size:1.45rem;margin:.2rem 0 .1rem}h2{font-size:1.1rem;margin:1.4rem 0 .5rem}
h3{font-size:1rem;margin:1.1rem 0 .4rem}h4{font-size:.92rem;margin:.9rem 0 .3rem;color:var(--accent)}
.meta{color:var(--muted);font-size:.85rem;margin:0 0 .8rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:0 0 12px}
.card ul{margin:.3rem 0 0;padding-left:1.1rem}.card li{margin:.25rem 0}
.legend{display:flex;flex-wrap:wrap;gap:8px}
.legend span{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:8px;padding:3px 8px;font-size:.85rem}
.legend small{color:var(--muted)}.legend span{font-size:.78rem;padding:1px 7px}.legend small{display:none}
.wrap{overflow-x:auto;border:1px solid var(--line);border-radius:10px;background:var(--card);max-width:100%}
table{border-collapse:separate;border-spacing:0;width:100%;table-layout:fixed;font-size:.8rem}
th,td{padding:6px 8px;border-bottom:1px solid var(--line);border-right:1px solid var(--line);vertical-align:top;text-align:left}
thead th{position:sticky;top:0;background:var(--head);z-index:3}
th:first-child,td:first-child{position:sticky;left:0;z-index:2;background:var(--card);width:11%;font-weight:600}
thead th:first-child{z-index:4;background:var(--head)}
td.vend{color:var(--muted);font-size:.76rem}th:nth-child(2){width:10%}td{overflow-wrap:anywhere}@media (max-width:1100px){table{width:max-content;table-layout:auto}th:first-child,td:first-child{min-width:140px;max-width:140px}td.cellc,td[class^="s-"]{min-width:165px;max-width:200px}}

tr.grp td{background:var(--head);font-weight:700;color:var(--accent);position:sticky;left:0}
.sym{display:inline-block;margin-right:5px;font-weight:700;white-space:nowrap}
.s-ok{background:var(--ok)}.s-plus{background:var(--plus)}.s-bug{background:var(--bug)}.s-wip{background:var(--wip)}
.s-plan{background:var(--plan)}.s-miss{background:var(--miss)}.s-na{background:var(--na)}.s-unk{background:var(--unk)}
td:first-child a{color:inherit;text-decoration:none;border-bottom:1px dotted var(--muted)}
details{border-top:1px solid var(--line);padding:6px 0}details summary{cursor:pointer;font-weight:600}
details p{margin:.4rem 0 .2rem;color:var(--muted)}
.small{font-size:.82rem;color:var(--muted)}
th small{font-weight:400;color:var(--muted)}
table.mi{table-layout:auto;width:100%}table.mi th:nth-child(2){width:auto}table.mi td:first-child{width:auto;min-width:16rem;font-weight:400}table.mi td.nt{font-size:.74rem;color:var(--muted)}table.mi td.c{text-align:center;white-space:nowrap}.lg{display:flex;flex-wrap:wrap;gap:3px 10px;font-size:.74rem;color:var(--muted);margin:.35rem 0 .5rem}.lg b{color:var(--fg);padding:0 6px;border-radius:5px;border:1px solid var(--line);font-weight:600}.lg b.sy{background:var(--card)}.pb{display:flex;height:14px;min-width:240px;border:1px solid var(--line);border-radius:4px;overflow:hidden;background:var(--na)}.pb i{display:block;height:100%}.b-dev{background:#2e9e5b}.b-host{background:#4f7fd9}.b-q{background:#a9b1bd}.b-noop{background:#9b82d0}.b-gap{background:#d9534f}details.full{border:1px solid var(--line);border-radius:8px;padding:6px 10px;margin:.4rem 0 1rem;background:var(--card)}details.full>summary{font-weight:600}details.full label{font-size:.8rem;color:var(--muted);margin-left:1rem}
td.cellc{overflow-wrap:anywhere}pre{overflow-x:auto;background:var(--head);padding:8px;border-radius:6px;font-size:.8rem}
.card ol,details ol{padding-left:1.4rem}details li{margin:.2rem 0}
.rc2{display:inline-block;margin-left:5px;padding:0 5px;border-radius:8px;background:var(--accent);color:var(--bg);font-size:.66rem;font-weight:700;vertical-align:middle}
.rc2.chg{background:var(--muted)}
td.new{box-shadow:inset 3px 0 0 var(--accent)}
.rc2bar{border:1px solid var(--line);border-radius:10px;background:var(--card);padding:8px 12px;margin:0 0 10px;font-size:.85rem}
.rc2bar table{width:auto;table-layout:auto;margin:.4rem 0}.rc2bar th,.rc2bar td{position:static;width:auto;font-weight:400;text-align:center;padding:3px 10px}
.rc2bar td:first-child,.rc2bar th:first-child{text-align:left;font-weight:600}
table.mx{table-layout:auto;width:max-content;min-width:100%;font-size:.82rem}
table.mx th,table.mx td{padding:3px 6px;vertical-align:middle}
table.mx th:first-child,table.mx td:first-child{width:auto;min-width:300px;max-width:520px;font-weight:400}
table.mx thead th{text-align:center;position:sticky;top:0}
table.mx thead th.fh{text-align:left}
table.mx tbody th{position:sticky;left:0;z-index:2;background:var(--card);text-align:left}
table.mx thead th:first-child{z-index:5}
table.mx td.mc{padding:2px 4px}table.mx tr.grp td{padding:2px 8px;font-size:.78rem}
.mwrap{max-height:76vh;overflow:auto;border:1px solid var(--line);border-radius:10px;background:var(--card)}
.mwrap .wrap{border:0}
td.mc{text-align:center;white-space:nowrap;cursor:pointer;min-width:52px;outline-offset:-2px}
td.mc .sym{margin:0}td.mc:hover{filter:brightness(.94)}td.mc:focus-visible,.fname:focus-visible,.tag:focus-visible,.chip:focus-visible{outline:2px solid var(--accent)}
td.mc.dim{opacity:.3}td.mc.sel{box-shadow:inset 0 0 0 2px var(--accent)}tr.selr th{box-shadow:inset 3px 0 0 var(--accent)}
td.mc i.rc2{font-style:normal;margin-left:3px;padding:0 4px;font-size:.6rem}
i.rc2{display:inline-block;padding:0 5px;border-radius:8px;background:var(--accent);color:var(--bg);font-size:.66rem;font-weight:700;font-style:normal;vertical-align:middle}
i.dtb{display:inline-block;margin-left:3px;padding:0 4px;border-radius:8px;font-size:.6rem;font-weight:700;font-style:normal;vertical-align:middle;color:#fff;background:var(--dc);box-shadow:inset 0 0 0 1px var(--dc)}
i.dtb.t{--dc:#1a8f5a}i.dtb.y{--dc:#6a4fc8}i.dtb.c{background:transparent;color:var(--dc)}
td.mc[data-dt]{box-shadow:inset 0 -3px 0 var(--dcol,#1a8f5a)}td.mc[data-dt^="y"]{--dcol:#6a4fc8}
i.rc2.chg{background:var(--muted)}i.rc2.nw{background:#d9822b;color:#fff}i.rc2.old{background:transparent;color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.lgbar{display:flex;flex-wrap:wrap;gap:4px 6px;border:1px solid var(--line);border-radius:10px;background:var(--card);padding:6px 8px;margin:0 0 8px;font-size:.78rem}
.lgi{border:1px solid var(--line);border-radius:8px;padding:1px 7px;cursor:help;display:inline-flex;align-items:center;gap:4px}.lgi:focus-visible{outline:2px solid var(--accent)}
.num{display:inline-block;min-width:1.6rem;color:var(--muted);font-size:.74rem}
.fname{background:none;border:0;padding:0;color:inherit;font:inherit;cursor:pointer;text-align:left;border-bottom:1px dotted var(--muted)}
.tags{margin-left:6px}
.tag{background:var(--head);border:1px solid var(--line);border-radius:8px;color:var(--muted);font-size:.64rem;padding:0 5px;margin:1px 3px 0 0;cursor:pointer}
.tag.on{background:var(--accent);color:var(--bg);border-color:var(--accent)}
.mxctl{border:1px solid var(--line);border-radius:10px;background:var(--card);padding:6px 10px;margin:0 0 8px}
.fg{display:flex;flex-wrap:wrap;align-items:center;gap:4px 5px;margin:3px 0}.fl{font-size:.74rem;color:var(--muted);min-width:3.2rem}
.chip{background:var(--card);border:1px solid var(--line);border-radius:8px;color:var(--fg);font:inherit;font-size:.78rem;padding:1px 8px;cursor:pointer}
.chip[aria-pressed="true"]{background:var(--accent);color:var(--bg);border-color:var(--accent)}
#mxq{font:inherit;font-size:.85rem;padding:3px 8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);min-width:min(280px,100%)}
.rc2bar{margin:0 0 8px;padding:6px 12px}.rc2bar summary{cursor:pointer;font-weight:600;font-size:.85rem}
.rc2bar table{font-size:.78rem}.rc2bar tr.sep td{border-top:2px solid var(--line)}
#tip{position:fixed;z-index:50;max-width:min(380px,calc(100vw - 16px));background:var(--fg);color:var(--bg);border-radius:8px;padding:6px 9px;font-size:.78rem;pointer-events:none;box-shadow:0 4px 14px #0005}
#tip .small{color:inherit;opacity:.7}#tip div{margin-top:2px}
#pn{position:fixed;z-index:40;top:0;right:0;bottom:0;width:min(520px,46vw);background:var(--card);border-left:1px solid var(--line);box-shadow:-6px 0 20px #0003;overflow:auto;padding:10px 14px}
#pn[hidden]{display:none}
.pnh{display:flex;justify-content:space-between;gap:8px;align-items:flex-start;position:sticky;top:-10px;background:var(--card);padding:6px 0;margin-top:-10px;border-bottom:1px solid var(--line)}
.pnh h3{margin:.2rem 0;font-size:.98rem}
#pnx{font-size:1.5rem;line-height:1;background:none;border:1px solid var(--line);border-radius:8px;color:var(--fg);cursor:pointer;padding:0 9px}
.cd{border-top:1px solid var(--line);padding:6px 0}.cd h4{margin:.2rem 0}.cd .tx{font-size:.84rem;margin:.3rem 0}
@media (max-width:1150px){table.mx th:first-child,table.mx td:first-child{min-width:210px;max-width:230px}.tags{display:block;margin-left:1.6rem}td.mc{min-width:46px}}
@media (min-width:801px){body.pnopen main{margin-right:min(520px,46vw)}}
@media (max-width:800px){#pn{top:auto;left:0;width:auto;height:68vh;border-left:0;border-top:1px solid var(--line);border-radius:12px 12px 0 0}
 table.mx th:first-child,table.mx td:first-child{min-width:150px;max-width:150px;font-size:.76rem}.tags{display:none}.mwrap{max-height:70vh}main{padding:10px}}
.wipm{font-size:.8em;margin-left:2px}
details.hist{border:1px solid var(--line);border-radius:10px;background:var(--card);padding:8px 14px;margin:1.4rem 0}details.hist>summary{font-size:1.05rem;font-weight:700}
"""

# status symbol -> (css class, title)
SYM = {
    "✅": ("s-ok", "supported"),
    "✅+": ("s-plus", "improved beyond vendor"),
    "⚠": ("s-bug", "known defect / deviation"),
    "🔧": ("s-wip", "in progress"),
    "📋": ("s-plan", "planned"),
    "❌": ("s-miss", "missing"),
    "—": ("s-na", "hardware does not have it"),
    "?": ("s-unk", "unknown"),
}
SYM_RE = re.compile(r"^(✅\+|✅|⚠️?|🔧|📋|❌|—|\?)(?:\s+|$)")

# function-table cell code -> (class, long label)
CODE = {
    "dev": ("s-ok", "done (dev)"),
    "host": ("s-wip", "done (host)"),
    "aud": ("s-unk", "done (?)"),
    "no-op": ("s-plan", "vendor no-op"),
    "nohw": ("s-wip", "done, not verified on hardware"),
    "cache": ("s-bug", "cache-only"),
    "stub": ("s-bug", "stub"),
    "err": ("s-miss", "error"),
    "miss": ("s-miss", "missing"),
    "n.a.": ("s-na", "n.a."),
    "?": ("s-unk", "?"),
}
GAP_CODES = {"cache", "stub", "err", "miss"}


def norm_sym(s):
    return s.replace("️", "")


def esc(s):
    return html.escape(s, quote=True)


SEEN_IDS = set()


def uid(t):
    """unique anchor id for a heading (second 'Legend' becomes legend-1)"""
    b = slug(t)
    i, c = 0, b
    while c in SEEN_IDS:
        i += 1
        c = "%s-%d" % (b, i)
    SEEN_IDS.add(c)
    return c


def slug(t):
    """GitHub style heading anchor."""
    t = re.sub(r"[`*]", "", t).strip().lower()
    t = re.sub(r"[^\w\- ]", "", t)
    return t.replace(" ", "-")


# ---------------------------------------------------------------- inline
def inline(s):
    codes = []

    def keep(m):
        codes.append("<code>%s</code>" % esc(m.group(2)))
        return "\x00%d\x00" % (len(codes) - 1)

    s = re.sub(r"(`+)(.+?)\1", keep, s)
    s = esc(s)
    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
               lambda m: '<a href="%s">%s</a>' % (m.group(2), m.group(1)), s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"(?<![\w*])\*(?![\s*])([^*\n]+?)(?<![\s*])\*(?![\w*])", r"<i>\1</i>", s)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], s)


# ---------------------------------------------------------------- block parse
def parse_blocks(lines):
    """-> list of (type, data).  Types: h(level,text) p(text) ul(items) ol(items)
    table(rows,aligns) code(text) rawopen(summary) rawclose"""
    out = []
    i, n = 0, len(lines)
    while i < n:
        ln = lines[i]
        if not ln.strip():
            i += 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", ln)
        if m:
            out.append(("h", (len(m.group(1)), m.group(2).strip())))
            i += 1
            continue
        if ln.startswith("```"):
            j = i + 1
            buf = []
            while j < n and not lines[j].startswith("```"):
                buf.append(lines[j])
                j += 1
            out.append(("code", "\n".join(buf)))
            i = j + 1
            continue
        m = re.match(r"^<details[^>]*>(?:<summary>(.*?)</summary>)?\s*$", ln)
        if m:
            out.append(("rawopen", m.group(1)))
            i += 1
            continue
        if ln.strip() == "</details>":
            out.append(("rawclose", None))
            i += 1
            continue
        if ln.startswith("|"):
            rows = []
            while i < n and lines[i].startswith("|"):
                rows.append(split_row(lines[i]))
                i += 1
            aligns = []
            if len(rows) > 1 and all(re.fullmatch(r":?-+:?", c.strip()) for c in rows[1]):
                aligns = ["r" if c.strip().endswith(":") else "" for c in rows[1]]
                del rows[1]
            out.append(("table", (rows, aligns)))
            continue
        if re.match(r"^\s*(?:[-*]\s+|\d+\.\s+)", ln):
            items = []
            ordered = bool(re.match(r"^\s*\d+\.\s", ln))
            while i < n:
                ln = lines[i]
                mm = re.match(r"^(\s*)(?:([-*])|(\d+)\.)\s+(.*)$", ln)
                if mm:
                    items.append([len(mm.group(1)), mm.group(3), mm.group(4)])
                    i += 1
                elif ln.strip() and ln.startswith(" ") and items:
                    items[-1][2] += " " + ln.strip()
                    i += 1
                elif not ln.strip() and i + 1 < n and re.match(r"^\s*(?:[-*]\s+|\d+\.\s+)", lines[i + 1]) \
                        and bool(re.match(r"^\s*\d+\.", lines[i + 1])) == ordered:
                    i += 1
                else:
                    break
            out.append(("ol" if ordered else "ul", items))
            continue
        buf = []
        while i < n and lines[i].strip() and not re.match(
                r"^(#{1,6}\s|```|\||<details|</details>|\s*[-*]\s+|\s*\d+\.\s+)", lines[i]):
            buf.append(lines[i].strip())
            i += 1
        if not buf:  # safety: never loop
            buf = [lines[i].strip()]
            i += 1
        out.append(("p", " ".join(buf)))
    return out


def split_row(ln):
    ln = ln.strip()
    if ln.startswith("|"):
        ln = ln[1:]
    if ln.endswith("|") and not ln.endswith("\\|"):
        ln = ln[:-1]
    cells, cur, k = [], "", 0
    while k < len(ln):
        c = ln[k]
        if c == "\\" and k + 1 < len(ln) and ln[k + 1] == "|":
            cur += "|"
            k += 2
            continue
        if c == "|":
            cells.append(cur.strip())
            cur = ""
        else:
            cur += c
        k += 1
    cells.append(cur.strip())
    return cells


# ---------------------------------------------------------------- renderers
def render_list(kind, items):
    """items: [indent, num, text]; nested by indent."""
    out = []
    stack = []  # indents

    def open_(ind, it):
        out.append("<%s>" % kind)
        stack.append(ind)

    for ind, num, text in items:
        if not stack:
            open_(ind, text)
        elif ind > stack[-1]:
            out.append("<%s>" % kind)
            stack.append(ind)
        else:
            out.append("</li>")
            while len(stack) > 1 and ind < stack[-1]:
                out.append("</%s></li>" % kind)
                stack.pop()
        val = ' value="%s"' % num if (kind == "ol" and num) else ""
        out.append("<li%s>%s" % (val, inline(text)))
    out.append("</li>")
    while len(stack) > 1:
        out.append("</%s></li>" % kind)
        stack.pop()
    out.append("</%s>" % kind)
    return "".join(out)


def legend_item(text):
    """bullet of a legend -> <span class=..>"""
    m = SYM_RE.match(text)
    if m:
        sym = norm_sym(m.group(1))
        cls = SYM[sym][0]
    else:
        m2 = re.match(r"^\*\*(.+?)\*\*", text)
        key = m2.group(1) if m2 else ""
        cls = CODE.get(key, ("s-na",))[0]
    mm = re.match(r"^(\*\*.+?\*\*|\S+)(.*)$", text)
    head, rest = mm.group(1), mm.group(2)
    if ": " in rest and not rest.lstrip().startswith("("):
        a, b = rest.split(": ", 1)
        return '<span class="%s" title="%s">%s%s <small>%s</small></span>' % (
            cls, esc(re.sub(r"[*`]", "", b)), "<b>%s</b>" % inline(head) if not head.startswith("**") else inline(head),
            inline(a), inline(b))
    return '<span class="%s">%s%s</span>' % (cls, inline(head), inline(rest))


def table_wrap(inner):
    return '<div class="wrap">%s</div>' % inner


def render_generic_table(rows, aligns):
    head, body = rows[0], rows[1:]
    h = "".join("<th>%s</th>" % inline(c) for c in head)
    b = []
    for r in body:
        tds = []
        for k, c in enumerate(r):
            cl = ' class="c"' if k < len(aligns) and aligns[k] == "r" else ""
            tds.append("<td%s>%s</td>" % (cl, inline(c)))
        b.append("<tr>%s</tr>" % "".join(tds))
    return table_wrap('<table class="mi"><thead><tr>%s</tr></thead><tbody>%s</tbody></table>'
                      % (h, "".join(b)))


def num(x):
    try:
        return float(x.replace("*", "").replace("%", "").strip())
    except ValueError:
        return None


def render_progress(rows, aligns):
    head, body = rows[0], rows[1:]
    idx = {h.replace("*", "").strip(): k for k, h in enumerate(head)}
    out = ['<div class="lg"><span><b class="b-dev" style="color:#fff">done (dev)</b></span>'
           '<span><b class="b-host" style="color:#fff">done (host)</b></span>'
           '<span><b class="b-q" style="color:#fff">done (?) / ?</b></span>'
           '<span><b class="b-noop" style="color:#fff">vendor no-op</b></span>'
           '<span><b class="b-gap" style="color:#fff">gaps</b></span></div>']
    th = "".join("<th>%s</th>" % inline(c) for c in [head[0], "progress"] + head[1:])
    trs = []
    for r in body:
        g = lambda k: num(r[idx[k]]) or 0
        fns = g("fns") or 1
        parts = [("b-dev", "done (dev)", g("dev")), ("b-host", "done (host)", g("host")),
                 ("b-q", "done (?) / ?", g("aud") + g("?")),
                 ("b-noop", "vendor no-op", g("no-op")), ("b-gap", "gaps", g("gaps"))]
        bar = "".join('<i class="%s" style="width:%.3f%%" title="%s: %d"></i>'
                      % (c, 100.0 * v / fns, t, v) for c, t, v in parts if v)
        tds = ["<td>%s</td>" % inline(r[0]), '<td><div class="pb">%s</div></td>' % bar]
        for k, c in enumerate(r[1:], 1):
            tds.append('<td class="c">%s</td>' % inline(c))
        trs.append("<tr>%s</tr>" % "".join(tds))
    out.append(table_wrap('<table class="mi"><thead><tr>%s</tr></thead><tbody>%s</tbody></table>'
                          % (th, "".join(trs))))
    return "".join(out)


def render_func_table(rows, aligns):
    head, body = rows[0], rows[1:]
    th = "".join("<th>%s</th>" % inline(c) for c in head)
    # SoC columns = header cells that look like Tnn
    soc = [k for k, c in enumerate(head) if re.fullmatch(r"T\d+", c)]
    trs = []
    for r in body:
        if len(r) > len(head) and "" in r[1:]:   # tolerate a stray '||' in the source
            sys.stderr.write("warning: extra empty cell in row %s\n" % r[0])
            r = r[:]
            r.remove("")
        gap = any(r[k] in GAP_CODES for k in soc if k < len(r))
        tds = []
        for k, c in enumerate(r):
            if k == 0:
                tds.append("<td>%s</td>" % inline(c))
            elif k in soc:
                mc = re.match(r"^(.*?)((?:\s*\[\d+\])*)$", c)
                code, refs = mc.group(1), mc.group(2).strip()
                cls, lab = CODE.get(code, ("s-unk", code))
                tds.append('<td class="c %s" data-code="%s">%s%s</td>' % (
                    cls, esc(code), esc(lab), " <small>%s</small>" % esc(refs) if refs else ""))
            elif k < len(head) and head[k] == "Used by":
                tds.append('<td class="c" style="white-space:normal">%s</td>' % inline(c))
            else:
                tds.append('<td class="nt">%s</td>' % inline(c))
        trs.append('<tr data-gap="%d">%s</tr>' % (1 if gap else 0, "".join(tds)))
    return (len(trs), sum(1 for t in trs if 'data-gap="1"' in t),
            table_wrap('<table class="mi"><thead><tr>%s</tr></thead><tbody>%s</tbody></table>'
                       % (th, "".join(trs))))


FILTER = ("<label><input type=\"checkbox\" onchange=\"var on=this.checked;"
          "this.closest('details').querySelectorAll('tr[data-gap=&quot;0&quot;]')"
          ".forEach(function(e){e.style.display=on?'none':''})\"> only rows with gaps</label>")


def render_streamer_gaps(items):
    """bullets '**T41**: 19 (timps 0, prudynt 3, raptor 18): functions' -> table"""
    rows = []
    for _, _, t in items:
        m = re.match(r"^\*\*(T\d+)\*\*:\s*(\d+)\s*\(timps (\d+), prudynt (\d+), raptor (\d+)\)(?::\s*(.*))?$", t)
        if m:
            fn = m.group(6) or ""
        else:
            m = re.match(r"^\*\*(T\d+)\*\*:\s*(\d+)\s*\((.*)\)\s*$", t)
            if not m:
                return None
            rows.append((m.group(1), m.group(2), "", "", "", m.group(3)))
            continue
        rows.append((m.group(1), m.group(2), m.group(3), m.group(4), m.group(5), fn))
    trs = []
    for soc, tot, a, b, c, fn in rows:
        trs.append('<tr><td>%s</td><td class="c"><b>%s</b></td><td class="c">%s</td><td class="c">%s</td>'
                   '<td class="c">%s</td><td class="nt">%s</td></tr>'
                   % (soc, tot, a, b, c, inline(fn)))
    return table_wrap('<table class="mi"><thead><tr><th>SoC</th><th>gaps used by at least one streamer</th>'
                      '<th>timps</th><th>prudynt</th><th>raptor</th><th>functions (state; streamers)</th>'
                      '</tr></thead><tbody>%s</tbody></table>' % "".join(trs))


# ---------------------------------------------------------------- matrix
SOCS = ["T10", "T20", "T21", "T23", "T30", "T31", "T32", "T33", "T40", "T41"]
API = None         # gen_api_coverage.Model, set in main()


def parse_details(blocks):
    """'### Matrix details' blocks -> {num: dict(title, vendor, cells, extra_cells, extra_blocks)}"""
    res, groups = {}, []
    cur = None
    for typ, d in blocks:
        if typ == "h" and d[0] == 4:
            groups.append(d[1])
            cur = None
        elif typ == "h" and d[0] == 5:
            m = re.match(r"^(\d+)\.\s+(.*)$", d[1])
            cur = {"title": d[1], "vendor": "", "cells": {}, "extra_cells": {}, "extra": [],
                   "group": groups[-1] if groups else ""}
            res[int(m.group(1)) if m else len(res) + 1000] = cur
        elif cur is not None:
            if typ == "ul":
                for _, _, t in d:
                    m = re.match(r"^\*\*(.+?):\*\*\s*(.*)$", t)
                    if not m:
                        cur["extra"].append(("ul", [[0, None, t]]))
                        continue
                    lab, txt = m.group(1), m.group(2)
                    if lab == "Vendor stack":
                        cur["vendor"] = txt
                    elif lab in SOCS:
                        cur["cells"][lab] = txt
                    elif lab.split(" ")[0] in SOCS:
                        cur["extra_cells"].setdefault(lab.split(" ")[0], []).append((lab, txt))
                    else:
                        cur["extra"].append(("ul", [[0, None, t]]))
            else:
                cur["extra"].append((typ, d))
        elif typ not in ("h",):
            pass
    return res, groups


LEGEND = {}        # symbol -> (label, description) from the Legend bullets of the md


def load_state(src, is_path, syms, txts):
    """Matrix symbols and detail texts of a baseline (git tag or file) -> syms {title: {soc: sym}}, txts {n: {soc: text}}."""
    syms.clear()
    txts.clear()
    try:
        if is_path:
            md = open(src, encoding="utf-8").read()
        else:
            md = subprocess.run(["git", "-C", ROOT, "show", "%s:docs/FEATURE_MATRIX.md" % src],
                                check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.stderr.write("warning: no baseline matrix %s (%s); nothing is marked for it\n" % (src, e))
        return
    socs = None
    for ln in md.split("\n"):
        if not ln.startswith("|"):
            continue
        r = split_row(ln)
        if r[:2] == ["#", "Feature"]:
            socs = r[2:]
        elif socs and re.fullmatch(r"\d+", r[0] or "x"):
            m = re.match(r"^\[(.*)\]\(#.*\)$", r[1])
            syms[m.group(1) if m else r[1]] = {
                soc: norm_sym(v.strip()) for soc, v in zip(socs, r[2:])}
    blocks = parse_blocks(re.sub(r"<!--.*?-->", "", md, flags=re.S).split("\n"))
    mi = next((k for k, (t, d) in enumerate(blocks) if t == "h" and d[1] == "Matrix details"), None)
    if mi is None:
        return
    mj = next((k for k in range(mi + 1, len(blocks)) if blocks[k][0] == "h" and blocks[k][1][0] <= 3), len(blocks))
    det, _ = parse_details(blocks[mi + 1:mj])
    for n, dd in det.items():
        txts[n] = {soc: SYM_RE.sub("", t, count=1).strip() for soc, t in dd["cells"].items()}


def load_baseline(path=None):
    del SYMS[:], TXTS[:]
    for i, (tag, _) in enumerate(RELEASES):
        sy, tx = {}, {}
        load_state(path if (path and i == 0) else tag, bool(path and i == 0), sy, tx)
        SYMS.append(sy)
        TXTS.append(tx)


def date_revs(now=None):
    """Last commits of docs/FEATURE_MATRIX.md before yesterday 00:00 and before today 00:00 (local time)."""
    now = now or datetime.datetime.now()
    t0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    out = []
    for d in (t0 - datetime.timedelta(days=1), t0):
        try:
            r = subprocess.run(["git", "-C", ROOT, "rev-list", "-1", "--before=" + d.strftime("%Y-%m-%d %H:%M:%S"),
                                "HEAD", "--", "docs/FEATURE_MATRIX.md"], check=True, capture_output=True, text=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            r = ""
        out.append(r or None)
    return out


def load_date_baselines():
    del DSYMS[:], DTXTS[:], DREVS[:]
    DREVS.extend(date_revs())
    for rev in DREVS:
        sy, tx = {}, {}
        if rev:
            load_state(rev, False, sy, tx)
        DSYMS.append(sy)
        DTXTS.append(tx)


def api_date_changes():
    """{(function, soc): 't' | 'y'}: evidence cell code changed since before today / between before yesterday and before today."""
    def cells(rev):
        try:
            d = json.loads(subprocess.run(["git", "-C", ROOT, "show", "%s:docs/api_evidence.json" % rev], check=True,
                                          capture_output=True, text=True).stdout)
        except (OSError, subprocess.CalledProcessError, ValueError):
            return None
        return {n: f.get("cells", {}) for n, f in d.get("functions", {}).items()}
    y, t = (cells(r) if r else None for r in DREVS)
    cur = {n: f["cells"] for n, f in API.ev["functions"].items()}
    res = {}
    for base, new, k in ((t, cur, "t"), (y, t, "y")):
        if base is None or new is None:
            continue
        for n, nc in new.items():
            for soc, c in nc.items():
                if n in base and base[n].get(soc) is not None and base[n][soc] != c:
                    res.setdefault((n, soc), k)
    return res


def intervals():
    """[(key, badge text, start index, end index or None for the current matrix)]; start/end index the RELEASES."""
    n = len(RELEASES)
    return [(RELEASES[i + 1][1].lower() if i + 1 < n else "new", RELEASES[i + 1][1] if i + 1 < n else "new",
             i, i + 1 if i + 1 < n else None) for i in range(n)]


def classify(old, new, otxt=None, ntxt=None):
    """None (same / no baseline), 'new' (better than the baseline) or 'chg' (symbol changed without
    improving, or the same symbol with a changed detail text)."""
    if old is None:
        return None
    if old == new:
        return "chg" if (otxt is not None and otxt != ntxt) else None
    if old == "—":
        return "new"
    if new == "—":
        return "chg"
    return "new" if RANK.get(new, 0) > RANK.get(old, 0) else "chg"


MX_JS = r"""(function(){
var D=JSON.parse(document.getElementById('mxdata').textContent),R=D.rows;
var $=function(s,e){return (e||document).querySelector(s)},$$=function(s,e){return [].slice.call((e||document).querySelectorAll(s))};
var tbl=$('#mx'),rows=$$('tbody tr[data-n]',tbl),grps=$$('tbody tr.grp',tbl),pn=$('#pn'),pnb=$('#pnb'),tip=$('#tip');
var F={q:'',soc:{},sym:{},tag:{},rk:'',dt:'',iv:''},last=null,cur=null;
function any(o){for(var k in o)if(o[k])return true;return false}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
var SS={'⚠':'⚠️'};function ss(s){return SS[s]||s}
function apply(){
  var vis=any(F.soc)?D.socs.filter(function(s){return F.soc[s]}):D.socs,q=F.q.toLowerCase(),n=0,sy=any(F.sym);
  $$('[data-soc]',tbl).forEach(function(e){e.style.display=vis.indexOf(e.getAttribute('data-soc'))<0?'none':''});
  rows.forEach(function(tr){
    var r=R[tr.getAttribute('data-n')],ok=true,cs=$$('td.mc',tr);
    if(q){var h=('#'+tr.getAttribute('data-n')+' '+r.t+' '+r.tags.join(' ')).toLowerCase();
      if(h.indexOf(q)<0){var f=false;D.socs.forEach(function(s){if(r.c[s].p.indexOf(q)>=0||r.v.toLowerCase().indexOf(q)>=0)f=true});ok=f}}
    if(ok&&any(F.tag)){var f2=false;r.tags.forEach(function(t){if(F.tag[t])f2=true});ok=f2}
    var m=false;
    cs.forEach(function(td){var s=td.getAttribute('data-sym'),v=vis.indexOf(td.getAttribute('data-soc'))>=0;
      var hit=sy&&F.sym[s]&&v;td.classList.toggle('dim',sy&&!F.sym[s]);if(hit)m=true});
    if(ok&&sy&&!m)ok=false;
    if(ok&&F.rk&&(' '+tr.getAttribute('data-k')+' ').indexOf(' '+F.rk+' ')<0)ok=false;
    if(ok&&F.dt){var dd=' '+tr.getAttribute('data-d')+' ';ok=F.dt.split(' ').some(function(x){return dd.indexOf(' '+x+' ')>=0})}
    tr.hidden=!ok;if(ok)n++;
  });
  var g=null,a=false;
  $$('tbody tr',tbl).forEach(function(tr){if(tr.classList.contains('grp')){if(g)g.hidden=!a;g=tr;a=false}else if(!tr.hidden)a=true});
  if(g)g.hidden=!a;
  $('#mxn').textContent=n+' of '+rows.length+' features';
}
function tog(map,btn,key){map[key]=!map[key];btn.setAttribute('aria-pressed',map[key]?'true':'false');apply()}
$$('[data-fsoc]').forEach(function(b){b.onclick=function(){tog(F.soc,b,b.getAttribute('data-fsoc'))}});
$$('[data-fsym]').forEach(function(b){b.onclick=function(){tog(F.sym,b,b.getAttribute('data-fsym'))}});
function tagBtns(){$$('[data-ftag]').forEach(function(b){b.setAttribute('aria-pressed',F.tag[b.getAttribute('data-ftag')]?'true':'false')});
  $$('.tag',tbl).forEach(function(b){b.classList.toggle('on',!!F.tag[b.getAttribute('data-tag')])})}
$$('[data-ftag]').forEach(function(b){b.onclick=function(){var k=b.getAttribute('data-ftag');F.tag[k]=!F.tag[k];tagBtns();apply()}});
$$('.tag',tbl).forEach(function(b){b.onclick=function(e){e.stopPropagation();var k=b.getAttribute('data-tag');F.tag[k]=!F.tag[k];tagBtns();apply()}});
$('#mxq').oninput=function(){F.q=this.value.trim();apply()};
var rk=$('#mxrk');if(rk)rk.onchange=function(){F.rk=this.value;apply()};
var dtsel=$('#mxdt');if(dtsel)dtsel.onchange=function(){F.dt=this.value;apply()};
$('#mxreset').onclick=function(){F={q:'',soc:{},sym:{},tag:{},rk:'',dt:''};$('#mxq').value='';if(rk)rk.value='';if(dtsel)dtsel.value='';
  $$('[aria-pressed="true"]').forEach(function(b){b.setAttribute('aria-pressed','false')});tagBtns();apply()};
/* tooltip */
function showTip(td){
  var tr=td.parentNode,r=R[tr.getAttribute('data-n')],s=td.getAttribute('data-soc'),c=r.c[s];
  var lg=D.leg[c.s],bd=c.k?'<i class="rc2'+D.bc[c.k]+'">'+D.bl[c.k]+'</i> ':'';
  tip.innerHTML='<b>#'+tr.getAttribute('data-n')+' '+esc(r.t)+' / '+s+'</b>'+
   '<div><b>'+esc(ss(c.s))+'</b> '+(lg?esc(lg[0])+': '+esc(lg[1]):'')+'</div>'+(c.k?'<div>'+bd+esc(c.bt)+'</div>':'')+(c.d?'<div><i class="dtb '+c.d.charAt(0)+(c.d.length>1?' c':'')+'">'+D.dt[c.d.charAt(0)]+'</i> '+esc(c.dtip)+'</div>':'')+
   '<div>'+(c.q?esc(c.q):'<i>no detail text</i>')+'</div><div class="small">click or Enter: full detail</div>';
  tip.hidden=false;var b=td.getBoundingClientRect(),w=tip.offsetWidth,h=tip.offsetHeight;
  var x=Math.min(Math.max(8,b.left+b.width/2-w/2),innerWidth-w-8),y=b.bottom+6;if(y+h>innerHeight-8)y=Math.max(8,b.top-h-6);
  tip.style.left=x+'px';tip.style.top=y+'px';
}
function hideTip(){tip.hidden=true}
tbl.addEventListener('mouseover',function(e){var td=e.target.closest&&e.target.closest('td.mc');if(td&&!(pn.contains(td)))showTip(td);else hideTip()});
tbl.addEventListener('mouseleave',hideTip);
tbl.addEventListener('focusin',function(e){var td=e.target.closest&&e.target.closest('td.mc');if(td)showTip(td);else hideTip()});
tbl.addEventListener('focusout',hideTip);
/* panel */
function apiSec(n,s,full){
  var A=D.api,fa=(A.fa||{})[n]||[],h='';
  fa.forEach(function(a){var d=(A.areas[a]||{})[s];if(!d)return;
    var tot=d.ok.length+d.wip.length+d.gap.length+d.unk.length+d.nob;
    h+='<div class="apx"><b>API: '+esc(a)+' on '+s+'</b> ';
    if(!tot){h+='<span class="small">no function of this area in the '+s+' vendor API</span></div>';return}
    if(d.nob){h+='<span class="small">no OpenIMP build for '+s+': '+d.nob+' vendor functions to provide</span></div>';return}
    h+='<span class="small">'+d.ok.length+'/'+tot+' work'+(d.wip.length?', '+d.wip.length+' implemented not device-tested':'')+(d.gap.length?', '+d.gap.length+' gaps':'')+(d.unk.length?', '+d.unk.length+' unknown':'')+(d.ws.length?', '+d.ws.length+' in progress':'')+'</span>';
    if(full){
      var L=function(t,l,o){return l.length?'<details'+(o?' open':'')+'><summary>'+t+' ('+l.length+')</summary><ul>'+l.map(function(x){return '<li><code>'+esc(x)+'</code></li>'}).join('')+'</ul></details>':''};
      h+=L('Missing / gaps',d.gap,true)+L('Unknown state',d.unk,false)+L('Implemented, not device-tested',d.wip,false)+L('Works',d.ok,false)}
    h+='</div>'});
  return h}
function block(r,s,full){
  var c=r.c[s];
  var h='<section class="cd"><h4><span class="sym">'+esc(ss(c.s))+'</span> '+s+(c.k?' <i class="rc2'+D.bc[c.k]+'" title="'+esc(c.bt)+'">'+D.bl[c.k]+'</i> <span class="small">'+esc(c.bt)+'</span>':'')+'</h4>';
  h+=c.h?'<div class="tx">'+c.h+'</div>':'<p class="small">No detail text for this cell.</p>';
  (c.x||[]).forEach(function(x){h+='<div class="tx"><b>'+x[0]+':</b> '+x[1]+'</div>'});
  (c.w||[]).forEach(function(w){h+='<div class="tx">&#128679; <b>in progress:</b> '+(w[3]?'<a href="'+esc(w[3])+'">'+esc(w[0])+'</a>':esc(w[0]))+' (since '+esc(w[1])+')'+(w[2]?' &ndash; '+esc(w[2]):'')+'</div>'});
  (c.i||[]).forEach(function(i){h+='<div class="tx">Issue <a href="'+esc(i[3])+'">#'+i[0]+'</a> ('+esc(i[2])+'): '+esc(i[1])+'</div>'});
  h+=apiSec(r.n,s,full);
  return h+'</section>';
}
function head(n,r){return (r.v?'<p class="vs"><b>Vendor stack:</b> '+r.v+'</p>':'')+'<p class="small">'+r.tags.map(esc).join(' &middot; ')+(r.l?' &middot; <a href="#'+r.l+'">full section (additional text)</a>':'')+'</p>'}
function openPanel(html,title){pnb.innerHTML=html;$('#pnt').textContent=title;pn.hidden=false;document.body.classList.add('pnopen');pnb.scrollTop=0;pn.scrollTop=0;hideTip();$('#pnx').focus()}
function mark(td){$$('td.sel',tbl).forEach(function(e){e.classList.remove('sel')});$$('tr.selr',tbl).forEach(function(e){e.classList.remove('selr')});if(td&&td.classList)td.classList.add('sel')}
function openCell(td){last=td;var n=td.parentNode.getAttribute('data-n'),r=R[n],s=td.getAttribute('data-soc');mark(td);
  openPanel(head(n,r)+'<p><button type="button" class="chip" data-allrow="'+n+'">Show all SoCs of this feature</button></p>'+block(r,s,true),'#'+n+' '+r.t+' / '+s);cur=null}
function openRow(n,btn){last=btn||last;var r=R[n];mark(null);$('#r-'+n).classList.add('selr');
  openPanel(head(n,r)+D.socs.map(function(s){return block(r,s)}).join(''),'#'+n+' '+r.t+' (all SoCs)')}
function closePanel(){if(pn.hidden)return;pn.hidden=true;document.body.classList.remove('pnopen');mark(null);$$('tr.selr',tbl).forEach(function(e){e.classList.remove('selr')});if(last&&document.body.contains(last))last.focus()}
$('#pnx').onclick=closePanel;
pn.addEventListener('click',function(e){var b=e.target.closest('[data-allrow]');if(b)openRow(b.getAttribute('data-allrow'),last);
  var a=e.target.closest('a[href^="#"]');if(a&&innerWidth<800)closePanel()});
tbl.addEventListener('click',function(e){
  var f=e.target.closest('.fname');if(f){openRow(f.getAttribute('data-row'),f);return}
  var td=e.target.closest('td.mc');if(td)openCell(td)});
document.addEventListener('keydown',function(e){
  if(e.key==='Escape'){if(!pn.hidden){closePanel();e.preventDefault()}else hideTip();return}
  var td=document.activeElement;if(!td||!td.classList||!td.classList.contains('mc'))return;
  if(e.key==='Enter'||e.key===' '){e.preventDefault();openCell(td);return}
  var tr=td.parentNode,i=D.socs.indexOf(td.getAttribute('data-soc')),nx=null;
  if(e.key==='ArrowRight'||e.key==='ArrowLeft'){var cs=$$('td.mc',tr).filter(function(x){return x.style.display!=='none'});var k=cs.indexOf(td)+(e.key==='ArrowRight'?1:-1);nx=cs[k]}
  else if(e.key==='ArrowDown'||e.key==='ArrowUp'){var o=tr;do{o=e.key==='ArrowDown'?o.nextElementSibling:o.previousElementSibling}while(o&&(o.hidden||!o.hasAttribute('data-n')));
    if(o)nx=$('td.mc[data-soc="'+td.getAttribute('data-soc')+'"]',o)}
  if(nx){e.preventDefault();nx.focus();nx.scrollIntoView({block:'nearest',inline:'nearest'})}});
/* links into collapsed <details> */
function openHash(){var h=location.hash.slice(1);if(!h)return;var el=document.getElementById(h);if(!el)return;
  for(var p=el;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;if(el.tagName==='DETAILS')el.open=true;el.scrollIntoView()}
addEventListener('hashchange',openHash);openHash();
apply();
})();
"""


# ---- compact matrix (v2): symbols only, details in a side panel -------------
# Tags per feature, derived from the section ("####" group) of the matrix table
# plus a few per-number additions.  "beyond-vendor" is derived from the cells
# (a feature with at least one ✅+ cell).  Nothing here makes a claim about a SoC.
GROUP_TAGS = {
    "ISP core and image pipeline": ["ISP"],
    "Exposure (AE)": ["ISP"],
    "Colour and image quality": ["ISP"],
    "Video encoder and streams": ["encoder"],
    "Audio (documentation only, no tests)": ["audio"],
    "Memory, size, load": ["footprint"],
    "Stability, helper libraries, telemetry": ["stability"],
    "Region of interest": ["encoder"],
}
NUM_TAGS = {2: ["stability"], 3: ["stability"], 28: ["OSD"], 29: ["IVS"], 39: ["encoder"],
            40: ["encoder"], 41: ["stability"], 42: ["API"], 43: ["API"], 44: ["API"]}
NUM_REPLACE = {28, 29}      # the group tag is replaced, not extended
TAG_ORDER = ["ISP", "encoder", "audio", "OSD", "IVS", "footprint", "stability", "API", "beyond-vendor"]
SYM_ORDER = ["✅+", "✅", "⚠", "🔧", "📋", "❌", "?", "—"]
SYM_SHOW = {"⚠": "⚠️"}


def plain(s):
    """Markdown -> plain text for tooltips and search."""
    s = re.sub(r"\[([^\]]+)\]\([^)\s]+\)", r"\1", s)
    s = re.sub(r"[`*]", "", s)
    return re.sub(r"\s+", " ", s).strip()


def short(s, lim=200):
    """First sentence of a plain text, at most ~lim characters."""
    m = re.search(r"(?<=[a-z0-9\)])[.!?](?=\s+[A-Z(])", s)
    t = s[:m.start() + 1] if m else s
    if len(t) > lim:
        t = t[:lim].rsplit(" ", 1)[0].rstrip(",;:(") + " ..."
    return t


def json_js(o):
    return json.dumps(o, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/").replace("<!--", "<\\!--")


def badge_tables():
    """kind key -> text / css suffix / short text / long text for every interval plus 'chg'."""
    iv = intervals()
    txt, cls, short_, desc = {}, {}, {}, {}
    for j, (key, label, i, e) in enumerate(iv):
        txt[key] = label
        cls[key] = " nw" if e is None else ("" if j == len(iv) - 2 else " old")
        a = "%s (%s)" % (RELEASES[i][1], RELEASES[i][0])
        if e is None:
            short_[key] = "improved after %s (in next, not released)" % RELEASES[i][1]
            desc[key] = "new: improved after the %s tag (%s); in next, not yet released" % (RELEASES[i][1], RELEASES[i][0])
        else:
            short_[key] = "improved between %s and %s" % (RELEASES[i][1], label)
            desc[key] = "%s: improved between %s and %s (%s); included in the %s release" % (
                label, a, label, RELEASES[e][0], label)
    txt["chg"], cls["chg"] = "changed", " chg"
    short_["chg"] = "cell changed, not improved"
    desc["chg"] = "changed: the cell changed (other symbol without improvement, or new text) but did not improve"
    return txt, cls, short_, desc


BADGE_TXT, BADGE_CLS, BADGE_SHORT, BADGE_DESC = badge_tables()
DTXT = {"t": "today", "y": "yday"}


def badge_tip(kind, olds, ks, sym):
    """olds[i] = symbol at release i (None if unknown); ks[j] = classify result of interval j."""
    t = BADGE_DESC[kind]
    iv = intervals()
    if kind != "chg":
        i = next(i for key, _, i, _ in iv if key == kind)
        return t + " (%s at %s, now %s)" % (SYM_SHOW.get(olds[i], olds[i]), RELEASES[i][1], SYM_SHOW.get(sym, sym))
    parts = []
    for (key, label, i, e), k in zip(iv, ks):
        if k:
            o = olds[i]
            parts.append("%s %s, now %s" % (RELEASES[i][1], SYM_SHOW.get(o, o), SYM_SHOW.get(sym, sym)) if o != sym
                         else "%s: same symbol %s, new text" % (RELEASES[i][1], SYM_SHOW.get(sym, sym)))
    return t + " (" + "; ".join(parts) + ")"


def legend_bar():
    items = []
    for sym in SYM_ORDER:
        if sym in LEGEND:
            lab, desc = LEGEND[sym]
            items.append('<span class="lgi %s" tabindex="0" title="%s"><b>%s</b> %s</span>' % (
                SYM.get(sym, ("s-na",))[0], esc(desc), esc(SYM_SHOW.get(sym, sym)), esc(lab)))
    for k in [x[0] for x in intervals()][::-1] + ["chg"]:
        items.append('<span class="lgi" tabindex="0" title="%s"><i class="rc2%s">%s</i> %s</span>' % (
            esc(BADGE_DESC[k]), BADGE_CLS[k], BADGE_TXT[k], esc(BADGE_SHORT[k])))
    if DREVS and (DREVS[0] or DREVS[1]):
        for k, t in (("t", ""), ("y", "")):
            items.append('<span class="lgi" tabindex="0" title="%s"><i class="dtb %s">%s</i><i class="dtb %s c">%s</i> %s</span>' % (
                esc("cell compared with the last matrix commit before today / yesterday 00:00 (local time)"),
                k, DTXT[k], k, DTXT[k], esc("since " + ("00:00" if k == "t" else "yesterday 00:00") + " (filled = improved, outlined = changed)")))
    items.append('<span class="lgi" tabindex="0" title="%s"><b>&#128679;</b> in progress (issue)</span>' % esc(
        "work in progress: a branch is running; an open GitHub issue labelled type:wip; hover the sign for the issue and start date"))
    items.append('<span class="lgi" tabindex="0" title="%s">no %s badge</span>' % (esc(
        "the first release has no earlier matrix to compare with, so improvements before %s cannot be marked" % RELEASES[0][1]),
        esc(RELEASES[0][1])))
    return '<div class="lgbar" id="lgbar">%s</div>' % "".join(items)


def render_matrix(rows, details, groups_info):
    head = rows[0]
    socs = head[2:]
    mism = []
    iv = intervals()
    ikeys = [x[0] for x in iv]
    cnt = {soc: {k: 0 for k in ikeys + ["chg"]} for soc in socs}      # badge counts per kind
    rowcnt = {k: 0 for k in ikeys + ["chg"]}
    symcnt = {soc: {} for soc in socs}
    dkeys = ["t", "tc", "y", "yc"]
    dcnt = {soc: {k: 0 for k in dkeys} for soc in socs}     # date badge counts per SoC
    drow = {k: 0 for k in dkeys}
    trs, data = [], {}
    grp, nrows, nbadge = "", 0, 0
    th = ('<th scope="col" class="fh">Feature</th>'
          + "".join('<th scope="col" data-soc="%s">%s</th>' % (esc(s), esc(s)) for s in socs))
    for r in rows[1:]:
        if not r[0].strip() and r[1].startswith("**"):
            grp = r[1].strip("*")
            trs.append('<tr class="grp"><td colspan="%d">%s</td></tr>' % (len(head) - 1, inline(grp)))
            continue
        n = int(r[0])
        m = re.match(r"^\[(.*)\]\(#(.*)\)$", r[1])
        title = m.group(1) if m else r[1]
        d = details.get(n, {"vendor": "", "cells": {}, "extra_cells": {}, "extra": []})
        tags = NUM_TAGS.get(n, []) if n in NUM_REPLACE else GROUP_TAGS.get(grp, []) + NUM_TAGS.get(n, [])
        tags = list(tags)
        bases = [S.get(title, {}) for S in SYMS]
        cells, tds, rowk, plus, rowd = {}, [], set(), False, set()
        for soc, sym in zip(socs, r[2:]):
            sym = norm_sym(sym.strip())
            cls, ttl = SYM.get(sym, ("s-unk", "unknown"))
            symcnt[soc][sym] = symcnt[soc].get(sym, 0) + 1
            plus = plus or sym == "✅+"
            txt = d["cells"].get(soc, "")
            mm = SYM_RE.match(txt)
            if mm:
                if norm_sym(mm.group(1)) != sym:
                    mism.append((n, soc, sym, mm.group(1)))
                txt = txt[mm.end():]
            extras = [(lab, SYM_RE.sub("", t, count=1)) for lab, t in d["extra_cells"].get(soc, [])]
            ntxt = txt.strip()
            olds = [bs.get(soc) for bs in bases]
            ks = []
            for (key, label, i, e) in iv:
                if e is None:
                    nsym, ntx = sym, ntxt
                else:
                    nsym, ntx = olds[e], (TXTS[e].get(n, {}).get(soc) if n in TXTS[e] else None)
                ks.append(classify(olds[i], nsym, TXTS[i].get(n, {}).get(soc) if n in TXTS[i] else None, ntx))
            kind = next((iv[j][0] for j in range(len(iv) - 1, -1, -1) if ks[j] == "new"), None)
            if not kind and any(ks):
                kind = "chg"
            better = kind not in (None, "chg") or any(k == "new" for k in ks)
            dk, dtip = None, ""
            if DREVS and (DREVS[0] or DREVS[1]):
                tsym = DSYMS[1].get(title, {}).get(soc)
                ysym = DSYMS[0].get(title, {}).get(soc)
                kt = classify(tsym, sym, DTXTS[1].get(n, {}).get(soc) if n in DTXTS[1] else None, ntxt)
                ky = classify(ysym, tsym, DTXTS[0].get(n, {}).get(soc) if n in DTXTS[0] else None,
                              DTXTS[1].get(n, {}).get(soc) if n in DTXTS[1] else None) if tsym is not None else None
                if kt:
                    dk = "t" + ("" if kt == "new" else "c")
                    dtip = "today: %s (%s before today, now %s)" % (
                        "improved" if kt == "new" else "changed", SYM_SHOW.get(tsym, tsym), SYM_SHOW.get(sym, sym))
                elif ky:
                    dk = "y" + ("" if ky == "new" else "c")
                    dtip = "yesterday: %s (%s before yesterday, then %s)" % (
                        "improved" if ky == "new" else "changed", SYM_SHOW.get(ysym, ysym), SYM_SHOW.get(tsym, tsym))
                if dk:
                    rowd.add(dk)
                    dcnt[soc][dk] += 1
            pt = plain(txt)
            wl = API.wipfeat.get((n, soc), []) if API else []
            c = {"s": sym, "h": inline(txt), "q": short(pt) if pt else "",
                 "p": (pt + " " + " ".join(plain(t) for _, t in extras)).strip().lower()}
            if extras:
                c["x"] = [[inline(lab), inline(t)] for lab, t in extras]
            wm = ""
            if wl:
                c["w"] = [[w.get("branch", ""), w.get("since", ""), w.get("note", ""), w.get("url", "")] for w in wl]
                wm = ' <span class="wipm" title="%s">&#128679;</span>' % esc("work in progress: " + "; ".join(
                    "%s (since %s)" % (w.get("branch", ""), w.get("since", "")) for w in wl))
            il = API.cell_issues.get((n, soc), []) if API else []
            if il:
                c["i"] = [[i["number"], i.get("title", ""), i.get("state", ""), i.get("url", "")] for i in il]
            badge = ""
            tip = ""
            if kind:
                rowk.add(kind)
                cnt[soc][kind] += 1
                tip = badge_tip(kind, olds, ks, sym)
                c["k"] = kind
                c["o"] = olds[0] or ""
                c["bt"] = tip
                badge = '<i class="rc2%s">%s</i>' % (BADGE_CLS[kind], BADGE_TXT[kind])
            if dk:
                c["d"] = dk
                c["dtip"] = dtip
                badge += '<i class="dtb %s" title="%s">%s</i>' % (dk[0] + (" c" if dk.endswith("c") else ""), esc(dtip), DTXT[dk[0]])
                tip = (tip + "; " if tip else "") + dtip
            cells[soc] = c
            lg = LEGEND.get(sym)
            aria = "%s: %s%s%s" % (soc, lg[0] if lg else sym, "; " + tip if tip else "", "")
            tds.append('<td class="mc %s%s" tabindex="0" data-soc="%s" data-sym="%s"%s aria-label="%s">'
                       '<span class="sym">%s</span>%s</td>' % (
                           cls, " new" if better else "", esc(soc), esc(sym),
                           (' data-rc2="%s"' % kind if kind else "") + (' data-dt="%s"' % dk if dk else ""), esc(aria), esc(SYM_SHOW.get(sym, sym)), badge + wm))
        if plus:
            tags.append("beyond-vendor")
        tags = [t for t in TAG_ORDER if t in tags]
        slg = slug(d["title"]) if d.get("extra") else ""
        data[n] = {"n": n, "t": title, "g": grp, "v": inline(d["vendor"]), "tags": tags, "l": slg, "c": cells}
        chips = "".join('<button type="button" class="tag" data-tag="%s">%s</button>' % (esc(t), esc(t)) for t in tags)
        trs.append('<tr id="r-%d" data-n="%d" data-k="%s" data-d="%s"><th scope="row"><span class="num">%d</span>'
                   '<button type="button" class="fname" data-row="%d">%s</button><span class="tags">%s</span></th>%s</tr>'
                   % (n, n, " ".join(sorted(rowk)), " ".join(sorted(rowd)), n, n, inline(title), chips, "".join(tds)))
        nrows += 1
        for k in rowk:
            rowcnt[k] += 1
        nbadge += 1 if rowk else 0
        for k in rowd:
            drow[k] += 1
    for m in mism:
        sys.stderr.write("warning: row %d %s: table symbol %s, detail symbol %s\n" % m)
    alltags = [t for t in TAG_ORDER if any(t in v["tags"] for v in data.values())]
    allsyms = [s for s in SYM_ORDER if any(s in symcnt[x] for x in socs)]

    def chipgrp(label, items, attr, show=lambda x: x):
        return ('<div class="fg"><span class="fl">%s</span>%s</div>' % (label, "".join(
            '<button type="button" class="chip" %s="%s" aria-pressed="false">%s</button>'
            % (attr, esc(x), esc(show(x))) for x in items)))
    ctl = ('<div class="mxctl"><div class="fg"><input id="mxq" type="search" placeholder="Search features and details" '
           'aria-label="Search features and details"><span id="mxn" class="small" aria-live="polite"></span>'
           '<button type="button" id="mxreset" class="chip">Reset filters</button></div>'
           + chipgrp("SoC", socs, "data-fsoc")
           + chipgrp("Status", allsyms, "data-fsym", lambda x: SYM_SHOW.get(x, x))
           + chipgrp("Tags", alltags, "data-ftag")
           + ('<div class="fg"><span class="fl">Badge</span><select id="mxrk" aria-label="Show only rows with a release badge">'
              '<option value="">all features</option>%s<option value="chg">changed, not improved</option></select></div>'
              % "".join('<option value="%s">%s</option>' % (k, esc(
                  "changes since %s (new, in next)" % RELEASES[-1][1] if k == "new" else "in " + BADGE_TXT[k]))
                  for k in ikeys[::-1]) if any(SYMS) else "")
           + ('<div class="fg"><span class="fl">Changed</span><select id="mxdt" aria-label="Show only rows changed today or yesterday">'
              '<option value="">any time</option><option value="t tc">today</option><option value="y yc">yesterday</option>'
              '<option value="t tc y yc">today or yesterday</option><option value="t y">improved today or yesterday</option></select></div>'
              if DREVS and (DREVS[0] or DREVS[1]) else "") + '</div>')
    # counts per SoC (+ comparison with the baseline)
    hd = "".join("<th>%s</th>" % esc(x) for x in socs)
    body = ""
    for s in allsyms:
        body += "<tr><td>%s</td>%s</tr>" % (esc(SYM_SHOW.get(s, s)), "".join(
            "<td>%d</td>" % symcnt[x].get(s, 0) for x in socs))
    if any(SYMS):
        def crow(label, key, first=False):
            return "<tr%s><td>%s</td>%s</tr>" % (' class="sep"' if first else "", label, "".join(
                "<td>%s</td>" % (("<b>%d</b>" if first else "%d") % cnt[x][key]) for x in socs))
        for j, k in enumerate(ikeys[::-1]):
            body += crow('%s <i class="rc2%s">%s</i> (%d features)' % (BADGE_SHORT[k], BADGE_CLS[k], BADGE_TXT[k], rowcnt[k]), k, j == 0)
        body += crow('changed without improvement <i class="rc2 chg">changed</i> (%d features)' % rowcnt["chg"], "chg")
        if DREVS and (DREVS[0] or DREVS[1]):
            for k, lab in (("t", "improved today"), ("tc", "changed today, not improved"),
                           ("y", "improved yesterday"), ("yc", "changed yesterday, not improved")):
                body += "<tr%s><td>%s <i class=\"dtb %s\">%s</i> (%d features)</td>%s</tr>" % (
                    ' class="sep"' if k == "t" else "", lab, k[0] + (" c" if k.endswith("c") else ""), DTXT[k[0]], drow[k],
                    "".join("<td>%d</td>" % dcnt[x][k] for x in socs))
        body += "<tr><td>total improved since %s</td>%s</tr>" % (RELEASES[0][1], "".join(
            "<td>%d</td>" % sum(cnt[x][k] for k in ikeys) for x in socs))
        MATRIX_COUNTS.clear()
        MATRIX_COUNTS.update(cnt)
    sumline = ("%d of %d features have a badge on at least one cell; compared with the tags %s. There is no %s badge: "
               "nothing earlier to compare with." % (nbadge, nrows, ", ".join("%s = <code>%s</code>" % (l, t) for t, l in RELEASES),
                                                     RELEASES[0][1])) if any(SYMS) else "cells per symbol"
    if DREVS and (DREVS[0] or DREVS[1]):
        sumline += (" Date badges: today = changed since the last matrix commit before today 00:00 (%s), "
                    "yday = changed between the last commits before yesterday 00:00 (%s) and before today 00:00." % (
                        (DREVS[1] or "none")[:9], (DREVS[0] or "none")[:9]))
    bar = ('<details class="rc2bar"><summary>Counts per SoC and release comparison</summary><p class="small">%s</p>'
           '<table><thead><tr><th></th>%s</tr></thead><tbody>%s</tbody></table></details>'
           % (sumline, hd, body))
    panel = ('<aside id="pn" role="dialog" aria-modal="false" aria-labelledby="pnt" hidden>'
             '<div class="pnh"><h3 id="pnt"></h3><button type="button" id="pnx" aria-label="Close (Esc)">&times;</button></div>'
             '<div id="pnb"></div></aside><div id="tip" role="tooltip" hidden></div>')
    table = ('<div class="mwrap" id="mwrap"><table class="mx" id="mx"><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>'
             % (th, "".join(trs)))
    script = '<script type="application/json" id="mxdata">%s</script><script>%s</script>' % (
        json_js({"socs": socs, "rows": data, "leg": LEGEND, "bl": BADGE_TXT, "bc": BADGE_CLS, "bd": BADGE_DESC, "dt": DTXT,
                 "api": API.api_json() if API else {"areas": {}, "fa": {}}}), MX_JS)
    return legend_bar() + bar + ctl + table + panel + script


# ---------------------------------------------------------------- main render
def render(md):
    blocks = parse_blocks(md.split("\n"))
    out, hist = [], []
    dst = [out]
    npara = 0

    def W(x):
        dst[0].append(x)

    # split the matrix-details range off
    mi = next(k for k, (t, d) in enumerate(blocks) if t == "h" and d[1] == "Matrix details")
    mj = next((k for k in range(mi + 1, len(blocks))
               if blocks[k][0] == "h" and blocks[k][1][0] <= 3), len(blocks))
    details, _ = parse_details(blocks[mi + 1:mj])
    det_intro = [b for b in blocks[mi + 1:mj] if b[0] == "p"][:1]

    section = ""      # current h2
    sub = ""          # current h3
    in_card = False
    in_legend = False
    in_full = False
    full_gaps = None

    def close_card():
        nonlocal in_card
        if in_card:
            W("</div>")
            in_card = False

    k = 0
    while k < len(blocks):
        typ, d = blocks[k]
        k += 1
        if k - 1 == mi:   # matrix details: render extras only
            close_card()
            W("<h3>Matrix details</h3>")
            W('<p class="meta">Per feature: the vendor-stack behaviour and the full per-SoC cell '
                       'text (chronological test log, newest results win) are in the table above; '
                       'additional text per feature follows here.</p>')
            grp = ""
            for n in sorted(details):
                dd = details[n]
                if not dd["extra"]:
                    continue
                if dd["group"] != grp:
                    grp = dd["group"]
                    W("<h4>%s</h4>" % inline(grp))
                inner = "".join(render_block(t, x) for t, x in dd["extra"])
                W('<details id="%s"><summary>%s</summary>%s</details>'
                           % (slug(dd["title"]), inline(dd["title"]), inner))
            k = mj
            continue
        if typ == "h":
            lvl, text = d
            close_card()
            in_legend = False
            if lvl == 2:
                dst[0] = hist if text in ("Summary", "Legend") else ([] if text in ("Release gates", "Kernel driver port from the recovered vendor code (open-tx-isp)") else out)
            if lvl == 1:
                W("<h1>%s</h1>" % inline(text))
            elif lvl == 2:
                section = text
                sub = ""
                if text == "Matrix" and API is not None:
                    W(API.html_top())
                W('<h2 id="%s">%s</h2>' % (uid(text), inline(text)))
                if text in ("Summary",):
                    W('<div class="card">')
                    in_card = True
                if text == "Legend":
                    in_legend = True
            elif lvl == 3:
                sub = text
                W('<h3 id="%s">%s</h3>' % (uid(text), inline(text)))
                if text == "Notes":
                    W('<div class="card">')
                    in_card = True
                if text == "Legend":
                    in_legend = True
            else:
                W("<h4>%s</h4>" % inline(text))
            continue
        if typ == "rawopen":
            W('<details class="full"><summary>%s</summary>%s' % (inline(d or ""), FILTER))
            in_full = True
            continue
        if typ == "rawclose":
            W("</details>")
            in_full = False
            continue
        if typ == "table":
            rows, aligns = d
            if section == "Matrix" and rows[0][:2] == ["#", "Feature"]:
                W(render_matrix(rows, details, None))
            elif rows[0][0] == "Vendor function":
                cnt, gaps, h = render_func_table(rows, aligns)
                W(h)
            elif rows[0][0] == "SoC" and "fns" in rows[0]:
                W(render_progress(rows, aligns))
            else:
                W(render_generic_table(rows, aligns))
            continue
        if typ == "code":
            if sub == "Progress per SoC":
                continue   # ASCII bars: replaced by the bar column of the table
            W("<pre>%s</pre>" % esc(d))
            continue
        if typ in ("ul", "ol"):
            if in_legend:
                for _, _, t in d:
                    mm = SYM_RE.match(t)
                    if mm and norm_sym(mm.group(1)) not in LEGEND:
                        rest = plain(t[mm.end():])
                        lab, _, desc = rest.partition(": ")
                        LEGEND[norm_sym(mm.group(1))] = (lab, desc or lab)
                cls = "card legend" if not in_card else "legend"
                W('<div class="%s">%s</div>' % (cls, "".join(legend_item(t) for _, _, t in d)))
            elif sub.startswith("Streamer-relevant gaps") and render_streamer_gaps(d):
                W(render_streamer_gaps(d))
            else:
                W(render_list(typ, d))
            continue
        # paragraph
        npara += 1
        if section == "" and npara == 1:      # "As of" line: short version visible, the rest in History
            cut = d.find(";")
            if cut > 0:
                head_ = d[:cut] + (")" if d[:cut].count("(") > d[:cut].count(")") else "")
                W('<p class="meta">%s (full history below)</p>' % inline(head_))
                hist.insert(0, '<p class="small">%s</p>' % inline(d))
                continue
        if (len(d) > 350 or (section == "" and len(d) > 120)) and dst[0] is out and not in_full:
            hist.append('<p class="small">%s</p>' % inline(d))
            continue
        if in_card:
            W('<p class="small" style="margin:.6rem 0 0">%s</p>' % inline(d))
        elif in_full:
            W('<p class="small">%s</p>' % inline(d))
        else:
            W('<p class="meta">%s</p>' % inline(d))
    close_card()
    if API is not None:
        out.append(API.html_section())
    if hist:
        out.append('<details class="hist" id="history"><summary>History (update log, long notes)</summary>%s</details>'
                   % "\n".join(hist))
    return "\n".join(out)


def render_block(typ, d):
    if typ == "p":
        return "<p>%s</p>" % inline(d)
    if typ in ("ul", "ol"):
        return render_list(typ, d)
    if typ == "code":
        return "<pre>%s</pre>" % esc(d)
    if typ == "table":
        return render_generic_table(*d)
    if typ == "h":
        return "<h4>%s</h4>" % inline(d[1])
    return ""


def main():
    args = [a for a in sys.argv[1:]]
    base_file = os.environ.get("BASELINE_MD")
    if "--baseline" in args:
        k = args.index("--baseline")
        base_file = args[k + 1]
        del args[k:k + 2]
    check = "--check" in args
    if check:
        args.remove("--check")
    src = args[0] if args else SRC
    dst = args[1] if len(args) > 1 else DST
    load_baseline(base_file)
    load_date_baselines()
    global API
    if os.path.abspath(src) == os.path.abspath(SRC):
        API, changed = api.update_md(write=not check)       # regenerate FEATURE_MATRIX.md from the status data first
        print("%s: %s" % (SRC, ("out of date" if check else "regenerated") if changed else "up to date"))
        if check:
            for w in API.warnings:
                sys.stderr.write("warning: %s\n" % w)
            sys.exit(1 if (changed or API.warnings) else 0)
    else:
        API = api.Model(open(src, encoding="utf-8").read())
    for w in API.warnings:
        sys.stderr.write("warning: %s\n" % w)
    API.datechg = api_date_changes() if any(DREVS) else {}
    sys.stderr.write("date baselines: before yesterday %s, before today %s; API cells changed: %d today, %d yesterday\n" % (
        DREVS[0], DREVS[1], sum(v == "t" for v in API.datechg.values()), sum(v == "y" for v in API.datechg.values())))
    api.print_counts(API)
    md = open(src, encoding="utf-8").read()
    md = re.sub(r"<!--.*?-->", "", md, flags=re.S)
    k = md.find("\n## " + api.HEAD)
    if k >= 0:
        md = md[:k + 1]       # the section is rendered from the model
    body = render(md)
    title = re.search(r"^#\s+(.*)$", md, re.M).group(1)
    page = ('<!doctype html>\n<html lang="en"><head><meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
            "<title>%s</title>\n<style>%s</style></head><body><main>\n%s\n"
            "<p class=\"small\">Generated from docs/status/ and docs/api_evidence.json by tools/gen_feature_matrix_html.py; "
            "do not edit by hand.</p></main></body></html>\n" % (esc(title), CSS, body))
    open(dst, "w", encoding="utf-8").write(page)
    print("wrote %s (%d bytes)" % (dst, len(page.encode())))
    if MATRIX_COUNTS:
        ks = [x[0] for x in intervals()] + ["chg"]
        print("cells per SoC, badge counts (%s): %s" % ("/".join(ks), ", ".join(
            "%s %s" % (k, "/".join(str(v[x]) for x in ks)) for k, v in MATRIX_COUNTS.items())))

if __name__ == "__main__":
    main()
