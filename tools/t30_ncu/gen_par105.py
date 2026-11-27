# Mechanical transform of the Ghidra decompile of vendor libimp 1.0.5 ncu_func_par_download
# (unaligned-access patched analysis copy) into host C.  Usage: gen105.py in.c [--repo]
import re, sys
src = open(sys.argv[1]).read()
repo = '--repo' in sys.argv
body = src[src.index('{', src.index('ncu_func_par_download(')) + 1:src.rindex('}')]
decl, rest = [], []
for l in body.split('\n'):
    if re.match(r'^\s+(int|uint|undefined\d?|bool|code|char|byte|ushort|short)\s+\**[A-Za-z_0-9]+;\s*$', l):
        decl.append(l)
    else:
        rest.append(l)
t = '\n'.join(rest)
# logging only (flags bits 1/2 select it in the vendor, no register effect)
t = re.sub(r'\(\*\(code \*\)PTR_imp_log_fun_001a9a54\)[^;]*;', '', t)
t = re.sub(r'\w+ = \(\*\(code \*\)PTR_IMP_Log_Get_Option_001a9484\)\(\);', '', t)
t = re.sub(r'\w+ = PTR_UINT_001a8a00( \+ 0x[0-9a-f]+)?;', '', t)
t = t.replace('puVar2 = PTR_write_reg_001a8b54;', '').replace('puVar1 = PTR_DAT_001a89cc;', '')
t = re.sub(r'pcVar104 = \*\(code \*\*\)puVar2;', '', t)
t = t.replace('(*pcVar104)(', 'W(').replace('(**(code **)puVar2)(', 'W(')
t = t.replace('(**(code **)PTR_read_reg_001a9548)(', 'R(')
t = t.replace('(*(code *)PTR_ncu_func_linear_insert_001a97e8)', 'LIN')
t = t.replace('(*(code *)PTR_ncu_func_non_linear_insert_001a8cb0)', 'NLIN')
t = re.sub(r'puVar1 = PTR_ncu_image_size_height_001a9258;', '', t)
t = re.sub(r'puVar91 = PTR_ncu_image_size_width_001a9c60;', '', t)
t = t.replace('*(int *)puVar1', 'ncu_img_h').replace('*(int *)puVar91', 'ncu_img_w')
t = re.sub(r'puVar50 = \*\(uint \*\*\)\(PTR_DAT_001a89cc \+ 0x2c40\);', '', t)
t = re.sub(r'puVar50\[(0x[0-9a-f]+|\d+)\]', lambda m: 'P(%s)' % hex(4 * int(m.group(1), 0)), t)
t = t.replace('*puVar50', 'P(0)')
# divisions: the vendor traps on a zero divisor (break 7); the port returns -1 instead
t = re.sub(r'trap\(7\);', 'return -1;', t)
t = re.sub(r'\b(iVar\d+|uVar\d+|\(int\)uVar\d+) / (\(int\)uVar\d+|iVar\d+|uVar\d+)\b(?! \* )',
           lambda m: '(%s ? %s / %s : 0)' % (m.group(2), m.group(1), m.group(2)) if m.group(2) not in ('2',) else m.group(0), t)
t = re.sub(r'\b(iVar\d+|uVar\d+) % (\(int\)uVar\d+|iVar\d+|uVar\d+)\b',
           lambda m: '(%s ? %s %% %s : 0)' % (m.group(2), m.group(1), m.group(2)), t)
parts = re.split(r'(?<=;)', t)
alias = set(); res = []
for st in parts:
    m = re.search(r'(iVar\d+) = \*\(int \*\)\(puVar1 \+ 0x2c40\);', st)
    if m:
        alias.add(m.group(1)); res.append(st.replace(m.group(0), '')); continue
    for a in list(alias):
        st = re.sub(r'\*\((?:undefined4|uint|int) \*\)\(' + a + r' \+ (0x[0-9a-f]+|\d+)\)',
                    lambda mm: 'P(%s)' % hex(int(mm.group(1), 0)), st)
        st = re.sub(r'\*\((?:undefined4|uint|int) \*\)' + a + r'\b', 'P(0)', st)
    for a in list(alias):
        if re.search(r'\b' + a + r'\s*=[^=]', st):
            alias.discard(a)
    res.append(st)
t = ''.join(res)
t = re.sub(r'\n\s*\n', '\n', t)
d = '\n'.join(decl)
for a, b in (('undefined4 ', 'uint32_t '), ('uint ', 'uint32_t '), ('int ', 'int32_t '), ('undefined *', 'const uint8_t *'),
             ('code *', 'void *'), ('bool ', 'int ')):
    d = d.replace(a, b)
if repo:
    print('int t30_ncu_par105_download(const uint8_t *pb, int32_t param_1)\n{')
else:
    print('#include "gen_prelude.h"\nint ncu105_par_download_gen(const uint8_t *pb, int32_t param_1)\n{')
print(d); print(t); print('}')
