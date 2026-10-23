/* Declarações internas do runtime do jogo (escalonador, eventos, dispositivos, HLE). */
#ifndef RT_H
#define RT_H

#include <stdio.h>

#include "ppc.h"
#include "ppc_funcs.h"
#include "ppc_syms.h"

/* ---- dados do jogo usados pelo runtime (GM5E7D) ------------------------------ */
#define ADDR_InterruptHandlerTable 0x804BA0C0u /* ponteiro para __OSInterruptHandler[32] */

/* ---- tempo ---------------------------------------------------------------- */
#define TB_HZ 40500000ull /* timebase do Gekko: barramento de 162 MHz / 4 */
u64 tb_now(void);

/* ---- log ------------------------------------------------------------------ */
extern int g_verbose;
void rt_message(const char *msg); /* erro para o usuário: stderr e, sem terminal, uma janela */
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VLOG(...) do { if (g_verbose) LOG(__VA_ARGS__); } while (0)
void rt_fatal(const char *fmt, ...);

/* ---- chamadas para o código do jogo ---------------------------------------- */
PPCFunc ppc_lookup(u32 addr);
/* Executa uma função do jogo como se fosse um callback de interrupção: usa uma cópia
 * do contexto atual com a pilha 2 KiB abaixo, e descarta os registradores no fim. */
void guest_callback(PPCContext *c, u32 fn, u32 a0, u32 a1, u32 a2);

/* argumentos/retorno no padrão EABI */
#define ARG(i) (c->r[3 + (i)])
#define ARG64(i) (((u64)c->r[3 + (i)] << 32) | c->r[4 + (i)])
#define RET(v) (c->r[3] = (u32)(v))

/* ---- eventos e interrupções ------------------------------------------------- */
typedef void (*EventFn)(PPCContext *c, u64 data);
void ev_schedule(u64 when_tb, EventFn fn, u64 data);
void ev_cancel(EventFn fn, u64 data);
void irq_raise(u32 interrupt); /* __OSInterrupt (19 = PE finish, 24 = VI, ...) */
extern int g_in_irq;

/* ---- threads -------------------------------------------------------------- */
void sched_init_main(PPCContext **main_ctx);
void sched_poll(PPCContext *c);
PPCContext *sched_current_ctx(void);

/* ---- dispositivos ----------------------------------------------------------- */
void mmio_init(void);
void mmio_pe_finish(void);
void mmio_pe_token(u16 token, int irq);
/* GX (runtime/gx) */
void gx_init(void);
void gx_fifo_bytes(const u8 *b, int n, int linked);
void gx_fifo_flush(void); /* = gx_sync */
void gx_kick(void);        /* entrega à GPU o que estiver pendente, sem esperar */
void gx_sync(void);        /* entrega e espera a GPU terminar (a CPU quer o resultado) */
void gx_retrace(void);     /* no retrace: limita a GPU a um quadro de atraso */
void gx_poll_async(void);  /* aplica os eventos do PE vindos da thread da GPU */
void gx_idle_wait(u32 ms); /* espera ociosa que acorda com eventos da GPU */
void gx_fifo_put(u32 v, int size); /* escrita do write-gather pipe ligado à GPU */
void gx_print_stats(void);
void gx_frame_pump(void);
void vi_start_retrace(void);
void pad_retrace(PPCContext *c);
void mmio_dsp_poll(void);
/* som (runtime/audio.c) */
void audio_init(void);
void audio_push_ai_dma(u32 addr, u32 bytes);
extern u64 g_fifo_bytes;
extern u32 g_retrace_count;

/* ---- disco ---------------------------------------------------------------- */
int disc_open(const char *path);
void disc_close(void);
int disc_read(u64 offset, void *dst, u32 len);
u32 disc_read32(u64 offset);

#endif
