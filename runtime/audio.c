/* Saída de som: recebe os blocos de DMA do AI (PCM 16 bits big-endian, estéreo,
 * 32 kHz) e toca pelo waveOut do Windows. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "rt.h"

#define RATE 32000
#define RING_FRAMES (RATE / 2) /* meio segundo de folga */
#define BUF_FRAMES 640         /* 20 ms por buffer do waveOut */
#define NBUF 4

static s16 ring[RING_FRAMES * 2];
static volatile LONG ring_r, ring_w; /* em quadros (par L/R) */
static HWAVEOUT g_wave;
static WAVEHDR g_hdr[NBUF];
static s16 g_buf[NBUF][BUF_FRAMES * 2];
static int g_audio_on;
volatile s32 g_audio_peak; /* maior amplitude desde a última leitura (diagnóstico) */
volatile u64 g_audio_frames;
volatile u64 g_audio_underruns; /* quadros tocados como silêncio por falta de amostra (diagnóstico) */

/* MA_WAV_DUMP=arquivo.wav grava a saída do AI (diagnóstico) */
static FILE *g_wav;
static u32 g_wav_frames;

static void wav_header(FILE *f, u32 frames) {
    u32 data = frames * 4;
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                0x00, 0x7D, 0, 0, 0x00, 0xF4, 0x01, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    u32 riff = data + 36;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

void audio_push_ai_dma(u32 addr, u32 bytes) {
    const u8 *p = g_mem + (addr & MEM_MASK);
    u32 frames = bytes / 4;
    g_audio_frames += frames;
    if (g_wav) {
        for (u32 i = 0; i < frames; i++) {
            s16 lr[2] = {(s16)((p[4 * i + 2] << 8) | p[4 * i + 3]), (s16)((p[4 * i] << 8) | p[4 * i + 1])};
            fwrite(lr, 2, 2, g_wav);
        }
        g_wav_frames += frames;
        if ((g_wav_frames & 0x3FFF) < frames) { /* atualiza o cabeçalho de vez em quando */
            long pos = ftell(g_wav);
            wav_header(g_wav, g_wav_frames);
            fseek(g_wav, pos, SEEK_SET);
            fflush(g_wav);
        }
    }
    for (u32 i = 0; i < frames * 2; i++) {
        s32 s = (s16)((p[2 * i] << 8) | p[2 * i + 1]);
        if (s < 0) s = -s;
        if (s > g_audio_peak) g_audio_peak = s;
    }
    if (!g_audio_on) return;
    LONG w = ring_w;
    for (u32 i = 0; i < frames; i++) {
        LONG next = (w + 1) % RING_FRAMES;
        if (next == ring_r) break; /* cheio: descarta (o jogo está adiantado) */
        /* o AI entrega cada quadro como (direita, esquerda) */
        ring[w * 2 + 1] = (s16)((p[4 * i] << 8) | p[4 * i + 1]);
        ring[w * 2 + 0] = (s16)((p[4 * i + 2] << 8) | p[4 * i + 3]);
        w = next;
    }
    InterlockedExchange(&ring_w, w);
}

static void fill(int k) {
    s16 *out = g_buf[k];
    LONG rd = ring_r;
    for (int i = 0; i < BUF_FRAMES; i++) {
        if (rd == ring_w) { /* faltou amostra: silêncio */
            out[2 * i] = out[2 * i + 1] = 0;
            g_audio_underruns++;
            continue;
        }
        out[2 * i] = ring[rd * 2];
        out[2 * i + 1] = ring[rd * 2 + 1];
        rd = (rd + 1) % RING_FRAMES;
    }
    InterlockedExchange(&ring_r, rd);
}

static DWORD WINAPI audio_thread(LPVOID p) {
    (void)p;
    for (;;) {
        for (int k = 0; k < NBUF; k++) {
            if (g_hdr[k].dwFlags & WHDR_DONE) {
                fill(k);
                g_hdr[k].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_wave, &g_hdr[k], sizeof(WAVEHDR));
            }
        }
        Sleep(2);
    }
}

void audio_init(void) {
    const char *dump = getenv("MA_WAV_DUMP");
    if (dump && (g_wav = fopen(dump, "wb")) != NULL) wav_header(g_wav, 0);
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("[audio] waveOutOpen falhou: sem som");
        return;
    }
    for (int k = 0; k < NBUF; k++) {
        g_hdr[k].lpData = (LPSTR)g_buf[k];
        g_hdr[k].dwBufferLength = sizeof g_buf[k];
        waveOutPrepareHeader(g_wave, &g_hdr[k], sizeof(WAVEHDR));
        g_hdr[k].dwFlags |= WHDR_DONE;
    }
    g_audio_on = 1;
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    LOG("[audio] saída de som 32 kHz estéreo");
}
