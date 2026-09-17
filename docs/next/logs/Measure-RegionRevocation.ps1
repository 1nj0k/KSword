#requires -Version 5.1
<#
.SYNOPSIS
    Makes the descendant's source table change under a published region, and records what the lease does.

.DESCRIPTION
    A published region is supposed to stop serving when the source it was cloned
    from stops being what it was. Testing that needs a source change we can
    actually cause, and on this stack there is exactly one: a snapshot of the
    descendant. Taking one drops the intermediate VMM's entries for the region
    entirely -- which is visible here as the same base going from "512 entries,
    all agreeing" to STATUS_ADDRESS_ASSOCIATED-not while the snapshot exists --
    and deleting the snapshot brings them back.

    The sequence is publish, snapshot, poll, remove, delete snapshot, republish,
    remove. Every step's raw response is recorded, so the revocation reason is
    read from the driver rather than inferred from the fact that something
    happened.

    Which reason fires is itself the result. The lease's ordinary check runs at
    every shadow fill and covers the path the lease was captured on; the region
    recheck samples one other page per 4096 fills. A whole-VM event changes the
    captured path too, so the ordinary check sees it first and reports
    TRANSLATION_CHANGED. REGION_DRIFTED can only win when a page of the region
    other than the captured one changes while the captured path stays identical
    -- a per-page revocation, not a whole-table one -- and nothing we can ask
    this VMM to do produces that.

.NOTES
    Run inside the Windows VM that hosts the descendant, with the descendant
    running and its guest having touched the base being published.
#>
param(
    [string]$Exe = 'C:\ksword\hvm_ctl.exe',
    [string]$Vmx = 'C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx',
    [string]$VmRun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe',
    # Bases are tried in order; the first that publishes is used. At 4096 MiB
    # this descendant's bulk memory is above the 4 GiB line, not below it.
    [string[]]$Base = @('0x110000000', '0x118000000', '0x120000000', '0x108000000'),
    [int]$LeafShift = 21,
    [string]$SnapshotName = 'driftprobe',
    [int]$PollSeconds = 5,
    [int]$PollCount = 24,
    [string]$OutFile = 'C:\vmware\region-revocation.json'
)

$ErrorActionPreference = 'Continue'

function Invoke-Json {
    param([string[]]$Arguments)
    $text = & $Exe --json @Arguments 2>&1 | Out-String
    try { return ($text | ConvertFrom-Json) } catch { return [pscustomobject]@{ raw = $text.Trim() } }
}

& $Exe nested-page-remove > $null 2>&1
$query = Invoke-Json @('nested-page-query')
if ($null -eq $query.roots -or $query.roots.Count -eq 0) {
    [pscustomobject]@{ error = 'no-ept12-root' } | ConvertTo-Json |
        Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 2
}
$eptp = $query.roots[0]

$attempts = @()
$published = $null
foreach ($candidate in $Base) {
    $r = Invoke-Json @('nested-page-map-region-scan', $eptp, $candidate, "$LeafShift", '0')
    $attempts += [pscustomobject]@{ base = $candidate; status = $r.status; lastStatus = $r.lastStatus
                                    scannedLeafCount = $r.scannedLeafCount; scannedSharedBits = $r.scannedSharedBits }
    if ($r.status -eq 0) { $published = [pscustomobject]@{ base = $candidate; response = $r }; break }
}
if ($null -eq $published) {
    [pscustomobject]@{ error = 'nothing-admissible'; eptp = $eptp; attempts = $attempts } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutFile -Encoding ASCII
    exit 3
}

# The snapshot runs as a job so the lease can be polled while it is in progress:
# the revocation happens during the snapshot, not after vmrun returns.
$job = Start-Job -ScriptBlock { param($run, $path, $name)
    & $run -T ws snapshot "$path" $name 2>&1 | Out-String } -ArgumentList $VmRun, $Vmx, $SnapshotName

$samples = @()
$reason = 0
for ($i = 1; $i -le $PollCount; $i++) {
    Start-Sleep -Seconds $PollSeconds
    $q = Invoke-Json @('nested-page-query')
    $samples += [pscustomobject]@{ second = $i * $PollSeconds; active = $q.active; retired = $q.retired
                                   reason = $q.leaseRevocationReason; composed = $q.composedCount }
    if ($q.leaseRevocationReason -ne 0) { $reason = $q.leaseRevocationReason; break }
}
Remove-Job $job -Force -ErrorAction SilentlyContinue

$removal = Invoke-Json @('nested-page-remove')
# While the snapshot exists the entries are gone, so this attempt is expected to
# be refused; it is recorded because "refused while snapshotted" is the evidence
# that the snapshot, and not something else, is what removed them.
$whileSnapshotted = Invoke-Json @('nested-page-map-region-scan', $eptp, $published.base, "$LeafShift", '0')
& $Exe nested-page-remove > $null 2>&1

& $VmRun -T ws deleteSnapshot "$Vmx" $SnapshotName 2>&1 | Out-Null
Start-Sleep -Seconds 20
$afterDelete = Invoke-Json @('nested-page-map-region-scan', $eptp, $published.base, "$LeafShift", '0')
$finalRemoval = Invoke-Json @('nested-page-remove')

[pscustomobject]@{
    eptp              = $eptp
    base              = $published.base
    leafShift         = $LeafShift
    attempts          = $attempts
    publish           = $published.response
    samples           = $samples
    revocationReason  = $reason
    removalAfterRevocation = [pscustomobject]@{ status = $removal.status; active = $removal.active; retired = $removal.retired }
    republishWhileSnapshotted = [pscustomobject]@{ status = $whileSnapshotted.status; lastStatus = $whileSnapshotted.lastStatus
                                                   scannedLeafCount = $whileSnapshotted.scannedLeafCount }
    republishAfterDelete = [pscustomobject]@{ status = $afterDelete.status; lastStatus = $afterDelete.lastStatus
                                              scannedLeafCount = $afterDelete.scannedLeafCount
                                              scannedSharedBits = $afterDelete.scannedSharedBits }
    finalRemoval      = [pscustomobject]@{ status = $finalRemoval.status; active = $finalRemoval.active; retired = $finalRemoval.retired }
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutFile -Encoding ASCII

"base=$($published.base) reason=$reason removed=$($removal.status)/$($removal.retired) " +
"whileSnapshotted=$($whileSnapshotted.lastStatus) afterDelete=$($afterDelete.status)/$($afterDelete.scannedLeafCount)"
exit 0
