import sys
s = open(sys.argv[1]).read()
P = [
 # estimate_qp: indirect adjust_P_frame_dqp calls (stack args lost by Ghidra; from 0x190d30, 0x190928, 0x1910f0)
 ("""              lVar62 = (*(code *)PTR___fixdfdi_00211b0c)();
            }
            iVar46 = local_a4;
            iVar47 = (*local_84)(param_2,param_3,param_4);""",
  """              lVar62 = (*(code *)PTR___fixdfdi_00211b0c)(dVar58);
            }
            iVar46 = local_a4;
            iVar47 = adjust_P_frame_dqp(param_2,param_3,param_4,0,(uint)lVar62,(int)((ulonglong)lVar62 >> 0x20),(char)local_a4);"""),
 ("""            iVar40 = (*local_84)(param_2,param_3,param_4);
            if (0 < iVar40) {""",
  """            iVar40 = adjust_P_frame_dqp(param_2,param_3,param_4,0,local_a8,(int)((ulonglong)lVar3 >> 0x20),(char)local_a4);
            if (0 < iVar40) {"""),
 ("""            (*(code *)PTR___divdi3_00211af8)(uVar36,iVar53,iVar34,iVar42);
            iVar40 = (*local_84)(param_2,param_3,param_4);""",
  """            lVarQ = (*(code *)PTR___divdi3_00211af8)(uVar36,iVar53,iVar34,iVar42);
            iVar40 = adjust_P_frame_dqp(param_2,param_3,param_4,0,(uint)lVarQ,(int)((ulonglong)lVarQ >> 0x20),(char)local_a4);"""),
 ("""  longlong lVar62;\n""", """  longlong lVar62;\n  longlong lVarQ;\n"""),
]
for a, b in P:
    n = s.count(a)
    if n != 1:
        print('patch count %d: %s' % (n, a[:80]), file=sys.stderr)
    s = s.replace(a, b)
open(sys.argv[2], 'w').write(s)
