/* Profiler por amostragem (diagnóstico): MA_PROFILE=<segundos de espera>,<segundos de coleta>.
 * Pausa a thread principal ~1000 vezes por segundo, guarda o RIP e, no fim, imprime as
 * funções mais frequentes (nomes via dbghelp e o PDB do executável). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

#define MAX_SAMPLES 200000

static HANDLE g_main;
static DWORD64 g_samples[MAX_SAMPLES];
static int g_n;
static int g_wait_s = 40, g_collect_s = 15;

typedef struct { char name[128]; int count; } Bucket;

static int cmp_bucket(const void *a, const void *b) { return ((const Bucket *)b)->count - ((const Bucket *)a)->count; }

static void report(void) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    Bucket *b = (Bucket *)calloc(4096, sizeof(Bucket));
    int nb = 0;
    char buf[sizeof(SYMBOL_INFO) + 256];
    for (int i = 0; i < g_n; i++) {
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        memset(buf, 0, sizeof buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char *name = SymFromAddr(proc, g_samples[i], &disp, si) ? si->Name : "?";
        int game = name[0] == 'f' && name[1] == '_' && strlen(name) >= 10;
        char tagged[160];
        if ((si->Size && disp >= si->Size) || (game && disp >= 0x1000)) { /* longe do início: suspeito */
            snprintf(tagged, sizeof tagged, "%s+%llX (fora, tamanho %lX)", name, (unsigned long long)disp & ~0xFFull,
                     (unsigned long)si->Size);
            name = tagged;
        }
        /* duas contagens: por função (inclusive as do jogo) e o total do jogo num balde só */
        for (int pass = 0; pass < 2; pass++) {
            const char *key = pass == 0 ? name : (game ? "(código do jogo, total)" : NULL);
            if (!key) continue;
            int k;
            for (k = 0; k < nb; k++)
                if (!strcmp(b[k].name, key)) break;
            if (k == nb && nb < 4096) strncpy(b[nb++].name, key, 127);
            if (k < nb) b[k].count++;
        }
    }
    qsort(b, (size_t)nb, sizeof(Bucket), cmp_bucket);
    fprintf(stderr, "[profile] %d amostras\n", g_n);
    for (int k = 0; k < nb && k < 40; k++)
        fprintf(stderr, "[profile] %5.1f%%  %s\n", 100.0 * b[k].count / g_n, b[k].name);
    free(b);
}

static DWORD WINAPI sampler(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    Sleep((DWORD)g_wait_s * 1000);
    fprintf(stderr, "[profile] coletando %d s...\n", g_collect_s);
    DWORD end = GetTickCount() + (DWORD)g_collect_s * 1000;
    CONTEXT ctx;
    while (GetTickCount() < end && g_n < MAX_SAMPLES) {
        if (SuspendThread(g_main) != (DWORD)-1) {
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_main, &ctx)) g_samples[g_n++] = ctx.Rip;
            ResumeThread(g_main);
        }
        Sleep(1);
    }
    report();
    return 0;
}

void profiler_start(void) {
    const char *cfg = getenv("MA_PROFILE");
    if (!cfg) return;
    sscanf(cfg, "%d,%d", &g_wait_s, &g_collect_s);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}
