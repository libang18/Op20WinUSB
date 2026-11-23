<#
.SYNOPSIS
    Builds Op20WinUSB-Setup.msi with the Windows Installer automation interface
    and makecab - both ship with Windows, so no extra tooling is needed.

.DESCRIPTION
    The package is 64-bit and per-machine. Windows Installer itself copies the
    files to %ProgramFiles%\Op20WinUSB and writes the J2534 registry entries
    (32-bit and 64-bit views), so repair and uninstall work the standard way.
    Two custom actions run op20winusb-driver-setup.exe for the parts an MSI cannot
    do natively: trusting the driver package certificate, adding the INF to the
    driver store and removing the blocked Tactrix kernel driver.

    Inputs: .\driver (signed driver package), .\out (tools),
            .\out\installer\op20winusb-driver-setup.exe (built by build-installer.cmd).
#>
param([string]$Version = '1.0.0')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$work = Join-Path $root 'out\installer\msi'
$msiPath = Join-Path $root 'Op20WinUSB-Setup.msi'

$productName = 'Op20WinUSB - Tactrix Openport 2.0 J2534 driver'
$manufacturer = 'Op20WinUSB'
$upgradeCode = '{1D94BD47-A836-417B-835B-AE1376B4815C}'   # never changes
$productCode = '{' + [guid]::NewGuid().ToString().ToUpper() + '}'   # new per build: every build is a major upgrade
$packageCode = '{' + [guid]::NewGuid().ToString().ToUpper() + '}'
$j2534Key = 'SOFTWARE\PassThruSupport.04.04\Op20WinUSB - Openport 2.0'
$j2534Name = 'Op20WinUSB - Openport 2.0 (K-line, CAN)'

# File key, source, "8.3 name|long name", component, component GUID. Order = cabinet order.
$files = @(
    @{ Key = 'InfFile';      Src = 'driver\op20winusb.inf';   Name = 'OP20WUSB.INF|op20winusb.inf';     Comp = 'C_Inf';      Guid = '{B052B9E8-61F4-4B71-ADA5-BDFD5BB2AF2B}' }
    @{ Key = 'CatFile';      Src = 'driver\op20winusb.cat';   Name = 'OP20WUSB.CAT|op20winusb.cat';     Comp = 'C_Cat';      Guid = '{17947B51-13D2-42A8-8A53-A0602F8A0BD3}' }
    @{ Key = 'Dll32File';    Src = 'driver\op20winusb32.dll'; Name = 'OP20WU32.DLL|op20winusb32.dll';   Comp = 'C_Dll32';    Guid = '{A4D0B5DD-01E8-4BA1-8F40-C91ED4011494}' }
    @{ Key = 'Dll64File';    Src = 'driver\op20winusb64.dll'; Name = 'OP20WU64.DLL|op20winusb64.dll';   Comp = 'C_Dll64';    Guid = '{40761076-9DE3-48B9-A3FF-E5E713705BCB}' }
    @{ Key = 'Optest32File'; Src = 'out\x86\optest32.exe';    Name = 'optest32.exe';                    Comp = 'C_Optest32'; Guid = '{BCB46A72-272C-48FF-857D-26235C6D7C64}' }
    @{ Key = 'Optest64File'; Src = 'out\x64\optest64.exe';    Name = 'optest64.exe';                    Comp = 'C_Optest64'; Guid = '{9F5B04B4-4A74-41B1-B272-1E371DEEB8A8}' }
    @{ Key = 'HelperFile';   Src = 'out\installer\op20winusb-driver-setup.exe'; Name = 'OP20DSET.EXE|op20winusb-driver-setup.exe'; Comp = 'C_Helper'; Guid = '{8687E767-3C20-4CBE-9C57-4A70E0CD077F}' }
)
foreach ($f in $files) {
    $f.Path = Join-Path $root $f.Src
    if (-not (Test-Path $f.Path)) { throw "Missing $($f.Src) - run build.cmd, make-driver-package.ps1 -SelfSign and build-installer.cmd." }
    $f.Size = (Get-Item $f.Path).Length
}

