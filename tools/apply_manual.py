"""Aplica config/<id>/manual_symbols.tsv ao symbols.txt do dtk.

Uso:
    python apply_manual.py <symbols.txt> <manual_symbols.tsv>

Cada linha do .tsv: endereço, nome, confiança, evidência. Renomeia o símbolo de função
naquele endereço; recusa se o nome já existir em outro endereço.
"""
import re
import shutil
import sys

SYM_RE = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // (.*)$")


def main():
    sym_path, tsv_path = sys.argv[1:3]
    wanted = {}
    for line in open(tsv_path, encoding="utf-8"):
        if not line.strip() or line.startswith("#"):
            continue
        addr, name = line.rstrip("\n").split("\t")[:2]
        wanted[int(addr, 16)] = name

    lines = open(sym_path, encoding="utf-8").read().splitlines()
    by_name = {}
    for m in map(SYM_RE.match, lines):
        if m:
            by_name[m.group(1)] = int(m.group(3), 16)

    changed = 0
    for i, line in enumerate(lines):
        m = SYM_RE.match(line)
        if not m or "type:function" not in m.group(4):
            continue
        addr = int(m.group(3), 16)
        new = wanted.pop(addr, None)
        if new is None or m.group(1) == new:
            continue
        if new in by_name and by_name[new] != addr:
            print(f"ERRO: {new} já existe em {by_name[new]:#x}; não renomeado {addr:#x}")
            continue
        lines[i] = line.replace(m.group(1), new, 1)
        changed += 1

    for addr, name in wanted.items():
        print(f"aviso: nenhuma função em {addr:#x} para {name}")

    if changed:
        shutil.copyfile(sym_path, sym_path + ".bak")
        with open(sym_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    print(f"{changed} símbolos renomeados")


if __name__ == "__main__":
    main()
