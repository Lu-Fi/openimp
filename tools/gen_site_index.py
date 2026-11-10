#!/usr/bin/env python3
"""gen_site_index.py: the start page of the GitHub Pages site (index.html).

Sections: header with navigation, hero (banner, newest release, key numbers), SoC status (completeness per SoC
from the README status table), light-box protocols (every docs/lightbox/<vendor>-<soc>-<sensor>-<date>.html with
the row of docs/lightbox/README.md; filter, SoC selector, sorting) and a footer.  Per camera alias also
cam/<alias>.html: header, table of all runs and inline SVG trend charts (open vs vendor) of the key numbers in
docs/lightbox/<protocol>.json (written next to each protocol by lightbox_report.py).
usage: gen_site_index.py OUT.html   (run from the repository root; the workflow copies docs/lightbox/ and the
banner/logo from docs/assets/ next to it)
"""
import glob, html, json, math, os, re, subprocess, sys
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
h2.lbh{display:flex;align-items:center;gap:12px}h2.lbh img{width:72px;height:72px;flex:none}
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
<section id="lightbox"><h2 class="lbh"><img src="lightbox/lightbox-logo.webp" alt="" width="96" height="96" loading="lazy">Light-box protocols</h2><p class="sub">Each camera runs the vendor stack and the open stack one after the other in the same light box, with the same lamps and chart. Every protocol lists the measurements, pictures, charts and how each deviation is followed up.</p>
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


# ------------------------------------------------------------------ per-camera trend pages (cam/<alias>.html)
# Input: docs/lightbox/<protocol>.json next to every protocol (written by lightbox_report.py, schema 1):
# {alias, soc, sensor, date, time, image{name, build, BUILD_ID, commits}, counts{pass, fail, fail_by_handling},
#  metrics{name: {open, vendor, unit, better} | {value, unit, better}}}
METRICS = [  # (key regex, label, section); first match wins, keys not listed are left out
    (r"^p1\.day\.luma$", "Brightness, defaults, day (frame Y)", "Picture"),
    (r"^p1\.night\.luma$", "Brightness, defaults, night (frame Y)", "Picture"),
    (r"^nv12_noise\.a\.Ytemp$", "NV12 temporal noise Y, matched exposure, day", "Picture"),
    (r"^nv12_noise\.night\.Ytemp$", "NV12 temporal noise Y, matched exposure, night", "Picture"),
    (r"^colour\.dE00_mean$", "Colour error to the chart (dE00 mean)", "Colour"),
    (r"^colour\.dE00_grey_normalised$", "Colour error, grey-normalised (dE00 mean)", "Colour"),
    (r"^colour\.dE00_open_vs_vendor$", "Colour difference open vs vendor (dE00 mean)", "Colour"),
    (r"^colour\.chroma_ratio$", "Saturation (chroma / chart, 1 = chart)", "Colour"),
    (r"^colour\.saturation_open_over_vendor$", "Saturation open / vendor (1 = vendor)", "Colour"),
    (r"^sharpness\.mtf50$", "Sharpness MTF50 (cycles/pixel)", "Sharpness"),
    (r"^sharpness\.rise_10_90$", "Edge rise 10-90 % (px)", "Sharpness"),
    (r"^enc\.", None, "Encoder"),
    (r"^ae_step\.", None, "AE response and startup"),
    (r"^startup\.", None, "AE response and startup"),
    (r"^noise_gain\.", None, "Noise vs analog gain"),
    (r"^cpu\.streamer$", "CPU of the streamer (% of all CPU)", "CPU and reload"),
    (r"^cpu\.system_busy$", "CPU, system busy (%)", "CPU and reload"),
    (r"^reload\.ok_cycles$", "Module reload: cycles with a valid picture", "CPU and reload"),
]
BETTER = {"lower": "lower is better", "higher": "higher is better", "one": "closer to 1 is better", "zero": "closer to 0 is better"}


