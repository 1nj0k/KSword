# Runs only in the dedicated VMware Windows 10 clone from the read-only KSwordLab share.
[CmdletBinding()]
param([ValidateSet('Inspect','Configure')][string]$Mode = 'Inspect')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$share = Split-Path -Parent $PSCommandPath
$required = @('KswordARK.sys','KswordARK.pdb','hvm_ctl.exe','AMD-Lab-TestSigning.cer',
    'GuestWorkload.ps1','Invoke-GuestAcceptance.ps1','lab-ownership.json','Prepare-Guest.ps1','identity.json')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $share $name))) {
        throw "Missing staged lab file: $name"
    }
}

$os = Get-CimInstance Win32_OperatingSystem
$computer = Get-CimInstance Win32_ComputerSystem
if ($computer.Manufacturer -notmatch 'VMware' -or $os.Caption -notmatch 'Windows 10' -or
    [int]$os.BuildNumber -ge 22000) {
    throw 'This bootstrap is restricted to the verified VMware Windows 10 clone.'
}
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$secureBoot = Confirm-SecureBootUEFI
$output = [ordered]@{
    phase = $Mode; os = $os.Caption; build = $os.BuildNumber
    bootId = $os.LastBootUpTime.ToUniversalTime().ToString('o')
    processors = $computer.NumberOfLogicalProcessors; secureBoot = [bool]$secureBoot
    administrator = [bool]$isAdmin; configured = $false
}
if ($Mode -eq 'Inspect') {
    $output | ConvertTo-Json -Depth 6
    return
}
if (-not $isAdmin) { throw 'Run this bootstrap from an elevated Windows PowerShell in the clone.' }
if ($secureBoot) {
    throw 'The clone virtual Secure Boot is enabled. Shut down the clone, disable Secure Boot only in its VMware firmware settings, boot it again, then rerun Configure.'
}

$identity = Get-Content -LiteralPath (Join-Path $share 'identity.json') -Raw | ConvertFrom-Json
if (-not $identity.pdbIdentityMatched) { throw 'Candidate identity file does not attest a matching SYS/PDB.' }
$destination = Join-Path $env:SystemDrive 'KSwordLab\candidate'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in $required) {
    Copy-Item -LiteralPath (Join-Path $share $name) -Destination $destination -Force
}
foreach ($name in @('KswordARK.sys','KswordARK.pdb','hvm_ctl.exe','AMD-Lab-TestSigning.cer')) {
    $expected = $identity.candidateSha256.$name
    if ((Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash -ne $expected) {
        throw "Candidate hash mismatch after copy: $name"
    }
}
$certificate = Join-Path $destination 'AMD-Lab-TestSigning.cer'
Import-Certificate -FilePath $certificate -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $certificate -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
& (Join-Path $destination 'Prepare-Guest.ps1') -Configure -CloneManifest (Join-Path $destination 'lab-ownership.json')
if ($LASTEXITCODE -ne 0) { throw 'Guest preparation failed.' }
$output.configured = $true
$output.candidateDirectory = $destination
$output.nextStep = 'Restart this clone; then establish KD and load the staged driver before SVM commands.'
$output | ConvertTo-Json -Depth 8 | Tee-Object -FilePath (Join-Path $destination 'bootstrap.json')
