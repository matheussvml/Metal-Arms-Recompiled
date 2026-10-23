/* Threads do OS do GameCube como fibers do Windows, mais a fila de eventos
 * (interrupções, alarmes) e o relógio do timebase.
 *
 * Só uma thread do jogo roda por vez, como no console. A troca acontece nas
 * chamadas bloqueantes (OSSleepThread, OSSuspendThread...) e, por preempção,
 * quando uma thread de prioridade maior fica pronta fora de uma interrupção. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>

#include "rt.h"

/* ---- tempo ---------------------------------------------------------------- */

static LARGE_INTEGER g_qpc_freq, g_qpc_start;

static u64 g_tb_mult; /* TB_HZ / freq em ponto fixo 32.32 */

static void tb_init(void) {
    if (g_qpc_freq.QuadPart) return;
    QueryPerformanceCounter(&g_qpc_start);
    QueryPerformanceFrequency(&g_qpc_freq);
    g_tb_mult = (u64)(((double)TB_HZ / (double)g_qpc_freq.QuadPart) * 4294967296.0);
}

/* chamado muito (OSGetTime do jogo, pontos de checagem): sem divisão */
u64 tb_now(void) {
    LARGE_INTEGER now;
    if (!g_qpc_freq.QuadPart) tb_init(); /* outras threads (DSP, áudio) podem chegar antes */
    QueryPerformanceCounter(&now);
    u64 d = (u64)(now.QuadPart - g_qpc_start.QuadPart), hi;
    u64 lo = _umul128(d, g_tb_mult, &hi);
    return (hi << 32) | (lo >> 32);
}

u64 ppc_timebase(void) { return tb_now(); }

int g_log_to_file; /* sem terminal (aberto com dois cliques): o log vai para metalarms.log */

void rt_message(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    if (g_log_to_file) MessageBoxA(NULL, msg, "Metal Arms", MB_OK | MB_ICONERROR);
}

void rt_fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(msg, sizeof msg, "FATAL: ");
    vsnprintf(msg + n, sizeof msg - n, fmt, ap);
    va_end(ap);
    rt_message(msg);
    abort();
}

/* ---- threads -------------------------------------------------------------- */

enum { TS_READY = 1, TS_RUNNING = 2, TS_WAITING = 4, TS_MORIBUND = 8 };

typedef struct HThread {
    u32 guest; /* OSThread* no espaço do jogo */
    PPCContext ctx;
    void *fiber;
    int prio, suspend, state, detached;
    u32 wait_queue;
    u32 entry, param;
    u64 seq; /* ordem FIFO dentro da mesma prioridade */
    struct HThread *next;
} HThread;

static HThread *g_threads, *g_cur, *g_dead;
volatile u64 g_idle_tb; /* tempo ocioso da thread principal (diagnóstico de desempenho) */
static u64 g_seq;
static int g_sched_disabled, g_need_resched;
int g_in_irq;

/* Campos do OSThread que o código do jogo lê diretamente */
#define OST_STATE 0x2C8
#define OST_ATTR 0x2CA
#define OST_SUSPEND 0x2CC
#define OST_PRIO 0x2D0
#define OST_BASE 0x2D4
#define OST_VAL 0x2D8

static void sync_guest(HThread *t) {
    if (!t->guest) return;
    st16(t->guest + OST_STATE, (u16)(t->state == TS_MORIBUND && t->detached ? 0 : t->state));
    st32(t->guest + OST_SUSPEND, (u32)t->suspend);
    st32(t->guest + OST_PRIO, (u32)t->prio);
}

static HThread *find_thread(u32 guest) {
    for (HThread *t = g_threads; t; t = t->next)
        if (t->guest == guest) return t;
    return NULL;
}

PPCContext *sched_current_ctx(void) { return &g_cur->ctx; }

static int runnable(HThread *t) { return t->state == TS_READY && t->suspend <= 0; }

static HThread *pick_ready(void) {
    HThread *best = NULL;
    for (HThread *t = g_threads; t; t = t->next)
        if (runnable(t) && (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)))
            best = t;
    return best;
}

static void deliver_events(PPCContext *c, int force);

static VOID CALLBACK fiber_main(LPVOID p) {
    HThread *t = (HThread *)p;
    t->ctx.r[3] = t->param;
    t->ctx.lr = 0;
    PPCFunc fn = ppc_lookup(t->entry);
    if (!fn) rt_fatal("thread %08X: entrada %08X sem função", t->guest, t->entry);
    fn(&t->ctx);
    /* voltar da função de entrada = OSExitThread(r3) */
    hle_OSExitThread(&t->ctx);
}

