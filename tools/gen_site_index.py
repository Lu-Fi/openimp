#!/usr/bin/env python3
"""gen_site_index.py: the start page of the GitHub Pages site (index.html).

Sections: header with navigation, hero (banner, newest release, key numbers), SoC status (completeness per SoC
from the README status table), light-box protocols (every docs/lightbox/<vendor>-<soc>-<sensor>-<date>.html with
the row of docs/lightbox/README.md; filter, SoC selector, sorting) and a footer.
usage: gen_site_index.py OUT.html   (run from the repository root; the workflow copies docs/lightbox/ and the
banner/logo from docs/assets/ next to it)
"""
import glob, html, os, re, subprocess, sys
from datetime import datetime, timezone
from string import Template

REPO = "https://github.com/Lu-Fi"
BOARD = "https://github.com/users/Lu-Fi/projects/1"
PLANNED = ("T32", "T33")


def esc(s):
    return html.escape(str(s), quote=True)


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


def soc_status():
    """[(soc, functions, done, pct, status)] from the README status table ('| T10 | 329 | 304 (92.4 %) | ... | RC3 gate |')"""
    out = []
    try:
        text = open("README.md", encoding="utf-8").read()
    except OSError:
        return out
    for m in re.finditer(r"^\|\s*(T\d\d)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\(([\d.]+)\s*%\)\s*\|.*\|\s*([^|]+?)\s*\|\s*$", text, re.M):
        out.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4)), m.group(5)))
    return out


def newest_tag():
    try:
        out = subprocess.run(["git", "tag", "--list", "v20*", "--sort=-creatordate"], capture_output=True, text=True).stdout.split()
        return out[0] if out else ""
    except OSError:
        return ""


def counts(res):
    """'28 pass / 3 differ' -> (28, 3)"""
    p = re.search(r"(\d+)\s*pass", res)
    f = re.search(r"(\d+)\s*(?:fail|differ)", res)
    return (int(p.group(1)) if p else None, int(f.group(1)) if f else None)


