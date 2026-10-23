/* Interpretador do fluxo de comandos do GX (o que o jogo escreve em 0xCC008000).
 *
 * Os bytes chegam pelo write-gather pipe (runtime/mmio.c) e vão para um buffer.
 * Como a GPU de verdade, a execução é assíncrona: o buffer é interpretado em blocos
 * ou quando alguém precisa do resultado (gx_fifo_flush: thread dormindo à espera
 * da GPU, leitura de registradores do CP/PE, retrace). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gx_internal.h"

GXRegs g_gx;
s16 g_tev_reg[4][4]; /* PREV, C0, C1, C2 (RGBA, 11 bits com sinal) */
u8 g_tev_konst[4][4];
u32 g_xf_dirty = ~0u, g_bp_dirty = ~0u;

static s16 s11(u32 v) { return (s16)(v & 0x400 ? (s32)(v & 0x7FF) - 0x800 : (s32)(v & 0x7FF)); }

/* BP 0xE0-0xE7: pares L (vermelho/alfa) e H (azul/verde); o bit 23 diz se é konst */
static void tev_color_write(u32 reg, u32 val) {
    u32 n = (reg - 0xE0) >> 1;
    int hi = reg & 1, konst = (val >> 23) & 1;
    u32 x = val & 0x7FF, y = (val >> 12) & 0x7FF;
    if (konst) {
        if (hi) { g_tev_konst[n][2] = (u8)x; g_tev_konst[n][1] = (u8)y; }
        else { g_tev_konst[n][0] = (u8)x; g_tev_konst[n][3] = (u8)y; }
    } else {
        if (hi) { g_tev_reg[n][2] = s11(x); g_tev_reg[n][1] = s11(y); }
        else { g_tev_reg[n][0] = s11(x); g_tev_reg[n][3] = s11(y); }
    }
}

static struct {
    u64 cmds, draws, verts, dl_calls, unknown;
} g_stats;

/* ---- formato de vértice ------------------------------------------------------ */

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static const int comp_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const int color_size[8] = {2, 3, 4, 2, 3, 4, 4, 4};
static const int idx_size[4] = {0, 0, 1, 2};

/* n componentes do mesmo formato: um switch por atributo em vez de um por componente */
static __forceinline void read_n(const u8 *p, u32 fmt, float scale, float *out, int n) {
    switch (fmt) {
    case 0: for (int k = 0; k < n; k++) out[k] = p[k] * scale; break;
    case 1: for (int k = 0; k < n; k++) out[k] = (s8)p[k] * scale; break;
    case 2:
        for (int k = 0; k < n; k++) { u16 h; memcpy(&h, p + 2 * k, 2); out[k] = _byteswap_ushort(h) * scale; }
        break;
    case 3:
        for (int k = 0; k < n; k++) { u16 h; memcpy(&h, p + 2 * k, 2); out[k] = (s16)_byteswap_ushort(h) * scale; }
        break;
    default:
        for (int k = 0; k < n; k++) {
            u32 w;
            memcpy(&w, p + 4 * k, 4);
            w = _byteswap_ulong(w);
            memcpy(&out[k], &w, 4);
        }
        break;
    }
}

static void read_color(const u8 *p, u32 fmt, u8 out[4]) {
    switch (fmt) {
    case 0: {
        u16 v = be16(p);
        out[0] = (u8)(((v >> 11) & 31) * 255 / 31);
        out[1] = (u8)(((v >> 5) & 63) * 255 / 63);
        out[2] = (u8)((v & 31) * 255 / 31);
        out[3] = 255;
        break;
    }
    case 1: case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3:
        out[0] = (u8)((p[0] >> 4) * 17); out[1] = (u8)((p[0] & 15) * 17);
        out[2] = (u8)((p[1] >> 4) * 17); out[3] = (u8)((p[1] & 15) * 17);
        break;
    case 4: {
        u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
        out[0] = (u8)(((v >> 18) & 63) * 255 / 63); out[1] = (u8)(((v >> 12) & 63) * 255 / 63);
        out[2] = (u8)(((v >> 6) & 63) * 255 / 63); out[3] = (u8)((v & 63) * 255 / 63);
        break;
    }
    default: memcpy(out, p, 4); break;
    }
}

