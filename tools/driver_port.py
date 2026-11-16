#!/usr/bin/env python3
"""Section "Kernel driver port from the recovered vendor code (open-tx-isp)": Markdown + HTML card (stdlib).
Data: docs/status/driver_port.json (refreshed locally by tools/status_driver_port.py). Used by gen_api_coverage.update_md and the HTML generator."""
import html
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "docs", "status", "driver_port.json")
TITLE = "Kernel driver port from the recovered vendor code (open-tx-isp)"
INTRO = ("Separate from the streamer-function bars above: this counts functions of the recovered vendor kernel driver that were "
         "ported to open C and proven bit-exact against the recovered code by a host oracle, out of all functions of the recovered "
         "driver. It says nothing about the OpenIMP library. The T32/T33 driver port is merged into open-tx-isp "
         "next; its reference-C oracles stay on the T32 branch until upstream PR #15 lands. Host-proven is not device-tested. T33 has no recovered vendor C: its row counts the "
         "T33-only functions of the vendor module that pass the T33 binary oracle (the vendor instructions run in an emulator "
         "against our C), see its note; 'wired' = proven functions whose port also runs in the driver (the rest are proven ports "
         "that still wait for the vendor block-state layer).")


def wtxt(r):
    s = "; wired %d" % r["wired"] if r.get("wired") is not None else ""
    if r.get("unreachable"):
        s += "; unreachable %d" % r["unreachable"]
    return s


LINK = ("How the ports were made: success rate of the paid API port loop per model, by project and block size: "
        "https://lu-fi.github.io/openimp/port-loop.html")


def load():
    return json.load(open(DATA, encoding="utf-8")) if os.path.exists(DATA) else None


def bar(n, total, width=20):
    k = int(round(width * n / total)) if total else 0
    return "█" * k + "░" * (width - k)


def rows():
    d = load()
    if not d:
        return None, []
    out = []
    for soc in sorted(d["socs"]):
        e = d["socs"][soc]
        out.append(dict(soc=soc, proven=e["proven"], total=e.get("total"), wired=e.get("wired"), unreachable=e.get("unreachable"), branch=e.get("branch"), date=e.get("date"),
                        note=e.get("note", "")))
    return d, out


RTITLE = "Readable C (driver de-lift, open-tx-isp)"
RINTRO = ("Per SoC: C functions compiled into the open module that are real, readable C and proven (Ingenic SDK source, our own "
          "code, or a recovered function rewritten and proven bit-exact by a de-lift oracle), out of all C functions of the module. "
          "Machine-like = recovered code that still has register-variable names, raw *(T *)(p + 0x..) accesses, goto chains or "
          "magic offsets. 'before' = the module when the de-lift of that SoC started. "
          "'Readable' only says the C is readable and checked by a de-lift oracle or is SDK/own source; it is not a statement about the vendor. "
          "'Vendor-proven' counts the functions whose behaviour was replayed against the vendor binary in an emulator (Unicorn: recorded scenario calls, "
          "DIS-on and general-FSM scenarios, argument fuzz, mutation fuzz) with 0 unexplained differences; a difference that is a documented "
          "beyond-vendor decision (vendor traps) counts as proven. Functions with an open finding are not counted. "
          "'SDK source' = functions whose body is the published Ingenic SDK C (not lifted, not compared: the SDK is the vendor's own source; "
          "'patched' = SDK function changed by us). 'Unreachable' = no vendor entry point reaches the function, nothing to compare. "
          "Counts are over all C functions of the module; the percentages are not rounded up. A dash = vendor proof not measured for that SoC yet.")


def vtxt(r):
    """one-line vendor proof summary (text)"""
    if r.get("vendor_proven") is None:
        return "vendor-proven: not measured"
    return "vendor-proven %d/%d (%.1f %%) · SDK source %d (+%d patched) · unreachable %d" % (
        r["vendor_proven"], r["total"], 100.0 * r["vendor_proven"] / r["total"], r.get("sdk_source", 0), r.get("sdk_patched", 0), r.get("unreachable", 0))


def rrows():
    d = load() or {}
    return [dict(soc=k, **v) for k, v in sorted(d.get("readable", {}).items())]


def rnote(r):
    t = r.get("note") or "inventory only"
    if r.get("issue"):
        t += " (openimp #%s)" % r["issue"]
    if r.get("vendor_basis"):
        t += "; vendor proof: " + r["vendor_basis"]
    return t + ("; as of %s" % r["date"] if r.get("date") else "")


