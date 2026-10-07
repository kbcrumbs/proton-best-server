@echo off
setlocal
rem Build protonvpn-tools.exe with MSVC. Run from any prompt; finds Visual Studio via vswhere.
cd /d "%~dp0"

if defined VCToolsInstallDir goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\protonvpn-tools-vs.txt"
set /p VSPATH=<"%TEMP%\protonvpn-tools-vs.txt"
del "%TEMP%\protonvpn-tools-vs.txt" >nul 2>&1
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:build
if not exist build mkdir build

rc /nologo /fo build\app.res src\app.rc || exit /b 1

cl /nologo /W4 /O1 /Os /GL /Gy /GF /GS- /MT /utf-8 ^
   /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /Fo:build\ /Fe:build\protonvpn-tools.exe ^
   src\main.c src\servers.c src\ping.c src\net.c src\json.c src\util.c build\app.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /OPT:REF /OPT:ICF /LTCG /RELEASE || exit /b 1

for %%f in (build\protonvpn-tools.exe) do echo Built %%f  (%%~zf bytes)
exit /b 0

:novs
echo Visual Studio with the C++ build tools was not found.
exit /b 1
