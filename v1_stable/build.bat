@echo off
setlocal

echo ============================================================
echo  ScreenMirror - Build Script (MSVC)
echo ============================================================

REM ---- Locate vswhere to find Visual Studio ----
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)

if not exist %VSWHERE% (
    echo [ERRO] vswhere.exe nao encontrado.
    echo Instale o Visual Studio 2019 ou 2022 com suporte a C++.
    pause & exit /b 1
)

REM ---- Find latest MSVC installation ----
for /f "usebackq tokens=*" %%i in (
    `%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set VS_PATH=%%i

if "%VS_PATH%"=="" (
    echo [ERRO] Nenhuma instalacao do Visual Studio com VC++ encontrada.
    pause & exit /b 1
)

REM ---- Initialize MSVC environment (x64) ----
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
if errorlevel 1 (
    echo [ERRO] Falha ao inicializar ambiente MSVC x64.
    pause & exit /b 1
)

REM ---- Create output directory ----
if not exist bin mkdir bin

REM ---- Compile ----
echo.
echo [Compilando src\main.cpp ...]
echo.

cl.exe /nologo /std:c++17 /O2 /W3 /EHsc ^
    /D"WIN32" /D"UNICODE" /D"_UNICODE" /D"NDEBUG" ^
    /Fe"bin\ScreenMirror.exe" ^
    /Fo"bin\\" ^
    src\main.cpp ^
    /link ^
    /SUBSYSTEM:WINDOWS ^
    /ENTRY:WinMainCRTStartup ^
    d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib kernel32.lib

if errorlevel 1 (
    echo.
    echo [ERRO] Compilacao falhou. Veja as mensagens acima.
    pause & exit /b 1
)

echo.
echo [OK] Build concluido: bin\ScreenMirror.exe
echo.

REM ---- Ask to run ----
set /p RUN="Deseja executar agora? (s/n): "
if /i "%RUN%"=="s" (
    start "" "bin\ScreenMirror.exe"
)

endlocal
