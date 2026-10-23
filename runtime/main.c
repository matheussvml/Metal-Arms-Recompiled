/* Ponto de entrada do port: carrega o jogo da imagem de disco do usuário,
 * prepara a memória como o IPL/apploader deixaria e executa __start. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "rt.h"

int g_verbose;
extern void (*g_poll_hook)(PPCContext *c);
extern u64 g_dvd_bytes;
void ppc_mem_init(void);

static u32 load_dol_from_disc(void) {
    u32 dol = disc_read32(0x420);
    u8 hdr[0x100];
    disc_read(dol, hdr, sizeof hdr);
#define BE32(p) (((u32)(p)[0] << 24) | ((u32)(p)[1] << 16) | ((u32)(p)[2] << 8) | (p)[3])
    for (int i = 0; i < 18; i++) {
        u32 off = BE32(hdr + i * 4), addr = BE32(hdr + 0x48 + i * 4), size = BE32(hdr + 0x90 + i * 4);
        if (off && size) disc_read((u64)dol + off, host_ptr(addr), size);
    }
    u32 entry = BE32(hdr + 0xE0);
#undef BE32
    return entry;
}

/* O que o IPL e o apploader deixam em 0x80000000 antes de pular para o jogo. */
static void setup_boot_info(void) {
    disc_read(0, host_ptr(0x80000000), 0x20); /* ID do jogo, versão etc. */
    st32(0x80000020, 0x0D15EA5E);             /* boot normal pelo IPL */
    st32(0x80000024, 1);
    st32(0x80000028, 0x01800000); /* 24 MiB */
    st32(0x8000002C, 0x00000003); /* console de varejo */
    st32(0x800000CC, 0);          /* NTSC */
    st32(0x800000D0, 0x01000000); /* ARAM de 16 MiB */
    st32(0x800000F0, 0x01800000);
    st32(0x800000F8, 162000000);  /* barramento */
    st32(0x800000FC, 486000000);  /* CPU */

    /* FST e bi2.bin no topo da memória; a arena termina logo abaixo */
    u32 fst_off = disc_read32(0x424), fst_size = disc_read32(0x428), fst_max = disc_read32(0x42C);
    u32 fst_addr = (0x81800000u - fst_max) & ~0x1Fu;
    disc_read(fst_off, host_ptr(fst_addr), fst_size);
    u32 bi2_addr = (fst_addr - 0x2000) & ~0x1Fu;
    disc_read(0x440, host_ptr(bi2_addr), 0x2000);
    st32(0x80000030, 0);
    st32(0x80000034, bi2_addr);
    st32(0x80000038, fst_addr);
    st32(0x8000003C, fst_max);
    st32(0x800000F4, bi2_addr);
    VLOG("FST %08X (%#x bytes), bi2 %08X", fst_addr, fst_size, bi2_addr);
}

static DWORD WINAPI heartbeat(LPVOID p) {
    (void)p;
    extern volatile u64 g_idle_tb, g_gx_tb, g_gx_draws, g_gx_frames, g_gx_syncs;
    extern u64 gx_stat_verts(void);
    u64 last_t = tb_now(), last_idle = 0, last_gx = 0, last_draws = 0, last_frames = 0, last_verts = 0, last_syncs = 0;
    for (;;) {
        Sleep(5000);
        u64 t = tb_now(), dt = t - last_t, verts = gx_stat_verts();
        double draws = (double)(g_gx_draws - last_draws);
        fprintf(stderr, "[perf] %.1f quadros/s  %.0f desenhos/s (%.1f vértices/desenho)  ocioso %.0f%%  renderizador %.0f%%  esperas pela GPU %.0f/s\n",
                (double)(g_gx_frames - last_frames) * TB_HZ / dt, draws * TB_HZ / dt,
                draws ? (double)(verts - last_verts) / draws : 0.0, 100.0 * (g_idle_tb - last_idle) / dt,
                100.0 * (g_gx_tb - last_gx) / dt, (double)(g_gx_syncs - last_syncs) * TB_HZ / dt);
        last_syncs = g_gx_syncs;
        last_t = t; last_idle = g_idle_tb; last_gx = g_gx_tb; last_draws = g_gx_draws; last_frames = g_gx_frames;
        last_verts = verts;
        extern u32 g_pad_reads;
        extern volatile s32 g_audio_peak;
        extern volatile u64 g_audio_frames, g_dsp_cycles, g_audio_underruns;
        fprintf(stderr, "[rt] t=%.1fs retraces=%u fifo=%llu KiB dvd=%llu KiB pad=%u dsp=%lluM ciclos áudio=%llu quadros (faltas %llu) pico=%d\n",
                (double)tb_now() / TB_HZ, g_retrace_count, g_fifo_bytes >> 10, g_dvd_bytes >> 10, g_pad_reads,
                g_dsp_cycles / 1000000, g_audio_frames, g_audio_underruns, g_audio_peak);
        g_audio_peak = 0;
    }
}

/* O exe é do subsistema GUI (sem janela de console ao abrir com dois cliques). Saída herdada
 * (pipe ou arquivo) continua valendo; aberto de um terminal, escreve nele; sem nenhum dos dois,
 * o log vai para metalarms.log ao lado do executável. */
static void setup_log(void) {
    extern int g_log_to_file;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
        return;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, sizeof path - (sl ? (size_t)(sl + 1 - path) : 0), "metalarms.log");
    if (freopen(path, "w", stderr)) g_log_to_file = 1;
}

int main(int argc, char **argv) {
    setup_log();
    const char *disc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else disc = argv[i];
    }
    extern void config_init(void);
    extern const char *config_disc(const char *arg);
    config_init();
    disc = config_disc(disc);
    if (!disc) {
        rt_message("Imagem do disco não encontrada.\n\nUso: metalarms [-v] [imagem do disco .iso|.ciso]\n"
                   "Sem argumento, usa a chave \"disco\" do metalarms.ini ou procura o disco ao lado do executável.");
        return 2;
    }
    ppc_mem_init();
    (void)tb_now(); /* inicia o relógio antes das threads do DSP e do áudio */
    mmio_init();
    gx_init();
    if (!disc_open(disc)) {
        char msg[MAX_PATH + 64];
        snprintf(msg, sizeof msg, "Não consegui abrir a imagem do disco:\n%s", disc);
        rt_message(msg);
        return 2;
    }
    char id[7] = {0};
    disc_read(0, id, 6);
    if (strcmp(id, "GM5E7D") != 0) {
        char msg[128];
        snprintf(msg, sizeof msg, "Este disco é %s; o port foi feito para o Metal Arms americano (GM5E7D).", id);
        rt_message(msg);
        return 2;
    }

    u32 entry = load_dol_from_disc();
    setup_boot_info();

    PPCContext *c;
    sched_init_main(&c);
    c->msr = 0x2000; /* FP ligado, interrupções desligadas como no boot */
    g_poll_hook = sched_poll;
    vi_start_retrace();
    CreateThread(NULL, 0, heartbeat, NULL, 0, NULL);
    extern void profiler_start(void);
    profiler_start();

    LOG("[rt] executando __start em %08X", entry);
    PPCFunc start = ppc_lookup(entry);
    if (!start) rt_fatal("sem função no entry point %08X", entry);
    start(c);
    LOG("[rt] __start retornou");
    return 0;
}
