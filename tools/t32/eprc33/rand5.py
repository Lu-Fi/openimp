# slice-block (VPI_set_sliceinfo / get_bu_target_bits) coverage scenarios: rand4.py-like
# streams plus the stream knobs of the slice block, poked at FRAME_START entry before the
# snapshot (both sides get the same input):
#  - the HW revision e+0x34 (cur+0, 6 = T32 HERA, 7 = the other T33 path)
#  - intra refresh ring cur+0x7b0..0x7b8, second reference cur+0x7b4, cur+0x7ae,
#    SMART bit map cur+0x43c / +0x440, buffer addresses cur+0x44c..0x470, last IDR slice
#    sizes cur+0x21c / +0x228.. / +0x328, the work-buffer flag vb+0xa4
import sys, random, os, struct
seed=int(sys.argv[1]); n=int(sys.argv[2])
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'diff.py')).read().split("if __name__")[0])
import eprc_t33_oracle as O
tot=0; bad=0
K0=int(sys.argv[3]) if len(sys.argv)>3 else 0
VB=int(sys.argv[4]) if len(sys.argv)>4 else 1
for k in range(K0,K0+n):
    r=random.Random(seed*1000+k)
    mode=r.choice([1,2,3,4,5,3,5,0]); kbps=r.choice([256,1000,4000,8000,20000,45000])
    w,h=r.choice([(1920,1088),(1280,720),(640,368),(2560,1440),(352,288)])
    gop=r.choice([10,25,30,50]); fps=r.choice([10,15,25,30])
    minqp=r.choice([10,15,20,25]); maxqp=r.choice([38,42,45,51]); initqp=r.choice([-1,25,30,35])
    ex={}
    if r.random()<0.6:
        ex[0x25c]=('B',1); ex[0x260]=('I',r.choice([0,HEAP+0x30000]))
        ex[0x26c]=('B',r.choice([0,1,2])); ex[0x26d]=('B',r.randint(0,7))
        ex[0x168]=('B',r.choice([0,1])); ex[0x264]=('I',r.choice([0,8,16,40]))
    if r.random()<0.4:
        for i in range(16):
            if r.random()<0.3:
                ex[0x1c2+9*i]=('B',1); ex[0x1c3+9*i]=('B',r.choice([0,1,2]))
                ex[0x1c4+9*i]=('b',r.choice([-40,-5,0,3,10,30,51,60]))
    O.W,O.H,O.GOP,O.FPS=w,h,gop,fps
    rev=r.choice([6,6,7,7,7])   # another value: the vendor runs away in get_bu_target_bits (no basic units)
    kn={}
    if r.random()<0.6:   # intra refresh ring / second reference / ROI map / SMART map
        kn[0x7b0]=r.choice([0,1,1]); kn[0x7b1]=r.choice([0,1,2,3]); kn[0x7b2]=r.choice([0,1])
        kn[0x7b3]=r.choice([0,1]); kn[0x7b4]=r.choice([0,1,2,3]); kn[0x7b5]=r.choice([0,1])
    kn[0x7ae]=r.choice([0,1,5])   # cur+0x7ac (ROI map) comes from the SMART flow: it also starts the still vote
    kn[0x43c]=r.choice([0,0,1])
    ring=r.random()<0.5; sizes=r.random()<0.5; a4=r.choice([0,0,0.2])
    hh=Harness(mode,kbps,verbose=VB,w=w,h=h,gop=gop,fps=fps,minqp=minqp,maxqp=maxqp,initqp=initqp,extra=ex)
    idr=r.choice([gop,gop,max(1,gop//2),gop+3])
    hh.t.gop=idr
    def poke(name,H):
        if name!='JZ_VPU_RC_FRAME_START_H264_T33':
            return
        L=H.L; e=H.t.P+0x360; cur=e+0x34
        L.wr(cur,bytes([rev]))
        for o,v in kn.items():
            L.wr(cur+o,bytes([v if r.random()<0.9 else r.choice([0,1,2])]))
        L.w32(cur+0x7b8,r.randint(0,0xffffffff))
        L.w32(cur+0x440,HEAP+0x30000)
        if ring:
            for o in range(0x44c,0x474,4): L.w32(cur+o,r.randint(0,0x7fffffff))
            L.w32(cur+0x330,r.randint(0,0xffffffff)); L.w32(cur+0x334,r.randint(0,0xffffffff))
        if sizes:
            L.w32(cur+0x21c,r.choice([0,r.randint(1,1<<24)]))
            L.w32(cur+0x328,r.choice([0,r.randint(1,1<<24),0x80000000|r.randint(0,1<<20)]))
            for i in range(0x40): L.w32(cur+0x228+4*i,r.randint(0,1<<20))
        if a4 and r.random()<a4:
            L.wr(L.r32(e+0x88c)+0xa4,b'\x01')
    hh.poke=poke
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
    print('scen %d mode %d %dk %dx%d gop %d idr %d rev %d kn %s ring %d sizes %d pics %d: calls %d diff %d  qp[-5:] %s'%(k,mode,kbps,w,h,gop,idr,rev,kn,ring,sizes,pics,hh.calls,hh.ndiff,seq[-5:]),flush=True)
print('TOTAL',tot,'bad',bad)
