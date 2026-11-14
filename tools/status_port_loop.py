#!/usr/bin/env python3
"""Refresh docs/status/port_loop.json: every attempt of the paid-API port loop (one ds_port run per block per model).

Local tool (the pages build in CI reads only the JSON).  Reads the local loop logs (never published):
  T32  <audit>/t32pr/ds/*.log|*.out   result lines of ds_port.py ("PASS after N rounds ..." / "NOT PASSED after ..."), the same
       attempt shows up in several aggregate logs and is merged by its token counts; block size from the task files / queue lists
  T33  <audit>/t33-port/loop-cost.md   one row per attempt (block, functions, model, result, date); an "auto" row is split into
       deepseek-flash / deepseek-v4-pro with <audit>/t33-port/loop/logs/run-<block>-auto.out
  T10  <audit>/delift/t10/cost.md      de-lift loop; only rows with a model name count (deterministic rewrites are no attempts)
Only counts and rates are written: no money, no keys, no account data, no local paths.  Anything a log does not settle is
stored as null / "unknown" (never guessed).
Model of an attempt: "auto" starts on deepseek-flash and escalates to deepseek-v4-pro inside the same run (ds_port.py).  A run that
escalated is two attempts: a failed deepseek-flash one and a deepseek-v4-pro one carrying the run's result (via: flash-escalation).  Result: pass / fail (test never went green) /
rejected-in-review (only when a log says the finished port was rejected; none is recorded so far).  "review": "gaps" = an independent
review of a passing oracle listed gaps that a follow-up fix closed (the attempt stays a pass).
Usage: python3 tools/status_port_loop.py [--audit DIR] [--print]
"""
import argparse
import datetime
import glob
import json
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "status", "port_loop.json")
AUDIT = "/mnt/NVMe/git/scratch-clones/session-110bf918/audit"
DEEPSEEK = ("deepseek-flash", "deepseek-v4-pro")
RES = re.compile(r"^(?:(?P<pre>[\w ]+?)(?: \((?P<paren>[^)]+)\))?: )?(?P<res>PASS|NOT PASSED) after (?P<rounds>\d+) rounds, "
                 r"total tokens in (?P<tin>\d+) out (?P<tout>\d+)(?:, model (?P<model>[\w.-]+?))?(?:,| about|$)")
ROUND = re.compile(r"^round \d+ \(([\w.-]+)[,)]")


def day(path):
    return datetime.date.fromtimestamp(os.path.getmtime(path)).isoformat()


def md_count(path):
    try:
        return sum(1 for l in open(path, errors="replace") if l.startswith("- `"))
    except OSError:
        return None


def t32_funcs(d, block, suf):
    """function count of a block from its task file; None when the candidates disagree (no guessing)"""
    cands = set()
    for s in ([suf] if suf else ["f", "p", "d"]):
        n = md_count(os.path.join(d, "task-%s%s.md" % (block, s)))
        if n:
            cands.add(n)
    return cands.pop() if len(cands) == 1 else None


