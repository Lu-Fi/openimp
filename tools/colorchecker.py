#!/usr/bin/env python3
"""colorchecker.py (#102, #230): colour accuracy of the SpyderCheckr 24 in the light box, open vs vendor stack, from the snapshots the
fair passes already save (numpy + PIL only).

usage (every call needs --ref FILE or $SPYDER_REF, see docs/COLORCHECKER.md):
  colorchecker.py RUN_DIR [--phases p1_day,p2_a,...] [--out COLOUR.md] [--nv12 W H]
      RUN_DIR has open/<phase>/snaps/*.jpg and vendor/<phase>/snaps/*.jpg (fair-pass layout).  Default phases: every phase
      present in either stack (union; a one-stack run is reported alone) whose name does not contain 'night' (night = IR/mono, no colour).  Writes RUN_DIR/COLOUR.md,
      RUN_DIR/colour/chart_de.json (--nv12: COLOUR_NV12.md, chart_de_nv12.json, overlay_*_nv12.png) and one debug overlay PNG per stack and phase (RUN_DIR/colour/overlay_<stack>_<phase>.png,
      LOCAL ONLY, never put camera pictures online).
      --nv12 W H: use <stack>/<phase>/nv12/*.nv12 instead of the JPEGs (full NV12 frames of W x H, BT.601 full range like JPEG;
      the fair_preload crops are the centre half and usually do not hold the whole chart).
  colorchecker.py --files A.jpg [B.jpg ...] [--vs C.jpg ...]   quick check of loose files (one stack, optionally vs a second one)

Method
  Detection: (flat-region blobs -> 6x4 lattice -> homography -> 8 orientations scored by grey-normalised
  dE00), run on EVERY frame separately (robust to small shifts between frames / stacks); a frame whose detection fails reuses the
  previous lattice.  Sampling: mean of the central 50 % of each patch (square of 0.5 x patch side around the patch centre, patch
  side = PATCH_FRAC x lattice pitch, default 0.80 = SpyderCheckr 24 patch/pitch measured on the T40 images; check the overlay).
  Pixels >= 250 in any channel in the window are counted; a patch with > 2 % such pixels is flagged clipped ('*') and left out of
  the "unclipped" means.
  Colour: sRGB (IEC 61966-2-1) -> linear -> XYZ (D65) -> Lab(D65 white); and XYZ(D65) -> Bradford -> XYZ(D50) -> Lab(D50).
  The reference is D50, so every dE00 vs the reference uses Lab(D50 Bradford).  Exposure: the image is scaled in LINEAR light by
  one factor so that Y of the mid grey A4 equals the reference (no white balance, no per-channel gain): the AE target difference of
  the two stacks drops out, the colour cast stays in.  "gn" = in addition a per-channel gain fitted on the greys A2..A5
  (camera white balance removed): what is left is the colour matrix / saturation / tone.
  Open vs vendor: dE00 between the two stacks' exposure-normalised Lab(D50), per patch.  Under a non-D50 lamp (3000 K) this is
  the robust number: both stacks see the same light, the reference does not.

Reference (cite): Datacolor SpyderCheckr 24 default patch measurements, block "Original" (L*a*b*, D50/2 deg), from the file
  "SpyderCheckr Default Measurements.txt" shipped with Datacolor SpyderCheckr 1.6 software.  The values are licensed: NOT in this repo;
  pass them as CSV (id,L_orig,a_orig,b_orig; ids A1..D6) with --ref or $SPYDER_REF.  Not batch specific.
  Layout (user, landscape): top row
  A = greys, dark..white left to right (A1 white .. A6 black); rows B, C, D below.  Skin patches: D5 (light skin) and D6 (dark skin)
  = the two reference patches in the skin locus (hue 40-55 deg, C* 15-35).
"""
import csv, glob, json, math, os, sys
import numpy as np

ROWS = "ABCD"
IDS = [f"{r}{c}" for r in ROWS for c in range(1, 7)]     # file order A1..A6 B1..D6
ASSUMPTION = "reference = Datacolor default measurements (Original), D50 assumed, not batch specific"

# ------------------------------------------------------------------ reference
def load_ref(path=None):
    path = path or os.environ.get("SPYDER_REF")
    if not path:
        sys.exit("colorchecker: reference CSV missing: pass --ref FILE or set $SPYDER_REF (the Datacolor values are licensed and not part of this repo, see docs/COLORCHECKER.md)")
    out = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            if "L_orig" in r:
                out[r["id"]] = (float(r["L_orig"]), float(r["a_orig"]), float(r["b_orig"]))
            else:
                out[r["id"]] = (float(r["L_d50"]), float(r["a_d50"]), float(r["b_d50"]))
    return out

_REF = None
def ref():
    global _REF
    if _REF is None:
        _REF = load_ref()
    return _REF

def ref_array():
    return np.array([ref()[i] for i in IDS])

# ------------------------------------------------------------------ colour maths
M_RGB2XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                      [0.2126729, 0.7151522, 0.0721750],
                      [0.0193339, 0.1191920, 0.9503041]])
W_D65 = np.array([0.95047, 1.0, 1.08883])
W_D50 = np.array([0.96422, 1.0, 0.82521])
BRAD = np.array([[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]])
M_ADAPT = np.linalg.inv(BRAD) @ np.diag((BRAD @ W_D50) / (BRAD @ W_D65)) @ BRAD   # D65 -> D50 (Bradford)
M_RGB2XYZ50 = M_ADAPT @ M_RGB2XYZ

def srgb_to_lin(c):
    c = np.asarray(c, float) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def lin_to_srgb(l):
    l = np.clip(np.asarray(l, float), 0, 1)
    return 255.0 * np.where(l <= 0.0031308, 12.92 * l, 1.055 * l ** (1 / 2.4) - 0.055)

