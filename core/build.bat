@echo off
REM Build the core as a Windows DLL for the Python driver.
REM Needs the MSVC compiler: Visual Studio 2019/2022, any edition, or just the
REM standalone "Build Tools for Visual Studio" with the C++ workload.
REM
REM On Linux and macOS use the Makefile in this directory instead: make -C core

setlocal enabledelayedexpansion

REM If cl is already on PATH we are inside a developer prompt; just use it.
where cl >nul 2>&1 && goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto :notfound

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :notfound

set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" goto :notfound
call "!VCVARS!" >nul 2>&1

:compile
cd /d "%~dp0"
cl /nologo /utf-8 /W4 /O2 /LD petkit_core.c /Fe:petkit_core.dll
if errorlevel 1 exit /b 1

del /q petkit_core.obj petkit_core.exp petkit_core.lib 2>nul
echo [+] core\petkit_core.dll built.
exit /b 0

:notfound
echo [!] Could not find the MSVC build environment.
echo     Install "Build Tools for Visual Studio" with the C++ workload:
echo     https://visualstudio.microsoft.com/downloads/
echo.
echo     If you have it somewhere unusual, open a developer prompt and run:
echo         cl /nologo /utf-8 /W4 /O2 /LD petkit_core.c /Fe:petkit_core.dll
exit /b 1
