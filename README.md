# Metal Arms: Glitch in the System — port nativo para PC

Recompilação estática do executável do GameCube (GM5E7D, EUA) para C, com um runtime nativo para Windows: gráficos em Direct3D 11, áudio pelo DSP emulado, memory card em arquivos e controle XInput/teclado. Não traz nada do jogo: tudo é lido da imagem de disco do usuário.

## Compilar

Requisitos: Windows 10+, Visual Studio Build Tools (MSVC x64), Python 3, Git Bash.

O repositório não traz nada do jogo. Antes do primeiro build, extraia o executável do seu disco para `orig/GM5E7D/sys/main.dol` (SHA-1 `b01e7602f08f26219cda00bbfcee7985155821c2`):

```bash
python tools/gc_extract.py "<disco .ciso/.iso>" extracted   # grava extracted/main.dol (e main.elf)
mkdir -p orig/GM5E7D/sys && cp extracted/main.dol orig/GM5E7D/sys/
```

Depois:

```bash
bash tools/build/build_game.sh   # gera o C, compila (incremental) e linka build/metalarms.exe
```

O decomp-toolkit (`tools/bin/dtk.exe`, versão 1.8.4) só é necessário para refazer a análise do binário (`config/GM5E7D/symbols.txt` já está no repositório). Ele não é versionado e pode ser baixado em github.com/encounter/decomp-toolkit.

`bash tools/build/package.sh` monta `dist/MetalArmsPC` (exe, ROM livre do DSP e um LEIA-ME), pronto para copiar para outra pasta ou PC. O disco continua sendo o do usuário.

Um build completo leva cerca de 10 minutos. Acontece quando muda `runtime/ppc.h`, a lista de HLE ou o gerador. O incremental leva segundos.

## Rodar

```bash
./build/metalarms.exe                                                            # acha o disco sozinho
./build/metalarms.exe "<caminho>/Metal Arms - Glitch in the System (USA).ciso"   # ou indica a imagem (.ciso/.iso/.gcm)
```

Também dá para abrir o `build/metalarms.exe` com dois cliques. O exe não abre janela de console: rodando de um terminal, o log sai nele; com dois cliques, vai para `metalarms.log` ao lado do exe, e erros aparecem numa caixa de mensagem. Caminhos relativos do ini (`saves`, `dsp_rom`) são procurados na pasta atual, na pasta do ini e uma pasta acima do exe.

Sem argumento, o port usa a chave `disco` do `metalarms.ini`. Se ela estiver vazia, procura uma imagem GM5E7D na pasta do executável, na pasta atual e nas subpastas delas; se não achar, abre uma janela para escolher o arquivo. O caminho encontrado fica gravado no ini.

Precisa de `data/dsp/dsp_rom.bin` e `data/dsp/dsp_coef.bin` (ROM livre do DSP do projeto Dolphin) para ter áudio; a pasta `data` é procurada na pasta atual, na do executável e uma acima. Os saves ficam em `saves/slotA/*.gci`. Os shaders compilados ficam em `shaders.cache`, ao lado do exe, e apagar esse arquivo só faz o jogo compilá-los de novo.

### metalarms.ini

Criado com os valores padrão ao lado do executável na primeira execução:

| Chave | Efeito |
|---|---|
| `disco=` | imagem do disco (vazio = procurar) |
| `escala=2` | resolução interna, 1 a 4 |
| `tela_cheia=0` | 1 = começa em tela cheia sem bordas |
| `vsync=1` | espera o monitor ao apresentar (0 = sem espera, pode rasgar a imagem) |
| `anisotropia=8` | filtro anisotrópico (0 = como no GameCube, até 16) |
| `teclado_separado=0` | 1 = teclado e mouse são o jogador 1 sozinhos e os controles viram os jogadores 2 a 4 |
| `mouse=1` | mira com o mouse (0 = desliga) |
| `mouse_sensibilidade=4` | ângulo por unidade de movimento do mouse (4 = ~0,14°) |
| `mouse_sensibilidade_vertical=1` | multiplica só a vertical |
| `mouse_inverter_y=0` | 1 = inverte o eixo vertical |
| `widescreen=0` | 1 = 16:9 com mais campo de visão; o HUD fica na área 4:3 do centro |
| `saves=saves/slotA` | pasta do memory card |
| `dsp_rom=data/dsp` | pasta das ROMs do DSP |
| `pular_videos=0` | 1 = vídeos com 1 quadro |

