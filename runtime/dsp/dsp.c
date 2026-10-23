/* Interpretador do DSP do GameCube.
 *
 * O comportamento das instruções, das flags, dos laços em hardware, das exceções,
 * do DMA e do acelerador segue a documentação de engenharia reversa do projeto
 * Dolphin (Source/Core/Core/DSP), que é a referência pública desse processador.
 *
 * Memória (endereços em palavras de 16 bits):
 *   instruções: 0x0000 IRAM (4K), 0x8000 IROM (4K)
 *   dados:      0x0000 DRAM (4K), 0x1000 COEF (2K, ROM), 0xFF00 registradores (IFX)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>

#include "../rt.h"
#include "dsp.h"

/* ---- estado ------------------------------------------------------------------ */

enum { SR_CARRY = 0x0001, SR_OVERFLOW = 0x0002, SR_ARITH_ZERO = 0x0004, SR_SIGN = 0x0008,
       SR_OVER_S32 = 0x0010, SR_TOP2BITS = 0x0020, SR_LOGIC_ZERO = 0x0040, SR_OVERFLOW_STICKY = 0x0080,
       SR_100 = 0x0100, SR_INT_ENABLE = 0x0200, SR_EXT_INT_ENABLE = 0x0800, SR_MUL_MODIFY = 0x2000,
       SR_40_MODE_BIT = 0x4000, SR_MUL_UNSIGNED = 0x8000, SR_CMP_MASK = 0x3F };

enum { CR_RESET = 0x0001, CR_EXTERNAL_INT = 0x0002, CR_HALT = 0x0004, CR_INIT_CODE = 0x0400, CR_INIT = 0x0800 };

enum { R_AR0 = 0x00, R_IX0 = 0x04, R_WR0 = 0x08, R_ST0 = 0x0C, R_ACH0 = 0x10, R_ACH1 = 0x11, R_CR = 0x12,
       R_SR = 0x13, R_PRODL = 0x14, R_PRODM = 0x15, R_PRODH = 0x16, R_PRODM2 = 0x17, R_AXL0 = 0x18,
       R_AXL1 = 0x19, R_AXH0 = 0x1A, R_AXH1 = 0x1B, R_ACL0 = 0x1C, R_ACL1 = 0x1D, R_ACM0 = 0x1E, R_ACM1 = 0x1F };

enum { ST_CALL = 0, ST_DATA = 1, ST_LOOP_ADDR = 2, ST_LOOP_COUNT = 3 };

typedef union {
    u64 val;
    struct { u16 l, m, h, m2; };
} Prod;
typedef union {
    u32 val;
    struct { u16 l, h; };
} AX;
typedef union {
    u64 val;
    struct { u16 l, m; u32 h; }; /* h com sinal estendido (só 8 bits valem) */
} AC;

static struct {
    u16 ar[4], ix[4], wr[4], st[4];
    u16 cr, sr;
    Prod prod;
    AX ax[2];
    AC ac[2];
} r;

static u16 pc, control_reg;
static u8 exceptions;
static u16 stacks[4][32];
static u8 stack_ptr[4];
static u16 iram[0x1000], irom[0x1000], dram[0x1000], coef[0x800];
static u16 ifx[256];
static volatile LONG mailbox[2]; /* 0 = CPU -> DSP, 1 = DSP -> CPU; bit 31 = cheio */
static volatile LONG ext_int_pending, cpu_irq_pending;
static u8 *g_aram;
static u32 g_aram_mask;
volatile u64 g_dsp_cycles;

/* ---- acelerador (leitura de amostras da ARAM com decodificação ADPCM/PCM) ------ */

static struct {
    u32 start, end, cur;
    u16 format;
    s16 gain, yn1, yn2;
    u16 pred_scale, input;
    int reads_stopped;
} acc;

static void set_exception(int n) { exceptions |= (u8)(1 << n); }

static u8 aram_read(u32 a) { return g_aram[a & g_aram_mask]; }

static u16 acc_current_sample(void) {
    u16 v = 0;
    switch (acc.format & 3) {
    case 0: v = aram_read(acc.cur >> 1); v = (acc.cur & 1) ? (v & 0xF) : (v >> 4); break;
    case 1: v = aram_read(acc.cur); break;
    case 2: v = (u16)((aram_read(acc.cur * 2) << 8) | aram_read(acc.cur * 2 + 1)); break;
    default: break;
    }
    return v;
}

static void acc_set_cur(u32 a) { acc.cur = a & 0xBFFFFFFF; }

static u16 acc_read_raw(void) {
    u16 v = acc_current_sample();
    if ((acc.format & 3) != 3) acc.cur++;
    else acc.cur = (acc.cur & ~3u) | ((acc.cur + 1) & 3);
    if (acc.cur - 1 == acc.end) {
        acc.cur = acc.start;
        set_exception(3);
    }
    acc_set_cur(acc.cur);
    return v;
}

static void acc_write_raw(u16 v) {
    if (acc.cur & 0x80000000) {
        g_aram[(acc.cur * 2) & g_aram_mask] = (u8)(v >> 8);
        g_aram[(acc.cur * 2 + 1) & g_aram_mask] = (u8)v;
        acc.cur++;
        set_exception(4);
    }
}

static u16 acc_read_sample(void) {
    if (acc.reads_stopped) return 0;
    const s16 *coefs = (const s16 *)&ifx[0xA0];
    u32 decode = (acc.format >> 2) & 3, gain_scale = (acc.format >> 4) & 3;
    s16 raw = (decode == 1 || decode == 3) ? (s16)acc.input : (s16)acc_current_sample();
    int ci = (acc.pred_scale >> 4) & 7;
    s32 c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
    u16 val = 0;
    u8 step = 0;
    if (decode == 0) { /* ADPCM */
        raw &= 0xF;
        int scale = 1 << (acc.pred_scale & 0xF);
        if (raw >= 8) raw -= 16;
        s32 v32 = scale * raw + ((0x400 + c1 * acc.yn1 + c2 * acc.yn2) >> 11);
        if (v32 > 32767) v32 = 32767;
        if (v32 < -32768) v32 = -32768;
        val = (u16)v32;
        step = 2;
        acc.yn2 = acc.yn1;
        acc.yn1 = (s16)val;
        acc.cur++;
        if ((acc.end & 0xF) == 0 && acc.cur == acc.end) {
            acc.cur = acc.start + 1;
        } else if ((acc.end & 0xF) == 1 && acc.cur == acc.end - 1) {
            acc.cur = acc.start;
        } else if ((acc.cur & 15) == 0) {
            acc.pred_scale = aram_read((acc.cur & ~15u) >> 1);
            acc.cur += 2;
            step += 2;
        }
    } else { /* PCM */
        int sh = gain_scale == 0 ? 11 : gain_scale == 2 ? 16 : 0;
        s32 v32 = (((s32)acc.gain * raw) >> sh) + ((c1 * acc.yn1) >> sh) + ((c2 * acc.yn2) >> sh);
        val = (u16)(s16)v32;
        acc.yn2 = acc.yn1;
        acc.yn1 = (s16)val;
        step = 2;
        if (decode != 1) acc.cur++;
    }
    if (acc.cur == acc.end + step - 1) {
        acc.cur = acc.start;
        acc.reads_stopped = 1;
        set_exception(5);
    }
    acc_set_cur(acc.cur);
    return val;
}

/* ---- mailboxes, DMA e registradores IFX ------------------------------------------ */

static void mail_write_hi(int mb, u16 v) {
    LONG old = mailbox[mb];
    InterlockedExchange(&mailbox[mb], (LONG)((((u32)old & 0xFFFF) | ((u32)v << 16)) & 0x7FFFFFFF));
}
static int g_mail_log = -1;

static void mail_write_lo(int mb, u16 v) {
    LONG old = mailbox[mb];
    u32 nv = (((u32)old & 0xFFFF0000) | v) | 0x80000000;
    InterlockedExchange(&mailbox[mb], (LONG)nv);
    if (g_mail_log < 0) g_mail_log = getenv("MA_DSP_LOG") ? 400 : 0;
    if (g_mail_log > 0) {
        g_mail_log--;
        fprintf(stderr, "[dsp] %s %08X (pc %04X)\n", mb ? "DSP->CPU" : "CPU->DSP", nv & 0x7FFFFFFF | (nv & 0x80000000), pc);
    }
}
static u16 mail_read_hi(int mb) { return (u16)((u32)mailbox[mb] >> 16); }
static u16 mail_read_lo(int mb) {
    u32 v = (u32)InterlockedAnd(&mailbox[mb], 0x7FFFFFFF);
    return (u16)v;
}

