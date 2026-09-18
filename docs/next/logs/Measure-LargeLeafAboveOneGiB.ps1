#requires -Version 5.1
<#
.SYNOPSIS
    Publishes a 2 MiB override above the 1 GiB line and records what it does.

.DESCRIPTION
    Records, as raw driver responses rather than as prose:

      * publish   - the scanning admission rule over a region the descendant's
                    VMM maps at 4 KiB, with the shared permission bits it proved.
      * t0        - source and backing digests immediately after publication;
                    equal, because the region is cloned from the original.
      * t1        - the same digests after an idle interval.
      * stage     - one page overwritten; the backing digest moves and the
                    source digest does not, which is what decoupled means.
      * stage-oob - the page index one past the end, refused rather than wrapped.
      * remove    - the override withdrawn and every processor invalidated.

    The base must already be mapped by the intermediate VMM. Configured memory
    is not mapped memory: run a workload in the descendant first, or the probe
    returns STATUS_ADDRESS_NOT_ASSOCIATED and nothing here is meaningful.

.NOTES
    Run inside the Windows VM that hosts the descendant.
#>
param(
    [string]$Exe = 'C:\ksword\hvm_ctl.exe',
    [string]$Base = '0x60000000',
    [int]$LeafShift = 21,
    [int]$StageIndex = 300,
    [int]$IdleSeconds = 30,
    [string]$OutFile = 'C:\vmware\large-leaf-above-1gib.json'
)

$ErrorActionPreference = 'Continue'

function Invoke-Json {
    param([string[]]$Arguments)
    $text = & $Exe --json @Arguments 2>&1 | Out-String
    try { return ($text | ConvertFrom-Json) } catch { return [pscustomobject]@{ raw = $text.Trim() } }
}

# Start from no override, whatever a previous run left behind.
& $Exe nested-page-remove > $null 2>&1

$query = Invoke-Json @('nested-page-query')
if ($null -eq $query.roots -or $query.roots.Count -eq 0) {
    [pscustomobject]@{ error = 'no-ept12-root'; query = $query } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 2
}
$eptp = $query.roots[0]

$steps = [ordered]@{}
$steps['publish'] = Invoke-Json @('nested-page-map-region-scan', $eptp, $Base, "$LeafShift", '0')
$steps['t0']      = Invoke-Json @('nested-page-digest')
Start-Sleep -Seconds $IdleSeconds
$steps['t1']      = Invoke-Json @('nested-page-digest')
$steps['stage']   = Invoke-Json @('nested-page-stage', "$StageIndex", '0xAB')
$steps['t2']      = Invoke-Json @('nested-page-digest')
# One past the last page of the region: a refusal, not a wrapped index.
$oob = [int]([math]::Pow(2, $LeafShift - 12))
$steps['stageOutOfRange'] = Invoke-Json @('nested-page-stage', "$oob", '0xCD')
$steps['t3']      = Invoke-Json @('nested-page-digest')
$steps['remove']  = Invoke-Json @('nested-page-remove')

[pscustomobject]@{
    eptp       = $eptp
    base       = $Base
    leafShift  = $LeafShift
    stageIndex = $StageIndex
    outOfRangeIndex = $oob
    idleSeconds = $IdleSeconds
    steps      = $steps
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutFile -Encoding ASCII

foreach ($name in $steps.Keys) {
    $s = $steps[$name]
    '{0,-16} status={1} last={2} leaf={3} scanned={4} bits={5} source={6} backing={7} staged={8} active={9}' -f `
        $name, $s.status, $s.lastStatus, $s.leafShift, $s.scannedLeafCount, $s.scannedSharedBits,
        $s.sourceDigest, $s.backingDigest, $s.stagedPageCount, $s.active
}
exit 0
