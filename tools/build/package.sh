#!/usr/bin/env bash
# Monta o release: dist/MetalArmsRecompiled-<versão>-windows-x64.zip com o executável, a ROM livre
# do DSP, um README.txt e os avisos de terceiros. Os arquivos ficam na raiz do zip (o Quiver Launcher
# e quem extrai à mão encontram o metalarms.exe direto). Não inclui nada do jogo: o disco é sempre o
# do usuário, achado ao lado do exe ou escolhido na primeira execução.
set -euo pipefail
cd "$(dirname "$0")/../.."
version=$(tr -d '[:space:]' < VERSION)
name="MetalArmsRecompiled-$version-windows-x64"
stage="dist/$name"
rm -rf "$stage" "dist/$name.zip"
mkdir -p "$stage/data/dsp"
cp build/metalarms.exe "$stage/"
cp data/dsp/dsp_rom.bin data/dsp/dsp_coef.bin "$stage/data/dsp/"
cp LICENSE "$stage/LICENSE.txt"
repo="https://github.com/matheussvml/Metal-Arms-Recompiled"

cat > "$stage/README.txt" <<EOF
Metal Arms: Recompiled $version
Metal Arms: Glitch in the System (GameCube, USA) running natively on Windows.

YOU NEED YOUR OWN COPY OF THE GAME. Nothing from the game is included.

How to play
- Run metalarms.exe. It looks for a disc image (.iso, .gcm or .ciso of
  Metal Arms: Glitch in the System, USA, GM5E7D) next to the executable;
  if none is found, a window asks you to pick the file. RVZ/GCZ images must
  be converted to ISO in Dolphin first (right-click the game > Convert File).
- metalarms.ini (created on first launch): resolution, fullscreen, widescreen,
  v-sync, anisotropic filtering, mouse sensitivity and key bindings.
- Saves: saves/slotA (.gci files, compatible with Dolphin's GCI folder).
- Problems: see metalarms.log next to the executable.

Controls (keyboard and mouse; every key can be changed in metalarms.ini)
  WASD move, mouse aims (click the window to capture it; the key below Esc
  shows/hides the cursor), left click fires, right click melee, Space jumps,
  Tab back, F = X, V = Y, Z = L, Esc pauses, F11 fullscreen.
  Up to 4 XInput controllers.

Source code, updates and bug reports: $repo
License: GNU GPL v3.0 (LICENSE.txt). Third-party files: THIRD_PARTY_NOTICES.txt.

----
Metal Arms: Recompiled $version (português)

VOCÊ PRECISA DA SUA PRÓPRIA CÓPIA DO JOGO. Nada do jogo vem incluído.

Como jogar
- Abra o metalarms.exe. Ele procura a imagem do disco (.iso, .gcm ou .ciso de
  Metal Arms: Glitch in the System, EUA, GM5E7D) ao lado do executável; se não
  achar, uma janela pede o arquivo. Imagens RVZ/GCZ precisam ser convertidas
  para ISO no Dolphin antes (botão direito no jogo > Converter arquivo).
- metalarms.ini (criado na primeira execução): resolução, tela cheia,
  widescreen, v-sync, filtro anisotrópico, sensibilidade do mouse e teclas.
- Saves: saves/slotA (arquivos .gci, compatíveis com a pasta GCI do Dolphin).
- Problemas: veja o metalarms.log ao lado do executável.

Controles (teclado e mouse; todas as teclas mudam no metalarms.ini)
  WASD anda, mouse mira (clique na janela para capturar; a tecla abaixo do
  Esc mostra/esconde o cursor), clique esquerdo atira, clique direito golpe de
  perto, Espaço pula, Tab volta, F = X, V = Y, Z = L, Esc pausa, F11 tela cheia.
  Até 4 controles XInput.

Código-fonte, atualizações e relatos de problemas: $repo
Licença: GNU GPL v3.0 (LICENSE.txt). Arquivos de terceiros: THIRD_PARTY_NOTICES.txt.
EOF

cat > "$stage/THIRD_PARTY_NOTICES.txt" <<EOF
metalarms.exe
  Metal Arms: Recompiled. Licensed under the GNU General Public License v3.0
  (LICENSE.txt). Source code: $repo

data/dsp/dsp_rom.bin, data/dsp/dsp_coef.bin
  Free replacement GameCube DSP ROM from the Dolphin Emulator project.
  Copyright Dolphin Emulator Project. Licensed under the GNU General Public
  License, version 2 or later. Source: https://github.com/dolphin-emu/dolphin
  (Data/Sys/GC/dsp_rom.bin, dsp_coef.bin).

This release contains no data from Metal Arms: Glitch in the System. Metal Arms
and all related names belong to their respective owners. The program icon is
made from the game's loading screen artwork, which belongs to its owners.
EOF

python -I - "$stage" "dist/$name.zip" <<'PYEOF'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, _, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
PYEOF
ls -la "dist/$name.zip"
