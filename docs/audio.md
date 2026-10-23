# Áudio (DSP do GameCube emulado)

O som do Metal Arms é feito pelo MusyX. A cada quadro de áudio (5 ms), a CPU manda uma lista de comandos para o microcódigo do MusyX rodando no DSP. O DSP lê as amostras ADPCM da ARAM pelo acelerador, mixa as vozes e devolve PCM por DMA. O AI toca esse PCM.

Em vez de reimplementar o MusyX, o port **emula o DSP** e roda o microcódigo original.

| Arquivo | Papel |
|---|---|
| `runtime/dsp/dsp.c` | Interpretador do DSP: as ~230 instruções e as operações paralelas, flags, laços em hardware, exceções, DMA, mailboxes e acelerador ADPCM/PCM. Roda numa thread própria a 81 MHz. |
| `runtime/mmio.c` | Lado da CPU: mailboxes (`0xCC005000`–`0x5006`), DSPCR, DMA da ARAM e DMA de áudio. As escritas no controle do DSP vão por uma fila sem bloqueio. |
| `runtime/audio.c` | Pega os blocos do DMA do AI (16 bits big-endian, 32 kHz) e toca pelo `waveOut`. `MA_WAV_DUMP=arquivo.wav` grava a saída. |
| `data/dsp/dsp_rom.bin`, `dsp_coef.bin` | ROM livre do DSP feita pelo projeto Dolphin (GPLv2). `MA_DSP_ROM=<pasta>` aponta para outra pasta, por exemplo um dump do próprio console. |

## Sequência observada (`MA_DSP_LOG=1`)

1. A ROM manda `0x8071FEED`.
2. A CPU envia o microcódigo do MusyX pelos comandos `0x80F3xxxx`: ele está em `0x803E3F60`, tem `0x1EC0` bytes e o ponto de entrada é `0x0010`.
3. O microcódigo responde `0xDCD10000` (tarefa iniciada).
4. Daí em diante, a cada quadro, a CPU manda `0xBABE0024` mais o endereço da lista (`0x817D4780`), o DSP responde `0xDCD10002` e a CPU responde `0xCDD10003`.

## Pontos de atenção

- O comportamento das instruções segue a documentação de engenharia reversa do Dolphin, que é a referência pública desse processador. Se o port for distribuído, é preciso considerar a licença GPL dela e da ROM livre.
- O travamento que deixava o jogo lento vinha de um `CRITICAL_SECTION` entre CPU e DSP, que não é justo. Hoje a CPU nunca espera pelo DSP: lê o controle direto e enfileira as escritas.
- A saída é estéreo: `OSGetSoundMode` devolve estéreo (HLE). No dump da gameplay, L e R diferem em ~99% das amostras.
- O DMA de áudio usa cadência fixa: cada bloco é agendado a partir do horário previsto para o anterior, não do momento em que o evento foi entregue. Antes o atraso de entrega se acumulava, o AI entregava ~29,5 mil amostras por segundo em vez de 32 mil (música ~8% lenta) e o `waveOut` tocava silêncio nas faltas (estalos). O `[rt]` mostra o total de amostras e as faltas, que devem ficar paradas durante o jogo.
