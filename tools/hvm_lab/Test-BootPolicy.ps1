#requires -Version 5.1
# Pure policy tests: no administrator, BCD writes, tasks or restart required.
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'AmdLabBoot.psm1') -Force
& (Get-Module AmdLabBoot) {
    $checks = 0
    function Assert($Condition, $Name) {
        if (-not $Condition) { throw "FAILED: $Name" }
        $script:testCount++
    }
    $script:testCount = 0
    Assert (Test-AmdElementEqual @{present=$false;value=$null} @{present=$false;value=$null}) 'absent remains absent'
    Assert (-not (Test-AmdElementEqual @{present=$false;value=$null} @{present=$true;value=0})) 'absent is not Off'
    Assert (-not (Test-AmdElementEqual @{present=$true;value=0} @{present=$true;value=1})) 'external change detected'
    Assert (Test-AmdOwnedSequence @{present=$false;value=$null} 'normal' 'lab') 'empty sequence'
    Assert (Test-AmdOwnedSequence @{present=$true;value=@('lab')} 'normal' 'lab') 'repeated enter'
    Assert (Test-AmdOwnedSequence @{present=$true;value=@('normal')} 'normal' 'lab') 'repeated restore'
    Assert (-not (Test-AmdOwnedSequence @{present=$true;value=@('foreign')} 'normal' 'lab')) 'foreign pending boot'
    Assert (-not (Test-AmdOwnedSequence @{present=$true;value=@('lab','foreign')} 'normal' 'lab')) 'mixed sequence'
    # Mock only the provider boundary, leaving the production comparison code intact.
    function Assert-AmdOriginalUnchanged($Baseline, $Environment) {
        if ($Environment.machine -ne $Baseline.machine) { throw 'foreign baseline' }
    }
    $baseline = @{machine='test';loader='normal';hypervisor=$true;vbs=2;services=@(0);secureBoot=$true;bitLockerProtection='Off'}
    $owner = @{labLoader='lab'}
    $envData = @{machine='test';loader='lab';hypervisor=$false;vbs=0;services=@(0);secureBoot=$true;bitLockerProtection='Off'}
    Assert ((Get-AmdBootVerdict $baseline $owner $envData Lab) -eq 'LabHostReady') 'verified lab'
    $envData.hypervisor = $true
    Assert ((Get-AmdBootVerdict $baseline $owner $envData Lab) -eq 'VerificationFailed') 'hypervisor still running'
    $envData.loader = 'normal'; $envData.vbs = 2
    Assert ((Get-AmdBootVerdict $baseline $owner $envData Normal) -eq 'NormalRestored') 'verified restore'
    $envData.vbs = 0
    Assert ((Get-AmdBootVerdict $baseline $owner $envData Normal) -eq 'VerificationFailed') 'incomplete restore'
    "BOOT_POLICY_CHECKS=$script:testCount"
}
# Parse every shipped script using the actual Windows PowerShell parser.
Get-ChildItem $PSScriptRoot -File | Where-Object Extension -in '.ps1','.psm1' | ForEach-Object {
    $tokens=$null; $errors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($_.FullName,[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
'BOOT_POLICY_RESULT=SUCCESS'
