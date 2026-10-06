#!/usr/bin/env python3
"""imgfx_collage.py - evaluate an imgfx run (host side, python3 + PIL only).

usage: imgfx_collage.py [--log FILE]... OUTDIR [OUTDIR ...]

OUTDIR is the directory the on-camera imgfx wrote ([<tag>.]NN-<case>.nv12 files and
summary-<SoC>.txt).  The "[R] <case> set=<ret> get=..." lines are read from
every summary-*.txt and *.log in OUTDIR plus every --log FILE (e.g. a saved
stdout of the run: a filtered re-run appends only its own cases to
summary-<SoC>.txt, so the lines of the full run can live elsewhere); later
lines win.  Cases without any [R] line are shown with ret "?".  For every
OUTDIR it writes
  jpg/NN-<case>.jpg          each picture as JPEG
  contact-1.jpg, -2.jpg ...  labelled contact sheets (label = case + set ret)
  report.txt                 which cases changed the picture (mean abs diff vs
                             base per channel Y/U/V, and dH = change of the
                             mean |Laplacian| of Y in %, the detail/noise
                             energy) plus flags

Flags:
  RET<0      the set call returned an error
  SUSPECT    set returned 0 (or the ret is unknown) but the picture did NOT
             change (function possibly not connected / no effect).  Only for
             cases where a visible change is expected; "info" cases
             (anti-flicker, fps, ...) are listed as no-change-expected.
  detail     verdict: Y/U/V mean diff below the threshold but the detail
             energy (dH) moved beyond its noise floor: sharpness / noise
             reduction / shading type changes show up here, not in dY.
  N/A        function is not exported by the camera's libimp
  size?      picture file is not 640x360 NV12
Pictures with the same <tag> (IMGFX_TAG, one per batch run) form a batch; every
case is compared with the base of its own batch and the contact sheets show
that base left of the case.
The noise floor comes from the base-end picture (taken after all cases) vs
base: a case counts as changed when a channel diff exceeds
max(1.0, 2.5 * noise) (U/V: max(0.4, 2.5 * noise)).  Keep the scene static
while the tool runs, otherwise motion/light changes look like changes.
"""
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageStat

W, H = 640, 360
# cases where no picture change is expected / detectable on a static scene
NO_CHANGE_OK = ("aflicker", "fps-", "awb-auto", "mode-day", "base", "hilight-depress", "backlight-comp",
                "wbalgo", "scaler-lv", "dpc-", "drc-ratio", "defog", "drc-", "wdr", "shading", "rawdrc", "bypass-lsc",
                "bypass-dpc", "bypass-adr", "bypass-defog", "bypass-sdns", "bypass-mdns", "bypass-ydns", "maxagain",
                "maxdgain", "isp-bypass", "ae-hlc", "ae-blc", "sinter", "temper")


