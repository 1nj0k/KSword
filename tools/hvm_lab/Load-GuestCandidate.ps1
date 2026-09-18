# Load the staged candidate only in the dedicated guest after bootstrap and KD are ready.
[CmdletBinding()]
param([string]$CandidateDirectory = 'C:\KSwordLab\candidate',
      [string]$ControlSourceDirectory = $PSScriptRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$candidate = (Resolve-Path -LiteralPath $CandidateDirectory).Path
$driver = Join-Path $candidate 'KswordARK.sys'
$control = Join-Path $candidate 'hvm_ctl.exe'
foreach ($path in @($driver, $control, (Join-Path $candidate 'identity.json'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing staged candidate file: $path" }
}
$os = Get-CimInstance Win32_OperatingSystem
$computer = Get-CimInstance Win32_ComputerSystem
if ($computer.Manufacturer -notmatch 'VMware' -or $os.Caption -notmatch 'Windows 10' -or
    [int]$os.BuildNumber -ge 22000) {
    throw 'This loader is restricted to the verified VMware Windows 10 clone.'
}
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run the candidate loader from an elevated Windows PowerShell in the clone.'
}
$bootId = $os.LastBootUpTime.ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$evidence = Join-Path $candidate ("driver-load-" + $bootId + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $evidence | Out-Null
# Refresh only user-mode control files. An already loaded SYS must never be replaced.
$sourceManifest = Join-Path $ControlSourceDirectory 'identity.json'
if ((Test-Path -LiteralPath $sourceManifest) -and
    [IO.Path]::GetFullPath($ControlSourceDirectory).TrimEnd('\') -ine $candidate.TrimEnd('\')) {
    $incoming = Get-Content -LiteralPath $sourceManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($name in @('KswordARK.sys','KswordARK.pdb')) {
        if ((Get-FileHash -LiteralPath (Join-Path $candidate $name) -Algorithm SHA256).Hash -ne $incoming.candidateSha256.$name) {
            throw "Control update requires the same loaded driver and PDB: $name"
        }
    }
    $sourceControl = Join-Path $ControlSourceDirectory 'hvm_ctl.exe'
    if ((Get-FileHash -LiteralPath $sourceControl -Algorithm SHA256).Hash -ne $incoming.candidateSha256.'hvm_ctl.exe') {
        throw 'Shared control tool does not match its identity manifest.'
    }
    Copy-Item -LiteralPath (Join-Path $candidate 'identity.json') -Destination (Join-Path $evidence 'previous-identity.json')
    Copy-Item -LiteralPath $sourceControl -Destination $control -Force
    Copy-Item -LiteralPath $sourceManifest -Destination (Join-Path $candidate 'identity.json') -Force
}
$identity = Get-Content -LiteralPath (Join-Path $candidate 'identity.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($name in @('KswordARK.sys','hvm_ctl.exe')) {
    if ((Get-FileHash -LiteralPath (Join-Path $candidate $name) -Algorithm SHA256).Hash -ne $identity.candidateSha256.$name) {
        throw "Candidate hash mismatch: $name"
    }
}
$signature = Get-AuthenticodeSignature -LiteralPath $driver
if ($signature.Status -ne 'Valid') { throw "Candidate signature is not trusted in this clone: $($signature.Status) $($signature.StatusMessage)" }

function Invoke-Recorded([string]$Name, [string[]]$Arguments) {
    $out = Join-Path $evidence ($Name + '.txt')
    & sc.exe @Arguments 2>&1 | Tee-Object -FilePath $out | Out-Null
    return [int]$LASTEXITCODE
}

$serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK'
if (Test-Path -LiteralPath $serviceKey) {
    $query = Invoke-Recorded 'sc-query-existing' @('queryex','KswordARK')
    $configQuery = Invoke-Recorded 'sc-qc-existing' @('qc','KswordARK')
    if ($query -ne 0 -or $configQuery -ne 0) {
        throw 'KswordARK registry key exists but SCM cannot query the service.'
    }
    # Read plain strings: PS 5.1 Get-Content adds provider properties which
    # ConvertTo-Json can recursively serialize into very large evidence files.
    $stateText = [IO.File]::ReadAllText((Join-Path $evidence 'sc-query-existing.txt'))
    $stopped = $stateText -match 'STATE\s*:\s*1\s+STOPPED'
    $running = $stateText -match 'STATE\s*:\s*4\s+RUNNING'
    $currentPath = (Get-ItemProperty -LiteralPath $serviceKey -Name ImagePath -ErrorAction Stop).ImagePath
    @{imagePath=$currentPath; query=$stateText;
        configuration=[IO.File]::ReadAllText((Join-Path $evidence 'sc-qc-existing.txt'))} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'previous-service.json') -Encoding UTF8
    $normalizedPath = ($currentPath.Trim('"') -replace '^\\{1,2}\?\?\\', '')
    $sameDriver = [string]::Equals($normalizedPath, $driver, [StringComparison]::OrdinalIgnoreCase)
    if (-not $stopped -and -not ($running -and $sameDriver)) {
        throw 'Existing driver is active at another path or transitioning; preserve it and inspect the recorded SCM evidence.'
    }
    if (-not $sameDriver) {
        $config = Invoke-Recorded 'sc-config-candidate' @('config','KswordARK','type=','kernel','start=','demand',
            'binPath=',$driver,'DisplayName=','KswordARK Driver Service')
        if ($config -ne 0) { throw "Cannot redirect the stopped KswordARK service to the candidate ($config)." }
    }
} else {
    $create = Invoke-Recorded 'sc-create' @('create','KswordARK','type=','kernel','start=','demand',
        'binPath=',$driver,'DisplayName=','KswordARK Driver Service')
    if ($create -ne 0) { throw "Cannot create the candidate driver service ($create)." }
}

$query = Invoke-Recorded 'sc-query-before-start' @('queryex','KswordARK')
if ($query -ne 0) { throw "Cannot query the candidate driver service ($query)." }
$statusText = Get-Content -LiteralPath (Join-Path $evidence 'sc-query-before-start.txt') -Raw
if ($statusText -notmatch 'STATE\s*:\s*4\s+RUNNING') {
    $start = Invoke-Recorded 'sc-start' @('start','KswordARK')
    if ($start -ne 0) { throw "Candidate driver start failed ($start); preserve $evidence for KD diagnosis." }
}
Start-Sleep -Milliseconds 500
$query = Invoke-Recorded 'sc-query-after-start' @('queryex','KswordARK')
if ($query -ne 0) { throw "Cannot query the candidate driver after start ($query)." }
$statusText = Get-Content -LiteralPath (Join-Path $evidence 'sc-query-after-start.txt') -Raw
if ($statusText -notmatch 'STATE\s*:\s*4\s+RUNNING') { throw "Candidate driver did not reach RUNNING; preserve $evidence for KD diagnosis." }

$queryPath = Join-Path $evidence 'hvm-status.json'
& $control --json status 2> (Join-Path $evidence 'hvm-status.stderr.txt') |
    Out-File -LiteralPath $queryPath -Encoding UTF8 -Width 32768
if ($LASTEXITCODE -ne 0) { throw "Driver loaded but hvm_ctl status failed ($LASTEXITCODE); preserve $evidence." }
$result = [ordered]@{result='PASS'; driver=$driver; signature=$signature.Status; evidence=$evidence;
    status=(Get-Content -LiteralPath $queryPath -Raw -Encoding UTF8 | ConvertFrom-Json)}
$result | ConvertTo-Json -Depth 12 | Tee-Object -FilePath (Join-Path $evidence 'result.json')
