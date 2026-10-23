# Metal Arms: Recompiled

**Metal Arms: Glitch in the System** rodando nativamente no Windows, a partir da recompilação estática do executável original do GameCube para C. Não é um emulador: o código do próprio jogo roda como um programa nativo de PC, com renderizador moderno, mouse e teclado, widescreen e resoluções maiores.

[English](README.md)

<p align="center">
  <img src="docs/images/gameplay.jpg" width="49%" alt="Gameplay">
  <img src="docs/images/title.jpg" width="49%" alt="Tela título">
</p>

> [!IMPORTANT]
> **Você precisa da sua própria cópia do jogo.** Este projeto não inclui nenhum código, dado, música ou vídeo do jogo. Na primeira execução, você indica uma imagem do disco de **Metal Arms: Glitch in the System** para **GameCube (EUA, GM5E7D)**, feita a partir de um disco seu.

## Recursos

- Executável nativo para Windows: as 42 missões da campanha e as 14 arenas do multijogador abrem e rodam (verificado por um teste automático; a campanha inteira ainda está sendo jogada)
- Renderizador em Direct3D 11 com resolução interna de até 4× (1280×1056 por padrão), filtro anisotrópico e v-sync
- Widescreen 16:9 opcional, com campo de visão maior (o HUD fica na área 4:3)
- Mira com o mouse feita para PC: o mesmo giro por movimento do mouse nos dois eixos, em qualquer velocidade
- Teclado e mouse totalmente configuráveis
- Até 4 controles XInput (Xbox e compatíveis) para tela dividida, com vibração
- Áudio original pelo DSP do GameCube emulado, rodando o driver de som MusyX do próprio jogo
- Saves de memory card em arquivos `.gci`, compatíveis com o Dolphin
- 60 FPS estáveis, com o renderizador numa thread própria

<p align="center">
  <img src="docs/images/widescreen.jpg" width="70%" alt="Widescreen">
</p>

## Requisitos

- Windows 10 ou 11 (64 bits) e placa de vídeo com Direct3D 11
- Uma imagem do disco de **Metal Arms: Glitch in the System (EUA)**, GameCube, ID **GM5E7D**, num destes formatos:
  - `.iso` / `.gcm` (imagem crua do disco)
  - `.ciso` (ISO compacta)

  Imagens RVZ, GCZ e WIA ainda não são lidas diretamente. Converta para ISO no Dolphin: clique com o botão direito no jogo → **Converter arquivo...** → **ISO**.

Outras regiões (Europa, Japão) e as versões de PlayStation 2 e Xbox não são suportadas.

## Instalação

### Quiver Launcher

Adicione **Metal Arms: Recompiled** pelo catálogo e clique em Jogar. Na primeira execução, escolha a imagem do seu disco (veja abaixo).

### Manual

