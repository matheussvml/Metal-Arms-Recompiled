/* Núcleo do runtime: estado da CPU Gekko e acesso à memória emulada.
 * O código gerado por recomp/gen.py inclui só este cabeçalho. */
#ifndef PPC_H
#define PPC_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#define PPC_INLINE static __forceinline
#else
#define PPC_INLINE static inline __attribute__((always_inline))
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* Bits de cada campo do CR, na ordem do PowerPC (LT é o bit mais alto do campo). */
enum { CR_LT = 8, CR_GT = 4, CR_EQ = 2, CR_SO = 1 };

typedef struct {
    double ps0, ps1;
} PPCFpr;

typedef struct PPCContext {
    u32 r[32];
    PPCFpr f[32];
    u8 cr[8];
    u8 xer_so, xer_ov, xer_ca;
    u8 xer_bc;
    u32 lr, ctr;
    u32 fpscr;
    u32 msr;
    u32 gqr[8];
    u32 spr[1024]; /* SPRs sem tratamento próprio (HID*, SRR*, SPRG*, BAT*...) */
} PPCContext;

/* ---- memória ---------------------------------------------------------------- */

#define MEM_SIZE 0x02000000u /* 32 MiB cobre os 24 MiB de MEM1 */
#define MEM_MASK (MEM_SIZE - 1)

extern u8 *g_mem;

/* Acessos a 0xCCxxxxxx (registradores de hardware, write-gather pipe do GX) e
 * 0xC8xxxxxx (EFB) vão para o runtime; o resto é RAM espelhada (0x8/0xC). */
u32 mmio_read32(u32 ea);
u16 mmio_read16(u32 ea);
u8 mmio_read8(u32 ea);
void mmio_write32(u32 ea, u32 v);
void mmio_write16(u32 ea, u16 v);
void mmio_write8(u32 ea, u8 v);

/* 0xC8 (EFB) e 0xCC (registradores): (hi | 4) == 0xCC cobre os dois com um teste só */
#define IS_MMIO(ea) ((((ea) >> 24) | 4u) == 0xCCu)

PPC_INLINE u8 *host_ptr(u32 ea) { return g_mem + (ea & MEM_MASK); }

/* Caminho rápido curto de propósito: estas funções são expandidas em cada acesso
 * do código gerado, e o tamanho delas pesa no tempo de compilação. x86 aceita
 * acesso desalinhado. */
PPC_INLINE u8 ld8(u32 ea) { return IS_MMIO(ea) ? mmio_read8(ea) : g_mem[ea & MEM_MASK]; }
PPC_INLINE u16 ld16(u32 ea) {
    return IS_MMIO(ea) ? mmio_read16(ea) : _byteswap_ushort(*(const u16 *)(g_mem + (ea & MEM_MASK)));
}
PPC_INLINE u32 ld32(u32 ea) {
    return IS_MMIO(ea) ? mmio_read32(ea) : _byteswap_ulong(*(const u32 *)(g_mem + (ea & MEM_MASK)));
}
PPC_INLINE u64 ld64(u32 ea) {
    if (IS_MMIO(ea)) return ((u64)mmio_read32(ea) << 32) | mmio_read32(ea + 4);
    return _byteswap_uint64(*(const u64 *)(g_mem + (ea & MEM_MASK)));
}
PPC_INLINE void st8(u32 ea, u8 v) {
    if (IS_MMIO(ea)) mmio_write8(ea, v);
    else g_mem[ea & MEM_MASK] = v;
}
PPC_INLINE void st16(u32 ea, u16 v) {
    if (IS_MMIO(ea)) mmio_write16(ea, v);
    else *(u16 *)(g_mem + (ea & MEM_MASK)) = _byteswap_ushort(v);
}
PPC_INLINE void st32(u32 ea, u32 v) {
    if (IS_MMIO(ea)) mmio_write32(ea, v);
    else *(u32 *)(g_mem + (ea & MEM_MASK)) = _byteswap_ulong(v);
}
PPC_INLINE void st64(u32 ea, u64 v) {
    if (IS_MMIO(ea)) { mmio_write32(ea, (u32)(v >> 32)); mmio_write32(ea + 4, (u32)v); }
    else *(u64 *)(g_mem + (ea & MEM_MASK)) = _byteswap_uint64(v);
}

