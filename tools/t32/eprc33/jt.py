import sys,struct,re
exec(open('ro.py').read().split('for spec')[0])
for a in sys.argv[1:]:
    a=eval(a)
    print(hex(a), [hex((struct.unpack('<i',rd(a+4*i,4))[0]+0x219aa0)&0xfffffffe) for i in range(6)])
