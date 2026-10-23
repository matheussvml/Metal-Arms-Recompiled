/* Registradores de hardware (0xCC00xxxx) e o write-gather pipe do GX (0xCC008000).
 *
 * Modelo: um banco de 64 KiB guarda o que o jogo escreveu e devolve nas leituras.
 * Só os registradores cujo comportamento importa têm ganchos: retrace do VI,
 * interrupções do PE, transferências EXI, DMA da ARAM e DMA de áudio. */
#include "rt.h"
#include "dsp/dsp.h"

static u8 io[0x10000];
static u32 read_count[0x10000 / 4];
static u8 logged[0x10000 / 4];

u64 g_fifo_bytes;
u32 g_retrace_count;
static FILE *g_fifo_dump;

#define ARAM_SIZE 0x01000000u
static u8 *aram;

static u16 rd16(u32 off) { return (u16)((io[off] << 8) | io[off + 1]); }
static u32 rd32(u32 off) { return ((u32)rd16(off) << 16) | rd16(off + 2); }
static void wr16(u32 off, u16 v) { io[off] = (u8)(v >> 8); io[off + 1] = (u8)v; }
static void wr32(u32 off, u32 v) { wr16(off, (u16)(v >> 16)); wr16(off + 2, (u16)v); }

static const char *region(u32 off) {
    switch (off >> 12) {
    case 0: return "CP";
    case 1: return "PE";
    case 2: return "VI";
    case 3: return "PI";
    case 4: return "MI";
    case 5: return "DSP";
    case 6: return off < 0x6400 ? "DI" : off < 0x6800 ? "SI" : off < 0x6C00 ? "EXI" : "AI";
    case 8: return "GXFIFO";
    default: return "?";
    }
}

static void note_access(u32 ea, int write, u32 v) {
    u32 off = ea & 0xFFFF;
    if (!logged[off >> 2] && g_verbose) {
        logged[off >> 2] = 1;
        LOG("mmio %s %s %08X = %08X", write ? "w" : "r", region(off), ea, v);
    }
    if (!write && ++read_count[off >> 2] == 2000000)
        LOG("aviso: %s %08X lido 2 milhões de vezes (laço de espera?)", region(off), ea);
}

/* ---- GX: write-gather pipe ------------------------------------------------- */

/* Chamados pelo interpretador do GX ao executar os comandos BP de sincronização. */
void mmio_pe_finish(void) { /* PE_DONE: GXSetDrawDone */
    wr16(0x100A, (u16)(rd16(0x100A) | 0x8));
    irq_raise(19);
}

void mmio_pe_token(u16 token, int irq) { /* PE_TOKEN(_INT): GXSetDrawSync */
    wr16(0x100E, token);
    if (irq) {
        wr16(0x100A, (u16)(rd16(0x100A) | 0x4));
        irq_raise(18);
    }
}

/* O pipe grava na RAM no ponteiro do FIFO da CPU (PI 0x300C base, 0x3010 fim,
 * 0x3014 escrita) e só executa quando esse FIFO é o mesmo que a GPU lê (CP 0x20).
 *
 * Caminho rápido: o estado "ligado" é recalculado só quando esses registradores
 * mudam, e no modo ligado o ponteiro de escrita só é materializado quando lido. */
static int g_wgp_linked = 1;
static u32 g_wgp_pending; /* bytes escritos desde a última atualização do 0x3014 */

static void wgp_sync_wp(void) {
    if (!g_wgp_pending) return;
    u32 base = rd32(0x300C) & 0x03FFFFE0, end = rd32(0x3010) & 0x03FFFFE0;
    if (base && end > base) {
        u32 wp = rd32(0x3014) & 0x03FFFFFF;
        u32 size = end - base;
        if (wp < base || wp >= end) wp = base;
        wp = base + (wp - base + g_wgp_pending % size) % size;
        wr32(0x3014, wp);
    }
    g_wgp_pending = 0;
}

static void wgp_update_link(void) {
    wgp_sync_wp();
    u32 base = rd32(0x300C) & 0x03FFFFE0;
    u32 gp_base = ((u32)rd16(0x0022) << 16 | rd16(0x0020)) & 0x03FFFFE0;
    g_wgp_linked = !base || base == gp_base;
}