def md_readable():
    rs = rrows()
    if not rs:
        return []
    o = ["### " + RTITLE, "", RINTRO, "", "| SoC | readable / all functions | vendor proof | progress | machine-like lines | note |", "|---|--:|---|---|--:|---|"]
    for r in rs:
        o.append("| %s | %d/%d (%.1f %%; before %d) | %s | `%s` | %.1f %% (before %.1f %%) | %s |" % (
            r["soc"], r["proven"], r["total"], 100.0 * r["proven"] / r["total"], r["base_proven"], vtxt(r), bar(r["proven"], r["total"]),
            r["pct_machine_lines"], r["base_pct_machine_lines"], rnote(r)))
    o.append("")
    return o


def html_readable():
    tr = []
    for r in rrows():
        pct = 100.0 * r["proven"] / r["total"]
        tr.append("<tr><td>%s</td><td class='c'>%d/%d <span class='small'>(%.1f %%, before %d)</span></td>"
                  "<td><div class=\"pb\"><i class=\"b-dev\" style=\"width:%.2f%%\"></i><i class=\"b-gap\" style=\"width:%.2f%%\"></i></div></td>"
                  "<td class='c small'>%s</td><td class='c'>%.1f %% <span class='small'>(before %.1f %%)</span></td><td class='small'>%s</td></tr>" % (
                      html.escape(r["soc"]), r["proven"], r["total"], pct, r["base_proven"], pct, 100 - pct, html.escape(vtxt(r)),
                      r["pct_machine_lines"], r["base_pct_machine_lines"], html.escape(rnote(r))))
    if not tr:
        return ""
    return ('<h3>%s</h3><p class="small">%s</p><table class="mi"><thead><tr><th>SoC</th><th>readable / all functions</th><th>progress</th><th>vendor proof</th>'
            '<th>machine-like lines</th><th>note</th></tr></thead><tbody>%s</tbody></table>' % (html.escape(RTITLE), html.escape(RINTRO), "".join(tr)))


def md():
    d, rs = rows()
    if not d:
        return ""
    o = ["## " + TITLE, "", "<!-- GENERATED by tools/driver_port.py from docs/status/driver_port.json. Do not edit by hand. -->", "",
         INTRO, "", LINK, "", "| SoC | proven / all | progress | note |", "|---|--:|---|---|"]
    for r in rs:
        if r["total"]:
            o.append("| %s | %d/%d (%.1f %%%s) | `%s` | %s; as of %s |" % (r["soc"], r["proven"], r["total"], 100.0 * r["proven"] / r["total"],
                                                                        wtxt(r), bar(r["proven"], r["total"]), r["note"], r["date"]))
        else:
            o.append("| %s | 0 %% (not started) | `%s` | %s |" % (r["soc"], bar(0, 1), r["note"]))
    o.append("")
    o += md_readable()
    return "\n".join(o) + "\n"


def html_card():
    d, rs = rows()
    if not d:
        return ""
    tr = []
    for r in rs:
        if r["total"]:
            pct = 100.0 * r["proven"] / r["total"]
            num = "%d/%d <span class='small'>(%.1f %%%s)</span>" % (r["proven"], r["total"], pct, html.escape(wtxt(r)))
            un = 100.0 * (r.get("unreachable") or 0) / r["total"]
            pb = ('<div class="pb"><i class="b-dev" style="width:%.2f%%" title="%d of %d"></i>%s<i class="b-gap" style="width:%.2f%%"></i></div>'
                  % (pct, r["proven"], r["total"],
                     ('<i class="b-q" style="width:%.2f%%" title="unreachable: no vendor entry reaches it"></i>' % un) if un else "", 100 - pct - un))
            note = "%s (as of %s)" % (r["note"], r["date"])
        else:
            num, note = "0 % <span class='small'>(not started)</span>", r["note"]
            pb = '<div class="pb"><i class="b-gap" style="width:100%"></i></div>'
        tr.append("<tr><td>%s</td><td class='c'>%s</td><td>%s</td><td class='small'>%s</td></tr>" % (
            html.escape(r["soc"]), num, pb, html.escape(note)))
    return ('<section class="card" id="driver-port"><h2>%s</h2><p class="small">%s</p><p class="small">How the ports were made: <a href="port-loop.html">success rate of the paid API port loop per model</a>, by project and block size.</p>'
            '<table class="mi"><thead><tr><th>SoC</th><th>proven / all functions</th><th>progress</th><th>note</th></tr></thead><tbody>%s</tbody></table>%s</section>'
            % (html.escape(TITLE), html.escape(INTRO), "".join(tr), html_readable()))