/* Plano de leitura de um formato (VCD + VAT), calculado uma vez por desenho */
typedef struct {
    u32 mode, fmt, cnt, size; /* modo 0 nada, 1 direto, 2 índice8, 3 índice16 */
    float scale;
} Attr;

typedef struct {
    int size;
    int pnmtx, texmtx_mask;
    Attr pos, nrm, col[2], tex[8];
    int nbt3; /* normal indexada com 3 índices (N, B, T) */
    int ntex, tex_list[8]; /* coordenadas de textura presentes, em ordem */
} VtxFmt;

static void tex_attr(u32 a, u32 b, u32 c, int i, u32 *cnt, u32 *fmt, u32 *shift) {
    switch (i) {
    case 0: *cnt = (a >> 21) & 1; *fmt = (a >> 22) & 7; *shift = (a >> 25) & 31; break;
    case 1: *cnt = b & 1; *fmt = (b >> 1) & 7; *shift = (b >> 4) & 31; break;
    case 2: *cnt = (b >> 9) & 1; *fmt = (b >> 10) & 7; *shift = (b >> 13) & 31; break;
    case 3: *cnt = (b >> 18) & 1; *fmt = (b >> 19) & 7; *shift = (b >> 22) & 31; break;
    case 4: *cnt = (b >> 27) & 1; *fmt = (b >> 28) & 7; *shift = c & 31; break;
    case 5: *cnt = (c >> 5) & 1; *fmt = (c >> 6) & 7; *shift = (c >> 9) & 31; break;
    case 6: *cnt = (c >> 14) & 1; *fmt = (c >> 15) & 7; *shift = (c >> 18) & 31; break;
    default: *cnt = (c >> 23) & 1; *fmt = (c >> 24) & 7; *shift = (c >> 27) & 31; break;
    }
}

static VtxFmt g_fmt_cache[8];
static u32 g_fmt_valid; /* bit por VAT; zerado quando VCD/VAT mudam */

static const VtxFmt *vtx_format(int vat) {
    VtxFmt *f = &g_fmt_cache[vat];
    if (g_fmt_valid & (1u << vat)) return f;
    u32 lo = g_gx.vcd_lo, hi = g_gx.vcd_hi, a = g_gx.vat_a[vat], b = g_gx.vat_b[vat], c = g_gx.vat_c[vat];
    memset(f, 0, sizeof *f);
    int s = 0;
    f->pnmtx = lo & 1;
    s += f->pnmtx;
    f->texmtx_mask = (lo >> 1) & 0xFF;
    for (int i = 0; i < 8; i++) s += (f->texmtx_mask >> i) & 1;

    Attr *p = &f->pos;
    p->mode = (lo >> 9) & 3; p->fmt = (a >> 1) & 7; p->cnt = (a & 1) ? 3 : 2;
    p->scale = p->fmt < 4 ? 1.0f / (float)(1u << ((a >> 4) & 31)) : 1.0f;
    p->size = (u32)comp_size[p->fmt] * p->cnt;
    s += p->mode == 1 ? (int)p->size : idx_size[p->mode];

    Attr *n = &f->nrm;
    n->mode = (lo >> 11) & 3; n->fmt = (a >> 10) & 7;
    int nbt = (a >> 9) & 1;
    n->cnt = nbt ? 9 : 3;
    n->scale = n->fmt == 1 ? 1.0f / 64 : n->fmt == 3 ? 1.0f / 16384 : 1.0f;
    n->size = (u32)comp_size[n->fmt] * n->cnt;
    f->nbt3 = nbt && ((a >> 31) & 1);
    if (n->mode == 1) s += (int)n->size;
    else if (n->mode) s += idx_size[n->mode] * (f->nbt3 ? 3 : 1);

    for (int ci = 0; ci < 2; ci++) {
        Attr *col = &f->col[ci];
        col->mode = (lo >> (13 + 2 * ci)) & 3;
        col->fmt = (a >> (14 + 4 * ci)) & 7;
        col->size = (u32)color_size[col->fmt];
        s += col->mode == 1 ? (int)col->size : idx_size[col->mode];
    }
    for (int i = 0; i < 8; i++) {
        Attr *t = &f->tex[i];
        u32 cnt, fmt, shift;
        tex_attr(a, b, c, i, &cnt, &fmt, &shift);
        t->mode = (hi >> (2 * i)) & 3;
        t->fmt = fmt;
        t->cnt = cnt ? 2 : 1;
        t->scale = fmt < 4 ? 1.0f / (float)(1u << shift) : 1.0f;
        t->size = (u32)comp_size[fmt] * t->cnt;
        s += t->mode == 1 ? (int)t->size : idx_size[t->mode];
        if (t->mode) f->tex_list[f->ntex++] = i;
    }
    f->size = s;
    g_fmt_valid |= 1u << vat;
    return f;
}

