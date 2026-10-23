/* Memory card em HLE: a API CARD* do SDK é atendida aqui e cada save do jogo vira
 * um arquivo .gci em saves/slotA (o mesmo formato que o Dolphin importa/exporta).
 * Slot A: cartão de 59 blocos. Slot B: vazio. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <direct.h>
#include <io.h>
#include <time.h>

#include "rt.h"

enum {
    CARD_READY = 0, CARD_BUSY = -1, CARD_NOCARD = -3, CARD_NOFILE = -4, CARD_EXIST = -7, CARD_NOENT = -8,
    CARD_INSSPACE = -9, CARD_NAMETOOLONG = -12, CARD_FATAL = -128
};

#define SECTOR 8192u
#define USER_BLOCKS 59u
#define MAX_FILES 127

typedef struct {
    int used;
    u8 dir[0x40]; /* entrada de diretório no formato GCI (big-endian) */
    u8 *data;
    u32 size;
    char path[512]; /* arquivo de onde veio: o save é regravado nele, como no Dolphin */
} CardFile;

static CardFile g_files[MAX_FILES];
static int g_loaded;
static const char *k_dir = "saves/slotA"; /* MA_SAVE_DIR troca (testes automáticos) */

/* ---- entrada de diretório (GCI) ------------------------------------------------- */

static u16 rd16be(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void wr16be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32be(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static u32 card_time(void) { return (u32)(time(NULL) - 946684800); } /* segundos desde 2000 */

/* Arquivo de um save: o mesmo de onde foi lido; save novo usa o nome que o Dolphin daria
 * (fabricante-jogo-nome.gci), para a pasta poder ser usada pelos dois. */
static void file_path(const CardFile *f, char *out, size_t n) {
    if (f->path[0]) {
        snprintf(out, n, "%s", f->path);
        return;
    }
    char name[33] = {0};
    memcpy(name, f->dir + 8, 32);
    for (char *p = name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            *p = '_';
    snprintf(out, n, "%s/%.2s-%.4s-%s.gci", k_dir, (const char *)f->dir + 4, (const char *)f->dir, name);
}

static void save_file(CardFile *f) {
    char dir[512]; /* cria a pasta e as intermediárias */
    snprintf(dir, sizeof dir, "%s", k_dir);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; _mkdir(dir); *p = c; }
    _mkdir(dir);
    char path[512];
    file_path(f, path, sizeof path);
    snprintf(f->path, sizeof f->path, "%s", path);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOG("[card] não consegui gravar %s", path);
        return;
    }
    fwrite(f->dir, 1, 0x40, fp);
    fwrite(f->data, 1, f->size, fp);
    fclose(fp);
}

static void load_all(void) {
    if (g_loaded) return;
    g_loaded = 1;
    if (getenv("MA_SAVE_DIR")) k_dir = getenv("MA_SAVE_DIR");
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s/*.gci", k_dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return;
    int n = 0;
    do {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", k_dir, fd.name);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        CardFile f = {0};
        if (fread(f.dir, 1, 0x40, fp) == 0x40) {
            f.size = rd16be(f.dir + 0x38) * SECTOR;
            /* mesmas recusas do Dolphin: tamanho que não bate com os blocos e save repetido */
            int dup = 0;
            for (int i = 0; i < MAX_FILES; i++)
                if (g_files[i].used && !memcmp(g_files[i].dir, f.dir, 6) && !memcmp(g_files[i].dir + 8, f.dir + 8, 32))
                    dup = 1;
            if ((u32)fd.size != 0x40 + f.size || !f.size || dup) {
                LOG("[card] ignorando %s: %s", fd.name, dup ? "save repetido (mesmo jogo e nome de outro arquivo)"
                                                              : "tamanho não bate com o número de blocos");
                fclose(fp);
                continue;
            }
            snprintf(f.path, sizeof f.path, "%s", path);
            f.data = (u8 *)calloc(1, f.size ? f.size : 1);
            fread(f.data, 1, f.size, fp);
            for (int i = 0; i < MAX_FILES; i++)
                if (!g_files[i].used) {
                    f.used = 1;
                    g_files[i] = f;
                    n++;
                    break;
                }
        }
        fclose(fp);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    LOG("[card] slot A: %d arquivo(s) em %s", n, k_dir);
}