static void do_dma(void) {
    u32 addr = ((u32)ifx[0xCE] << 16) | ifx[0xCF];
    u16 ctl = ifx[0xC9];
    u16 dsp_addr = (u16)(ifx[0xCD] * 2);
    u16 len = ifx[0xCB];
    if (len > 0x4000) {
        LOG("DSP: DMA inválido pc=%04X len=%X", pc, len);
        return;
    }
    u16 *mem = (ctl & 2) ? iram : dram;
    u16 *p = mem + (dsp_addr / 2 & 0xFFF);
    u8 *m = g_mem + (addr & MEM_MASK);
    if (ctl & 1) { /* DSP -> RAM */
        if (!(ctl & 2))
            for (u32 i = 0; i < len / 2u; i++) { m[2 * i] = (u8)(p[i] >> 8); m[2 * i + 1] = (u8)p[i]; }
    } else {
        for (u32 i = 0; i < len / 2u; i++) p[i] = (u16)((m[2 * i] << 8) | m[2 * i + 1]);
    }
}

static void write_ifx(u16 a, u16 v) {
    switch (a & 0xFF) {
    case 0xFB: /* DIRQ: interrompe a CPU */
        if (v & 1) InterlockedExchange(&cpu_irq_pending, 1);
        break;
    case 0xFC: mail_write_hi(1, v); break;
    case 0xFD: mail_write_lo(1, v); break;
    case 0xFE: mail_write_hi(0, v); break;
    case 0xFF: mail_write_lo(0, v); break;
    case 0xCB: /* DSBL: tamanho, dispara o DMA */
        ifx[0xCB] = v;
        if (!ifx[0xEF]) do_dma();
        ifx[0xC9] &= ~4;
        ifx[0xCB] = 0;
        break;
    case 0xD4: acc.start = ((u32)v << 16 | (acc.start & 0xFFFF)) & 0x3FFFFFFF; break;
    case 0xD5: acc.start = ((acc.start & 0xFFFF0000) | v) & 0x3FFFFFFF; break;
    case 0xD6: acc.end = ((u32)v << 16 | (acc.end & 0xFFFF)) & 0x3FFFFFFF; break;
    case 0xD7: acc.end = ((acc.end & 0xFFFF0000) | v) & 0x3FFFFFFF; break;
    case 0xD8: acc_set_cur((u32)v << 16 | (acc.cur & 0xFFFF)); break;
    case 0xD9: acc_set_cur((acc.cur & 0xFFFF0000) | v); break;
    case 0xD1: acc.format = v; break;
    case 0xDE: acc.gain = (s16)v; break;
    case 0xDB: acc.yn1 = (s16)v; break;
    case 0xDC: acc.yn2 = (s16)v; acc.reads_stopped = 0; break;
    case 0xDA: acc.pred_scale = v & 0x7F; break;
    case 0xD3: acc_write_raw(v); break;
    case 0xDF: acc.input = v; break;
    default: ifx[a & 0xFF] = v; break;
    }
}

static u16 read_ifx(u16 a) {
    switch (a & 0xFF) {
    case 0xFC: return mail_read_hi(1);
    case 0xFD: return mail_read_lo(1);
    case 0xFE: return mail_read_hi(0);
    case 0xFF: return mail_read_lo(0);
    case 0xD4: return (u16)(acc.start >> 16);
    case 0xD5: return (u16)acc.start;
    case 0xD6: return (u16)(acc.end >> 16);
    case 0xD7: return (u16)acc.end;
    case 0xD8: return (u16)(acc.cur >> 16);
    case 0xD9: return (u16)acc.cur;
    case 0xD1: return acc.format;
    case 0xDE: return (u16)acc.gain;
    case 0xDB: return (u16)acc.yn1;
    case 0xDC: return (u16)acc.yn2;
    case 0xDA: return acc.pred_scale;
    case 0xDD: return acc_read_sample();
    case 0xD3: return acc_read_raw();
    case 0xDF: return acc.input;
    default: return ifx[a & 0xFF];
    }
}

static u16 read_imem(u16 a) {
    switch (a >> 12) {
    case 0: return iram[a & 0xFFF];
    case 8: return irom[a & 0xFFF];
    default: return 0;
    }
}
static u16 read_dmem(u16 a) {
    switch (a >> 12) {
    case 0: return dram[a & 0xFFF];
    case 1: return coef[a & 0x7FF];
    case 0xF: return read_ifx(a);
    default: return 0;
    }
}
static void write_dmem(u16 a, u16 v) {
    if ((a >> 12) == 0) dram[a & 0xFFF] = v;
    else if ((a >> 12) == 0xF) write_ifx(a, v);
}

/* ---- pilhas, registradores e flags -------------------------------------------- */

static void push_stack(int s, u16 v) {
    stack_ptr[s] = (stack_ptr[s] + 1) & 31;
    stacks[s][stack_ptr[s]] = r.st[s];
    r.st[s] = v;
}
static u16 pop_stack(int s) {
    u16 v = r.st[s];
    r.st[s] = stacks[s][stack_ptr[s]];
    stack_ptr[s] = (stack_ptr[s] - 1) & 31;
    return v;
}

static int sr_set(u16 f) { return (r.sr & f) != 0; }

static s64 get_acc(int i) { return (s64)r.ac[i].val; }
static void set_acc(int i, s64 v) { r.ac[i].val = (u64)((v << 24) >> 24); }
static s64 get_acx(int i) { return (s32)(((u32)r.ax[i].h << 16) | r.ax[i].l); }
static s16 get_axl(int i) { return (s16)r.ax[i].l; }
static s16 get_axh(int i) { return (s16)r.ax[i].h; }
static s16 get_acm(int i) { return (s16)r.ac[i].m; }

static s64 get_prod(void) {
    s64 v = (s8)(u8)r.prod.h;
    v <<= 32;
    s64 low = r.prod.m;
    low += r.prod.m2;
    low <<= 16;
    low |= r.prod.l;
    return v + low;
}
static s64 get_prod_rounded(void) {
    s64 p = get_prod();
    return (p & 0x10000) ? (p + 0x8000) & ~0xFFFFll : (p + 0x7FFF) & ~0xFFFFll;
}
static void set_prod(s64 v) { r.prod.val = (u64)v & 0x000000FFFFFFFFFFull; }

static s64 mul_prod(u16 a, u16 b, int sign) {
    s64 p;
    if (sign == 1 && sr_set(SR_MUL_UNSIGNED)) p = (u32)(a * b);
    else if (sign == 2 && sr_set(SR_MUL_UNSIGNED)) p = (s64)a * (s16)b;
    else p = (s64)(s16)a * (s16)b;
    if (!sr_set(SR_MUL_MODIFY)) p <<= 1;
    return p;
}
static s64 mul_x(int axh0, int axh1, u16 v1, u16 v2) {
    if (!axh0 && !axh1) return mul_prod(v1, v2, 1);
    if (!axh0 && axh1) return mul_prod(v1, v2, 2);
    if (axh0 && !axh1) return mul_prod(v2, v1, 2);
    return mul_prod(v1, v2, 0);
}

static int over_s32(s64 v) { return v != (s32)v; }
static s64 conv_acc(s64 v) { return (v << 24) >> 24; }
static s64 round_acc(s64 v) { return (v & 0x10000) ? (v + 0x8000) & ~0xFFFFll : (v + 0x7FFF) & ~0xFFFFll; }

static void update_sr16(s16 v, int carry, int ovf, int os32) {
    r.sr &= ~SR_CMP_MASK;
    if (carry) r.sr |= SR_CARRY;
    if (ovf) r.sr |= SR_OVERFLOW | SR_OVERFLOW_STICKY;
    if (v == 0) r.sr |= SR_ARITH_ZERO;
    if (v < 0) r.sr |= SR_SIGN;
    if (os32) r.sr |= SR_OVER_S32;
    if (((u16)v >> 14) == 0 || ((u16)v >> 14) == 3) r.sr |= SR_TOP2BITS;
}
static void update_sr64(s64 v, int carry, int ovf) {
    r.sr &= ~SR_CMP_MASK;
    if (carry) r.sr |= SR_CARRY;
    if (ovf) r.sr |= SR_OVERFLOW | SR_OVERFLOW_STICKY;
    if (v == 0) r.sr |= SR_ARITH_ZERO;
    if (v < 0) r.sr |= SR_SIGN;
    if (over_s32(v)) r.sr |= SR_OVER_S32;
    if ((v & 0xC0000000) == 0 || (v & 0xC0000000) == 0xC0000000) r.sr |= SR_TOP2BITS;
}
static int is_overflow(s64 a, s64 b, s64 res) { return ((a ^ res) & (b ^ res)) < 0; }
static void update_sr64_add(s64 a, s64 b, s64 res) { update_sr64(res, (u64)a > (u64)res, is_overflow(a, b, res)); }
static void update_sr64_sub(s64 a, s64 b, s64 res) { update_sr64(res, (u64)a >= (u64)res, is_overflow(a, -b, res)); }
static void update_logic_zero(int z) { if (z) r.sr |= SR_LOGIC_ZERO; else r.sr &= ~SR_LOGIC_ZERO; }