static void switch_to(HThread *t) {
    HThread *prev = g_cur;
    g_cur = t;
    t->state = TS_RUNNING;
    sync_guest(t);
    st32(0x800000E4, t->guest);   /* __OSCurrentThread */
    st32(0x800000D4, t->guest);   /* contexto atual (OSContext no início do OSThread) */
    st32(0x800000C0, t->guest & 0x3FFFFFFF);
    if (t == prev) return;
    if (!t->fiber) {
        t->fiber = CreateFiber(1 << 20, fiber_main, t);
        if (!t->fiber) rt_fatal("CreateFiber falhou");
    }
    SwitchToFiber(t->fiber);
    /* voltamos a rodar: libera fibers de threads que já terminaram */
    while (g_dead) {
        HThread *d = g_dead;
        g_dead = d->next;
        DeleteFiber(d->fiber);
        free(d);
    }
}

/* Escolhe a próxima thread. A atual já deve estar com o estado ajustado
 * (READY se continua competindo, WAITING/MORIBUND se não). */
static void schedule(void) {
    for (;;) {
        HThread *best = pick_ready();
        if (best) {
            switch_to(best);
            return;
        }
        /* ninguém pronto: o console ficaria ocioso com interrupções ligadas */
        gx_kick();
        gx_poll_async();
        mmio_dsp_poll();
        deliver_events(&g_cur->ctx, 1);
        if (!pick_ready()) {
            u64 t0 = tb_now();
            gx_idle_wait(1);
            g_idle_tb += tb_now() - t0;
        }
    }
}

static void maybe_preempt(void) {
    if (g_in_irq || g_sched_disabled > 0) {
        g_need_resched = 1;
        return;
    }
    g_need_resched = 0;
    HThread *best = pick_ready();
    if (best && best != g_cur && (g_cur->state != TS_RUNNING || best->prio < g_cur->prio)) {
        if (g_cur->state == TS_RUNNING) {
            g_cur->state = TS_READY;
            g_cur->seq = 0; /* preemptada: volta antes das outras da mesma prioridade */
        }
        schedule();
    }
}

void sched_init_main(PPCContext **main_ctx) {
    tb_init();
    HThread *t = (HThread *)calloc(1, sizeof *t);
    t->fiber = ConvertThreadToFiber(NULL);
    t->prio = 16;
    t->state = TS_RUNNING;
    t->detached = 1;
    t->seq = ++g_seq;
    g_threads = g_cur = t;
    *main_ctx = &t->ctx;
}

/* __OSThreadInit monta a thread padrão (DefaultThread) do lado do jogo. */
void hle___OSThreadInit(PPCContext *c) {
    f_8033F220_orig(c);
    g_cur->guest = ld32(0x800000E4);
    sync_guest(g_cur);
    VLOG("thread padrão em %08X", g_cur->guest);
}

void hle_OSCreateThread(PPCContext *c) {
    u32 guest = ARG(0), entry = ARG(1), param = ARG(2), stack = ARG(3), size = ARG(4);
    s32 prio = (s32)ARG(5);
    u32 attr = ARG(6);
    PPCContext saved = *c;
    f_8033F940_orig(c); /* preenche o OSThread do lado do jogo (contexto, pilha, listas) */
    if (!c->r[3]) return;
    HThread *t = find_thread(guest);
    if (!t) {
        t = (HThread *)calloc(1, sizeof *t);
        t->next = g_threads;
        g_threads = t;
    } else if (t->fiber && t != g_cur) {
        DeleteFiber(t->fiber);
        t->fiber = NULL;
    }
    t->guest = guest;
    t->entry = entry;
    t->param = param;
    t->prio = prio;
    t->suspend = 1;
    t->state = TS_READY;
    t->detached = attr & 1;
    t->seq = ++g_seq;
    t->ctx = saved;
    memset(t->ctx.r, 0, sizeof t->ctx.r);
    t->ctx.r[1] = (stack & ~7u) - 8;
    st32(t->ctx.r[1], 0);
    t->ctx.r[2] = saved.r[2];
    t->ctx.r[13] = saved.r[13];
    t->ctx.msr |= 0x8000;
    sync_guest(t);
    VLOG("OSCreateThread %08X entrada %08X prio %d pilha %08X (%#x)", guest, entry, prio, stack, size);
    RET(1);
}

void hle_OSResumeThread(PPCContext *c) {
    HThread *t = find_thread(ARG(0));
    if (!t) rt_fatal("OSResumeThread de thread desconhecida %08X", ARG(0));
    int old = t->suspend;
    if (t->suspend > 0) t->suspend--;
    if (old == 1) {
        t->seq = ++g_seq;
        sync_guest(t);
        maybe_preempt();
    }
    RET(old);
}

void hle_OSSuspendThread(PPCContext *c) {
    HThread *t = find_thread(ARG(0));
    if (!t) rt_fatal("OSSuspendThread de thread desconhecida %08X", ARG(0));
    int old = t->suspend++;
    sync_guest(t);
    if (old == 0 && t == g_cur) {
        g_cur->state = TS_READY;
        schedule();
    }
    RET(old);
}

void hle_OSSleepThread(PPCContext *c) {
    gx_kick(); /* quem dorme pode estar esperando a GPU (GXDrawDone, tokens) */
    g_cur->state = TS_WAITING;
    g_cur->wait_queue = ARG(0);
    sync_guest(g_cur);
    schedule();
}

