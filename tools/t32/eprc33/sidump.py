#!/usr/bin/env python3
# sidump.py LIB START END [LABELS]: a vendor .data / .rodata range as a C byte table (16 per row), with the
# names of the objects that start in it (LABELS: got.txt-style "... ADDR ... OBJECT n NAME" lines) as comments.
# Used for the slice-block tables of src/eprc/eprc_t33_h265.c (t65_si_data / t65_si_ro).
import sys
from elftools.elf.elffile import ELFFile
lib, a0, a1 = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
names = {}
if len(sys.argv) > 4:
    for l in open(sys.argv[4]):
        f = l.split()
        if len(f) >= 7 and f[4] == 'OBJECT':
            names[int(f[2], 16)] = f[6]
f = ELFFile(open(lib, 'rb'))
mem = {}
for s in f.iter_sections():
    if s['sh_type'] == 'SHT_PROGBITS' and s['sh_addr'] <= a0 and a1 <= s['sh_addr'] + s['sh_size']:
        d = s.data()[a0 - s['sh_addr']:a1 - s['sh_addr']]
        break
else:
    sys.exit('range not in one section')
a = a0
row = []
def flush():
    global row
    if row:
        print('    ' + ' '.join('%d,' % x for x in row))
        row = []
while a < a1:
    if a in names:
        flush(); print('    /* %#x %s */' % (a, names[a]))
    row.append(d[a - a0]); a += 1
    if len(row) == 16 or (a in names):
        flush()
flush()