# The catalog must still match the files it is shipped with.
$st = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if ($st) {
    foreach ($name in 'op20winusb.inf', 'op20winusb32.dll', 'op20winusb64.dll') {
        $out = & $st.FullName verify /pa /c (Join-Path $root 'driver\op20winusb.cat') (Join-Path $root "driver\$name") 2>&1 | Out-String
        if ($out -match 'not found in the specified catalog') { throw "driver\$name does not match the catalog - re-run make-driver-package.ps1 -SelfSign." }
    }
}

# ---- cabinet ----------------------------------------------------------------
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$ddf = @(
    '.OPTION EXPLICIT'
    '.Set CabinetNameTemplate=op20.cab'
    ".Set DiskDirectoryTemplate=`"$work`""
    '.Set MaxDiskSize=0'
    '.Set Cabinet=on'
    '.Set Compress=on'
    '.Set CompressionType=MSZIP'
    '.Set UniqueFiles=on'
    '.Set RptFileName=nul'
    '.Set InfFileName=nul'
)
foreach ($f in $files) { $ddf += "`"$($f.Path)`" $($f.Key)" }
$ddfPath = Join-Path $work 'op20.ddf'
Set-Content -Path $ddfPath -Value $ddf -Encoding ascii
& makecab.exe /F $ddfPath | Out-Null
$cabPath = Join-Path $work 'op20.cab'
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $cabPath)) { throw "makecab failed ($LASTEXITCODE)" }

# ---- database ---------------------------------------------------------------
Remove-Item $msiPath -Force -ErrorAction SilentlyContinue
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase($msiPath, 3)   # 3 = create

function Invoke-Sql([string]$sql) {
    $view = $db.OpenView($sql)
    $view.Execute()
    $view.Close()
}

# Inserts one row; $null stays NULL, [int] goes in as an integer, the rest as text.
function Add-Row([string]$table, [string[]]$columns, [object[]]$values) {
    $cols = ($columns | ForEach-Object { '`' + $_ + '`' }) -join ', '
    $marks = ($columns | ForEach-Object { '?' }) -join ', '
    $view = $db.OpenView('INSERT INTO `' + $table + '` (' + $cols + ') VALUES (' + $marks + ')')
    $rec = $installer.CreateRecord($columns.Count)
    for ($i = 0; $i -lt $columns.Count; $i++) {
        $v = $values[$i]
        if ($null -eq $v) { continue }
        if ($v -is [int]) { $rec.IntegerData($i + 1) = $v } else { $rec.StringData($i + 1) = [string]$v }
    }
    $view.Execute($rec)
    $view.Close()
}

