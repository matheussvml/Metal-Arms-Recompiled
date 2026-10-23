#!/usr/bin/env bash
# Monta dist/MetalArmsPC: o executável, a ROM livre do DSP e um LEIA-ME. Não inclui nada do
# jogo: o disco (.iso/.ciso) é sempre o do usuário, achado ao lado do exe ou escolhido na 1ª vez.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=dist/MetalArmsPC
rm -rf "$out"
mkdir -p "$out/data/dsp"
cp build/metalarms.exe "$out/"
cp data/dsp/dsp_rom.bin data/dsp/dsp_coef.bin "$out/data/dsp/"
cat > "$out/LEIA-ME.txt" <<'EOF'
Metal Arms: Glitch in the System - port nativo para PC (recompilação estática do GameCube)

Como jogar
- Coloque a imagem do disco americano (GM5E7D, .iso/.ciso/.gcm) nesta pasta ou numa subpasta,
  ou abra o metalarms.exe e escolha o arquivo na janela que aparece.
- Na primeira execução é criado o metalarms.ini (resolução, tela cheia, widescreen, v-sync,
  anisotropia, mouse). Os saves ficam em saves/slotA (arquivos .gci, compatíveis com o Dolphin).

Controles
- Teclado e mouse: WASD anda, mouse mira (clique na janela para capturar; a tecla abaixo do Esc
  mostra/esconde o cursor), botão esquerdo atira, botão direito = golpe de perto, Espaço pula,
  Tab volta, F = X, V = Y, Z = L, Esc pausa. Teclas configuráveis na seção [teclado] do ini.
- Controle XInput (Xbox): até 4 jogadores.
- F11: tela cheia sem bordas.

Problemas: o log fica em metalarms.log, ao lado do executável.

A ROM do DSP em data/dsp é a versão livre do projeto Dolphin (GPLv2).
EOF
ls -la "$out" "$out/data/dsp"