static int same_game(const CardFile *f) { return memcmp(f->dir, host_ptr(0x80000000), 4) == 0 && memcmp(f->dir + 4, host_ptr(0x80000004), 2) == 0; }

static int find_file(const char *name) {
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used && same_game(&g_files[i]) && strncmp((const char *)g_files[i].dir + 8, name, 32) == 0) return i;
    return -1;
}

static u32 used_blocks(void) {
    u32 b = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used) b += rd16be(g_files[i].dir + 0x38);
    return b;
}

/* CARDFileInfo: chan 0x00, fileNo 0x04, offset 0x08, length 0x0C, iBlock 0x10 */
static void fill_info(u32 fi, int chan, int no) {
    st32(fi + 0x00, (u32)chan);
    st32(fi + 0x04, (u32)no);
    st32(fi + 0x08, 0);
    st32(fi + 0x0C, g_files[no].size);
    st16(fi + 0x10, (u16)(5 + no));
}

/* ---- callbacks assíncronos ---------------------------------------------------------- */

static void cb_event(PPCContext *c, u64 packed) {
    u32 cb = (u32)(packed >> 32);
    s32 chan = (s32)((packed >> 16) & 0xFF), result = (s16)(u16)packed;
    guest_callback(c, cb, (u32)chan, (u32)result, 0);
}

static s32 finish(u32 cb, s32 chan, s32 result) {
    if (cb) ev_schedule(0, cb_event, ((u64)cb << 32) | ((u64)(chan & 0xFF) << 16) | (u16)result);
    return result;
}

/* ---- API ------------------------------------------------------------------------- */

void hle_CARDInit(PPCContext *c) { (void)c; }

void hle_CARDProbeEx(PPCContext *c) {
    s32 chan = (s32)ARG(0);
    if (chan != 0) { RET(CARD_NOCARD); return; }
    if (ARG(1)) st32(ARG(1), 4);      /* 4 Mbit = 59 blocos */
    if (ARG(2)) st32(ARG(2), SECTOR);
    RET(CARD_READY);
}

void hle_CARDMount(PPCContext *c) {
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    load_all();
    RET(CARD_READY);
}

void hle_CARDMountAsync(PPCContext *c) {
    s32 chan = (s32)ARG(0);
    if (chan != 0) { RET(CARD_NOCARD); return; }
    load_all();
    RET(finish(ARG(3), chan, CARD_READY)); /* attachCallback */
}

void hle_CARDUnmount(PPCContext *c) { RET(ARG(0) == 0 ? CARD_READY : CARD_NOCARD); }
void hle_CARDCheck(PPCContext *c) { RET(ARG(0) == 0 ? CARD_READY : CARD_NOCARD); }

void hle_CARDCheckExAsync(PPCContext *c) {
    s32 chan = (s32)ARG(0);
    if (chan != 0) { RET(CARD_NOCARD); return; }
    if (ARG(1)) st32(ARG(1), 0);
    RET(finish(ARG(2), chan, CARD_READY));
}

void hle_CARDGetSectorSize(PPCContext *c) {
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    st32(ARG(1), SECTOR);
    RET(CARD_READY);
}

void hle_CARDFreeBlocks(PPCContext *c) {
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    load_all();
    int files = 0;
    for (int i = 0; i < MAX_FILES; i++) files += g_files[i].used;
    if (ARG(1)) st32(ARG(1), (USER_BLOCKS - used_blocks()) * SECTOR);
    if (ARG(2)) st32(ARG(2), (u32)(MAX_FILES - files));
    RET(CARD_READY);
}

void hle_CARDFormat(PPCContext *c) {
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used) {
            char path[512];
            file_path(&g_files[i], path, sizeof path);
            remove(path);
            free(g_files[i].data);
            g_files[i].used = 0;
        }
    RET(CARD_READY);
}

void hle_CARDOpen(PPCContext *c) {
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    load_all();
    int no = find_file((const char *)host_ptr(ARG(1)));
    if (no < 0) { RET(CARD_NOFILE); return; }
    fill_info(ARG(2), 0, no);
    RET(CARD_READY);
}

void hle_CARDFastOpen(PPCContext *c) {
    s32 no = (s32)ARG(1);
    if (ARG(0) != 0) { RET(CARD_NOCARD); return; }
    if (no < 0 || no >= MAX_FILES || !g_files[no].used) { RET(CARD_NOFILE); return; }
    fill_info(ARG(2), 0, no);
    RET(CARD_READY);
}