Invoke-Sql 'CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)'
Invoke-Sql 'CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)'
Invoke-Sql 'CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `ComponentId` CHAR(38), `Directory_` CHAR(72) NOT NULL, `Attributes` SHORT NOT NULL, `Condition` CHAR(255), `KeyPath` CHAR(72) PRIMARY KEY `Component`)'
Invoke-Sql 'CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), `Title` CHAR(64) LOCALIZABLE, `Description` CHAR(255) LOCALIZABLE, `Display` SHORT, `Level` SHORT NOT NULL, `Directory_` CHAR(72), `Attributes` SHORT NOT NULL PRIMARY KEY `Feature`)'
Invoke-Sql 'CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)'
Invoke-Sql 'CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Version` CHAR(72), `Language` CHAR(20), `Attributes` SHORT, `Sequence` LONG NOT NULL PRIMARY KEY `File`)'
Invoke-Sql 'CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` LONG NOT NULL, `DiskPrompt` CHAR(64) LOCALIZABLE, `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), `Source` CHAR(72) PRIMARY KEY `DiskId`)'
Invoke-Sql 'CREATE TABLE `Registry` (`Registry` CHAR(72) NOT NULL, `Root` SHORT NOT NULL, `Key` CHAR(255) NOT NULL LOCALIZABLE, `Name` CHAR(255) LOCALIZABLE, `Value` LONGCHAR LOCALIZABLE, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Registry`)'
Invoke-Sql 'CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, `Source` CHAR(72), `Target` CHAR(255), `ExtendedType` LONG PRIMARY KEY `Action`)'
Invoke-Sql 'CREATE TABLE `Binary` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)'
Invoke-Sql 'CREATE TABLE `Upgrade`(`UpgradeCode` CHAR(38) NOT NULL, `VersionMin` CHAR(20), `VersionMax` CHAR(20), `Language` CHAR(255), `Attributes` LONG NOT NULL, `Remove` CHAR(255), `ActionProperty` CHAR(72) NOT NULL PRIMARY KEY `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, `Attributes`)'
foreach ($seq in 'InstallExecuteSequence', 'InstallUISequence', 'AdminExecuteSequence', 'AdminUISequence') {
    Invoke-Sql ('CREATE TABLE `' + $seq + '` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)')
}

# Properties
$props = [ordered]@{
    ProductCode            = $productCode
    ProductName            = $productName
    ProductVersion         = $Version
    ProductLanguage        = '1033'
    Manufacturer           = $manufacturer
    UpgradeCode            = $upgradeCode
    ALLUSERS               = '1'              # per-machine: Windows asks for administrator rights
    ARPNOMODIFY            = '1'
    ARPCOMMENTS            = 'User-mode J2534 driver (WinUSB) for the Tactrix Openport 2.0 with K-line support'
    REINSTALLMODE          = 'amus'           # always replace files left by an earlier copy
    SecureCustomProperties = 'OLDPRODUCTS'
    DefaultUIFont          = 'DlgFont'
    # Shown on the result page if the custom action could not fill them in.
    OP20_RESULT_TITLE      = 'Setup finished'
    OP20_RESULT_TEXT       = 'The requested changes were made.'
}
foreach ($p in $props.GetEnumerator()) { Add-Row 'Property' @('Property', 'Value') @($p.Key, $p.Value) }

# Directories: %ProgramFiles%\Op20WinUSB (64-bit Program Files)
Add-Row 'Directory' @('Directory', 'Directory_Parent', 'DefaultDir') @('TARGETDIR', $null, 'SourceDir')
Add-Row 'Directory' @('Directory', 'Directory_Parent', 'DefaultDir') @('ProgramFiles64Folder', 'TARGETDIR', '.')
Add-Row 'Directory' @('Directory', 'Directory_Parent', 'DefaultDir') @('INSTALLFOLDER', 'ProgramFiles64Folder', 'OP20WUSB|Op20WinUSB')

