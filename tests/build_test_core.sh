#!/usr/bin/env bash
# Gera só as funções usadas pelo teste (e o que elas chamam) e compila tests/test_core.exe.
set -euo pipefail
cd "$(dirname "$0")/.."

ONLY=0x800032B0,0x8000540C,0x800054F4,0x803910A0,0x80391144,0x803914F0,0x80393988,0x803939B8,\
0x803939F8,0x80393B20,0x80393B64,0x80393C1C,0x803994B4,0x8039A00C,0x8039A198,0x8039A284,\
0x8039A3BC,0x8039A4A0,0x8039A4E8,0x80339EC0,0x80339EEC,0x80339FB8,0x8033A0B0

rm -rf build/recomp_test
PYTHONIOENCODING=utf-8 python -I recomp/gen.py orig/GM5E7D/sys/main.dol config/GM5E7D/symbols.txt \
    build/recomp_test --only "$ONLY"

cd build/recomp_test
cmd.exe //c "..\\..\\tools\\build\\msvc.bat cl /nologo /O2 /W3 /wd4102 /I..\\..\\runtime /I. \
    /Fe:..\\..\\tests\\test_core.exe ..\\..\\tests\\test_core.c ..\\..\\runtime\\host.c ..\\..\\runtime\\test_stubs.c ppc_table.c ppc_funcs_*.c" \
    2>&1 | grep -v -E "vswhere|ou externo|^ppc_funcs|^ppc_table|^host\.c|^test_core\.c|^Gerando" || true
