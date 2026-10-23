/* Leitura da imagem do disco do usuário: ISO crua ou CISO (formato do Dolphin). */
#include "rt.h"

static FILE *g_disc;
static u32 g_block_size;      /* 0 = ISO crua */
static u64 *g_block_pos;      /* posição no arquivo de cada bloco, ~0 = bloco de zeros */
static u32 g_block_count;

void disc_close(void) {
    if (g_disc) fclose(g_disc);
    free(g_block_pos);
    g_disc = NULL;
    g_block_pos = NULL;
    g_block_size = 0;
}

int disc_open(const char *path) {
    disc_close();
    g_disc = fopen(path, "rb");
    if (!g_disc) return 0;
    u8 head[0x8000];
    if (fread(head, 1, sizeof head, g_disc) != sizeof head) { disc_close(); return 0; }
    if (memcmp(head, "CISO", 4) == 0) {
        g_block_size = head[4] | (head[5] << 8) | (head[6] << 16) | ((u32)head[7] << 24);
        g_block_count = 0x8000 - 8;
        g_block_pos = (u64 *)malloc(sizeof(u64) * g_block_count);
        u64 pos = 0x8000;
        for (u32 i = 0; i < g_block_count; i++) {
            if (head[8 + i]) {
                g_block_pos[i] = pos;
                pos += g_block_size;
            } else {
                g_block_pos[i] = ~0ull;
            }
        }
    }
    return 1;
}

int disc_read(u64 offset, void *dst, u32 len) {
    u8 *out = (u8 *)dst;
    if (!g_block_size) {
        _fseeki64(g_disc, (s64)offset, SEEK_SET);
        return fread(out, 1, len, g_disc) == len;
    }
    while (len) {
        u64 idx = offset / g_block_size;
        u32 inner = (u32)(offset % g_block_size);
        u32 n = g_block_size - inner < len ? g_block_size - inner : len;
        if (idx >= g_block_count || g_block_pos[idx] == ~0ull) {
            memset(out, 0, n);
        } else {
            _fseeki64(g_disc, (s64)(g_block_pos[idx] + inner), SEEK_SET);
            if (fread(out, 1, n, g_disc) != n) return 0;
        }
        out += n;
        offset += n;
        len -= n;
    }
    return 1;
}

u32 disc_read32(u64 offset) {
    u8 b[4];
    disc_read(offset, b, 4);
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
