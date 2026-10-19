@echo off
setlocal
rem Gera dist\NiGBA-windows.zip: um NiGBA.exe único (sem DLLs, sem janela de terminal) + LEIA-ME.
rem Uso: empacotar.bat            (vcpkg em C:\vcpkg)
rem      empacotar.bat D:\vcpkg   (vcpkg em outro lugar)

set "VCPKG=%~1"
if "%VCPKG%"=="" set "VCPKG=C:\vcpkg"
cd /d "%~dp0"

rem Fora do terminal do Visual Studio o cmake e o compilador nao estao no PATH: carrega o ambiente dele.
where cmake >nul 2>nul || call :ambienteVS || goto :erro

echo === 1/4 SDL2 estatico (so na primeira vez demora) ===
"%VCPKG%\vcpkg.exe" install sdl2:x64-windows-static || goto :erro

echo === 2/4 Configurando ===
cmake -B build-dist -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static -DNIGBA_STATIC=ON -DNIGBA_CONSOLE=OFF || goto :erro

echo === 3/4 Compilando ===
cmake --build build-dist --config Release --target NiGBA || goto :erro

echo === 4/4 Montando o pacote ===
if exist dist rmdir /s /q dist
mkdir dist\NiGBA
copy /y build-dist\Release\NiGBA.exe dist\NiGBA\ >nul || goto :erro
copy /y LEIA-ME.txt dist\NiGBA\ >nul || goto :erro
powershell -NoProfile -Command "Compress-Archive -Path 'dist\NiGBA' -DestinationPath 'dist\NiGBA-windows.zip' -Force" || goto :erro

echo.
echo Pronto! Pacote em: %cd%\dist\NiGBA-windows.zip
call :esperar
exit /b 0

:erro
echo.
echo Algo deu errado. Veja a mensagem acima.
call :esperar
exit /b 1

rem Acha o Visual Studio (com as ferramentas de C++) e carrega o ambiente de compilação de 64 bits.
:ambienteVS
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Nao encontrei o Visual Studio. Rode este script pelo "x64 Native Tools Command Prompt".
    exit /b 1
)
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo O Visual Studio nao tem as ferramentas de C++ instaladas.
    exit /b 1
)
echo === Carregando o ambiente do Visual Studio ===
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where cmake >nul 2>nul || (
    echo O CMake nao veio com o Visual Studio. Instale o componente "Ferramentas do CMake para Windows".
    exit /b 1
)
exit /b 0

rem Aberto com dois cliques, a janela fecharia na hora: espera uma tecla para dar tempo de ler.
rem No GitHub Actions (variável CI definida) não há ninguém para apertar a tecla: não espera.
:esperar
if defined CI exit /b 0
echo %cmdcmdline% | find /i "/c" >nul && pause
exit /b 0