1. Baixe o `MetalArmsRecompiled-<versão>-windows-x64.zip` mais recente na página [Releases](https://github.com/matheussvml/Metal-Arms-Recompiled/releases).
2. Extraia em qualquer pasta.
3. Abra o `metalarms.exe`.

### Primeira execução

O jogo procura a imagem do disco ao lado do `metalarms.exe` (e nas subpastas). Se não achar, abre uma janela para você escolher o arquivo. O caminho fica guardado no `metalarms.ini`, então isso só acontece uma vez.

## Controles

| Ação | Controle | Teclado e mouse |
|---|---|---|
| Andar | Analógico esquerdo | W A S D |
| Mirar / câmera | Analógico direito | Mouse (ou I J K L) |
| Pular / confirmar (A) | A | Espaço |
| Voltar (B) | B | Tab |
| X / Y | X / Y | F / V |
| Golpe de perto (Z) | RB | Botão direito do mouse |
| L | LT | Z |
| Atirar (R) | RT | Botão esquerdo do mouse (ou E) |
| Pausar (Start) | Start | Esc (ou Enter) |
| Direcional | Direcional | Setas |

- Clique dentro da janela para capturar o mouse. A tecla abaixo do Esc (`'` no teclado ABNT2) mostra ou esconde o cursor, e trocar de janela também solta o mouse.
- **F11** alterna a tela cheia sem bordas. A janela pode ser redimensionada à vontade.
- Todas as teclas podem ser trocadas na seção `[teclado]` do `metalarms.ini`.

## Configurações

O `metalarms.ini` é criado ao lado do executável na primeira execução:

| Chave | Padrão | Efeito |
|---|---|---|
| `escala` | `2` | Multiplicador da resolução interna, de 1 a 4 |
| `tela_cheia` | `0` | `1` = começa em tela cheia sem bordas |
| `widescreen` | `0` | `1` = 16:9 com campo de visão maior |
| `vsync` | `1` | `0` = não espera o monitor (pode rasgar a imagem) |
| `anisotropia` | `8` | Filtro anisotrópico, de `0` (como no GameCube) a `16` |
| `mouse` | `1` | `0` = desliga a mira com o mouse |
| `mouse_sensibilidade` | `4` | Sensibilidade do mouse (4 ≈ 0,14° por unidade de movimento) |
| `mouse_sensibilidade_vertical` | `1` | Multiplicador só da mira vertical |
| `mouse_inverter_y` | `0` | `1` = inverte a mira vertical |
| `teclado_separado` | `0` | `1` = teclado e mouse são o jogador 1 sozinhos; os controles viram os jogadores 2 a 4 |
| `pular_videos` | `0` | `1` = pula todos os vídeos |
| `disco` | | Caminho da imagem do disco (preenchido sozinho) |
| `saves` | `saves/slotA` | Pasta do memory card |
| `tecla_<botão>` | | Teclas, por exemplo `tecla_a=ESPACO`, `tecla_r=MOUSE_ESQ,E` (várias teclas separadas por vírgula) |

## Saves

Os saves ficam em `saves/slotA`, em arquivos `.gci`, o mesmo formato que o Dolphin usa nos memory cards em pasta GCI. Dá para copiar saves entre os dois, nos dois sentidos:

- **Do Dolphin:** configure o Memory Card do Slot A como **Pasta GCI** e copie os arquivos de `Dolphin Emulator/GC/USA/Card A/` para `saves/slotA`.
- **Para o Dolphin:** copie no sentido contrário.

## Problemas conhecidos

- Só Windows. O renderizador usa Direct3D 11.
- O giro com o mouse é limitado pela velocidade máxima de giro do próprio jogo (~300°/s). Puxadas muito rápidas giram até onde o jogo permite e param.
- A imagem do disco precisa estar em ISO, GCM ou CISO (veja Requisitos).
- O multijogador local com mais de um controle ainda não foi testado com controles de verdade.
- O jogo ainda não foi jogado do começo ao fim neste port. Cada missão foi verificada abrindo e rodando, mas podem aparecer problemas mais adiante numa missão.

Achou um problema? [Abra uma issue](https://github.com/matheussvml/Metal-Arms-Recompiled/issues) com o final do `metalarms.log` (ao lado do executável) e, se for visual, uma captura de tela.

## Compilar o código

Requisitos: Visual Studio Build Tools (MSVC x64), Python 3 e Git Bash. O executável do jogo (`main.dol`) vem do seu disco e nunca é versionado:

```bash
python tools/gc_extract.py "<seu disco .iso/.ciso>" extracted
mkdir -p orig/GM5E7D/sys && cp extracted/main.dol orig/GM5E7D/sys/
bash tools/build/build_game.sh   # -> build/metalarms.exe
bash tools/build/package.sh      # -> dist/MetalArmsRecompiled-<versão>-windows-x64.zip
```

A documentação técnica está em [docs/](docs/desenvolvimento.md): o recompilador, o renderizador, o áudio, o memory card, o desempenho e as opções de desenvolvimento.

## Como funciona

O `recomp/gen.py` traduz para C cada função PowerPC do executável do jogo. Os limites das funções vêm de uma análise feita com o decomp-toolkit, das assinaturas do SDK do Dolphin e de funções identificadas à mão. O C gerado é compilado junto com um runtime que substitui o hardware do GameCube:

- **CPU e sistema:** as threads do jogo rodam como fibers do Windows. Disco, memory card e controles são reimplementados de forma nativa (HLE).
- **Gráficos:** o fluxo de comandos do GX é interpretado numa thread própria e desenhado com Direct3D 11. O TEV vira pixel shaders gerados, e as texturas são decodificadas de todos os formatos do GameCube.
- **Áudio:** o DSP do GameCube é emulado com o microcódigo MusyX original do jogo.
- **Tempo:** as interrupções de vídeo, o DMA de áudio e o resto do hardware seguem o tempo do console.

## Créditos

- Equipe do **Dolphin Emulator**: a documentação do hardware do GameCube em que este port se apoia, a ROM livre do DSP incluída em `data/dsp` (GPLv2) e o banco de assinaturas do SDK usado para nomear funções.
- **decomp-toolkit**, de encounter: análise do executável.
- Os projetos **N64: Recompiled** e **Nebula**, que inspiraram a abordagem.
- **Swingin' Ape Studios** e **Vivendi Universal Games**, criadora e publicadora de Metal Arms: Glitch in the System.

## Licença

O código deste repositório está sob a [GNU General Public License v3.0](LICENSE). A ROM livre do DSP em `data/dsp` e o banco de assinaturas do SDK em `tools/sigs` vêm do projeto Dolphin Emulator (GPL-2.0 ou posterior). A licença não cobre o jogo em si: o código, os dados, os nomes e as marcas pertencem aos seus donos.

## Aviso legal

Este é um projeto de fã, não oficial. Não tem vínculo, aprovação ou patrocínio da Swingin' Ape Studios, Vivendi, Activision Blizzard ou Nintendo. Metal Arms: Glitch in the System e todos os nomes e materiais relacionados pertencem aos seus donos. Este repositório e seus releases não contêm dados do jogo: você precisa fornecer uma imagem de disco feita a partir de uma cópia sua, obtida legalmente. O ícone do programa (`assets/`) e as capturas em `docs/images` mostram a arte do próprio jogo, que pertence aos seus donos.