/* ---- conversões de bits ------------------------------------------------------ */

PPC_INLINE float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }
PPC_INLINE u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
PPC_INLINE double u2d(u64 u) { double d; memcpy(&d, &u, 8); return d; }
PPC_INLINE u64 d2u(double d) { u64 u; memcpy(&u, &d, 8); return u; }
PPC_INLINE double rsingle(double d) { return (double)(float)d; }

/* ---- CR / XER ----------------------------------------------------------------- */

PPC_INLINE u8 cmp_s(s32 a, s32 b, u8 so) { return (u8)((a < b ? CR_LT : a > b ? CR_GT : CR_EQ) | so); }
PPC_INLINE u8 cmp_u(u32 a, u32 b, u8 so) { return (u8)((a < b ? CR_LT : a > b ? CR_GT : CR_EQ) | so); }
PPC_INLINE u8 cmp_f(double a, double b) {
    if (a != a || b != b) return CR_SO; /* unordered */
    return (u8)(a < b ? CR_LT : a > b ? CR_GT : CR_EQ);
}

/* bit 0..31 do CR na numeração IBM (0 = LT do cr0) */
PPC_INLINE u32 crbit(const PPCContext *c, u32 bi) { return (c->cr[bi >> 2] >> (3 - (bi & 3))) & 1; }
PPC_INLINE void set_crbit(PPCContext *c, u32 bi, u32 v) {
    u8 m = (u8)(1u << (3 - (bi & 3)));
    c->cr[bi >> 2] = (u8)(v ? (c->cr[bi >> 2] | m) : (c->cr[bi >> 2] & ~m));
}
PPC_INLINE u32 get_cr(const PPCContext *c) {
    u32 v = 0;
    for (int i = 0; i < 8; i++) v |= (u32)c->cr[i] << (28 - 4 * i);
    return v;
}
PPC_INLINE void set_cr_masked(PPCContext *c, u32 v, u32 crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i)) c->cr[i] = (u8)((v >> (28 - 4 * i)) & 0xF);
}
PPC_INLINE u32 get_xer(const PPCContext *c) {
    return ((u32)c->xer_so << 31) | ((u32)c->xer_ov << 30) | ((u32)c->xer_ca << 29) | c->xer_bc;
}
PPC_INLINE void set_xer(PPCContext *c, u32 v) {
    c->xer_so = (v >> 31) & 1;
    c->xer_ov = (v >> 30) & 1;
    c->xer_ca = (v >> 29) & 1;
    c->xer_bc = v & 0x7F;
}

/* ---- aritmética inteira ------------------------------------------------------ */

PPC_INLINE u32 rotl32(u32 v, u32 n) { return _rotl(v, (int)(n & 31)); }

PPC_INLINE u32 cntlzw(u32 v) {
    unsigned long idx;
    return _BitScanReverse(&idx, v) ? 31 - idx : 32;
}

/* soma com carry: devolve a soma e grava CA */
PPC_INLINE u32 add_ca(PPCContext *c, u32 a, u32 b, u32 cin) {
    u64 s = (u64)a + b + cin;
    c->xer_ca = (u8)(s >> 32);
    return (u32)s;
}

PPC_INLINE u32 divw(s32 a, s32 b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) return a < 0 ? 0xFFFFFFFFu : 0;
    return (u32)(a / b);
}
PPC_INLINE u32 divwu(u32 a, u32 b) { return b ? a / b : 0; }

PPC_INLINE u32 sraw(PPCContext *c, u32 v, u32 sh) {
    s32 s = (s32)v;
    if (sh & 0x20) {
        c->xer_ca = (u8)(s < 0);
        return (u32)(s >> 31);
    }
    sh &= 31;
    c->xer_ca = (u8)(s < 0 && sh && (v & ((1u << sh) - 1)));
    return (u32)(s >> sh);
}

