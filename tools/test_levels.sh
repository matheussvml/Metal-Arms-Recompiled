#!/usr/bin/env bash
# Varredura de fases: abre cada entrada da tabela (MA_LEVEL) pela campanha, anda um pouco e
# guarda capturas, avisos de recurso não emulado e se o processo caiu.
# Uso: bash tools/test_levels.sh [primeira] [última] [segundos por fase]
set -u
cd "$(dirname "$0")/.."
first=${1:-0}
last=${2:-42}
secs=${3:-80}
out=build/levels
mkdir -p "$out"
summary="$out/resumo.txt"
: > "$summary"
for i in $(seq "$first" "$last"); do
    n=$(printf '%02d' "$i")
    rm -rf build/test_saves "$out/shots_$n"
    mkdir -p "$out/shots_$n"
    MA_LEVEL=$i MA_SKIP_MOVIES=1 MA_SAVE_DIR=build/test_saves MA_SHOT_DIR="$out/shots_$n" MA_SHOT_EVERY=900 \
        MA_INPUT="300:START,450:START,600:START,750:START,900:A,1000:DOWN,1100:A,1250:A,1400:A,3300-3600:SU,3600-3700:SL,3700-3720:R,3800-4100:SU" \
        timeout "$secs" ./build/metalarms.exe > "$out/log_$n.txt" 2>&1
    code=$?
    fase=$(grep -o 'trocando por "[^"]*"' "$out/log_$n.txt" | head -1)
    rec=$(grep -o 'recurso ainda não emulado: [^(]*' "$out/log_$n.txt" | sort -u | tr '\n' ';')
    fps=$(grep '\[perf\]' "$out/log_$n.txt" | tail -3 | awk '{printf "%s ", $2}')
    shots=$(ls "$out/shots_$n" | wc -l)
    status=ok
    [ "$code" -ne 124 ] && status="SAIU($code)"
    grep -q FATAL "$out/log_$n.txt" && status="FATAL"
    echo "$n $status $fase fps=[$fps] capturas=$shots $rec" | tee -a "$summary"
done
echo FIM >> "$summary"
