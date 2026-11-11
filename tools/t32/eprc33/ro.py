import sys,struct
from elftools.elf.elffile import ELFFile
f=open('/mnt/NVMe/git/scratch-clones/session-110bf918/audit/issue295/gh/libimp-2.2.0.so','rb'); e=ELFFile(f)
segs=[(s['p_vaddr'],s.data()) for s in e.iter_segments() if s['p_type']=='PT_LOAD']
def rd(a,n):
    for v,d in segs:
        if v<=a and a+n<=v+len(d): return d[a-v:a-v+n]
for spec in sys.argv[1:]:
    t,a,n=spec.split(':'); a=eval(a); n=int(n)
    fmt={'f':'<f','d':'<d','b':'<b','B':'<B','h':'<h','H':'<H','i':'<i','I':'<I'}[t]
    sz=struct.calcsize(fmt)
    print(spec, [struct.unpack(fmt,rd(a+i*sz,sz))[0] for i in range(n)])