/* Ponteiro para o dado de um atributo: direto no fluxo ou indexado no array. */
static const u8 *attr_ptr(const u8 **pp, u32 mode, int array, u32 direct_size) {
    const u8 *p = *pp;
    if (mode == 1) {
        *pp = p + direct_size;
        return p;
    }
    u32 idx;
    if (mode == 2) { idx = p[0]; *pp = p + 1; }
    else { idx = be16(p); *pp = p + 2; }
    return host_ptr(g_gx.array_base[array] + idx * g_gx.array_stride[array]);
}

/* Vértice-modelo com os valores padrão (matrizes do MATIDX, normal +Z, cor branca,
 * coordenadas zeradas): cada vértice começa como cópia dele e só os atributos
 * presentes no formato são lidos. */
static void make_template(const VtxFmt *f, GXVertex *t, u32 mia, u32 mib) {
    memset(t, 0, sizeof *t);
    t->posmtx = (u8)(mia & 0x3F);
    for (int i = 0; i < 8; i++)
        t->texmtx[i] = (u8)(i < 4 ? (mia >> (6 + 6 * i)) & 0x3F : (mib >> (6 * (i - 4))) & 0x3F);
    t->nrm[2] = 1.0f;
    memset(t->col, 255, sizeof t->col);
    (void)f;
}

static void decode_vertex(const u8 **pp, const VtxFmt *f, GXVertex *v, const GXVertex *tmpl) {
    const u8 *p = *pp;
    *v = *tmpl;
    if (f->pnmtx) v->posmtx = *p++;
    if (f->texmtx_mask)
        for (int i = 0; i < 8; i++)
            if ((f->texmtx_mask >> i) & 1) v->texmtx[i] = *p++;
    {
        const Attr *a = &f->pos;
        const u8 *src = attr_ptr(&p, a->mode, 0, a->size);
        if (a->cnt == 3) read_n(src, a->fmt, a->scale, v->pos, 3);
        else { read_n(src, a->fmt, a->scale, v->pos, 2); v->pos[2] = 0.0f; }
    }
    if (f->nrm.mode) {
        const Attr *a = &f->nrm;
        const u8 *src = attr_ptr(&p, a->mode, 1, a->size);
        read_n(src, a->fmt, a->scale, v->nrm, 3);
        if (a->mode != 1 && f->nbt3) { attr_ptr(&p, a->mode, 1, 0); attr_ptr(&p, a->mode, 1, 0); }
    }
    for (int ci = 0; ci < 2; ci++) {
        const Attr *a = &f->col[ci];
        if (a->mode) read_color(attr_ptr(&p, a->mode, 2 + ci, a->size), a->fmt, v->col[ci]);
    }
    for (int k = 0; k < f->ntex; k++) {
        int i = f->tex_list[k];
        const Attr *a = &f->tex[i];
        const u8 *src = attr_ptr(&p, a->mode, 4 + i, a->size);
        read_n(src, a->fmt, a->scale, v->tex[i], a->cnt);
    }
    *pp = p;
}

/* ---- primitivas -------------------------------------------------------------- */

static GXVertex *g_vin;
static u32 *g_idx;
static u32 g_vcap;

static void ensure_cap(u32 n) {
    if (n <= g_vcap) return;
    g_vcap = n * 2;
    g_vin = (GXVertex *)realloc(g_vin, sizeof(GXVertex) * g_vcap);
    g_idx = (u32 *)realloc(g_idx, sizeof(u32) * g_vcap * 3);
}