def xyz_to_lab(xyz, white=W_D50):
    t = np.asarray(xyz, float) / white
    f = np.where(t > 216 / 24389, np.cbrt(t), (24389 / 27 * t + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], axis=-1)

def lab_to_xyz(lab, white=W_D50):
    lab = np.asarray(lab, float)
    fy = (lab[..., 0] + 16) / 116
    fx, fz = fy + lab[..., 1] / 500, fy - lab[..., 2] / 200
    f = np.stack([fx, fy, fz], axis=-1)
    t = np.where(f ** 3 > 216 / 24389, f ** 3, (116 * f - 16) * 27 / 24389)
    return t * white

def lin_to_lab(lin):
    """linear sRGB (D65 primaries) -> Lab D50 (Bradford adapted)"""
    return xyz_to_lab(np.asarray(lin, float) @ M_RGB2XYZ50.T)

def lab_to_lin(lab):
    return lab_to_xyz(lab) @ np.linalg.inv(M_RGB2XYZ50).T

def srgb_to_lab(rgb255):
    return lin_to_lab(srgb_to_lin(rgb255))

def lab_to_srgb(lab):
    return lin_to_srgb(lab_to_lin(lab))

def de2000(lab1, lab2):
    """CIEDE2000 (Sharma 2005), numpy broadcasting over the last axis (L,a,b)"""
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
    SL = 1 + 0.015 * (Lm - 50) ** 2 / np.sqrt(20 + (Lm - 50) ** 2)
    SC, SH = 1 + 0.045 * Cmp, 1 + 0.015 * Cmp * T
    RT = -np.sin(np.radians(2 * dth)) * RC
    x, y, z = dLp / SL, dCp / SC, dHp / SH
    return np.sqrt(x * x + y * y + z * z + RT * y * z)

# ------------------------------------------------------------------ geometry / detection
def _runs(mask):
    """connected components (8-neighbour) of a boolean image via run-length union-find.
    returns list of dict(area, cx, cy, ecc) """
    h, w = mask.shape
    parent, runs, prev = [], [], []
    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for y in range(h):
        row = mask[y].astype(np.int8)
        d = np.diff(np.concatenate(([0], row, [0])))
        st, en = np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]
        cur = []
        for a, b in zip(st, en):
            idx = len(runs)
            runs.append((y, int(a), int(b)))
            parent.append(idx)
            for (pa, pb, pi) in prev:
                if pa < b + 1 and a < pb + 1:           # touching incl. diagonal
                    ra, rb = find(idx), find(pi)
                    if ra != rb:
                        parent[ra] = rb
            cur.append((int(a), int(b), idx))
        prev = cur
    comps = {}
    for i, (y, a, b) in enumerate(runs):
        n = b - a
        sx = (a + b - 1) * n / 2.0
        sxx = sum_sq(a, b)
        c = comps.setdefault(find(i), [0, 0.0, 0.0, 0.0, 0.0, 0.0])
        c[0] += n; c[1] += sx; c[2] += y * n; c[3] += sxx; c[4] += y * sx; c[5] += y * y * n
    out = []
    for n, sx, sy, sxx, sxy, syy in comps.values():
        cx, cy = sx / n, sy / n
        vxx, vyy, vxy = sxx / n - cx * cx, syy / n - cy * cy, sxy / n - cx * cy
        tr, det = vxx + vyy, vxx * vyy - vxy * vxy
        disc = math.sqrt(max(tr * tr / 4 - det, 0.0))
        l1, l2 = tr / 2 + disc, max(tr / 2 - disc, 1e-9)
        out.append({"area": n, "cx": cx, "cy": cy, "ecc": math.sqrt(l1 / l2)})
    return out

def sum_sq(a, b):
    # sum of x^2 for x in [a, b)
    f = lambda n: (n - 1) * n * (2 * n - 1) / 6.0
    return f(b) - f(a)

def _downscale(a, maxside=640):
    f = max(1, int(math.ceil(max(a.shape[:2]) / maxside)))
    h, w = (a.shape[0] // f) * f, (a.shape[1] // f) * f
    return a[:h, :w].reshape(h // f, f, w // f, f, -1).mean(axis=(1, 3)), f

def fit_h(src, dst):
    """homography src(i,j) -> dst(x,y), DLT with normalisation"""
    src, dst = np.asarray(src, float), np.asarray(dst, float)
    m, s = dst.mean(axis=0), max(dst.std(), 1e-9)
    d = (dst - m) / s
    A = []
    for (x, y), (u, v) in zip(src, d):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y, -u])
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y, -v])
    H = np.linalg.svd(np.array(A))[2][-1].reshape(3, 3)
    T = np.array([[s, 0, m[0]], [0, s, m[1]], [0, 0, 1]])
    H = T @ H
    return H / H[2, 2]

def apply_h(H, pts):
    p = np.c_[np.asarray(pts, float), np.ones(len(pts))] @ H.T
    return p[:, :2] / p[:, 2:3]

