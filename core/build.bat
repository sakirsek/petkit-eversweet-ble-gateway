@echo off
REM Build the core as a Windows DLL for the PC driver.
REM Requires Visual Studio 2022 Build Tools (adjust the path if yours differs).

set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo [!] vcvars64.bat not found at:
    echo     %VCVARS%
    echo     Edit this script to point at your Visual Studio installation.
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
cl /nologo /utf-8 /W4 /O2 /LD petkit_core.c /Fe:petkit_core.dll
if errorlevel 1 exit /b 1

del /q petkit_core.obj petkit_core.exp petkit_core.lib 2>nul
echo [+] core\petkit_core.dll built.
