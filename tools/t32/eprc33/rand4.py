# FRAME_START / FRAME_END / VIDEO_CFG coverage scenarios: rand3.py plus
#  - fixed QP (mode 0), forced IDR periods that differ from the GOP (P pictures at GOP position 0/1)
#  - small still-map blocks (+0x264 -> P+0xb41), class counts (P+0xb42)
#  - the motion-level knobs the encoder hands over per picture (X+0x164/0x165/0x168 -> cur+0x7d4/0x7d5/0x7d8)
#  - out-of-range configuration values at VIDEO_CFG (the clamps)
# Pokes are written at function entry before the snapshot, so both sides get the same input.
import sys, random, os, struct
seed=int(sys.argv[1]); n=int(sys.argv[2])
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'diff.py')).read().split("if __name__")[0])
import eprc_t33_oracle as O
tot=0; bad=0
K0=int(sys.argv[3]) if len(sys.argv)>3 else 0
VB=int(sys.argv[4]) if len(sys.argv)>4 else 1
# VIDEO_CFG fields of e (offset, struct format, candidate values)
CFGF=[(0x10,'I',[0,70000]),(0x18,'I',[0,300]),(0x14,'I',[0,500]),(0x1c,'B',[60]),(0x1d,'B',[55]),
      (0xc,'B',[3]),(0x1e,'B',[2]),(0x826,'B',[9]),(0x825,'b',[-5,60,0,40]),(0x827,'B',[0,1,3]),(0x828,'B',[1,4]),
      (0x829,'B',[7]),(0x82c,'B',[60]),(0x82d,'B',[70]),(0x82e,'B',[99]),(0x834,'I',[0,12,3000]),(0x838,'I',[0,1,0x20000]),
      (0x83c,'I',[0,0x30000]),(0x840,'B',[0,20,120,200]),(0x841,'B',[0,150,90]),(0x842,'B',[0,200,5]),(0x843,'B',[0,9,60]),
      (0x844,'B',[9]),(0x845,'b',[-20,20,5]),(0x846,'H',[0,1200,200,60]),(0x847,'B',[0,60]),(0x848,'H',[0,2000]),
      (0x84a,'B',[0,60]),(0x84b,'B',[0,60]),(0x850,'i',[0,70,-3,40]),(0x851,'b',[-3,40]),(0x854,'b',[-3,40]),
      (0x855,'b',[-3,40]),(0x856,'b',[-3,40]),(0x859,'B',[5]),(0x85c,'i',[0xc350001+5]),(0x860,'i',[0xc350001+5]),
      (0x899,'B',[5]),(0x89c,'I',[100]),(0x8a0,'I',[0x20000]),(0x8a8,'I',[0xffffffff]),(0x8ac,'I',[0xffffffff])]
