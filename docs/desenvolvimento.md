# Desenvolvimento

Guia técnico do port: como compilar, rodar a partir do terminal, opções de diagnóstico e detalhes de implementação que não cabem no README.

## Compilar

Requisitos: Windows 10+, Visual Studio Build Tools (MSVC x64), Python 3, Git Bash.

O repositório não traz nada do jogo. Antes do primeiro build, extraia o executável do seu disco para `orig/GM5E7D/sys/main.dol` (SHA-1 `b01e7602f08f26219cda00bbfcee7985155821c2`):

```bash
python tools/gc_extract.py "<disco .ciso/.iso>" extracted   # grava extracted/main.dol (e main.elf)
mkdir -p orig/GM5E7D/sys && cp extracted/main.dol orig/GM5E7D/sys/
bash tools/build/build_game.sh                              # gera o C, compila (incremental) e linka build/metalarms.exe
```

Um build completo leva cerca de 10 minutos. Ele acontece quando muda `runtime/ppc.h`, a lista de HLE (`config/GM5E7D/hle.txt`) ou o gerador. O incremental leva segundos.

O decomp-toolkit (`tools/bin/dtk.exe`, versão 1.8.4) só é necessário para refazer a análise do binário, porque `config/GM5E7D/symbols.txt` já está no repositório. Ele não é versionado e pode ser baixado em github.com/encounter/decomp-toolkit.

## Release

```bash
bash tools/build/package.sh
```

Monta `dist/MetalArmsRecompiled-<versão>-windows-x64.zip` (a versão vem do arquivo `VERSION`) com o exe, a ROM livre do DSP, um `README.txt` e os avisos de terceiros. Esse zip é o arquivo que vai nos Releases do GitHub; o Quiver Launcher instala a partir dele (filtro de nome sugerido: `windows-x64`). Ele não contém nada do disco.

## Rodar pelo terminal

```bash
./build/metalarms.exe                                                            # acha o disco sozinho
./build/metalarms.exe "<caminho>/Metal Arms - Glitch in the System (USA).ciso"   # ou indica a imagem (.ciso/.iso/.gcm)
./build/metalarms.exe -v                                                         # log detalhado
```

O exe é do subsistema GUI. Rodando de um terminal, o log sai nele. Com dois cliques, vai para `metalarms.log` ao lado do exe, e os erros aparecem numa caixa de mensagem.

Sem argumento, o port usa a chave `disco` do `metalarms.ini`. Se ela estiver vazia, procura uma imagem GM5E7D na pasta do executável, na pasta atual e nas subpastas delas; se não achar, abre uma janela para escolher o arquivo. O caminho encontrado fica gravado no ini. Caminhos relativos do ini (`saves`, `dsp_rom`) são procurados na pasta atual, na pasta do ini e uma pasta acima do exe.

## Variáveis de ambiente

As variáveis `MA_*` têm prioridade sobre o `metalarms.ini`.

| Variável | Efeito |
|---|---|
| `MA_SCALE=1..4` | resolução interna (padrão 2 = 1280×1056 de EFB) |
| `MA_WIDESCREEN=1` | imagem 16:9 (ver `docs/renderizador.md`) |
| `MA_FULLSCREEN=1` | começa em tela cheia sem bordas |
| `MA_VSYNC=0/1`, `MA_ANISO=0..16` | v-sync e filtro anisotrópico |
| `MA_DSP_ROM=<pasta>` | outra pasta com as ROMs do DSP (por exemplo, um dump do próprio console) |
| `MA_SKIP_MOVIES=1` | encurta todos os vídeos Bink para 1 quadro |
| `MA_SAVE_DIR=<pasta>` | pasta do memory card (padrão `saves/slotA`); use uma separada nos testes automáticos |
| `MA_MOUSE=0`, `MA_MOUSE_SENS`, `MA_MOUSE_SENS_Y`, `MA_MOUSE_INVERT=1` | mira com o mouse |
| `MA_KB_SEPARATE=1` | teclado e mouse sozinhos no jogador 1 |
| `MA_TECLA_<BOTÃO>=<tecla>` | teclado (o mesmo que `tecla_<botão>` no ini) |

## Diagnóstico

