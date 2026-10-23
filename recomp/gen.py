"""Recompilador estático PowerPC (Gekko) -> C.

Lê o main.dol e o symbols.txt do dtk e gera um arquivo C por bloco de funções.
Cada função PPC vira `void f_XXXXXXXX(PPCContext *c)`.

Uso:
    python recomp/gen.py <main.dol> <symbols.txt> <dir_saida> [--only 0xADDR,...] [--chunk N]
"""
import argparse
import bisect
import collections
import os
import re
import struct

# ---------------------------------------------------------------------------
# entrada


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.secs = []
        for i in range(18):
            off = struct.unpack_from(">I", self.data, i * 4)[0]
            addr = struct.unpack_from(">I", self.data, 0x48 + i * 4)[0]
            size = struct.unpack_from(">I", self.data, 0x90 + i * 4)[0]
            if off and size:
                self.secs.append((addr, size, off))
        self.entry = struct.unpack_from(">I", self.data, 0xE0)[0]

    def word(self, addr):
        for a, s, o in self.secs:
            if a <= addr < a + s:
                return struct.unpack_from(">I", self.data, o + addr - a)[0]
        return None


SYM_RE = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // (.*)$")


def load_symbols(path):
    funcs, jumptables = [], []
    for line in open(path, encoding="utf-8"):
        m = SYM_RE.match(line.rstrip("\n"))
        if not m:
            continue
        name, sec, addr, attrs = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        sm = re.search(r"size:0x([0-9A-Fa-f]+)", attrs)
        size = int(sm.group(1), 16) if sm else 0
        if "type:function" in attrs and size:
            funcs.append((addr, size, name))
        elif name.startswith("jumptable_") and size:
            jumptables.append((addr, size))
    funcs.sort()
    return funcs, jumptables


# ---------------------------------------------------------------------------
# decodificação


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def sext12(v):
    return v - 0x1000 if v & 0x800 else v


