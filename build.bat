@echo off
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set "GAMEDIR=%~2"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set CMAKE=cmake

pushd "%~dp0"
echo === BullyDX12 (Win32)
"%CMAKE%" -S . -B build -A Win32 -DBULLY_GAME_DIR="%GAMEDIR%" || goto :fail
"%CMAKE%" --build build --config %CONFIG% --parallel || goto :fail

echo === BullyRTBridge (x64)
"%CMAKE%" -S bridge -B build64 -A x64 -DBULLY_GAME_DIR="%GAMEDIR%" || goto :fail
"%CMAKE%" --build build64 --config %CONFIG% --parallel || goto :fail
popd
echo.
if defined GAMEDIR (echo Built %CONFIG% and copied to %GAMEDIR%) else (echo Built %CONFIG%. Pass the game folder as the second argument to copy it there.)
exit /b 0

:fail
popd
echo BUILD FAILED
exit /b 1