void hle_OSWakeupThread(PPCContext *c) {
    u32 q = ARG(0);
    for (HThread *t = g_threads; t; t = t->next)
        if (t->state == TS_WAITING && t->wait_queue == q) {
            t->state = TS_READY;
            t->wait_queue = 0;
            t->seq = ++g_seq;
            sync_guest(t);
        }
    maybe_preempt();
}

void hle_OSYieldThread(PPCContext *c) {
    (void)c;
    g_cur->state = TS_READY;
    g_cur->seq = ++g_seq;
    schedule();
}

void hle_OSDisableScheduler(PPCContext *c) { RET(g_sched_disabled++); }

void hle_OSEnableScheduler(PPCContext *c) {
    int old = g_sched_disabled;
    if (g_sched_disabled > 0) g_sched_disabled--;
    if (!g_sched_disabled && g_need_resched) maybe_preempt();
    RET(old);
}

static void exit_thread(HThread *t, u32 val) {
    t->state = TS_MORIBUND;
    st32(t->guest + OST_VAL, val);
    sync_guest(t);
    VLOG("thread %08X terminou (%08X)", t->guest, val);
    /* acorda quem espera no queueJoin (OSThread + 0x2E8) */
    for (HThread *o = g_threads; o; o = o->next)
        if (o->state == TS_WAITING && o->wait_queue == t->guest + 0x2E8) {
            o->state = TS_READY;
            o->seq = ++g_seq;
            sync_guest(o);
        }
}

void hle_OSExitThread(PPCContext *c) {
    HThread *t = g_cur;
    exit_thread(t, ARG(0));
    /* tira da lista; o fiber é apagado pela próxima thread que rodar */
    HThread **pp = &g_threads;
    while (*pp != t) pp = &(*pp)->next;
    *pp = t->next;
    t->next = g_dead;
    g_dead = t;
    HThread *best = pick_ready();
    while (!best) {
        deliver_events(c, 1);
        best = pick_ready();
    }
    g_cur = best;
    best->state = TS_RUNNING;
    sync_guest(best);
    st32(0x800000E4, best->guest);
    if (!best->fiber) best->fiber = CreateFiber(1 << 20, fiber_main, best);
    SwitchToFiber(best->fiber);
    rt_fatal("thread terminada voltou a rodar");
}

void hle_OSCancelThread(PPCContext *c) {
    HThread *t = find_thread(ARG(0));
    if (!t) return;
    if (t == g_cur) {
        ARG(0) = 0;
        hle_OSExitThread(c);
    }
    exit_thread(t, 0);
}

/* ---- eventos -------------------------------------------------------------- */

typedef struct Event {
    u64 when;
    EventFn fn;
    u64 data;
    struct Event *next;
} Event;

static Event *g_events;

void ev_schedule(u64 when, EventFn fn, u64 data) {
    Event *e = (Event *)malloc(sizeof *e);
    e->when = when;
    e->fn = fn;
    e->data = data;
    Event **pp = &g_events;
    while (*pp && (*pp)->when <= when) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
}

void ev_cancel(EventFn fn, u64 data) {
    for (Event **pp = &g_events; *pp;) {
        if ((*pp)->fn == fn && (*pp)->data == data) {
            Event *e = *pp;
            *pp = e->next;
            free(e);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void guest_callback(PPCContext *c, u32 fn, u32 a0, u32 a1, u32 a2) {
    PPCFunc f = ppc_lookup(fn);
    if (!f) rt_fatal("callback para %08X sem função", fn);
    PPCContext tmp = *c;
    tmp.r[1] = ((c->r[1] - 0x800) & ~0xFu);
    st32(tmp.r[1], 0);
    tmp.r[3] = a0;
    tmp.r[4] = a1;
    tmp.r[5] = a2;
    tmp.lr = 0;
    tmp.msr &= ~0x8000u; /* como numa interrupção: EE desligado */
    f(&tmp);
}

static void irq_event(PPCContext *c, u64 n) {
    u32 table = ld32(ADDR_InterruptHandlerTable);
    u32 handler = table ? ld32(table + 4 * (u32)n) : 0;
    if (!handler) {
        VLOG("interrupção %u sem handler", (u32)n);
        return;
    }
    guest_callback(c, handler, (u32)n, ld32(0x800000D4), 0);
}

void irq_raise(u32 interrupt) { ev_schedule(0, irq_event, interrupt); }

static void deliver_events(PPCContext *c, int force) {
    if (!g_events || g_in_irq) return;
    if (!force && !(c->msr & 0x8000)) return;
    u64 now = tb_now();
    g_in_irq++;
    while (g_events && g_events->when <= now) {
        Event *e = g_events;
        g_events = e->next;
        e->fn(c, e->data);
        free(e);
    }
    g_in_irq--;
}

void sched_poll(PPCContext *c) {
    gx_poll_async();
    mmio_dsp_poll();
    deliver_events(c, 0);
    if (g_need_resched && !g_in_irq && g_sched_disabled == 0) maybe_preempt();
}
