#!/usr/bin/env bash
# Gera o C do main.dol (só regrava blocos que mudaram), compila de forma incremental
# com /Od (o otimizador do MSVC trava nas funções enormes) e linka build/metalarms.exe.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd -W 2>/dev/null || pwd)

PYTHONIOENCODING=utf-8 python -I recomp/gen.py orig/GM5E7D/sys/main.dol config/GM5E7D/symbols.txt \
    build/recomp --hle config/GM5E7D/hle.txt

cd build/recomp
# fontes cujo .obj está velho (ou o cabeçalho de protótipos mudou)
stale=()
for src in ppc_table.c ppc_funcs_*.c; do
    obj="${src%.c}.obj"
    if [[ ! -f $obj || $src -nt $obj || ppc_funcs.h -nt $obj || ../../runtime/ppc.h -nt $obj ]]; then
        stale+=("$src")
    fi
done
if ((${#stale[@]})); then
    echo "compilando ${#stale[@]} arquivo(s) gerado(s)"
    cmd.exe //c "..\\..\\tools\\build\\msvc.bat cl /nologo /c /MP16 /O2 /Ob1 /W3 /wd4102 /I..\\..\\runtime /I. ${stale[*]}" \
        2>&1 | grep -v -E "vswhere|ou externo|^ppc_" || true
fi

mkdir -p rt
RT_SRC="host.c sched.c mmio.c disc.c hle_os.c hle_dvd.c hle_card.c main.c config.c audio.c profiler.c dsp\\dsp.c gx\\gx_fifo.c gx\\gx_tex.c gx\\gx_d3d.cpp"
RT_ARGS=""
for f in $RT_SRC; do RT_ARGS="$RT_ARGS ..\\..\\runtime\\$f"; done
cmd.exe //c "..\\..\\tools\\build\\msvc.bat cl /nologo /c /O2 /Zi /W3 /MP16 /EHsc /D_CRT_SECURE_NO_WARNINGS \
    /I..\\..\\runtime /I. /Fort\\ /Fdrt\\ $RT_ARGS" \
    2>&1 | grep -v -E "vswhere|ou externo|^[a-z_0-9]+\.(c|cpp)$" || true
cmd.exe //c "..\\..\\tools\\build\\msvc.bat link /nologo /DEBUG /STACK:16777216 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:..\\metalarms.exe \
    rt\\*.obj ppc_table.obj ppc_funcs_*.obj user32.lib d3d11.lib dxgi.lib d3dcompiler.lib winmm.lib dbghelp.lib comdlg32.lib" \
    2>&1 | grep -v -E "vswhere|ou externo" || true
ls -la ../metalarms.exe