As variáveis de ambiente `MA_*` abaixo têm prioridade sobre o ini.

### Controles

| GameCube | Controle XInput | Teclado e mouse |
|---|---|---|
| Analógico | analógico esquerdo | W A S D |
| C-stick (mira) | analógico direito | mouse (ou I J K L) |
| A (pular, confirmar) | A | Espaço |
| B (voltar) | B | Tab |
| X / Y | X / Y | F / V |
| Z (golpe de perto) | RB | botão direito do mouse |
| L | LT | Z |
| R (atirar) | RT | botão esquerdo do mouse (ou E) |
| Start (pausa) | Start | Esc (ou Enter) |
| Direcional | direcional | setas |

O mouse vale depois de um clique na janela, que o captura (cursor escondido e preso). A tecla abaixo do Esc (`'` no teclado ABNT2) mostra e esconde o cursor; trocar de janela também solta. A mira com o mouse é proporcional, com o mesmo ângulo por unidade de movimento nos dois eixos e em qualquer velocidade (ver "Mira com o mouse" abaixo). As teclas são configuráveis na seção `[teclado]` do `metalarms.ini`, com várias teclas por botão separadas por vírgula (`tecla_r=MOUSE_ESQ,E`). Um nome inválido gera um aviso no log e mantém a tecla padrão.

### Mira com o mouse

O jogo transforma o C-stick em giro com curvas próprias. Medido por quadro, a horizontal gira ~0,0006 rad com o C-stick em 0,25, ~0,045 em 1 e no máximo ~0,089 (~300°/s), com uma aceleração leve; a vertical gira ~0,0127 × v². O passo de tempo do jogo varia ±10% de um quadro para o outro. Por isso, simular o C-stick com o mouse deixava a vertical lenta, a diagonal mais lenta (o analógico é limitado a um círculo) e os movimentos lentos quase parados.

`mouse_aim`, em `runtime/hle_os.c`, roda depois que o jogo processa o controle (`Pad_SampleCallback`, `0x802C0438`) e soma ao C-stick do jogador 1:

- o movimento do mouse naquele quadro, convertido pela curva inversa medida (parte direta, sem esperar medição);
- 25% do erro acumulado, medido pelo vetor global da mira em `0x80417308`, que muda no mesmo quadro do comando.

Duas regras de freio cuidam das puxadas rápidas. O jogo tem velocidade máxima de giro horizontal, que começa em ~0,04 rad por quadro e acelera até ~0,09. Antes, o que a mão pedia além disso ficava acumulado, e a mira seguia girando sozinha por até ~0,4 s depois que a mão parava. Agora o atraso acumulado é limitado a 0,06 rad (~3,4°), e o excesso é descartado. Além disso, com o mouse parado, a correção nunca volta contra o último movimento, para não dar um tranco se o jogo passar um pouco do ponto. Numa puxada de 0,3 s a 3000 unidades/s, a mira gira o máximo que o jogo permite e para junto com a mão.

A câmera (`0x804381C0`) repete esse vetor um quadro depois. Usar ela como referência, ou corrigir 100% do erro pela medição, fazia a mira passar do ponto e oscilar (velocidade alternando entre quadros), e piorava andando. Num teste andando, com dois círculos, cima/baixo e diagonais a 250 eventos por segundo, a velocidade acompanha o mouse sem oscilar, e a mira para em 1 ou 2 quadros quando o mouse para, sem voltar. `MA_AIM_TEST="início-fim:x:y,..."` soma um valor fixo ao C-stick processado, e `MA_TRACE="endereço,..."` registra vetores a cada quadro, para medir.

