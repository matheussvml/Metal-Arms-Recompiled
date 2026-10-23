/* Stubs de hardware para os testes unitários do recompilador (sem runtime do jogo). */
#include <stdio.h>

#include "ppc.h"

u64 ppc_timebase(void) {
    static u64 fake;
    return fake += 1000;
}

u32 mmio_read32(u32 ea) { fprintf(stderr, "mmio r32 %08X\n", ea); return 0; }
u16 mmio_read16(u32 ea) { fprintf(stderr, "mmio r16 %08X\n", ea); return 0; }
u8 mmio_read8(u32 ea) { fprintf(stderr, "mmio r8 %08X\n", ea); return 0; }
void mmio_write32(u32 ea, u32 v) { fprintf(stderr, "mmio w32 %08X = %08X\n", ea, v); }
void mmio_write16(u32 ea, u16 v) { fprintf(stderr, "mmio w16 %08X = %04X\n", ea, v); }
void mmio_write8(u32 ea, u8 v) { fprintf(stderr, "mmio w8 %08X = %02X\n", ea, v); }
