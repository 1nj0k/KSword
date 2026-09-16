#requires -Version 5.1
<#
.SYNOPSIS
    Sweeps guest-physical addresses to find how far the descendant's EPT12 actually maps.

.DESCRIPTION
    Configured memory is not mapped memory. VMware populates EPT12 on demand, so a
    descendant told it has 4 GiB still has entries only where the guest has touched
    something. Publishing an override at a given base therefore answers a question
    about the *source* table, not about our planner: a refusal at 0x40000000 with
    scannedLeafCount 0 means EPT12 has nothing there to clone.

    Each probe publishes at most one override and removes it again, because only one
    override can be active at a time.

.NOTES
    Run this inside the Windows VM that hosts the descendant. The driver must be
    resident and the descendant running.
#>
param(
    [string]$Exe = 'C:\ksword\hvm_ctl.exe',
    # Hex bases, with or without the 0x prefix. Must be aligned to the leaf size.
    [string[]]$Gpa = @('2000000', '4000000', '8000000', '10000000', '20000000', '40000000'),
    [int]$LeafShift = 21,
    [switch]$Scan,
    [string]$OutFile = 'C:\vmware\ept12-extent.json'
)

$ErrorActionPreference = 'Continue'

$queryText = & $Exe --json nested-page-query 2>&1 | Out-String
$query = $null
try { $query = $queryText | ConvertFrom-Json } catch { }
if ($null -eq $query -or $null -eq $query.roots -or $query.roots.Count -eq 0) {
    "NO-EPT12-ROOT`n$queryText" | Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 2
}
$eptp = $query.roots[0]

$command = if ($Scan) { 'nested-page-map-region-scan' } else { 'nested-page-map-region' }
$results = @()

foreach ($base in $Gpa) {
    $hex = if ($base -like '0x*') { $base } else { '0x' + $base }
    $text = & $Exe --json $command $eptp $hex $LeafShift 0 2>&1 | Out-String
    $row = $null
    try { $row = $text | ConvertFrom-Json } catch { }
    $results += [pscustomobject]@{
        base            = $hex
        command         = $command
        leafShift       = $LeafShift
        status          = if ($row) { $row.status } else { $null }
        lastStatus      = if ($row) { $row.lastStatus } else { $null }
        active          = if ($row) { $row.active } else { $null }
        sourceLeafShift = if ($row) { $row.sourceLeafShift } else { $null }
        regionBytes     = if ($row) { $row.regionBytes } else { $null }
        admittedByScan  = if ($row) { $row.admittedByScan } else { $null }
        scannedLeafCount = if ($row) { $row.scannedLeafCount } else { $null }
        scannedSharedBits = if ($row) { $row.scannedSharedBits } else { $null }
        composedCount   = if ($row) { $row.composedCount } else { $null }
        raw             = if ($row) { $null } else { $text.Trim() }
    }
    # Always drop the override, successful or not, so the next base starts clean.
    & $Exe nested-page-remove > $null 2>&1
}

$payload = [pscustomobject]@{
    eptp    = $eptp
    roots   = $query.roots
    probes  = $results
}
$payload | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutFile -Encoding ASCII
$payload.probes | Format-Table base,status,lastStatus,sourceLeafShift,admittedByScan,scannedLeafCount -AutoSize | Out-String
exit 0