static void draw_prim(int prim, int vat, u32 count, const u8 *data) {
    const VtxFmt *f = vtx_format(vat);
    ensure_cap(count + 4);
    GXVertex tmpl;
    make_template(f, &tmpl, XF(0x1018), XF(0x1019));
    const u8 *p = data;
    for (u32 i = 0; i < count; i++) decode_vertex(&p, f, &g_vin[i], &tmpl);
    g_stats.draws++;
    g_stats.verts += count;

    /* strips, fans e quads viram listas por índice (sem duplicar vértices) */
    u32 n = 0;
    int out_prim = 2;
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) {
            g_idx[n++] = i; g_idx[n++] = i + 1; g_idx[n++] = i + 2;
            g_idx[n++] = i; g_idx[n++] = i + 2; g_idx[n++] = i + 3;
        }
        break;
    case PRIM_TRIS:
        for (u32 i = 0; i + 2 < count; i += 3) { g_idx[n++] = i; g_idx[n++] = i + 1; g_idx[n++] = i + 2; }
        break;
    case PRIM_TRISTRIP:
        for (u32 i = 2; i < count; i++) {
            if (i & 1) { g_idx[n++] = i - 1; g_idx[n++] = i - 2; }
            else { g_idx[n++] = i - 2; g_idx[n++] = i - 1; }
            g_idx[n++] = i;
        }
        break;
    case PRIM_TRIFAN:
        for (u32 i = 2; i < count; i++) { g_idx[n++] = 0; g_idx[n++] = i - 1; g_idx[n++] = i; }
        break;
    case PRIM_LINES:
        for (u32 i = 0; i + 1 < count; i += 2) { g_idx[n++] = i; g_idx[n++] = i + 1; }
        out_prim = 5;
        break;
    case PRIM_LINESTRIP:
        for (u32 i = 1; i < count; i++) { g_idx[n++] = i - 1; g_idx[n++] = i; }
        out_prim = 5;
        break;
    default:
        for (u32 i = 0; i < count; i++) g_idx[n++] = i;
        out_prim = 7;
        break;
    }
    if (n) gxb_draw(out_prim, g_vin, count, g_idx, n);
}

/* ---- registradores ------------------------------------------------------------ */

static void cp_write(u8 reg, u32 v) {
    if (reg == 0x30) g_gx.matidx_a = v;
    else if (reg == 0x40) g_gx.matidx_b = v;
    else if (reg == 0x50) { g_gx.vcd_lo = v; g_fmt_valid = 0; }
    else if (reg == 0x60) { g_gx.vcd_hi = v; g_fmt_valid = 0; }
    else if ((reg & 0xF8) == 0x70) { g_gx.vat_a[reg & 7] = v; g_fmt_valid &= ~(1u << (reg & 7)); }
    else if ((reg & 0xF8) == 0x80) { g_gx.vat_b[reg & 7] = v; g_fmt_valid &= ~(1u << (reg & 7)); }
    else if ((reg & 0xF8) == 0x90) { g_gx.vat_c[reg & 7] = v; g_fmt_valid &= ~(1u << (reg & 7)); }
    else if ((reg & 0xF0) == 0xA0) g_gx.array_base[reg & 15] = v & 0x03FFFFFF;
    else if ((reg & 0xF0) == 0xB0) g_gx.array_stride[reg & 15] = v & 0xFF;
}

static void xf_write(u32 addr, u32 v) {
    if (addr >= 0x1000 + 0x60) return;
    if (g_gx.xfmem[addr] == v) return;
    g_gx.xfmem[addr] = v;
    if (addr < 0x100) g_xf_dirty |= XFD_POS;
    else if (addr >= 0x400 && addr < 0x460) g_xf_dirty |= XFD_NRM;
    else if (addr >= 0x500 && addr < 0x600) g_xf_dirty |= XFD_POST;
    else if (addr >= 0x600 && addr < 0x680) g_xf_dirty |= XFD_LIGHT;
    else if (addr >= 0x1000) g_xf_dirty |= XFD_REG;
}

static void load_tlut(void) {
    u32 src = (BP(0x64) & 0xFFFFFF) << 5;
    u32 dst = (BP(0x65) & 0x3FF) << 9;
    u32 len = ((BP(0x65) >> 10) & 0x7FF) * 32;
    if (dst + len > sizeof g_tmem) len = sizeof g_tmem - dst;
    memcpy(g_tmem + dst, host_ptr(src), len);
}

static void post_pe(u8 kind, u16 token);