def parse_t32(d):
    recs = {}
    for f in sorted(glob.glob(os.path.join(d, "*.log")) + glob.glob(os.path.join(d, "*.out"))):
        base = os.path.basename(f)
        hdr = {"block": None, "suf": "", "model": None, "kind": "port", "funcs": None}
        rounds, esc = [], False
        for ln in open(f, errors="replace"):
            ln = ln.rstrip("\n")
            m = re.match(r"^== (\d\d:\d\d) gapfix (\w+?)x? (claude-\S+)", ln)
            if m:
                hdr = {"block": m.group(2), "suf": "", "model": m.group(3), "kind": "gap-fix", "funcs": None}
                rounds, esc = [], False
                continue
            m = re.match(r"^== (\d\d:\d\d) new block (\w+) \((\d+) functions\) (claude-\S+)", ln)
            if m:
                hdr = {"block": m.group(2), "suf": "", "model": m.group(4), "kind": "new-block", "funcs": int(m.group(3))}
                rounds, esc = [], False
                continue
            m = re.match(r"^== (\d\d:\d\d) (\w+)(?: (\w+))? (auto|claude-\S+)\s*$", ln)
            if m:
                hdr = {"block": m.group(2), "suf": m.group(3) or "", "model": m.group(4), "kind": "port", "funcs": None}
                rounds, esc = [], False
                continue
            m = re.match(r"^== (\w+) (\d\d:\d\d)(?: model (\S+?))?(?:,|$)", ln)
            if m and m.group(1) != "done":
                hdr = {"block": m.group(1), "suf": "", "model": m.group(3) or "auto", "kind": "port", "funcs": None}
                rounds, esc = [], False
                continue
            m = re.match(r"^== (flash|pro)$", ln)
            if m:
                hdr = {"block": "ydns", "suf": "", "model": {"flash": "deepseek-flash", "pro": "deepseek-v4-pro"}[m.group(1)], "kind": "port", "funcs": None}
                rounds, esc = [], False
                continue
            m = ROUND.match(ln)
            if m:
                rounds.append(m.group(1))
                continue
            if ln.startswith("escalate to"):
                esc = True
                continue
            m = RES.match(ln)
            if not m:
                continue
            key = (int(m.group("tin")), int(m.group("tout")), int(m.group("rounds")))
            model = m.group("model")
            if not model and rounds:
                model = rounds[-1]
            if not model and m.group("paren"):
                model = m.group("paren")
            if not model:
                model = hdr["model"]
            if model == "sonnet":
                model = "claude-sonnet-5-5"
            block = hdr["block"] or (m.group("pre") or "").split()[-1:] and (m.group("pre") or "").split()[-1] or None
            if not hdr["block"] and m.group("pre") and m.group("pre").strip() in ("opus gib", "opus csc"):
                block = m.group("pre").split()[1]
            if base.startswith("par-p.log") and m.group("pre"):
                hdr = dict(hdr, suf="p")
            funcs = hdr["funcs"]
            if funcs is None and block and hdr["kind"] == "port" and base != "run3.log":
                funcs = t32_funcs(d, block, hdr["suf"] if hdr["suf"] else ("p" if base.startswith("par-p") else ""))
            r = {"date": day(f), "block": block, "suf": hdr["suf"], "funcs": funcs, "model": model, "kind": hdr["kind"],
                 "result": "pass" if m.group("res") == "PASS" else "fail", "rounds": key[2], "esc": esc}
            old = recs.get(key)
            if old is None:
                recs[key] = r
            else:   # same attempt seen in another log: keep the most specific facts, earliest date
                if old["kind"] == "port" and r["kind"] != "port":
                    old["kind"], old["funcs"] = r["kind"], r["funcs"]
                if old["kind"] != "port":
                    old["funcs"] = None if old["kind"] == "gap-fix" else old["funcs"]
                for k in ("funcs", "block", "suf"):
                    if old[k] in (None, ""):
                        old[k] = r[k]
                if old["model"] in (None, "auto") and r["model"] not in (None, "auto"):
                    old["model"] = r["model"]
                old["esc"] = old["esc"] or r["esc"]
                old["date"] = min(old["date"], r["date"])
    out = []
    for r in recs.values():
        if r["kind"] == "gap-fix":
            r["funcs"] = None
        if r["model"] is None:
            r["model"], r["kind"] = "unknown", "early-trial"
        if r["model"] == "auto":
            r["model"] = "deepseek-unknown"
        elif r["model"] == "deepseek-flash" and r["esc"]:
            r["model"] = "deepseek-v4-pro"
        out.append(r)
    return out


