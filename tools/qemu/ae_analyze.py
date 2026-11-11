#!/usr/bin/env python3
"""ae_analyze.py RAW --soc t31 --clip step --fps 15 --stack open --out DIR [--target Y]

Reads the rows of ae_sample.sh (AE,uptime,frames,it,again,dgain,ispdgain,rgain,bgain,ct,ev,luma) and checks the
AE/AWB behaviour against the clip's timeline (gen_scenes.sh). Time axis = ISP frames / fps (the scene advances
by frame, not by wall time). Y = the camera daemon's ae_luma (ISP AE statistics mean, 0..255), exposure = "ISP EV value" of
isp-m0 (linear: integration time x gain). Writes DIR/<soc>-<stack>-<clip>.{txt,json,csv[,png]}; exit 1 on a failed check.

Limits are in LIMITS below; they are loose on purpose (QEMU renders in linear light with a table-driven sensor
model, so absolute numbers are a regression reference for the open stack, not a vendor equivalence proof)."""
import argparse, csv, json, math, os, statistics, sys

LIMITS = {
    "settle_s": 45.0,        # time from stream start / from the step until the signal stays inside the tolerance band
    "tol_pct": 12.0,         # tolerance band around the final Y (percent, at least 3 levels)
    "overshoot_pct": 25.0,   # excursion past the final Y after the first crossing
    "target_pct": 20.0,      # final Y against the reference target
    "steady_pct": 8.0,       # grey clip: Y spread (max-min) over the last 40 % of the run
    "step_ev_ratio": 1.5,    # step clip: exposure must drop at least by this factor
    "track_pct": 30.0,       # ramps: Y deviation from the median after the first 15 s
    "rho": 0.85,             # ramps / tint: rank correlation magnitude
    "rb_ratio": 1.10,        # tint: (R gain / B gain) cool against warm
    "step_t": 30.0, "step_t_win": 8.0,   # nominal step time in the clip, and how far the detected one may be off
}

# scene mean luma (gen_scenes.sh) as a function of clip time
def scene_mean(clip, t):
    if clip == "ramp_up":
        return 60 if t < 10 else (60 + 130 * (t - 10) / 50 if t < 60 else 190)
    if clip == "ramp_down":
        return 190 if t < 10 else (190 - 130 * (t - 10) / 50 if t < 60 else 60)
    if clip == "step":
        return 50 if t < 30 else 200
    return 110


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


def load(path, fps):
    rows = []
    for ln in open(path, errors="replace"):
        f = ln.strip().split(",")
        if len(f) != 13 or f[0] != "AE":
            continue
        v = [num(x) for x in f[1:12]]
        up, fr, it, ag, dg, ig, rg, bg, ct, ev, y = v
        if fr is None or ev is None or not rg or not bg:
            continue
        rows.append(dict(up=up, fr=fr, it=it, ag=ag, dg=dg, ig=ig, rg=rg, bg=bg, ct=ct, ev=ev,
                         y=float("nan") if y is None or y < 0 else y, mode=f[12]))
    if not rows:
        return rows
    for r in rows:
        r["t"] = r["fr"] / fps          # the scene's clock: the clip's frame n is the sensor's frame n since stream-on
        r["rb"] = r["rg"] / r["bg"]
    # one loop of the clip only (it repeats after 90 s)
    return [r for r in rows if r["t"] <= 90.0]


def med(a):
    return statistics.median(a) if a else float("nan")


def win(rows, key, t0, t1):
    return [r[key] for r in rows if t0 <= r["t"] < t1]


def final_of(rows, key="y"):
    n = max(3, int(len(rows) * 0.15))
    return med([r[key] for r in rows[-n:]])


def settle(rows, t0, final, tol, key="y"):
    """seconds from t0 until Y stays inside final +- tol (None: never)"""
    seg = [r for r in rows if r["t"] >= t0]
    last_out = None
    for r in seg:
        if abs(r[key] - final) > tol:
            last_out = r["t"]
    if last_out is None:
        return 0.0
    nxt = [r["t"] for r in seg if r["t"] > last_out]
    return (nxt[0] - t0) if nxt else None


