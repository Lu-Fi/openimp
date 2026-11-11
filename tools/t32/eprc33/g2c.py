#!/usr/bin/env python3
"""Ghidra pseudo-C (32-bit MIPS) -> arena C: every pointer becomes a 32-bit OEM
address, every memory access goes through the X->m accessors (LD_/ST_).
Development aid for the T33 eprc port (#295)."""
import re, sys, struct, copy
from pycparser import c_parser, c_ast, c_generator

TYPEDEFS = """
typedef unsigned char undefined1; typedef unsigned char undefined; typedef unsigned char byte;
typedef unsigned char uchar; typedef unsigned short undefined2; typedef unsigned short ushort;
typedef unsigned int undefined4; typedef unsigned int uint; typedef unsigned long long undefined8;
typedef unsigned long long ulonglong; typedef long long longlong; typedef long long DItype;
typedef unsigned long long UDItype; typedef int bool; typedef void code; typedef unsigned short word;
typedef unsigned int dword; typedef signed char sbyte; typedef unsigned int undefined3; typedef double DFtype; typedef float SFtype;
"""
# Ghidra type name -> (kind, size, signed)
BASE = {
    'undefined1': ('i', 1, False), 'undefined': ('i', 1, False), 'byte': ('i', 1, False),
    'uchar': ('i', 1, False), 'char': ('i', 1, True), 'sbyte': ('i', 1, True),
    'undefined2': ('i', 2, False), 'ushort': ('i', 2, False), 'short': ('i', 2, True),
    'word': ('i', 2, False), 'undefined3': ('i', 4, False),
    'undefined4': ('i', 4, False), 'uint': ('i', 4, False), 'int': ('i', 4, True),
    'dword': ('i', 4, False), 'bool': ('i', 4, True),
    'undefined8': ('i', 8, False), 'ulonglong': ('i', 8, False), 'longlong': ('i', 8, True),
    'DItype': ('i', 8, True), 'UDItype': ('i', 8, False),
    'SFtype': ('f', 4, True), 'DFtype': ('f', 8, True), 'float': ('f', 4, True), 'double': ('f', 8, True), 'void': ('v', 0, False), 'code': ('c', 4, False),
}
CT = {('i', 1, False): 'uint8_t', ('i', 1, True): 'int8_t', ('i', 2, False): 'uint16_t',
      ('i', 2, True): 'int16_t', ('i', 4, False): 'uint32_t', ('i', 4, True): 'int32_t',
      ('i', 8, False): 'uint64_t', ('i', 8, True): 'int64_t', ('f', 4, True): 'float',
      ('f', 8, True): 'double'}
SUF = {('i', 1, False): 'U8', ('i', 1, True): 'S8', ('i', 2, False): 'U16', ('i', 2, True): 'S16',
       ('i', 4, False): 'U32', ('i', 4, True): 'S32', ('i', 8, False): 'U64', ('i', 8, True): 'S64',
       ('f', 4, True): 'F32', ('f', 8, True): 'F64', ('c', 4, False): 'U32'}

PTR = ('p',)          # ('p', pointee)
INT = ('i', 4, True)


def tsize(t):
    if t[0] == 'p':
        return 4
    return t[1]


def ptr(t):
    return ('p', t)


def is_ptr(t):
    return t is not None and t[0] == 'p'


def decl_type(node):
    """pycparser type node -> our type"""
    if isinstance(node, c_ast.TypeDecl):
        names = node.type.names
        n = ' '.join(names)
        if n in BASE:
            return BASE[n]
        if n in ('unsigned int',):
            return ('i', 4, False)
        if n in ('long',):
            return INT
        raise ValueError('type %s' % n)
    if isinstance(node, c_ast.PtrDecl):
        return ptr(decl_type(node.type))
    if isinstance(node, c_ast.ArrayDecl):
        return ('a', decl_type(node.type), node.dim)
    if isinstance(node, c_ast.FuncDecl):
        return ('fn', decl_type(node.type))
    raise ValueError('decl %r' % node)


