# Recompilador PowerPC (Gekko) → C

## Peças

| Arquivo | Papel |
|---|---|
| `recomp/gen.py` | Gerador. Decodifica as palavras do `main.dol` e emite C. Os limites de função e as tabelas de salto vêm do `config/GM5E7D/symbols.txt` do dtk. |
| `runtime/ppc.h` | Estado da CPU (`PPCContext`), memória big-endian, CR/XER, quantização GQR dos paired singles. |
| `runtime/host.c` | Serviços mínimos: carregar o DOL, despacho indireto por tabela, erros. O MMIO ainda é stub. |
| `tests/test_core.c` | Testes de funções puras recompiladas contra resultados do host. |

## Comandos

```bash
# binário inteiro (12.328 funções, cerca de 48 MB de C em build/recomp)
python -I recomp/gen.py orig/GM5E7D/sys/main.dol config/GM5E7D/symbols.txt build/recomp

# testes (gera só as funções usadas, mais dependências diretas e callbacks lis/addi)
bash tests/build_test_core.sh && ./tests/test_core.exe
```

## Modelo de execução

- Cada função PPC vira `void f_XXXXXXXX(PPCContext *c)`. Os registradores ficam em `c->r[]`, `c->f[].ps0/ps1`, `c->cr[8]` etc.
- `bl` vira chamada C direta, e `c->lr` recebe o endereço de retorno, porque alguns códigos leem o LR.
- `blr` vira `return`. Isso pressupõe que o LR contém o endereço de retorno da chamada, o que vale para código CW normal.
- Um `b` ou `bc` dentro da função vira `goto`. Para fora, vira tail call (`f_X(c); return;`).
- `bctrl`/`blrl` vão por `ppc_call_indirect`, que faz busca binária em `g_ppc_funcs[]`.
- `bctr` dentro de uma função com tabela de salto vira `switch` sobre os alvos da tabela.
- Um alvo de `bl`/`b` no meio de uma função vira uma entrada própria, que vai daquele ponto até o fim da função (é o caso de `_savegpr_N`/`_restgpr_N`).
- A memória é um buffer de 32 MiB com `ea & 0x01FFFFFF`, espelhando 0x8xxxxxxx e 0xCxxxxxxx. Os endereços 0xCC/0xC8 vão para `mmio_*`.

## Regras do Gekko implementadas

- Instruções simples (`fadds` etc.), `frsp`, `fres` e `lfs` preenchem ps0 e ps1. As duplas e `fmr`/`fneg`/`fabs` escrevem só ps0.
- `psq_l`/`psq_st` com tipo e escala do GQR (float, u8, u16, s8, s16), com saturação na escrita.
- `fctiw(z)` grava o inteiro nos 32 bits baixos com o padrão `0xFFF80000` no alto.
- Carry (`addc`/`adde`/`subfe`/`srawi`...), `Rc=1` nas inteiras, `mtcrf`/`mfcr`, `mtfsf`/`mffs`.

## Runtime do jogo (boot)

```bash
bash tools/build/build_game.sh          # gera + compila incremental (/Od) + linka build/metalarms.exe
./build/metalarms.exe [-v] "<imagem .ciso ou .iso>"
MA_FIFO_DUMP=build/fifo.bin ./build/metalarms.exe ...   # grava o fluxo de comandos GX
```

| Arquivo | Papel |
|---|---|
| `runtime/main.c` | Carrega o DOL do disco, monta BootInfo/FST/bi2 como o apploader e chama `__start`. |
| `runtime/sched.c` | Threads do OS como fibers do Windows, fila de eventos, interrupções e timebase (40,5 MHz). |
| `runtime/mmio.c` | Banco de registradores 0xCC00xxxx com ganchos: retrace do VI, PE (BP 0x45/0x48 no FIFO), EXI, DMA da ARAM, DMA do AI, mailbox do DSP. |
| `runtime/hle_dvd.c`, `disc.c` | Comandos de drive em HLE, lendo da imagem CISO/ISO. O sistema de arquivos (FST) continua recompilado. |
| `runtime/hle_os.c` | `OSReport`/`OSPanic` (via `sprintf` do jogo), alarmes, PAD pelo teclado e CARD ausente. |
| `config/GM5E7D/hle.txt` | Lista de funções substituídas. O original fica como `f_<addr>_orig`. |

Estado em 2026-10-21: o boot completo do SDK funciona (OS, DVD, VI, AR, ARQ, AI, GX, DSP). O motor cria as threads, carrega o `mettlearms_gc.mst` e toca a sequência de abertura (`gc_Loading2`, `gc_License` com `MA_Theme.wvs`, logos VUG/SOL/SAS). Ele chega ao vídeo de demonstração (`GC_Demo_Mov.bik`) sem travar. Ainda não há vídeo nem áudio na tela: o FIFO do GX é só contado ou gravado, e o microcódigo do MusyX só tem o protocolo de carga emulado.

## Limitações conhecidas

- **Ponto flutuante não é bit-exato.** `fmadd` é calculado como produto mais soma em double, sem o arredondamento de 25 bits do Gekko. `fres` e `frsqrte` dão o valor exato em vez da estimativa do hardware.
- **Saltos não locais** (`setjmp`/`longjmp`, unwinding de exceção C++) não funcionam com `blr` = `return`. Precisam de HLE quando aparecerem.
- **Exceções e interrupções do hardware** (`rfi`, vetores em 0x80000xxx) não são executadas. A ideia é substituir o OS por HLE.
- `Rc=1` em instruções FP e `OE=1` em inteiras caem em `ppc_unimplemented` (não aparecem no jogo).
- Tabelas de salto só são conhecidas se o dtk as detectou (126 detectadas).
- Desempenho ainda não foi trabalhado: todo acesso à memória testa MMIO.
