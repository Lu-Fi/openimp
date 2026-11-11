import sys, random, os
sys.argv=[sys.argv[0]]+sys.argv[1:]
seed=int(sys.argv[1]); n=int(sys.argv[2])
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'diff.py')).read().split("if __name__")[0])
import eprc_t33_oracle as O
tot=0; bad=0
for k in range(n):
    r=random.Random(seed*1000+k)
    mode=r.choice([1,2,3,4,5]); kbps=r.choice([256,500,1000,2000,3000,4000,6000,8000])
    w,h=r.choice([(1920,1088),(1280,720),(640,368),(2560,1440),(2304,1296)])
    gop=r.choice([10,25,30,50,60]); fps=r.choice([10,15,20,25,30])
    minqp=r.choice([10,15,20,25]); maxqp=r.choice([38,42,45,48,51]); initqp=r.choice([25,30,35])
    O.W,O.H,O.GOP,O.FPS=w,h,gop,fps
    hh=Harness(mode,kbps,verbose=1,w=w,h=h,gop=gop,fps=fps,minqp=minqp,maxqp=maxqp,initqp=initqp)
    pics=r.choice([80,150,250])
    lvl=[60.0]; still=[0]
    def cpl(i):
        if r.random()<0.03: lvl[0]=r.choice([5.0,20.0,60.0,120.0,250.0])    # scene change
        if r.random()<0.02: still[0]=r.randint(10,60)
        if still[0]>0: still[0]-=1; return 1.0+r.random()
        return max(0.5, lvl[0]*(0.8+0.4*r.random()))
    seq=hh.run(pics,cpl)
    tot+=1; bad+= hh.ndiff>1
    print('scen %d mode %d %dk %dx%d gop %d fps %d qp %d..%d init %d pics %d: calls %d diff %d  qp[-5:] %s'%(k,mode,kbps,w,h,gop,fps,minqp,maxqp,initqp,pics,hh.calls,hh.ndiff,seq[-5:]),flush=True)
print('TOTAL',tot,'bad',bad)
