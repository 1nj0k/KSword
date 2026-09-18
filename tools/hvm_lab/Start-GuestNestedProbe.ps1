# Run from the staged nested-probe share after a cold guest boot; never touch the host.
[CmdletBinding()]
param([ValidateSet(1,2,4,8)][int]$Vcpu=1,
      [ValidateRange(1,1000)][int]$Cycles=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$machine=Get-CimInstance Win32_ComputerSystem
$os=Get-CimInstance Win32_OperatingSystem
if ($machine.Manufacturer -notmatch 'VMware' -or $os.Caption -notmatch 'Windows 10' -or
    [int]$os.BuildNumber -ge 22000 -or [int]$machine.NumberOfLogicalProcessors -ne $Vcpu) {
    throw 'This entry requires the Windows 10 lab clone with the requested CPU count.'
}
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Use an elevated guest PowerShell.' }
if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK') {
    $state=& sc.exe query KswordARK
    if ($LASTEXITCODE -ne 0 -or ($state -join "`n") -notmatch 'STATE\s*:\s*1\s+STOPPED') {
        throw 'The existing driver must be STOPPED. This entry does not unload an active driver.'
    }
}
$identity=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'identity.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$suffix=(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N')
$candidate=Join-Path $env:SystemDrive ('KSwordLab\nested-candidate-'+$suffix)
$evidence=Join-Path $env:SystemDrive ('KSwordLab\nested-probe-'+$suffix)
New-Item -ItemType Directory -Path $candidate | Out-Null
foreach ($name in @('KswordARK.sys','KswordARK.pdb','hvm_ctl.exe')) {
    $source=Join-Path $PSScriptRoot $name
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $identity.candidateSha256.$name) { throw "Source hash mismatch: $name" }
    $target=Join-Path $candidate $name
    Copy-Item -LiteralPath $source -Destination $target
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $identity.candidateSha256.$name) { throw "Copy hash mismatch: $name" }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'identity.json') -Destination $candidate
try {
    & (Join-Path $PSScriptRoot 'Load-GuestCandidate.ps1') -CandidateDirectory $candidate -ControlSourceDirectory $PSScriptRoot | Out-Null
    Write-Host "Candidate loaded. Starting bounded nested probe; evidence: $evidence"
    & (Join-Path $PSScriptRoot 'Invoke-GuestAcceptance.ps1') -Ctl (Join-Path $candidate 'hvm_ctl.exe') `
        -EvidenceDirectory $evidence -Vcpu $Vcpu -Cycles $Cycles -NestedProbe
} finally {
    # Export completed and failed evidence alike. Never issue a driver control from cleanup.
    if (-not (Test-Path -LiteralPath $evidence)) { New-Item -ItemType Directory -Path $evidence | Out-Null }
    Copy-Item -LiteralPath (Join-Path $candidate 'identity.json') -Destination $evidence
    foreach ($load in @(Get-ChildItem -LiteralPath $candidate -Directory -Filter 'driver-load-*')) {
        Copy-Item -LiteralPath $load.FullName -Destination $evidence -Recurse
    }
    $results=[string]::Concat([char]92,[char]92,'vmware-host',[char]92,'Shared Folders',[char]92,'KSwordResults')
    if (Test-Path -LiteralPath $results) {
        $destination=Join-Path $results ('nested-probe-'+$suffix)
        New-Item -ItemType Directory -Path $destination | Out-Null
        $records=@()
        foreach ($file in @(Get-ChildItem -LiteralPath $evidence -File -Recurse)) {
            $relative=$file.FullName.Substring($evidence.Length+1)
            $target=Join-Path $destination $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $file.FullName -Destination $target
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) { throw "Evidence export mismatch: $relative" }
            $records+=@{path=$relative;sha256=$hash;bytes=$file.Length}
        }
        @{schema=1;files=$records} | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $destination 'export-manifest.json') -Encoding UTF8
        Write-Host "Evidence exported: $destination"
    } else { Write-Warning "Results share unavailable; evidence retained: $evidence" }
}