def mask32(mb, me):
    if mb <= me:
        m = (0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    else:
        m = ((0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return m


def branch_target(pc, inst):
    """Alvo de b/bc (ou None se não for desvio direto)."""
    op = inst >> 26
    if op == 18:
        li = inst & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (li if inst & 2 else pc + li) & 0xFFFFFFFF
    if op == 16:
        bd = sext16(inst & 0xFFFC)
        return (bd if inst & 2 else pc + bd) & 0xFFFFFFFF
    return None


# ---------------------------------------------------------------------------
# geração


R = "c->r[{}]".format
F0 = "c->f[{}].ps0".format
F1 = "c->f[{}].ps1".format


def hx(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Unimpl(Exception):
    pass


class FuncGen:
    def __init__(self, prog, start, end, name):
        self.p = prog
        self.start, self.end, self.name = start, end, name
        self.lines = []
        self.labels = set()
        self.jt_targets = sorted(t for t in prog.jt_targets if start <= t < end)

    # -- auxiliares -------------------------------------------------------
    def inside(self, t):
        return self.start <= t < self.end

    def goto(self, t, pc=None):
        if self.inside(t):
            self.labels.add(t)
            # desvio para trás = laço: ponto seguro para entregar eventos/interrupções
            poll = "PPC_POLL(c); " if pc is not None and t <= pc else ""
            return f"{{ {poll}goto L_{t:08X}; }}" if poll else f"goto L_{t:08X};"
        return self.tail(t)

    def tail(self, t):
        f = self.p.entry_name(t)
        if f:
            return f"{{ {f}(c); return; }}"
        return f"{{ ppc_call_indirect(c, {hx(t)}); return; }}"

    def call(self, t, pc):
        f = self.p.entry_name(t)
        tgt = f"{f}(c);" if f else f"ppc_call_indirect(c, {hx(t)});"
        return f"c->lr = {hx(pc + 4)}; {tgt}"

    @staticmethod
    def ra0(a):
        return "0u" if a == 0 else R(a)

    def rc(self, reg):
        return f" c->cr[0] = cmp_s((s32){reg}, 0, c->xer_so);"

    def bcond(self, inst):
        bo, bi = (inst >> 21) & 31, (inst >> 16) & 31
        pre, conds = "", []
        if not bo & 4:
            pre = "c->ctr--; "
            conds.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
        if not bo & 0x10:
            f, m = bi >> 2, 1 << (3 - (bi & 3))
            conds.append(f"(c->cr[{f}] & {m})" if bo & 8 else f"!(c->cr[{f}] & {m})")
        return pre, " && ".join(conds)

    def bctr_dispatch(self):
        if not self.jt_targets:
            return "{ ppc_call_indirect(c, c->ctr); return; }"
        cases = []
        for t in self.jt_targets:
            self.labels.add(t)
            cases.append(f"case {hx(t)}: goto L_{t:08X};")
        return "switch (c->ctr) { " + " ".join(cases) + " default: ppc_call_indirect(c, c->ctr); return; }"

    # -- instruções -------------------------------------------------------
    def emit(self, pc, inst):
        op = inst >> 26
        d, a, b, cc = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, (inst >> 6) & 31
        simm, uimm = sext16(inst & 0xFFFF), inst & 0xFFFF
        rcbit = inst & 1
        s = d  # rS ocupa o mesmo campo que rD

        if op == 14:
            return f"{R(d)} = {self.ra0(a)} + {hx(simm)};"
        if op == 15:
            return f"{R(d)} = {self.ra0(a)} + {hx(simm << 16)};"
        if op == 32:
            return f"{R(d)} = ld32({self.ra0(a)} + {hx(simm)});"
        if op == 36:
            return f"st32({self.ra0(a)} + {hx(simm)}, {R(s)});"
        if op in (48, 49):
            upd = op == 49
            ea = f"{R(a)} + {hx(simm)}" if upd else f"{self.ra0(a)} + {hx(simm)}"
            return (f"{{ u32 ea = {ea}; double v = (double)u2f(ld32(ea)); {F0(d)} = v; {F1(d)} = v;"
                    + (f" {R(a)} = ea;" if upd else "") + " }")
        if op in (52, 53):
            upd = op == 53
            ea = f"{R(a)} + {hx(simm)}" if upd else f"{self.ra0(a)} + {hx(simm)}"
            return f"{{ u32 ea = {ea}; st32(ea, f2u((float){F0(s)}));" + (f" {R(a)} = ea;" if upd else "") + " }"
        if op in (50, 51):
            upd = op == 51
            ea = f"{R(a)} + {hx(simm)}" if upd else f"{self.ra0(a)} + {hx(simm)}"
            return f"{{ u32 ea = {ea}; {F0(d)} = u2d(ld64(ea));" + (f" {R(a)} = ea;" if upd else "") + " }"
        if op in (54, 55):
            upd = op == 55
            ea = f"{R(a)} + {hx(simm)}" if upd else f"{self.ra0(a)} + {hx(simm)}"
            return f"{{ u32 ea = {ea}; st64(ea, d2u({F0(s)}));" + (f" {R(a)} = ea;" if upd else "") + " }"

        intload = {32: ("ld32", ""), 33: ("ld32", "u"), 34: ("ld8", ""), 35: ("ld8", "u"),
                   40: ("ld16", ""), 41: ("ld16", "u"), 42: ("(u32)(s32)(s16)ld16", ""),
                   43: ("(u32)(s32)(s16)ld16", "u")}
        if op in intload:
            fn, upd = intload[op]
            if upd:
                return f"{{ u32 ea = {R(a)} + {hx(simm)}; {R(d)} = {fn}(ea); {R(a)} = ea; }}"
            return f"{R(d)} = {fn}({self.ra0(a)} + {hx(simm)});"
        intstore = {36: ("st32", "", ""), 37: ("st32", "u", ""), 38: ("st8", "", "(u8)"),
                    39: ("st8", "u", "(u8)"), 44: ("st16", "", "(u16)"), 45: ("st16", "u", "(u16)")}
        if op in intstore:
            fn, upd, cast = intstore[op]
            if upd:
                return f"{{ u32 ea = {R(a)} + {hx(simm)}; {fn}(ea, {cast}{R(s)}); {R(a)} = ea; }}"
            return f"{fn}({self.ra0(a)} + {hx(simm)}, {cast}{R(s)});"
        if op == 46:
            return f"{{ u32 ea = {self.ra0(a)} + {hx(simm)}; for (int i = {d}; i < 32; i++, ea += 4) c->r[i] = ld32(ea); }}"
        if op == 47:
            return f"{{ u32 ea = {self.ra0(a)} + {hx(simm)}; for (int i = {s}; i < 32; i++, ea += 4) st32(ea, c->r[i]); }}"

        if op == 7:
            return f"{R(d)} = (u32)((s64)(s32){R(a)} * {simm});"
        if op == 8:
            return f"{R(d)} = add_ca(c, ~{R(a)}, {hx(simm)}, 1);"
        if op == 10:
            return f"c->cr[{d >> 2}] = cmp_u({R(a)}, {hx(uimm)}, c->xer_so);"
        if op == 11:
            return f"c->cr[{d >> 2}] = cmp_s((s32){R(a)}, {simm}, c->xer_so);"
        if op in (12, 13):
            return f"{R(d)} = add_ca(c, {R(a)}, {hx(simm)}, 0);" + (self.rc(R(d)) if op == 13 else "")
        if op == 24:
            return f"{R(a)} = {R(s)} | {hx(uimm)};"
        if op == 25:
            return f"{R(a)} = {R(s)} | {hx(uimm << 16)};"
        if op == 26:
            return f"{R(a)} = {R(s)} ^ {hx(uimm)};"
        if op == 27:
            return f"{R(a)} = {R(s)} ^ {hx(uimm << 16)};"
        if op == 28:
            return f"{R(a)} = {R(s)} & {hx(uimm)};" + self.rc(R(a))
        if op == 29:
            return f"{R(a)} = {R(s)} & {hx(uimm << 16)};" + self.rc(R(a))
        if op in (20, 21, 23):
            mb, me = (inst >> 6) & 31, (inst >> 1) & 31
            m = mask32(mb, me)
            rot = f"rotl32({R(s)}, {R(b)})" if op == 23 else (f"rotl32({R(s)}, {b})" if b else R(s))
            if op == 20:
                code = f"{R(a)} = ({rot} & {hx(m)}) | ({R(a)} & {hx(~m)});"
            else:
                code = f"{R(a)} = {rot} & {hx(m)};"
            return code + (self.rc(R(a)) if rcbit else "")
        if op == 3:
            return self.trap_cond(pc, d, f"(s32){R(a)}", f"{simm}", f"{R(a)}", hx(simm))
        if op == 17:
            return f"ppc_syscall(c, {hx(pc)});"

        if op == 18:
            t = branch_target(pc, inst)
            if inst & 1:
                return self.call(t, pc)
            return self.goto(t, pc)
        if op == 16:
            t = branch_target(pc, inst)
            pre, cond = self.bcond(inst)
            body = self.call(t, pc) if inst & 1 else self.goto(t, pc)
            if not cond:
                return pre + body
            return f"{pre}if ({cond}) {{ {body} }}"
        if op == 19:
            return self.op19(pc, inst, d, a, b)
        if op == 31:
            return self.op31(pc, inst, d, a, b, rcbit)
        if op == 59:
            return self.op59(inst, d, a, b, cc)
        if op == 63:
            return self.op63(inst, d, a, b, cc)
        if op == 4:
            return self.op4(inst, d, a, b, cc)
        if op in (56, 57, 60, 61):
            w, i, disp = (inst >> 15) & 1, (inst >> 12) & 7, sext12(inst & 0xFFF)
            upd = op in (57, 61)
            ea = f"{R(a)} + {hx(disp)}" if upd else f"{self.ra0(a)} + {hx(disp)}"
            fn = "psq_l" if op in (56, 57) else "psq_st"
            return f"{{ u32 ea = {ea}; {fn}(c, {d}, ea, {w}, {i});" + (f" {R(a)} = ea;" if upd else "") + " }"
        raise Unimpl

    def trap_cond(self, pc, to, sa, sb, ua, ub):
        conds = []
        if to & 16: conds.append(f"{sa} < {sb}")
        if to & 8: conds.append(f"{sa} > {sb}")
        if to & 4: conds.append(f"{ua} == {ub}")
        if to & 2: conds.append(f"{ua} < {ub}")
        if to & 1: conds.append(f"{ua} > {ub}")
        if to == 31:
            return f"ppc_trap(c, {hx(pc)});"
        return f"if ({' || '.join(conds) or '0'}) ppc_trap(c, {hx(pc)});"

    def op19(self, pc, inst, d, a, b):
        xo = (inst >> 1) & 0x3FF
        if xo == 16:  # bclr
            pre, cond = self.bcond(inst)
            if inst & 1:
                body = f"{{ u32 t = c->lr; c->lr = {hx(pc + 4)}; ppc_call_indirect(c, t); }}"
            else:
                body = "return;"
            return pre + (f"if ({cond}) {body}" if cond else body)
        if xo == 528:  # bcctr
            pre, cond = self.bcond(inst)
            if inst & 1:
                body = f"{{ c->lr = {hx(pc + 4)}; ppc_call_indirect(c, c->ctr); }}"
            else:
                body = self.bctr_dispatch()
            return pre + (f"if ({cond}) {body}" if cond else body)
        if xo == 0:
            return f"c->cr[{d >> 2}] = c->cr[{a >> 2}];"
        crops = {257: "x & y", 129: "x & !y", 289: "x == y", 225: "!(x & y)", 33: "!(x | y)",
                 449: "x | y", 417: "x | !y", 193: "x ^ y"}
        if xo in crops:
            expr = crops[xo].replace("x", f"crbit(c, {a})").replace("y", f"crbit(c, {b})")
            return f"set_crbit(c, {d}, {expr});"
        if xo == 150:
            return ";"
        if xo == 50:
            # rfi: o SDK só usa como salto (RealMode) e retorno dele; exceções não rodam aqui
            return ("{ u32 t = c->spr[26]; c->msr = c->spr[27]; "
                    "if (((t ^ c->lr) & 0x3FFFFFFFu) == 0) return; ppc_call_indirect(c, t); return; }")
        raise Unimpl

    def op31(self, pc, inst, d, a, b, rcbit):
        xo = (inst >> 1) & 0x3FF
        s = d
        rc = (lambda reg: self.rc(reg) if rcbit else "")
        # formas XO com OE: o bit OE faz parte do campo de 10 bits
        xo9 = xo & 0x1FF
        xo_forms = {8, 10, 40, 75, 11, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}
        if xo9 in xo_forms:
            if xo & 0x200:
                raise Unimpl  # OE=1 (overflow) não aparece no jogo
            xo = xo9
            ra, rb = R(a), R(b)
            exprs = {
                266: f"{ra} + {rb}",
                40: f"{rb} - {ra}",
                10: f"add_ca(c, {ra}, {rb}, 0)",
                138: f"add_ca(c, {ra}, {rb}, c->xer_ca)",
                202: f"add_ca(c, {ra}, 0, c->xer_ca)",
                234: f"add_ca(c, {ra}, 0xFFFFFFFFu, c->xer_ca)",
                8: f"add_ca(c, ~{ra}, {rb}, 1)",
                136: f"add_ca(c, ~{ra}, {rb}, c->xer_ca)",
                200: f"add_ca(c, ~{ra}, 0, c->xer_ca)",
                232: f"add_ca(c, ~{ra}, 0xFFFFFFFFu, c->xer_ca)",
                104: f"0u - {ra}",
                235: f"(u32)((s64)(s32){ra} * (s32){rb})",
                75: f"(u32)(((s64)(s32){ra} * (s32){rb}) >> 32)",
                11: f"(u32)(((u64){ra} * {rb}) >> 32)",
                491: f"divw((s32){ra}, (s32){rb})",
                459: f"divwu({ra}, {rb})",
            }
            return f"{R(d)} = {exprs[xo]};" + rc(R(d))

        logic = {28: "{s} & {b}", 60: "{s} & ~{b}", 444: "{s} | {b}", 412: "{s} | ~{b}",
                 316: "{s} ^ {b}", 124: "~({s} | {b})", 476: "~({s} & {b})", 284: "~({s} ^ {b})"}
        if xo in logic:
            return f"{R(a)} = {logic[xo].format(s=R(s), b=R(b))};" + rc(R(a))
        if xo == 24:
            return f"{R(a)} = ({R(b)} & 0x20) ? 0u : {R(s)} << ({R(b)} & 31);" + rc(R(a))
        if xo == 536:
            return f"{R(a)} = ({R(b)} & 0x20) ? 0u : {R(s)} >> ({R(b)} & 31);" + rc(R(a))
        if xo == 792:
            return f"{R(a)} = sraw(c, {R(s)}, {R(b)} & 0x3F);" + rc(R(a))
        if xo == 824:
            return f"{R(a)} = sraw(c, {R(s)}, {b});" + rc(R(a))
        if xo == 26:
            return f"{R(a)} = cntlzw({R(s)});" + rc(R(a))
        if xo == 954:
            return f"{R(a)} = (u32)(s32)(s8){R(s)};" + rc(R(a))
        if xo == 922:
            return f"{R(a)} = (u32)(s32)(s16){R(s)};" + rc(R(a))
        if xo == 0:
            return f"c->cr[{d >> 2}] = cmp_s((s32){R(a)}, (s32){R(b)}, c->xer_so);"
        if xo == 32:
            return f"c->cr[{d >> 2}] = cmp_u({R(a)}, {R(b)}, c->xer_so);"
        if xo == 4:
            return self.trap_cond(pc, d, f"(s32){R(a)}", f"(s32){R(b)}", R(a), R(b))

        # load/store indexados
        ea = f"{self.ra0(a)} + {R(b)}"
        eau = f"{R(a)} + {R(b)}"
        loads = {23: ("ld32", 0), 55: ("ld32", 1), 87: ("ld8", 0), 119: ("ld8", 1), 279: ("ld16", 0),
                 311: ("ld16", 1), 343: ("(u32)(s32)(s16)ld16", 0), 375: ("(u32)(s32)(s16)ld16", 1),
                 534: ("_byteswap_ulong(ld32", 0), 790: ("(u32)_byteswap_ushort(ld16", 0)}
        if xo in loads:
            fn, upd = loads[xo]
            close = ")" if fn.startswith(("_byteswap", "(u32)_byteswap")) else ""
            if upd:
                return f"{{ u32 ea = {eau}; {R(d)} = {fn}(ea){close}; {R(a)} = ea; }}"
            return f"{R(d)} = {fn}({ea}){close};"
        stores = {151: ("st32", 0, ""), 183: ("st32", 1, ""), 215: ("st8", 0, "(u8)"), 247: ("st8", 1, "(u8)"),
                  407: ("st16", 0, "(u16)"), 439: ("st16", 1, "(u16)")}
        if xo in stores:
            fn, upd, cast = stores[xo]
            if upd:
                return f"{{ u32 ea = {eau}; {fn}(ea, {cast}{R(s)}); {R(a)} = ea; }}"
            return f"{fn}({ea}, {cast}{R(s)});"
        if xo == 662:
            return f"st32({ea}, _byteswap_ulong({R(s)}));"
        if xo == 918:
            return f"st16({ea}, _byteswap_ushort((u16){R(s)}));"
        if xo in (535, 567):
            e = eau if xo == 567 else ea
            return (f"{{ u32 ea = {e}; double v = (double)u2f(ld32(ea)); {F0(d)} = v; {F1(d)} = v;"
                    + (f" {R(a)} = ea;" if xo == 567 else "") + " }")
        if xo in (599, 631):
            e = eau if xo == 631 else ea
            return f"{{ u32 ea = {e}; {F0(d)} = u2d(ld64(ea));" + (f" {R(a)} = ea;" if xo == 631 else "") + " }"
        if xo in (663, 695):
            e = eau if xo == 695 else ea
            return f"{{ u32 ea = {e}; st32(ea, f2u((float){F0(s)}));" + (f" {R(a)} = ea;" if xo == 695 else "") + " }"
        if xo in (727, 759):
            e = eau if xo == 759 else ea
            return f"{{ u32 ea = {e}; st64(ea, d2u({F0(s)}));" + (f" {R(a)} = ea;" if xo == 759 else "") + " }"
        if xo == 983:
            return f"st32({ea}, (u32)d2u({F0(s)}));"
        if xo == 1014:
            return f"{{ u32 ea = ({ea}) & ~31u; for (int i = 0; i < 32; i += 4) st32(ea + i, 0); }}"
        if xo in (54, 86, 470, 278, 246, 982, 598, 854, 306):
            return ";"  # cache, sync, TLB: sem efeito no host

        # registradores especiais
        if xo == 19:
            return f"{R(d)} = get_cr(c);"
        if xo == 144:
            return f"set_cr_masked(c, {R(s)}, {(inst >> 12) & 0xFF});"
        if xo == 83:
            return f"{R(d)} = c->msr;"
        if xo == 146:
            return f"c->msr = {R(s)};"
        if xo == 512:
            return f"c->cr[{d >> 2}] = (u8)((get_xer(c) >> 28) & 0xF); c->xer_so = c->xer_ov = c->xer_ca = 0;"
        if xo in (339, 467, 371):
            spr = ((inst >> 16) & 31) | (((inst >> 11) & 31) << 5)
            if xo == 371 or (xo == 339 and spr in (268, 269)):
                part = "(u32)(ppc_timebase() >> 32)" if spr == 269 else "(u32)ppc_timebase()"
                return f"{R(d)} = {part};"
            names = {1: None, 8: "c->lr", 9: "c->ctr"}
            if 912 <= spr <= 919:
                reg = f"c->gqr[{spr - 912}]"
            elif spr in names:
                reg = names[spr]
            else:
                reg = f"c->spr[{spr}]"
            if spr == 1:
                return f"{R(d)} = get_xer(c);" if xo == 339 else f"set_xer(c, {R(s)});"
            return f"{R(d)} = {reg};" if xo == 339 else f"{reg} = {R(s)};"
        if xo in (595, 659):
            return f"{R(d)} = 0;"  # registradores de segmento: não usados fora do boot
        if xo in (210, 242):
            return ";"
        raise Unimpl

    def op59(self, inst, d, a, b, cc):
        xo = (inst >> 1) & 31
        if inst & 1:
            raise Unimpl
        fa, fb, fc = F0(a), F0(b), F0(cc)
        exprs = {18: f"{fa} / {fb}", 20: f"{fa} - {fb}", 21: f"{fa} + {fb}", 22: f"sqrt({fb})",
                 24: f"1.0 / {fb}", 25: f"{fa} * {fc}", 28: f"{fa} * {fc} - {fb}", 29: f"{fa} * {fc} + {fb}",
                 30: f"-({fa} * {fc} - {fb})", 31: f"-({fa} * {fc} + {fb})"}
        if xo not in exprs:
            raise Unimpl
        return f"{{ double v = rsingle({exprs[xo]}); {F0(d)} = v; {F1(d)} = v; }}"

    def op63(self, inst, d, a, b, cc):
        if inst & 1:
            raise Unimpl
        fa, fb, fc = F0(a), F0(b), F0(cc)
        xo5 = (inst >> 1) & 31
        aform = {18: f"{fa} / {fb}", 20: f"{fa} - {fb}", 21: f"{fa} + {fb}", 22: f"sqrt({fb})",
                 23: f"fsel({fa}, {fc}, {fb})", 25: f"{fa} * {fc}", 26: f"frsqrte({fb})",
                 28: f"{fa} * {fc} - {fb}", 29: f"{fa} * {fc} + {fb}", 30: f"-({fa} * {fc} - {fb})",
                 31: f"-({fa} * {fc} + {fb})"}
        if xo5 in aform:
            return f"{F0(d)} = {aform[xo5]};"
        xo = (inst >> 1) & 0x3FF
        if xo in (0, 32):
            return f"c->cr[{d >> 2}] = cmp_f({fa}, {fb});"
        if xo == 12:
            return f"{{ double v = rsingle({fb}); {F0(d)} = v; {F1(d)} = v; }}"
        if xo == 14:
            return f"{F0(d)} = fctiw({fb});"
        if xo == 15:
            return f"{F0(d)} = fctiwz({fb});"
        if xo == 40:
            return f"{F0(d)} = -{fb};"
        if xo == 72:
            return f"{F0(d)} = {fb};"
        if xo == 264:
            return f"{F0(d)} = fabs({fb});"
        if xo == 136:
            return f"{F0(d)} = -fabs({fb});"
        if xo == 583:
            return f"{F0(d)} = u2d(0xFFF8000000000000ull | c->fpscr);"
        if xo == 711:
            fm = (inst >> 17) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f"c->fpscr = (c->fpscr & {hx(~m)}) | ((u32)d2u({fb}) & {hx(m)});"
        if xo == 38:
            return f"c->fpscr |= {hx(0x80000000 >> d)};"
        if xo == 70:
            return f"c->fpscr &= {hx(~(0x80000000 >> d))};"
        if xo == 134:
            sh = 28 - 4 * (d >> 2)
            return f"c->fpscr = (c->fpscr & {hx(~(0xF << sh))}) | {hx(((inst >> 12) & 0xF) << sh)};"
        if xo == 64:
            sh = 28 - 4 * (a >> 2)
            return f"c->cr[{d >> 2}] = (u8)((c->fpscr >> {sh}) & 0xF);"
        raise Unimpl

    def op4(self, inst, d, a, b, cc):
        xo10 = (inst >> 1) & 0x3FF
        a0, a1, b0, b1, c0, c1 = F0(a), F1(a), F0(b), F1(b), F0(cc), F1(cc)

        def pair(e0, e1, single=True):
            if single:
                e0, e1 = f"rsingle({e0})", f"rsingle({e1})"
            return f"{{ double t0 = {e0}, t1 = {e1}; {F0(d)} = t0; {F1(d)} = t1; }}"

        if inst & 1 and xo10 not in (0, 32, 64, 96):
            raise Unimpl
        x10 = {
            40: pair(f"-{b0}", f"-{b1}", False),
            72: pair(b0, b1, False),
            136: pair(f"-fabs({b0})", f"-fabs({b1})", False),
            264: pair(f"fabs({b0})", f"fabs({b1})", False),
            528: pair(a0, b0, False),
            560: pair(a0, b1, False),
            592: pair(a1, b0, False),
            624: pair(a1, b1, False),
        }
        if xo10 in x10:
            return x10[xo10]
        if xo10 in (0, 32):
            return f"c->cr[{d >> 2}] = cmp_f({a0}, {b0});"
        if xo10 in (64, 96):
            return f"c->cr[{d >> 2}] = cmp_f({a1}, {b1});"
        if xo10 == 1014:
            return f"{{ u32 ea = ({self.ra0(a)} + {R(b)}) & ~31u; for (int i = 0; i < 32; i += 4) st32(ea + i, 0); }}"
        xo6 = (inst >> 1) & 0x3F
        if xo6 in (6, 7, 38, 39):
            w, i = (inst >> 10) & 1, (inst >> 7) & 7
            upd = xo6 in (38, 39)
            ea = f"{R(a)} + {R(b)}" if upd else f"{self.ra0(a)} + {R(b)}"
            fn = "psq_l" if xo6 in (6, 38) else "psq_st"
            return f"{{ u32 ea = {ea}; {fn}(c, {d}, ea, {w}, {i});" + (f" {R(a)} = ea;" if upd else "") + " }"
        xo5 = (inst >> 1) & 31
        aform = {
            10: (f"{a0} + {b1}", c1),
            11: (c0, f"{a0} + {b1}"),
            12: (f"{a0} * {c0}", f"{a1} * {c0}"),
            13: (f"{a0} * {c1}", f"{a1} * {c1}"),
            14: (f"{a0} * {c0} + {b0}", f"{a1} * {c0} + {b1}"),
            15: (f"{a0} * {c1} + {b0}", f"{a1} * {c1} + {b1}"),
            18: (f"{a0} / {b0}", f"{a1} / {b1}"),
            20: (f"{a0} - {b0}", f"{a1} - {b1}"),
            21: (f"{a0} + {b0}", f"{a1} + {b1}"),
            23: (f"fsel({a0}, {c0}, {b0})", f"fsel({a1}, {c1}, {b1})"),
            24: (f"1.0 / {b0}", f"1.0 / {b1}"),
            25: (f"{a0} * {c0}", f"{a1} * {c1}"),
            26: (f"frsqrte({b0})", f"frsqrte({b1})"),
            28: (f"{a0} * {c0} - {b0}", f"{a1} * {c1} - {b1}"),
            29: (f"{a0} * {c0} + {b0}", f"{a1} * {c1} + {b1}"),
            30: (f"-({a0} * {c0} - {b0})", f"-({a1} * {c1} - {b1})"),
            31: (f"-({a0} * {c0} + {b0})", f"-({a1} * {c1} + {b1})"),
        }
        if xo5 in aform:
            e0, e1 = aform[xo5]
            return pair(e0, e1, xo5 != 23)
        raise Unimpl

    # -- função inteira ---------------------------------------------------
    def generate(self):
        body = []
        for pc in range(self.start, self.end, 4):
            inst = self.p.dol.word(pc)
            try:
                code = self.emit(pc, inst)
            except Unimpl:
                self.p.unimpl[(inst >> 26, (inst >> 1) & 0x3FF)] += 1
                code = f"ppc_unimplemented(c, {hx(pc)}, {hx(inst)});"
            body.append((pc, code))
        hle = self.p.hle.get(self.start)
        fname = f"f_{self.start:08X}_orig" if hle else f"f_{self.start:08X}"
        # __restrict: a RAM emulada (g_mem) nunca se sobrepõe ao contexto, então o
        # compilador pode manter registradores do PPC em registradores do host entre stores
        out = [f"/* {self.name} */", f"void {fname}(PPCContext *__restrict c) {{"]
        for pc, code in body:
            if pc in self.labels:
                out.append(f"L_{pc:08X}:;")
            out.append(f"    {code}")
        out.append("}")
        if hle:
            out.append(f"void f_{self.start:08X}(PPCContext *c) {{ hle_{hle}(c); }}")
        return out


class Program:
    def __init__(self, dol, funcs, jumptables, hle=None):
        self.dol = dol
        self.funcs = funcs
        self.hle = hle or {}  # endereço -> nome da implementação nativa (hle_<nome>)
        self.starts = [f[0] for f in funcs]
        self.unimpl = collections.Counter()
        self.jt_targets = set()
        for addr, size in jumptables:
            for o in range(0, size, 4):
                t = dol.word(addr + o)
                if t is not None:
                    self.jt_targets.add(t)
        for a, s, _ in funcs:
            self.jt_targets.update(self.find_switch_tables(a, a + s))
        # entradas = inícios de função + alvos de bl/b que caem no meio de uma função
        self.entries = {f[0]: f for f in funcs}
        for addr, size, name in funcs:
            for pc in range(addr, addr + size, 4):
                inst = dol.word(pc)
                t = branch_target(pc, inst)
                if t is None or (addr <= t < addr + size) or t in self.entries:
                    continue
                host = self.containing(t)
                if host:
                    self.entries[t] = (t, host[0] + host[1] - t, f"{host[2]}+{t - host[0]:#x}")

    def find_switch_tables(self, start, end):
        """Alvos de switch no padrão do CodeWarrior:
            cmplwi rN, K ; bgt default ; lis rT, tbl@ha ; addi rT, rT, tbl@l
            slwi r0, rN, 2 ; lwzx r0, rT, r0 ; mtctr r0 ; bctr
        """
        out = set()
        word = self.dol.word
        for pc in range(start, end, 4):
            if word(pc) != 0x4E800420:  # bctr
                continue
            base = count = None
            lo = hi = None
            for back in range(pc - 4, max(start, pc - 64) - 4, -4):
                inst = word(back)
                op, rd, ra, rb = inst >> 26, (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31
                if base is None and op == 31 and (inst >> 1) & 0x3FF == 23:  # lwzx
                    base = ra if rb == 0 else rb if ra == 0 else ra
                elif base is not None and lo is None and op == 14 and rd == base and ra == base:
                    lo = sext16(inst & 0xFFFF)
                elif base is not None and lo is not None and hi is None and op == 15 and rd == base:
                    hi = (inst & 0xFFFF) << 16
                elif count is None and op == 10:  # cmplwi rN, K
                    count = (inst & 0xFFFF) + 1
            if hi is None or lo is None:
                continue
            table = (hi + lo) & 0xFFFFFFFF
            for k in range(count or 512):
                t = word(table + 4 * k)
                if t is None or not (start <= t < end):
                    break
                out.add(t)
        return out

    def containing(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            a, s, n = self.funcs[i]
            if a <= addr < a + s:
                return self.funcs[i]
        return None

    def entry_name(self, t):
        return f"f_{t:08X}" if t in self.entries else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("symbols")
    ap.add_argument("out")
    ap.add_argument("--only", help="lista de endereços de entrada a gerar (com dependências diretas)")
    ap.add_argument("--chunk", type=int, default=400, help="funções por arquivo .c")
    ap.add_argument("--hle", help="arquivo com nomes de funções substituídas por hle_<nome>()")
    args = ap.parse_args()

    dol = Dol(args.dol)
    funcs, jts = load_symbols(args.symbols)
    hle = {}
    if args.hle:
        by_name = {n: a for a, s, n in funcs}
        for line in open(args.hle, encoding="utf-8"):
            name = line.split("#")[0].strip()
            if not name:
                continue
            if name not in by_name:
                raise SystemExit(f"HLE: função desconhecida {name}")
            hle[by_name[name]] = name
    prog = Program(dol, funcs, jts, hle)

    entries = sorted(prog.entries.values())
    if args.only:
        wanted = {int(x, 16) for x in args.only.split(",")}
        # fecha sobre as chamadas diretas para o conjunto compilar sozinho
        todo, keep = list(wanted), set()
        while todo:
            e = todo.pop()
            if e in keep or e not in prog.entries:
                continue
            keep.add(e)
            a, s, _ = prog.entries[e]
            his = {}
            for pc in range(a, a + s, 4):
                inst = dol.word(pc)
                t = branch_target(pc, inst)
                if t is not None and t in prog.entries and not (a <= t < a + s):
                    todo.append(t)
                # endereços de função montados com lis + addi/ori (callbacks, ponteiros)
                op, rd, ra = inst >> 26, (inst >> 21) & 31, (inst >> 16) & 31
                if op == 15 and ra == 0:
                    his[rd] = (inst & 0xFFFF) << 16
                elif op in (14, 24) and ra in his:
                    lo = sext16(inst & 0xFFFF) if op == 14 else inst & 0xFFFF
                    v = (his[ra] + lo) & 0xFFFFFFFF
                    if v in prog.entries:
                        todo.append(v)
        entries = [e for e in entries if e[0] in keep]

    os.makedirs(args.out, exist_ok=True)

    def write_if_changed(fn, text):
        # só regrava o que mudou, para o build incremental recompilar o mínimo
        path = os.path.join(args.out, fn)
        if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
            return
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    names = []
    for k in range(0, len(entries), args.chunk):
        chunk = entries[k:k + args.chunk]
        lines = ['#include "ppc.h"', '#include "ppc_funcs.h"', ""]
        for a, s, n in chunk:
            lines += FuncGen(prog, a, a + s, n).generate()
            lines.append("")
        fn = f"ppc_funcs_{k // args.chunk:03d}.c"
        write_if_changed(fn, "\n".join(lines))
        names.append(fn)

    hdr = ["#pragma once", '#include "ppc.h"']
    for a, s, n in entries:
        hdr.append(f"void f_{a:08X}(PPCContext *c); /* {n} */")
    for a, n in sorted(hle.items()):
        hdr.append(f"void f_{a:08X}_orig(PPCContext *c);")
        hdr.append(f"void hle_{n}(PPCContext *c);")
    hdr += ["typedef struct { u32 addr; PPCFunc fn; } PPCFuncEntry;",
            "extern const PPCFuncEntry g_ppc_funcs[];", "extern const u32 g_ppc_func_count;", ""]
    write_if_changed("ppc_funcs.h", "\n".join(hdr))
    tbl = ['#include "ppc_funcs.h"', "const PPCFuncEntry g_ppc_funcs[] = {"]
    tbl += [f"    {{0x{a:08X}u, f_{a:08X}}}," for a, s, n in entries]
    tbl += ["};", f"const u32 g_ppc_func_count = {len(entries)};", ""]
    write_if_changed("ppc_table.c", "\n".join(tbl))
    syms = ["#pragma once", "/* endereços das funções com nome (config/GM5E7D/symbols.txt) */"]
    for a, s, n in funcs:
        if re.fullmatch(r"[A-Za-z_]\w*", n) and not re.fullmatch(r"fn_[0-9A-F]{8}", n):
            syms.append(f"#define SYM_{n} 0x{a:08X}u")
    write_if_changed("ppc_syms.h", "\n".join(syms) + "\n")

    total = sum(s // 4 for _, s, _ in entries)
    bad = sum(prog.unimpl.values())
    print(f"{len(entries)} entradas, {total} instruções, {len(names)} arquivos .c")
    print(f"não implementadas: {bad} ({100 * bad / max(total, 1):.3f}%)")
    for (op, xo), n in prog.unimpl.most_common(25):
        print(f"  op {op:2d} xo {xo:4d}: {n}")


if __name__ == "__main__":
    main()
