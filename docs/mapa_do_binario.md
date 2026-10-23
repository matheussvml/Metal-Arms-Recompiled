# Mapa do main.dol (GM5E7D, Metal Arms USA)

SHA1 `b01e7602f08f26219cda00bbfcee7985155821c2`, entry `0x80003154`, compilador Metrowerks CodeWarrior.

## Regiões de `.text`

| Faixa | Conteúdo |
|---|---|
| `0x80003100`–`0x80005600` | `.init`: `__start`, runtime CW, `memcpy`/`memset`, vetores do TRK |
| `0x80005BE0`–`0x8032226C` | Jogo (motor Fang da Swingin' Ape + SmallAMX), cerca de 10.500 funções, sem nomes |
| `0x8032226C`–`0x80366F30` | SDK Nintendo (DB, AR/ARQ, CARD, DSP, DVD, EXI, GX, MTX, OS, PAD, SI, AI, VI) + MusyX |
| `0x80366F30`–`0x80367980` | Ponte de plataforma MusyX/Bink (IRQ, helpers) |
| `0x80367980`–`~0x80385800` | Bink Video (RAD), com saída de áudio via streams do MusyX |
| `~0x80385800`–fim | MetroTRK, MSL C/C++ (stdio, math, exceções), `__save_gpr`/`__rest_gpr` |

## Pontos de entrada do motor que interessam ao port (código do jogo, nomes provisórios)

| Endereço | Papel inferido | Evidência |
|---|---|---|
| `0x80218038` | `main` | dtk |
| `0x802C129C` | abrir arquivo do disco | chama `DVDConvertPathToEntrynum` + `DVDFastOpen` |
| `0x802C1424` | fechar arquivo | chama `DVDClose` |
| `0x802C1678`, `0x802C1914` | leitura assíncrona | chamam `DVDReadAsyncPrio` |
| `0x802D0FC8` | inicialização de áudio do motor | único chamador de `sndInit` |
| `0x802D2090` | carregamento de grupo de som | chama `sndPushGroup` |
| `0x80309E48`–`0x8030A9D8` | player de vídeo do jogo (wrapper do Bink) | únicos chamadores do bloco Bink |

## Observações de arquitetura

- O jogo usa semáforos do OS (`OSInitSemaphore`/`OSWaitSemaphore`/`OSSignalSemaphore`), não filas de mensagem.
- Os macros GX do CodeWarrior (`GXPosition*`, `GXColor*`, ...) ficam inline e escrevem direto no write-gather pipe
  (`0xCC008000`). O runtime precisa interpretar o fluxo de comandos GX, não só substituir funções `GX*`.
- O áudio é MusyX (`snd*`), o que permite HLE no nível da API MusyX em vez de emular o DSP.
- Os vídeos (`files/Movies/*.bik`, 39 arquivos) são Bink 1 e podem ser tocados por um decodificador nativo.

## Como os nomes foram obtidos

1. `dtk dol split` (assinaturas embutidas): 304 nomes.
2. `tools/apply_dsy.py` com o `totaldb.dsy` do Dolphin (hash de opcodes + tamanho, nome único, fora da faixa do jogo): +694.
3. `config/GM5E7D/manual_symbols.tsv` (ordem de link do SDK, chamadas e instruções), aplicado com `tools/apply_manual.py`: +30.