Add-Row 'Feature' @('Feature', 'Feature_Parent', 'Title', 'Description', 'Display', 'Level', 'Directory_', 'Attributes') `
    @('Main', $null, 'Op20WinUSB', 'J2534 driver for the Tactrix Openport 2.0', 1, 1, 'INSTALLFOLDER', 0)

# Files: one component per file, 64-bit component (attribute 256), key path = the file.
$sequence = 0
foreach ($f in $files) {
    $sequence++
    Add-Row 'Component' @('Component', 'ComponentId', 'Directory_', 'Attributes', 'Condition', 'KeyPath') @($f.Comp, $f.Guid, 'INSTALLFOLDER', 256, $null, $f.Key)
    Add-Row 'FeatureComponents' @('Feature_', 'Component_') @('Main', $f.Comp)
    Add-Row 'File' @('File', 'Component_', 'FileName', 'FileSize', 'Version', 'Language', 'Attributes', 'Sequence') @($f.Key, $f.Comp, $f.Name, [int]$f.Size, $null, $null, 512, $sequence)
}
Add-Row 'Media' @('DiskId', 'LastSequence', 'DiskPrompt', 'Cabinet', 'VolumeLabel', 'Source') @(1, $sequence, $null, '#op20.cab', $null, $null)

# J2534 registration. A component without the 64-bit attribute writes to the
# 32-bit registry view (WOW6432Node), one with it to the 64-bit view.
$regComponents = @(
    @{ Comp = 'C_Reg32'; Guid = '{16CC4972-1E80-49E3-A1A8-9E6B15897178}'; Attr = 4;   Dll = '[INSTALLFOLDER]op20winusb32.dll'; Tag = 'R32' }
    @{ Comp = 'C_Reg64'; Guid = '{8BCDA384-DC60-41F0-8152-E0839E0CF113}'; Attr = 260; Dll = '[INSTALLFOLDER]op20winusb64.dll'; Tag = 'R64' }
)
foreach ($rc in $regComponents) {
    $t = $rc.Tag
    Add-Row 'Component' @('Component', 'ComponentId', 'Directory_', 'Attributes', 'Condition', 'KeyPath') @($rc.Comp, $rc.Guid, 'INSTALLFOLDER', $rc.Attr, $null, "${t}_FunctionLibrary")
    Add-Row 'FeatureComponents' @('Feature_', 'Component_') @('Main', $rc.Comp)
    $values = [ordered]@{ Name = $j2534Name; Vendor = $manufacturer; FunctionLibrary = $rc.Dll; ConfigApplication = $null }
    foreach ($p in 'CAN', 'ISO15765', 'ISO9141', 'ISO9141_CH1', 'ISO9141_CH2', 'ISO9141_CH3', 'ISO14230', 'ISO14230_CH1', 'ISO14230_CH2') { $values[$p] = '#1' }
    foreach ($p in 'J1850VPW', 'J1850PWM', 'SCI_A_ENGINE', 'SCI_A_TRANS', 'SCI_B_ENGINE', 'SCI_B_TRANS') { $values[$p] = '#0' }
    foreach ($v in $values.GetEnumerator()) {
        Add-Row 'Registry' @('Registry', 'Root', 'Key', 'Name', 'Value', 'Component_') @("${t}_$($v.Key)", 2, $j2534Key, $v.Key, $v.Value, $rc.Comp)
    }
}

# Custom actions: the installed helper, run elevated in the install script.
#   18 = exe installed with the product, 1024 = deferred, 2048 = run as SYSTEM, 64 = ignore failure
# A failed driver step does not abort the install (which would only produce a generic
# error box); the helper records the outcome and the result page reports it.
Add-Row 'CustomAction' @('Action', 'Type', 'Source', 'Target', 'ExtendedType') @('DriverInstall', 3154, 'HelperFile', '/driver-install', $null)
Add-Row 'CustomAction' @('Action', 'Type', 'Source', 'Target', 'ExtendedType') @('DriverUninstall', 3154, 'HelperFile', '/driver-uninstall', $null)

# Text of the final page: a DLL kept inside the package (Binary table) looks at
# what actually happened and sets OP20_RESULT_TITLE / OP20_RESULT_TEXT.
#   1 = DLL stored in the Binary table, 64 = ignore failure
$caDll = Join-Path $root 'out\installer\op20msica.dll'
if (-not (Test-Path $caDll)) { throw 'Missing out\installer\op20msica.dll - run build-installer.cmd.' }
$view = $db.OpenView('INSERT INTO `Binary` (`Name`, `Data`) VALUES (?, ?)')
$rec = $installer.CreateRecord(2)
$rec.StringData(1) = 'CaDll'
$rec.SetStream(2, $caDll)
$view.Execute($rec)
$view.Close()
Add-Row 'CustomAction' @('Action', 'Type', 'Source', 'Target', 'ExtendedType') @('SetResultText', 65, 'CaDll', 'SetResultText', $null)

# ---- user interface: one wizard-style window ---------------------------------
# Welcome (or Maintenance) -> Progress -> Result, all the same size and position,
# so it reads as a single window whose content changes. No message boxes.
Invoke-Sql 'CREATE TABLE `Dialog` (`Dialog` CHAR(72) NOT NULL, `HCentering` SHORT NOT NULL, `VCentering` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, `Title` CHAR(128) LOCALIZABLE, `Control_First` CHAR(50) NOT NULL, `Control_Default` CHAR(50), `Control_Cancel` CHAR(50) PRIMARY KEY `Dialog`)'
Invoke-Sql 'CREATE TABLE `Control` (`Dialog_` CHAR(72) NOT NULL, `Control` CHAR(50) NOT NULL, `Type` CHAR(20) NOT NULL, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, `Property` CHAR(50), `Text` LONGCHAR LOCALIZABLE, `Control_Next` CHAR(50), `Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Dialog_`, `Control`)'
Invoke-Sql 'CREATE TABLE `ControlEvent` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, `Argument` CHAR(255) NOT NULL, `Condition` CHAR(255), `Ordering` SHORT PRIMARY KEY `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`)'
Invoke-Sql 'CREATE TABLE `EventMapping` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, `Attribute` CHAR(50) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Event`)'
Invoke-Sql 'CREATE TABLE `TextStyle` (`TextStyle` CHAR(72) NOT NULL, `FaceName` CHAR(32) NOT NULL, `Size` SHORT NOT NULL, `Color` LONG, `StyleBits` SHORT PRIMARY KEY `TextStyle`)'
Invoke-Sql 'CREATE TABLE `ActionText` (`Action` CHAR(72) NOT NULL, `Description` LONGCHAR LOCALIZABLE, `Template` LONGCHAR LOCALIZABLE PRIMARY KEY `Action`)'