static void bp_write(u32 v) {
    u32 reg = v >> 24;
    u32 val = v & 0xFFFFFF;
    if (reg != 0xFE) {
        val = (BP(reg) & ~g_gx.bp_mask) | (val & g_gx.bp_mask);
        g_gx.bp_mask = 0xFFFFFF;
    }
    if (BP(reg) != val) {
        BP(reg) = val;
        g_bp_dirty |= 1u << (reg >> 3); /* um bit por grupo de 8 registradores */
    }
    switch (reg) {
    case 0xFE: g_gx.bp_mask = val; break;
    case 0x45: post_pe(0, 0); break; /* PE_DONE: GXSetDrawDone */
    case 0x47: post_pe(1, (u16)val); break;
    case 0x48: post_pe(2, (u16)val); break;
    case 0x52: gxb_efb_copy(val, (BP(0x4B) & 0xFFFFFF) << 5); break;
    case 0x65: load_tlut(); break;
    default:
        if (reg >= 0xE0 && reg <= 0xE7) tev_color_write(reg, val);
        break;
    }
}

/* ---- comandos ------------------------------------------------------------------ */

static void run_commands(const u8 *buf, u32 len, u32 *consumed, u32 *need, int in_dl);

/* Executa um comando; devolve o tamanho, ou 0 (e quanto falta em *need) se ainda
 * não chegou inteiro. */
static u32 run_one(const u8 *p, u32 avail, u32 *need, int in_dl) {
    u8 op = p[0];
    switch (op) {
    case 0x00: return 1;
    case 0x08:
        if (avail < 6) { *need = 6; return 0; }
        cp_write(p[1], be32(p + 2));
        return 6;
    case 0x10: {
        if (avail < 5) { *need = 5; return 0; }
        u32 hdr = be32(p + 1);
        u32 n = ((hdr >> 16) & 0xF) + 1, addr = hdr & 0xFFFF;
        if (avail < 5 + 4 * n) { *need = 5 + 4 * n; return 0; }
        for (u32 i = 0; i < n; i++) xf_write(addr + i, be32(p + 5 + 4 * i));
        return 5 + 4 * n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        if (avail < 5) { *need = 5; return 0; }
        u32 v = be32(p + 1);
        int arr = 12 + (op - 0x20) / 8;
        u32 idx = v >> 16, n = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
        const u8 *src = host_ptr(g_gx.array_base[arr] + idx * g_gx.array_stride[arr]);
        for (u32 i = 0; i < n; i++) xf_write(addr + i, be32(src + 4 * i));
        return 5;
    }
    case 0x40: {
        if (avail < 9) { *need = 9; return 0; }
        u32 addr = be32(p + 1) & 0x03FFFFFF, size = be32(p + 5);
        g_stats.dl_calls++;
        if (!in_dl && size < 0x01000000) {
            u32 used, dummy;
            run_commands(host_ptr(addr), size, &used, &dummy, 1);
        }
        return 9;
    }
    case 0x44: return 1;
    case 0x48: return 1;
    case 0x61:
        if (avail < 5) { *need = 5; return 0; }
        bp_write(be32(p + 1));
        return 5;
    default:
        if (op & 0x80) {
            if (avail < 3) { *need = 3; return 0; }
            u32 count = be16(p + 1);
            u32 total = 3 + count * (u32)vtx_format(op & 7)->size;
            if (avail < total) { *need = total; return 0; }
            draw_prim((op >> 3) & 7, op & 7, count, p + 3);
            return total;
        }
        g_stats.unknown++;
        if (g_stats.unknown < 20) LOG("GX: opcode desconhecido %02X", op);
        return 1;
    }
}

static void run_commands(const u8 *buf, u32 len, u32 *consumed, u32 *need, int in_dl) {
    u32 pos = 0;
    *need = 0;
    while (pos < len) {
        u32 n = run_one(buf + pos, len - pos, need, in_dl);
        if (!n) break;
        g_stats.cmds++;
        pos += n;
    }
    *consumed = pos;
}