def C(t):
    if t[0] == 'p' or t[0] == 'c':
        return 'uint32_t'
    if t[0] == 'v':
        return 'void'
    return CT[t]


def ident(n):
    return c_ast.ID(n)


def call(n, *args):
    return c_ast.FuncCall(ident(n), c_ast.ExprList(list(args)))


def const(v):
    return c_ast.Constant('int', '0x%xu' % (v & 0xffffffff) if v >= 0x80000000 or v < 0 else '0x%x' % v)


def cast(ct, e):
    return c_ast.Cast(c_ast.Typename(None, [], None, c_ast.TypeDecl(None, [], None, c_ast.IdentifierType([ct]))), e)


class Conv:
    def __init__(self, got, funcs, ghidra_off=0x10000):
        self.got = got            # ghidra GOT slot address -> file VMA value
        self.funcs = funcs        # internal function name -> (ret type, [param types])
        self.off = ghidra_off
        self.unknown = set()

    # ---------------------------------------------------------------- types
    def gsym(self, name):
        m = re.match(r'PTR_.*_([0-9a-f]{8})$', name)
        if m:
            slot = int(m.group(1), 16) - self.off
            return self.got.get(slot)
        return None

    def ext_name(self, name):
        m = re.match(r'PTR_(.*)_[0-9a-f]{8}$', name)
        return m.group(1) if m else name

    # returns (newnode, type)
    def ex(self, n):
        k = type(n).__name__
        return getattr(self, 'e_' + k)(n)

    def e_Constant(self, n):
        if n.type in ('float', 'double'):
            return n, ('f', 8, True) if n.type == 'double' else ('f', 4, True)
        if n.type == 'char':
            return n, ('i', 1, True)
        if n.type == 'string':
            return const(0), INT
        v = n.value.lower().rstrip('ul')
        t = ('i', 4, 'u' not in n.value.lower())
        if 'll' in n.value.lower():
            t = ('i', 8, 'u' not in n.value.lower())
        return n, t

    def e_ID(self, n):
        if n.name in self.memvars:
            a, t = self.memvars[n.name]
            return self.load(a, t)
        if n.name in self.env:
            t = self.env[n.name]
            if t[0] == 'a':
                return n, ptr(t[1])
            return n, t
        if n.name in ('true', 'false'):
            return const(1 if n.name == 'true' else 0), INT
        v = self.gsym(n.name)
        if v is not None:
            return const(v), ptr(('i', 1, False))
        m = re.match(r'(?:DAT|LAB)_([0-9a-f]{8})$', n.name)
        if m:
            return const(int(m.group(1), 16) - self.off), ('i', 1, False)   # used as &DAT
        if n.name in self.funcs:
            return n, ('fnref', n.name)
        self.unknown.add(n.name)
        return n, INT

    def e_Cast(self, n):
        t = decl_type(n.to_type.type)
        e, et = self.ex(n.expr)
        if t[0] == 'p':
            return cast('uint32_t', e), t
        if t[0] == 'v':
            return e, t
        return cast(C(t), e), t

    def load(self, addr, t):
        if t[0] == 'v' or t[0] == 'c':
            t = ('i', 1, False) if t[0] == 'v' else t
        if t[0] == 'p':
            return call('LD_U32', addr), t
        return call('LD_' + SUF[t], addr), t

    def addr_of(self, n):
        """lvalue memory location -> (address expr, type) or None for a plain variable"""
        if isinstance(n, c_ast.ID) and n.name in self.memvars:
            return self.memvars[n.name]
        if isinstance(n, c_ast.StructRef):
            m = re.match(r'_(\d+)_(\d+)_$', n.field.name)
            a, t = self.addr_of(n.name)
            sz = int(m.group(2))
            return c_ast.BinaryOp('+', a, const(int(m.group(1)))), ('i', sz, False)
        if isinstance(n, c_ast.UnaryOp) and n.op == '*':
            a, at = self.ex(n.expr)
            if not is_ptr(at):
                at = ptr(('i', 1, False))
            return a, at[1]
        if isinstance(n, c_ast.ArrayRef):
            a, at = self.ex(c_ast.BinaryOp('+', n.name, n.subscript))
            return a, at[1]
        return None

    def e_UnaryOp(self, n):
        if n.op == '*':
            a, t = self.addr_of(n)
            return self.load(a, t)
        if n.op == '&':
            r = self.addr_of(n.expr)
            if r is None:
                if isinstance(n.expr, c_ast.ID) and n.expr.name in self.funcs:
                    return const(0), INT
                raise ValueError('address of variable %s' % c_generator.CGenerator().visit(n.expr))
            return r[0], ptr(r[1])
        if n.op in ('++', '--', 'p++', 'p--'):
            op = '+=' if '+' in n.op else '-='
            return self.ex(c_ast.Assignment(op, n.expr, c_ast.Constant('int', '1')))[0], INT
        if n.op == 'sizeof':
            raise ValueError('sizeof')
        e, t = self.ex(n.expr)
        if n.op == '!':
            return c_ast.UnaryOp('!', e), INT
        return c_ast.UnaryOp(n.op, e), t

    def e_StructRef(self, n):
        a, t = self.addr_of(n)
        return self.load(a, t)

    def e_ArrayRef(self, n):
        a, t = self.addr_of(n)
        return self.load(a, t)

    def arith_type(self, a, b):
        if a[0] == 'f' or b[0] == 'f':
            return ('f', max(a[1] if a[0] == 'f' else 0, b[1] if b[0] == 'f' else 0), True)
        sz = max(a[1], b[1], 4)
        if sz == 8:
            s = not ((a[1] == 8 and not a[2]) or (b[1] == 8 and not b[2]))
            return ('i', 8, s)
        s = not ((a[1] == 4 and not a[2]) or (b[1] == 4 and not b[2]))
        return ('i', 4, s)

    def e_BinaryOp(self, n):
        l, lt = self.ex(n.left)
        r, rt = self.ex(n.right)
        op = n.op
        if op in ('+', '-') and is_ptr(lt) and not is_ptr(rt):
            s = tsize(lt[1]) if lt[1][0] != 'v' else 1
            if s != 1:
                r = c_ast.BinaryOp('*', cast('int32_t', r), const(s))
            return c_ast.BinaryOp(op, l, r), lt
        if op == '+' and is_ptr(rt) and not is_ptr(lt):
            s = tsize(rt[1])
            if s != 1:
                l = c_ast.BinaryOp('*', cast('int32_t', l), const(s))
            return c_ast.BinaryOp(op, l, r), rt
        if op == '-' and is_ptr(lt) and is_ptr(rt):
            s = tsize(lt[1])
            d = cast('int32_t', c_ast.BinaryOp('-', l, r))
            return (c_ast.BinaryOp('/', d, const(s)) if s != 1 else d), INT
        if op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
            return c_ast.BinaryOp(op, l, r), INT
        if op in ('<<', '>>'):
            return c_ast.BinaryOp(op, l, r), (lt if lt[0] == 'i' and lt[1] >= 4 else INT)
        if is_ptr(lt):
            lt = ('i', 4, False)
        if is_ptr(rt):
            rt = ('i', 4, False)
        return c_ast.BinaryOp(op, l, r), self.arith_type(lt, rt)

    def e_TernaryOp(self, n):
        c, _ = self.ex(n.cond)
        a, at = self.ex(n.iftrue)
        b, bt = self.ex(n.iffalse)
        return c_ast.TernaryOp(c, a, b), at

    def store(self, a, t, v):
        if t[0] == 'p' or t[0] == 'c':
            return call('ST_U32', a, v)
        if t[0] == 'v':
            t = ('i', 1, False)
        return call('ST_' + SUF[t], a, v)

    def e_Assignment(self, n):
        loc = self.addr_of(n.lvalue)
        r, rt = self.ex(n.rvalue)
        if loc is None:
            lv, lt = self.ex(n.lvalue)
            if n.op in ('+=', '-=') and is_ptr(lt) and not is_ptr(rt):
                s = tsize(lt[1])
                if s != 1:
                    r = c_ast.BinaryOp('*', cast('int32_t', r), const(s))
            return c_ast.Assignment(n.op, lv, r), lt
        a, t = loc
        if n.op != '=':
            cur, _ = self.load(a, t)
            v = c_ast.BinaryOp(n.op[:-1], cur, r)
            if t[0] == 'p' and n.op in ('+=', '-='):
                s = tsize(t[1])
                if s != 1:
                    v = c_ast.BinaryOp(n.op[:-1], cur, c_ast.BinaryOp('*', cast('int32_t', r), const(s)))
        else:
            v = r
        if t[0] != 'p' and t[0] in CT and t[0] != 'c':
            v = cast(C(t), v)
        return self.store(a, t, v), t

    def e_ExprList(self, n):
        return c_ast.ExprList([self.ex(e)[0] for e in n.exprs]), INT

    def e_FuncCall(self, n):
        f = n.name
        args = n.args.exprs if n.args else []
        name = None
        if isinstance(f, c_ast.ID):
            name = f.name
        elif isinstance(f, c_ast.UnaryOp) and f.op == '*':
            inner = f.expr
            if isinstance(inner, c_ast.Cast):
                inner = inner.expr
            if isinstance(inner, c_ast.ID) and inner.name.startswith('PTR_'):
                name = self.ext_name(inner.name)
            elif isinstance(inner, c_ast.ID):
                name = '__fnptr_' + inner.name
        if name is None:
            raise ValueError('call %s' % c_generator.CGenerator().visit(n))
        if name in ('fread', 'fseeko', 'fclose', 'fopen', 'printf', 'puts', 'fwrite', 'fprintf',
                    '__assert', 'exit', 'free', 'access'):
            return call('OEM_IGNORE_' + name.strip('_')), INT
        conv = [self.ex(a) for a in args]
        if name in self.funcs:
            rt = self.funcs[name][0]
            if self.funcs[name][1] is not None and len(args) != self.funcs[name][1]:
                print('ARGCOUNT %s -> %s: %d args, callee has %d' % (self.cur, name, len(args), self.funcs[name][1]), file=sys.stderr)
            return call(name, ident('X'), *[c for c, _ in conv]), rt
        if name in ('CONCAT44', 'CONCAT11', 'CONCAT22', 'ZEXT48'):
            return call(name, *[c for c, _ in conv]), (('i', 8, False) if name in ('CONCAT44', 'ZEXT48') else INT)
        if name == 'trap':
            return call('OEM_TRAP'), INT
        if name in ('__divdi3', '__moddi3'):
            return call('OEM_' + name.strip('_'), *[c for c, _ in conv]), ('i', 8, True)
        if name in ('__udivdi3', '__umoddi3'):
            return call('OEM_' + name.strip('_'), *[c for c, _ in conv]), ('i', 8, False)
        if name in ('memset', 'memcpy', 'calloc', 'malloc'):
            return call('OEM_' + name, ident('X'), *[c for c, _ in conv]), (ptr(('v', 0, False)))
        if name in ('__floatdisf', '__floatdidf', '__fixsfdi', '__fixdfdi', '__floatundisf',
                    '__fixunssfdi', '__fixunsdfdi', '__floatundidf'):
            rt = {'__floatdisf': ('f', 4, True), '__floatdidf': ('f', 8, True), '__floatundisf': ('f', 4, True),
                  '__floatundidf': ('f', 8, True), '__fixsfdi': ('i', 8, True), '__fixdfdi': ('i', 8, True),
                  '__fixunssfdi': ('i', 8, False), '__fixunsdfdi': ('i', 8, False)}[name]
            if name in ('__fixsfdi', '__fixdfdi') and len(args) == 1 and isinstance(args[0], c_ast.FuncCall) \
                    and isinstance(args[0].name, c_ast.ID) and args[0].name.name == 'CONCAT44':
                conv = [self.ex(args[0].args.exprs[1])]
            return call('OEM_' + name.strip('_'), *[c for c, _ in conv]), rt
        if name in ('pow', 'log', 'sqrt', 'exp', 'floor', 'powf', 'logf', 'log2f', 'sqrtf', 'expf', 'fabs'):
            return call(name, *[c for c, _ in conv]), ('f', 4 if name.endswith('f') else 8, True)
        self.unknown.add('call:' + name)
        return call('OEM_UNKNOWN_' + name.strip('_'), *[c for c, _ in conv]), INT

    # --------------------------------------------------------------- statements
    def st(self, n):
        if n is None:
            return None
        k = type(n).__name__
        if k == 'Compound':
            items = []
            for i in (n.block_items or []):
                if isinstance(i, c_ast.Decl):
                    t = decl_type(i.type)
                    self.env[i.name] = t
                    if i.name in self.memvars:
                        self.memvars[i.name] = (self.memvars[i.name][0], t if t[0] != 'a' else t[1])
                        continue
                    if t[0] == 'a':
                        raise ValueError('local array %s' % i.name)
                    items.append(c_ast.Decl(i.name, [], [], [], [], c_ast.TypeDecl(i.name, [], None, c_ast.IdentifierType([C(t)])),
                                            self.ex(i.init)[0] if i.init else None, None))
                else:
                    items.append(self.st(i))
            return c_ast.Compound(items)
        if k in ('If',):
            return c_ast.If(self.ex(n.cond)[0], self.st(n.iftrue), self.st(n.iffalse))
        if k == 'While':
            return c_ast.While(self.ex(n.cond)[0], self.st(n.stmt))
        if k == 'DoWhile':
            return c_ast.DoWhile(self.ex(n.cond)[0], self.st(n.stmt))
        if k == 'For':
            return c_ast.For(self.ex(n.init)[0] if n.init else None, self.ex(n.cond)[0] if n.cond else None,
                             self.ex(n.next)[0] if n.next else None, self.st(n.stmt))
        if k == 'Switch':
            return c_ast.Switch(self.ex(n.cond)[0], self.st(n.stmt))
        if k == 'Case':
            return c_ast.Case(n.expr, [self.st(s) for s in n.stmts])
        if k == 'Default':
            return c_ast.Default([self.st(s) for s in n.stmts])
        if k == 'Label':
            return c_ast.Label(n.name, self.st(n.stmt))
        if k in ('Goto', 'Break', 'Continue', 'EmptyStatement'):
            return n
        if k == 'Return':
            return c_ast.Return(self.ex(n.expr)[0] if n.expr else None)
        return self.ex(n)[0]

    def func(self, fd):
        self.cur = fd.decl.name
        self.fidx = getattr(self, 'fidx', -1) + 1
        self.env = {}
        self.memvars = {}
        g0 = c_generator.CGenerator().visit(fd.body)
        if re.search(r'&\s*\(?local_[0-9a-f]+\b|\blocal_[0-9a-f]+\._', g0) or re.search(r'&local_', g0):
            offs = sorted(set(int(x, 16) for x in re.findall(r'\blocal_([0-9a-f]+)\b', g0)))
            top = max(offs) + 8
            for o in offs:
                self.memvars['local_%x' % o] = (c_ast.BinaryOp('+', ident('X->frame'), const(self.fidx * 0x200 + top - o)), INT)
        ft = fd.decl.type
        rt = decl_type(ft.type)
        params = []
        for p in (ft.args.params if ft.args else []):
            if isinstance(p, c_ast.Typename):
                continue
            t = decl_type(p.type)
            self.env[p.name] = t
            params.append('%s %s' % (C(t), p.name))
        body = self.st(fd.body)
        g = c_generator.CGenerator()
        sig = 'static %s %s(E33 *X%s)' % (C(rt), fd.decl.name, ''.join(', ' + p for p in params))
        return sig, sig + '\n' + g.visit(body)