def _lattice_from_blobs(pts):
    """pts (m,2) centres of equal sized blobs -> dict(H, dims, n) or None"""
    m = len(pts)
    if m < 16:
        return None
    D = np.hypot(*(pts[:, None, :] - pts[None, :, :]).transpose(2, 0, 1))
    np.fill_diagonal(D, 1e9)
    p = float(np.median(D.min(axis=1)))
    pair = np.argwhere((D < 1.4 * p) & (D > 0.5 * p))
    vec = pts[pair[:, 1]] - pts[pair[:, 0]]
    th = np.arctan2(vec[:, 1], vec[:, 0])
    th0 = np.angle(np.mean(np.exp(4j * th))) / 4
    u, v = np.array([math.cos(th0), math.sin(th0)]), np.array([-math.sin(th0), math.cos(th0)])
    pu = np.abs(vec @ u); pv = np.abs(vec @ v)
    ln = np.hypot(vec[:, 0], vec[:, 1])
    pu = float(np.median(pu[pu > 0.7 * ln])) if np.any(pu > 0.7 * ln) else p
    pv = float(np.median(pv[pv > 0.7 * ln])) if np.any(pv > 0.7 * ln) else p
    d = pts - pts[0]
    ij = np.stack([np.rint(d @ u / pu), np.rint(d @ v / pv)], axis=1)
    H = None
    for _ in range(3):
        ij = ij - ij.min(axis=0)
        if len({tuple(x) for x in ij}) != m:
            return None
        if H is not None:                         # re-assign with the perspective model
            Hi = np.linalg.inv(H)
            q = apply_h(Hi, pts)
            ij = np.rint(q)
            ij = ij - ij.min(axis=0)
            if len({tuple(x) for x in ij}) != m:
                return None
        H = fit_h(ij, pts)
    n1, n2 = int(ij[:, 0].max()) + 1, int(ij[:, 1].max()) + 1
    if (n1, n2) not in ((6, 4), (4, 6)):
        return None
    res = np.hypot(*(apply_h(H, ij) - pts).T)
    if np.max(res) > 0.35 * p:
        return None
    return {"H": H, "dims": (n1, n2), "n": m, "fit_px": float(res.mean())}

