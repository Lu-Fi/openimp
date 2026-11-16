#!/usr/bin/env python3
"""tools/ccm_fit.py (#102, docs/COLORCHECKER.md): fit a CCM correction on SpyderCheckr 24 snapshots (T10/T20 Apical CCM tables).

usage: SPYDER_REF=ref.csv ccm_fit.py --table D40|D50|A --eff "9 proc values" --sat N [--kl 2] [--wb fold|keep] snaps/*.jpg
  --eff : the 9 'ISP CCM ..' values of /proc/jz/isp/isp-m0 at capture time (proc order R_R R_G R_B G_R G_B G_G B_R B_G B_B)
  --sat : 'Saturation Target' at capture time
Prints: measured dE00 (tool method), predicted dE00 after, the new table (floats + Q8) and a file line for OPENIMP_CCM_FILE.

Model: JPEG sRGB -> linear (inverse sRGB) ~ f(S_sat * T * wb * raw); a 3x3 C in that linear space
(target = reference Lab D50 -> linear sRGB) => new effective = C * S * T  => new table T' = S^-1 * C * S * T.
Objective: mean CIEDE2000 (kL, default 2: lightness half weight, the box light falls off ~20 % top to bottom and
the CCM cannot fix position-dependent lightness) over unclipped patches after the tool's A4 exposure
normalisation, + a small pull towards the identity.  --wb keep: rows of C keep the grey axis
(C @ grey_meas proportional to grey_meas), i.e. no white-balance change inside the CCM.
"""
import argparse, glob, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import colorchecker as CC

# ---- Apical (T10/T20) CCM helpers
G = np.array([[76,150,29]]*3, float)
def sm2f(v):  # sign-magnitude Q8 -> float
    v = np.asarray(v, int); return np.where(v & 0x8000, -(v & 0x7fff), v) / 256.0
def f2sm(m):
    q = np.rint(np.asarray(m, float) * 256).astype(int)
    return [int(x) if x >= 0 else int(0x8000 | (-x)) for x in q.ravel()]
def S(target):
    sc = target * 2; return (np.eye(3) * 256 * sc + G * (256 - sc)) / 65536.0
VENDOR = {  # jxh42-t10 bin day tables (A=api 40, D40=41, D50=42)
 'A':   sm2f([360,32851,32789,32824,335,32791,32806,33051,577]).reshape(3,3),
 'D50': sm2f([269,32768,32781,32826,422,32877,32788,33024,532]).reshape(3,3),
 'D40': sm2f([282,32768,32794,32856,410,32834,32786,33037,543]).reshape(3,3),
}



def de2000k(lab1, lab2, kL=1.0):
    l1, a1, b1 = [np.asarray(lab1, float)[..., i] for i in range(3)]
    l2, a2, b2 = [np.asarray(lab2, float)[..., i] for i in range(3)]
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cm = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cm ** 7 / (Cm ** 7 + 25.0 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = l2 - l1, C2p - C1p
    prod0 = (C1p * C2p) == 0
    dh = h2p - h1p
    dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh))
    dh = np.where(prod0, 0.0, dh)
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lm, Cmp = (l1 + l2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hm = np.where(np.abs(h1p - h2p) <= 180, hs / 2, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2))
    hm = np.where(prod0, hs, hm)
    T = (1 - 0.17 * np.cos(np.radians(hm - 30)) + 0.24 * np.cos(np.radians(2 * hm))
         + 0.32 * np.cos(np.radians(3 * hm + 6)) - 0.20 * np.cos(np.radians(4 * hm - 63)))
    dth = 30 * np.exp(-((hm - 275) / 25) ** 2)
    RC = 2 * np.sqrt(Cmp ** 7 / (Cmp ** 7 + 25.0 ** 7))
    SL = kL * (1 + 0.015 * (Lm - 50) ** 2 / np.sqrt(20 + (Lm - 50) ** 2))
    SC, SH = 1 + 0.045 * Cmp, 1 + 0.015 * Cmp * T
    RT = -np.sin(np.radians(2 * dth)) * RC
    x, y, z = dLp / SL, dCp / SC, dHp / SH
    return np.sqrt(x * x + y * y + z * z + RT * y * z)


