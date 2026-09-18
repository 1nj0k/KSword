# Diagnostic-only host load/query/unload. Never issue prepare, self-test or resident.
[CmdletBinding()]
param([string]$DriverPath, [string]$ControlPath)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($DriverPath)) { $DriverPath=Join-Path $repository 'Ksword5.1\x64\Release\KswordARK.sys' }
if ([string]::IsNullOrWhiteSpace($ControlPath)) { $ControlPath=Join-Path $repository 'tools\hvm_ctl\hvm_ctl.exe' }
$DriverPath=(Resolve-Path -LiteralPath $DriverPath).Path
$ControlPath=(Resolve-Path -LiteralPath $ControlPath).Path
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run from an elevated host PowerShell.' }
$service=Get-CimInstance Win32_SystemDriver -Filter "Name='KswordARK'"
if (-not $service -or $service.State -ne 'Stopped') { throw 'Existing KswordARK service must be STOPPED.' }
$evidence=Join-Path $PSScriptRoot ('artifacts\host-admission-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $evidence | Out-Null
$os=Get-CimInstance Win32_OperatingSystem
$machine=Get-CimInstance Win32_ComputerSystem
@{driver=$DriverPath;driverSha256=(Get-FileHash $DriverPath).Hash;control=$ControlPath;
    controlSha256=(Get-FileHash $ControlPath).Hash;previousServicePath=$service.PathName;
    bootId=$os.LastBootUpTime.ToUniversalTime().ToString('o');build=$os.BuildNumber;
    hypervisorPresent=$machine.HypervisorPresent;logicalProcessors=$machine.NumberOfLogicalProcessors;
    scope='load-query-unload-only'} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $evidence 'identity.json') -Encoding UTF8
function Invoke-HostService([string]$Label,[string[]]$Arguments) {
    & sc.exe @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $evidence ($Label+'.txt')) | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Service operation failed: $Label, exit=$LASTEXITCODE. Evidence: $evidence" }
}
$started=$false
try {
    Invoke-HostService 'config' @('config','KswordARK','binPath=',$DriverPath,'start=','demand')
    Invoke-HostService 'start' @('start','KswordARK')
    $started=$true
    # Production CLI emits ASCII JSON, so both PS5 and PS7 native decoders preserve it.
    $raw=& $ControlPath --json status 2> (Join-Path $evidence 'status.stderr.txt')
    $queryExit=$LASTEXITCODE
    $raw | Set-Content -LiteralPath (Join-Path $evidence 'status.json') -Encoding UTF8
    if ($queryExit -ne 0) { throw "Status query failed: $queryExit. Evidence: $evidence" }
    $query=($raw -join "`n") | ConvertFrom-Json
    $probe=$query.svmProbe
    if (-not $probe.PSObject.Properties['rejectReason']) { throw 'Diagnostic fields missing; update the SYS and CLI together.' }
    [pscustomobject]@{backendStatus=$query.backendStatus;probe=$probe;evidence=$evidence} | ConvertTo-Json -Depth 8
} finally {
    if ($started) {
        Invoke-HostService 'stop' @('stop','KswordARK')
        Invoke-HostService 'query-after-stop' @('query','KswordARK')
    }
    Write-Host "Host admission evidence: $evidence"
}