static int cond(u8 c) {
    int carry = sr_set(SR_CARRY), ovf = sr_set(SR_OVERFLOW), os32 = sr_set(SR_OVER_S32);
    int less = sr_set(SR_OVERFLOW) != sr_set(SR_SIGN), zero = sr_set(SR_ARITH_ZERO);
    int lz = sr_set(SR_LOGIC_ZERO);
    int condb = (!(os32 || sr_set(SR_TOP2BITS))) || zero;
    switch (c & 0xF) {
    case 0x0: return !less;
    case 0x1: return less;
    case 0x2: return !less && !zero;
    case 0x3: return less || zero;
    case 0x4: return !zero;
    case 0x5: return zero;
    case 0x6: return !carry;
    case 0x7: return carry;
    case 0x8: return !os32;
    case 0x9: return os32;
    case 0xA: return !condb;
    case 0xB: return condb;
    case 0xC: return !lz;
    case 0xD: return lz;
    case 0xE: return ovf;
    default: return 1;
    }
}

static u16 inc_ar(int i) {
    u32 ar = r.ar[i], wr = r.wr[i], nar = ar + 1;
    if ((nar ^ ar) > ((wr | 1) << 1)) nar -= wr + 1;
    return (u16)nar;
}
static u16 dec_ar(int i) {
    u32 ar = r.ar[i], wr = r.wr[i], nar = ar + wr;
    if (((nar ^ ar) & ((wr | 1) << 1)) > wr) nar -= wr + 1;
    return (u16)nar;
}
static u16 add_ar(int i, s16 ix_) {
    u32 ar = r.ar[i], wr = r.wr[i];
    s32 ix = ix_;
    u32 mx = (wr | 1) << 1, nar = ar + (u32)ix, dar = (nar ^ ar ^ (u32)ix) & mx;
    if (ix >= 0) {
        if (dar > wr) nar -= wr + 1;
    } else {
        if ((((nar + wr + 1) ^ nar) & dar) <= wr) nar += wr + 1;
    }
    return (u16)nar;
}
static u16 sub_ar(int i, s16 ix_) {
    u32 ar = r.ar[i], wr = r.wr[i];
    s32 ix = ix_;
    u32 mx = (wr | 1) << 1, nar = ar - (u32)ix, dar = (nar ^ ar ^ ~(u32)ix) & mx;
    if ((u32)ix > 0xFFFF8000u) {
        if (dar > wr) nar -= wr + 1;
    } else {
        if ((((nar + wr + 1) ^ nar) & dar) <= wr) nar += wr + 1;
    }
    return (u16)nar;
}

static u16 op_read_reg(int reg) {
    reg &= 0x1F;
    switch (reg) {
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return pop_stack(reg - 0x0C);
    case 0x00: case 0x01: case 0x02: case 0x03: return r.ar[reg];
    case 0x04: case 0x05: case 0x06: case 0x07: return r.ix[reg - 4];
    case 0x08: case 0x09: case 0x0A: case 0x0B: return r.wr[reg - 8];
    case R_ACH0: case R_ACH1: return (u16)r.ac[reg - R_ACH0].h;
    case R_CR: return r.cr;
    case R_SR: return r.sr;
    case R_PRODL: return r.prod.l;
    case R_PRODM: return r.prod.m;
    case R_PRODH: return r.prod.h;
    case R_PRODM2: return r.prod.m2;
    case R_AXL0: case R_AXL1: return r.ax[reg - R_AXL0].l;
    case R_AXH0: case R_AXH1: return r.ax[reg - R_AXH0].h;
    case R_ACL0: case R_ACL1: return r.ac[reg - R_ACL0].l;
    default: { /* ACM0/ACM1 com saturação no modo de 40 bits */
        int i = reg - R_ACM0;
        if (sr_set(SR_40_MODE_BIT)) {
            s64 a = get_acc(i);
            if (a != (s32)a) return a > 0 ? 0x7FFF : 0x8000;
        }
        return r.ac[i].m;
    }
    }
}

static void op_write_reg(int reg, u16 v) {
    reg &= 0x1F;
    switch (reg) {
    case R_ACH0: case R_ACH1: r.ac[reg - R_ACH0].h = (u32)(s32)(s8)v; break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: push_stack(reg - 0x0C, v); break;
    case 0x00: case 0x01: case 0x02: case 0x03: r.ar[reg] = v; break;
    case 0x04: case 0x05: case 0x06: case 0x07: r.ix[reg - 4] = v; break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: r.wr[reg - 8] = v; break;
    case R_CR: r.cr = v & 0xFF; break;
    case R_SR: r.sr = v & ~SR_100; break;
    case R_PRODL: r.prod.l = v; break;
    case R_PRODM: r.prod.m = v; break;
    case R_PRODH: r.prod.h = v & 0xFF; break;
    case R_PRODM2: r.prod.m2 = v; break;
    case R_AXL0: case R_AXL1: r.ax[reg - R_AXL0].l = v; break;
    case R_AXH0: case R_AXH1: r.ax[reg - R_AXH0].h = v; break;
    case R_ACL0: case R_ACL1: r.ac[reg - R_ACL0].l = v; break;
    case R_ACM0: case R_ACM1: r.ac[reg - R_ACM0].m = v; break;
    }
}

static void extend_acc(int reg) {
    if (reg != R_ACM0 && reg != R_ACM1) return;
    if (!sr_set(SR_40_MODE_BIT)) return;
    int i = reg - R_ACM0;
    r.ac[i].h = (r.ac[i].m & 0x8000) ? 0xFFFFFFFF : 0;
    r.ac[i].l = 0;
}

/* gravações adiadas das operações estendidas (paralelas): aplicadas depois da principal */
static int wb_idx[5] = {-1, -1, -1, -1, -1};
static u16 wb_val[5];
static void wb(int i, int reg, u16 v) { wb_idx[i] = reg; wb_val[i] = v; }
static void wb_apply(void) {
    for (int i = 0; wb_idx[i] != -1; i++) {
        op_write_reg(wb_idx[i], wb_val[i]);
        wb_idx[i] = -1;
    }
}

/* ---- instruções ------------------------------------------------------------------ */

typedef void (*Op)(u16);

static u16 fetch(void) { return read_imem(pc++); }

/* tamanho de cada opcode (para pular instruções nos IFcc/LOOP com contador 0) */
static u8 op_size[65536];
static void skip_instruction(void) { pc += op_size[read_imem(pc)]; }

static void op_nop(u16 o) { (void)o; }
static void op_dar(u16 o) { r.ar[o & 3] = dec_ar(o & 3); }
static void op_iar(u16 o) { r.ar[o & 3] = inc_ar(o & 3); }
static void op_subarn(u16 o) { r.ar[o & 3] = sub_ar(o & 3, (s16)r.ix[o & 3]); }
static void op_addarn(u16 o) { r.ar[o & 3] = add_ar(o & 3, (s16)r.ix[(o >> 2) & 3]); }
static void op_halt(u16 o) { (void)o; control_reg |= CR_HALT; pc--; }
static void op_ret(u16 o) { if (cond((u8)o)) pc = pop_stack(ST_CALL); }
static void op_rti(u16 o) {
    if (!cond((u8)o)) return;
    r.sr = pop_stack(ST_DATA);
    pc = pop_stack(ST_CALL);
}
static void op_call(u16 o) {
    u16 dest = fetch();
    if (cond((u8)o)) { push_stack(ST_CALL, pc); pc = dest; }
}
static void op_ifcc(u16 o) { if (!cond((u8)o)) skip_instruction(); }
static void op_jcc(u16 o) {
    u16 dest = fetch();
    if (cond((u8)o)) pc = dest;
}
static void op_jmprcc(u16 o) { if (cond((u8)o)) pc = op_read_reg((o >> 5) & 7); }
static void op_callr(u16 o) {
    if (!cond((u8)o)) return;
    u16 a = op_read_reg((o >> 5) & 7);
    push_stack(ST_CALL, pc);
    pc = a;
}
static void op_sbclr(u16 o) { r.sr &= ~(1u << ((o & 7) + 6)); }
static void op_sbset(u16 o) { r.sr |= (u16)(1u << ((o & 7) + 6)); }

