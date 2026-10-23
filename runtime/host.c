/* Serviços mínimos do runtime para o código recompilado: memória, despacho
 * indireto e erros. O hardware (MMIO) ainda é só um stub que registra acessos. */
#include <stdio.h>

#include "ppc.h"
#include "ppc_funcs.h"

u8 *g_mem;

/* 2^e para o campo de escala de 6 bits com sinal dos GQR (e = 0..31, -32..-1) */
#define P(e) (1.0 * (1ull << (e)))
#define N(e) (1.0 / (1ull << (e)))
const double g_gqr_scale[64] = {
    P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P(10), P(11), P(12), P(13), P(14), P(15),
    P(16), P(17), P(18), P(19), P(20), P(21), P(22), P(23), P(24), P(25), P(26), P(27), P(28), P(29), P(30), P(31),
    N(32), N(31), N(30), N(29), N(28), N(27), N(26), N(25), N(24), N(23), N(22), N(21), N(20), N(19), N(18), N(17),
    N(16), N(15), N(14), N(13), N(12), N(11), N(10), N(9), N(8), N(7), N(6), N(5), N(4), N(3), N(2), N(1),
};
#undef P
#undef N

void ppc_mem_init(void) {
    /* folga no fim: leituras de vértice/textura perto do topo passam alguns bytes */
    if (!g_mem) g_mem = (u8 *)calloc(1, MEM_SIZE + 0x200000);
}

/* Copia as seções do DOL para a memória emulada e zera o bss. */
int ppc_load_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u8 hdr[0x100];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }
#define BE32(p) (((u32)(p)[0] << 24) | ((u32)(p)[1] << 16) | ((u32)(p)[2] << 8) | (p)[3])
    for (int i = 0; i < 18; i++) {
        u32 off = BE32(hdr + i * 4), addr = BE32(hdr + 0x48 + i * 4), size = BE32(hdr + 0x90 + i * 4);
        if (!off || !size) continue;
        fseek(f, (long)off, SEEK_SET);
        if (fread(host_ptr(addr), 1, size, f) != size) { fclose(f); return 0; }
    }
    u32 bss = BE32(hdr + 0xD8), bss_size = BE32(hdr + 0xDC);
#undef BE32
    fclose(f);
    (void)bss;
    (void)bss_size; /* o bss do DOL se sobrepõe a .sdata; memória já começa zerada */
    return 1;
}

/* Tabela direta endereço -> função para as chamadas indiretas (bctrl, callbacks).
 * Cobre 0x80000000-0x80400000 (o código do DOL), uma entrada por instrução. */
#define FAST_BASE 0x80000000u
#define FAST_WORDS (0x00400000u / 4)
static PPCFunc *g_fast;

static PPCFunc ppc_lookup_slow(u32 addr);

PPCFunc ppc_lookup(u32 addr) {
    if (!g_fast) {
        g_fast = (PPCFunc *)calloc(FAST_WORDS, sizeof(PPCFunc));
        for (u32 i = 0; i < g_ppc_func_count; i++) {
            u32 a = g_ppc_funcs[i].addr - FAST_BASE;
            if (a < FAST_WORDS * 4) g_fast[a / 4] = g_ppc_funcs[i].fn;
        }
    }
    u32 off = addr - FAST_BASE;
    if (off < FAST_WORDS * 4 && !(off & 3)) return g_fast[off / 4];
    return ppc_lookup_slow(addr);
}

static PPCFunc ppc_lookup_slow(u32 addr) {
    u32 lo = 0, hi = g_ppc_func_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_ppc_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_ppc_func_count && g_ppc_funcs[lo].addr == addr ? g_ppc_funcs[lo].fn : NULL;
}

void ppc_call_indirect(PPCContext *c, u32 target) {
    /* endereço físico (código em modo real, ex.: RealMode do SDK) -> espelho em cache */
    if (target < 0x01800000u) target |= 0x80000000u;
    PPCFunc fn = ppc_lookup(target);
    if (!fn) {
        fprintf(stderr, "ppc_call_indirect: sem função em 0x%08X (lr=0x%08X)\n", target, c->lr);
        abort();
    }
    fn(c);
}

void ppc_unimplemented(PPCContext *c, u32 pc, u32 inst) {
    fprintf(stderr, "instrução não implementada em 0x%08X: 0x%08X (lr=0x%08X)\n", pc, inst, c->lr);
    abort();
}

void ppc_syscall(PPCContext *c, u32 pc) {
    /* o único uso no SDK é PPCSync (sync/isync via vetor 0xC00): nada a fazer */
    (void)c;
    (void)pc;
}

void ppc_trap(PPCContext *c, u32 pc) {
    fprintf(stderr, "trap em 0x%08X (lr=0x%08X)\n", pc, c->lr);
    abort();
}

/* O runtime do jogo instala o gancho; nos testes ele fica nulo. */
u32 g_poll_countdown = 1024;
void (*g_poll_hook)(PPCContext *c);

void ppc_poll(PPCContext *c) {
    g_poll_countdown = 1024;
    if (g_poll_hook) g_poll_hook(c);
}