def parse_t33(base):
    out, rows = [], []
    cost = os.path.join(base, "loop-cost.md")
    for ln in open(cost, errors="replace"):
        c = [x.strip() for x in ln.strip().strip("|").split("|")]
        if len(c) == 6 and c[3] in ("PASS", "FAIL") and c[1].isdigit():
            rows.append(c)
    logs = os.path.join(base, "loop", "logs")
    auto = {}   # (block, usd) -> flash|pro from run-<block>-auto.out
    for f in glob.glob(os.path.join(logs, "run-*-auto.out")):
        b = os.path.basename(f)[4:-9]
        txt = open(f, errors="replace").read()
        m = re.search(r"^(PASS|NOT PASSED) after \d+ rounds.*?(?:model (\S+?),)? about \$([0-9.]+)", txt, re.M)
        if m:
            fm = re.search(r"^auto: (\S+)", txt, re.M)
            esc = "escalate to" in txt
            model = m.group(2) or (("deepseek-v4-pro" if esc else fm.group(1)) if fm else None)
            auto[(b, m.group(3))] = (model, esc)
    used = set()
    for blk, nf, model, res, usd, when in rows:
        usd = usd.lstrip("$")
        if model == "auto":
            model, esc = auto.get((blk, usd), (None, False))
            if model is None:   # the run log was overwritten by a later retry; a failed auto run always ended on Pro (it escalates after Flash)
                model, esc = ("deepseek-v4-pro", True) if res == "FAIL" else ("deepseek-unknown", False)
            used.add((blk, usd))
        mm = re.match(r"(\d\d)-(\d\d)", when)
        out.append({"date": "2026-%s-%s" % mm.groups(), "block": blk, "suf": "", "funcs": int(nf), "model": model,
                    "kind": "port", "result": "pass" if res == "PASS" else "fail", "rounds": None, "esc": esc})
    for (b, usd), (model, esc) in auto.items():   # run logs without a cost row (earlier runs)
        if (b, usd) in used:
            continue
        f = os.path.join(logs, "run-%s-auto.out" % b)
        txt = open(f, errors="replace").read()
        m = re.search(r"^(PASS|NOT PASSED) after (\d+) rounds", txt, re.M)
        out.append({"date": day(f), "block": b, "suf": "", "funcs": None, "model": model, "kind": "port",
                    "result": "pass" if m.group(1) == "PASS" else "fail", "rounds": int(m.group(2)), "esc": esc})
    return out


def parse_t10(path):
    out = []
    if not os.path.exists(path):
        return out
    for ln in open(path, errors="replace"):
        c = [x.strip() for x in ln.strip().strip("|").split("|")]
        if len(c) >= 5 and re.search(r"claude-|deepseek", c[2]):
            res = "pass" if c[3].lower().startswith("pass") else "fail"
            nf = re.match(r"~?(\d+)$", c[1])
            out.append({"date": day(path), "block": c[0][:30], "suf": "", "funcs": int(nf.group(1)) if nf else None, "model": re.search(r"(claude-[\w.-]+|deepseek-[\w.-]+)", c[2]).group(1), "kind": "de-lift",
                        "result": res, "rounds": None, "esc": False})
    return out


def expand(att):
    """a DeepSeek run that escalated from Flash to Pro (ds_port.py) is two attempts: Flash failed (it only escalates when the Flash
    rounds did not go green), then Pro with the result of the run"""
    out = []
    for r in att:
        if r["model"] == "deepseek-v4-pro" and r.get("esc"):
            out.append(dict(r, model="deepseek-flash", result="fail", review=None, esc=False, via=None))
            r = dict(r, via="flash-escalation")
        out.append(r)
    return out


FN = re.compile(r"fn_[0-9a-f]+_[A-Za-z0-9_]+\.c")
REPO = os.environ.get("OPEN_TX_ISP", "/mnt/NVMe/git/open-tx-isp")
SRC_ORDER = ["api-deepseek-flash", "api-deepseek-v4-pro", "api-claude-sonnet-5-5", "api-claude-opus-5-5", "api-claude-fable-5-2", "api-unknown",
             "agent-opus", "agent-sonnet", "agent-fable", "agent-unknown", "script", "no-port", "unknown"]


def git(*a):
    return subprocess.run(["git", "-C", REPO] + list(a), capture_output=True, text=True).stdout


