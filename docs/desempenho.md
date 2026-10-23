# Desempenho

## Ferramentas

- `[perf]` no stderr a cada 5 s: quadros apresentados por segundo, desenhos por segundo, % de tempo ocioso da thread do jogo (esperando eventos), % gasto no backend gráfico e quantas vezes por segundo a thread do jogo esperou a GPU terminar. Se o ocioso é maior que zero, quem limita é o próprio jogo, por exemplo vídeos a 30 quadros por segundo.
- `MA_PROFILE=<espera>,<duração>` (`runtime/profiler.c`): profiler por amostragem da thread principal. Mostra as funções mais quentes, inclusive as do jogo (`f_XXXXXXXX`), e o total do código do jogo. Símbolos vêm do PDB via dbghelp.

## Histórico (tela título em 3D, ~9 mil desenhos por quadro)

| Mudança | Quadros/s |
|---|---|
| Início: código do jogo com `/Od`, FIFO interpretado a cada escrita | 15 |
| Código do jogo com `/O2 /Ob0` | 21 |
| FIFO em lote, vértices por índice, shader/estados/constantes reaproveitados | 31 |
| Acessos à memória inline (`/O2 /Ob1` com caminho rápido enxuto em `ppc.h`) | 52 |

## Gameplay (primeira fase, ~5 mil desenhos e ~32 vértices por desenho)

| Mudança | Quadros/s | Ocioso |
|---|---|---|
| Antes da cadência fixa do VI (retrace atrasando) | 55 | 13% |
| Retrace em cadência fixa de 59,94 Hz (mostra o custo real) | 45–52 | 22–30% |
| `__restrict` no contexto + junção de desenhos com o mesmo estado | 55–59 | 18–20% |
| GPU em thread própria | **60** (estável) | 45–64% |

O jogo mira 60 quadros por segundo. Quando um quadro passa de 16,7 ms, ele espera o retrace seguinte, por isso a média cai em degraus.

## Decisões

- **Compilação do código gerado:** `/O2 /Ob1`. Com `/Ob2`, ou com os acessos à memória gordos, o MSVC passa de 10 minutos por bloco. Com `/Ob0` os acessos viram chamadas de função. O caminho rápido de `ld*`/`st*` é deliberadamente curto: um teste de MMIO (`(hi|4)==0xCC`) e uma leitura direta. O build completo leva cerca de 10 minutos e o incremental, segundos.
- **GPU em thread própria** (`runtime/gx/gx_fifo.c`), como o modo dual-core do Dolphin. O write-gather pipe só junta bytes (`gx_fifo_put`) em blocos de 16 KiB, que vão para uma fila. A thread da GPU interpreta o FIFO, decodifica os vértices e chama o D3D11. A thread do jogo:
  - entrega o bloco parcial sem esperar (`gx_kick`) em `OSSleepThread` e quando fica ociosa;
  - espera a GPU terminar tudo (`gx_sync`) só ao ler o PE, o status do CP (`0x0000`) ou o EFB; na primeira fase isso acontece 0 vezes por segundo;
  - no retrace, espera apenas se a GPU estiver mais de um quadro atrás (`gx_retrace`), para o jogo não reescrever dados que a GPU ainda vai ler.

  Os eventos do PE (draw done, tokens) voltam por uma fila e só viram registradores e interrupções na thread do jogo (`gx_poll_async`, chamada no `sched_poll` e no laço ocioso, que acorda com um evento da GPU). A janela pertence à thread do jogo, que continua tratando mensagens enquanto espera a GPU. `MA_GPU_SYNC=1` volta a interpretar tudo na thread do jogo, para comparar. O ganho tirou do caminho do jogo a decodificação de vértices (16–18% da thread) e as chamadas ao D3D.
- **Cache de shaders em disco** (`shaders.cache` ao lado do exe, em `compile()`): bytecode indexado pelo hash do fonte HLSL, da entrada e do alvo. Na primeira fase, uma execução sem cache compila 66 shaders: o vertex shader leva 292 ms na abertura e os pixel shaders, 5 ms em média (pico de 10 ms) cada, na hora em que o efeito aparece. Vários de uma vez causam uma travadinha. Com o cache, a segunda execução não compila nenhum. Se o gerador mudar, o fonte muda e a entrada antiga só deixa de ser usada. Um registro cortado (jogo fechado no meio da gravação) faz o arquivo ser regravado só com o que é válido.
- **Ponteiro de escrita do PI:** só é materializado quando o jogo o lê.
- **Backend:** as constantes XF ficam divididas em 5 buffers, reenviados só quando a região muda. O pixel shader, o blend, o Z, o raster, o viewport e as texturas só são reaplicados quando os registradores BP correspondentes mudam (`g_bp_dirty`, por grupo de 8 registradores).
- **Chamadas indiretas:** tabela direta endereço → função, em vez de busca binária.
- **`__restrict` no `PPCContext`** das funções geradas: a RAM emulada nunca se sobrepõe ao contexto, então o MSVC mantém registradores do PPC em registradores do host entre stores.
- **Junção de desenhos:** desenhos consecutivos sem nenhuma mudança de BP ou XF viram um só `DrawIndexed`. `MA_BATCH_CHECK=1` confere isso registrador a registrador.
- **Vértice de 108 bytes** (antes 164): coordenadas de entrada com 2 componentes e índices de matriz em 1 byte.
- **Retrace em cadência fixa:** o próximo retrace é calculado a partir do início, e não a partir da entrega do anterior.
- **Relógio:** `tb_now` usa multiplicação em ponto fixo, sem divisão de 64 bits. O jogo lê o relógio com muita frequência, sobretudo no Bink.
