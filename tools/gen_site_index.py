#!/usr/bin/env python3
"""gen_site_index.py: the start page of the GitHub Pages site (index.html).

Links the feature matrix, the issue views and every light-box protocol in docs/lightbox/
(one row per <vendor>-<soc>-<sensor>-<date>.html, the result column taken from docs/lightbox/README.md).
usage: gen_site_index.py OUT.html   (run from the repository root; the workflow copies docs/lightbox/ next to it)
"""
import glob, html, os, re, subprocess, sys

REPO = "https://github.com/Lu-Fi"
BOARD = "https://github.com/users/Lu-Fi/projects/1"


def readme_rows():
    """file name -> (soc, camera, date, lamps, result text) from the docs/lightbox/README.md table"""
    rows = {}
    try:
        lines = open("docs/lightbox/README.md", encoding="utf-8").read().splitlines()
    except OSError:
        return rows
    for line in lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        m = re.search(r"\(([^)]+\.html)\)", line)
        if len(cells) >= 5 and m:
            res = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", cells[4])
            rows[m.group(1)] = (cells[0], cells[1], cells[2], cells[3], res)
    return rows


def newest_tag():
    try:
        out = subprocess.run(["git", "tag", "--list", "v20*", "--sort=-creatordate"], capture_output=True, text=True).stdout
        return out.split()[0] if out.split() else ""
    except OSError:
        return ""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "_site/index.html"
    rows = readme_rows()
    reports = sorted(glob.glob("docs/lightbox/*-20??-??-??.html"), reverse=True)
    tr = []
    for p in reports:
        f = os.path.basename(p)
        soc, cam, date, lamps, res = rows.get(f, ("", f[:-16], f[-15:-5], "", ""))
        tr.append("<tr data-soc=\"%s\" data-date=\"%s\"><td>%s</td><td><a href=\"lightbox/%s\">%s</a></td><td>%s</td><td>%s</td><td>%s</td></tr>"
                  % (html.escape(soc.upper()), html.escape(date), html.escape(soc), html.escape(f), html.escape(cam), html.escape(date),
                     html.escape(lamps), html.escape(res)))
    socs = sorted({(rows.get(os.path.basename(p), ("",))[0] or "").upper() for p in reports} - {""})
    opts = "".join('<option value="%s">%s</option>' % (html.escape(x), html.escape(x)) for x in socs)
    tag = newest_tag()
    rel = ('<a href="%s/openimp/releases/tag/%s">%s</a>' % (REPO, tag, tag)) if tag else "see the releases page"
    page = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Open camera stack</title>
<style>
:root{--bg:#f6f7f9;--fg:#1d232b;--mut:#5b6672;--card:#fff;--line:#d9dee4;--acc:#1f6feb}
@media (prefers-color-scheme:dark){:root{--bg:#14171c;--fg:#e6e9ee;--mut:#9aa4af;--card:#1c2027;--line:#2c323b;--acc:#58a6ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:980px;margin:0 auto;padding:24px 16px 48px}h1{font-size:1.6em;margin:.2em 0}h2{font-size:1.15em;margin:1.6em 0 .5em}
p{color:var(--mut);margin:.3em 0 1em}a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.card b{display:block;margin-bottom:4px}.card span{color:var(--mut);font-size:.9em}
.tw{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:10px}
table{border-collapse:collapse;width:100%%;font-size:.92em}th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--mut);font-weight:600}
.flt{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:0 0 10px}
.flt input,.flt select{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:7px 10px;font:inherit}
.flt input{flex:1 1 220px;min-width:0}#cnt{color:var(--mut);font-size:.9em}tr:last-child td{border-bottom:0}footer{margin-top:28px;color:var(--mut);font-size:.85em}
</style></head><body><main>
<h1>Open camera stack: OpenIMP + open-tx-isp</h1>
<p>Open replacement of the Ingenic camera stack (kernel ISP driver open-tx-isp and the IMP user-space library OpenIMP) for
T10, T20, T21, T23, T30, T31, T40 and T41 (T32/T33 planned). Newest release: %s.</p>
<div class="cards">
<a class="card" href="feature-matrix.html"><b>Feature matrix</b><span>State per function and SoC, vendor-equal or better, release gates</span></a>
<a class="card" href="%s"><b>Project board</b><span>All open work with status</span></a>
<a class="card" href="%s/openimp/issues"><b>Issues: OpenIMP</b><span>Open issues and findings</span></a>
<a class="card" href="%s/open-tx-isp/issues"><b>Issues: open-tx-isp</b><span>Kernel driver issues</span></a>
<a class="card" href="%s/openimp/releases"><b>Releases</b><span>Tags, release notes and images</span></a>
<div class="card"><b>Changelog</b><span><a href="%s/openimp/blob/next/CHANGELOG.md">OpenIMP</a> and <a href="%s/open-tx-isp/blob/next/CHANGELOG.md">open-tx-isp</a></span></div>
</div>
<h2>Light-box protocols (open stack vs vendor stack)</h2>
<p>Each camera runs the vendor stack and the open stack one after the other in the same light box, same lamps and chart.
The protocols list every measurement with pictures and charts.</p>
<div class="flt"><input id="q" type="search" placeholder="Filter: camera, sensor, result, lamps ..." aria-label="Filter the reports">
<select id="soc" aria-label="SoC"><option value="">All SoCs</option>%s</select>
<select id="sort" aria-label="Sort"><option value="new">Newest first</option><option value="old">Oldest first</option><option value="soc">By SoC</option></select>
<span id="cnt"></span></div>
<div class="tw"><table><thead><tr><th>SoC</th><th>Camera / report</th><th>Date</th><th>Lamps and runs</th><th>Result</th></tr></thead>
<tbody>%s</tbody></table></div>
<script>
(function(){
  var q=document.getElementById("q"), soc=document.getElementById("soc"), so=document.getElementById("sort"), cnt=document.getElementById("cnt");
  var tb=document.querySelector("tbody"), rows=[].slice.call(tb.querySelectorAll("tr[data-date]"));
  function run(){
    var t=q.value.toLowerCase().trim(), s=soc.value, n=0, k=so.value;
    rows.sort(function(a,b){
      if(k=="soc"){var c=a.dataset.soc.localeCompare(b.dataset.soc); if(c) return c;}
      return k=="old" ? a.dataset.date.localeCompare(b.dataset.date) : b.dataset.date.localeCompare(a.dataset.date);
    }).forEach(function(r){
      var ok=(!s||r.dataset.soc==s)&&(!t||r.textContent.toLowerCase().indexOf(t)>=0);
      r.style.display=ok?"":"none"; if(ok) n++; tb.appendChild(r);
    });
    cnt.textContent=n+" of "+rows.length+" reports";
  }
  [q,soc,so].forEach(function(e){e.addEventListener("input",run);}); run();
})();
</script>
<footer>Generated from the <code>next</code> branch of <a href="%s/openimp">Lu-Fi/openimp</a>.</footer>
</main></body></html>
""" % (rel, BOARD, REPO, REPO, REPO, REPO, REPO, opts, "\n".join(tr) or "<tr><td colspan=5>none yet</td></tr>", REPO)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "w", encoding="utf-8").write(page)
    print("%s: %d light-box reports" % (out, len(reports)))


if __name__ == "__main__":
    main()