def conf_funcs(path, txt):
    if "t32_oracle" in path:
        return set(FN.findall(txt))
    out = set()
    m = re.search(r"^FUNCS='(.*?)'", txt, re.S | re.M)
    for ln in (m.group(1) if m else "").splitlines():
        w = ln.split()
        if w and not w[0].startswith("#") and w[0] not in ("VEC", "GLOBAL", "CALLS"):
            out.add(w[0])
    m = re.search(r"^PROVES='(.*?)'", txt, re.S | re.M)
    return out | set((m.group(1) if m else "").split())


def pass_model(att, block, sufs):
    """model of the passing port attempt(s) of a block; None unless exactly one model passed (never guessed)"""
    ms = {a["model"] for a in att if a["block"] == block and a["suf"] in sufs and a["result"] == "pass" and a["kind"] == "port"}
    return ms.pop() if len(ms) == 1 else None


def contributors(ref, t32, t33, delift):
    """proven functions per source and project (a function counts once, for the oldest oracle block that proves it).
    Source of a block: explicit model in its commit subject (T33 loop commits, T32 "Claude API"), else the passing attempt of the
    T32/T33 loop logs when exactly one model passed, else the Co-Authored-By trailer of the commit (agent models), else unknown."""
    files = [f for f in git("ls-tree", "-r", "--name-only", ref, "tests/t32_oracle", "tests/t33_binoracle").split() if f.endswith(".conf")]
    first = {}   # conf -> (subject, body); log is oldest first
    for blk in git("log", ref, "--reverse", "--diff-filter=A", "--name-only", "--format=@@%H").split("@@")[1:]:
        ls = blk.strip().split("\n")
        body = git("show", "-s", "--format=%s%n%b", ls[0])
        for f in ls[1:]:
            if f in files and f not in first:
                first[f] = body
    res = {"T32": {}, "T33": {}}
    seen = {"T32": set(), "T33": set()}
    blocks = {"T32": {}, "T33": {}}
    for f in files:   # oldest-first order of the add commits
        pass
    for f in sorted(first, key=lambda x: list(first).index(x)):
        proj = "T32" if "t32_oracle" in f else "T33"
        body = first[f]
        stem = os.path.basename(f)[:-5]
        subj = body.split("\n", 1)[0]
        src = None
        if proj == "T33":
            m = re.search(r"port loop: block \S+ .*?#99, ([\w.-]+),", subj)
            mdl = m.group(1) if m else (pass_model(t33, stem, [""]) if "port loop batch" in subj else None)
            if m and mdl == "auto":
                mdl = pass_model(t33, stem, [""])
            if m or "port loop batch" in subj:
                src = "api-" + mdl if mdl in ("deepseek-flash", "deepseek-v4-pro", "claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-2") else "api-unknown"
        else:
            m = re.search(r"Claude API (claude-[\w.-]+)", subj)
            if m:
                src = "api-" + m.group(1)
            elif "DeepSeek port loop" in subj:
                m2 = re.match(r"t32_(\w+?)([a-z]?)_oracle$", stem)
                mdl = None
                if m2:
                    b, sf = m2.groups()
                    mdl = pass_model(t32, b, [sf, ""]) if sf else pass_model(t32, b, [""])
                src = "api-" + mdl if mdl in ("deepseek-flash", "deepseek-v4-pro", "claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-2") else "api-unknown"
        if src is None:
            tr = " ".join(re.findall(r"(?im)^co-authored-by: Claude (\w+)", body)).lower()
            src = "agent-" + tr.split()[0] if tr and tr.split()[0] in ("opus", "sonnet", "fable") else "agent-unknown"
        fs = conf_funcs(f, git("show", "%s:%s" % (ref, f))) - seen[proj]
        seen[proj] |= fs
        if fs:
            res[proj][src] = res[proj].get(src, 0) + len(fs)
            blocks[proj][src] = blocks[proj].get(src, 0) + 1
    out = {}
    for proj in ("T32", "T33"):
        out[proj] = {"functions": len(seen[proj]), "sources": {k: res[proj][k] for k in SRC_ORDER if k in res[proj]},
                     "blocks": {k: blocks[proj][k] for k in SRC_ORDER if k in blocks[proj]}}
    if delift:
        cur, base, rows = delift
        script = bool(rows)   # the STATE file reports exactly this proven count after the deterministic batch 1 (no LLM)
        d = cur - base
        out["T10 de-lift"] = {"functions": cur, "sources": {k: v for k, v in (("no-port", base), ("script" if script else "unknown", d)) if v},
                              "blocks": {}}
    return out


