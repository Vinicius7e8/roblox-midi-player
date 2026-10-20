@echo off
rem Compila o Roblox MIDI Player. Precisa do GCC (MinGW-w64, ex.: WinLibs) no PATH, versao 13 ou mais nova.
rem Builds Roblox MIDI Player. Needs GCC (MinGW-w64, e.g. WinLibs) on PATH, version 13 or newer.
rem O aviso ".rsrc merge failure: multiple non-default manifests" e normal: o manifesto do programa vence o padrao do MinGW.
rem The ".rsrc merge failure: multiple non-default manifests" warning is expected: the app's manifest wins over MinGW's default.

cd /d "%~dp0"
if not exist build mkdir build

windres src\RobloxMidiPlayer.rc -O coff -o build\res.o || goto :erro
g++ -std=c++23 -O2 -DUNICODE -D_UNICODE -mwindows -static src\RobloxMidiPlayer.cpp build\res.o ^
    -o build\RobloxMidiPlayer.exe -lcomctl32 -lwinmm -lcomdlg32 -lgdi32 || goto :erro

if not exist build\Musicas mkdir build\Musicas
copy /y Musicas\* build\Musicas\ >nul
echo Pronto: build\RobloxMidiPlayer.exe
exit /b 0

:erro
echo Falhou a compilacao.
exit /b 1
