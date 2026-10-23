# Renderizador (GX → Direct3D 11)

## Fluxo

1. O jogo escreve comandos no write-gather pipe (`0xCC008000`). `runtime/mmio.c` grava os bytes na RAM, no ponteiro do FIFO da CPU (PI `0x300C`/`0x3010`/`0x3014`). Os comandos só são executados quando esse FIFO é o mesmo que a GPU lê (CP `0x20`). Fora disso, o jogo está gravando uma display list.
2. `runtime/gx/gx_fifo.c` interpreta os comandos numa thread própria, a "GPU" (ver [desempenho.md](desempenho.md)):
   - registradores CP (formato de vértice), XF (matrizes, luzes, texgen) e BP (TEV, PE, texturas);
   - carregador de vértices (direto e indexado, todos os formatos de componente);
   - display lists (`0x40`);
   - sincronização (BP `0x45`/`0x47`/`0x48` → interrupções do PE).
3. `runtime/gx/gx_d3d.cpp` desenha:
   - EFB de 640×528 em 2× (1280×1056) com Z de 32 bits;
   - vertex shader único que lê a memória XF (posição/normal, 2 canais de luz, texgen e dual tex);
   - pixel shader gerado a partir do TEV (16 estágios, konst, swap tables, comparações, teste de alfa, neblina e texturas indiretas), com cache por configuração;
   - mipmaps (níveis guardados em sequência na RAM, limitados pelo `max LOD`) e sampler com LOD bias/mín./máx.;
   - texturas em `runtime/gx/gx_tex.c` (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CI4/CI8/CI14x2/CMPR, TLUT na TMEM);
   - cópias do EFB: para o XFB apresentam o quadro; para textura ficam na GPU, associadas ao endereço de destino, e passam por um shader de conversão que aplica o formato pedido (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/A8/G8/B8/RG8/GB8, intensidade YUV, quantização, meia escala e cópias de Z24).

## Decisões conferidas na tela

- Culling: o registrador usa 1 = descarta trás e 2 = descarta frente, com frente = sentido horário na tela. Com anti-horário a cena mostrava as faces internas e os elementos 2D sumiam.
- `GXSetDstAlpha` (BP `0x42`) só muda o alfa gravado no EFB, aplicado via blend factor. O blend de cor continua usando o alfa do TEV.
- Z: o clip do GX vai de -w a 0. O vertex shader soma w para chegar ao intervalo do D3D, e a faixa de profundidade vem de `zRange`/`farZ` da XF.

## Texturas indiretas

Seguem o hardware em ponto fixo, como o gerador de shaders do Dolphin:

- as coordenadas viram inteiros com 7 bits de fração (`uv × tamanho SU × 128`, BP `0x30`-`0x3F`);
- cada estágio indireto (até 4, `IREF` em BP `0x27`) lê o mapa com a coordenada reduzida por `RAS1_SS` (BP `0x25`/`0x26`) e guarda `abg × 255`;
- cada estágio TEV com `IND_CMD` (BP `0x10`-`0x1F`) aplica formato, bias, matriz estática (BP `0x06`-`0x0E`, 11 bits com sinal, deslocamento `17 - escala`) ou dinâmica (S/T), wrap e "somar à anterior", e o alpha bump vira os canais de rasterização 5 e 6.

Na primeira fase há um único desenho indireto por quadro: um efeito de distorção com matriz 0,5·identidade e bias em todos os componentes. `MA_GX_TRACE` mostra `IREF`, as escalas e o `IND_CMD` de cada estágio desses desenhos.

## Widescreen

É anamórfico. Com `widescreen=1` o EFB fica 4/3 mais largo em pixels, mas as coordenadas do GX continuam as mesmas: viewport, scissor e cópias só usam uma escala horizontal maior. Assim tudo o que é desenhado sai esticado, e a projeção de cada desenho comprime o x do clip por 3/4 para desfazer isso:

- a cena 3D (viewport de 512×480) ganha campo de visão nas laterais;
- o HUD ortográfico fica com a proporção certa, na área 4:3 do centro;
- quadros ortográficos que cobrem a largura toda (fades, filtros de tela) não são comprimidos e esticam até as bordas;
- desenhos em viewport pequeno (o mapa de sombra de 128×128) não mudam.

A decisão é tomada na CPU, por desenho (`ws_squeeze`), e marca a projeção como suja quando muda. O jogo ainda descarta objetos pelo frustum 4:3 dele, então algo pode aparecer ou sumir nas laterais.

## Cópias do EFB na primeira fase

Uma cópia por quadro, quando há sombra projetada: a silhueta dos personagens é desenhada em 128×128 no canto (512,0) do EFB e copiada como R4 (I4) para `0x015D2500`. Depois o chão a projeta. `MA_COPY_DUMP=<quadro>` (com `MA_SHOT_DIR`) salva as seis primeiras cópias a partir daquele quadro como PNG.

## Diagnóstico

```bash
MA_SHOT_DIR=build/shots MA_SHOT_EVERY=150 ./build/metalarms.exe <disco>   # PNG a cada N quadros
MA_GX_TRACE=1100 ./build/metalarms.exe <disco>    # lista desenhos e cópias do quadro 1100
MA_GX_DEBUG_ORTHO=15 ...                          # desliga blend(1)/Z(2)/culling(4)/scissor(8) nos ortográficos
MA_INPUT="1200:A,1950:START" ...                  # botões roteirizados (retrace:botão, 6 retraces cada)
```

## Ainda falta

- Z-texture, emboss (texgen tipo 1) e anisotropia.
- Escrita das cópias de EFB na RAM (o jogo só as usa como textura; a CPU nunca lê o resultado) e filtro de cópia (BP `0x53`/`0x54`).
- Logic ops do PE e quantização do EFB (RGB565/RGBA6).
