@echo off
setlocal
rem Gera dist\NiGBA-windows.zip: um NiGBA.exe único (sem DLLs, sem janela de terminal) + LEIA-ME.
rem Uso: empacotar.bat            (vcpkg em C:\vcpkg)
rem      empacotar.bat D:\vcpkg   (vcpkg em outro lugar)

set "VCPKG=%~1"
if "%VCPKG%"=="" set "VCPKG=C:\vcpkg"
cd /d "%~dp0"

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
exit /b 0

:erro
echo.
echo Algo deu errado. Veja a mensagem acima.
exit /b 1
