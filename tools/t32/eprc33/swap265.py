#!/usr/bin/env python3
# swap265.py: build the H.265 swap oracle (run in the local work directory, E265DIR): the generated
# controller (h265p.c -> g2c.py) minus the functions of swapmap265.txt, which come from
# src/eprc/eprc_t33_h265.c instead; t265.c gives the e33_call tail, e265oem.h the OEM helpers.
import os, subprocess, sys
W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
T = os.path.dirname(os.path.abspath(__file__))
os.chdir(os.environ.get('E265DIR', '.'))
M = dict(l.split() for l in open('swapmap265.txt') if l.strip() and not l.startswith('#'))
SKIP = 'librc_h264_get_version,md_cfg_test,tfm_cfg_test,mce_cfg_test'
subprocess.check_call(['python3', os.path.join(T, 'g2c.py'), 'h265p.c', 'got.txt', 'genx265.c', SKIP, ','.join(M)],
                      stderr=subprocess.DEVNULL)
with open('swap265.c', 'w') as f:
    f.write('#include "eprc/eprc_t33_mem.h"\n#include "e265oem.h"\nstatic uint32_t e33_watch;\n')
    # generated helpers the hand code still calls (pendmap265.txt: hand name -> generated name)
    P = dict(l.split() for l in open('pendmap265.txt') if l.strip() and not l.startswith('#'))
    gx = open('genx265.c').read()
    for h, g in P.items():
        proto = [l for l in gx.split('\n') if l.startswith('static') and (' %s(' % g) in l][0]
        f.write(proto + '\n#define %s %s\n' % (h, g))
    f.write('#include "%s/src/eprc/eprc_t33_h265.c"\n' % W)
    for g, h in M.items():
        f.write('#define %s(X, ...) %s(X, __VA_ARGS__)\n' % (g, h))
    f.write('#include "genx265.c"\n')
    f.write(open('t265.c').read().split('#include "gen265.c"')[1])
out = os.environ.get('OUT', 'libswap265.so')
subprocess.check_call(['gcc', '-O1', '-fwrapv', '-ffp-contract=off', '-w', '-shared', '-fPIC', '-I' + W + '/src', '-I.',
                       'swap265.c', '-o', out, '-lm'])
print('built %s with %d hand functions' % (out, len(M)))