F11 alterna tela cheia sem bordas. A janela pode ser redimensionada, e a imagem fica sempre em 4:3 (16:9 com `widescreen=1`).

### Configuração (variáveis de ambiente)

| Variável | Efeito |
|---|---|
| `MA_SCALE=1..4` | resolução interna (padrão 2 = 1280×1056 de EFB) |
| `MA_WIDESCREEN=1` | imagem 16:9 (ver `docs/renderizador.md`) |
| `MA_FULLSCREEN=1` | começa em tela cheia sem bordas |
| `MA_VSYNC=0/1`, `MA_ANISO=0..16` | v-sync e filtro anisotrópico |
| `MA_DSP_ROM=<pasta>` | outra pasta com as ROMs do DSP (por exemplo, um dump do próprio console) |
| `MA_SKIP_MOVIES=1` | encurta todos os vídeos Bink para 1 quadro (desenvolvimento) |
| `MA_SAVE_DIR=<pasta>` | pasta do memory card (padrão `saves/slotA`); use uma separada nos testes automáticos |

### Diagnóstico

| Variável | Efeito |
|---|---|
| `-v` (argumento) | log detalhado: DVD, memory card, mailboxes, criação de shaders |
| `MA_SHOT_DIR=<pasta>`, `MA_SHOT_EVERY=N` | salva um PNG a cada N quadros |
| `MA_INPUT="1200:A,1400:START"` | aperta botões em retraces definidos (testes sem teclado) |
| `MA_GX_TRACE=<quadro>` | lista todos os desenhos e cópias do EFB daquele quadro |
| `MA_COPY_DUMP=<quadro>` | salva em `MA_SHOT_DIR` as seis primeiras cópias de EFB para textura a partir do quadro |
| `MA_PROFILE=<espera>,<duração>` | profiler por amostragem da thread principal |
| `MA_WAV_DUMP=arquivo.wav` | grava a saída de áudio |
| `MA_FIFO_DUMP=arquivo` | grava o fluxo de comandos GX |
| `MA_DSP_LOG=1` | mensagens trocadas entre CPU e DSP |
| `MA_GX_DEBUG_ORTHO=máscara`, `MA_NO_BATCH=1`, `MA_BATCH_CHECK=1` | diagnóstico do renderizador |
| `MA_LEVEL=<entrada>` | começa direto na fase pedida (tabela em [docs/fases.md](docs/fases.md)); só para testes |
| `MA_RAM_DUMP=<retrace>` | grava os 24 MiB da RAM em `ram_<retrace>.bin` |
| `MA_GPU_SYNC=1` | interpreta o FIFO na thread do jogo (sem a thread da GPU), para comparar |

## Documentação

- [docs/mapa_do_binario.md](docs/mapa_do_binario.md): regiões do `main.dol` e como os símbolos foram obtidos.
- [docs/recompilador.md](docs/recompilador.md): gerador PowerPC → C, runtime e boot.
- [docs/renderizador.md](docs/renderizador.md): GX → Direct3D 11.
- [docs/audio.md](docs/audio.md): emulação do DSP e MusyX.
- [docs/memory_card.md](docs/memory_card.md): saves em GCI.
- [docs/desempenho.md](docs/desempenho.md): medições e decisões de desempenho.
- [docs/fases.md](docs/fases.md): tabela de fases do jogo e como começar direto numa delas.

## Licenças

O comportamento do DSP segue a documentação de engenharia reversa do Dolphin, e a ROM do DSP em `data/dsp` é a versão livre do Dolphin (GPLv2). Para distribuir o port, isso precisa ser considerado.