/* ---- entrada pelo write-gather pipe ------------------------------------------ *
 *
 * Como no console, a GPU trabalha em paralelo com a CPU: os bytes do FIFO são
 * juntados em blocos que a thread da GPU interpreta (gpu_main). A CPU só espera
 * por ela quando precisa do resultado (gx_sync: leitura do PE, do status do CP ou
 * do EFB) e, no retrace, se a GPU estiver mais de um quadro atrás. Os eventos do PE
 * (draw done, tokens) voltam por uma fila e viram interrupções na thread do jogo
 * (gx_poll_async). MA_GPU_SYNC=1 interpreta tudo na própria thread do jogo. */

#define FIFO_BATCH 16384 /* tamanho do bloco entregue à GPU */
#define CHUNK_CAP (FIFO_BATCH + 64)

typedef struct Chunk {
    struct Chunk *next;
    u32 len;
    u8 data[CHUNK_CAP];
} Chunk;

static Chunk *g_fill;                     /* bloco sendo preenchido pela CPU */
static Chunk *g_qhead, *g_qtail, *g_free; /* fila para a GPU e blocos livres */
static CRITICAL_SECTION g_qlock;
static HANDLE g_gpu_wake, g_gpu_idle, g_cpu_wake;
static volatile LONG64 g_submitted, g_completed; /* bytes entregues / interpretados */
static int g_threaded;
static u64 g_fence; /* bytes entregues até o retrace anterior */
volatile u64 g_gx_syncs; /* esperas completas da CPU pela GPU (diagnóstico) */

/* buffer linear da GPU: o comando pendente pode atravessar blocos */
static u8 *g_q;
static u32 g_qr, g_qw, g_qcap;
static u32 g_qneed; /* bytes necessários para o comando pendente */

static void grow(u32 n) {
    while (g_qw + n > g_qcap) g_qcap = g_qcap ? g_qcap * 2 : 1u << 20;
    g_q = (u8 *)realloc(g_q, g_qcap);
}

static void gpu_run(const Chunk *c) {
    if (g_qw + c->len > g_qcap) grow(c->len);
    memcpy(g_q + g_qw, c->data, c->len);
    g_qw += c->len;
    if (g_qw - g_qr < g_qneed) return;
    u32 used, need;
    run_commands(g_q + g_qr, g_qw - g_qr, &used, &need, 0);
    g_qr += used;
    g_qneed = need;
    if (g_qr == g_qw) {
        g_qr = g_qw = 0;
    } else if (g_qr > g_qcap / 2) {
        memmove(g_q, g_q + g_qr, g_qw - g_qr);
        g_qw -= g_qr;
        g_qr = 0;
    }
}

/* tira e interpreta tudo o que está na fila (na thread da GPU, ou na do jogo sem thread) */
static void gpu_drain(void) {
    for (;;) {
        EnterCriticalSection(&g_qlock);
        Chunk *c = g_qhead;
        if (c) {
            g_qhead = c->next;
            if (!g_qhead) g_qtail = NULL;
        }
        LeaveCriticalSection(&g_qlock);
        if (!c) return;
        gpu_run(c);
        u32 len = c->len;
        EnterCriticalSection(&g_qlock);
        c->next = g_free;
        g_free = c;
        LeaveCriticalSection(&g_qlock);
        InterlockedAdd64(&g_completed, len);
    }
}

static DWORD WINAPI gpu_main(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_gpu_wake, INFINITE);
        gpu_drain();
        SetEvent(g_gpu_idle);
    }
}

static Chunk *new_chunk(void) {
    EnterCriticalSection(&g_qlock);
    Chunk *c = g_free;
    if (c) g_free = c->next;
    LeaveCriticalSection(&g_qlock);
    if (!c) c = (Chunk *)malloc(sizeof(Chunk));
    c->next = NULL;
    c->len = 0;
    return c;
}

/* entrega o bloco em preenchimento à GPU */
static void submit(void) {
    Chunk *c = g_fill;
    if (!c->len) return;
    g_submitted += c->len;
    g_fill = new_chunk();
    EnterCriticalSection(&g_qlock);
    if (g_qtail) g_qtail->next = c;
    else g_qhead = c;
    g_qtail = c;
    LeaveCriticalSection(&g_qlock);
    if (g_threaded) SetEvent(g_gpu_wake);
    else gpu_drain();
}

/* ---- eventos do PE: da GPU para a thread do jogo ---- */

