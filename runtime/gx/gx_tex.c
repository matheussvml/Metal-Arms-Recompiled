/* Decodificação das texturas do GX (layout em blocos de 32 bytes) para RGBA8. */
#include "gx_internal.h"

u8 g_tmem[0x100000];

enum { TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
       TF_CI4 = 8, TF_CI8 = 9, TF_CI14 = 10, TF_CMPR = 14 };

static void block_dims(u32 fmt, u32 *bw, u32 *bh) {
    switch (fmt) {
    case TF_I4: case TF_CI4: case TF_CMPR: *bw = 8; *bh = 8; break;
    case TF_I8: case TF_IA4: case TF_CI8: *bw = 8; *bh = 4; break;
    default: *bw = 4; *bh = 4; break;
    }
}

u32 gx_texture_size(u32 w, u32 h, u32 fmt) {
    u32 bw, bh;
    block_dims(fmt, &bw, &bh);
    u32 blocks = ((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    return blocks * (fmt == TF_RGBA8 ? 64 : 32);
}

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

static void rgb565(u16 v, u8 *o) {
    o[0] = (u8)((((v >> 11) & 31) << 3) | (((v >> 11) & 31) >> 2));
    o[1] = (u8)((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4));
    o[2] = (u8)(((v & 31) << 3) | ((v & 31) >> 2));
    o[3] = 255;
}

static void rgb5a3(u16 v, u8 *o) {
    if (v & 0x8000) {
        o[0] = (u8)((((v >> 10) & 31) << 3) | (((v >> 10) & 31) >> 2));
        o[1] = (u8)((((v >> 5) & 31) << 3) | (((v >> 5) & 31) >> 2));
        o[2] = (u8)(((v & 31) << 3) | ((v & 31) >> 2));
        o[3] = 255;
    } else {
        o[0] = (u8)(((v >> 8) & 15) * 17);
        o[1] = (u8)(((v >> 4) & 15) * 17);
        o[2] = (u8)((v & 15) * 17);
        o[3] = (u8)(((v >> 12) & 7) * 255 / 7);
    }
}

static void tlut_color(u32 tlut_addr, u32 tlut_fmt, u32 idx, u8 *o) {
    u16 v = be16(g_tmem + ((tlut_addr + idx * 2) & (sizeof g_tmem - 1)));
    if (tlut_fmt == 0) { o[0] = o[1] = o[2] = (u8)v; o[3] = (u8)(v >> 8); } /* IA8 */
    else if (tlut_fmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

static void cmpr_block(const u8 *src, u8 *dst, u32 stride, u32 x0, u32 y0, u32 w, u32 h) {
    u16 c0 = be16(src), c1 = be16(src + 2);
    u8 pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    for (int k = 0; k < 3; k++) {
        if (c0 > c1) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = pal[2][k];
        }
    }
    pal[2][3] = 255;
    pal[3][3] = c0 > c1 ? 255 : 0;
    for (u32 y = 0; y < 4; y++) {
        u8 bits = src[4 + y];
        for (u32 x = 0; x < 4; x++) {
            u32 px = x0 + x, py = y0 + y;
            if (px >= w || py >= h) continue;
            memcpy(dst + (py * stride + px) * 4, pal[(bits >> (6 - 2 * x)) & 3], 4);
        }
    }
}

int gx_decode_texture(u8 *dst, const u8 *src, u32 w, u32 h, u32 fmt, u32 tlut_addr, u32 tlut_fmt) {
    u32 bw, bh;
    block_dims(fmt, &bw, &bh);
    if (fmt > TF_CMPR || (fmt == 7) || (fmt > TF_CI14 && fmt != TF_CMPR)) return 0;
    for (u32 by = 0; by < h; by += bh) {
        for (u32 bx = 0; bx < w; bx += bw) {
            if (fmt == TF_CMPR) {
                for (u32 sb = 0; sb < 4; sb++, src += 8)
                    cmpr_block(src, dst, w, bx + (sb & 1) * 4, by + (sb >> 1) * 4, w, h);
                continue;
            }
            if (fmt == TF_RGBA8) {
                for (u32 i = 0; i < 16; i++) {
                    u32 px = bx + (i & 3), py = by + (i >> 2);
                    if (px >= w || py >= h) continue;
                    u8 *o = dst + (py * w + px) * 4;
                    o[3] = src[i * 2];
                    o[0] = src[i * 2 + 1];
                    o[1] = src[32 + i * 2];
                    o[2] = src[32 + i * 2 + 1];
                }
                src += 64;
                continue;
            }
            for (u32 y = 0; y < bh; y++) {
                for (u32 x = 0; x < bw; x++) {
                    u32 px = bx + x, py = by + y;
                    u32 i = y * bw + x;
                    u8 o[4];
                    switch (fmt) {
                    case TF_I4: { u8 v = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15; o[0] = o[1] = o[2] = o[3] = (u8)(v * 17); break; }
                    case TF_I8: o[0] = o[1] = o[2] = o[3] = src[i]; break;
                    case TF_IA4: { u8 v = src[i]; o[0] = o[1] = o[2] = (u8)((v & 15) * 17); o[3] = (u8)((v >> 4) * 17); break; }
                    case TF_IA8: o[3] = src[i * 2]; o[0] = o[1] = o[2] = src[i * 2 + 1]; break;
                    case TF_RGB565: rgb565(be16(src + i * 2), o); break;
                    case TF_RGB5A3: rgb5a3(be16(src + i * 2), o); break;
                    case TF_CI4: tlut_color(tlut_addr, tlut_fmt, (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15, o); break;
                    case TF_CI8: tlut_color(tlut_addr, tlut_fmt, src[i], o); break;
                    default: tlut_color(tlut_addr, tlut_fmt, be16(src + i * 2) & 0x3FFF, o); break;
                    }
                    if (px < w && py < h) memcpy(dst + (py * w + px) * 4, o, 4);
                }
            }
            src += 32;
        }
    }
    return 1;
}
