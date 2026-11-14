#!/usr/bin/env python3
"""Generate the wiki page Module-Parameters-All.md (every module parameter of every SoC) from an open-tx-isp checkout.

Usage: python3 tools/gen_module_params_wiki.py --out <openimp.wiki checkout> [--repo /path/to/open-tx-isp]
       make mp-wiki WIKI_DIR=<openimp.wiki checkout> [OPEN_TX_ISP=/path/to/open-tx-isp]

The checkout path comes from --repo, else the environment variable OPEN_TX_ISP, else ../open-tx-isp (like
tools/status_driver_port.py). Use a worktree of open-tx-isp origin/next: the page describes the merged state only.
Reads module_param*() and the SoC bring-up macros (*_bringup_param*) in driver/**/*.{c,h,inc} and the MODULE_PARM_DESC texts.
Default = initial value of the variable in the declaration just above the module_param (a few are overwritten at load time;
callback parameters show "see source"). The curated page Module-Parameters is written by hand and is not touched.
"""
import argparse
import os
import re
import sys

MP = re.compile(r'(?<![A-Za-z0-9_])([a-z0-9]+_bringup_)?(?:module_)?param(_named|_cb|_call|_array|_string)?'
                r'\s*\(\s*([A-Za-z0-9_]+)\s*,\s*([^;]*?)\)\s*;', re.S)
DESC = re.compile(r'(?:MODULE_PARM_DESC|[a-z0-9]+_bringup_param_desc)\s*\(\s*([A-Za-z0-9_]+)\s*,\s*'
                  r'((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.S)
DEFINE = re.compile(r'\s*#\s*define\s+([A-Z][A-Z0-9_]+)\s+(\(?[A-Za-z0-9_ +\-\*U()]+\)?)\s*(?:/\*.*)?$')
SOCS = ['t20', 't21', 't23', 't30', 't31', 't32', 't33', 't40', 't41', 'common']


def files(root):
    for dp, _dn, fn in os.walk(root):
        for f in sorted(fn):
            if f.endswith(('.c', '.h', '.inc')):
                yield dp, os.path.join(dp, f)


def find_decls(lines, var):
    out = []
    vr = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(var) + r'(?![A-Za-z0-9_])')
    pat = re.compile(r'^\s*(?:static\s+|const\s+|volatile\s+|unsigned\s+|__initdata\s+)*[A-Za-z_][\w\s\*]*?[\s\*]'
                     + re.escape(var) + r'\s*(?:\[[^\]]*\])?\s*((?:[A-Z][A-Z0-9_]*(?:\([^)]*\))?\s*|'
                     r'__attribute__\(\(.*?\)\)\s*)*)(?:=\s*(.*?))?\s*(;|$)')
    for i, line in enumerate(lines):
        if var not in line or line.lstrip().startswith(('*', '/*', '//', '#')) or not vr.search(line):
            continue
        m = pat.match(line)
        if m and (m.group(2) is not None or line.rstrip().endswith(';')):
            v = m.group(2)
            out.append((i, '0' if v is None else (lines[i + 1].strip() if v == '' else v.strip())))
    return out


def scan(root):
    defs = {}
    for _dp, p in files(root):
        for line in open(p, errors='replace'):
            m = DEFINE.match(line)
            if m:
                defs.setdefault(m.group(1), m.group(2).strip())

    def resolve(v, depth=0):
        v = v.strip()
        return resolve(defs[v], depth + 1) if depth < 4 and v in defs else v

    params, descs = {}, {}
    for dp, p in files(root):
        t = open(p, errors='replace').read()
        lines = t.split('\n')
        soc = os.path.relpath(dp, root).split('/')[0]
        if soc not in SOCS:
            soc = 'common'
        for m in DESC.finditer(t):
            s = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))).replace('\\n', ' ').replace('\\"', '"')
            descs[(m.group(1), soc)] = ' '.join(s.split())
        for m in MP.finditer(t):
            bring, kind, a = bool(m.group(1)), m.group(2) or '', m.group(3)
            rest = [x.strip() for x in m.group(4).split(',')]
            if kind == '_named':
                name, var, typ, perm = a, rest[0], rest[1], (rest[2] if len(rest) > 2 else '?')
            elif kind in ('_cb', '_call'):
                name, var, typ, perm = a, None, 'callback', rest[-1]
            else:
                name, var, typ, perm = a, a, rest[0], (rest[1] if len(rest) > 1 else '?')
            if name.startswith('name') or not re.match(r'^[a-z][a-z0-9_]*$', name):
                continue
            default = 'see source (callback)'
            if var:
                pl = t[:m.start()].count('\n')
                ds = find_decls(lines, var)
                before = [x for x in ds if x[0] <= pl]
                default = resolve((before[-1] if before else ds[0])[1]) if ds else 'see source'
            params.setdefault((soc, name), []).append(dict(bring=bring, type=typ, perm=perm, default=default))
    return params, descs


