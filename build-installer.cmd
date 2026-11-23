@echo off
rem Builds Op20WinUSB-Setup.msi: first the helper program the package runs for the
rem driver/certificate steps, then the MSI itself (build-msi.ps1).
rem Run build.cmd and make-driver-package.ps1 -SelfSign first.
setlocal
cd /d "%~dp0"

for %%f in (driver\op20winusb.inf driver\op20winusb.cat driver\op20winusb32.dll driver\op20winusb64.dll out\x86\optest32.exe out\x64\optest64.exe) do (
    if not exist "%%f" (
        echo Missing %%f - run build.cmd and make-driver-package.ps1 -SelfSign first.
        exit /b 1
    )
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio installation with the C++ tools was found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1

if not exist "out\installer" mkdir "out\installer"
rc /nologo /fo "out\installer\driverhelper.res" installer\driverhelper.rc || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE installer\driverhelper.cpp "out\installer\driverhelper.res" ^
    /Fo"out\installer\\" /Fe"out\installer\op20winusb-driver-setup.exe" ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "/MANIFESTUAC:level='asInvoker' uiAccess='false'" ^
    crypt32.lib setupapi.lib cfgmgr32.lib shell32.lib advapi32.lib user32.lib || exit /b 1

rem Custom-action DLL that fills in the text of the installer's final page.
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /LD installer\msica.cpp ^
    /Fo"out\installer\\" /Fe"out\installer\op20msica.dll" ^
    /link /IMPLIB:"out\installer\op20msica.lib" setupapi.lib cfgmgr32.lib advapi32.lib user32.lib || exit /b 1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-msi.ps1" || exit /b 1
exit /b 0