def split_funcs(text):
    parts = re.split(r'^// ===== (\S+) @ ([0-9a-f]+)\n', text, flags=re.M)
    out = []
    for i in range(1, len(parts), 3):
        out.append((parts[i], int(parts[i + 1], 16), parts[i + 2]))
    return out


def resolve_fnptrs(body, got, a2n):
    pat = re.compile(r'\b(\w+) = (?:\(code \*\))?\(?(PTR_\w+?_([0-9a-f]{8}))(?: \+ (-?0x[0-9a-f]+))?\)?;')
    assigns = []
    for m in pat.finditer(body):
        slot = int(m.group(3), 16) - 0x10000
        if slot in got:
            assigns.append((m.start(), m.group(1), got[slot] + (int(m.group(4), 16) if m.group(4) else 0)))
    def rep(m):
        v = m.group(1)
        last = [a for p, n, a in assigns if n == v and p < m.start()]
        if not last or last[-1] not in a2n:
            return m.group(0)
        return a2n[last[-1]] + '('
    return re.sub(r'\(\*(?:\(code \*\))?(\w+)\)\(', rep, body)


def clean(body):
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'^//.*$', '', body, flags=re.M)
    # GOT-called soft-float helpers whose result Ghidra lost (in_f0 / in_v0 ...)
    body = re.sub(r'^(\s+)\(\*\(code \*\)(PTR___(?:floatdisf|floatdidf|floatundisf)_[0-9a-f]+)\)\(([^;]*)\);',
                  lambda m: '%sin_f0 = (*(code *)%s)(%s);' % (m.group(1), m.group(2), m.group(3)), body, flags=re.M)
    return body


