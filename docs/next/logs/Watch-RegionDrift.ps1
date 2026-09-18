#requires -Version 5.1
<#
.SYNOPSIS
    Publishes a scan-admitted region and waits for its source to stop agreeing.

.DESCRIPTION
    Admission by scanning proves that every source leaf under a region grants the
    same access and memory type -- at one instant. The driver rechecks one page
    per 4096 shadow fills and revokes with LEASE_REGION_DRIFTED when a leaf stops
    agreeing or goes absent.

    Observing that on hardware needs three things at once, and this script makes
    each of them checkable rather than assumed:

      * a region the scanning rule actually admits -- several bases are tried and
        the first that publishes is used, because which bases are mapped depends
        on what the descendant has touched;
      * enough shadow fills that the sampler runs at all -- the fill counters are
        recorded at both ends, so a run that saw no drift can be told apart from
        a run where the sampler never ticked;
      * a source that is free to change -- with the descendant's memory pinned
        and page sharing disabled the intermediate VMM has little reason to
        touch its own tables.

    The region is removed on the way out whether or not drift was seen.

    The fill counters are read before publication and after removal, never while
    an override is active. Reading them means running the all-processor nested
    probe, which performs its own VMX entry on every processor; the first version
    of this script ran it with a published override and a busy descendant, and
    the Windows VM hung hard enough to lose its heartbeat and ignore an injected
    NMI. That is recorded as an unexplained hazard, not as a diagnosis -- but
    there is no reason to sample inside the window, so it does not.

.NOTES
    Run inside the Windows VM that hosts the descendant, with a workload running
    in the descendant.
#>
param(
    [string]$Exe = 'C:\ksword\hvm_ctl.exe',
    [string[]]$Base = @('0x10000000', '0x20000000', '0x8000000', '0x40000000', '0x60000000'),
    [int]$LeafShift = 21,
    [int]$TimeoutSeconds = 600,
    [int]$PollSeconds = 3,
    [string]$OutFile = 'C:\vmware\region-drift.json'
)

$ErrorActionPreference = 'Continue'

function Invoke-Json {
    param([string[]]$Arguments)
    $text = & $Exe --json @Arguments 2>&1 | Out-String
    try { return ($text | ConvertFrom-Json) } catch { return [pscustomobject]@{ raw = $text.Trim() } }
}

function Get-Fills {
    $probe = Invoke-Json @('nested-probe-all')
    if ($null -eq $probe.row) { return @() }
    return @($probe.row | ForEach-Object { [long]$_.shadowFill })
}

& $Exe nested-page-remove > $null 2>&1
$fillsAtStart = Get-Fills
$query = Invoke-Json @('nested-page-query')
if ($null -eq $query.roots -or $query.roots.Count -eq 0) {
    [pscustomobject]@{ error = 'no-ept12-root' } | ConvertTo-Json |
        Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 2
}
$eptp = $query.roots[0]

$published = $null
$attempts = @()
foreach ($candidate in $Base) {
    $r = Invoke-Json @('nested-page-map-region-scan', $eptp, $candidate, "$LeafShift", '0')
    $attempts += [pscustomobject]@{
        base = $candidate; status = $r.status; lastStatus = $r.lastStatus
        scannedLeafCount = $r.scannedLeafCount; scannedSharedBits = $r.scannedSharedBits
    }
    if ($r.status -eq 0) { $published = [pscustomobject]@{ base = $candidate; response = $r }; break }
}
if ($null -eq $published) {
    [pscustomobject]@{ error = 'nothing-admissible'; eptp = $eptp; attempts = $attempts } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 3
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$samples = @()
$reason = 0
$elapsed = 0
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $PollSeconds
    $elapsed += $PollSeconds
    $q = Invoke-Json @('nested-page-query')
    $samples += [pscustomobject]@{
        second = $elapsed; active = $q.active; retired = $q.retired
        reason = $q.leaseRevocationReason; composed = $q.composedCount
    }
    if ($q.leaseRevocationReason -ne 0) { $reason = $q.leaseRevocationReason; break }
}

$removal = Invoke-Json @('nested-page-remove')
# Only now, with nothing published, is it safe to run the probe again.
$fillsAtEnd = Get-Fills

$deltas = @()
for ($i = 0; $i -lt $fillsAtStart.Count -and $i -lt $fillsAtEnd.Count; $i++) {
    $deltas += ($fillsAtEnd[$i] - $fillsAtStart[$i])
}
# One recheck per 4096 fills, summed over the processors that did them.
$ticks = 0
foreach ($d in $deltas) { $ticks += [math]::Floor($d / 4096) }

[pscustomobject]@{
    eptp                 = $eptp
    attempts             = $attempts
    base                 = $published.base
    leafShift            = $LeafShift
    admittedSharedBits   = $published.response.scannedSharedBits
    scannedLeafCount     = $published.response.scannedLeafCount
    regionPageCount      = $published.response.regionPageCount
    observedSeconds      = $elapsed
    revocationReason     = $reason
    fillDeltaPerProcessor = $deltas
    estimatedSamplerTicks = $ticks
    samples              = $samples
    removal              = [pscustomobject]@{ status = $removal.status; active = $removal.active; retired = $removal.retired }
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutFile -Encoding ASCII

"base=$($published.base) bits=$($published.response.scannedSharedBits) seconds=$elapsed reason=$reason ticks=$ticks fills=$($deltas -join ',')"
if ($reason -ne 0) { exit 0 } else { exit 4 }