static void fifo_write(u32 v, int size) {
    g_fifo_bytes += (u64)size;
    if (g_wgp_linked && !g_fifo_dump) {
        g_wgp_pending += (u32)size;
        gx_fifo_put(v, size);
        return;
    }
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    const u8 *p = b + 4 - size;
    if (g_wgp_linked) { /* com MA_FIFO_DUMP */
        g_wgp_pending += (u32)size;
        fwrite(p, 1, (size_t)size, g_fifo_dump);
        gx_fifo_bytes(p, size, 1);
        return;
    }
    /* display list sendo gravada: os bytes precisam estar na RAM */
    u32 base = rd32(0x300C) & 0x03FFFFE0, end = rd32(0x3010) & 0x03FFFFE0;
    if (base && end > base) {
        u32 wp = rd32(0x3014) & 0x03FFFFFF;
        for (int i = 0; i < size; i++) {
            g_mem[wp & MEM_MASK] = p[i];
            if (++wp >= end) wp = base;
        }
        wr32(0x3014, wp);
    }
}

/* registradores que mudam o destino do write-gather pipe */
static int is_wgp_reg(u32 off) { return (off >= 0x300C && off < 0x3018) || off == 0x0020 || off == 0x0022; }

/* ---- VI ------------------------------------------------------------------- */

static u64 g_retrace_base;

static void retrace_event(PPCContext *c, u64 n) {
    (void)c;
    g_retrace_count++;
    gx_retrace();
    /* diagnóstico: MA_RAM_DUMP=<retrace>[,<retrace>...] grava os 24 MiB da RAM nesses retraces */
    static const char *dumps;
    static int dumps_init;
    if (!dumps_init) { dumps = getenv("MA_RAM_DUMP"); dumps_init = 1; }
    int dump_now = 0;
    for (const char *p = dumps; p && *p;) {
        if ((unsigned)strtoul(p, (char **)&p, 10) == g_retrace_count) dump_now = 1;
        if (*p == ',') p++;
        else break;
    }
    if (dump_now) {
        char path[64];
        snprintf(path, sizeof path, "ram_%u.bin", g_retrace_count);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_mem, 1, 0x01800000, f); fclose(f); LOG("[rt] RAM gravada em %s", path); }
    }
    gx_frame_pump(); /* mensagens da janela */
    wr16(0x2030, (u16)(rd16(0x2030) | 0x8000)); /* VI_DI0: interrupção pendente */
    irq_raise(24);
    pad_retrace(c); /* amostragem do controle (SI) */
    /* 59,94 Hz no NTSC, em cadência fixa: o próximo retrace é calculado a partir do
     * início, não de quando este foi entregue (senão os atrasos se acumulam) */
    u64 next = g_retrace_base + (n + 1) * TB_HZ * 1001 / 60000;
    u64 now = tb_now();
    if (next + TB_HZ / 10 < now) { /* atraso grande (depurador, carga): recomeça a contagem */
        g_retrace_base = now;
        n = 0;
        next = now + TB_HZ * 1001 / 60000;
    }
    ev_schedule(next, retrace_event, n + 1);
}

void vi_start_retrace(void) {
    g_retrace_base = tb_now();
    ev_schedule(g_retrace_base + TB_HZ * 1001 / 60000, retrace_event, 1);
}

/* ---- EXI ------------------------------------------------------------------ */

static void exi_cr_write(u32 off, u32 v) {
    u32 ch = (off - 0x6800) / 0x14;
    u32 base = 0x6800 + ch * 0x14;
    wr32(off, v & ~1u); /* TSTART: termina na hora; sem dispositivo, dados = 0 */
    if (v & 1) {
        u32 csr = rd32(base) | 0x8; /* TCINT */
        wr32(base, csr);
        if (csr & 0x4) irq_raise(10 + 3 * ch);
    }
}

/* ---- DSP / ARAM / AI ------------------------------------------------------ */

#define DSPCR 0x500A

/* O DSP é emulado em runtime/dsp (thread própria). Aqui ficam os registradores do
 * lado da CPU: mailboxes, DSPCR (interrupções DSP/ARAM/AI e controle do DSP),
 * DMA da ARAM e DMA de áudio. */
static int g_dsp_on;

/* interrupção pedida pelo DSP (DIRQ): chamada nos pontos de checagem da CPU */
void mmio_dsp_poll(void) {
    if (g_dsp_on && dsp_take_cpu_interrupt()) {
        u16 cr = (u16)(rd16(DSPCR) | 0x80); /* DSPINT */
        wr16(DSPCR, cr);
        if (cr & 0x100) irq_raise(7);
    }
}

