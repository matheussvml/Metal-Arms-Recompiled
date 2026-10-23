@echo off
rem Executa um comando com o ambiente do MSVC x64 carregado.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
