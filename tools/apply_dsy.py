"""Aplica um banco de assinaturas .dsy do Dolphin (totaldb.dsy) às funções que o dtk
detectou em symbols.txt, renomeando as que batem em hash e tamanho.

Uso:
    python apply_dsy.py <main.dol> <symbols.txt> <db.dsy> [--write] [--report arquivo.tsv]
                        [--exclude-range 0xINICIO 0xFIM]

Sem --write só gera o relatório. Com --write, reescreve symbols.txt (backup .bak).
--exclude-range ignora acertos numa faixa (ex.: o código do próprio jogo, onde um hash
que bate é quase sempre coincidência com função de outro jogo).
"""
import re
import shutil
import struct
import sys
from collections import defaultdict

SYM_RE = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // (.*)$")
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MIN_SIZE = 0x10  # abaixo disso as colisões de hash são frequentes demais


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

    def word(self, addr):
        for a, s, o in self.secs:
            if a <= addr < a + s:
                return struct.unpack_from(">I", self.data, o + addr - a)[0]
        raise ValueError(f"endereço fora do DOL: {addr:#x}")


def checksum(dol, start, size):
    """Mesmo algoritmo de HashSignatureDB::ComputeCodeChecksum do Dolphin."""
    s = 0
    for addr in range(start, start + size, 4):
        opcode = dol.word(addr)
        op = opcode & 0xFC000000
        op2 = op3 = 0
        aux = op >> 26
        if aux == 4:
            op2 = opcode & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = opcode & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = opcode & 0x7FF
        elif aux == 59:
            op2 = opcode & 0x3F
            if op2 < 16:
                op3 = opcode & 0x7C0
        elif 32 <= aux < 56:
            op2 = opcode & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)
        s ^= op | op2 | op3
    return s


def load_dsy(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d)[0]
    db = {}
    for i in range(n):
        c, size, raw = struct.unpack_from("<II128s", d, 4 + i * 136)
        full = raw.split(b"\0")[0].decode("latin-1")
        name, _, origin = full.partition("\t")
        db[c] = (size, name.strip(), origin.strip())
    return db


def main():
    dol_path, sym_path, dsy_path = sys.argv[1:4]
    write = "--write" in sys.argv
    report = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
    excl = None
    if "--exclude-range" in sys.argv:
        i = sys.argv.index("--exclude-range")
        excl = (int(sys.argv[i + 1], 16), int(sys.argv[i + 2], 16))

    dol = Dol(dol_path)
    db = load_dsy(dsy_path)
    lines = open(sym_path, encoding="utf-8").read().splitlines()
    existing = {m.group(1) for m in map(SYM_RE.match, lines) if m}

    matches = []  # (idx_linha, addr, size, nome_antigo, nome_novo, origem)
    for idx, line in enumerate(lines):
        m = SYM_RE.match(line)
        if not m or "type:function" not in m.group(4):
            continue
        sm = re.search(r"size:0x([0-9A-Fa-f]+)", m.group(4))
        if not sm:
            continue
        addr, size = int(m.group(3), 16), int(sm.group(1), 16)
        if size < MIN_SIZE:
            continue
        hit = db.get(checksum(dol, addr, size))
        if hit and hit[0] == size:
            matches.append((idx, addr, size, m.group(1), hit[1], hit[2]))

    # o mesmo nome em mais de um endereço indica colisão; nomeia só os únicos
    by_name = defaultdict(list)
    for mt in matches:
        by_name[mt[4]].append(mt)

    applied, skipped = [], []
    for mt in matches:
        idx, addr, size, old, new, origin = mt
        auto = re.fullmatch(r"fn_[0-9A-F]{8}", old) is not None
        if excl and excl[0] <= addr < excl[1]:
            skipped.append((mt, "faixa excluída (código do jogo)"))
        elif not auto:
            skipped.append((mt, "já nomeada pelo dtk" if old == new else f"dtk: {old}"))
        elif not IDENT_RE.match(new):
            skipped.append((mt, "nome C++/não-identificador"))
        elif len(by_name[new]) > 1:
            skipped.append((mt, f"ambígua ({len(by_name[new])} endereços)"))
        elif new in existing:
            skipped.append((mt, "nome já existe em outro símbolo"))
        else:
            applied.append(mt)

    print(f"assinaturas no banco: {len(db)}")
    print(f"funções com hash+tamanho batendo: {len(matches)}")
    print(f"aplicáveis (fn_ automáticas, nome único e válido): {len(applied)}")
    print(f"puladas: {len(skipped)}")

    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write("status\taddr\tsize\tantigo\tnovo\torigem\tmotivo\n")
            for idx, addr, size, old, new, origin in applied:
                f.write(f"aplicada\t{addr:#010x}\t{size:#x}\t{old}\t{new}\t{origin}\t\n")
            for (idx, addr, size, old, new, origin), why in skipped:
                f.write(f"pulada\t{addr:#010x}\t{size:#x}\t{old}\t{new}\t{origin}\t{why}\n")

    if write and applied:
        shutil.copyfile(sym_path, sym_path + ".bak")
        for idx, addr, size, old, new, origin in applied:
            lines[idx] = lines[idx].replace(old, new, 1)
        with open(sym_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"symbols.txt atualizado (backup em {sym_path}.bak)")


if __name__ == "__main__":
    main()