PERM = {'S_IRUGO': '0444', 'S_IRUGO|S_IWUSR': '0644', 'S_IRUGO|S_IWUSR|S_IWGRP': '0664', '0': '0 (not in sysfs)'}


def norm_default(d, typ):
    d = d.replace('0 (unset)', '0')
    d = {'true': '1', 'false': '0'}.get(d, d)
    d = re.sub(r'\b(\d+)U\b', r'\1', d)
    return 'see source' if len(d) > 60 else d


HEADER = '''# Module parameters: complete list (generated)

Generated from the `module_param*()` declarations and `MODULE_PARM_DESC()` texts of open-tx-isp `next` (%s, `driver/<soc>/`); the curated explanation of the parameters that matter is on [Module Parameters](Module-Parameters). **Default** is the initial value of the variable in the source (the declaration just above the `module_param`); a few parameters are overwritten at load time by SoC code, and callback parameters (`module_param_cb`) show "see source". **Perm** is the sysfs permission (`0` = not in sysfs, `0444` read-only, `0644` writable at run time under `/sys/module/<module>/parameters/`). Parameters marked **bring-up** exist only in a build with the SoC's `TX_ISP_<SOC>_BRINGUP_PARAMS=y` switch (not in firmware images; they are listed by name at the end of each SoC section). T10 is built from the T20 sources. Read-only counters and telemetry (`*_now`, `*_last_*`, `*_hits`, ...) are included. A 3.10 kernel refuses to load a module given an unknown parameter, so only pass names that this list shows for the SoC.
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', required=True, help='openimp.wiki checkout')
    ap.add_argument('--repo', default=os.environ.get('OPEN_TX_ISP', '../open-tx-isp'), help='open-tx-isp checkout (env OPEN_TX_ISP)')
    ap.add_argument('--date', default='', help='date text for the header (default: today)')
    a = ap.parse_args()
    root = os.path.join(a.repo, 'driver')
    if not os.path.isdir(root):
        sys.exit('no driver/ directory in %s (use --repo or OPEN_TX_ISP)' % a.repo)
    import datetime
    date = a.date or datetime.date.today().isoformat()
    params, descs = scan(root)
    rows = {}
    for (soc, name), ents in params.items():
        r = None
        for e in ents:
            d = norm_default(e['default'], e['type'])
            perm = PERM.get(e['perm'].replace(' ', ''), e['perm'])
            if r is None:
                r = dict(name=name, type=e['type'], perm=perm, default=d, bring=e['bring'])
            else:
                if d not in r['default'].split(' / '):
                    r['default'] += ' / ' + d
                r['bring'] = r['bring'] and e['bring']
        r['desc'] = descs.get((name, soc), '')
        rows.setdefault(soc, []).append(r)
    out = [HEADER % date]
    total = 0
    for soc in SOCS:
        lst = sorted(rows.get(soc, []), key=lambda r: r['name'])
        if not lst:
            continue
        title = {'t20': 'T10 / T20 (one driver source tree)', 'common': 'Shared code (all SoCs)'}.get(soc, soc.upper())
        reg = [r for r in lst if not r['bring']]
        br = [r for r in lst if r['bring']]
        out.append('\n## %s\n\n%d parameters in a default build, %d more in a bring-up build.\n' % (title, len(reg), len(br)))
        if reg:
            out.append('| Parameter | Type | Perm | Default | Description |\n|---|---|---|---|---|')
            for r in reg:
                d = r['desc'].replace('|', '/')
                if len(d) > 180:
                    d = d[:177] + '...'
                out.append('| `%s` | %s | %s | %s | %s |' % (r['name'], r['type'], r['perm'], r['default'].replace('|', '/'), d))
                total += 1
        if br:
            out.append('\nBring-up build only: ' + ', '.join('`%s`' % r['name'] for r in br) + '.')
    path = os.path.join(a.out, 'Module-Parameters-All.md')
    open(path, 'w').write('\n'.join(out) + '\n')
    print('%s: %d parameters in default builds' % (path, total))


if __name__ == '__main__':
    main()