Add-Row 'TextStyle' @('TextStyle', 'FaceName', 'Size', 'Color', 'StyleBits') @('DlgFont', 'Segoe UI', 9, $null, 0)
Add-Row 'TextStyle' @('TextStyle', 'FaceName', 'Size', 'Color', 'StyleBits') @('DlgTitle', 'Segoe UI', 13, $null, 1)

$dialogCols = @('Dialog', 'HCentering', 'VCentering', 'Width', 'Height', 'Attributes', 'Title', 'Control_First', 'Control_Default', 'Control_Cancel')
$controlCols = @('Dialog_', 'Control', 'Type', 'X', 'Y', 'Width', 'Height', 'Attributes', 'Property', 'Text', 'Control_Next', 'Help')
$eventCols = @('Dialog_', 'Control_', 'Event', 'Argument', 'Condition', 'Ordering')
$caption = 'Op20WinUSB Setup'
$W = 370; $H = 270
$textAttr = 196611     # visible, enabled, transparent, no prefix
$buttonAttr = 3        # visible, enabled
$shieldAttr = 8388611  # ... plus the UAC shield icon
$modal = 7             # visible, modal, minimize box
$modeless = 5          # visible, minimize box

function Add-Dialog($name, $attributes, $first, $default, $cancel) {
    Add-Row 'Dialog' $dialogCols @($name, 50, 50, $W, $H, $attributes, $caption, $first, $default, $cancel)
    Add-Row 'Control' $controlCols @($name, 'BottomLine', 'Line', 0, 234, $W, 0, 1, $null, $null, $null, $null)
}
function Add-Text($dialog, $name, $y, $height, $text) {
    Add-Row 'Control' $controlCols @($dialog, $name, 'Text', 20, $y, 330, $height, $textAttr, $null, $text, $null, $null)
}
function Add-Button($dialog, $name, $x, $text, $next, $attributes = $buttonAttr) {
    Add-Row 'Control' $controlCols @($dialog, $name, 'PushButton', $x, 243, 56, 17, $attributes, $null, $text, $next, $null)
}
function Add-Event($dialog, $control, $eventName, $argument, $order = 1) {
    Add-Row 'ControlEvent' $eventCols @($dialog, $control, $eventName, $argument, '1', $order)
}
$nl = "`r`n"

