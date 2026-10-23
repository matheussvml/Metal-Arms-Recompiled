/* DVD: a biblioteca do SDK fica recompilada nas partes de sistema de arquivos
 * (FST, DVDOpen, DVDFastOpen, DVDReadAsyncPrio...) e o HLE assume a partir dos
 * comandos ao drive. A leitura acontece na entrega do evento, como se fosse a
 * interrupção do DI, e o callback recebe o número de bytes lidos. */
#include "rt.h"

/* DVDCommandBlock */
#define CB_COMMAND 0x08
#define CB_STATE 0x0C
#define CB_OFFSET 0x10
#define CB_LENGTH 0x14
#define CB_ADDR 0x18
#define CB_CURR 0x1C
#define CB_TRANSFERRED 0x20
#define CB_ID 0x24
#define CB_CALLBACK 0x28

enum { DVD_STATE_END = 0, DVD_STATE_BUSY = 1, DVD_STATE_CANCELED = 10 };
enum { CMD_READ = 1, CMD_READ_ID = 5, CMD_INQUIRY = 14, CMD_CANCEL_STREAM = 7 };

static int g_pending;
u64 g_dvd_bytes;

static void complete(PPCContext *c, u64 block64) {
    u32 block = (u32)block64;
    g_pending--;
    if ((s32)ld32(block + CB_STATE) != DVD_STATE_BUSY) return; /* cancelado */
    u32 cmd = ld32(block + CB_COMMAND), len = ld32(block + CB_LENGTH), addr = ld32(block + CB_ADDR);
    s32 result = (s32)len;
    if (cmd == CMD_READ) {
        u32 off = ld32(block + CB_OFFSET);
        if (!disc_read(off, host_ptr(addr), len)) {
            LOG("DVD: falha lendo %X bytes em %X", len, off);
            result = -1;
        }
        g_dvd_bytes += len;
        VLOG("DVD: leu %6X bytes do disco %08X para %08X", len, off, addr);
    } else if (cmd == CMD_READ_ID) {
        disc_read(0, host_ptr(addr), 0x20);
        result = 0x20;
    } else if (cmd == CMD_INQUIRY) {
        memset(host_ptr(addr), 0, 0x20);
        st16(addr + 2, 0x0001); /* revisão do drive */
        result = 0x20;
    } else {
        result = 0;
    }
    st32(block + CB_TRANSFERRED, len);
    st32(block + CB_STATE, DVD_STATE_END);
    u32 cb = ld32(block + CB_CALLBACK);
    if (cb) guest_callback(c, cb, (u32)result, block, 0);
}

static void submit(u32 block, u32 cmd, u32 addr, u32 len, u32 offset, u32 cb) {
    st32(block + CB_COMMAND, cmd);
    st32(block + CB_ADDR, addr);
    st32(block + CB_LENGTH, len);
    st32(block + CB_OFFSET, offset);
    st32(block + CB_CURR, 0);
    st32(block + CB_TRANSFERRED, 0);
    st32(block + CB_CALLBACK, cb);
    st32(block + CB_STATE, DVD_STATE_BUSY);
    g_pending++;
    ev_schedule(0, complete, block);
}

void hle_DVDInit(PPCContext *c) {
    PPCContext tmp = *c;
    ppc_lookup(SYM___DVDFSInit)(&tmp); /* lê a FST de BootInfo->FSTLocation */
    VLOG("DVDInit: FST em %08X", ld32(0x80000038));
}

void hle_DVDReadAbsAsyncPrio(PPCContext *c) {
    submit(ARG(0), CMD_READ, ARG(1), ARG(2), ARG(3), ARG(4));
    RET(1);
}

void hle_DVDReadAbsAsyncForBS(PPCContext *c) {
    submit(ARG(0), CMD_READ, ARG(1), ARG(2), ARG(3), ARG(4));
    RET(1);
}

void hle_DVDReadDiskID(PPCContext *c) {
    submit(ARG(0), CMD_READ_ID, ARG(1), 0x20, 0, ARG(2));
    RET(1);
}

void hle_DVDInquiryAsync(PPCContext *c) {
    submit(ARG(0), CMD_INQUIRY, ARG(1), 0x20, 0, ARG(2));
    RET(1);
}

void hle_DVDCancelStreamAsync(PPCContext *c) {
    submit(ARG(0), CMD_CANCEL_STREAM, 0, 0, 0, ARG(1));
    RET(1);
}

void hle_DVDCancelAsync(PPCContext *c) {
    u32 block = ARG(0), cb = ARG(1);
    if ((s32)ld32(block + CB_STATE) == DVD_STATE_BUSY) st32(block + CB_STATE, DVD_STATE_CANCELED);
    if (cb) guest_callback(c, cb, 0, block, 0);
    RET(1);
}

void hle_DVDCancel(PPCContext *c) {
    u32 block = ARG(0);
    if ((s32)ld32(block + CB_STATE) == DVD_STATE_BUSY) st32(block + CB_STATE, DVD_STATE_CANCELED);
    RET(0);
}

void hle_DVDGetDriveStatus(PPCContext *c) { RET(g_pending ? DVD_STATE_BUSY : DVD_STATE_END); }
void hle_DVDCheckDisk(PPCContext *c) { RET(1); }
void hle_DVDResume(PPCContext *c) { (void)c; }
void hle_DVDReset(PPCContext *c) { (void)c; }