PAGE = Template(r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenIMP open camera stack</title>
<meta name="description" content="Open replacement of the Ingenic camera stack: open-tx-isp kernel ISP driver and the OpenIMP library, with vendor comparisons in a light box.">
<link rel="icon" href="assets/logo.svg" type="image/svg+xml">
<style>
:root{--bg:#f5f6f8;--fg:#17202a;--mut:#5c6773;--card:#ffffff;--line:#dde2e8;--acc:#3e63dd;--acc-soft:#e8edfd;
  --ok:#1a7f4b;--ok-soft:#e3f4ea;--warn:#a15c00;--warn-soft:#fdf0dc;--plan:#6b7280;--shadow:0 1px 2px rgba(16,24,40,.06),0 1px 3px rgba(16,24,40,.08)}
@media (prefers-color-scheme:dark){:root{--bg:#0f1216;--fg:#e6e9ee;--mut:#97a1ad;--card:#171b21;--line:#272d36;--acc:#7c9cff;--acc-soft:#1c2440;
  --ok:#4cc38a;--ok-soft:#15281f;--warn:#f0b35a;--warn-soft:#2c2214;--plan:#8b93a0;--shadow:none}}
*{box-sizing:border-box}html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 Inter,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
.wrap{max-width:1080px;margin:0 auto;padding:0 16px}
header.top{position:sticky;top:0;z-index:5;background:color-mix(in srgb,var(--bg) 86%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
header.top .wrap{display:flex;align-items:center;gap:16px;height:56px}
.brand{display:flex;align-items:center;gap:10px;font-weight:650;color:var(--fg)}.brand img{width:26px;height:26px}
nav{margin-left:auto;display:flex;gap:4px;flex-wrap:nowrap;overflow-x:auto}
nav a{color:var(--mut);padding:6px 10px;border-radius:8px;font-size:.93em;white-space:nowrap}nav a:hover{background:var(--acc-soft);color:var(--acc);text-decoration:none}
.hero{padding:40px 0 18px}.hero picture img{display:block;max-width:min(520px,100%);height:auto}
.hero p.lead{font-size:1.12em;color:var(--mut);max-width:720px;margin:14px 0 18px}
.pill{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);background:var(--card);border-radius:999px;padding:4px 12px;font-size:.88em;color:var(--mut)}
.pill b{color:var(--fg)}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin:22px 0 6px}
.stat{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;box-shadow:var(--shadow)}
.stat .v{font-size:1.7em;font-weight:700;line-height:1.1}.stat .k{color:var(--mut);font-size:.88em;margin-top:4px}
section{padding:26px 0}h2{font-size:1.25em;margin:0 0 4px}.sub{color:var(--mut);margin:0 0 16px}
.socs{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.soc{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px;box-shadow:var(--shadow)}
.soc .n{display:flex;justify-content:space-between;align-items:baseline;font-weight:650}.soc .n span{font-weight:500;font-size:.8em;color:var(--mut)}
.bar{height:6px;border-radius:6px;background:var(--line);margin:10px 0 6px;overflow:hidden}.bar i{display:block;height:100%;background:var(--acc)}
.soc .p{font-size:.85em;color:var(--mut)}.soc.plan{opacity:.8}.soc.plan .bar i{background:var(--plan)}
.links{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
.link{display:block;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;color:var(--fg);box-shadow:var(--shadow);transition:border-color .15s}
.link:hover{border-color:var(--acc);text-decoration:none}.link b{display:block;margin-bottom:2px}.link span{color:var(--mut);font-size:.9em}
.flt{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:0 0 12px}
.flt input,.flt select{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:10px;padding:8px 12px;font:inherit}
.flt input{flex:1 1 240px;min-width:0}#cnt{color:var(--mut);font-size:.9em;margin-left:auto}
.reps{display:grid;gap:10px}
.rep{display:grid;grid-template-columns:auto 1fr auto;gap:6px 14px;align-items:center;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 16px;box-shadow:var(--shadow)}
.rep .socb{font-weight:700;font-size:.85em;color:var(--acc);background:var(--acc-soft);border-radius:8px;padding:6px 9px;align-self:start}
.rep .t{font-weight:600}.rep .t a{color:var(--fg)}.rep .m{color:var(--mut);font-size:.88em}
.chips{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.chip{font-size:.8em;border-radius:999px;padding:3px 10px;white-space:nowrap}.chip.plan{background:var(--line);color:var(--plan)}
.soc .n .chip{font-weight:600;font-size:.72em;padding:2px 8px}.chip.ok{background:var(--ok-soft);color:var(--ok)}.chip.warn{background:var(--warn-soft);color:var(--warn)}
.rep .open{grid-column:3;justify-self:end;font-size:.88em}
.rep .hist{grid-column:1/-1;font-size:.88em;color:var(--mut)}.rep .hist summary{cursor:pointer}.rep .hist ul{margin:6px 0 0;padding-left:18px}
@media (max-width:640px){.rep{grid-template-columns:auto 1fr}.chips,.rep .open{grid-column:1/-1;justify-content:flex-start;justify-self:start}}
footer{border-top:1px solid var(--line);margin-top:24px;padding:20px 0 40px;color:var(--mut);font-size:.88em}
footer .wrap{display:flex;flex-wrap:wrap;gap:8px 20px;justify-content:space-between}
</style></head><body>
<header class="top"><div class="wrap">
  <a class="brand" href="./"><img src="assets/logo.svg" alt="">OpenIMP</a>
  <nav><a href="#status">Status</a><a href="#lightbox">Light box</a><a href="feature-matrix.html">Matrix</a><a href="$board">Board</a><a href="$repo/openimp/issues">Issues</a><a href="$repo/openimp/releases">Releases</a><a href="$repo/openimp">GitHub</a></nav>
</div></header>
<main class="wrap">
<div class="hero">
  <picture><source srcset="assets/banner-dark.svg" media="(prefers-color-scheme: dark)"><img src="assets/banner.svg" alt="OpenIMP - OSS stack for Ingenic SoCs" width="520"></picture>
  <p class="lead">An open replacement of the Ingenic camera stack: the <b>open-tx-isp</b> kernel ISP driver and the <b>OpenIMP</b> library,
  measured feature by feature against the vendor stack, in a light box, on real cameras.</p>
  <span class="pill">Newest release <b>$release</b></span>
  <div class="stats">
    <div class="stat"><div class="v">$ngate</div><div class="k">SoCs at the release gate</div></div>
    <div class="stat"><div class="v">$avg %</div><div class="k">of vendor functions done (mean)</div></div>
    <div class="stat"><div class="v">$nrep</div><div class="k">light-box protocols</div></div>
    <div class="stat"><div class="v">$nplan</div><div class="k">SoCs planned</div></div>
  </div>
</div>
<section id="status"><h2>Status per SoC</h2><p class="sub">Share of the vendor IMP functions implemented and checked, from the feature matrix. <a href="feature-matrix.html">Full matrix &rarr;</a></p>
<div class="socs">$socs</div></section>
<section><h2>Project</h2><p class="sub">Where the work is tracked.</p><div class="links">
<a class="link" href="feature-matrix.html"><b>Feature matrix</b><span>Every function per SoC: done, vendor-equal or better, release gates</span></a>
<a class="link" href="$board"><b>Project board</b><span>All open work with its status</span></a>
<a class="link" href="$repo/openimp/issues"><b>Issues</b><span>OpenIMP; kernel driver: <span>open-tx-isp</span></span></a>
<a class="link" href="$repo/openimp/blob/next/CHANGELOG.md"><b>Changelog</b><span>What changed, with device evidence</span></a>
</div></section>
<section id="lightbox"><h2>Light-box protocols</h2><p class="sub">Each camera runs the vendor stack and the open stack one after the other in the same light box, with the same lamps and chart. Every protocol lists the measurements, pictures, charts and how each deviation is followed up.</p>
<div class="flt"><input id="q" type="search" placeholder="Filter: camera, sensor, lamps, result ..." aria-label="Filter the protocols">
<select id="soc" aria-label="SoC"><option value="">All SoCs</option>$opts</select>
<select id="sort" aria-label="Sort"><option value="new">Newest first</option><option value="old">Oldest first</option><option value="soc">By SoC</option></select>
<span id="cnt"></span></div>
<div class="reps" id="reps">$reps</div></section>
</main>
<footer><div class="wrap"><span>Generated $gen from the <code>next</code> branch of <a href="$repo/openimp">Lu-Fi/openimp</a>.</span>
<span><a href="$repo/openimp">OpenIMP</a> &middot; <a href="$repo/open-tx-isp">open-tx-isp</a> &middot; <a href="$repo/openimp/releases">Releases</a></span></div></footer>
<script>
(function(){
  var q=document.getElementById("q"),soc=document.getElementById("soc"),so=document.getElementById("sort"),cnt=document.getElementById("cnt"),box=document.getElementById("reps");
  var items=[].slice.call(box.querySelectorAll(".rep"));
  function run(){var t=q.value.toLowerCase().trim(),s=soc.value,k=so.value,n=0;
    items.sort(function(a,b){if(k=="soc"){var c=a.dataset.soc.localeCompare(b.dataset.soc);if(c)return c;}
      return k=="old"?a.dataset.date.localeCompare(b.dataset.date):b.dataset.date.localeCompare(a.dataset.date);})
    .forEach(function(r){var ok=(!s||r.dataset.soc==s)&&(!t||r.textContent.toLowerCase().indexOf(t)>=0);r.style.display=ok?"":"none";if(ok)n++;box.appendChild(r);});
    cnt.textContent=n+" of "+items.length+" protocols";}
  [q,soc,so].forEach(function(e){e.addEventListener("input",run);});run();})();
</script>
</body></html>
""")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "_site/index.html"
    rows = readme_rows()
    # every protocol is kept: <alias>-<YYYY-MM-DD>[-<HHMM>].html; one card per camera alias (newest run),
    # the earlier runs of that camera in a "history" list below it
    reports = sorted(glob.glob("docs/lightbox/*-20??-??-??*.html"), reverse=True)
    groups = {}
    for p in reports:
        f = os.path.basename(p)
        m = re.match(r"(.+?)-(20\d\d-\d\d-\d\d)(?:-(\d{4}))?\.html$", f)
        if not m:
            continue
        groups.setdefault(m.group(1), []).append((m.group(2) + (" " + m.group(3)[:2] + ":" + m.group(3)[2:] if m.group(3) else ""), f))
    reps, socset = [], set()
    for alias, runs in sorted(groups.items(), key=lambda kv: max(r[0] for r in kv[1]), reverse=True):
        runs.sort(reverse=True)
        f = runs[0][1]
        soc, cam, date, lamps, res = rows.get(f, ("", alias, runs[0][0], "", ""))
        date = runs[0][0]
        socset.add(soc.upper())
        hist = ""
        if len(runs) > 1:
            items = []
            for d, hf in runs[1:]:
                hres = rows.get(hf, ("", "", "", "", ""))[4]
                hp, hn = counts(hres)
                items.append('<li><a href="lightbox/%s">%s</a>%s</li>' % (esc(hf), esc(d),
                             (" &middot; %d pass / %d %s" % (hp, hn or 0, "differ" if "differ" in hres else "fail")) if hp is not None else ""))
            hist = '<details class="hist"><summary>History: %d earlier run%s</summary><ul>%s</ul></details>' % (
                len(runs) - 1, "s" if len(runs) > 2 else "", "".join(items))
        npass, nfail = counts(res)
        chips = ""
        if npass is not None:
            chips += '<span class="chip ok">%d pass</span>' % npass
        if nfail:
            chips += '<span class="chip warn">%d %s</span>' % (nfail, "differ" if "differ" in res else "fail")
        note = re.sub(r"^\s*report\s*", "", re.sub(r"\(?\d+\s*pass\s*/\s*\d+\s*(?:fail|differ)\)?[,;]?\s*", "", res)).strip(" ()")
        reps.append('<article class="rep" data-soc="%s" data-date="%s"><span class="socb">%s</span>'
                    '<div><div class="t"><a href="lightbox/%s">%s</a></div><div class="m">%s &middot; %s%s</div></div>'
                    '<div class="chips">%s</div>%s</article>'
                    % (esc(soc.upper()), esc(date), esc(soc or "?"), esc(f), esc(cam), esc(date), esc(lamps),
                       (" &middot; " + esc(note)) if note else "", chips, hist))
    st = soc_status()
    cards = []
    for soc, nf, nd, pct, status in st:
        passed = "gate" in status.lower()   # README status "RC3 gate" = passed the RC3 release gate
        chip = '<span class="chip %s" title="%s">%s%s</span>' % ("ok" if passed else "warn", esc(status),
                                                                 "&#10003; " if passed else "", esc(status))
        cards.append('<div class="soc"><div class="n">%s%s</div><div class="bar"><i style="width:%.1f%%"></i></div>'
                     '<div class="p">%.1f %% &middot; %d of %d</div></div>' % (esc(soc), chip, pct, pct, nd, nf))
    for soc in PLANNED:
        cards.append('<div class="soc plan"><div class="n">%s<span class="chip plan">planned</span></div><div class="bar"><i style="width:0"></i></div>'
                     '<div class="p">QEMU only</div></div>' % soc)
    avg = sum(x[3] for x in st) / len(st) if st else 0.0
    tag = newest_tag()
    page = PAGE.substitute(
        repo=REPO, board=BOARD, release=esc(tag or "-"), ngate=len(st), avg="%.1f" % avg, nrep=len(reports), nplan=len(PLANNED),
        socs="".join(cards),
        opts="".join('<option value="%s">%s</option>' % (esc(x), esc(x)) for x in sorted(socset - {""})),
        reps="".join(reps) or '<p class="sub">No protocols yet.</p>',
        gen=datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "w", encoding="utf-8").write(page)
    print("%s: %d SoCs, %d light-box protocols" % (out, len(st), len(reports)))


if __name__ == "__main__":
    main()