for k in range(K0,K0+n):
    r=random.Random(seed*1000+k)
    mode=r.choice([0,1,2,3,4,5,3,5]); kbps=r.choice([256,500,1000,2000,4000,8000,20000,45000,80000])
    w,h=r.choice([(1920,1088),(1280,720),(640,368),(2560,1440),(2304,1296)])
    gop=r.choice([1,2,10,25,30,50,60,120]); fps=r.choice([5,10,15,25,30,60])
    minqp=r.choice([10,15,20,25]); maxqp=r.choice([38,42,45,48,51]); initqp=r.choice([-1,25,30,35])
    ex={}
    if r.random()<0.8:
        ex[0x25c]=('B',1); ex[0x260]=('I',r.choice([0,HEAP+0x30000,HEAP+0x30000]))
        ex[0x26c]=('B',r.choice([0,1,1,2])); ex[0x26d]=('B',r.randint(0,7))
        ex[0x168]=('B',r.choice([0,1,1])); ex[0x264]=('I',r.choice([0,8,40,w//8,w//6,w//10,w//4]))
    if r.random()<0.4:       # ROI regions (P+0x1c0.. -> cur+0x3ac.., 16 x 9 bytes: on, mode, QP)
        for i in range(16):
            if r.random()<0.3:
                ex[0x1c2+9*i]=('B',1); ex[0x1c3+9*i]=('B',r.choice([0,1,2,3,4]))
                ex[0x1c4+9*i]=('b',r.choice([-20,-5,0,3,10,30,51,60]))
    if r.random()<0.7: ex[0x138]=('i',r.choice([0,1,2,3,4,5,7,8]))   # -> inst+0x18
    O.W,O.H,O.GOP,O.FPS=w,h,gop,fps
    hh=Harness(mode,kbps,verbose=VB,w=w,h=h,gop=gop,fps=fps,minqp=minqp,maxqp=maxqp,initqp=initqp,extra=ex)
    # forced IDR period (the encoder restarts Hh+0); differs from the GOP in some scenarios
    idr=r.choice([gop,gop,gop*2,max(1,gop//2),gop+3,max(1,gop-1)]) if gop > 2 else gop   # GOP 1-2: the vendor asserts on P pictures
    hh.t.gop=idr
    X=hh.t.X
    mv=[r.choice([0,1,2,3,4]), r.choice([0,1]), r.choice([0,1,2,3,5,8,-1])]
    hh.L.wr(X+0x164,bytes([mv[0],mv[1],r.randint(0,3)])); hh.L.wr(X+0x168,struct.pack('<i',mv[2]))
    if r.random()<0.5:       # intra refresh (X+0x151.. -> cur+0x7bc..0x7cc)
        hh.L.wr(X+0x151,bytes([r.choice([0,1,1]),r.choice([0,1]),r.choice([0,1])]))
        hh.L.wr(X+0x154,struct.pack('<iii',r.choice([0,1,2,3,5,12,40]),r.choice([0,1,2,3,8,20]),r.choice([0,1,2,3,10,100,70000])))
    pk30=r.choice([0,0,0,0.05,0.2]); pk53=r.choice([0,1])
    ncls=r.choice([4,4,1,2,3,5,6,7,8,9,12,16])
    cfgp=[f for f in CFGF if r.random()<0.25] if r.random()<0.5 else []
    cfgp=[(o,fm,r.choice(v)) for o,fm,v in cfgp]
    p398=r.choice([0,0,0.05,0.15])
    def poke(name,H):
        L=H.L; e=H.t.P+0x360
        if name=='JZ_VPU_RC_VIDEO_CFG_H264_T33':
            for o,fm,v in cfgp: L.wr(e+o,struct.pack('<'+fm,v))
        elif name=='JZ_VPU_RC_FRAME_START_H264_T33':
            if ncls!=4: L.wr(H.t.P+0xb42,bytes([ncls]))
            if pk30:     # the encoder's per-picture flag e+0x30 (+ e+0x53); 0 otherwise
                on=r.random()<pk30
                L.wr(e+0x30,bytes([on])); L.wr(e+0x53,bytes([pk53 if on else 0]))
            if r.random()<0.05:      # motion knobs change at run time
                L.wr(X+0x164,bytes([r.choice([0,1,2,3]),r.choice([0,1])]))
                L.wr(H.t.P+0xb68,L.rd(X+0x164,2))
        elif name=='JZ_VPU_RC_FRAME_END_H264_T33':
            if p398 and r.random()<p398:
                st=L.r32(e+0x868); L.w32(st+0x398,0)
    hh.poke=poke
    pics=r.choice([80,150,250])
    hh.rich=random.Random(seed*7+k)
    lvl=[60.0]; still=[0]; rchg=r.random()<0.5
    def cpl(i):
        if rchg and i>2 and r.random()<0.03:
            nk=r.choice([256,1000,4000,8000,20000,45000,80000])
            hh.L.w32(hh.t.P + 0x110, nk); hh.kbps=nk
        if r.random()<0.03: lvl[0]=r.choice([5.0,20.0,60.0,120.0,250.0])
        if r.random()<0.03: still[0]=r.randint(10,60)
        if still[0]>0: still[0]-=1; return 1.0+r.random()
        return max(0.5, lvl[0]*(0.8+0.4*r.random()))
    try:
        seq=hh.run(pics,cpl)
    except Exception as ex_:
        print('scen %d EXC %r'%(k,ex_)); seq=[]
        hh.ndiff+=0
    tot+=1; bad+= hh.ndiff>0
    print('scen %d mode %d %dk %dx%d gop %d idr %d fps %d qp %d..%d init %d mv %s ncls %d cfg %d p398 %s pics %d: calls %d diff %d  qp[-5:] %s'%(k,mode,kbps,w,h,gop,idr,fps,minqp,maxqp,initqp,mv,ncls,len(cfgp),p398,pics,hh.calls,hh.ndiff,seq[-5:]),flush=True)
print('TOTAL',tot,'bad',bad)
