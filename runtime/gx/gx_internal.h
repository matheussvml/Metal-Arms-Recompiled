/* Estado do processador gráfico do GameCube (CP, XF, BP) e interface com o backend. */
#ifndef GX_INTERNAL_H
#define GX_INTERNAL_H

#include "../rt.h"

/* ---- registradores -------------------------------------------------------- */

typedef struct {
    /* CP: formato dos vértices */
    u32 matidx_a, matidx_b;
    u32 vcd_lo, vcd_hi;
    u32 vat_a[8], vat_b[8], vat_c[8];
    u32 array_base[16], array_stride[16];
    /* XF: transformação e iluminação */
    u32 xfmem[0x1000 + 0x60]; /* 0x0000-0x0FFF memória, 0x1000-0x105F registradores */
    /* BP: rasterização, TEV, pixel engine */
    u32 bp[0x100];
    u32 bp_mask;
} GXRegs;

extern GXRegs g_gx;
extern s16 g_tev_reg[4][4];
extern u8 g_tev_konst[4][4];

/* O que mudou desde o último desenho (o backend limpa o que já enviou à GPU).
 * g_bp_dirty: um bit por grupo de 8 registradores BP (bit = reg >> 3). */
enum { XFD_POS = 1, XFD_NRM = 2, XFD_POST = 4, XFD_LIGHT = 8, XFD_REG = 16 };
extern u32 g_xf_dirty, g_bp_dirty;
#define BPG(reg) (1u << ((reg) >> 3))

#define XF(addr) g_gx.xfmem[(addr)]
#define XFF(addr) (*(float *)&g_gx.xfmem[(addr)])
#define BP(reg) g_gx.bp[(reg)]

/* ---- vértice já decodificado ------------------------------------------------ */

typedef struct {
    float pos[3];
    float nrm[3];
    u8 col[2][4];  /* RGBA */
    float tex[8][2];
    u8 posmtx;      /* índice da matriz de posição (linhas de 4 floats na XF) */
    u8 pad[3];
    u8 texmtx[8];
} GXVertex; /* 108 bytes: é copiado e enviado à GPU a cada vértice */

enum { PRIM_QUADS = 0, PRIM_QUADS2 = 1, PRIM_TRIS = 2, PRIM_TRISTRIP = 3, PRIM_TRIFAN = 4,
       PRIM_LINES = 5, PRIM_LINESTRIP = 6, PRIM_POINTS = 7 };

/* ---- backend (gx_d3d.c) ------------------------------------------------------ */

int gxb_init(void);
/* Desenha triângulos (2), linhas (5) ou pontos (7) indexados nos vértices dados. */
void gxb_draw(int prim, const GXVertex *v, u32 nverts, const u32 *idx, u32 nidx);
/* Cópia do EFB: para o XFB (apresenta) ou para uma textura em addr. */
void gxb_efb_copy(u32 bp52, u32 dest_addr);
void gxb_pump_messages(void);

/* ---- texturas (gx_tex.c) ----------------------------------------------------- */

extern u8 g_tmem[0x100000];
/* Decodifica uma textura GX para RGBA8. Devolve 0 se o formato não é suportado. */
int gx_decode_texture(u8 *dst, const u8 *src, u32 w, u32 h, u32 fmt, u32 tlut_addr, u32 tlut_fmt);
u32 gx_texture_size(u32 w, u32 h, u32 fmt);

#endif