static void aram_dma(void) {
    u32 mm = rd32(0x5020) & 0x03FFFFE0, ar = rd32(0x5024) & 0x03FFFFE0, cnt = rd32(0x5028);
    u32 len = cnt & 0x7FFFFFE0;
    ar &= ARAM_SIZE - 1; /* acima do tamanho real a ARAM se espelha (o __ARChecksize testa isso) */
    if (ar + len <= ARAM_SIZE && mm + len <= MEM_SIZE) {
        if (cnt & 0x80000000) memcpy(g_mem + mm, aram + ar, len); /* ARAM -> RAM */
        else memcpy(aram + ar, g_mem + mm, len);
    } else {
        LOG("DMA ARAM fora dos limites: mm %08X ar %08X len %X", mm, ar, len);
    }
    wr32(0x5028, cnt & 0x80000000); /* contador zerado = DMA concluído */
    u16 cr = (u16)(rd16(DSPCR) | 0x20); /* ARINT */
    wr16(DSPCR, cr);
    if (cr & 0x40) irq_raise(6);
}

static int ai_dma_on;
static u64 ai_dma_due; /* quando o bloco atual deveria terminar */

static void ai_dma_event(PPCContext *c, u64 gen) {
    (void)c;
    if (!ai_dma_on || gen != (u64)ai_dma_on) return;
    u32 blocks = rd16(0x5036) & 0x7FFF; /* em blocos de 32 bytes */
    audio_push_ai_dma(((u32)rd16(0x5030) << 16 | rd16(0x5032)) & 0x03FFFFE0, blocks * 32);
    u16 cr = (u16)(rd16(DSPCR) | 0x08); /* AIDINT */
    wr16(DSPCR, cr);
    if (cr & 0x10) irq_raise(5);
    /* 32 bytes = 8 amostras estéreo de 16 bits a 32 kHz */
    /* cadência fixa: o próximo bloco conta a partir do horário previsto, não da entrega
     * (que atrasa um pouco a cada vez e deixava o áudio ~8% lento) */
    u64 dt = blocks ? TB_HZ * blocks * 8 / 32000 : TB_HZ / 200;
    u64 now = tb_now();
    ai_dma_due += dt;
    if (ai_dma_due + TB_HZ / 10 < now) ai_dma_due = now; /* ficou muito para trás (pausa): recomeça */
    ev_schedule(ai_dma_due, ai_dma_event, gen);
}

static void dsp_write16(u32 off, u16 v) {
    if (off == DSPCR) {
        /* bits de interrupção (AI 3, ARAM 5, DSP 7) se limpam escrevendo 1;
         * reset/interrupção externa/halt/init (0x0C07) vão para o DSP */
        u16 cur = rd16(DSPCR);
        u16 ack = v & 0xA8;
        u16 dspbits = g_dsp_on ? dsp_cpu_write_control(v) : (u16)(v & 0x0C06);
        wr16(DSPCR, (u16)((v & ~0x0CAF) | (cur & 0xA8 & ~ack) | dspbits));
        return;
    }
    if (off == 0x5000) { if (g_dsp_on) dsp_cpu_write_mail_hi(v); return; }
    if (off == 0x5002) { if (g_dsp_on) dsp_cpu_write_mail_lo(v); return; }
    wr16(off, v);
    if (off == 0x502A) aram_dma(); /* metade baixa do contador inicia o DMA */
    if (off == 0x5036) {
        if (v & 0x8000) {
            if (!ai_dma_on) {
                static int gen;
                ai_dma_on = ++gen;
                ai_dma_due = tb_now();
                ev_schedule(ai_dma_due, ai_dma_event, (u64)ai_dma_on);
            }
        } else {
            ai_dma_on = 0;
        }
    }
}

/* ---- acessos -------------------------------------------------------------- */

void mmio_init(void) {
    aram = (u8 *)calloc(1, ARAM_SIZE);
    wr32(0x3000, 0x00010000); /* PI: botão de reset solto (bit 16 = 1) */
    wr16(0x5016, 0x0001);     /* AR_MODE: ARAM pronta (__ARChecksize espera esse bit) */
    g_dsp_on = dsp_init(getenv("MA_DSP_ROM") ? getenv("MA_DSP_ROM") : "data/dsp", aram, ARAM_SIZE);
    audio_init();
    const char *dump = getenv("MA_FIFO_DUMP");
    if (dump) g_fifo_dump = fopen(dump, "wb");
}

