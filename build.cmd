@echo off
rem Builds op20winusb32.dll / op20winusb64.dll and the optest tools with MSVC.
rem Needs Visual Studio (or Build Tools) with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - is Visual Studio installed?
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio installation with the C++ tools was found.
    exit /b 1
)
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat"

call :build x86 32 || exit /b 1
call :build x64 64 || exit /b 1
echo.
echo Build OK: out\x86\op20winusb32.dll, out\x64\op20winusb64.dll
exit /b 0

:build
setlocal
call "%VCVARS%" %1 >nul || exit /b 1
if not exist "out\%1\obj" mkdir "out\%1\obj"
set "CFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
cl %CFLAGS% /LD src\api.cpp src\device.cpp src\transport.cpp src\log.cpp ^
    /Fo"out\%1\obj\\" /Fe"out\%1\op20winusb%2.dll" ^
    /link /DEF:src\exports.def /IMPLIB:"out\%1\obj\op20winusb%2.lib" winusb.lib setupapi.lib cfgmgr32.lib advapi32.lib || exit /b 1
cl %CFLAGS% tools\optest.cpp /Fo"out\%1\obj\\" /Fe"out\%1\optest%2.exe" || exit /b 1
cl %CFLAGS% tests\selftest.cpp src\device.cpp src\transport.cpp src\log.cpp ^
    /Fo"out\%1\obj\\" /Fe"out\%1\obj\selftest%2.exe" ^
    /link winusb.lib setupapi.lib cfgmgr32.lib advapi32.lib || exit /b 1
"out\%1\obj\selftest%2.exe" || exit /b 1
endlocal
exit /b 0