def spearman(a, b):
    def rank(x):
        o = sorted(range(len(x)), key=lambda i: x[i]); r = [0.0] * len(x); i = 0
        while i < len(o):
            j = i
            while j + 1 < len(o) and x[o[j + 1]] == x[o[i]]:
                j += 1
            for k in range(i, j + 1):
                r[o[k]] = (i + j) / 2.0
            i = j + 1
        return r
    if len(a) < 4:
        return float("nan")
    ra, rb = rank(a), rank(b)
    ma, mb = statistics.mean(ra), statistics.mean(rb)
    sa = math.sqrt(sum((x - ma) ** 2 for x in ra)); sb = math.sqrt(sum((x - mb) ** 2 for x in rb))
    return sum((x - ma) * (y - mb) for x, y in zip(ra, rb)) / (sa * sb) if sa and sb else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw"); ap.add_argument("--soc", required=True); ap.add_argument("--clip", required=True)
    ap.add_argument("--fps", type=float, default=15); ap.add_argument("--stack", default="open")
    ap.add_argument("--out", required=True); ap.add_argument("--target", type=float)
    ap.add_argument("--open-loop", action="store_true", help="the QEMU sensor model has no exposure model: the picture does not follow the "
                    "exposure, so the AE loop cannot close; Y is only the scene's level and the checks run on the exposure instead")
    a = ap.parse_args()
    L = LIMITS
    rows = load(a.raw, a.fps)
    tag = "%s-%s-%s" % (a.soc, a.stack, a.clip)
    checks, info = [], {}

    def chk(name, val, ok, limit):
        checks.append(dict(name=name, value=None if val is None else round(val, 3) if isinstance(val, float) else val,
                           limit=limit, ok=bool(ok)))

    if len(rows) < 8:
        chk("rows", len(rows), False, ">= 8 usable rows")
        return finish(a, tag, rows, checks, info)
    info["rows"] = len(rows)
    if rows[0]["t"] > 5:
        info["note"] = "sampling began %.0f s into the clip (stream restart, console and daemon start-up)" % rows[0]["t"]
    info["span_s"] = round(rows[-1]["t"] - rows[0]["t"], 1)
    info["row_interval_s"] = round(info["span_s"] / (len(rows) - 1), 2)
    chk("coverage_end_s", rows[-1]["t"], rows[-1]["t"] >= 80, "samples reach >= 80 s of the clip")

    have_y = not all(math.isnan(r["y"]) for r in rows)
    closed = (not a.open_loop) and have_y
    sig = "y" if closed else "ev"        # the signal whose convergence is judged
    info["loop"] = ("closed (exposure moves the picture)" if not a.open_loop else "open (sensor model has no exposure model)") + \
                   ("" if have_y else "; no AE luma on this SoC, judged on the exposure")
    night = sum(1 for r in rows if r["mode"] == "Night")
    chk("stayed_in_day_mode", night, night == 0, "0 rows in ISP Night mode")
    fin_y = final_of(rows) if have_y else float("nan"); ev_fin = final_of(rows, "ev"); fin = final_of(rows, sig)
    tol = max(L["tol_pct"] / 100 * fin, 3.0 if closed else 0.0)
    info.update(final_Y=round(fin_y, 1), final_EV=round(ev_fin, 1), tol=round(tol, 1), judged_signal=sig)
    # AE at its exposure floor (1 line, no gain) or at the gain/integration ceiling cannot be at a target
    last = rows[-1]
    rail = "floor" if last["ev"] <= 2 else ("ceiling" if last["it"] >= 0.9 * max(r["it"] for r in rows) and last["ag"] >= 160 else None)
    info["ae_rail_at_end"] = rail
    if not a.open_loop:
        chk("ae_not_railed", 0 if rail is None else 1, rail is None, "AE ends between its limits")

    # reference target: --target, else the same SoC/stack grey run
    tgt = a.target
    if tgt is None and a.clip != "grey" and closed:
        try:
            tgt = json.load(open(os.path.join(a.out, "%s-%s-grey.json" % (a.soc, a.stack))))["info"]["final_Y"]
            info["target_from"] = "grey run"
        except (OSError, KeyError, ValueError):
            pass
    if a.clip == "grey":
        tgt = fin_y; info["target_from"] = "self (reference run)"
    info["target_Y"] = None if tgt is None else round(tgt, 1)

    if a.clip == "grey":
        st = settle(rows, 0.0, fin, tol, sig)
        chk("convergence_s", st, st is not None and st <= L["settle_s"], "<= %g s from stream start (%s)" % (L["settle_s"], sig))
        late = [r for r in rows if r["t"] >= rows[-1]["t"] * 0.6]
        if closed:
            sp = (max(r["y"] for r in late) - min(r["y"] for r in late)) / fin * 100
            chk("steady_Y_spread_pct", sp, sp <= L["steady_pct"], "<= %g %%" % L["steady_pct"])
        esp = (max(r["ev"] for r in late) - min(r["ev"] for r in late)) / ev_fin * 100
        chk("steady_EV_spread_pct", esp, esp <= 15, "<= 15 %")
        rbd = (max(r["rb"] for r in late) - min(r["rb"] for r in late)) / med([r["rb"] for r in late]) * 100
        chk("awb_steady_pct", rbd, rbd <= 5, "<= 5 %")
    elif a.clip == "step":
        # the step is the largest luma jump between neighbouring rows (exposure cannot follow within one row)
        if have_y:
            best = max(range(1, len(rows)), key=lambda i: abs(rows[i]["y"] - rows[i - 1]["y"]))
            t0 = rows[best]["t"]; up = rows[best]["y"] > rows[best - 1]["y"]
            info["step_seen_at_s"] = round(t0, 1)
            chk("step_seen_near_30s", t0, abs(t0 - L["step_t"]) <= L["step_t_win"], "30 +- %g s" % L["step_t_win"])
        else:
            t0, up = L["step_t"], True       # no luma: the clip's own clock (t = frames / fps) and its known direction
        pre = med(win(rows, "y", max(0, t0 - 12), t0)); pre_ev = med(win(rows, "ev", max(0, t0 - 12), t0))
        info.update(pre_Y=round(pre, 1), pre_EV=round(pre_ev, 1))
        peak = float("nan")
        if have_y:
            jump = [r["y"] for r in rows if t0 <= r["t"] < t0 + 5]
            peak = max(jump) if up else min(jump)
            info["Y_right_after_step"] = round(peak, 1)
        st = settle(rows, t0, fin, tol, sig)
        chk("settle_after_step_s", st, st is not None and st <= L["settle_s"], "<= %g s (%s)" % (L["settle_s"], sig))
        # overshoot: how far past the final value (towards the other side) after the first crossing; judged on Y while the
        # loop is closed, on the exposure otherwise (the exposure running past where it ends up)
        ref0 = peak if closed else pre_ev
        sgn = 1 if ref0 > fin else -1
        cross = next((r["t"] for r in rows if r["t"] >= t0 and sgn * (r[sig] - fin) <= 0), None)
        ov = 0.0
        if cross is not None:
            ov = max([(-sgn) * (r[sig] - fin) for r in rows if r["t"] >= cross] + [0]) / fin * 100
        chk("overshoot_pct", ov, ov <= L["overshoot_pct"], "<= %g %% (%s)" % (L["overshoot_pct"], sig))
        if closed:
            d = abs(fin - pre) / pre * 100
            chk("final_vs_pre_Y_pct", d, d <= L["target_pct"], "<= %g %% (same AE target before and after)" % L["target_pct"])
        ratio = (pre_ev / ev_fin) if up else (ev_fin / pre_ev)
        chk("exposure_follows_step", ratio, ratio >= L["step_ev_ratio"], ">= %g x" % L["step_ev_ratio"])
    elif a.clip in ("ramp_up", "ramp_down"):
        seg = [r for r in rows if 10 <= r["t"] <= 60]
        rho = spearman([r["ev"] for r in seg], [scene_mean(a.clip, r["t"]) for r in seg])
        chk("exposure_vs_scene_rho", rho, rho <= -L["rho"], "<= -%g (exposure falls as the scene brightens)" % L["rho"])
        if closed:
            late = [r["y"] for r in rows if r["t"] >= 15]
            m = med(late); dev = max(abs(y - m) for y in late) / m * 100
            chk("tracking_dev_pct", dev, dev <= L["track_pct"], "<= %g %% around the median after 15 s" % L["track_pct"])
        e0 = med([r["ev"] for r in ([x for x in rows if x["t"] < 10] or rows[:3])])
        direction = (ev_fin < e0) if a.clip == "ramp_up" else (ev_fin > e0)
        chk("exposure_direction", ev_fin / e0, direction, "EV %s over the ramp" % ("falls" if a.clip == "ramp_up" else "rises"))
    elif a.clip == "tint":
        ew = [r for r in rows if 8 <= r["t"] < 20] or rows[:4]      # the warm hold; if sampling began later, the first rows
        w = med([r["rb"] for r in ew]); c = med(win(rows, "rb", 75, 90))
        info.update(rb_warm=round(w, 3), rb_cool=round(c, 3), ct_warm=med([r["ct"] for r in ew]), ct_cool=med(win(rows, "ct", 75, 90)),
                    warm_window_s=[round(ew[0]["t"], 1), round(ew[-1]["t"], 1)])
        chk("rg_over_bg_cool_vs_warm", c / w, c / w >= L["rb_ratio"], ">= %g (red gain falls, blue rises under warm light)" % L["rb_ratio"])
        seg = [r for r in rows if 20 <= r["t"] <= 70]
        rho = spearman([r["rb"] for r in seg], [r["t"] for r in seg])
        chk("rg_over_bg_vs_time_rho", rho, rho >= L["rho"], ">= %g" % L["rho"])
        chk("ct_warm_below_cool", info["ct_cool"] - info["ct_warm"], info["ct_cool"] > info["ct_warm"], "colour temperature rises")
        if closed:
            late = [r["y"] for r in rows if r["t"] >= 15]
            m = med(late); dev = max(abs(y - m) for y in late) / m * 100
            chk("luma_held_dev_pct", dev, dev <= L["track_pct"], "<= %g %%" % L["track_pct"])
    if tgt is not None and a.clip != "grey" and closed:
        d = abs(fin - tgt) / tgt * 100
        chk("final_Y_vs_target_pct", d, d <= L["target_pct"], "<= %g %% of the grey-run target %g" % (L["target_pct"], round(tgt, 1)))
    finish(a, tag, rows, checks, info)