def detect(img, quad=None):
    """img float array (h,w,3) 0..255 -> lattice dict with H in full-resolution pixel coordinates"""
    if quad is not None:
        q = np.asarray(quad, float).reshape(4, 2)        # centres of the 4 corner patches, cyclic order
        a, b = np.linalg.norm(q[1] - q[0]), np.linalg.norm(q[2] - q[1])
        dims = (6, 4) if a >= b else (4, 6)
        H = fit_h([(0, 0), (dims[0] - 1, 0), (dims[0] - 1, dims[1] - 1), (0, dims[1] - 1)], q)
        return {"H": H, "dims": dims, "n": 24, "fit_px": 0.0, "thr": None}
    small, f = _downscale(img)
    pad = np.pad(small, ((1, 1), (1, 1), (0, 0)), mode="edge")
    blur = sum(pad[dy:dy + small.shape[0], dx:dx + small.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
    gx = np.abs(np.gradient(blur, axis=1)).max(axis=2)
    gy = np.abs(np.gradient(blur, axis=0)).max(axis=2)
    g = gx + gy
    N = g.size
    for thr in (2.0, 3.5, 5.0, 8.0, 12.0, 18.0):
        flat = g < thr
        er = flat.copy()
        er[1:] &= flat[:-1]; er[:-1] &= flat[1:]; er[:, 1:] &= flat[:, :-1]; er[:, :-1] &= flat[:, 1:]
        comps = [c for c in _runs(er) if 30 <= c["area"] <= 0.02 * N and c["ecc"] < 2.2]
        if len(comps) < 16:
            continue
        areas = np.array([c["area"] for c in comps])
        best = max(range(len(comps)), key=lambda i: np.sum((areas > 0.3 * areas[i]) & (areas < 3.3 * areas[i])))
        sel = [c for c, a in zip(comps, areas) if 0.3 * areas[best] < a < 3.3 * areas[best]]
        pts = np.array([[c["cx"], c["cy"]] for c in sel])
        lat = _lattice_from_blobs(pts)
        if lat:
            S_ = np.diag([f, f, 1.0])
            off = np.array([[1, 0, (f - 1) / 2.0], [0, 1, (f - 1) / 2.0], [0, 0, 1]])
            # block-average centre of small pixel k is at full-res k*f + (f-1)/2
            lat["H"] = off @ S_ @ lat["H"]
            lat["thr"] = thr
            lat["fit_px"] *= f
            return lat
    return None

# ------------------------------------------------------------------ sampling / orientation
def layout_id(r, c, layout=None):
    layout = layout or os.environ.get("SPYDER_LAYOUT", "dark-left")
    return f"{ROWS[r]}{(6 - c) if layout == 'dark-left' else (c + 1)}"

def candidates(dims):
    """list of (name, M, off): (c,r) = M @ (i,j) + off for the 4 maps that fit the lattice shape"""
    n1, n2 = dims
    out = []
    if (n1, n2) == (6, 4):
        for fc in (0, 1):
            for fr in (0, 1):
                M = np.array([[-1 if fc else 1, 0], [0, -1 if fr else 1]])
                out.append((M, np.array([5 if fc else 0, 3 if fr else 0])))
    else:
        for fc in (0, 1):
            for fr in (0, 1):
                M = np.array([[0, -1 if fc else 1], [-1 if fr else 1, 0]])
                out.append((M, np.array([5 if fc else 0, 3 if fr else 0])))
    return out

def sample_cells(img, H, dims, frac=0.25):
    n1, n2 = dims
    ij = np.array([(i, j) for j in range(n2) for i in range(n1)], float)
    ctr = apply_h(H, ij)
    p = min(np.linalg.norm(apply_h(H, [(1, 0)])[0] - apply_h(H, [(0, 0)])[0]),
            np.linalg.norm(apply_h(H, [(0, 1)])[0] - apply_h(H, [(0, 0)])[0]))
    hw = max(2, int(round(frac * p)))
    out = {}
    for (i, j), (x, y) in zip(ij.astype(int), ctr):
        x, y = int(round(x)), int(round(y))
        win = img[max(y - hw, 0):y + hw + 1, max(x - hw, 0):x + hw + 1].reshape(-1, 3)
        out[(i, j)] = np.median(win, axis=0)
    return out, ctr, p

def _canon(cells, M, off, layout):
    """cells {(i,j): rgb} -> (24,3) in IDS order for map (M, off)"""
    arr = np.zeros((24, 3))
    for (i, j), rgb in cells.items():
        c, r = M @ np.array([i, j]) + off
        arr[IDS.index(layout_id(int(r), int(c), layout))] = rgb
    return arr

# ------------------------------------------------------------------ analysis
GREY_MID = [1, 2, 3, 4]        # A2..A5 (not the clipping-prone white, not the black)
EXPO = [2, 3]                  # A3,A4 exposure reference (as in the older chart.py)

def _yref():
    return ((ref_array()[:, 0] + 16) / 116) ** 3

def correct(rgb, mode="raw", card=None):
    """rgb (24,3) sRGB 0..255 -> Lab (24,3) after exposure scaling (raw), per-channel grey fit (gn) or card WB (card)"""
    lin = srgb_to_lin(rgb)
    ref_lin = lab_to_lin(ref_array())
    if mode == "gn":
        gain = ref_lin[GREY_MID].sum(axis=0) / np.maximum(lin[GREY_MID].sum(axis=0), 1e-9)
        lin = lin * gain
        meta = {"gain_r": gain[0], "gain_g": gain[1], "gain_b": gain[2]}
    else:
        if mode == "card" and card is not None:
            card = np.asarray(card, float)
            g = card[1] / card                                    # G fixed
            lin = lin * g
        Y = lin @ M_RGB2XYZ[1]
        k = _yref()[EXPO].sum() / max(Y[EXPO].sum(), 1e-9)
        lin = lin * k
        meta = {"exposure_gain": k}
    return lin_to_lab(lin), meta

def summary(lab, meta=None):
    R = ref_array()
    de = de2000(R, lab)
    col = np.array([not i.startswith("A") for i in IDS])
    gr = ~col
    cm, cr = np.hypot(lab[:, 1], lab[:, 2]), np.hypot(R[:, 1], R[:, 2])
    hm = np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) % 360
    hr = np.degrees(np.arctan2(R[:, 2], R[:, 1])) % 360
    dh = (hm - hr + 180) % 360 - 180
    mid = np.zeros(24, bool); mid[GREY_MID] = True
    d = {"dE00_mean": de.mean(), "dE00_colour": de[col].mean(), "dE00_max": de.max(), "dE00_grey": de[gr].mean(),
         "chroma_ratio": cm[col].sum() / cr[col].sum(), "chroma_err_mean": np.mean(np.abs(cm[col] - cr[col])),
         "hue_err_mean": np.mean(np.abs(dh[col])), "dL_mean": np.mean(lab[:, 0] - R[:, 0]),
         "grey_a": lab[mid, 1].mean(), "grey_b": lab[mid, 2].mean(),
         "grey_da": (lab[mid, 1] - R[mid, 1]).mean(), "grey_db": (lab[mid, 2] - R[mid, 2]).mean(),
         "grey_chroma": np.hypot(lab[mid, 1], lab[mid, 2]).mean()}
    for i, n in enumerate(IDS):
        d["dE00_" + n] = de[i]
    d["worst_patch"] = IDS[int(np.argmax(de))]
    d.update(meta or {})
    return d

def tone_curve(rgb):
    """front greys A1..A6: code values, L* error after exposure normalisation, log-log slope (1 = ref)"""
    R = ref_array()[:6]
    lin = srgb_to_lin(rgb[:6])
    Y = lin @ M_RGB2XYZ[1]
    yref = _yref()[:6]
    k = yref[2:4].sum() / Y[2:4].sum()
    Ln = 116 * np.cbrt(np.clip(Y * k, 0, None)) - 16
    code = rgb[:6] @ np.array([0.2126, 0.7152, 0.0722])
    ok = Y > 0.003
    slope = float(np.polyfit(np.log(yref[ok]), np.log(Y[ok]), 1)[0]) if ok.sum() >= 3 else float("nan")
    d = {"slope_loglog": slope, "dL_max": float(np.max(np.abs(Ln - R[:, 0]))), "dL_mean": float(np.mean(Ln - R[:, 0])),
         "white_code": code[0], "black_code": code[5], "range_ratio": float(Y[0] / max(Y[5], 1e-6)),
         "clipped": int(np.sum(code >= 250)) + int(np.sum(code <= 2))}
    for i in range(6):
        d[f"code_A{i+1}"] = code[i]
        d[f"dL_A{i+1}"] = Ln[i] - R[i, 0]
    return d

def sp_analyse(imgs, card=None, quad=None, layout=None):
    """imgs: list of float (h,w,3) arrays (same chart position). returns result dict"""
    lat = detect(imgs[0], quad)
    if not lat:
        return {"ok": False, "error": "chart not found"}
    H, dims = lat["H"], lat["dims"]
    acc, ctr = None, None
    for im in imgs:
        cells, ctr, pitch = sample_cells(im, H, dims)
        acc = {k: v.copy() for k, v in cells.items()} if acc is None else {k: acc[k] + v for k, v in cells.items()}
    cells = {k: v / len(imgs) for k, v in acc.items()}
    c0 = np.array([(dims[0] - 1) / 2.0, (dims[1] - 1) / 2.0])      # Jacobian at the lattice centre
    ex = apply_h(H, [c0 + (0.5, 0)])[0] - apply_h(H, [c0 - (0.5, 0)])[0]
    ey = apply_h(H, [c0 + (0, 0.5)])[0] - apply_h(H, [c0 - (0, 0.5)])[0]
    A = np.stack([ex, ey], axis=1)
    R = ref_array()
    scored = []
    for M, off in candidates(dims):
        rgb = _canon(cells, M, off, layout)
        lab, _ = correct(rgb, "gn")
        de = float(de2000(R, lab).mean())
        Cimg = A @ np.linalg.inv(M)
        scored.append((de, M, off, rgb, bool(np.linalg.det(Cimg) < 0), math.degrees(math.atan2(Cimg[1, 0], Cimg[0, 0]))))
    scored.sort(key=lambda s: s[0])
    best = scored[0]
    out = {"ok": True, "dims": dims, "blobs": lat["n"], "thr": lat.get("thr"), "lattice_fit_px": lat["fit_px"],
           "mirrored": best[4], "rotation_deg": best[5], "orient_resid_dE00": best[0],
           "orient_runnerup_dE00": scored[1][0], "orient_margin": scored[1][0] - best[0], "rgb": best[3],
           "assumption": ASSUMPTION, "H": H, "pitch": float(pitch)}
    # patch centres in image pixels, IDS order (A1..D6), for ROIs placed relative to the chart (noise/flicker auto ROI)
    n1, n2 = dims
    ij = [(i, j) for j in range(n2) for i in range(n1)]
    out["centres"] = _canon({k: np.array([x, y, 0.0]) for k, (x, y) in zip(ij, ctr)}, best[1], best[2], layout)[:, :2]
    for name, mode in (("raw", "raw"), ("gn", "gn")) + ((("card", "card"),) if card is not None else ()):
        lab, meta = correct(best[3], mode, card)
        out[name] = summary(lab, meta)
        out[name]["lab"] = lab
    out["tone"] = tone_curve(best[3])
    return out

def load_float(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB"), float)

def analyse_files(paths, **kw):
    return sp_analyse([load_float(p) for p in paths], **kw)

def report(res, tag=""):
    if not res.get("ok"):
        return f"== {tag}  {res.get('error')}"
    o = [f"== {tag}  SpyderCheckr 24 {res['dims'][0]}x{res['dims'][1]} lattice, rotation {res['rotation_deg']:+.1f} deg"
         f"{' MIRRORED' if res['mirrored'] else ''}, orientation residual dE00 {res['orient_resid_dE00']:.2f} "
         f"(runner-up {res['orient_runnerup_dE00']:.2f})",
         f"   {res['assumption']}"]
    for n in ("raw", "gn", "card"):
        if n in res:
            s = res[n]
            o.append(f"   {n:4s} dE00 mean {s['dE00_mean']:.2f} colour {s['dE00_colour']:.2f} max {s['dE00_max']:.1f} ({s['worst_patch']})"
                     f"  chroma x{s['chroma_ratio']:.2f} dC {s['chroma_err_mean']:.1f} hue {s['hue_err_mean']:.1f} deg"
                     f"  grey a*/b* {s['grey_a']:+.1f}/{s['grey_b']:+.1f}")
    t = res["tone"]
    o.append(f"   tone: slope {t['slope_loglog']:.2f} dL max {t['dL_max']:.1f} white {t['white_code']:.0f} black {t['black_code']:.0f}")
    return "\n".join(o)


# ================================================================== run-directory report
GREYS = [f"A{i}" for i in range(1, 7)]
SKIN = ["D5", "D6"]
COLOURS = [i for i in IDS if not i.startswith("A")]
PATCH_FRAC = float(os.environ.get("PATCH_FRAC", 0.80))
CLIP = 250
NAMES = {"A1": "white", "A2": "grey 80", "A3": "grey 65", "A4": "grey 50", "A5": "grey 35", "A6": "black",
         "B1": "aqua", "B2": "lavender", "B3": "yellow", "B4": "red", "B5": "green", "B6": "blue",
         "C1": "orange", "C2": "blue-violet", "C3": "pink-red", "C4": "purple", "C5": "yellow-green", "C6": "orange-yellow",
         "D1": "cyan-green", "D2": "lilac", "D3": "foliage", "D4": "sky blue", "D5": "light skin", "D6": "dark skin"}


# ------------------------------------------------------------------ colour maths 
def lin_to_lab65(lin):
    return xyz_to_lab(np.asarray(lin, float) @ M_RGB2XYZ.T, W_D65)


def lin_to_lab50(lin):
    return lin_to_lab(lin)                    # Bradford D65 -> D50


def expo_norm(lin):
    """one linear factor so that Y(A4) = reference (mid grey: A3 is already on the shoulder / clipped in the P1 default exposure)"""
    Y = lin @ M_RGB2XYZ[1]
    yref = ((ref_array()[:, 0] + 16) / 116) ** 3
    k = yref[3] / max(Y[3], 1e-9)
    return lin * k, k


def grey_norm(lin):
    ref_lin = lab_to_lin(ref_array())
    g = ref_lin[1:5].sum(0) / np.maximum(lin[1:5].sum(0), 1e-9)
    return lin * g, g


# ------------------------------------------------------------------ sampling
def nv12_to_rgb(path, W, H):
    f = np.fromfile(path, np.uint8)
    if f.size < W * H * 3 // 2:
        return None
    Y = f[:W * H].reshape(H, W).astype(float)
    uv = f[W * H:W * H * 3 // 2].reshape(H // 2, W // 2, 2).astype(float)
    U = np.repeat(np.repeat(uv[..., 0], 2, 0), 2, 1) - 128
    V = np.repeat(np.repeat(uv[..., 1], 2, 0), 2, 1) - 128
    return np.clip(np.stack([Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U], -1), 0, 255)


def frame_cells(img, H, dims):
    """per lattice cell: mean RGB of the central 50 % window, clipped-pixel fraction, window box"""
    n1, n2 = dims
    pitch = min(np.linalg.norm(apply_h(H, [(1, 0)])[0] - apply_h(H, [(0, 0)])[0]),
                np.linalg.norm(apply_h(H, [(0, 1)])[0] - apply_h(H, [(0, 0)])[0]))
    hw = max(2, int(round(0.25 * PATCH_FRAC * pitch)))        # half of (0.5 x patch side)
    out = {}
    for j in range(n2):
        for i in range(n1):
            x, y = apply_h(H, [(i, j)])[0]
            x, y = int(round(x)), int(round(y))
            win = img[max(y - hw, 0):y + hw + 1, max(x - hw, 0):x + hw + 1].reshape(-1, 3)
            out[(i, j)] = (win.mean(0), float((win >= CLIP).any(1).mean()), (x - hw, y - hw, x + hw, y + hw))
    return out, pitch


def orient(cells, dims, layout=None):
    """best of the lattice-compatible maps (grey-normalised dE00 vs reference), as in spyder.analyse"""
    R = ref_array()
    best = None
    rgb_cells = {k: v[0] for k, v in cells.items()}
    for M, off in candidates(dims):
        rgb = _canon(rgb_cells, M, off, layout)
        lab, _ = correct(rgb, "gn")
        de = float(de2000(R, lab).mean())
        if best is None or de < best[0]:
            best = (de, M, off)
    return best


def canon_cells(cells, M, off, layout=None):
    rgb = np.zeros((24, 3)); clip = np.zeros(24); box = [None] * 24
    for (i, j), (m, c, b) in cells.items():
        cc, r = M @ np.array([i, j]) + off
        k = IDS.index(layout_id(int(r), int(cc), layout))
        rgb[k], clip[k], box[k] = m, c, b
    return rgb, clip, box


def measure(frames):
    """frames: list of float (h,w,3) images of one stack/phase -> dict(rgb, clip, boxes, info) or None"""
    lat, ori, acc, cl, n, boxes, fails, shifts, ref_ctr = None, None, np.zeros((24, 3)), np.zeros(24), 0, None, 0, [], None
    for im in frames:
        d = detect(im)
        if d:
            lat = d
        elif lat is None:
            fails += 1
            continue
        else:
            fails += 1
        cells, pitch = frame_cells(im, lat["H"], lat["dims"])
        if ori is None:
            ori = orient(cells, lat["dims"])
        rgb, clip, box = canon_cells(cells, ori[1], ori[2])
        ctr = np.array([((b[0] + b[2]) / 2, (b[1] + b[3]) / 2) for b in box])
        if ref_ctr is None:
            ref_ctr = ctr
        shifts.append(float(np.abs(ctr - ref_ctr).max()))
        acc += rgb; cl = np.maximum(cl, clip); n += 1; boxes = box
    if n == 0:
        return None
    return {"rgb": acc / n, "clip": cl, "boxes": boxes, "frames": n, "detect_fail": fails, "orient_dE": ori[0],
            "max_shift_px": max(shifts), "pitch": float(pitch), "dims": lat["dims"]}


def overlay(img, boxes, clip, path, title):
    from PIL import Image, ImageDraw
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(im)
    for k, b in enumerate(boxes):
        col = (255, 0, 0) if clip[k] > 0.02 else (0, 255, 0)
        dr.rectangle(b, outline=col, width=2)
        dr.text((b[0] + 2, b[1] + 2), IDS[k], fill=col)
    dr.text((8, 8), title, fill=(255, 255, 0))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


# ------------------------------------------------------------------ analysis of one measurement
def analyse(m):
    R = ref_array()
    lin = srgb_to_lin(m["rgb"])
    linE, k = expo_norm(lin)
    lab50, lab65 = lin_to_lab50(linE), lin_to_lab65(linE)
    linG, g = grey_norm(lin)
    labG = lin_to_lab50(linG)
    labRaw = lin_to_lab50(lin)
    de = de2000(R, lab50); deG = de2000(R, labG); deRaw = de2000(R, labRaw)
    C, Cr = np.hypot(lab50[:, 1], lab50[:, 2]), np.hypot(R[:, 1], R[:, 2])
    h = np.degrees(np.arctan2(lab50[:, 2], lab50[:, 1])) % 360
    hr = np.degrees(np.arctan2(R[:, 2], R[:, 1])) % 360
    dh = (h - hr + 180) % 360 - 180
    clipped = m["clip"] > 0.02
    col = np.array([i in COLOURS for i in IDS])
    ok = ~clipped
    Yg = (linE @ M_RGB2XYZ[1])[:6]; Yr = ((R[:6, 0] + 16) / 116) ** 3
    sel = (Yg > 0.003) & ok[:6]
    gamma = float(np.polyfit(np.log(Yr[sel]), np.log(Yg[sel]), 1)[0]) if sel.sum() >= 3 else float("nan")
    code = m["rgb"] @ np.array([0.299, 0.587, 0.114])
    st = lambda a: {"mean": float(np.mean(a)), "median": float(np.median(a)), "max": float(np.max(a))}
    return {"lab50": lab50, "lab65": lab65, "labG": labG, "de": de, "deG": deG, "deRaw": deRaw, "C": C, "Cr": Cr, "dh": dh,
            "clipped": clipped, "code": code, "gamma": gamma, "expo_k": float(k), "gn_gain": g,
            "all": st(de), "unclipped": st(de[ok]), "colour": st(de[col & ok]), "grey": st(de[:6][ok[:6]]),
            "gn": st(deG[ok]), "raw": st(deRaw[ok]), "skin": st(de[[IDS.index(s) for s in SKIN]]),
            "chroma_ratio": float(C[col & ok].sum() / Cr[col & ok].sum()), "hue_err": float(np.mean(np.abs(dh[col & ok])))}


# ------------------------------------------------------------------ report
def f1(x):
    return "-" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.1f}"


def f2(x):
    return "-" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.2f}"


def phase_report(ph, res, meas):
    stacks = [s for s in ("open", "vendor") if s in res]
    L = [f"## {ph}", ""]
    for s in stacks:
        m = meas[s]
        L.append(f"- {s}: {m['frames']} frames (detect failures {m['detect_fail']}, max patch shift {m['max_shift_px']:.1f} px, "
                 f"pitch {m['pitch']:.0f} px, orientation residual dE00 {m['orient_dE']:.2f}), exposure factor x{res[s]['expo_k']:.2f}, "
                 f"grey-fit gains R/G/B {res[s]['gn_gain'][0]:.2f}/{res[s]['gn_gain'][1]:.2f}/{res[s]['gn_gain'][2]:.2f}"
                 + (f"; AWB: {m['awb']}" if m.get("awb") else ""))
    L += ["", "dE00 vs reference (Lab D50 Bradford, exposure-normalised on A4, no WB):", "",
          "| stack | mean | median | max | mean unclipped | colour 18 | greys | skin D5/D6 | gn mean (WB removed) | raw mean (no expo norm) | C* ratio | hue err deg | gamma (log-log, 1=ref) |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for s in stacks:
        r = res[s]
        L.append(f"| {s} | {f2(r['all']['mean'])} | {f2(r['all']['median'])} | {f1(r['all']['max'])} | {f2(r['unclipped']['mean'])} | "
                 f"{f2(r['colour']['mean'])} | {f2(r['grey']['mean'])} | {f2(r['skin']['mean'])} | {f2(r['gn']['mean'])} | {f2(r['raw']['mean'])} | "
                 f"{r['chroma_ratio']:.2f} | {r['hue_err']:.1f} | {r['gamma']:.2f} |")
    ov = None
    if len(stacks) == 2:
        ov = de2000(res["open"]["lab50"], res["vendor"]["lab50"])
        ok = ~(res["open"]["clipped"] | res["vendor"]["clipped"])
        L += ["", f"open vs vendor dE00 (both exposure-normalised): mean {ov.mean():.2f}, median {np.median(ov):.2f}, max {ov.max():.1f} "
              f"({IDS[int(np.argmax(ov))]}); unclipped mean {ov[ok].mean():.2f}; colour 18 {ov[6:].mean():.2f}, greys {ov[:6].mean():.2f}, "
              f"skin {ov[[IDS.index(x) for x in SKIN]].mean():.2f}"]
    # per patch table
    hdr = "| patch | name | ref L/a/b | " + " | ".join(f"{s} L/a/b | {s} dE00 | {s} C*/C*ref" for s in stacks) + (" | open-vs-vendor dE00 |" if ov is not None else " |")
    L += ["", "Per patch (Lab D50 after exposure normalisation; * = clipped > 2 % of the window):", "", hdr,
          "|" + "---|" * (3 + 3 * len(stacks) + (1 if ov is not None else 0))]
    R = ref_array()
    for k, pid in enumerate(IDS):
        row = f"| {pid} | {NAMES[pid]} | {R[k,0]:.1f}/{R[k,1]:+.1f}/{R[k,2]:+.1f} | "
        for s in stacks:
            r = res[s]; lb = r["lab50"][k]; c = "*" if r["clipped"][k] else ""
            ratio = r["C"][k] / r["Cr"][k] if pid in COLOURS else float("nan")
            row += f"{lb[0]:.1f}/{lb[1]:+.1f}/{lb[2]:+.1f}{c} | {r['de'][k]:.2f} | {f2(ratio)} | "
        if ov is not None:
            row += f"{ov[k]:.2f} |"
        L.append(row)
    # grey row
    L += ["", "Grey row A1..A6 (neutrality and tone; L* after exposure normalisation; code = mean Y' 0..255 of the JPEG):", "",
          "| patch | ref L* | " + " | ".join(f"{s} code | {s} L* | {s} dL* | {s} a*/b* D50 | {s} a*/b* D65" for s in stacks) + " |",
          "|" + "---|" * (2 + 5 * len(stacks))]
    for k in range(6):
        row = f"| A{k+1} | {R[k,0]:.1f} | "
        for s in stacks:
            r = res[s]; c = "*" if r["clipped"][k] else ""
            row += (f"{r['code'][k]:.0f}{c} | {r['lab50'][k,0]:.1f} | {r['lab50'][k,0]-R[k,0]:+.1f} | "
                    f"{r['lab50'][k,1]:+.1f}/{r['lab50'][k,2]:+.1f} | {r['lab65'][k,1]:+.1f}/{r['lab65'][k,2]:+.1f} | ")
        L.append(row)
    for s in stacks:
        r = res[s]
        L.append(f"\n{s}: grey mean a*/b* (A2..A5, D50) {r['lab50'][1:5,1].mean():+.2f}/{r['lab50'][1:5,2].mean():+.2f}, "
                 f"(D65) {r['lab65'][1:5,1].mean():+.2f}/{r['lab65'][1:5,2].mean():+.2f}; tone slope {r['gamma']:.2f}; "
                 f"skin D5 dE00 {r['de'][IDS.index('D5')]:.2f} hue err {r['dh'][IDS.index('D5')]:+.1f} deg C* x{r['C'][IDS.index('D5')]/r['Cr'][IDS.index('D5')]:.2f}, "
                 f"D6 dE00 {r['de'][IDS.index('D6')]:.2f} hue err {r['dh'][IDS.index('D6')]:+.1f} deg C* x{r['C'][IDS.index('D6')]/r['Cr'][IDS.index('D6')]:.2f}")
    L.append("")
    return L, ov


def read_awb(d):
    """optional AWB/CT readout saved by run_colour_*.sh (<stack>/<phase>/awb.txt, first line)"""
    p = os.path.join(d, "awb.txt")
    if os.path.exists(p):
        with open(p, errors="replace") as f:
            return " ".join(f.read().split())[:200]
    return None


def load_frames(d, nv12=None):
    if nv12:
        W, H = nv12
        fr = [nv12_to_rgb(p, W, H) for p in sorted(glob.glob(os.path.join(d, "nv12", "*.nv12")))]
        return [f for f in fr if f is not None]
    files = sorted(glob.glob(os.path.join(d, "snaps", "*.jpg")))
    return [load_float(p) for p in files]


def run_dir(R, phases=None, out=None, nv12=None):
    if not phases:
        sub = "nv12" if nv12 else "snaps"
        ph_o = {os.path.basename(os.path.dirname(p)) for p in glob.glob(os.path.join(R, "open", "*", sub))}
        ph_v = {os.path.basename(os.path.dirname(p)) for p in glob.glob(os.path.join(R, "vendor", "*", sub))}
        phases = sorted(p for p in (ph_o | ph_v) if "night" not in p)
    head = ["# Colour accuracy, SpyderCheckr 24 (colorchecker.py, #102 #230)", "",
            f"Run dir: `{R}`.  Source: {'NV12 ' + 'x'.join(map(str, nv12)) if nv12 else 'JPEG snapshots (snaps/*.jpg)'}.",
            "Reference: Datacolor SpyderCheckr 24 default measurements 'Original' (D50/2 deg, SpyderCheckr 1.6 software), not batch specific.",
            "dE00 vs reference compares against D50; under a 3000 K / 5000 K lamp the camera's white balance decides the cast, so read "
            "'gn' (WB removed) for the colour matrix and the open-vs-vendor dE00 as the fair stack comparison.", ""]
    body, summ, J = [], [], {}
    for ph in phases:
        meas, res = {}, {}
        for s in ("open", "vendor"):
            d = os.path.join(R, s, ph)
            if not os.path.isdir(d):
                continue
            fr = load_frames(d, nv12)
            if not fr:
                continue
            m = measure(fr)
            if not m:
                body.append(f"- {s} {ph}: chart not found in {len(fr)} frames")
                continue
            m["awb"] = read_awb(d)
            meas[s] = m
            res[s] = analyse(m)
            overlay(fr[-1], m["boxes"], m["clip"], os.path.join(R, "colour", f"overlay_{s}_{ph}{'_nv12' if nv12 else ''}.png"), f"{s} {ph}")
        if not res:
            continue
        L, ov = phase_report(ph, res, meas)
        body += L
        sm = {s: {"dE00_mean": res[s]["all"]["mean"], "dE00_median": res[s]["all"]["median"], "dE00_max": res[s]["all"]["max"],
                  "dE00_mean_unclipped": res[s]["unclipped"]["mean"], "gn_mean": res[s]["gn"]["mean"], "chroma_ratio": res[s]["chroma_ratio"],
                  "skin": res[s]["skin"]["mean"], "grey_a": float(res[s]["lab50"][1:5, 1].mean()), "grey_b": float(res[s]["lab50"][1:5, 2].mean()),
                  "gamma": res[s]["gamma"], "per_patch_dE00": dict(zip(IDS, map(float, res[s]["de"])))} for s in res}
        if ov is not None:
            sm["open_vs_vendor"] = {"mean": float(ov.mean()), "median": float(np.median(ov)), "max": float(ov.max()),
                                    "per_patch": dict(zip(IDS, map(float, ov)))}
        J[ph] = sm
        g = lambda s, k: f2(sm[s][k]) if s in sm else "-"
        summ.append(f"| {ph} | {g('open','dE00_mean')} / {g('open','dE00_median')} / {f1(sm['open']['dE00_max']) if 'open' in sm else '-'} | "
                    f"{g('vendor','dE00_mean')} / {g('vendor','dE00_median')} / {f1(sm['vendor']['dE00_max']) if 'vendor' in sm else '-'} | "
                    f"{g('open','gn_mean')} / {g('vendor','gn_mean')} | {g('open','chroma_ratio')} / {g('vendor','chroma_ratio')} | "
                    f"{g('open','grey_a')},{g('open','grey_b')} / {g('vendor','grey_a')},{g('vendor','grey_b')} | "
                    f"{g('open','skin')} / {g('vendor','skin')} | "
                    + (f"{sm['open_vs_vendor']['mean']:.2f} / {sm['open_vs_vendor']['median']:.2f} / {sm['open_vs_vendor']['max']:.1f} |" if ov is not None else "- |"))
    top = ["## Summary", "",
           "| phase | open dE00 mean/median/max | vendor dE00 mean/median/max | gn mean open/vendor | C* ratio open/vendor | grey a*,b* open/vendor | skin dE00 open/vendor | open-vs-vendor dE00 mean/median/max |",
           "|---|---|---|---|---|---|---|---|"] + summ + [""]
    txt = "\n".join(head + top + body) + "\n"
    out = out or os.path.join(R, "COLOUR_NV12.md" if nv12 else "COLOUR.md")
    with open(out, "w") as f:
        f.write(txt)
    os.makedirs(os.path.join(R, "colour"), exist_ok=True)
    with open(os.path.join(R, "colour", "chart_de_nv12.json" if nv12 else "chart_de.json"), "w") as f:
        json.dump(J, f, indent=1, default=float)
    print("\n".join(head + top))
    print(f"written {out}")


def loose(files, vs=None):
    meas, res = {}, {}
    for s, fl in (("open", files), ("vendor", vs)):
        if not fl:
            continue
        m = measure([load_float(p) for p in fl])
        if m:
            meas[s], res[s] = m, analyse(m)
    L, _ = phase_report("files", res, meas)
    print("\n".join(L))


if __name__ == "__main__":
    a = sys.argv[1:]
    if "--ref" in a:
        i = a.index("--ref"); os.environ["SPYDER_REF"] = os.path.abspath(a[i + 1]); del a[i:i + 2]
    if not a or a[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if a[0] == "--files":
        a = a[1:]
        vs = a[a.index("--vs") + 1:] if "--vs" in a else None
        loose(a[:a.index("--vs")] if vs else a, vs)
        sys.exit(0)
    R, phases, out, nv = a[0], None, None, None
    if "--phases" in a:
        phases = a[a.index("--phases") + 1].split(",")
    if "--out" in a:
        out = a[a.index("--out") + 1]
    if "--nv12" in a:
        i = a.index("--nv12"); nv = (int(a[i + 1]), int(a[i + 2]))
    run_dir(os.path.abspath(R), phases, out, nv)