def delift_data(d):
    try:
        cur = json.load(open(os.path.join(d, "summary.json")))["proven_readable"]
        base = json.load(open(os.path.join(d, "summary_base.json")))["proven_readable"]
        rows = [l for l in open(os.path.join(d, "cost.md"), errors="replace") if l.startswith("|") and not l.startswith("|---") and "| block |" not in l]
        st = re.search(r"now (\d+) \(", open(os.path.join(d, "STATE.md"), errors="replace").read())
        rows = rows if st and int(st.group(1)) == cur and any("no LLM" in l for l in rows) else []
    except (OSError, ValueError, KeyError):
        return None
    return cur, base, rows


def reviews(d):
    """(block, suffix) of oracles an independent review found gaps in (all verdicts so far: gaps, closed by follow-up fixes)"""
    s = set()
    for f in glob.glob(os.path.join(d, "rev-*.json")):
        try:
            v = json.load(open(f)).get("verdict")
        except (OSError, ValueError):
            continue
        m = re.match(r"rev-(\w+?)-(\w*?)(?:-new)?\.json$", os.path.basename(f))
        if m and v == "gaps":
            s.add((m.group(1), m.group(2)))
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--audit", default=AUDIT)
    ap.add_argument("--ref", default="origin/next", help="open-tx-isp ref with the merged oracle blocks (checkout: $OPEN_TX_ISP)")
    ap.add_argument("--print", action="store_true")
    o = ap.parse_args()
    d32 = os.path.join(o.audit, "t32pr", "ds")
    att = []
    t32 = parse_t32(d32)
    rv = reviews(d32)
    for r in t32:
        r["project"] = "T32"
        r["review"] = "gaps" if r["result"] == "pass" and r["kind"] != "gap-fix" and (r["block"], r["suf"]) in rv else None
    att += t32
    for r in parse_t33(os.path.join(o.audit, "t33-port")):
        r["project"] = "T33"
        r["review"] = None
        att.append(r)
    for r in parse_t10(os.path.join(o.audit, "delift", "t10", "cost.md")):
        r["project"] = "T10 de-lift"
        r["review"] = None
        att.append(r)
    contrib = contributors(o.ref, t32, [r for r in att if r["project"] == "T33"], delift_data(os.path.join(o.audit, "delift", "t10")))
    att = expand(att)
    att.sort(key=lambda r: (r["date"] or "", r["project"], r["block"] or ""))
    keep = ("date", "project", "block", "funcs", "model", "kind", "result", "review", "via")
    data = {"generated": datetime.date.today().isoformat(),
            "note": "counts only; regenerate with tools/status_port_loop.py (local logs). null = the logs do not settle it.",
            "contributors": contrib,
            "attempts": [{k: r.get(k) for k in keep} for r in att]}
    if o.print:
        from collections import Counter
        c = Counter((r["project"], r["model"], r["result"]) for r in att)
        for k in sorted(c, key=str):
            print(k, c[k])
        print(json.dumps(contrib, indent=1))
        print("unknown size:", sum(1 for r in att if r["funcs"] is None and r["kind"] == "port"))
        return
    with open(OUT, "w") as f:
        json.dump(data, f, indent=1, sort_keys=False)
        f.write("\n")
    print("wrote %s: %d attempts" % (OUT, len(att)))


if __name__ == "__main__":
    main()