def nelder_mead(f, x0, step=0.05, iters=4000, tol=1e-7):
    n = len(x0); pts = [np.array(x0, float)]
    for i in range(n):
        p = np.array(x0, float); p[i] += step; pts.append(p)
    vals = [f(p) for p in pts]
    for _ in range(iters):
        o = np.argsort(vals); pts = [pts[i] for i in o]; vals = [vals[i] for i in o]
        if abs(vals[-1] - vals[0]) < tol:
            break
        c = np.mean(pts[:-1], 0)
        xr = c + (c - pts[-1]); fr = f(xr)
        if fr < vals[0]:
            xe = c + 2 * (c - pts[-1]); fe = f(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = c + 0.5 * (pts[-1] - c); fc = f(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                pts = [pts[0] + 0.5 * (p - pts[0]) for p in pts]; vals = [f(p) for p in pts]
    i = int(np.argmin(vals)); return pts[i], vals[i]


def proc_to_mat(v):
    v = list(v); v[4], v[5] = v[5], v[4]       # proc prints G_B before G_G
    return sm2f(v).reshape(3, 3)


def measure(files):
    frames = [CC.load_float(p) for p in files]
    m = CC.measure(frames)
    if m is None:
        sys.exit("chart not detected")
    return m


def stats(lin, clip):
    """the tool's numbers for a linear patch set (analyse() on a fake measurement)"""
    rgb = CC.lin_to_srgb(np.clip(lin, 0, None))
    r = CC.analyse({"rgb": rgb, "clip": clip})
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", required=True, choices=list(VENDOR))
    ap.add_argument("--eff", required=True)
    ap.add_argument("--sat", type=float, required=True)
    ap.add_argument("--kl", type=float, default=2.0)
    ap.add_argument("--wb", choices=["fold", "keep"], default="fold")
    ap.add_argument("--reg", type=float, default=0.5)
    ap.add_argument("--base", default=None, help="9 floats: table the snapshots were taken with (default: vendor)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    files = sorted(sum([glob.glob(f) for f in a.files], []))
    m = measure(files)
    lin = CC.srgb_to_lin(m["rgb"]); clip = m["clip"]
    R = CC.ref_array()
    ok = clip <= 0.02
    ok[0] = False                     # A1 white: shoulder of the tone curve, never a matrix target
    before = stats(lin, clip)
    grey = lin[1:5].sum(0)

    def apply(p):
        C = p.reshape(3, 3)
        if a.wb == "keep":            # keep the grey axis: scale rows so that C @ grey = grey
            C = C * (grey / np.maximum(C @ grey, 1e-9))[:, None]
        return C

    def cost(p):
        C = apply(p)
        out = np.clip(lin @ C.T, 1e-6, None)
        outE, _ = CC.expo_norm(out)
        lab = CC.lin_to_lab(outE)
        d = de2000k(R[ok], lab[ok], a.kl)
        return float(d.mean()) + a.reg * float(((C - np.eye(3)) ** 2).sum()) * 0.1

    # initial guess: least squares in linear light after exposure normalisation
    linE, _ = CC.expo_norm(lin)
    tgt = CC.lab_to_lin(R)
    X, Y = linE[ok], tgt[ok]
    C0 = np.linalg.lstsq(X, Y, rcond=None)[0].T
    best = None
    for start in (np.eye(3), C0):
        p, v = nelder_mead(cost, start.ravel(), 0.05)
        p, v = nelder_mead(cost, p, 0.01)
        if best is None or v < best[1]:
            best = (p, v)
    C = apply(best[0])
    yw = CC.M_RGB2XYZ[1]
    C = C * (float(grey @ yw) / float((C @ grey) @ yw))   # keep the grey luminance: AE (and its gain) stays put
    after = stats(lin @ C.T, clip)
    Sm = S(a.sat)
    T = np.array([float(x) for x in a.base.split()]).reshape(3, 3) if a.base else VENDOR[a.table]
    E = proc_to_mat([int(x) for x in a.eff.replace(",", " ").split()])
    pred = Sm @ T
    Tn = np.linalg.inv(Sm) @ C @ E          # new table from the measured effective matrix
    np.set_printoptions(precision=3, suppress=True)
    print(f"files {len(files)}  frames {m['frames']}  table {a.table}  sat {a.sat}  kL {a.kl}  wb {a.wb}")
    print(f"check S*T vs proc (Q8): max diff {np.abs(pred - E).max() * 256:.1f}")
    for tag, r in (("before", before), ("after (predicted)", after)):
        print(f"{tag:18s} dE00 mean {r['all']['mean']:.2f} median {r['all']['median']:.2f} max {r['all']['max']:.1f}"
              f" unclipped {r['unclipped']['mean']:.2f} gn {r['gn']['mean']:.2f} C* ratio {r['chroma_ratio']:.2f}"
              f" hue {r['hue_err']:.1f} skin {r['skin']['mean']:.2f} grey a/b {r['lab50'][1:5,1].mean():+.2f}/{r['lab50'][1:5,2].mean():+.2f}"
              f" kL2 {de2000k(R[ok], r['lab50'][ok], 2).mean():.2f}")
    print("C =", C.round(3).tolist(), " row sums", C.sum(1).round(3))
    print("old table", T.round(3).ravel().tolist())
    print("new table", Tn.round(4).ravel().tolist())
    print("new Q8   ", f2sm(Tn))
    print(f"LINE {a.table} " + " ".join(f"{x:.4f}" for x in Tn.ravel()))


if __name__ == "__main__":
    main()