# Welcome: what is going to happen, [Install] [Cancel]
Add-Dialog 'WelcomeDlg' $modal 'Install' 'Install' 'Cancel'
Add-Text 'WelcomeDlg' 'Title' 15 22 '{\DlgTitle}Op20WinUSB [ProductVersion]'
Add-Text 'WelcomeDlg' 'Body' 45 180 (
    "J2534 driver for the Tactrix Openport 2.0 with K-line support (ISO9141, ISO14230, CAN, ISO15765).$nl$nl" +
    "Setup will:$nl" +
    "   -  install a WinUSB driver package for the cable; its self-signed certificate is added to this PC's trusted certificates$nl" +
    "   -  remove the original Tactrix kernel driver, which Windows 11 blocks$nl" +
    "   -  copy the J2534 libraries to Program Files and register them for 32-bit and 64-bit programs$nl$nl" +
    "Nothing is written to the cable itself.$nl$nl" +
    "Click Install to continue. Windows will ask for administrator rights.")
Add-Button 'WelcomeDlg' 'Install' 236 '&Install' 'Cancel' $shieldAttr
Add-Button 'WelcomeDlg' 'Cancel' 300 'Cancel' 'Install'
Add-Event 'WelcomeDlg' 'Install' 'EndDialog' 'Return'
Add-Event 'WelcomeDlg' 'Cancel' 'EndDialog' 'Exit'

# Maintenance: shown when this exact version is already installed
Add-Dialog 'MaintenanceDlg' $modal 'Repair' 'Repair' 'Cancel'
Add-Text 'MaintenanceDlg' 'Title' 15 22 '{\DlgTitle}Op20WinUSB [ProductVersion] is installed'
Add-Text 'MaintenanceDlg' 'Body' 45 180 (
    "Repair installs the files, the J2534 registration and the WinUSB driver package again.$nl$nl" +
    "Remove uninstalls Op20WinUSB: the J2534 registration, the WinUSB driver package, its certificate and the files. " +
    "Other drivers for the cable (OPTA, Tactrix) are left untouched.")
Add-Button 'MaintenanceDlg' 'Repair' 172 '&Repair' 'Remove' $shieldAttr
Add-Button 'MaintenanceDlg' 'Remove' 236 'Re&move' 'Cancel' $shieldAttr
Add-Button 'MaintenanceDlg' 'Cancel' 300 'Cancel' 'Repair'
Add-Event 'MaintenanceDlg' 'Repair' 'ReinstallMode' 'amus' 1
Add-Event 'MaintenanceDlg' 'Repair' 'Reinstall' 'ALL' 2
Add-Event 'MaintenanceDlg' 'Repair' 'EndDialog' 'Return' 3
Add-Event 'MaintenanceDlg' 'Remove' 'Remove' 'ALL' 1
Add-Event 'MaintenanceDlg' 'Remove' 'EndDialog' 'Return' 2
Add-Event 'MaintenanceDlg' 'Cancel' 'EndDialog' 'Exit'

# Progress: stays up while the changes are made
Add-Dialog 'ProgressDlg' $modeless 'Cancel' 'Cancel' 'Cancel'
Add-Text 'ProgressDlg' 'Title' 15 22 '{\DlgTitle}Working...'
Add-Text 'ProgressDlg' 'Body' 45 30 'Please wait while Setup makes the changes. This can take a moment.'
Add-Text 'ProgressDlg' 'ActionText' 100 15 $null
Add-Row 'Control' $controlCols @('ProgressDlg', 'ProgressBar', 'ProgressBar', 20, 118, 330, 12, 65537, $null, 'Progress', $null, $null)
Add-Button 'ProgressDlg' 'Cancel' 300 'Cancel' $null
Add-Event 'ProgressDlg' 'Cancel' 'EndDialog' 'Exit'
Add-Row 'EventMapping' @('Dialog_', 'Control_', 'Event', 'Attribute') @('ProgressDlg', 'ActionText', 'ActionText', 'Text')
Add-Row 'EventMapping' @('Dialog_', 'Control_', 'Event', 'Attribute') @('ProgressDlg', 'ProgressBar', 'SetProgress', 'Progress')

