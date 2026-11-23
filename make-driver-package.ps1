<#
.SYNOPSIS
    Builds the driver package in .\driver: op20winusb.inf + op20winusb.cat + the DLLs.

.DESCRIPTION
    Copies the built DLLs next to the INF and creates the catalog (op20winusb.cat)
    covering the INF and the DLLs. Needs makecat.exe from the Windows SDK.

    Windows only installs an INF whose catalog is signed by a publisher the
    machine trusts. Without a commercial code-signing certificate that means a
    self-signed one:

    -SelfSign  creates a one-off self-signed code-signing certificate in the
               current user's store, signs the catalog with it and then deletes
               the certificate together with its private key, so nothing else
               can ever be signed with it. The public certificate stays embedded
               in the .cat; the installer (MSI) adds it to the machine's trusted
               stores.

    No admin rights are needed and nothing in Windows' trust settings changes here.
#>
param([switch]$SelfSign)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = Join-Path $root 'driver'
$files = 'op20winusb.inf', 'op20winusb32.dll', 'op20winusb64.dll'

Copy-Item (Join-Path $root 'out\x86\op20winusb32.dll'), (Join-Path $root 'out\x64\op20winusb64.dll') -Destination $pkg -Force

$makecat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter makecat.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $makecat) { throw 'makecat.exe not found - install the Windows SDK.' }

# Catalog definition: the same attributes inf2cat writes for a Windows 10/11 package.
$cdf = @(
    '[CatalogHeader]'
    'Name=op20winusb.cat'
    'PublicVersion=0x0000001'
    'EncodingType=0x00010001'
    'PageHashes=false'
    'CatalogVersion=2'
    'HashAlgorithms=SHA256'
    'CATATTR1=0x10010001:OSAttr:2:10.0'
    'CATATTR2=0x10010001:HWID1:usb\vid_0403&pid_cc4d'
    ''
    '[CatalogFiles]'
)
foreach ($f in $files) {
    $cdf += "<HASH>$f=$f"
    $cdf += "<HASH>${f}ATTR1=0x10010001:File:$f"
    $cdf += "<HASH>${f}ATTR2=0x10010001:OSAttr:2:10.0"
}
$cdfPath = Join-Path $pkg 'op20winusb.cdf'
Set-Content -Path $cdfPath -Value $cdf -Encoding ascii

Push-Location $pkg
try {
    Remove-Item 'op20winusb.cat' -ErrorAction SilentlyContinue
    & $makecat.FullName -v 'op20winusb.cdf' | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path 'op20winusb.cat')) { throw "makecat failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
    Remove-Item $cdfPath -ErrorAction SilentlyContinue
}
$cat = Join-Path $pkg 'op20winusb.cat'
Write-Host "Catalog created: $cat"

if ($SelfSign) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Op20WinUSB driver package (self-signed)' `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears(20) -CertStoreLocation 'Cert:\CurrentUser\My'
    try {
        $result = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if (-not $result.SignerCertificate) { throw "signing failed: $($result.StatusMessage)" }
        Write-Host "Catalog signed. Certificate thumbprint: $($cert.Thumbprint)"
    } finally {
        # Throw the private key away: the certificate can never sign anything again.
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -Force
        Write-Host 'Signing certificate and its private key deleted.'
    }
} else {
    Write-Host 'Catalog is NOT signed - Windows will refuse to install this INF until it is.'
}

Get-ChildItem $pkg | Select-Object Name, Length | Format-Table -AutoSize