def finish(a, tag, rows, checks, info):
    ok = all(c["ok"] for c in checks)
    lines = ["%s: %s" % (tag, "PASS" if ok else "FAIL")] + ["  %s" % json.dumps(info)] + \
            ["  %-28s %-10s %s  (limit %s)" % (c["name"], c["value"], "ok " if c["ok"] else "BAD", c["limit"]) for c in checks]
    txt = "\n".join(lines)
    print(txt)
    os.makedirs(a.out, exist_ok=True)
    base = os.path.join(a.out, tag)
    open(base + ".txt", "w").write(txt + "\n")
    json.dump(dict(tag=tag, ok=ok, info=info, checks=checks), open(base + ".json", "w"), indent=1)
    if rows:
        with open(base + ".csv", "w", newline="") as f:
            w = csv.writer(f); w.writerow(["t_s", "frames", "uptime", "it_lines", "again", "dgain", "ispdgain", "rgain", "bgain", "ct", "ev", "Y"])
            for r in rows:
                w.writerow([round(r["t"], 2), int(r["fr"]), r["up"], r["it"], r["ag"], r["dg"], r["ig"], r["rg"], r["bg"], r["ct"], r["ev"], r["y"]])
        try:
            import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
            fig, ax = plt.subplots(3, 1, figsize=(8, 7), sharex=True)
            t = [r["t"] for r in rows]
            ax[0].plot(t, [r["y"] for r in rows], "o-", ms=3); ax[0].set_ylabel("ae_luma")
            ax[1].semilogy(t, [r["ev"] for r in rows], "o-", ms=3); ax[1].set_ylabel("EV")
            ax[2].plot(t, [r["rg"] for r in rows], label="rgain"); ax[2].plot(t, [r["bg"] for r in rows], label="bgain")
            ax[2].legend(); ax[2].set_xlabel("clip time (s)"); fig.suptitle(tag); fig.savefig(base + ".png", dpi=80); plt.close(fig)
        except Exception:
            pass
    sys.exit(0 if ok else 1)


main()
