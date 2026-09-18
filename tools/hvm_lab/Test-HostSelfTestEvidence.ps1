# Exercise only the production evidence validator; never load a driver or execute SVM.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-HostSvmSelfTest.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors|Out-String) }
$validator=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-HostSvmSelfTestEvidence'},$true)
Invoke-Expression $validator.Extent.Text
function New-TestEvidence([int]$Count=2) {
    $cpus=@(0..($Count-1)|ForEach-Object {
        [pscustomobject]@{group=0;number=$_;backend=2;executionStage=2;stateFlags=3;
            lastStatus='0x00000000';vmExitCount=1;svmExitCode='0x0000000000000072'}
    })
    $metrics=@(0..($Count-1)|ForEach-Object {
        [pscustomobject]@{group=0;number=$_;valid=1;sequence=2;stage=2;generation=3;
            failureStatus='0x00000000';failureStage=0;exitCode='0x0000000000000072';
            tlbRequests='1';hsavePa='0x0000000000001000';nptRootPa='0x0000000000002000'}
    })
    # JSON round-trip keeps each snapshot independent, as real CLI processes do.
    return ([pscustomobject]@{
        Prepared=@{queryStatus=0;backend=2;stateFlags=3;preparedProcessorCount=$Count;powerGeneration=0;generation=2;processors=$cpus}
        After=@{queryStatus=0;backend=2;stateFlags=19;processorCount=$Count;preparedProcessorCount=$Count;
            selfTestPassedProcessorCount=$Count;residentProcessorCount=0;powerGeneration=0;generation=3;processors=$cpus}
        Metrics=@{backend=2;version=4;svmProcessors=$metrics}
        Control=@{status=0;failedProcessorCount=0;selfTestPassedProcessorCount=$Count;residentProcessorCount=0}
    } | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}
$checks=0
foreach ($count in @(1,2,8,32)) {
    $t=New-TestEvidence $count
    Assert-HostSvmSelfTestEvidence $t.Prepared $t.After $t.Metrics $t.Control $count
    ++$checks
}
$failures=@(
    {param($t) $t.After.processors[1].number=0},
    {param($t) $t.Metrics.svmProcessors[1].number=3},
    {param($t) $t.After.processors=@($t.After.processors[0])},
    {param($t) $t.After.stateFlags=3},
    {param($t) $t.After.residentProcessorCount=1},
    {param($t) $t.After.powerGeneration=1},
    {param($t) $t.After.generation=2},
    {param($t) $t.Control.failedProcessorCount=1},
    {param($t) $t.Metrics.version=3},
    {param($t) $t.After.processors[0].vmExitCount=0},
    {param($t) $t.After.processors[0].svmExitCode='0xFFFFFFFFFFFFFFFF'},
    {param($t) $t.Metrics.svmProcessors[0].valid=0},
    {param($t) $t.Metrics.svmProcessors[0].sequence=3},
    {param($t) $t.Metrics.svmProcessors[0].failureStatus='0xC00000BB'},
    {param($t) $t.Metrics.svmProcessors[0].generation=2},
    {param($t) $t.Metrics.svmProcessors[0].tlbRequests='0'},
    {param($t) $t.Metrics.svmProcessors[0].hsavePa='0x0000000000000000'}
)
foreach ($change in $failures) {
    $t=New-TestEvidence
    & $change $t
    $rejected=$false
    try { Assert-HostSvmSelfTestEvidence $t.Prepared $t.After $t.Metrics $t.Control 2 }
    catch { $rejected=$true }
    if (-not $rejected) { throw "Invalid evidence accepted: $change" }
    ++$checks
}
Write-Output "HOST_SELF_TEST_EVIDENCE_CHECKS=$checks PASS (simulated evidence; no driver loaded)"
