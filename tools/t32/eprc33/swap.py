# build a swap oracle: gen264 minus the functions given, wired to the hand versions
import sys, subprocess
W='/mnt/NVMe/git/scratch-clones/session-110bf918/wt-eprc33'
import os
M = dict(l.split() for l in open('swapmap.txt') if l.strip() and not l.startswith('#'))
subprocess.check_call(['python3','g2c.py','h264p.c','got.txt','genx.c','', ','.join(M)])
with open('swap.c','w') as f:
    f.write('#include "eprc/eprc_t33_mem.h"\n#include "e33oem.h"\nstatic uint32_t e33_watch;\n')
    P = dict(l.split() for l in open('pendmap.txt') if l.strip() and not l.startswith('#'))
    gx = open('genx.c').read()
    for h, g in P.items():
        proto = [l for l in gx.split('\n') if l.startswith('static') and (' %s(' % g) in l][0]
        f.write(proto + '\n#define %s %s\n' % (h, g))
    f.write('#include "%s/src/eprc/eprc_t33_h264.c"\n' % W)
    for g, h in M.items():
        f.write('#define %s(X, ...) %s(X, __VA_ARGS__)\n' % (g, h))
    f.write('#include "genx.c"\n')
    f.write(open('t.c').read().split('#include "gen264.c"')[1])
subprocess.check_call(['gcc','-O1','-fwrapv','-ffp-contract=off','-w','-shared','-fPIC','-I'+W+'/src','-I.','swap.c','-o',__import__('os').environ.get('OUT','libswap.so'),'-lm'])
print('built libswap.so with', len(M), 'hand functions')