# Result pages: success (text filled in by SetResultText), failure, cancelled
Add-Dialog 'ExitDlg' $modal 'Exit' 'Exit' 'Exit'
Add-Text 'ExitDlg' 'Title' 15 22 '{\DlgTitle}[OP20_RESULT_TITLE]'
Add-Text 'ExitDlg' 'Body' 45 180 '[OP20_RESULT_TEXT]'
Add-Button 'ExitDlg' 'Exit' 300 'E&xit' $null
Add-Event 'ExitDlg' 'Exit' 'EndDialog' 'Return'

Add-Dialog 'FatalErrorDlg' $modal 'Exit' 'Exit' 'Exit'
Add-Text 'FatalErrorDlg' 'Title' 15 22 '{\DlgTitle}Setup did not complete'
Add-Text 'FatalErrorDlg' 'Body' 45 180 (
    "Op20WinUSB was not installed. Setup ended before the changes could be completed and anything already done was rolled back.$nl$nl" +
    "You can run the installer again. To get a detailed log, start it from a command prompt:$nl$nl" +
    "msiexec /i Op20WinUSB-Setup.msi /l*v install.log")
Add-Button 'FatalErrorDlg' 'Exit' 300 'E&xit' $null
Add-Event 'FatalErrorDlg' 'Exit' 'EndDialog' 'Exit'

Add-Dialog 'UserExitDlg' $modal 'Exit' 'Exit' 'Exit'
Add-Text 'UserExitDlg' 'Title' 15 22 '{\DlgTitle}Setup was cancelled'
Add-Text 'UserExitDlg' 'Body' 45 180 'Nothing was changed on this computer.'
Add-Button 'UserExitDlg' 'Exit' 300 'E&xit' $null
Add-Event 'UserExitDlg' 'Exit' 'EndDialog' 'Exit'

# What the progress page says for the main steps
$actionText = [ordered]@{
    InstallFiles         = 'Copying files'
    DriverInstall        = 'Installing the WinUSB driver package for the cable'
    WriteRegistryValues  = 'Registering the J2534 libraries'
    RegisterProduct      = 'Registering the product'
    PublishProduct       = 'Finishing'
    DriverUninstall      = 'Removing the WinUSB driver package'
    RemoveRegistryValues = 'Removing the J2534 registration'
    RemoveFiles          = 'Removing files'
    RemoveExistingProducts = 'Removing the previous version'
    InstallValidate      = 'Preparing'
}
foreach ($a in $actionText.GetEnumerator()) { Add-Row 'ActionText' @('Action', 'Description', 'Template') @($a.Key, $a.Value, $null) }

# A newer build removes the previous one first (major upgrade), whatever its version.
Add-Row 'Upgrade' @('UpgradeCode', 'VersionMin', 'VersionMax', 'Language', 'Attributes', 'Remove', 'ActionProperty') @($upgradeCode, '0.0.0', $null, $null, 256, $null, 'OLDPRODUCTS')