static void op_lsl(u16 o) { int d = (o >> 8) & 1; u64 a = (u64)get_acc(d); a <<= (o & 0x3F); set_acc(d, (s64)a); update_sr64(get_acc(d), 0, 0); }
static void op_lsr(u16 o) {
    int d = (o >> 8) & 1;
    u64 a = (u64)get_acc(d) & 0x000000FFFFFFFFFFull;
    int sh = (o & 0x3F) == 0 ? 0 : 0x40 - (o & 0x3F);
    a >>= sh;
    set_acc(d, (s64)a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_asl(u16 o) { op_lsl(o); }
static void op_asr(u16 o) {
    int d = (o >> 8) & 1;
    int sh = (o & 0x3F) == 0 ? 0 : 0x40 - (o & 0x3F);
    s64 a = get_acc(d) >> sh;
    set_acc(d, a);
    update_sr64(get_acc(d), 0, 0);
}
static int shift_amount(u16 v) { return (v & 0x3F) == 0 ? 0 : (v & 0x40) ? -0x40 + (v & 0x3F) : (v & 0x3F); }
static void op_lsrn(u16 o) {
    (void)o;
    int sh = shift_amount((u16)get_acm(1));
    u64 a = (u64)get_acc(0) & 0x000000FFFFFFFFFFull;
    if (sh > 0) a >>= sh; else if (sh < 0) a <<= -sh;
    set_acc(0, (s64)a);
    update_sr64(get_acc(0), 0, 0);
}
static void op_asrn(u16 o) {
    (void)o;
    int sh = shift_amount((u16)get_acm(1));
    s64 a = get_acc(0);
    if (sh > 0) a >>= sh; else if (sh < 0) a = (s64)((u64)a << -sh);
    set_acc(0, a);
    update_sr64(get_acc(0), 0, 0);
}
static void op_lsrnrx(u16 o) {
    int d = (o >> 8) & 1, s = (o >> 9) & 1;
    int sh = shift_amount(r.ax[s].h);
    u64 a = (u64)get_acc(d) & 0x000000FFFFFFFFFFull;
    if (sh > 0) a <<= sh; else if (sh < 0) a >>= -sh;
    set_acc(d, (s64)a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_asrnrx(u16 o) {
    int d = (o >> 8) & 1, s = (o >> 9) & 1;
    int sh = shift_amount(r.ax[s].h);
    s64 a = get_acc(d);
    if (sh > 0) a = (s64)((u64)a << sh); else if (sh < 0) a >>= -sh;
    set_acc(d, a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_lsrnr(u16 o) {
    int d = (o >> 8) & 1;
    int sh = shift_amount((u16)get_acm(1 - d));
    u64 a = (u64)get_acc(d) & 0x000000FFFFFFFFFFull;
    if (sh > 0) a <<= sh; else if (sh < 0) a >>= -sh;
    set_acc(d, (s64)a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_asrnr(u16 o) {
    int d = (o >> 8) & 1;
    int sh = shift_amount((u16)get_acm(1 - d));
    s64 a = get_acc(d);
    if (sh > 0) a = (s64)((u64)a << sh); else if (sh < 0) a >>= -sh;
    set_acc(d, a);
    update_sr64(get_acc(d), 0, 0);
}

static void op_lri(u16 o) { int reg = o & 0x1F; op_write_reg(reg, fetch()); extend_acc(reg); }
static void op_lr(u16 o) { int reg = o & 0x1F; u16 a = fetch(); op_write_reg(reg, read_dmem(a)); extend_acc(reg); }
static void op_sr(u16 o) { int reg = o & 0x1F; u16 a = fetch(); write_dmem(a, op_read_reg(reg)); }
static void op_mrr(u16 o) { int s = o & 0x1F, d = (o >> 5) & 0x1F; op_write_reg(d, op_read_reg(s)); extend_acc(d); }
static void op_si(u16 o) { u16 a = (u16)(s16)(s8)o; write_dmem(a, fetch()); }
static void op_lris(u16 o) { int reg = ((o >> 8) & 7) + R_AXL0; op_write_reg(reg, (u16)(s16)(s8)o); extend_acc(reg); }

static void op_addis(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), imm = (s64)(s8)o << 16;
    set_acc(d, a + imm);
    update_sr64_add(a, imm, get_acc(d));
}
static void op_cmpis(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), imm = (s64)(s8)o << 16;
    update_sr64_sub(a, imm, conv_acc(a - imm));
}
static void op_addi(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), imm = (s64)(s16)fetch() << 16;
    set_acc(d, a + imm);
    update_sr64_add(a, imm, get_acc(d));
}
static void op_xori(u16 o) { int d = (o >> 8) & 1; r.ac[d].m ^= fetch(); update_sr16((s16)r.ac[d].m, 0, 0, over_s32(get_acc(d))); }
static void op_andi(u16 o) { int d = (o >> 8) & 1; r.ac[d].m &= fetch(); update_sr16((s16)r.ac[d].m, 0, 0, over_s32(get_acc(d))); }
static void op_ori(u16 o) { int d = (o >> 8) & 1; r.ac[d].m |= fetch(); update_sr16((s16)r.ac[d].m, 0, 0, over_s32(get_acc(d))); }
static void op_cmpi(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), imm = (s64)(s16)fetch() << 16;
    update_sr64_sub(a, imm, conv_acc(a - imm));
}
static void op_andf(u16 o) { int d = (o >> 8) & 1; u16 imm = fetch(); update_logic_zero(((u16)get_acm(d) & imm) == 0); }
static void op_andcf(u16 o) { int d = (o >> 8) & 1; u16 imm = fetch(); update_logic_zero(((u16)get_acm(d) & imm) == imm); }

static void ilrr_common(u16 o, int mode) {
    int reg = o & 3, d = R_ACM0 + ((o >> 8) & 1);
    r.ac[d - R_ACM0].m = read_imem(r.ar[reg]);
    extend_acc(d);
    if (mode == 1) r.ar[reg] = dec_ar(reg);
    else if (mode == 2) r.ar[reg] = inc_ar(reg);
    else if (mode == 3) r.ar[reg] = add_ar(reg, (s16)r.ix[reg]);
}
static void op_ilrr(u16 o) { ilrr_common(o, 0); }
static void op_ilrrd(u16 o) { ilrr_common(o, 1); }
static void op_ilrri(u16 o) { ilrr_common(o, 2); }
static void op_ilrrn(u16 o) { ilrr_common(o, 3); }

static void start_loop(u16 loop_pc, u16 cnt) {
    push_stack(ST_CALL, pc);
    push_stack(ST_LOOP_ADDR, loop_pc);
    push_stack(ST_LOOP_COUNT, cnt);
}
static void op_loop(u16 o) {
    u16 cnt = op_read_reg(o & 0x1F), lp = pc;
    if (cnt) start_loop(lp, cnt); else skip_instruction();
}
static void op_loopi(u16 o) {
    u16 cnt = o & 0xFF, lp = pc;
    if (cnt) start_loop(lp, cnt); else skip_instruction();
}
static void op_bloop(u16 o) {
    u16 cnt = op_read_reg(o & 0x1F), lp = fetch();
    if (cnt) start_loop(lp, cnt); else { pc = lp; skip_instruction(); }
}
static void op_bloopi(u16 o) {
    u16 cnt = o & 0xFF, lp = fetch();
    if (cnt) start_loop(lp, cnt); else { pc = lp; skip_instruction(); }
}

static void lrr_common(u16 o, int mode) {
    int s = (o >> 5) & 3, d = o & 0x1F;
    op_write_reg(d, read_dmem(op_read_reg(s)));
    extend_acc(d);
    if (mode == 1) r.ar[s] = dec_ar(s);
    else if (mode == 2) r.ar[s] = inc_ar(s);
    else if (mode == 3) r.ar[s] = add_ar(s, (s16)r.ix[s]);
}
static void op_lrr(u16 o) { lrr_common(o, 0); }
static void op_lrrd(u16 o) { lrr_common(o, 1); }
static void op_lrri(u16 o) { lrr_common(o, 2); }
static void op_lrrn(u16 o) { lrr_common(o, 3); }
static void srr_common(u16 o, int mode) {
    int d = (o >> 5) & 3, s = o & 0x1F;
    write_dmem(r.ar[d], op_read_reg(s));
    if (mode == 1) r.ar[d] = dec_ar(d);
    else if (mode == 2) r.ar[d] = inc_ar(d);
    else if (mode == 3) r.ar[d] = add_ar(d, (s16)r.ix[d]);
}
static void op_srr(u16 o) { srr_common(o, 0); }
static void op_srrd(u16 o) { srr_common(o, 1); }
static void op_srri(u16 o) { srr_common(o, 2); }
static void op_srrn(u16 o) { srr_common(o, 3); }

static void op_lrs(u16 o) {
    int reg = ((o >> 8) & 7) + 0x18;
    op_write_reg(reg, read_dmem((u16)((r.cr << 8) | (o & 0xFF))));
    extend_acc(reg);
}
static void op_srsh(u16 o) { write_dmem((u16)((r.cr << 8) | (o & 0xFF)), op_read_reg(((o >> 8) & 1) + R_ACH0)); }
static void op_srs(u16 o) { write_dmem((u16)((r.cr << 8) | (o & 0xFF)), op_read_reg(((o >> 8) & 3) + R_ACL0)); }

/* lógicas sobre o meio do acumulador */
#define LOGIC16(name, expr)                                                  \
    static void name(u16 o) {                                               \
        int d = (o >> 8) & 1, s = (o >> 9) & 1;                             \
        (void)s;                                                            \
        u16 m = (u16)(expr);                                                \
        r.ac[d].m = m;                                                      \
        update_sr16((s16)m, 0, 0, over_s32(get_acc(d)));                    \
    }
LOGIC16(op_xorr, r.ac[d].m ^ r.ax[s].h)
LOGIC16(op_andr, r.ac[d].m & r.ax[s].h)
LOGIC16(op_orr, r.ac[d].m | r.ax[s].h)
LOGIC16(op_andc, r.ac[d].m & r.ac[1 - d].m)
LOGIC16(op_orc, r.ac[d].m | r.ac[1 - d].m)
LOGIC16(op_xorc, r.ac[d].m ^ r.ac[1 - d].m)
LOGIC16(op_notc, r.ac[d].m ^ 0xFFFF)

static s64 ax_reg16(int sreg) {
    switch (sreg) {
    case R_AXL0: case R_AXL1: return (s16)r.ax[sreg - R_AXL0].l;
    case R_AXH0: case R_AXH1: return (s16)r.ax[sreg - R_AXH0].h;
    default: return 0;
    }
}
static void op_addr(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), x = ax_reg16(((o >> 9) & 3) + R_AXL0) << 16;
    set_acc(d, a + x);
    update_sr64_add(a, x, get_acc(d));
}
static void op_addax(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), x = get_acx((o >> 9) & 1); set_acc(d, a + x); update_sr64_add(a, x, get_acc(d)); }
static void op_add(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), b = get_acc(1 - d); set_acc(d, a + b); update_sr64_add(a, b, get_acc(d)); }
static void op_addp(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), p = get_prod(); set_acc(d, a + p); update_sr64_add(a, p, get_acc(d)); }
static void op_subr(u16 o) {
    int d = (o >> 8) & 1;
    s64 a = get_acc(d), x = ax_reg16(((o >> 9) & 3) + R_AXL0) << 16;
    set_acc(d, a - x);
    update_sr64_sub(a, x, get_acc(d));
}
static void op_subax(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), x = get_acx((o >> 9) & 1); set_acc(d, a - x); update_sr64_sub(a, x, get_acc(d)); }
static void op_sub(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), b = get_acc(1 - d); set_acc(d, a - b); update_sr64_sub(a, b, get_acc(d)); }
static void op_subp(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d), p = get_prod(); set_acc(d, a - p); update_sr64_sub(a, p, get_acc(d)); }
static void op_movr(u16 o) { int d = (o >> 8) & 1; s64 x = ax_reg16(((o >> 9) & 3) + R_AXL0) << 16; set_acc(d, x); update_sr64(x, 0, 0); }
static void op_movax(u16 o) { int d = (o >> 8) & 1; s64 x = get_acx((o >> 9) & 1); set_acc(d, x); update_sr64(x, 0, 0); }
static void op_mov(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(1 - d); set_acc(d, a); update_sr64(a, 0, 0); }
static void op_movp(u16 o) { int d = (o >> 8) & 1; s64 a = get_prod(); set_acc(d, a); update_sr64(a, 0, 0); }
static void op_addaxl(u16 o) {
    int d = (o >> 8) & 1, s = (o >> 9) & 1;
    u64 a = (u64)get_acc(d);
    u16 x = (u16)get_axl(s);
    u64 res = a + x;
    set_acc(d, (s64)res);
    update_sr64_add((s64)a, x, get_acc(d));
}
static void op_incm(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d); set_acc(d, a + 0x10000); update_sr64_add(a, 0x10000, get_acc(d)); }
static void op_inc(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d); set_acc(d, a + 1); update_sr64_add(a, 1, get_acc(d)); }
static void op_decm(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d); set_acc(d, a - 0x10000); update_sr64_sub(a, 0x10000, get_acc(d)); }
static void op_dec(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d); set_acc(d, a - 1); update_sr64_sub(a, 1, get_acc(d)); }
static void op_neg(u16 o) { int d = (o >> 8) & 1; s64 a = get_acc(d); set_acc(d, 0 - a); update_sr64_sub(0, a, get_acc(d)); }
static void op_movnp(u16 o) { int d = (o >> 8) & 1; s64 a = -get_prod(); set_acc(d, a); update_sr64(a, 0, 0); }

