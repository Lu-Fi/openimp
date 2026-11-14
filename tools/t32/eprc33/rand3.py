# coverage scenarios: rand.py + rand_rate.py plus the knobs the I/P QP paths read
# (GOP 1, no initial QP, high frame rates, SMART/AVBR still-scene settings)
import sys, random, os
seed=int(sys.argv[1]); n=int(sys.argv[2])
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'diff.py')).read().split("if __name__")[0])
import eprc_t33_oracle as O
tot=0; bad=0
K0=int(sys.argv[3]) if len(sys.argv)>3 else 0
VB=int(sys.argv[4]) if len(sys.argv)>4 else 1
for k in range(K0,K0+n):
    r=random.Random(seed*1000+k)
    mode=r.choice([1,2,3,4,5,3,5]); kbps=r.choice([256,500,1000,2000,4000,8000,20000,45000,80000])
    w,h=r.choice([(1920,1088),(1280,720),(640,368),(2560,1440),(2304,1296)])
    gop=r.choice([1,10,25,30,50,60,120]); fps=r.choice([5,10,15,25,30,60])
    minqp=r.choice([10,15,20,25]); maxqp=r.choice([38,42,45,48,51]); initqp=r.choice([-1,25,30,35])
    ex={}
    if r.random()<0.7:
        ex[0x25c]=('B',1); ex[0x260]=('I',r.choice([0,HEAP+0x30000,HEAP+0x30000]))
        ex[0x26c]=('B',r.choice([0,1,1,2])); ex[0x26d]=('B',r.randint(0,7))
        ex[0x168]=('B',r.choice([0,1,1])); ex[0x264]=('I',r.choice([0,0,8,16,40]))
    if r.random()<0.7: ex[0x138]=('i',r.choice([0,1,2,3,4,5,7,8]))   # -> inst+0x18
    O.W,O.H,O.GOP,O.FPS=w,h,gop,fps
    hh=Harness(mode,kbps,verbose=VB,w=w,h=h,gop=gop,fps=fps,minqp=minqp,maxqp=maxqp,initqp=initqp,extra=ex)
    pics=r.choice([80,150,250])
    hh.rich=random.Random(seed*7+k)
    lvl=[60.0]; still=[0]; rchg=r.random()<0.4
    def cpl(i):
        if rchg and i>2 and r.random()<0.02:
            nk=r.choice([256,1000,4000,8000,20000,45000,80000])
            hh.L.w32(hh.t.P + 0x110, nk); hh.kbps=nk
        if r.random()<0.03: lvl[0]=r.choice([5.0,20.0,60.0,120.0,250.0])
        if r.random()<0.03: still[0]=r.randint(10,60)
        if still[0]>0: still[0]-=1; return 1.0+r.random()
        return max(0.5, lvl[0]*(0.8+0.4*r.random()))
    seq=hh.run(pics,cpl)
    tot+=1; bad+= hh.ndiff>0
    print('scen %d mode %d %dk %dx%d gop %d fps %d qp %d..%d init %d ex %s pics %d: calls %d diff %d  qp[-5:] %s'%(k,mode,kbps,w,h,gop,fps,minqp,maxqp,initqp,{hex(a):v[1] for a,v in ex.items()},pics,hh.calls,hh.ndiff,seq[-5:]),flush=True)
print('TOTAL',tot,'bad',bad)