static int is_fifo(u32 ea) { return (ea & 0xFFFFF000u) == 0xCC008000u; }

u32 mmio_read32(u32 ea) {
    if ((ea >> 24) == 0xC8) { gx_sync(); return 0; } /* EFB */
    u32 off = ea & 0xFFFC;
    if (off == 0x0000 || (off >= 0x1000 && off < 0x2000)) gx_sync(); /* status do CP, PE: a CPU quer o estado da GPU */
    if (off == 0x3014) wgp_sync_wp();
    if (off == 0x5000 || off == 0x5004) return ((u32)mmio_read16(ea) << 16) | mmio_read16(ea + 2);
    u32 v = rd32(off);
    if (off == 0x6C08) v = (u32)(tb_now() * 32000 / TB_HZ); /* AI: contador de amostras */
    note_access(ea, 0, v);
    return v;
}

u16 mmio_read16(u32 ea) {
    if ((ea >> 24) == 0xC8) { gx_sync(); return 0; }
    u32 off = ea & 0xFFFE;
    if (off == 0x0000 || (off >= 0x1000 && off < 0x2000)) gx_sync();
    if ((off & ~3u) == 0x3014) wgp_sync_wp();
    u16 v = rd16(off);
    if (off == 0x0000) v = (u16)(v | 0x000C); /* CP: leitura e comandos ociosos */
    if (off == 0x202C) v = (u16)((tb_now() * 15734 / TB_HZ) % 525 + 1); /* VI: linha atual */
    if (g_dsp_on && (off >> 4) == 0x500) {
        if (off == 0x5000) v = dsp_cpu_read_mail_hi(1);      /* CPU -> DSP (bit 15 = ainda cheio) */
        else if (off == 0x5002) v = rd16(0x5002);
        else if (off == 0x5004) v = dsp_cpu_read_mail_hi(0); /* DSP -> CPU */
        else if (off == 0x5006) v = dsp_cpu_read_mail_lo(0); /* ler a metade baixa esvazia */
        else if (off == DSPCR) {
            mmio_dsp_poll();
            v = (u16)((rd16(DSPCR) & ~0x0C07) | dsp_cpu_read_control());
        }
    }
    note_access(ea, 0, v);
    return v;
}

u8 mmio_read8(u32 ea) {
    u16 v = mmio_read16(ea & ~1u);
    return (u8)(ea & 1 ? v : v >> 8);
}

void mmio_write32(u32 ea, u32 v) {
    if (is_fifo(ea)) { fifo_write(v, 4); return; }
    if ((ea >> 24) == 0xC8) return;
    u32 off = ea & 0xFFFC;
    note_access(ea, 1, v);
    if (off >= 0x6800 && off < 0x6800 + 3 * 0x14 && (off - 0x6800) % 0x14 == 0x0C) { exi_cr_write(off, v); return; }
    if ((off >> 12) == 5) { dsp_write16(off, (u16)(v >> 16)); dsp_write16(off + 2, (u16)v); return; }
    if (is_wgp_reg(off)) { wgp_sync_wp(); wr32(off, v); wgp_update_link(); return; }
    wr32(off, v);
}

void mmio_write16(u32 ea, u16 v) {
    if (is_fifo(ea)) { fifo_write(v, 2); return; }
    if ((ea >> 24) == 0xC8) return;
    u32 off = ea & 0xFFFE;
    note_access(ea, 1, v);
    if ((off >> 12) == 5) { dsp_write16(off, v); return; }
    if (off == 0x100A) { /* PE: escrever 1 nos bits 2/3 limpa a interrupção */
        u16 cur = rd16(off);
        wr16(off, (u16)((v & ~0xC) | (cur & 0xC & ~(v & 0xC))));
        return;
    }
    if (is_wgp_reg(off & ~3u)) { wgp_sync_wp(); wr16(off, v); wgp_update_link(); return; }
    wr16(off, v);
}

void mmio_write8(u32 ea, u8 v) {
    if (is_fifo(ea)) { fifo_write(v, 1); return; }
    if ((ea >> 24) == 0xC8) return;
    u32 off = ea & 0xFFFF;
    note_access(ea, 1, v);
    io[off] = v;
}