static void op_nx(u16 o) { (void)o; }
static void op_clr(u16 o) { int d = (o >> 11) & 1; set_acc(d, 0); update_sr64(0, 0, 0); }
static void op_cmp(u16 o) { (void)o; s64 a = get_acc(0), b = get_acc(1); update_sr64_sub(a, b, conv_acc(a - b)); }
static void op_mulaxh(u16 o) { (void)o; s16 v = get_axh(0); set_prod(mul_prod((u16)v, (u16)v, 0)); }
static void op_clrp(u16 o) { (void)o; r.prod.l = 0; r.prod.m = 0xFFF0; r.prod.h = 0x00FF; r.prod.m2 = 0x0010; }
static void op_tstprod(u16 o) { (void)o; update_sr64(get_prod(), 0, 0); }
static void op_tstaxh(u16 o) { update_sr16(get_axh((o >> 8) & 1), 0, 0, 0); }
static void op_srbith(u16 o) {
    switch ((o >> 8) & 7) {
    case 2: r.sr &= ~SR_MUL_MODIFY; break;
    case 3: r.sr |= SR_MUL_MODIFY; break;
    case 4: r.sr &= ~SR_MUL_UNSIGNED; break;
    case 5: r.sr |= SR_MUL_UNSIGNED; break;
    case 6: r.sr &= ~SR_40_MODE_BIT; break;
    case 7: r.sr |= SR_40_MODE_BIT; break;
    }
}
static void op_mul(u16 o) { int s = (o >> 11) & 1; set_prod(mul_prod((u16)get_axh(s), (u16)get_axl(s), 0)); }
static void op_asr16(u16 o) { int d = (o >> 11) & 1; s64 a = get_acc(d) >> 16; set_acc(d, a); update_sr64(get_acc(d), 0, 0); }
static void mul_family(u16 o, int kind, s64 accsrc_mode) {
    /* kind: 0 = AX(l,h) de (o>>11), 1 = MULX (o>>11 treg, o>>12 sreg), 2 = MULC */
    int rreg = (o >> 8) & 1;
    s64 acc;
    if (accsrc_mode == 0) acc = get_acc(rreg) + get_prod();      /* AC */
    else if (accsrc_mode == 1) acc = get_prod();                 /* MV */
    else acc = get_prod_rounded();                               /* MVZ */
    s64 prod;
    if (kind == 0) {
        int s = (o >> 11) & 1;
        prod = mul_prod((u16)get_axl(s), (u16)get_axh(s), 0);
    } else if (kind == 1) {
        int t = (o >> 11) & 1, s = (o >> 12) & 1;
        u16 v1 = s == 0 ? (u16)get_axl(0) : (u16)get_axh(0), v2 = t == 0 ? (u16)get_axl(1) : (u16)get_axh(1);
        prod = mul_x(s, t, v1, v2);
    } else {
        int t = (o >> 11) & 1, s = (o >> 12) & 1;
        prod = mul_prod((u16)get_acm(s), (u16)get_axh(t), 0);
    }
    set_prod(prod);
    set_acc(rreg, acc);
    update_sr64(get_acc(rreg), 0, 0);
}
static void op_mulac(u16 o) { mul_family(o, 0, 0); }
static void op_mulmv(u16 o) { mul_family(o, 0, 1); }
static void op_mulmvz(u16 o) { mul_family(o, 0, 2); }
static void op_mulx(u16 o) {
    int t = (o >> 11) & 1, s = (o >> 12) & 1;
    u16 v1 = s == 0 ? (u16)get_axl(0) : (u16)get_axh(0), v2 = t == 0 ? (u16)get_axl(1) : (u16)get_axh(1);
    set_prod(mul_x(s, t, v1, v2));
}
static void op_mulxac(u16 o) { mul_family(o, 1, 0); }
static void op_mulxmv(u16 o) { mul_family(o, 1, 1); }
static void op_mulxmvz(u16 o) { mul_family(o, 1, 2); }
static void op_abs(u16 o) {
    int d = (o >> 11) & 1;
    s64 a = get_acc(d);
    if (a < 0) a = 0 - a;
    set_acc(d, a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_tst(u16 o) { update_sr64(get_acc((o >> 11) & 1), 0, 0); }
static void op_mulc(u16 o) { int t = (o >> 11) & 1, s = (o >> 12) & 1; set_prod(mul_prod((u16)get_acm(s), (u16)get_axh(t), 0)); }
static void op_cmpaxh(u16 o) {
    int rr = (o >> 12) & 1, s = (o >> 11) & 1;
    s64 a = get_acc(s), x = (s64)get_axh(rr) << 16;
    update_sr64_sub(a, x, conv_acc(a - x));
}
static void op_mulcac(u16 o) { mul_family(o, 2, 0); }
static void op_mulcmv(u16 o) { mul_family(o, 2, 1); }
static void op_mulcmvz(u16 o) { mul_family(o, 2, 2); }
static void op_maddx(u16 o) {
    int t = (o >> 8) & 1, s = (o >> 9) & 1;
    u16 v1 = s == 0 ? (u16)get_axl(0) : (u16)get_axh(0), v2 = t == 0 ? (u16)get_axl(1) : (u16)get_axh(1);
    set_prod(get_prod() + mul_prod(v1, v2, 0));
}
static void op_msubx(u16 o) {
    int t = (o >> 8) & 1, s = (o >> 9) & 1;
    u16 v1 = s == 0 ? (u16)get_axl(0) : (u16)get_axh(0), v2 = t == 0 ? (u16)get_axl(1) : (u16)get_axh(1);
    set_prod(get_prod() - mul_prod(v1, v2, 0));
}
static void op_maddc(u16 o) { int t = (o >> 8) & 1, s = (o >> 9) & 1; set_prod(get_prod() + mul_prod((u16)get_acm(s), (u16)get_axh(t), 0)); }
static void op_msubc(u16 o) { int t = (o >> 8) & 1, s = (o >> 9) & 1; set_prod(get_prod() - mul_prod((u16)get_acm(s), (u16)get_axh(t), 0)); }
static void op_lsl16(u16 o) { int d = (o >> 8) & 1; s64 a = (s64)((u64)get_acc(d) << 16); set_acc(d, a); update_sr64(get_acc(d), 0, 0); }
static void op_madd(u16 o) { int s = (o >> 8) & 1; set_prod(get_prod() + mul_prod((u16)get_axl(s), (u16)get_axh(s), 0)); }
static void op_lsr16(u16 o) {
    int d = (o >> 8) & 1;
    u64 a = ((u64)get_acc(d) & 0x000000FFFFFFFFFFull) >> 16;
    set_acc(d, (s64)a);
    update_sr64(get_acc(d), 0, 0);
}
static void op_msub(u16 o) { int s = (o >> 8) & 1; set_prod(get_prod() - mul_prod((u16)get_axl(s), (u16)get_axh(s), 0)); }
static void op_addpaxz(u16 o) {
    int d = (o >> 8) & 1, s = (o >> 9) & 1;
    s64 oldprod = get_prod(), prod = get_prod_rounded(), ax = get_acx(s);
    s64 res = prod + (ax & ~0xFFFFll);
    set_acc(d, res);
    res = get_acc(d);
    update_sr64(res, (u64)oldprod > (u64)res, 0);
}
static void op_clrl(u16 o) { int d = (o >> 8) & 1; s64 a = round_acc(get_acc(d)); set_acc(d, a); update_sr64(a, 0, 0); }
static void op_movpz(u16 o) { int d = (o >> 8) & 1; s64 a = get_prod_rounded(); set_acc(d, a); update_sr64(a, 0, 0); }

/* ---- operações estendidas (paralelas) ------------------------------------------------- */

static int same_area(u16 a, u16 b) { return (a >> 10) == (b >> 10); }

static void ext_nop(u16 o) { (void)o; }
static void ext_dr(u16 o) { wb(0, o & 3, dec_ar(o & 3)); }
static void ext_ir(u16 o) { wb(0, o & 3, inc_ar(o & 3)); }
static void ext_nr(u16 o) { wb(0, o & 3, add_ar(o & 3, (s16)r.ix[o & 3])); }
static void ext_mv(u16 o) { wb(0, ((o >> 2) & 3) + R_AXL0, op_read_reg((o & 3) + R_ACL0)); }
static void ext_s(u16 o) {
    int d = o & 3, s = ((o >> 3) & 3) + R_ACL0;
    write_dmem(r.ar[d], op_read_reg(s));
    wb(0, d, inc_ar(d));
}
static void ext_sn(u16 o) {
    int d = o & 3, s = ((o >> 3) & 3) + R_ACL0;
    write_dmem(r.ar[d], op_read_reg(s));
    wb(0, d, add_ar(d, (s16)r.ix[d]));
}
static void ext_l_common(u16 o, int n) {
    int s = o & 3, d = ((o >> 3) & 7) + R_AXL0;
    u16 next = n ? add_ar(s, (s16)r.ix[s]) : inc_ar(s);
    if (d >= R_ACM0 && sr_set(SR_40_MODE_BIT)) {
        u16 v = read_dmem(r.ar[s]);
        wb(0, d - R_ACM0 + R_ACH0, (v & 0x8000) ? 0xFFFF : 0);
        wb(1, d, v);
        wb(2, d - R_ACM0 + R_ACL0, 0);
        wb(3, s, next);
    } else {
        wb(0, d, read_dmem(r.ar[s]));
        wb(1, s, next);
    }
}
static void ext_l(u16 o) { ext_l_common(o, 0); }
static void ext_ln(u16 o) { ext_l_common(o, 1); }
/* LS/SL e variantes: guarda ac(m) em ar3 (LS) ou ar0 (SL), carrega ax de ar0 (LS) ou ar3 (SL) */
static void ext_ls_common(u16 o, int store_ar, int load_ar, int n0, int m3) {
    int s = (o & 1) + R_ACM0, d = ((o >> 4) & 3) + R_AXL0;
    write_dmem(r.ar[store_ar], op_read_reg(s));
    wb(0, d, read_dmem(r.ar[load_ar]));
    wb(1, 3, m3 ? add_ar(3, (s16)r.ix[3]) : inc_ar(3));
    wb(2, 0, n0 ? add_ar(0, (s16)r.ix[0]) : inc_ar(0));
}
static void ext_ls(u16 o) { ext_ls_common(o, 3, 0, 0, 0); }
static void ext_lsn(u16 o) { ext_ls_common(o, 3, 0, 1, 0); }
static void ext_lsm(u16 o) { ext_ls_common(o, 3, 0, 0, 1); }
static void ext_lsnm(u16 o) { ext_ls_common(o, 3, 0, 1, 1); }
static void ext_sl(u16 o) { ext_ls_common(o, 0, 3, 0, 0); }
static void ext_sln(u16 o) { ext_ls_common(o, 0, 3, 1, 0); }
static void ext_slm(u16 o) { ext_ls_common(o, 0, 3, 0, 1); }
static void ext_slnm(u16 o) { ext_ls_common(o, 0, 3, 1, 1); }
static void ext_ld_common(u16 o, int n, int m) {
    int d = (o >> 5) & 1, rr = (o >> 4) & 1, s = o & 3;
    wb(0, (d << 1) + R_AXL0, read_dmem(r.ar[s]));
    wb(1, (rr << 1) + R_AXL1, read_dmem(same_area(r.ar[s], r.ar[3]) ? r.ar[s] : r.ar[3]));
    wb(2, s, n ? add_ar(s, (s16)r.ix[s]) : inc_ar(s));
    wb(3, 3, m ? add_ar(3, (s16)r.ix[3]) : inc_ar(3));
}
static void ext_ld(u16 o) { ext_ld_common(o, 0, 0); }
static void ext_ldn(u16 o) { ext_ld_common(o, 1, 0); }
static void ext_ldm(u16 o) { ext_ld_common(o, 0, 1); }
static void ext_ldnm(u16 o) { ext_ld_common(o, 1, 1); }
static void ext_ldax_common(u16 o, int n, int m) {
    int s = (o >> 5) & 1, rr = (o >> 4) & 1;
    wb(0, rr + R_AXH0, read_dmem(r.ar[s]));
    wb(1, rr + R_AXL0, read_dmem(same_area(r.ar[s], r.ar[3]) ? r.ar[s] : r.ar[3]));
    wb(2, s, n ? add_ar(s, (s16)r.ix[s]) : inc_ar(s));
    wb(3, 3, m ? add_ar(3, (s16)r.ix[3]) : inc_ar(3));
}
static void ext_ldax(u16 o) { ext_ldax_common(o, 0, 0); }
static void ext_ldaxn(u16 o) { ext_ldax_common(o, 1, 0); }
static void ext_ldaxm(u16 o) { ext_ldax_common(o, 0, 1); }
static void ext_ldaxnm(u16 o) { ext_ldax_common(o, 1, 1); }

/* ---- tabelas -------------------------------------------------------------------- */

typedef struct { u16 opcode, mask; u8 size, ext; Op fn; } OpDef;

/* mesma ordem da tabela de referência: vale a primeira entrada que casa */
static const OpDef k_ops[] = {
    {0x0000, 0xfffc, 1, 0, op_nop}, {0x0004, 0xfffc, 1, 0, op_dar}, {0x0008, 0xfffc, 1, 0, op_iar},
    {0x000c, 0xfffc, 1, 0, op_subarn}, {0x0010, 0xfff0, 1, 0, op_addarn}, {0x0021, 0xffff, 1, 0, op_halt},
    {0x02d0, 0xfff0, 1, 0, op_ret}, {0x02f0, 0xfff0, 1, 0, op_rti}, {0x02b0, 0xfff0, 2, 0, op_call},
    {0x0270, 0xfff0, 1, 0, op_ifcc}, {0x0290, 0xfff0, 2, 0, op_jcc}, {0x1700, 0xff10, 1, 0, op_jmprcc},
    {0x1710, 0xff10, 1, 0, op_callr}, {0x1200, 0xff00, 1, 0, op_sbclr}, {0x1300, 0xff00, 1, 0, op_sbset},
    {0x1400, 0xfec0, 1, 0, op_lsl}, {0x1440, 0xfec0, 1, 0, op_lsr}, {0x1480, 0xfec0, 1, 0, op_asl},
    {0x14c0, 0xfec0, 1, 0, op_asr}, {0x02ca, 0xffff, 1, 0, op_lsrn}, {0x02cb, 0xffff, 1, 0, op_asrn},
    {0x0080, 0xffe0, 2, 0, op_lri}, {0x00c0, 0xffe0, 2, 0, op_lr}, {0x00e0, 0xffe0, 2, 0, op_sr},
    {0x1c00, 0xfc00, 1, 0, op_mrr}, {0x1600, 0xff00, 2, 0, op_si}, {0x0400, 0xfe00, 1, 0, op_addis},
    {0x0600, 0xfe00, 1, 0, op_cmpis}, {0x0800, 0xf800, 1, 0, op_lris}, {0x0200, 0xfeff, 2, 0, op_addi},
    {0x0220, 0xfeff, 2, 0, op_xori}, {0x0240, 0xfeff, 2, 0, op_andi}, {0x0260, 0xfeff, 2, 0, op_ori},
    {0x0280, 0xfeff, 2, 0, op_cmpi}, {0x02a0, 0xfeff, 2, 0, op_andf}, {0x02c0, 0xfeff, 2, 0, op_andcf},
    {0x0210, 0xfefc, 1, 0, op_ilrr}, {0x0214, 0xfefc, 1, 0, op_ilrrd}, {0x0218, 0xfefc, 1, 0, op_ilrri},
    {0x021c, 0xfefc, 1, 0, op_ilrrn}, {0x0040, 0xffe0, 1, 0, op_loop}, {0x0060, 0xffe0, 2, 0, op_bloop},
    {0x1000, 0xff00, 1, 0, op_loopi}, {0x1100, 0xff00, 2, 0, op_bloopi}, {0x1800, 0xff80, 1, 0, op_lrr},
    {0x1880, 0xff80, 1, 0, op_lrrd}, {0x1900, 0xff80, 1, 0, op_lrri}, {0x1980, 0xff80, 1, 0, op_lrrn},
    {0x1a00, 0xff80, 1, 0, op_srr}, {0x1a80, 0xff80, 1, 0, op_srrd}, {0x1b00, 0xff80, 1, 0, op_srri},
    {0x1b80, 0xff80, 1, 0, op_srrn}, {0x2000, 0xf800, 1, 0, op_lrs}, {0x2800, 0xfe00, 1, 0, op_srsh},
    {0x2c00, 0xfc00, 1, 0, op_srs},
    {0x3000, 0xfc80, 1, 1, op_xorr}, {0x3400, 0xfc80, 1, 1, op_andr}, {0x3800, 0xfc80, 1, 1, op_orr},
    {0x3c00, 0xfe80, 1, 1, op_andc}, {0x3e00, 0xfe80, 1, 1, op_orc}, {0x3080, 0xfe80, 1, 1, op_xorc},
    {0x3280, 0xfe80, 1, 1, op_notc}, {0x3480, 0xfc80, 1, 1, op_lsrnrx}, {0x3880, 0xfc80, 1, 1, op_asrnrx},
    {0x3c80, 0xfe80, 1, 1, op_lsrnr}, {0x3e80, 0xfe80, 1, 1, op_asrnr},
    {0x4000, 0xf800, 1, 1, op_addr}, {0x4800, 0xfc00, 1, 1, op_addax}, {0x4c00, 0xfe00, 1, 1, op_add},
    {0x4e00, 0xfe00, 1, 1, op_addp}, {0x5000, 0xf800, 1, 1, op_subr}, {0x5800, 0xfc00, 1, 1, op_subax},
    {0x5c00, 0xfe00, 1, 1, op_sub}, {0x5e00, 0xfe00, 1, 1, op_subp}, {0x6000, 0xf800, 1, 1, op_movr},
    {0x6800, 0xfc00, 1, 1, op_movax}, {0x6c00, 0xfe00, 1, 1, op_mov}, {0x6e00, 0xfe00, 1, 1, op_movp},
    {0x7000, 0xfc00, 1, 1, op_addaxl}, {0x7400, 0xfe00, 1, 1, op_incm}, {0x7600, 0xfe00, 1, 1, op_inc},
    {0x7800, 0xfe00, 1, 1, op_decm}, {0x7a00, 0xfe00, 1, 1, op_dec}, {0x7c00, 0xfe00, 1, 1, op_neg},
    {0x7e00, 0xfe00, 1, 1, op_movnp}, {0x8000, 0xf700, 1, 1, op_nx}, {0x8100, 0xf700, 1, 1, op_clr},
    {0x8200, 0xff00, 1, 1, op_cmp}, {0x8300, 0xff00, 1, 1, op_mulaxh}, {0x8400, 0xff00, 1, 1, op_clrp},
    {0x8500, 0xff00, 1, 1, op_tstprod}, {0x8600, 0xfe00, 1, 1, op_tstaxh}, {0x8a00, 0xff00, 1, 1, op_srbith},
    {0x8b00, 0xff00, 1, 1, op_srbith}, {0x8c00, 0xff00, 1, 1, op_srbith}, {0x8d00, 0xff00, 1, 1, op_srbith},
    {0x8e00, 0xff00, 1, 1, op_srbith}, {0x8f00, 0xff00, 1, 1, op_srbith}, {0x9000, 0xf700, 1, 1, op_mul},
    {0x9100, 0xf700, 1, 1, op_asr16}, {0x9200, 0xf600, 1, 1, op_mulmvz}, {0x9400, 0xf600, 1, 1, op_mulac},
    {0x9600, 0xf600, 1, 1, op_mulmv}, {0xa000, 0xe700, 1, 1, op_mulx}, {0xa100, 0xf700, 1, 1, op_abs},
    {0xa200, 0xe600, 1, 1, op_mulxmvz}, {0xa400, 0xe600, 1, 1, op_mulxac}, {0xa600, 0xe600, 1, 1, op_mulxmv},
    {0xb100, 0xf700, 1, 1, op_tst}, {0xc000, 0xe700, 1, 1, op_mulc}, {0xc100, 0xe700, 1, 1, op_cmpaxh},
    {0xc200, 0xe600, 1, 1, op_mulcmvz}, {0xc400, 0xe600, 1, 1, op_mulcac}, {0xc600, 0xe600, 1, 1, op_mulcmv},
    {0xe000, 0xfc00, 1, 1, op_maddx}, {0xe400, 0xfc00, 1, 1, op_msubx}, {0xe800, 0xfc00, 1, 1, op_maddc},
    {0xec00, 0xfc00, 1, 1, op_msubc}, {0xf000, 0xfe00, 1, 1, op_lsl16}, {0xf200, 0xfe00, 1, 1, op_madd},
    {0xf400, 0xfe00, 1, 1, op_lsr16}, {0xf600, 0xfe00, 1, 1, op_msub}, {0xf800, 0xfc00, 1, 1, op_addpaxz},
    {0xfc00, 0xfe00, 1, 1, op_clrl}, {0xfe00, 0xfe00, 1, 1, op_movpz},
};

static const OpDef k_ext[] = {
    {0x0000, 0x00fc, 0, 0, ext_nop}, {0x0004, 0x00fc, 0, 0, ext_dr}, {0x0008, 0x00fc, 0, 0, ext_ir},
    {0x000c, 0x00fc, 0, 0, ext_nr}, {0x0010, 0x00f0, 0, 0, ext_mv}, {0x0020, 0x00e4, 0, 0, ext_s},
    {0x0024, 0x00e4, 0, 0, ext_sn}, {0x0040, 0x00c4, 0, 0, ext_l}, {0x0044, 0x00c4, 0, 0, ext_ln},
    {0x0080, 0x00ce, 0, 0, ext_ls}, {0x0082, 0x00ce, 0, 0, ext_sl}, {0x0084, 0x00ce, 0, 0, ext_lsn},
    {0x0086, 0x00ce, 0, 0, ext_sln}, {0x0088, 0x00ce, 0, 0, ext_lsm}, {0x008a, 0x00ce, 0, 0, ext_slm},
    {0x008c, 0x00ce, 0, 0, ext_lsnm}, {0x008e, 0x00ce, 0, 0, ext_slnm}, {0x00c3, 0x00cf, 0, 0, ext_ldax},
    {0x00c7, 0x00cf, 0, 0, ext_ldaxn}, {0x00cb, 0x00cf, 0, 0, ext_ldaxm}, {0x00cf, 0x00cf, 0, 0, ext_ldaxnm},
    {0x00c0, 0x00cc, 0, 0, ext_ld}, {0x00c4, 0x00cc, 0, 0, ext_ldn}, {0x00c8, 0x00cc, 0, 0, ext_ldm},
    {0x00cc, 0x00cc, 0, 0, ext_ldnm},
};

static Op op_table[65536];
static u8 op_ext[65536];
static Op ext_table[256];

static void build_tables(void) {
    for (u32 i = 0; i < 65536; i++) {
        op_table[i] = op_nop;
        op_size[i] = 1;
        for (size_t k = 0; k < sizeof k_ops / sizeof *k_ops; k++)
            if ((i & k_ops[k].mask) == k_ops[k].opcode) {
                op_table[i] = k_ops[k].fn;
                op_size[i] = k_ops[k].size;
                op_ext[i] = k_ops[k].ext;
                break;
            }
    }
    for (u32 i = 0; i < 256; i++) {
        ext_table[i] = ext_nop;
        for (size_t k = 0; k < sizeof k_ext / sizeof *k_ext; k++)
            if ((i & k_ext[k].mask) == k_ext[k].opcode) {
                ext_table[i] = k_ext[k].fn;
                break;
            }
    }
}

/* ---- execução ------------------------------------------------------------------- */

static void check_exceptions(void) {
    if (!exceptions) return;
    for (int i = 7; i > 0; i--) {
        if (!(exceptions & (1 << i))) continue;
        if (sr_set(SR_INT_ENABLE) || i == 7) {
            push_stack(ST_CALL, pc);
            push_stack(ST_DATA, r.sr);
            pc = (u16)(i * 2);
            exceptions &= (u8)~(1 << i);
            if (i == 7) r.sr &= ~SR_EXT_INT_ENABLE;
            else r.sr &= ~SR_INT_ENABLE;
            return;
        }
    }
}

static void handle_loop(void) {
    u16 call_addr = r.st[0], loop_addr = r.st[2];
    if (loop_addr > 0 && r.st[3] > 0 && (u16)(pc - 1) == loop_addr) {
        r.st[3]--;
        if (r.st[3] > 0) {
            pc = call_addr;
        } else {
            pop_stack(ST_CALL);
            pop_stack(ST_LOOP_ADDR);
            pop_stack(ST_LOOP_COUNT);
        }
    }
}

static void step(void) {
    check_exceptions();
    u16 o = fetch();
    if (op_ext[o]) {
        ext_table[(o >> 12) == 3 ? (o & 0x7F) : (o & 0xFF)](o);
        op_table[o](o);
        wb_apply();
    } else {
        op_table[o](o);
    }
    handle_loop();
}

static void external_interrupt(void) {
    if (!sr_set(SR_EXT_INT_ENABLE)) return;
    set_exception(7);
    control_reg &= ~CR_EXTERNAL_INT;
}

/* ---- lado da CPU ------------------------------------------------------------------ */

u16 dsp_cpu_read_mail_hi(int cpu_mb) { return mail_read_hi(cpu_mb ? 0 : 1); }
u16 dsp_cpu_read_mail_lo(int cpu_mb) { return mail_read_lo(cpu_mb ? 0 : 1); }
void dsp_cpu_write_mail_hi(u16 v) { mail_write_hi(0, v); }
void dsp_cpu_write_mail_lo(u16 v) { mail_write_lo(0, v); }

static void reset(void) {
    pc = 0x8000;
    for (int i = 0; i < 4; i++) r.wr[i] = 0xFFFF;
}

/* A CPU não toca no estado do DSP: lê o controle direto e enfileira as escritas,
 * que a thread do DSP aplica entre blocos de instruções (fila sem bloqueio,
 * um produtor e um consumidor). */
static volatile LONG ctrl_q[64];
static volatile LONG ctrl_head, ctrl_tail;
static u64 init_code_set_at;

u16 dsp_cpu_read_control(void) { return *(volatile u16 *)&control_reg & 0x0C07; }

u16 dsp_cpu_write_control(u16 val) {
    u16 cur = *(volatile u16 *)&control_reg;
    u16 res = (u16)(val & ~CR_RESET);
    if ((cur & CR_INIT) && !(val & CR_INIT)) res = (u16)((res & ~CR_INIT) | CR_INIT_CODE);
    LONG t = ctrl_tail;
    while (((t + 1) & 63) == ctrl_head) Sleep(0); /* fila cheia: espera o DSP */
    ctrl_q[t] = val;
    InterlockedExchange(&ctrl_tail, (t + 1) & 63);
    return res & 0x0C07;
}

static void apply_control(u16 val) {
    if (val & CR_RESET) {
        reset();
        val &= ~CR_RESET;
    }
    if ((control_reg & CR_INIT) && !(val & CR_INIT)) {
        /* fim do "init": o DSP carrega 4 KiB de 0x81000000 na IRAM e começa em 0 */
        pc = 0;
        u8 *m = g_mem + (0x01000000 & MEM_MASK);
        for (u32 i = 0; i < 0x800; i++) iram[i] = (u16)((m[2 * i] << 8) | m[2 * i + 1]);
        val &= ~CR_INIT;
        val |= CR_INIT_CODE;
        init_code_set_at = g_dsp_cycles;
    }
    control_reg = val;
    if (val & CR_EXTERNAL_INT) {
        external_interrupt();
        check_exceptions();
    }
}

static void drain_control_queue(void) {
    while (ctrl_head != ctrl_tail) {
        apply_control((u16)ctrl_q[ctrl_head]);
        InterlockedExchange(&ctrl_head, (ctrl_head + 1) & 63);
    }
    /* "init code" some pouco depois (o SDK espera ele cair) */
    if ((control_reg & CR_INIT_CODE) && g_dsp_cycles - init_code_set_at > 10000) control_reg &= ~CR_INIT_CODE;
}

int dsp_take_cpu_interrupt(void) { return InterlockedExchange(&cpu_irq_pending, 0) != 0; }

/* ---- thread ------------------------------------------------------------------------ */

#define DSP_HZ 81000000ull

static DWORD WINAPI dsp_thread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    u64 start = tb_now();
    for (;;) {
        drain_control_queue();
        u64 target = (tb_now() - start) * (DSP_HZ / TB_HZ);
        if (g_dsp_cycles >= target) {
            Sleep(1);
            continue;
        }
        u64 budget = target - g_dsp_cycles;
        if (budget > 400000) { /* atraso grande (ex.: depurador): não tenta recuperar */
            g_dsp_cycles = target - 400000;
            budget = 400000;
        }
        for (u64 done = 0; done < budget; done += 2048) {
            drain_control_queue();
            for (u32 n = 0; n < 2048 && !(control_reg & CR_HALT); n++) step();
        }
        g_dsp_cycles += budget;
    }
}

static int load_rom(const char *dir, const char *name, u16 *dst, u32 words) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u8 buf[0x2000];
    size_t n = fread(buf, 1, words * 2, f);
    fclose(f);
    if (n != words * 2) return 0;
    for (u32 i = 0; i < words; i++) dst[i] = (u16)((buf[2 * i] << 8) | buf[2 * i + 1]);
    return 1;
}

int dsp_init(const char *rom_dir, u8 *aram, u32 aram_size) {
    g_aram = aram;
    g_aram_mask = aram_size - 1;
    if (!load_rom(rom_dir, "dsp_rom.bin", irom, 0x1000) || !load_rom(rom_dir, "dsp_coef.bin", coef, 0x800)) {
        LOG("[dsp] ROMs não encontradas em %s (dsp_rom.bin, dsp_coef.bin): áudio desligado", rom_dir);
        return 0;
    }
    build_tables();
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 0x1000; i++) iram[i] = 0x0021; /* HALT */
    for (int i = 0; i < 4; i++) r.wr[i] = 0xFFFF;
    r.sr |= SR_INT_ENABLE | SR_EXT_INT_ENABLE;
    control_reg = CR_INIT | CR_HALT;
    CreateThread(NULL, 0, dsp_thread, NULL, 0, NULL);
    LOG("[dsp] interpretador pronto (ROM em %s)", rom_dir);
    return 1;
}
