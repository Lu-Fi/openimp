import re,struct,subprocess,sys
from elftools.elf.elffile import ELFFile
L='/mnt/NVMe/git/scratch-clones/session-110bf918/audit/issue295/gh/libimp-2.2.0.so'
OD='/mnt/NVMe/git/thingino-out/abus_ppic90520_t30n_imx307_xr871-abus/host/bin/mipsel-linux-objdump'
GP=0x209aa0
f=open(L,'rb'); e=ELFFile(f); segs=[(s['p_vaddr'],s.data()) for s in e.iter_segments() if s['p_type']=='PT_LOAD']
def r32(a):
    for v,d in segs:
        if v<=a<v+len(d)-3: return struct.unpack_from('<I',d,a-v)[0]
    raise KeyError(hex(a))
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
txt=subprocess.run([OD,'-d','--no-show-raw-insn','--start-address=%d'%lo,'--stop-address=%d'%hi,L],capture_output=True,text=True).stdout
ins=[]
for l in txt.splitlines():
    m=re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)',l)
    if m: ins.append((int(m.group(1),16),m.group(2),[x.strip() for x in m.group(3).split('<')[0].split(',')] if m.group(3) else []))
out=[]
for k,(a,mn,ops) in enumerate(ins):
    if mn!='jr' or ops[0]=='ra': continue
    reg=ops[0]; win=ins[max(0,k-40):k]
    def defn(r, before):
        for j in range(len(before)-1,-1,-1):
            b=before[j]
            if b[2] and b[2][0]==r and b[1] not in ('sw','sb','sh','beqz','bnez','beq','bne','swc1','sdc1'): return j,b
        return None,None
    j,d=defn(reg,win)
    ok=False
    try:
        assert d[1]=='addu' and 'gp' in d[2][1:]
        x=[o for o in d[2][1:] if o!='gp'][0]
        j2,d2=defn(x,win[:j]); assert d2[1]=='lw'
        m=re.match(r'(-?\d+)\((\w+)\)',d2[2][1]); assert m.group(1)=='0'
        j3,d3=defn(m.group(2),win[:j2]); assert d3[1]=='addu'
        tb=None;idx=None
        for o in d3[2][1:]:
            j4,d4=defn(o,win[:j3])
            if d4 and d4[1]=='addiu':
                j5,d5=defn(d4[2][1],win[:j4]); m5=re.match(r'(-?\d+)\(gp\)',d5[2][1])
                tb=(r32(GP+int(m5.group(1)))+int(d4[2][2]))&0xffffffff
            elif d4 and d4[1]=='sll': idx=d4[2][1]
        assert tb
        n=None
        for b in reversed(win[:j3]):
            if b[1]=='sltiu' and b[2][1]==idx: n=int(b[2][2],0); break
        if n is None:
            n=0
            while n<200 and lo<= (r32(tb+4*n)+GP)&0xffffffff <hi: n+=1
        tg=[(r32(tb+4*i)+GP)&0xffffffff for i in range(n)]
        out.append((a,tg)); ok=True
    except Exception as ex:
        print('FAIL %x %s'%(a,ex),file=sys.stderr)
for a,tg in out: print('%x %s'%(a,' '.join('%x'%t for t in tg)))
