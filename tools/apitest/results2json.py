#!/usr/bin/env python3
"""Turn the logs of one device run into per-function results for docs/api_evidence.json.

Inputs (any combination):
  --apitest FILE   tools/apitest output:   [A] IMP_x_y ret=N PASS|FAIL|N/A|SKIP <detail>
  --devtest FILE   tests/t40/devtest:      RES <Area_Func> PASS|PART|FAIL <detail>   (T40)
  --imgfx FILE     tools/imgfx (IMGFX_STATS=1): [R] <case> set=N get=<readback> | ST Y=.. U=.. V=.. H=.. G=..
Output (--out): {"soc","ip","date","image","funcs":{key:{"v":"PASS|FAIL|PART|SKIP|NA","detail":..,"src":[..]}},"unmapped":[..]}
Keys are the function keys of docs/api_evidence.json (IMP_ prefix removed, Get/Set pairs joined).  A key with
several member functions (Get/Set) is PASS only when every member reported PASS; a FAIL of any member is FAIL;
PASS + (N/A|SKIP) members is PART.  imgfx cases are mapped through IMGFX_CASES below.
Usage: results2json.py --soc T41 --ip 192.168.10.34 --image agg47 [--date D] --out result.json [inputs]
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EVID = os.path.join(HERE, "..", "..", "docs", "api_evidence.json")

# imgfx case-name prefix -> function keys that the case exercises (set + read back + picture effect)
IMGFX_CASES = [
    ("isp-hflip", ["ISP_Tuning_Get/SetHVFlip"]), ("isp-vflip", ["ISP_Tuning_Get/SetHVFlip"]),
    ("isp-hvflip2", ["ISP_Tuning_Get/SetHVFLIP"]), ("isp-hvflip", ["ISP_Tuning_Get/SetHVFlip"]),
    ("sensor-", ["ISP_Tuning_Get/SetHVFLIP"]),
    ("fcrop-", ["ISP_Tuning_Get/SetFrontCrop"]), ("fs1-crop", ["FrameSource_Get/SetChnCrop"]),
    ("autozoom", ["ISP_Tuning_Get/SetAutoZoom"]),
    ("brightness", ["ISP_Tuning_Get/SetBrightness"]), ("contrast", ["ISP_Tuning_Get/SetContrast"]),
    ("saturation", ["ISP_Tuning_Get/SetSaturation"]), ("sharpness", ["ISP_Tuning_Get/SetSharpness"]),
    ("hue", ["ISP_Tuning_Get/SetBcshHue"]), ("gamma", ["ISP_Tuning_Get/SetGammaAttr"]),
    ("ae-comp", ["ISP_Tuning_Get/SetAeComp"]), ("exp-", ["ISP_Tuning_Get/SetExpr"]),
    ("aeattr", ["ISP_Tuning_Get/SetAeAttr"]), ("maxagain", ["ISP_Tuning_Get/SetMaxAgain"]),
    ("maxdgain", ["ISP_Tuning_Get/SetMaxDgain"]),
    ("awb-", ["ISP_Tuning_Get/SetWB"]), ("awbct", ["ISP_Tuning_Get/SetAwbCt"]),
    ("ccm", ["ISP_Tuning_Get/SetCCMAttr"]), ("csc", ["ISP_Tuning_Get/SetISPCSCAttr"]),
    ("defog", ["ISP_Tuning_Get/SetDefog_Strength"]), ("wdr", ["ISP_Tuning_Get/SetWDRAttr"]),
    ("drc", ["ISP_Tuning_Get/SetRawDRC"]), ("sinter", ["ISP_Tuning_Get/SetSinterStrength"]),
    ("temper", ["ISP_Tuning_Get/SetTemperStrength"]), ("dpc", ["ISP_Tuning_Get/SetDPStrength"]),
    ("mask", ["ISP_Tuning_Get/SetMask"]), ("antiflicker", ["ISP_Tuning_Get/SetAntiFlickerAttr"]),
    ("runmode", ["ISP_Tuning_Get/SetISPRunningMode"]), ("fps", ["ISP_Tuning_Get/SetSensorFPS"]),
]
# cases where no picture change is expected on a static scene (set + readback only)
NOEFFECT = ("antiflicker", "runmode", "fps", "base", "awbct", "maxagain", "maxdgain", "dpc", "wdr", "drc", "bypass")


def load_keys():
    ev = json.load(open(EVID, encoding="utf-8"))
    return list(ev["functions"])


def members(key):
    m = re.match(r"^(.*?)Get/Set(.*)$", key)
    return [m.group(1) + "Get" + m.group(2), m.group(1) + "Set" + m.group(2)] if m else [key]


class Acc:
    def __init__(self, keys):
        self.keys = keys
        self.m2k = {}
        for k in keys:
            if "*" in k or " " in k:
                continue
            for n in members(k):
                self.m2k.setdefault(n, k)
        self.seen = {}      # key -> {member: (verdict, detail, src)}
        self.unmapped = []

    def key_of(self, name):
        n = re.sub(r"^IMP_", "", name)
        n = re.sub(r"\([^)]*\)$", "", n)               # "Encoder_Get/SetX(CBR)" -> variant of the key
        if n in self.keys and "Get/Set" in n:           # a whole pair reported at once
            return [(n, x) for x in members(n)], None
        if n in self.m2k:
            return self.m2k[n], n
        if "/" in n and n not in self.keys:         # composite "Encoder_A/B/C" -> Encoder_A, Encoder_B, Encoder_C
            parts = n.split("/")
            pre = parts[0][:parts[0].rindex("_") + 1] if "_" in parts[0] else ""
            names = [parts[0]] + [pre + p for p in parts[1:]]
            hit = [x for x in names if x in self.m2k]
            if hit:
                return [(self.m2k[x], x) for x in hit], None
        return None, n

    def add(self, name, verdict, detail, src, whole=False):
        k, n = self.key_of(name)
        if k is None:
            self.unmapped.append("%s %s %s" % (src, name, verdict))
            return
        for kk, nn in (k if isinstance(k, list) else [(k, n)]):
            d = self.seen.setdefault(kk, {})
            old = d.get(nn)
            # keep the worst verdict of repeated reports of the same member
            order = {"FAIL": 0, "PART": 1, "PASS": 2, "SKIP": 3, "NA": 4}
            if old is None or order[verdict] < order[old[0]]:
                d[nn] = (verdict, detail, src)
            elif old[0] == verdict == "PASS" and detail and not old[1]:
                d[nn] = (verdict, detail, src)

    def result(self):
        out = {}
        for k, d in self.seen.items():
            mem = members(k)
            vs = [d[x][0] for x in mem if x in d]
            if "FAIL" in vs:
                v = "FAIL"
            elif vs and all(x == "PASS" for x in vs) and len(vs) == len(mem):
                v = "PASS"
            elif "PASS" in vs or "PART" in vs:
                v = "PART"
            elif "SKIP" in vs:
                v = "SKIP"
            else:
                v = "NA"
            detail = "; ".join("%s: %s" % (x, d[x][1]) if len(mem) > 1 else d[x][1] for x in mem if x in d and (d[x][0] in ("FAIL", "PART") or len(d) == 1 or True))[:400]
            out[k] = {"v": v, "detail": detail, "src": sorted({d[x][2] for x in d})}
        return out


def parse_apitest(path, acc):
    for l in open(path, errors="replace"):
        m = re.match(r"\[A\] (\S+) ret=(-?\d+|\S+) (PASS|FAIL|N/A|SKIP)(?: (.*))?$", l.rstrip("\n"))
        if not m:
            continue
        fn, ret, v, det = m.group(1), m.group(2), m.group(3), (m.group(4) or "")
        v = {"N/A": "NA"}.get(v, v)
        acc.add(fn, v, ("ret=%s %s" % (ret, det)).strip(), "apitest")


def parse_devtest(path, acc):
    for l in open(path, errors="replace"):
        m = re.match(r"RES (\S+) (PASS|PART|FAIL)(?: (.*))?$", l.rstrip("\n"))
        if m:
            acc.add(m.group(1), m.group(2), m.group(3) or "", "devtest")


def stats(line):
    m = re.search(r"ST Y=([\d.]+) U=([\d.]+) V=([\d.]+) H=([\d.]+) G=([\d.,]+)", line)
    if not m:
        return None
    return dict(Y=float(m.group(1)), U=float(m.group(2)), V=float(m.group(3)), H=float(m.group(4)), G=[float(x) for x in m.group(5).split(",")])


def parse_imgfx(path, acc):
    rows = []
    for l in open(path, errors="replace"):
        m = re.match(r"\[R\] (\S+) (?:N/A|set=(-?\d+) get=(.*?)(?: \| (ST .*))?)\s*$", l.rstrip("\n"))
        if not m:
            continue
        if "set=" not in l:
            rows.append((m.group(1), None, "", None))
        else:
            rows.append((m.group(1), int(m.group(2)), m.group(3), stats(m.group(4) or "")))
    base = [r for r in rows if r[0] == "base" and r[3]]
    end = [r for r in rows if r[0] == "base-end" and r[3]]
    if not base:
        return
    b = base[0][3]

    def diff(a, c):
        return dict(dY=abs(a["Y"] - c["Y"]), dU=abs(a["U"] - c["U"]), dV=abs(a["V"] - c["V"]),
                    mad=sum(abs(x - y) for x, y in zip(a["G"], c["G"])) / 64.0,
                    dH=abs(a["H"] - c["H"]) / max(a["H"], 0.01) * 100)
    noise = diff(b, end[0][3]) if end else dict(dY=0, dU=0, dV=0, mad=0, dH=0)

    def changed(d):
        return d["dY"] > max(1.0, 2 * noise["dY"]) or d["mad"] > max(1.0, 2 * noise["mad"]) or \
            d["dU"] > max(0.4, 2 * noise["dU"]) or d["dV"] > max(0.4, 2 * noise["dV"]) or d["dH"] > max(1.5, 2 * noise["dH"])
    for case, ret, info, st in rows:
        if case.startswith("base"):
            continue
        keys = next((v for p, v in IMGFX_CASES if case.startswith(p)), None)
        if ret is None:
            continue                      # N/A: not exported
        if not keys:
            acc.unmapped.append("imgfx case %s" % case)
            continue
        noeff = any(case.startswith(p) for p in NOEFFECT)
        if ret != 0:
            v, det = "FAIL", "case %s: set returned %d (%s)" % (case, ret, info)
        elif st is None:
            v, det = "FAIL", "case %s: no picture (%s)" % (case, info)
        else:
            d = diff(b, st)
            ch = changed(d)
            det = "case %s: %s; dY %.1f dU %.1f dV %.1f grid %.1f dH %.1f%%" % (case, info, d["dY"], d["dU"], d["dV"], d["mad"], d["dH"])
            v = "PASS" if (ch or noeff) else "PART"
            if not ch and not noeff:
                det += " (no visible effect)"
        for k in keys:
            acc.add(k, v, det, "imgfx")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--soc", required=True)
    ap.add_argument("--ip", default="")
    ap.add_argument("--image", default="")
    ap.add_argument("--date", default="")
    ap.add_argument("--apitest")
    ap.add_argument("--devtest")
    ap.add_argument("--imgfx")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    acc = Acc(load_keys())
    for f, fn in ((a.apitest, parse_apitest), (a.devtest, parse_devtest), (a.imgfx, parse_imgfx)):
        if f and os.path.exists(f):
            fn(f, acc)
    res = acc.result()
    cnt = {}
    for r in res.values():
        cnt[r["v"]] = cnt.get(r["v"], 0) + 1
    json.dump({"soc": a.soc, "ip": a.ip, "image": a.image, "date": a.date, "counts": cnt, "funcs": res,
               "unmapped": sorted(set(acc.unmapped))}, open(a.out, "w"), indent=1, sort_keys=True)
    print("%s: %d functions %s, %d unmapped" % (a.soc, len(res), cnt, len(set(acc.unmapped))))


if __name__ == "__main__":
    main()
