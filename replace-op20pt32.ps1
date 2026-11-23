<#
.SYNOPSIS
    Experimental: makes programs that load the Tactrix DLL by file name
    (op20pt32.dll, e.g. EcuFlash) use Op20WinUSB instead.
    Asks for administrator rights by itself (UAC prompt).

.DESCRIPTION
    Copies the installed op20winusb32.dll over C:\Windows\SysWOW64\op20pt32.dll.
    The original is kept as op20pt32.dll.tactrix-backup and is put back by
    -Restore (and by uninstalling Op20WinUSB).

.PARAMETER Restore
    Puts the original Tactrix DLL back.
#>
param([switch]$Restore, [switch]$KeepOpen)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $relaunch = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-KeepOpen')
    if ($Restore) { $relaunch += '-Restore' }
    Write-Host 'This needs administrator rights - please confirm the Windows (UAC) prompt.'
    try {
        $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $relaunch -Wait -PassThru
    } catch {
        Write-Host 'Administrator rights were not granted. Nothing was changed.'
        exit 1
    }
    exit $process.ExitCode
}

$exitCode = 0
try {
    $sys = Join-Path $env:windir 'SysWOW64\op20pt32.dll'
    $backup = "$sys.tactrix-backup"
    if ($Restore) {
        if (-not (Test-Path $backup)) { throw "No backup found at $backup - nothing to restore." }
        Copy-Item $backup $sys -Force
        Remove-Item $backup -Force
        Write-Host "Restored the original $sys"
    } else {
        $dll = Join-Path $env:ProgramFiles 'Op20WinUSB\op20winusb32.dll'
        if (-not (Test-Path $dll)) { throw "Op20WinUSB is not installed ($dll is missing). Run Op20WinUSB-Setup.msi first." }
        if ((Test-Path $sys) -and -not (Test-Path $backup)) {
            Copy-Item $sys $backup
            Write-Host "Original saved as $backup"
        }
        Copy-Item $dll $sys -Force
        Write-Host "Op20WinUSB now stands in for $sys"
    }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
if ($KeepOpen) { Read-Host 'Press Enter to close this window' | Out-Null }
exit $exitCode