/* ---- ponto flutuante ---------------------------------------------------------- */

PPC_INLINE double fctiwz(double v) {
    s32 r;
    if (v != v) r = INT32_MIN;
    else if (v >= 2147483647.0) r = INT32_MAX;
    else if (v <= -2147483648.0) r = INT32_MIN;
    else r = (s32)v;
    return u2d(0xFFF8000000000000ull | (u32)r);
}
PPC_INLINE double fctiw(double v) {
    s32 r;
    if (v != v) r = INT32_MIN;
    else if (v >= 2147483647.0) r = INT32_MAX;
    else if (v <= -2147483648.0) r = INT32_MIN;
    else r = (s32)nearbyint(v);
    return u2d(0xFFF8000000000000ull | (u32)r);
}
PPC_INLINE double fsel(double a, double c, double b) { return a >= 0.0 ? c : b; }
PPC_INLINE double frsqrte(double v) { return 1.0 / sqrt(v); }

/* ---- quantização dos paired singles (GQR) ------------------------------------- */

/* escala de 6 bits com sinal: 2^s na escrita, 2^-s na leitura (tabela em host.c) */
extern const double g_gqr_scale[64];
PPC_INLINE double gqr_scale(u32 s) { return g_gqr_scale[s & 0x3F]; }

PPC_INLINE double psq_ld1(u32 ea, u32 type, double dq) {
    switch (type) {
    case 4: return (double)ld8(ea) * dq;
    case 5: return (double)ld16(ea) * dq;
    case 6: return (double)(s8)ld8(ea) * dq;
    case 7: return (double)(s16)ld16(ea) * dq;
    default: return (double)u2f(ld32(ea));
    }
}
PPC_INLINE u32 psq_size(u32 type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

PPC_INLINE void psq_l(PPCContext *c, u32 fd, u32 ea, u32 w, u32 i) {
    u32 g = c->gqr[i];
    u32 type = (g >> 16) & 7;
    double dq = gqr_scale(0x40 - ((g >> 24) & 0x3F));
    c->f[fd].ps0 = psq_ld1(ea, type, dq);
    c->f[fd].ps1 = w ? 1.0 : psq_ld1(ea + psq_size(type), type, dq);
}

PPC_INLINE double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

PPC_INLINE void psq_st1(u32 ea, u32 type, double q, double v) {
    switch (type) {
    case 4: st8(ea, (u8)clampd(v * q, 0, 255)); break;
    case 5: st16(ea, (u16)clampd(v * q, 0, 65535)); break;
    case 6: st8(ea, (u8)(s8)clampd(v * q, -128, 127)); break;
    case 7: st16(ea, (u16)(s16)clampd(v * q, -32768, 32767)); break;
    default: st32(ea, f2u((float)v)); break;
    }
}

PPC_INLINE void psq_st(PPCContext *c, u32 fs, u32 ea, u32 w, u32 i) {
    u32 g = c->gqr[i];
    u32 type = g & 7;
    double q = gqr_scale((g >> 8) & 0x3F);
    psq_st1(ea, type, q, c->f[fs].ps0);
    if (!w) psq_st1(ea + psq_size(type), type, q, c->f[fs].ps1);
}

/* ---- serviços do runtime chamados pelo código gerado -------------------------- */

typedef void (*PPCFunc)(PPCContext *);

/* Chamado em todo desvio para trás (laços). A cada N passagens o runtime confere
 * o relógio e entrega eventos pendentes (interrupções, alarmes, troca de thread). */
extern u32 g_poll_countdown;
void ppc_poll(PPCContext *c);
#define PPC_POLL(c) do { if (--g_poll_countdown == 0) ppc_poll(c); } while (0)

void ppc_call_indirect(PPCContext *c, u32 target); /* bctrl/blrl e tail calls via ctr */
void ppc_unimplemented(PPCContext *c, u32 pc, u32 inst);
void ppc_syscall(PPCContext *c, u32 pc);
void ppc_trap(PPCContext *c, u32 pc);
u64 ppc_timebase(void);

#endif