void hle_CARDClose(PPCContext *c) {
    st32(ARG(0), (u32)-1);
    RET(CARD_READY);
}

static s32 do_create(s32 chan, const char *name, u32 size, u32 fi) {
    if (chan != 0) return CARD_NOCARD;
    load_all();
    if (strlen(name) > 32) return CARD_NAMETOOLONG;
    if (size == 0 || size % SECTOR) return CARD_FATAL;
    if (find_file(name) >= 0) return CARD_EXIST;
    if (used_blocks() + size / SECTOR > USER_BLOCKS) return CARD_INSSPACE;
    int no = -1;
    for (int i = 0; i < MAX_FILES; i++)
        if (!g_files[i].used) { no = i; break; }
    if (no < 0) return CARD_NOENT;
    CardFile *f = &g_files[no];
    memset(f, 0, sizeof *f);
    f->used = 1;
    memset(f->dir, 0xFF, 0x40);
    memcpy(f->dir, host_ptr(0x80000000), 4);
    memcpy(f->dir + 4, host_ptr(0x80000004), 2);
    f->dir[7] = 0;                         /* sem banner */
    memset(f->dir + 8, 0, 32);
    strncpy((char *)f->dir + 8, name, 32);
    wr32be(f->dir + 0x28, card_time());
    wr32be(f->dir + 0x2C, 0xFFFFFFFF);     /* sem ícone */
    wr16be(f->dir + 0x30, 0);
    wr16be(f->dir + 0x32, 0);
    f->dir[0x34] = 4;                      /* permissão pública */
    f->dir[0x35] = 0;
    wr16be(f->dir + 0x36, (u16)(5 + no));
    wr16be(f->dir + 0x38, (u16)(size / SECTOR));
    wr32be(f->dir + 0x3C, 0xFFFFFFFF);     /* sem comentário */
    f->size = size;
    f->data = (u8 *)calloc(1, size);
    save_file(f);
    fill_info(fi, 0, no);
    VLOG("[card] criado \"%s\" (%u bytes)", name, size);
    return CARD_READY;
}

void hle_CARDCreate(PPCContext *c) { RET(do_create((s32)ARG(0), (const char *)host_ptr(ARG(1)), ARG(2), ARG(3))); }
void hle_CARDCreateAsync(PPCContext *c) {
    s32 r = do_create((s32)ARG(0), (const char *)host_ptr(ARG(1)), ARG(2), ARG(3));
    RET(finish(ARG(4), (s32)ARG(0), r));
}

static CardFile *file_from_info(u32 fi) {
    s32 chan = (s32)ld32(fi), no = (s32)ld32(fi + 4);
    if (chan != 0 || no < 0 || no >= MAX_FILES || !g_files[no].used) return NULL;
    return &g_files[no];
}

static s32 do_rw(u32 fi, u32 buf, u32 len, u32 off, int write) {
    CardFile *f = file_from_info(fi);
    if (!f) return CARD_NOFILE;
    if ((u64)off + len > f->size) return CARD_FATAL;
    VLOG("[card] %s \"%.32s\" %X bytes em +%X", write ? "grava" : "lê", (const char *)f->dir + 8, len, off);
    if (write) {
        memcpy(f->data + off, host_ptr(buf), len);
        wr32be(f->dir + 0x28, card_time());
        save_file(f);
    } else {
        memcpy(host_ptr(buf), f->data + off, len);
    }
    st32(fi + 0x08, off + len);
    return CARD_READY;
}

void hle_CARDRead(PPCContext *c) { RET(do_rw(ARG(0), ARG(1), ARG(2), ARG(3), 0)); }
void hle_CARDWrite(PPCContext *c) { RET(do_rw(ARG(0), ARG(1), ARG(2), ARG(3), 1)); }
void hle_CARDReadAsync(PPCContext *c) {
    s32 r = do_rw(ARG(0), ARG(1), ARG(2), ARG(3), 0);
    RET(finish(ARG(4), (s32)ld32(ARG(0)), r));
}
void hle_CARDWriteAsync(PPCContext *c) {
    s32 r = do_rw(ARG(0), ARG(1), ARG(2), ARG(3), 1);
    RET(finish(ARG(4), (s32)ld32(ARG(0)), r));
}