$execute = @(
    @('FindRelatedProducts', $null, 25), @('ValidateProductID', $null, 700), @('CostInitialize', $null, 800),
    @('FileCost', $null, 900), @('CostFinalize', $null, 1000), @('InstallValidate', $null, 1400),
    @('RemoveExistingProducts', $null, 1401), @('InstallInitialize', $null, 1500), @('ProcessComponents', $null, 1600),
    @('UnpublishFeatures', $null, 1800),
    @('DriverUninstall', 'REMOVE="ALL"', 2550),          # while the helper is still on disk
    @('RemoveRegistryValues', $null, 2600), @('RemoveFiles', $null, 3500), @('InstallFiles', $null, 4000),
    @('DriverInstall', 'NOT REMOVE="ALL"', 4100),        # after the files are in place
    @('WriteRegistryValues', $null, 5000), @('RegisterUser', $null, 6000), @('RegisterProduct', $null, 6100),
    @('PublishFeatures', $null, 6300), @('PublishProduct', $null, 6400), @('InstallFinalize', $null, 6600)
)
foreach ($a in $execute) { Add-Row 'InstallExecuteSequence' @('Action', 'Condition', 'Sequence') $a }
foreach ($a in @(@('FindRelatedProducts', $null, 25), @('ValidateProductID', $null, 700), @('CostInitialize', $null, 800),
                 @('FileCost', $null, 900), @('CostFinalize', $null, 1000),
                 @('WelcomeDlg', 'NOT Installed', 1200),
                 @('MaintenanceDlg', 'Installed AND NOT RESUME AND NOT Preselected', 1210),
                 @('ProgressDlg', $null, 1280), @('ExecuteAction', $null, 1300),
                 @('SetResultText', $null, 1310),
                 @('ExitDlg', $null, -1), @('UserExitDlg', $null, -2), @('FatalErrorDlg', $null, -3))) {
    Add-Row 'InstallUISequence' @('Action', 'Condition', 'Sequence') $a
}
# Administrative install ("msiexec /a"): just unpacks the files; used to test the package.
foreach ($a in @(@('CostInitialize', $null, 800), @('FileCost', $null, 900), @('CostFinalize', $null, 1000),
                 @('InstallValidate', $null, 1400), @('InstallInitialize', $null, 1500), @('InstallAdminPackage', $null, 3900),
                 @('InstallFiles', $null, 4000), @('InstallFinalize', $null, 6600))) {
    Add-Row 'AdminExecuteSequence' @('Action', 'Condition', 'Sequence') $a
}
foreach ($a in @(@('CostInitialize', $null, 800), @('FileCost', $null, 900), @('CostFinalize', $null, 1000), @('ExecuteAction', $null, 1300))) {
    Add-Row 'AdminUISequence' @('Action', 'Condition', 'Sequence') $a
}

# Cabinet as an embedded stream ("#op20.cab" in the Media table).
$view = $db.OpenView('INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)')
$rec = $installer.CreateRecord(2)
$rec.StringData(1) = 'op20.cab'
$rec.SetStream(2, $cabPath)
$view.Execute($rec)
$view.Close()

# Summary information
$summary = $db.SummaryInformation(20)
$summary.Property(1) = 1252                              # code page
$summary.Property(2) = 'Installation Database'
$summary.Property(3) = $productName
$summary.Property(4) = $manufacturer
$summary.Property(5) = 'Installer'
$summary.Property(6) = "This installer database contains the logic and data required to install $productName."
$summary.Property(7) = 'x64;1033'                        # 64-bit package, English
$summary.Property(9) = $packageCode
$summary.Property(12) = [datetime]::Now
$summary.Property(13) = [datetime]::Now
$summary.Property(14) = 200                              # needs Windows Installer 2.0+
$summary.Property(15) = 2                                # long file names, compressed
$summary.Property(18) = 'Op20WinUSB build-msi.ps1'
$summary.Property(19) = 2                                # read-only recommended
$summary.Persist()
$db.Commit()

foreach ($o in $rec, $view, $summary, $db, $installer) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) }
[GC]::Collect()
[GC]::WaitForPendingFinalizers()

Write-Host ("Built {0} ({1:N0} bytes), product code {2}" -f $msiPath, (Get-Item $msiPath).Length, $productCode)
