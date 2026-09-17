# Run only in the isolated VMware Windows 10 guest, after KD and driver loading were verified.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Ctl,
      [Parameter(Mandatory)][string]$EvidenceDirectory,
      [Parameter(Mandatory)][ValidateSet(1,2,4,8)][int]$Vcpu,
      [ValidateRange(1,1000)][int]$Cycles=20,
      [ValidateRange(0,86400)][int]$SoakSeconds=0,
      [ValidateRange(10,600)][int]$CommandTimeoutSeconds=60)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$os=Get-CimInstance Win32_OperatingSystem
$machine=Get-CimInstance Win32_ComputerSystem
if ($os.Caption -notmatch 'Windows 10' -or [int]$os.BuildNumber -ge 22000 -or $machine.Manufacturer -notmatch 'VMware') {
    throw 'This acceptance runner is restricted to a VMware Windows 10 guest.'
}
if ([int]$machine.NumberOfLogicalProcessors -ne $Vcpu) { throw 'Unexpected vCPU topology.' }
$guard=Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard
if ($guard.VirtualizationBasedSecurityStatus -eq 2) { throw 'Guest VBS is still running.' }
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Use a fresh evidence directory for each run.' }
New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
$Ctl=(Resolve-Path -LiteralPath $Ctl).Path
$script:commandId=0; $script:uncertain=$false
$journal=Join-Path $EvidenceDirectory 'controls.jsonl'
function Record($Value) { $Value | ConvertTo-Json -Depth 12 -Compress | Add-Content -LiteralPath $journal -Encoding UTF8 }
function Hvm([string]$Verb) {
    $script:commandId++
    $stem=Join-Path $EvidenceDirectory ('{0:D5}-{1}' -f $script:commandId,$Verb)
    Record @{id=$script:commandId;phase='before';command=$Verb;utc=[DateTime]::UtcNow.ToString('o')}
    $process=Start-Process -FilePath $Ctl -ArgumentList @('--json',$Verb) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$stem.json" -RedirectStandardError "$stem.stderr.txt"
    if (-not $process.WaitForExit($CommandTimeoutSeconds*1000)) {
        $script:uncertain=$true
        Record @{id=$script:commandId;phase='timeout';pid=$process.Id;state='RollbackUnproven'}
        throw 'Control timed out. Process retained; use KD and preserve the VM/logs. No further controls will run.'
    }
    $process.WaitForExit()
    $exitCode=$process.ExitCode
    Record @{id=$script:commandId;phase='after';exitCode=$exitCode;utc=[DateTime]::UtcNow.ToString('o')}
    if ($exitCode -ne 0) { throw "Control $Verb failed ($exitCode); raw response retained." }
    return Get-Content -LiteralPath "$stem.json" -Raw | ConvertFrom-Json
}
function CheckSet($Query,[bool]$Active) {
    if ($Query.backend -ne 2 -or $Query.processorCount -ne $Vcpu -or @($Query.processors).Count -ne $Vcpu) { throw 'Wrong backend or incomplete processor set.' }
    $ids=@($Query.processors | ForEach-Object { "$($_.group):$($_.number)" } | Sort-Object -Unique)
    $expected=@(0..($Vcpu-1) | ForEach-Object { "0:$_" } | Sort-Object)
    if (($ids -join ',') -ne ($expected -join ',')) { throw 'Duplicate, missing or unexpected processor identity.' }
    foreach ($cpu in $Query.processors) {
        if (($cpu.stateNames -contains 'RESIDENT_ACTIVE') -ne $Active) { throw "Unacknowledged CPU $($cpu.group):$($cpu.number)." }
    }
    if ($Query.residentProcessorCount -ne $(if ($Active) {$Vcpu} else {0})) { throw 'Summary does not match per-CPU ownership.' }
    if ($Query.stateNames -contains 'ROLLBACK_REQUIRED' -or $Query.stateNames -contains 'FAULTED') { throw 'Driver retained a fault or rollback requirement.' }
}
$metadata=@{os=$os.Caption;build=$os.BuildNumber;bootId=$os.LastBootUpTime.ToUniversalTime().ToString('o');
    cpuCount=$Vcpu;cycles=$Cycles;soakSeconds=$SoakSeconds;ctlHash=(Get-FileHash $Ctl).Hash;hardwareResult='NotRun'}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'guest.json') -Encoding UTF8
try {
    $initial=Hvm status
    if ($initial.residentProcessorCount -ne 0 -or $initial.preparedProcessorCount -ne 0) { throw 'Start with a released, inactive driver.' }
    Hvm prepare | Out-Null
    $prepared=Hvm status
    CheckSet $prepared $false
    Hvm self-test | Out-Null
    $tested=Hvm status
    if ($tested.selfTestPassedProcessorCount -ne $Vcpu) { throw 'Incomplete real SVM self-test.' }
    $epoch=$tested.powerGeneration
    Hvm metrics | Out-Null
    for ($cycle=1; $cycle -le $Cycles; $cycle++) {
        Hvm resident | Out-Null
        $active=Hvm status
        CheckSet $active $true
        if ($active.powerGeneration -ne $epoch) { throw 'Power/topology generation changed.' }
        Hvm stop | Out-Null
        CheckSet (Hvm status) $false
        # Repeated stop is part of the public idempotency contract.
        Hvm stop | Out-Null
        Hvm metrics | Out-Null
    }
    if ($SoakSeconds -gt 0) {
        Hvm resident | Out-Null
        . (Join-Path $PSScriptRoot 'GuestWorkload.ps1')
        [KswordLabWorkload]::Start($Vcpu,$EvidenceDirectory)
        $lastProgress=New-Object long[] $Vcpu
        $deadline=[DateTime]::UtcNow.AddSeconds($SoakSeconds)
        try {
            while ([DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Seconds ([Math]::Min(10,[Math]::Max(1,($deadline-[DateTime]::UtcNow).TotalSeconds)))
                $active=Hvm status
                CheckSet $active $true
                if ($active.powerGeneration -ne $epoch) { throw 'Soak crossed a power/topology generation.' }
                $progress=@([KswordLabWorkload]::Progress())
                Record @{phase='workload';progress=$progress;errors=@([KswordLabWorkload]::Errors())}
                for ($cpu=0; $cpu -lt $Vcpu; $cpu++) {
                    if ($progress[$cpu] -le $lastProgress[$cpu]) { throw "Worker $cpu made no progress." }
                }
                $lastProgress=$progress
                if ([KswordLabWorkload]::Errors().Length) { throw 'Workload integrity failure.' }
                Hvm metrics | Out-Null
            }
        } finally { [KswordLabWorkload]::Stop() }
        Hvm stop | Out-Null
        CheckSet (Hvm status) $false
    }
    Hvm teardown | Out-Null
    $released=Hvm status
    if ($released.preparedProcessorCount -ne 0 -or $released.residentProcessorCount -ne 0) { throw 'Resources still owned after teardown.' }
    Record @{phase='complete';result='PASS';cycles=$Cycles;soakSeconds=$SoakSeconds}
} catch {
    # No speculative teardown after timeout/fault: preserve diagnostics and hardware ownership.
    Record @{phase='failed';error=$_.Exception.Message;uncertain=$script:uncertain;result='FAIL'}
    throw
}
