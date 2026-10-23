"""Monta um grafo de chamadas a partir do assembly gerado pelo dtk (build/<id>/asm).

Uso como biblioteca:
    from callgraph import load
    g = load("build/GM5E7D/asm")
    g["OSCreateAlarm"].callers

Uso como CLI:
    python callgraph.py <asm_dir> <nome_ou_0xaddr> [...]   # mostra chamadas/chamadores
"""
import glob
import os
import re
import sys

FN_RE = re.compile(r"^\.fn (\S+), \w+$")
END_RE = re.compile(r"^\.endfn ")
INS_RE = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  ([0-9A-F ]{11}) \*/\t(\S+)\s*(.*)$")
BL_RE = re.compile(r"^(bl|b)$")


class Func:
    __slots__ = ("name", "addr", "size", "calls", "tails", "callers", "insns", "refs")

    def __init__(self, name):
        self.name = name
        self.addr = None
        self.size = 0
        self.calls = []    # alvos de bl, em ordem
        self.tails = []    # alvos de b para outra função (tail call)
        self.callers = set()
        self.insns = []    # (addr, mnemônico, operandos)
        self.refs = set()  # símbolos lbl_/fn_ citados em operandos

    def __repr__(self):
        return f"<{self.name} {self.addr:#x} size {self.size:#x}>"


def load(asm_dir):
    funcs = {}
    for path in glob.glob(os.path.join(asm_dir, "**", "*.s"), recursive=True):
        cur = None
        for line in open(path, encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            m = FN_RE.match(line)
            if m:
                cur = funcs[m.group(1)] = Func(m.group(1))
                continue
            if cur is None:
                continue
            if END_RE.match(line):
                cur = None
                continue
            m = INS_RE.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            if cur.addr is None:
                cur.addr = addr
            cur.size = addr + 4 - cur.addr
            mnem, ops = m.group(3), m.group(4)
            cur.insns.append((addr, mnem, ops))
            if mnem == "bl":
                cur.calls.append(ops)
            elif mnem == "b" and not ops.startswith(".L_"):
                cur.tails.append(ops)
            for r in re.findall(r"\b((?:lbl|fn)_[0-9A-F]{8}|[A-Za-z_]\w*)@", ops):
                cur.refs.add(r)
    for f in funcs.values():
        for c in f.calls + f.tails:
            if c in funcs:
                funcs[c].callers.add(f.name)
    return funcs


def by_addr(funcs):
    return {f.addr: f for f in funcs.values() if f.addr is not None}


def main():
    g = load(sys.argv[1])
    idx = by_addr(g)
    for q in sys.argv[2:]:
        f = idx.get(int(q, 16)) if q.startswith("0x") else g.get(q)
        if not f:
            print(f"{q}: não encontrado")
            continue
        print(f"{f.name} @ {f.addr:#x} size {f.size:#x}")
        print("  chama:   ", ", ".join(dict.fromkeys(f.calls + f.tails)))
        print("  chamado: ", ", ".join(sorted(f.callers)))


if __name__ == "__main__":
    main()