| Variável | Efeito |
|---|---|
| `-v` (argumento) | log detalhado: DVD, memory card, mailboxes, criação de shaders |
| `MA_SHOT_DIR=<pasta>`, `MA_SHOT_EVERY=N` | salva um PNG a cada N quadros |
| `MA_INPUT="1200:A,1400:START,3000-3200:SU,..."` | aperta botões em retraces definidos (`SU/SD/SL/SR` analógico, `CU/CD/CL/CR` C-stick) |
| `MA_GX_TRACE=<quadro>` | lista todos os desenhos e cópias do EFB daquele quadro |
| `MA_COPY_DUMP=<quadro>` | salva em `MA_SHOT_DIR` as seis primeiras cópias de EFB para textura a partir do quadro |
| `MA_PROFILE=<espera>,<duração>` | profiler por amostragem da thread principal |
| `MA_WAV_DUMP=arquivo.wav` | grava a saída de áudio |
| `MA_FIFO_DUMP=arquivo` | grava o fluxo de comandos GX |
| `MA_DSP_LOG=1` | mensagens trocadas entre CPU e DSP |
| `MA_GX_DEBUG_ORTHO=máscara`, `MA_NO_BATCH=1`, `MA_BATCH_CHECK=1` | diagnóstico do renderizador |
| `MA_LEVEL=<entrada>` | começa direto na fase pedida (tabela em [fases.md](fases.md)) |
| `MA_RAM_DUMP=<retrace>[,<retrace>...]` | grava os 24 MiB da RAM em `ram_<retrace>.bin` |
| `MA_TRACE="endereço,..."` | registra 3 floats de cada endereço a cada amostra do controle |
| `MA_AIM_TEST="início-fim:x:y,..."` | soma um valor fixo ao C-stick já processado pelo jogo |
| `MA_GPU_SYNC=1` | interpreta o FIFO na thread do jogo (sem a thread da GPU), para comparar |

Testes automáticos de fases: `bash tools/test_levels.sh <primeira> <última> <segundos>` (ver [fases.md](fases.md)).

## Mira com o mouse

O jogo transforma o C-stick em giro com curvas próprias. Medido por quadro, a horizontal gira ~0,0006 rad com o C-stick em 0,25, ~0,045 em 1 e no máximo ~0,089 (~300°/s), com uma aceleração leve; a vertical gira ~0,0127 × v². O passo de tempo do jogo varia ±10% de um quadro para o outro. Por isso, simular o C-stick com o mouse deixava a vertical lenta, a diagonal mais lenta (o analógico é limitado a um círculo) e os movimentos lentos quase parados.

`mouse_aim`, em `runtime/hle_os.c`, roda depois que o jogo processa o controle (`Pad_SampleCallback`, `0x802C0438`) e soma ao C-stick do jogador 1:

- o movimento do mouse naquele quadro, convertido pela curva inversa medida (parte direta, sem esperar medição);
- 25% do erro acumulado, medido pelo vetor global da mira em `0x80417308`, que muda no mesmo quadro do comando.

Duas regras de freio cuidam das puxadas rápidas. A velocidade máxima de giro horizontal começa em ~0,04 rad por quadro e acelera até ~0,09. O atraso acumulado é limitado a 0,06 rad (~3,4°) e o excesso é descartado; sem isso, a mira seguia girando sozinha por até ~0,4 s depois que a mão parava. Com o mouse parado, a correção nunca volta contra o último movimento, para não dar um tranco se o jogo passar um pouco do ponto.

A câmera (`0x804381C0`) repete o vetor da mira um quadro depois. Usar ela como referência, ou corrigir 100% do erro pela medição, fazia a mira passar do ponto e oscilar. Num teste andando, com dois círculos, cima/baixo e diagonais a 250 eventos por segundo, a velocidade acompanha o mouse sem oscilar, e a mira para em 1 ou 2 quadros quando o mouse para, sem voltar.

## Documentação técnica

- [mapa_do_binario.md](mapa_do_binario.md): regiões do `main.dol` e como os símbolos foram obtidos.
- [recompilador.md](recompilador.md): gerador PowerPC → C, runtime e boot.
- [renderizador.md](renderizador.md): GX → Direct3D 11.
- [audio.md](audio.md): emulação do DSP e MusyX.
- [memory_card.md](memory_card.md): saves em GCI e compatibilidade com o Dolphin.
- [desempenho.md](desempenho.md): medições e decisões de desempenho.
- [fases.md](fases.md): tabela de fases do jogo e como começar direto numa delas.