def nv12_planes(data):
    y = Image.frombytes("L", (W, H), data[:W * H])
    uv = data[W * H:W * H * 3 // 2]
    u = Image.frombytes("L", (W // 2, H // 2), uv[0::2])
    v = Image.frombytes("L", (W // 2, H // 2), uv[1::2])
    return y, u, v


def to_rgb(planes):
    y, u, v = planes
    return Image.merge("YCbCr", (y, u.resize((W, H), Image.BILINEAR), v.resize((W, H), Image.BILINEAR))).convert("RGB")


def mad(a, b):
    return [ImageStat.Stat(ImageChops.difference(x, z)).mean[0] for x, z in zip(a, b)]


def parse_summary(d, extra=()):
    """case -> (status, ret, get); ret None = no [R] line found"""
    res = {}
    files = [os.path.join(d, fn) for fn in sorted(os.listdir(d))
             if (fn.startswith("summary-") and fn.endswith(".txt")) or fn.endswith(".log")]
    files += list(extra)
    for path in files:
        for line in open(path, errors="replace"):
            m = re.match(r"\[R\] (\S+) N/A", line)
            if m:
                res[m.group(1)] = ("N/A", None, "")
                continue
            m = re.match(r"\[R\] (\S+) set=(-?\d+) get=(.*)", line)
            if m:
                res[m.group(1)] = ("ok", int(m.group(2)), m.group(3).strip())
    return res


_LAP = ImageFilter.Kernel((3, 3), [0, -1, 0, -1, 4, -1, 0, -1, 0], scale=1, offset=128)


def detail(y):
    """mean |Laplacian| of the Y plane: edge + noise energy"""
    return ImageStat.Stat(ImageChops.difference(y.filter(_LAP), Image.new("L", y.size, 128))).mean[0]


def font(sz):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()


def process(d, extra=()):
    summ = parse_summary(d, extra)
    files = []
    for fn in sorted(os.listdir(d)):
        m = re.match(r"(?:(.+)\.)?(\d+)-(.+)\.nv12$", fn)
        if m:
            files.append((m.group(1) or "", int(m.group(2)), m.group(3), os.path.join(d, fn)))
    os.makedirs(os.path.join(d, "jpg"), exist_ok=True)
    # per batch (tag): its own base and base-end; a case is compared with the base of its batch
    pics = {}      # key (tag, case) -> (nn, planes, rgb, err)
    det = {}
    for tag, nn, case, path in files:
        data = open(path, "rb").read()
        if len(data) != W * H * 3 // 2:
            pics[(tag, case)] = (nn, None, None, "size?%d" % len(data))
            continue
        pl = nv12_planes(data)
        det[(tag, case)] = detail(pl[0])
        rgb = to_rgb(pl)
        rgb.save(os.path.join(d, "jpg", "%s%02d-%s.jpg" % (tag + "." if tag else "", nn, case)), quality=88)
        pics[(tag, case)] = (nn, pl, rgb, "")
    tags = sorted(set(t for t, _, _, _ in files), key=lambda t: min(n for tt, n, _, _ in files if tt == t))
    noise_of, thr_of, hnoise_of = {}, {}, {}
    for t in tags:
        bs, be = pics.get((t, "base")), pics.get((t, "base-end"))
        nz = mad(bs[1], be[1]) if bs and bs[1] and be and be[1] else [0.0, 0.0, 0.0]
        noise_of[t] = nz
        thr_of[t] = [max(1.0, 2.5 * nz[0]), max(0.4, 2.5 * nz[1]), max(0.4, 2.5 * nz[2])]
        hb = det.get((t, "base"))
        hnoise_of[t] = abs(det[(t, "base-end")] - hb) * 100.0 / hb if hb and (t, "base-end") in det else 0.0
    def med(v):
        v = sorted(v)
        return v[len(v) // 2] if v else 0.0
    noise = [med([noise_of[t][i] for t in tags]) for i in range(3)]
    thr = [med([thr_of[t][i] for t in tags]) for i in range(3)]
    hbase_all = [det[(t, "base")] for t in tags if (t, "base") in det]
    hbase = sum(hbase_all) / len(hbase_all) if hbase_all else 0.0
    hnoise = med(list(hnoise_of.values()))
    hthr = max(1.5, 3.0 * hnoise)
    # rows: one per picture (tag, case)
    rows = []
    for tag, nn, case, _ in sorted(files, key=lambda f: (f[1], f[0])):
        key = (tag, case)
        p = pics[key]
        st = summ.get(case, ("?", None, ""))
        isbase = case.startswith("base")
        base = pics.get((tag, "base"))
        hb = det.get((tag, "base"))
        diff = None
        if p[1] and base and base[1] and not isbase:
            diff = mad(p[1], base[1])
        t3 = thr_of[tag] if tag in thr_of else thr
        changed = diff is not None and any(x > t for x, t in zip(diff, t3))
        dh = None
        if hb and key in det and not isbase:
            dh = (det[key] - hb) * 100.0 / hb
        hth = max(1.5, 3.0 * hnoise_of.get(tag, 0.0))
        detailed = (not changed) and dh is not None and abs(dh) > hth
        flags = []
        if st[0] == "N/A":
            flags.append("N/A")
        if st[1] is not None and st[1] < 0:
            flags.append("RET<0")
        if p[3]:
            flags.append(p[3])
        if st[0] == "ok" and st[1] is None:
            flags.append("ret?")
        if (diff is not None and st[0] in ("ok", "?") and st[1] in (None, 0) and not changed and not detailed
                and not any(case.startswith(k) for k in NO_CHANGE_OK)):
            if "[stock-noop" in st[2]:
                flags.append("stock-noop")      # the tool says the vendor kernel has no effect either
            elif "[expect-same" in st[2]:
                flags.append("expected-same")   # e.g. csc mode 0 = the default preset
            else:
                flags.append("SUSPECT-unconnected")
        rows.append((nn, case if not isbase or not tag else "%s@%s" % (case, tag), st, diff, changed, flags, dh, detailed, key))
    for case in summ:   # cases with a result line but no picture
        if not any(k[1] == case for k in pics):
            rows.append((0, case, summ[case], None, False, ["N/A"] if summ[case][0] == "N/A" else [], None, False, ("", case)))
    # contact sheets: base of the case's batch (left) next to the case (right)
    shown = [r for r in rows if r[8] in pics and pics[r[8]][2] is not None]
    TW, TH, LH, COLS, PER = 240, 135, 34, 3, 18
    f = font(12)
    for page in range((len(shown) + PER - 1) // PER):
        chunk = shown[page * PER:(page + 1) * PER]
        nrows = (len(chunk) + COLS - 1) // COLS
        sheet = Image.new("RGB", (COLS * 2 * TW, nrows * (TH + LH)), (24, 24, 24))
        dr = ImageDraw.Draw(sheet)
        for i, (nn, case, st, diff, changed, flags, dh, detailed, key) in enumerate(chunk):
            x, y = (i % COLS) * 2 * TW, (i // COLS) * (TH + LH)
            bp = pics.get((key[0], "base"))
            if bp and bp[2] is not None:
                sheet.paste(bp[2].resize((TW, TH), Image.BILINEAR), (x, y))
            sheet.paste(pics[key][2].resize((TW, TH), Image.BILINEAR), (x + TW, y))
            dr.text((x + 3, y + 2), "base" + (" " + key[0] if key[0] else ""), fill=(255, 255, 80), font=f)
            ret = "?" if st[1] is None else str(st[1])
            col = (255, 90, 90) if any(fl.startswith(("RET", "SUSP")) for fl in flags) else (
                (120, 255, 120) if (changed or detailed) else (220, 220, 220))
            dr.text((x + 3, y + TH + 2), "%02d %s  set=%s" % (nn, case, ret), fill=col, font=f)
            dr.text((x + 3, y + TH + 17), (("dY %.1f dU %.1f dV %.1f dH %+.0f%%" % (tuple(diff) + (dh or 0.0,))) if diff else "reference"),
                    fill=col, font=f)
        sheet.save(os.path.join(d, "contact-%d.jpg" % (page + 1)), quality=88)
    # report
    out = []
    out.append("imgfx report for %s" % d)
    out.append("noise floor (median over batches, base vs base-end of the same batch): dY %.2f dU %.2f dV %.2f; change threshold Y>%.1f U>%.1f V>%.1f" % (
        tuple(noise) + tuple(thr)))
    out.append("batches (each case is compared with the base of its own batch): %d" % len(tags))
    for t in tags:
        bs = pics.get((t, "base"))
        out.append("  batch %-8s noise dY %.2f dU %.2f dV %.2f  base mean Y/U/V %s" % (t or "-", noise_of[t][0], noise_of[t][1], noise_of[t][2],
            ("%.1f/%.1f/%.1f" % tuple(ImageStat.Stat(p).mean[0] for p in bs[1])) if bs and bs[1] else "-"))
    out.append("detail energy (mean |Laplacian| of Y) mean base %.3f, median base-end drift %.2f %%; detail threshold %.1f %%" % (
        hbase or 0.0, hnoise, hthr))
    out.append("")
    out.append("%-3s %-24s %-5s %-6s %-6s %-6s %-7s %-9s %s" % ("NN", "case", "ret", "dY", "dU", "dV", "dH%", "verdict", "flags / readback"))
    suspects, bad, na, chg, det_only, noret = [], [], [], 0, [], 0
    for nn, case, st, diff, changed, flags, dh, detailed, _key in rows:
        if case.startswith("base") and _key[0]:
            continue   # per-batch base rows: see the batch list above
        ret = "?" if st[1] is None else str(st[1])
        if st[1] is None and st[0] != "N/A" and not case.startswith("base"):
            noret += 1
        if diff:
            ds = "%-6.2f %-6.2f %-6.2f %-+7.1f" % (tuple(diff) + (dh or 0.0,))
        else:
            ds = "%-6s %-6s %-6s %-7s" % ("-", "-", "-", "-")
        verdict = "N/A" if st[0] == "N/A" else ("changed" if changed else ("detail" if detailed else (
            "ref" if case.startswith("base") else "same")))
        if changed:
            chg += 1
        if detailed:
            det_only.append(case)
        out.append("%02d  %-24s %-5s %s %-9s %s %s" % (nn, case, ret, ds, verdict, " ".join(flags), st[2][:90]))
        if "SUSPECT-unconnected" in flags:
            suspects.append(case)
        if "RET<0" in flags:
            bad.append("%s(%s)" % (case, ret))
        if st[0] == "N/A":
            na.append(case)
    out.append("")
    out.append("changed the picture: %d" % chg)
    out.append("detail energy only (colour/brightness same, sharpness/noise moved): %s" % (", ".join(det_only) or "none"))
    if noret:
        out.append("no [R] line (ret unknown, shown as ?): %d cases - pass the stdout of the full run with --log" % noret)
    out.append("SUSPECT (set=0 but no picture change; maybe unconnected): %s" % (", ".join(suspects) or "none"))
    noops = [r[1] for r in rows if "stock-noop" in r[5] or "expected-same" in r[5]]
    if noops:
        out.append("no change expected (vendor kernel no-op / default value): %s" % ", ".join(noops))
    out.append("RET<0: %s" % (", ".join(bad) or "none"))
    out.append("N/A (not exported by libimp): %s" % (", ".join(na) or "none"))
    text = "\n".join(out) + "\n"
    open(os.path.join(d, "report.txt"), "w").write(text)
    print(text)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    args, logs, dirs = sys.argv[1:], [], []
    while args:
        a = args.pop(0)
        if a == "--log" and args:
            logs.append(args.pop(0))
        else:
            dirs.append(a)
    for dd in dirs:
        process(dd.rstrip("/"), logs)