void hle_CARDDeleteAsync(PPCContext *c) {
    s32 chan = (s32)ARG(0);
    s32 r = CARD_NOCARD;
    if (chan == 0) {
        int no = find_file((const char *)host_ptr(ARG(1)));
        if (no < 0) {
            r = CARD_NOFILE;
        } else {
            char path[512];
            file_path(&g_files[no], path, sizeof path);
            remove(path);
            free(g_files[no].data);
            g_files[no].used = 0;
            r = CARD_READY;
        }
    }
    RET(finish(ARG(2), chan, r));
}

/* CARDStat: fileName 0x00, length 0x20, time 0x24, gameName 0x28, company 0x2C, bannerFormat 0x2E,
 * iconAddr 0x30, iconFormat 0x34, iconSpeed 0x36, commentAddr 0x38, offsets 0x3C..0x6B */
static void icon_offsets(u32 st) {
    u32 off = ld32(st + 0x30);
    u8 banner = ld8(st + 0x2E) & 3;
    u16 fmt = ld16(st + 0x34);
    if (off == 0xFFFFFFFF) {
        for (u32 o = 0x3C; o < 0x6C; o += 4) st32(st + o, 0xFFFFFFFF);
        st32(st + 0x68, 0);
        return;
    }
    st32(st + 0x3C, banner ? off : 0xFFFFFFFF);
    if (banner == 1) { off += 96 * 32; st32(st + 0x40, off); off += 512; } /* C8 + paleta */
    else { st32(st + 0x40, 0xFFFFFFFF); if (banner == 2) off += 96 * 32 * 2; }
    int any_c8 = 0;
    for (int i = 0; i < 8; i++) {
        u32 f = (fmt >> (2 * i)) & 3;
        if (!f) { st32(st + 0x44 + 4 * i, 0xFFFFFFFF); continue; }
        st32(st + 0x44 + 4 * i, off);
        off += f == 1 ? 32 * 32 : 32 * 32 * 2;
        if (f == 1) any_c8 = 1;
    }
    if (any_c8) { st32(st + 0x64, off); off += 512; } else st32(st + 0x64, 0xFFFFFFFF);
    st32(st + 0x68, off);
}

void hle_CARDGetStatus(PPCContext *c) {
    s32 chan = (s32)ARG(0), no = (s32)ARG(1);
    u32 st = ARG(2);
    if (chan != 0) { RET(CARD_NOCARD); return; }
    if (no < 0 || no >= MAX_FILES || !g_files[no].used) { RET(CARD_NOFILE); return; }
    const CardFile *f = &g_files[no];
    memcpy(host_ptr(st), f->dir + 8, 32);
    st32(st + 0x20, f->size);
    st32(st + 0x24, rd32be(f->dir + 0x28));
    memcpy(host_ptr(st + 0x28), f->dir, 4);
    memcpy(host_ptr(st + 0x2C), f->dir + 4, 2);
    st8(st + 0x2E, f->dir[7]);
    st32(st + 0x30, rd32be(f->dir + 0x2C));
    st16(st + 0x34, rd16be(f->dir + 0x30));
    st16(st + 0x36, rd16be(f->dir + 0x32));
    st32(st + 0x38, rd32be(f->dir + 0x3C));
    icon_offsets(st);
    RET(CARD_READY);
}

void hle_CARDSetStatusAsync(PPCContext *c) {
    s32 chan = (s32)ARG(0), no = (s32)ARG(1);
    u32 st = ARG(2);
    s32 r = CARD_READY;
    if (chan != 0) r = CARD_NOCARD;
    else if (no < 0 || no >= MAX_FILES || !g_files[no].used) r = CARD_NOFILE;
    else {
        CardFile *f = &g_files[no];
        f->dir[7] = ld8(st + 0x2E);
        wr32be(f->dir + 0x2C, ld32(st + 0x30));
        wr16be(f->dir + 0x30, ld16(st + 0x34));
        wr16be(f->dir + 0x32, ld16(st + 0x36));
        wr32be(f->dir + 0x3C, ld32(st + 0x38));
        wr32be(f->dir + 0x28, card_time());
        save_file(f);
        icon_offsets(st);
    }
    RET(finish(ARG(3), chan, r));
}

/* SRAM: o runtime não emula a SRAM do relógio; o som sai em estéreo */
void hle_OSGetSoundMode(PPCContext *c) { RET(1); }
