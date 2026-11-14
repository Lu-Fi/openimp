import re,sys
for fn in sys.argv[1:]:
    t9=None
    for l in open(fn):
        m=re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)',l)
        if not m: continue
        a,mn,ops=m.groups()
        if mn=='lw' and ops.startswith('t9,'):
            t9=ops.split(',')[1]
        if mn=='jalr' and t9 in ('-31344(gp)','-29968(gp)','-27040(gp)'):
            print(a)
