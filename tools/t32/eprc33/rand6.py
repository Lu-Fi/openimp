# SMART/AVBR table-index scenarios (VPI_set_sliceinfo): the SMART rule / level (e+0x84a/0x84b ->
# inst+0x7d/0x7e -> pic+0x22/0x23) select a 0x30-byte row of cnst_ai_motion_smd_ofst (vendor .data
# 0x200f6c, 384 bytes = 8 rows: rule 1..2 x level 0..3), the motion step st+0xa9 (-> pic+0x2e) a
# 0x24-byte row of cnst_deadzone (0x200e74, 216 bytes = 6 rows: 0..5).
# i264e_ratecontrol_start copies P+0x26c/0x26d into e+0x84a/0x84b before every FRAME_START (when the
# still map P+0xb40 is on, else 0), after the VIDEO_CFG clamp; the vendor's only writers of P+0x26c/0x26d
# are i264e_param_default and i264e_reconfig from i264e_reconfig_qpgai_set, which clamps 0..2 / 0..3.
#   group A (argv[5] = A): rule 0..2 / level 0..3 in P, out-of-range values at VIDEO_CFG entry, the
#     motion mode cur+0x7d4 (X+0x164) 0..71 (st+0xa9 <= 71/3 + 4: rows inside the copied .data);
#     must give 0 differing calls.
#   group B (argv[5] = B, exploratory, not in reg.sh): P+0x26c/0x26d beyond the setter's clamp and
#     motion modes 72..127: rows past the copied .data (the vendor reads the next .data objects,
#     then .got / .bss); differences expected.
import sys, random, os, struct
seed=int(sys.argv[1]); n=int(sys.argv[2])
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'diff.py')).read().split("if __name__")[0])
import eprc_t33_oracle as O
tot=0; bad=0
K0=int(sys.argv[3]) if len(sys.argv)>3 else 0
VB=int(sys.argv[4]) if len(sys.argv)>4 else 1
GRP=sys.argv[5] if len(sys.argv)>5 else 'A'
BIG=[0,1,2,3,4,5,9,60,200,255]
for k in range(K0,K0+n):
    r=random.Random(seed*1000+k)
    mode=r.choice([3,5]); kbps=r.choice([256,1000,4000,8000,20000])
    w,h=r.choice([(1280,720),(640,368),(352,288)])
    gop=r.choice([10,25,50]); fps=r.choice([15,25,30])
    minqp=r.choice([10,15,20,25]); maxqp=r.choice([38,45,51]); initqp=r.choice([-1,25,35])
    ex={0x25c:('B',1),0x260:('I',r.choice([0,HEAP+0x30000])),0x26c:('B',r.choice([0,1,2] if GRP=='A' else [3,4,5,9,60,255])),0x26d:('B',r.choice([0,1,2,3] if GRP=='A' else [3,4,7,20,255])),
        0x168:('B',r.choice([0,1])),0x264:('I',r.choice([0,8,40]))}
    O.W,O.H,O.GOP,O.FPS=w,h,gop,fps
    rule,lvl_=r.choice(BIG),r.choice(BIG)
    cfgp=r.random()<0.6
    mot=r.choice([0,1,2,3,4,5,8,40,71]) if GRP=='A' else r.choice([72,100,127])
    still=r.random()<0.8
    seen=set()
    rev=r.choice([6,6,7])
    def poke(name,H):
        L=H.L; e=PB+0x360; cur=e+0x34
        if name=='JZ_VPU_RC_VIDEO_CFG_H264_T33' and cfgp:
            L.wr(e+0x84a,bytes([rule])); L.wr(e+0x84b,bytes([lvl_]))
        elif name=='JZ_VPU_RC_FRAME_START_H264_T33':
            L.wr(cur,bytes([rev]))
            L.wr(cur+0x7d4,bytes([mot]))
            seen.add((L.rd(e+0x84a,1)[0],L.rd(e+0x84b,1)[0]))
        elif name=='JZ_VPU_RC_FRAME_END_H264_T33' and still:
            # the still map for the next picture (i264e copies P+0x26c/0x26d only with P+0xb40 on)
            L.w32(PB+0xb3c,HEAP+0x30000); L.wr(PB+0xb40,b'\x01')
            L.wr(HEAP+0x30000,bytes(r.randint(0,3) for _ in range(0x4000)))
    class H6(Harness):
        def __init__(self,*a,**kw):
            self.poke=poke; Harness.__init__(self,*a,**kw)
    hh=H6(mode,kbps,verbose=VB,w=w,h=h,gop=gop,fps=fps,minqp=minqp,maxqp=maxqp,initqp=initqp,extra=ex)
    hh.L.wr(hh.t.X+0x164,bytes([mot,r.choice([0,1])]))
    pics=r.choice([40,80])
    hh.rich=random.Random(seed*7+k)
    lvl=[60.0]
    def cpl(i):
        if r.random()<0.05: lvl[0]=r.choice([5.0,20.0,60.0,120.0,250.0])
        return max(0.5, lvl[0]*(0.8+0.4*r.random()))
    try:
        seq=hh.run(pics,cpl)
    except Exception as ex_:
        from unicorn.mips_const import UC_MIPS_REG_PC
        print('scen %d EXC %r pc %x in %s'%(k,ex_,hh.L.uc.reg_read(UC_MIPS_REG_PC),hh.pending and hh.pending[0])); seq=[]
    tot+=1; bad+= hh.ndiff>0
    e=PB+0x360
    print('scen %d grp %s mode %d %dk %dx%d rev %d rule/level P %d/%d cfg %s -> e %s mot %d pics %d: calls %d diff %d  qp[-5:] %s'%(
        k,GRP,mode,kbps,w,h,rev,ex[0x26c][1],ex[0x26d][1],(rule,lvl_) if cfgp else '-',sorted(seen),
        mot,pics,hh.calls,hh.ndiff,seq[-5:]),flush=True)
print('TOTAL',tot,'bad',bad)
