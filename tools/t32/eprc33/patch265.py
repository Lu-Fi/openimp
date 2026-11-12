import sys, re
s = open(sys.argv[1]).read()
# estimate_qp: the three indirect adjust_P_frame_dqp calls (local_84 = PTR + -0x2178) lose their stack
# arguments in Ghidra; from the call sites (0x1a1a04, 0x1a1ec0, 0x1a22b8): a3 unused, sp+16/20 = the 64-bit
# value (fixdfdi / divdi3 result, or the local_b8 / s0 pair = the lVar of 'local_b8 = (uint)lVarNN'),
# sp+24 = (char)local_b4
m = re.search(r'local_b8 = \(uint\)(lVar\d+);', s)
pair = m.group(1)
CALL = r'adjust_P_frame_dqp(param_2,param_3,param_4,0,(uint)%s,(int)((ulonglong)%s >> 0x20),(char)local_b4)'
n = 0
def r1(m):
    global n; n += 1
    return m.group(1) + 'lVarQ = __fixdfdi(' + m.group(2) + m.group(3) + 'lVarQ = __fixdfdi(' + m.group(4) + m.group(5) + CALL % ('lVarQ', 'lVarQ') + ';'
s = re.sub(r'(\n\s+)__fixdfdi\(([^\n]*\);\n\s+\}\n\s+else \{\n\s+)__fixdfdi\(([^\n]*\);\n\s+\}\n\s+\w+ = )\(\*local_84\)\(param_2,param_3,param_4\);',
           lambda m: m.group(1) + 'lVarQ = __fixdfdi(' + m.group(2) + 'lVarQ = __fixdfdi(' + m.group(3) + CALL % ('lVarQ', 'lVarQ') + ';', s)
s, k2 = re.subn(r'(\n\s+)__divdi3\(([^\n]*)\);(\n\s+\w+ = )\(\*local_84\)\(param_2,param_3,param_4\);',
                lambda m: m.group(1) + 'lVarQ = __divdi3(' + m.group(2) + ');' + m.group(3) + CALL % ('lVarQ', 'lVarQ') + ';', s)
s, k3 = re.subn(r'(LAB_001b19dc:\n\s+\w+ = )\(\*local_84\)\(param_2,param_3,param_4\);', lambda m: m.group(1) + CALL % (pair, pair) + ';', s)
s = s.replace('  longlong %s;\n' % pair, '  longlong %s;\n  longlong lVarQ;\n' % pair, 1)
left = s.count('(*local_84)(')
if left or k2 != 1 or k3 != 1:
    print('patch265: left %d divdi3 %d lab %d' % (left, k2, k3), file=sys.stderr)
s = re.sub(r'^(// ===== \S+ @ [0-9a-f]+) size \d+$', r'\1', s, flags=re.M)
open(sys.argv[2], 'w').write(s)