def main():
    src, gotfile, out = sys.argv[1], sys.argv[2], sys.argv[3]
    skip = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else set()
    got = {}
    for l in open(gotfile):
        m = re.match(r'\s+([0-9a-f]{8}) -?\d+\(gp\)\s+([0-9a-f]{8})', l)
        if m:
            got[int(m.group(1), 16)] = int(m.group(2), 16)
    a2n = {}
    for l in open('/mnt/NVMe/git/scratch-clones/session-110bf918/audit/issue295/gh/syms.txt'):
        f = l.split()
        if len(f) >= 4:
            a2n.setdefault(int(f[0], 16), f[3])
    for l in open(gotfile):
        m = re.match(r'\s+[0-9a-f]{8} -?\d+\(gp\)\s+([0-9a-f]{8})\s+[0-9a-f]{8}\s+FUNC\s+UND\s+(\S+)', l)
        if m:
            a2n.setdefault(int(m.group(1), 16), m.group(2))
    fs = split_funcs(open(src).read())
    parser = c_parser.CParser()
    asts = {}
    funcs = {}
    for name, addr, body in fs:
        if name in skip:
            continue
        cname = name.replace('.', '_')
        b = clean(resolve_fnptrs(body, got, a2n)).replace(name, cname)
        for n2, _, _ in fs:
            b = re.sub(r'\b%s\b' % re.escape(n2), n2.replace('.', '_'), b)
        try:
            ast = parser.parse(TYPEDEFS + b, filename=name)
        except Exception as ex:
            print('PARSE FAIL', name, ex, file=sys.stderr)
            continue
        fd = [e for e in ast.ext if isinstance(e, c_ast.FuncDef)][0]
        asts[cname] = (addr, fd)
        ft = fd.decl.type
        funcs[cname] = (decl_type(ft.type), len([p for p in (ft.args.params if ft.args else []) if not isinstance(p, c_ast.Typename)]))
    cv = Conv(got, funcs)
    protos, bodies = [], []
    for cname, (addr, fd) in asts.items():
        try:
            sig, code = cv.func(fd)
        except Exception as ex:
            print('CONV FAIL', cname, ex, file=sys.stderr)
            continue
        protos.append(sig + ';')
        bodies.append('/* OEM %s @ 0x%x */\n%s' % (cname, addr - 0x10000, code))
    with open(out, 'w') as f:
        f.write('\n'.join(protos) + '\n\n' + '\n\n'.join(bodies))
    print('unknown:', sorted(cv.unknown), file=sys.stderr)


if __name__ == '__main__':
    main()