def metric_label(key):
    """generated label for the dynamic metric families (encoder cases, AE step, startup, noise per gain)"""
    p = key.split(".")
    if p[0] == "enc":
        cod, _, case = p[1].partition("_")
        case = case.replace("fixqp", "FixQP ").replace("cbr", "CBR ") + (" kbit/s" if case.startswith("cbr") else "")
        what = {"I_mean_B": "I frame bytes", "P_mean_B": "P frame bytes", "P_over_I": "P/I size ratio", "psnr_mean": "PSNR mean (dB)",
                "ssim_mean": "SSIM mean"}.get(p[2], p[2])
        return "%s %s: %s" % ({"h264": "H.264", "h265": "H.265"}.get(cod, cod), case, what)
    if p[0] == "ae_step":
        return "AE step, lamps %s: %s" % ("on -> off" if p[1] == "off" else "off -> on", "settle time (s)" if p[2] == "settle_s" else "overshoot (%)")
    if p[0] == "startup":
        return "Startup: streamer start -> %s (s)" % ("first snapshot" if "snapshot" in p[1] else "first RTSP frame")
    if p[0] == "noise_gain":
        return "Noise Y at analog gain %s (temporal std)" % p[1]
    return key


def nice_ticks(lo, hi, n=4):
    span = hi - lo
    step = 10 ** math.floor(math.log10(span / n))
    for m in (1, 2, 2.5, 5, 10):
        if span / (step * m) <= n:
            step *= m
            break
    t, out = (lo // step) * step, []
    while t <= hi + step * 1e-9:
        if t >= lo - step * 1e-9:
            out.append(round(t, 10))
        t += step
    return out


def fmt(v):
    if v is None:
        return "-"
    a = abs(v)
    return ("%.0f" % v) if a >= 1000 else ("%.1f" % v) if a >= 100 else ("%.2f" % v) if a >= 1 else ("%.3f" % v) if a >= 0.01 else ("%.4f" % v)


def trend_svg(runs, key, label):
    """inline SVG line chart over the runs: open (solid, circles) and vendor (dashed reference line, squares); a cross-stack metric
    ({value}) shows one series against the vendor reference (0 = identical, 1 = same saturation)"""
    W, H, L, R, T, B = 360, 180, 46, 52, 12, 30
    pts = {"open": [], "vendor": []}
    single = False
    for i, (_, j) in enumerate(runs):
        m = j.get("metrics", {}).get(key) if j else None
        if not m:
            continue
        if "value" in m:
            single = True
            pts["open"].append((i, m.get("value")))
        else:
            for st in pts:
                if m.get(st) is not None:
                    pts[st].append((i, m.get(st)))
    ref = None
    if single:
        ref = 1.0 if (runs[-1][1] or {}).get("metrics", {}).get(key, {}).get("better") == "one" else 0.0
    vals = [v for s in pts.values() for _, v in s] + ([ref] if ref is not None else [])
    if not vals:
        return ""
    lo, hi = min(vals), max(vals)
    # minimum span: 20 % of the largest magnitude, so a 0.1 % difference does not fill the whole chart
    span = max(hi - lo, 0.2 * max(abs(v) for v in vals), 1e-6)
    mid = (lo + hi) / 2
    lo, hi = mid - span * 0.62, mid + span * 0.62
    if min(vals) >= 0 and lo < 0:
        lo = 0.0
    n = len(runs)
    xs = lambda i: L + (W - L - R) * (0.5 if n == 1 else i / (n - 1))
    ys = lambda v: T + (H - T - B) * (1 - (v - lo) / (hi - lo))
    g = ['<svg class="tc" viewBox="0 0 %d %d" role="img" aria-label="%s"><title>%s</title>' % (W, H, esc(label), esc(label))]
    tk = nice_ticks(lo, hi)
    nd = max(0, -int(math.floor(math.log10(tk[1] - tk[0]))) + (1 if round((tk[1] - tk[0]) / 10 ** math.floor(math.log10(tk[1] - tk[0])), 6) == 2.5 else 0)) if len(tk) > 1 else 2
    for t in tk:
        g.append('<line class="grid" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/><text class="ax" x="%d" y="%.1f" text-anchor="end">%s</text>'
                 % (L, W - R, ys(t), ys(t), L - 6, ys(t) + 4, ("%%.%df" % nd) % t))
    every = max(1, (n + 5) // 6)
    for i, (d, _) in enumerate(runs):
        if i % every == 0 or i == n - 1:
            g.append('<text class="ax" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (xs(i), H - B + 17, esc(d[5:10] + (" " + d[11:] if len(d) > 10 else ""))))
    g.append('<line class="base" x1="%d" x2="%d" y1="%d" y2="%d"/>' % (L, W - R, H - B, H - B))
    if ref is not None:
        g.append('<line class="sv ref" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/><text class="ax" x="%d" y="%.1f" text-anchor="end">vendor</text>'
                 % (L, W - R, ys(ref), ys(ref), W - R, ys(ref) - 5))
    labs = []
    for st, cls in (("vendor", "sv"), ("open", "so")):
        s = pts[st]
        if len(s) > 1:
            g.append('<polyline class="%s" points="%s"/>' % (cls, " ".join("%.1f,%.1f" % (xs(i), ys(v)) for i, v in s)))
        for i, v in s:
            x, y = xs(i), ys(v)
            nm = "open vs vendor" if single else st
            mark = ('<rect class="m %s" x="%.1f" y="%.1f" width="9" height="9" rx="1.5"/>' % (cls, x - 4.5, y - 4.5)) if st == "vendor" else \
                ('<circle class="m %s" cx="%.1f" cy="%.1f" r="4.5"/>' % (cls, x, y))
            lab = ""
            if i == s[-1][0]:      # direct label at the newest point; vendor label moved off the open one when they would touch
                ly = y + 4
                if st == "vendor" and pts["open"] and pts["open"][-1][0] == i and abs(ys(pts["open"][-1][1]) - y) < 15:
                    ly = ys(pts["open"][-1][1]) + 4 + (15 if y >= ys(pts["open"][-1][1]) else -15)
                    labs.append(ly)
                elif st == "open" and labs and abs(labs[-1] - ly) < 15:
                    ly = labs[-1] + (-15 if ly <= labs[-1] else 15)
                lab = '<text class="dl" x="%.1f" y="%.1f">%s</text>' % (x + 8, ly, esc(fmt(v)))
            g.append('<g class="pt">%s%s<circle class="hit" cx="%.1f" cy="%.1f" r="13"><title>%s %s: %s</title></circle></g>'
                     % (mark, lab, x, y, esc(runs[i][0]), nm, esc(fmt(v))))
    g.append("</svg>")
    return "".join(g)


def cam_page(alias, runs, cam, soc):
    """runs: [(date label, protocol file, summary json or None, readme result text)] oldest first"""
    css = re.search(r"<style>(.*?)</style>", PAGE.template, re.S).group(1)
    last = next((j for _, _, j, _ in reversed(runs) if j), {}) or {}
    sensor = last.get("sensor") or (alias.rsplit("-", 1)[-1] if "-" in alias else "?")
    soc = last.get("soc") or soc or "?"
    rows = []
    for d, f, j, res in reversed(runs):
        cnt = (j or {}).get("counts") or {}
        p, nf = (cnt.get("pass"), cnt.get("fail")) if cnt else counts(res)
        split = ", ".join("%d %s" % (v, k) for k, v in sorted((cnt.get("fail_by_handling") or {}).items(), key=lambda kv: -kv[1]))
        im = (j or {}).get("image") or {}
        cm = ", ".join("%s %s" % (k, v[:8]) for k, v in sorted((im.get("commits") or {}).items()) if v)
        imtxt = "<b>%s</b>" % esc(im.get("build") or im.get("BUILD_ID") or "-")
        if im.get("name"):
            imtxt += '<br><span class="mut">%s</span>' % esc(im["name"])
        if cm:
            imtxt += '<br><span class="mut">%s</span>' % esc(cm)
        ko = (((j or {}).get("stack") or {}).get("open") or {}).get("ko")
        if ko:
            imtxt += '<br><span class="mut">open: %s</span>' % esc(ko)
        rows.append('<tr><td class="nw"><a href="../lightbox/%s">%s</a></td><td class="num">%s</td><td class="num">%s%s</td><td>%s</td><td class="nw">%s</td></tr>' % (
                        esc(f), esc(d), "-" if p is None else '<span class="chip ok">%d</span>' % p,
                        "-" if nf is None else '<span class="chip %s">%d</span>' % ("warn" if nf else "ok", nf),
                        ('<br><span class="mut fs">%s</span>' % esc(split)) if split else "", imtxt,
                        esc((j or {}).get("lamp_ct") and "%d K" % j["lamp_ct"] or "-")))
    # charts: every metric key of any run, in the METRICS order, grouped by section
    keys = list(dict.fromkeys(k for _, _, j, _ in runs if j for k in sorted(j.get("metrics", {}))))
    secs, order = {}, []
    for rx, lab, sec in METRICS:
        for k in keys:
            if re.search(rx, k) and not any(k in [x[0] for x in v] for v in secs.values()):
                if sec not in secs:
                    secs[sec] = []; order.append(sec)
                secs[sec].append((k, lab or metric_label(k)))
    tr = [(d, j) for d, _, j, _ in runs]
    nb = {"better": 0, "equal": 0, "worse": 0, "differs": 0}
    body = []
    for sec in order:
        cards = []
        for k, lab in secs[sec]:
            svg = trend_svg(tr, k, lab)
            if not svg:
                continue
            ms = [(d, (j or {}).get("metrics", {}).get(k)) for d, j in tr if j and (j.get("metrics") or {}).get(k)]
            m = ms[-1][1]
            single = "value" in m
            bt = m.get("better", "")
            if single:
                now = 'open vs vendor <b>%s</b>' % esc(fmt(m.get("value")))
                o, v = m.get("value"), (1.0 if bt == "one" else 0.0)
            else:
                o, v = m.get("open"), m.get("vendor")
                now = '<i class="lg so"></i>open <b>%s</b> <i class="lg sv"></i>vendor <b>%s</b>' % (esc(fmt(o)), esc(fmt(v)))
            chip = ""
            if bt and o is not None and v is not None:
                if bt in ("one", "zero"):
                    tgt = 1.0 if bt == "one" else 0.0
                    if single:
                        cl = "equal" if abs(o - tgt) <= 0.03 else "differs"
                    else:
                        do, dvv = abs(o - tgt), abs(v - tgt)
                        cl = "equal" if abs(do - dvv) <= 0.03 * max(abs(tgt), abs(v), 1e-9) else "better" if do < dvv else "worse"
                elif single:      # cross-stack dE00: below 1 = not visible
                    cl = "equal" if abs(o - v) < (1.0 if m.get("unit") == "dE00" else 0.03) else "differs"
                else:
                    rel = (o - v) / abs(v) if v else (0.0 if o == v else 1.0)
                    cl = "equal" if abs(rel) <= 0.03 else ("better" if (rel < 0) == (bt == "lower") else "worse")
                nb[cl] += 1
                chip = '<span class="chip %s">%s</span>' % ({"better": "ok", "equal": "eq", "worse": "warn", "differs": "warn"}[cl], cl)
            note = '<p class="note">%d run%s with this metric%s</p>' % (len(ms), "" if len(ms) == 1 else "s", ": more runs needed for a trend" if len(ms) < 2 else "")
            tbl = "".join('<tr><td class="nw">%s</td>%s</tr>' % (esc(d), ('<td class="num">%s</td>' % esc(fmt(x.get("value")))) if "value" in x else
                          '<td class="num">%s</td><td class="num">%s</td>' % (esc(fmt(x.get("open"))), esc(fmt(x.get("vendor"))))) for d, x in reversed(ms))
            cards.append('<figure class="tcard"><figcaption><b>%s</b><span class="mut">%s%s</span><span class="now">%s %s</span></figcaption>%s%s'
                         '<details><summary>Values</summary><table class="vt"><tr><th>run</th>%s</tr>%s</table></details></figure>' % (
                             esc(lab), esc(m.get("unit", "")), (" &middot; " + BETTER[bt]) if bt in BETTER else "", now, chip, svg, note,
                             "<th>open vs vendor</th>" if single else "<th>open</th><th>vendor</th>", tbl))
        if cards:
            body.append('<section id="%s"><h2>%s</h2><div class="tgrid">%s</div></section>' % (
                esc(re.sub(r"\W+", "-", sec.lower())), esc(sec), "".join(cards)))
    nj = sum(1 for r in runs if r[2])
    lp = last.get("counts") or {}
    stats = ('<div class="stats"><div class="stat"><div class="v">%d</div><div class="k">light-box runs</div></div>'
             '<div class="stat"><div class="v">%s</div><div class="k">checks pass, newest run</div></div>'
             '<div class="stat"><div class="v">%s</div><div class="k">checks fail, newest run</div></div>'
             '<div class="stat"><div class="v">%d / %d / %d</div><div class="k">metrics open better / equal / worse than vendor (newest)</div></div></div>'
             % (len(runs), lp.get("pass", "-"), lp.get("fail", "-"), nb["better"], nb["equal"], nb["worse"]))
    extra = """
.crumb{color:var(--mut);font-size:.9em;margin:22px 0 4px}h1{font-size:1.6em;margin:0 0 8px;line-height:1.2}
.pills{display:flex;flex-wrap:wrap;gap:6px}
.tw{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:12px;box-shadow:var(--shadow)}
table{border-collapse:collapse;width:100%;font-size:.9em}th,td{padding:8px 12px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--mut);font-weight:600;font-size:.88em}tr:last-child td{border-bottom:0}td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}.nw{white-space:nowrap}
.mut{color:var(--mut)}.fs{display:inline-block;min-width:9em;font-size:.88em}.chip.eq{background:var(--line);color:var(--fg)}
.legend{display:flex;flex-wrap:wrap;gap:14px;align-items:center;color:var(--mut);font-size:.9em;margin:0 0 4px}
.tgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(340px,100%),1fr));gap:12px}
.tcard{margin:0;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px 10px;box-shadow:var(--shadow);min-width:0}
.tcard figcaption{display:flex;flex-direction:column;gap:2px;margin-bottom:6px}.tcard figcaption .mut{font-size:.82em}
.tcard .now{font-size:.88em;display:flex;flex-wrap:wrap;align-items:center;gap:4px 8px;margin-top:4px}.tcard .now b{font-variant-numeric:tabular-nums}
.tc{display:block;width:100%;height:auto;overflow:visible}
.tc .grid{stroke:var(--line);stroke-width:1}.tc .base{stroke:var(--mut);stroke-width:1}
.tc .ax{fill:var(--mut);font-size:11px;font-variant-numeric:tabular-nums}.tc .dl{fill:var(--fg);font-size:12px;font-weight:600;font-variant-numeric:tabular-nums}
.tc polyline,.tc .ref{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.tc polyline.so{stroke:var(--s-open)}.tc polyline.sv,.tc .ref{stroke:var(--s-vendor);stroke-dasharray:6 4}
.tc .m{stroke:var(--card);stroke-width:2}.tc .m.so{fill:var(--s-open)}.tc .m.sv{fill:var(--s-vendor)}
.tc .hit{fill:transparent;stroke:none;cursor:default}.tc .pt:hover .m{stroke:var(--fg)}
i.lg{display:inline-block;width:18px;height:0;border-top:2px solid var(--s-open);vertical-align:middle;position:relative}
i.lg.sv{border-top:2px dashed var(--s-vendor)}
.note{margin:4px 0 0;color:var(--mut);font-size:.82em}
.tcard details{font-size:.85em;color:var(--mut);margin-top:4px}.tcard summary{cursor:pointer}.vt{margin-top:6px}.vt td,.vt th{padding:4px 8px}
:root{--s-open:#3e63dd;--s-vendor:#6b7280}
@media (prefers-color-scheme:dark){:root{--s-open:#6d8cf5;--s-vendor:#6b7280}}
"""
    return """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>%(cam)s trend</title>
<meta name="description" content="Light-box results of %(cam)s (%(soc)s, %(sensor)s) over time: open stack vs vendor stack.">
<link rel="icon" href="../assets/logo.svg" type="image/svg+xml">
<style>%(css)s%(extra)s</style></head><body>
<header class="top"><div class="wrap">
  <a class="brand" href="../"><img src="../assets/logo.svg" alt="">OpenIMP</a>
  <nav><a href="#runs">Runs</a><a href="#trends">Trends</a><a href="../#lightbox">All cameras</a><a href="../feature-matrix.html">Matrix</a><a href="%(repo)s/openimp">GitHub</a></nav>
</div></header>
<main class="wrap">
<p class="crumb"><a href="../">Start</a> &rsaquo; <a href="../#lightbox">Light box</a> &rsaquo; %(alias)s</p>
<h1>%(cam)s</h1>
<div class="pills"><span class="pill">SoC <b>%(soc)s</b></span><span class="pill">Sensor <b>%(sensor)s</b></span><span class="pill">Alias <b>%(alias)s</b></span></div>
%(stats)s
<section id="runs"><h2>Runs</h2><p class="sub">Every light-box protocol of this camera, newest first; the date opens the protocol. Image = flashed firmware build of the open stack.</p>
<div class="tw"><table><thead><tr><th>protocol</th><th class="num">pass</th><th class="num">fail (handling)</th><th>image / stack</th><th>lamps</th></tr></thead><tbody>%(rows)s</tbody></table></div></section>
<section id="trends"><h2>Development over time</h2><p class="sub">Key numbers of every run. The vendor stack is the reference (dashed line, squares); the open stack is the solid line with circles. %(nj)s</p>
<div class="legend"><span><i class="lg so"></i> open stack (open-tx-isp + OpenIMP)</span><span><i class="lg sv"></i> vendor stack (reference)</span></div></section>
%(body)s
</main>
<footer><div class="wrap"><span>Generated %(gen)s from the light-box protocols in <a href="%(repo)s/openimp/tree/next/docs/lightbox">docs/lightbox</a>.</span>
<span><a href="../">Start page</a> &middot; <a href="%(repo)s/openimp">OpenIMP</a> &middot; <a href="%(repo)s/open-tx-isp">open-tx-isp</a></span></div></footer>
</body></html>
""" % dict(css=css, extra=extra, cam=esc(cam), soc=esc(soc), sensor=esc(sensor), alias=esc(alias), repo=REPO, stats=stats, rows="".join(rows),
           body="".join(body) or '<p class="sub">No machine-readable key numbers yet (protocols older than the JSON summary).</p>',
           nj=("A metric with a single run shows one point: more runs are needed for a trend." if len(runs) < 2 else "") +
              ("" if nj == len(runs) else " %d of %d runs have no key-number file and are not in the charts." % (len(runs) - nj, len(runs))),
           gen=datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"))


def load_json(path):
    try:
        j = json.load(open(path, encoding="utf-8"))
        return j if isinstance(j, dict) else None
    except (OSError, ValueError):
        return None


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
        t = m.group(3)[:2] + ":" + m.group(3)[2:] if m.group(3) else ""
        if not t:   # no -HHMM in the file name: the run time from the protocol's key-number file (user 2026-11-10: always a time)
            j = load_json(p[:-5] + ".json")
            t = str(j.get("time") or "") if j else ""
        groups.setdefault(m.group(1), []).append((m.group(2) + (" " + t if t else ""), f))
    reps, socset, ncam = [], set(), 0
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
        mf = re.search(r"(\d+)\s*flaky", res)
        if mf and int(mf.group(1)):
            chips += '<span class="chip warn">%s flaky</span>' % mf.group(1)
        note = re.sub(r"^\s*report\s*", "", re.sub(r"\(?\d+\s*pass\s*/\s*\d+\s*(?:fail|differ)(?:\s*/\s*\d+\s*flaky)?\)?[,;]?\s*", "", res)).strip(" ()")
        reps.append('<article class="rep" data-soc="%s" data-date="%s"><span class="socb">%s</span>'
                    '<div><div class="t"><a href="lightbox/%s">%s</a></div><div class="m">%s &middot; %s%s</div></div>'
                    '<div class="chips">%s</div>%s<a class="open" href="cam/%s.html">Development over time &rarr;</a></article>'
                    % (esc(soc.upper()), esc(date), esc(soc or "?"), esc(f), esc(cam), esc(date), esc(lamps),
                       (" &middot; " + esc(note)) if note else "", chips, hist, esc(alias)))
        # trend page of this camera: all its runs oldest first, with the key-number JSON next to each protocol
        trend = [(d, hf, load_json(os.path.join("docs/lightbox", hf[:-5] + ".json")), rows.get(hf, ("", "", "", "", ""))[4])
                 for d, hf in sorted(runs)]
        cdir = os.path.join(os.path.dirname(out) or ".", "cam")
        os.makedirs(cdir, exist_ok=True)
        open(os.path.join(cdir, alias + ".html"), "w", encoding="utf-8").write(cam_page(alias, trend, cam, soc.upper()))
        ncam += 1
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
    print("%s: %d SoCs, %d light-box protocols, %d camera trend pages" % (out, len(st), len(reports), ncam))


if __name__ == "__main__":
    main()
