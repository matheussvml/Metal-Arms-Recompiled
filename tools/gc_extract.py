"""Extrai main.dol (e opcionalmente todo o FST) de uma imagem GameCube .ciso/.iso
e converte o DOL num ELF32 PowerPC big-endian que o Ghidra carrega sem plugin.

Uso:
    python gc_extract.py <imagem.ciso|.iso> <dir_saida> [--all]
"""
import os
import struct
import sys


class Disc:
    """Leitura aleatória de uma imagem GameCube, crua (.iso) ou CISO (formato Dolphin)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x8000)
        if head[:4] == b"CISO":
            self.block_size = struct.unpack_from("<I", head, 4)[0]
            block_map = head[8:0x8000]
            # bloco presente -> posição no arquivo; ausente -> zeros
            self.blocks = []
            pos = 0x8000
            for present in block_map:
                if present:
                    self.blocks.append(pos)
                    pos += self.block_size
                else:
                    self.blocks.append(None)
        else:
            self.block_size = None

    def read(self, offset, size):
        if self.block_size is None:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            idx, inner = divmod(offset, self.block_size)
            chunk = min(size, self.block_size - inner)
            pos = self.blocks[idx] if idx < len(self.blocks) else None
            if pos is None:
                out += bytes(chunk)
            else:
                self.f.seek(pos + inner)
                out += self.f.read(chunk)
            offset += chunk
            size -= chunk
        return bytes(out)


def read_fst(disc):
    hdr = disc.read(0, 0x440)
    fst_off, fst_size = struct.unpack_from(">II", hdr, 0x424)
    fst = disc.read(fst_off, fst_size)
    n = struct.unpack_from(">I", fst, 8)[0]
    names = fst[n * 12:]

    def name_at(o):
        return names[o:names.index(b"\0", o)].decode("shift_jis", "replace")

    files = []
    # pilha de (índice_fim, caminho) para diretórios
    stack = [(n, "")]
    i = 1
    while i < n:
        while i >= stack[-1][0]:
            stack.pop()
        flags_name, a, b = struct.unpack_from(">III", fst, i * 12)
        is_dir = flags_name >> 24
        name = name_at(flags_name & 0xFFFFFF)
        path = stack[-1][1] + name
        if is_dir:
            stack.append((b, path + "/"))
        else:
            files.append((path, a, b))
        i += 1
    return files


def extract_dol(disc):
    dol_off = struct.unpack_from(">I", disc.read(0x420, 4))[0]
    hdr = disc.read(dol_off, 0x100)
    size = 0x100
    for i in range(18):
        off = struct.unpack_from(">I", hdr, i * 4)[0]
        sz = struct.unpack_from(">I", hdr, 0x90 + i * 4)[0]
        if off:
            size = max(size, off + sz)
    return disc.read(dol_off, size)


def dol_to_elf(dol):
    entry = struct.unpack_from(">I", dol, 0xE0)[0]
    bss_addr, bss_size = struct.unpack_from(">II", dol, 0xD8)

    segs = []  # (nome, addr, file_off_no_dol, size, exec)
    for i in range(18):
        off = struct.unpack_from(">I", dol, i * 4)[0]
        addr = struct.unpack_from(">I", dol, 0x48 + i * 4)[0]
        sz = struct.unpack_from(">I", dol, 0x90 + i * 4)[0]
        if off and sz:
            is_text = i < 7
            name = f".text{i}" if is_text else f".data{i - 7}"
            segs.append((name, addr, off, sz, is_text))

    # O bss do DOL costuma cobrir .sdata/.sdata2 no meio; separa só as lacunas.
    bss_parts = []
    cur, end = bss_addr, bss_addr + bss_size
    for _, a, _, s, _ in sorted(segs, key=lambda x: x[1]):
        if a + s <= cur or a >= end:
            continue
        if a > cur:
            bss_parts.append((cur, a - cur))
        cur = max(cur, a + s)
    if cur < end:
        bss_parts.append((cur, end - cur))

    shstr = bytearray(b"\0")

    def add_name(s):
        o = len(shstr)
        shstr.extend(s.encode() + b"\0")
        return o

    n_load = len(segs) + len(bss_parts)
    ehsize, phentsize, shentsize = 52, 32, 40
    data_start = ehsize + phentsize * n_load
    body = bytearray()
    phdrs, shdrs = [], [struct.pack(">IIIIIIIIII", *([0] * 10))]

    for name, addr, off, sz, is_text in segs:
        foff = data_start + len(body)
        body += dol[off:off + sz]
        while len(body) % 4:
            body += b"\0"
        flags = 5 if is_text else 6  # R+X / R+W
        phdrs.append(struct.pack(">IIIIIIII", 1, foff, addr, addr, sz, sz, flags, 32))
        shflags = 0x6 if is_text else 0x3  # ALLOC|EXEC / ALLOC|WRITE
        shdrs.append(struct.pack(">IIIIIIIIII", add_name(name), 1, shflags, addr, foff, sz, 0, 0, 32, 0))

    for k, (addr, sz) in enumerate(bss_parts):
        foff = data_start + len(body)
        phdrs.append(struct.pack(">IIIIIIII", 1, foff, addr, addr, 0, sz, 6, 32))
        name = ".bss" if k == 0 else f".bss{k}"
        shdrs.append(struct.pack(">IIIIIIIIII", add_name(name), 8, 0x3, addr, foff, sz, 0, 0, 32, 0))

    shstr_name = add_name(".shstrtab")
    shstr_off = data_start + len(body)
    body += shstr
    while len(body) % 4:
        body += b"\0"
    shdrs.append(struct.pack(">IIIIIIIIII", shstr_name, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    shoff = data_start + len(body)

    ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    ehdr = ident + struct.pack(
        ">HHIIIIIHHHHHH", 2, 20, 1, entry, ehsize, shoff, 0,
        ehsize, phentsize, n_load, shentsize, len(shdrs), len(shdrs) - 1,
    )
    return ehdr + b"".join(phdrs) + bytes(body) + b"".join(shdrs), segs, bss_parts, entry


def main():
    img, out = sys.argv[1], sys.argv[2]
    extract_all = "--all" in sys.argv
    os.makedirs(out, exist_ok=True)
    disc = Disc(img)

    hdr = disc.read(0, 0x60)
    game_id = hdr[:6].decode()
    title = hdr[0x20:0x60].split(b"\0")[0].decode("shift_jis", "replace")
    print(f"Game ID: {game_id}  Título: {title}")

    dol = extract_dol(disc)
    with open(os.path.join(out, "main.dol"), "wb") as f:
        f.write(dol)
    elf, segs, bss_parts, entry = dol_to_elf(dol)
    with open(os.path.join(out, "main.elf"), "wb") as f:
        f.write(elf)
    print(f"main.dol: {len(dol)} bytes, entry 0x{entry:08X}")
    for name, addr, off, sz, _ in segs:
        print(f"  {name:8} 0x{addr:08X}-0x{addr + sz:08X}  ({sz:#x})")
    for addr, sz in bss_parts:
        print(f"  bss      0x{addr:08X}-0x{addr + sz:08X}  ({sz:#x})")

    files = read_fst(disc)
    with open(os.path.join(out, "fst.txt"), "w", encoding="utf-8") as f:
        for path, off, sz in files:
            f.write(f"{off:#010x}\t{sz:#010x}\t{path}\n")
    print(f"FST: {len(files)} arquivos (lista em fst.txt)")

    if extract_all:
        root = os.path.join(out, "files")
        for path, off, sz in files:
            dest = os.path.join(root, *path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                pos = 0
                while pos < sz:
                    n = min(sz - pos, 16 << 20)
                    f.write(disc.read(off + pos, n))
                    pos += n
        print(f"Arquivos extraídos em {root}")


if __name__ == "__main__":
    main()
