# Copy only KSword lab evidence into the dedicated writable results share.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$machine=Get-CimInstance Win32_ComputerSystem
if ($machine.Manufacturer -notmatch 'VMware') { throw 'Run this exporter inside the lab clone.' }
$lab=Join-Path $env:SystemDrive 'KSwordLab'
$share=[string]::Concat([char]92,[char]92,'vmware-host',[char]92,'Shared Folders',[char]92,'KSwordResults')
if (-not (Test-Path -LiteralPath $share)) { throw 'KSwordResults share is unavailable; local evidence is retained.' }
$destination=Join-Path $share ('export-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $destination | Out-Null
$sources=@(Get-ChildItem -LiteralPath $lab -Directory | Where-Object Name -match '^(single|irqfix|two-cpu|four-cpu|eight-cpu)(-|$)')
$candidate=Join-Path $lab 'candidate'
$sources+=@(Get-ChildItem -LiteralPath $candidate -Directory -Filter 'driver-load-*')
$records=@()
foreach ($source in $sources) {
    foreach ($file in @(Get-ChildItem -LiteralPath $source.FullName -File -Recurse)) {
        $relative=$file.FullName.Substring($lab.TrimEnd([char]92).Length+1)
        $target=Join-Path $destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        $expected=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $file.FullName -Destination $target
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected) { throw "Export hash mismatch: $relative" }
        $records+=@{path=$relative;sha256=$expected;bytes=$file.Length}
    }
}
if ($records.Count -eq 0) { throw 'No completed or failed lab run evidence found.' }
@{schema=1;utc=[DateTime]::UtcNow.ToString('o');files=$records} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destination 'export-manifest.json') -Encoding UTF8
[pscustomobject]@{kind='evidence-export';result='PASS';files=$records.Count;destination=$destination} | ConvertTo-Json
