/* DSP de áudio do GameCube (emulação de baixo nível do processador de 16 bits).
 * Roda numa thread própria; a CPU conversa com ele pelos mailboxes e pelo DSPCR. */
#ifndef DSP_H
#define DSP_H

#include "../ppc.h"

int dsp_init(const char *rom_dir, u8 *aram, u32 aram_size);

/* Lado da CPU (registradores 0xCC0050xx) */
u16 dsp_cpu_read_mail_hi(int cpu_mailbox);
u16 dsp_cpu_read_mail_lo(int cpu_mailbox);
void dsp_cpu_write_mail_hi(u16 v);
void dsp_cpu_write_mail_lo(u16 v);
u16 dsp_cpu_read_control(void);           /* bits 0x0C07 do DSPCR */
u16 dsp_cpu_write_control(u16 v);         /* devolve o novo valor dos bits 0x0C07 */
int dsp_take_cpu_interrupt(void);         /* 1 se o DSP pediu interrupção (DIRQ) desde a última chamada */

extern volatile u64 g_dsp_cycles;

#endif