typedef struct { u16 token; u8 kind; } PeEvent; /* 0 = finish, 1 = token, 2 = token com interrupção */
static PeEvent g_pe_ring[256];
static u32 g_pe_r, g_pe_w;
static volatile LONG g_pe_pending;

static void post_pe(u8 kind, u16 token) {
    EnterCriticalSection(&g_qlock);
    if (g_pe_w - g_pe_r < 256) {
        g_pe_ring[g_pe_w & 255].kind = kind;
        g_pe_ring[g_pe_w & 255].token = token;
        g_pe_w++;
    }
    LeaveCriticalSection(&g_qlock);
    InterlockedExchange(&g_pe_pending, 1);
    if (g_threaded) SetEvent(g_cpu_wake);
}

void gx_poll_async(void) {
    if (!g_pe_pending) return;
    InterlockedExchange(&g_pe_pending, 0);
    for (;;) {
        PeEvent e;
        EnterCriticalSection(&g_qlock);
        int have = g_pe_r != g_pe_w;
        if (have) e = g_pe_ring[g_pe_r++ & 255];
        LeaveCriticalSection(&g_qlock);
        if (!have) break;
        if (e.kind == 0) mmio_pe_finish();
        else mmio_pe_token(e.token, e.kind == 2);
    }
}

/* ---- pontos de sincronização (thread do jogo) ---- */

/* entrega o que houver, sem esperar */
void gx_kick(void) { submit(); }

static void wait_completed(u64 target) {
    while ((u64)g_completed < target) {
        /* a janela é desta thread: continua tratando mensagens enquanto espera */
        MsgWaitForMultipleObjects(1, &g_gpu_idle, FALSE, 1, QS_ALLINPUT);
        if ((u64)g_completed < target) gxb_pump_messages();
    }
}

/* entrega e espera a GPU terminar tudo; aplica os eventos do PE */
void gx_sync(void) {
    submit();
    if (g_threaded && (u64)g_completed < (u64)g_submitted) {
        g_gx_syncs++;
        wait_completed((u64)g_submitted);
    }
    gx_poll_async();
}

/* no retrace: a GPU pode ficar no máximo um quadro atrás */
void gx_retrace(void) {
    submit();
    if (g_threaded) wait_completed(g_fence);
    g_fence = (u64)g_submitted;
    gx_poll_async();
}

/* espera ociosa do escalonador: acorda cedo se a GPU mandar um evento */
void gx_idle_wait(u32 ms) {
    if (g_threaded) WaitForSingleObject(g_cpu_wake, ms);
    else Sleep(ms);
}

void gx_fifo_flush(void) { gx_sync(); }

void gx_fifo_bytes(const u8 *b, int n, int linked) {
    if (!linked) return;
    for (int i = 0; i < n; i++) {
        g_fill->data[g_fill->len++] = b[i];
        if (g_fill->len >= FIFO_BATCH) submit();
    }
}

/* caminho rápido do write-gather pipe ligado à GPU: grava em big-endian direto */
void gx_fifo_put(u32 v, int size) {
    Chunk *c = g_fill;
    u8 *d = c->data + c->len;
    if (size == 4) { d[0] = (u8)(v >> 24); d[1] = (u8)(v >> 16); d[2] = (u8)(v >> 8); d[3] = (u8)v; }
    else if (size == 2) { d[0] = (u8)(v >> 8); d[1] = (u8)v; }
    else d[0] = (u8)v;
    c->len += (u32)size;
    if (c->len >= FIFO_BATCH) submit();
}

void gx_init(void) {
    g_gx.bp_mask = 0xFFFFFF;
    InitializeCriticalSection(&g_qlock);
    g_fill = new_chunk();
    gxb_init();
    g_threaded = getenv("MA_GPU_SYNC") == NULL;
    if (g_threaded) {
        g_gpu_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_gpu_idle = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_cpu_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, gpu_main, NULL, 0, NULL);
    }
    LOG("[gx] GPU %s", g_threaded ? "em thread própria" : "na thread do jogo (MA_GPU_SYNC)");
}

u64 gx_stat_verts(void) { return g_stats.verts; }

void gx_print_stats(void) {
    LOG("[gx] cmds=%llu draws=%llu verts=%llu dl=%llu desconhecidos=%llu", g_stats.cmds, g_stats.draws,
        g_stats.verts, g_stats.dl_calls, g_stats.unknown);
}
