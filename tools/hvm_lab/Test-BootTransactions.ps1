# Simulated BCD provider and task scheduler; runs the production transaction body.
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'AmdLabBoot.psm1') -Force
& (Get-Module AmdLabBoot) {
    $script:Root = Join-Path $env:TEMP ('KSword-Boot-Test-' + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $script:Root | Out-Null
    $script:original = '{11111111-1111-1111-1111-111111111111}'
    $script:labId = '{22222222-2222-2222-2222-222222222222}'
    $script:sequence = @{present=$false;value=$null}
    $script:default = @{present=$true;value=$script:original}
    $script:writes = @{}
    $script:failWrite = $false; $script:failTask = $false; $script:deny = $false
    $script:environment = @{machine='test';systemDrive='C:';loader=$script:original;bootId='boot1';
        defaultLoader=$script:default;bootSequence=$script:sequence;hypervisorLaunch=@{present=$false;value=$null};
        vsmLaunch=@{present=$true;value=1};hypervisor=$true;vbs=2;services=@(1,2);secureBoot=$true;bitLockerProtection='Off'}
    function Assert-AmdLabAdmin { if ($script:deny) { throw 'denied' } }
    function Initialize-AmdLabDirectory { }
    function Get-AmdBootEnvironment {
        $script:environment.bootSequence=$script:sequence
        $script:environment.defaultLoader=$script:default
        return $script:environment
    }
    function Open-AmdBcdStore { return 'mock-store' }
    function Open-AmdBcdObject($Store,$Id) {
        $object = [pscustomobject]@{Id=$Id}
        $object | Add-Member ScriptMethod SetStringElement { param($Type,$Value) @{ReturnValue=$true} }
        $object | Add-Member ScriptMethod SetIntegerElement {
            param($Type,$Value)
            if ($script:failWrite -and $Type -eq $script:VsmType) { return @{ReturnValue=$false} }
            $script:writes["$($this.Id):$Type"]=$Value
            return @{ReturnValue=$true}
        }
        $object | Add-Member ScriptMethod SetObjectListElement {
            param($Type,$Value)
            $script:sequence=@{present=$true;value=@($Value)}
            return @{ReturnValue=$true}
        }
        return $object
    }
    function Get-AmdBcdElement($Object,$Type) {
        if ($Type -eq $script:SequenceType) { return $script:sequence }
        if ($Object.Id -eq $script:original) {
            if ($Type -eq $script:HypervisorType) { return @{present=$false;value=$null} }
            return @{present=$true;value=1}
        }
        $key="$($Object.Id):$Type"
        return @{present=$script:writes.ContainsKey($key);value=$script:writes[$key]}
    }
    function Register-AmdBootVerifier { if ($script:failTask) { throw 'scheduler unavailable' } }
    function Assert($Condition,$Name) { if (-not $Condition) { throw "FAIL: $Name" }; $script:checks++ }
    function Refused([scriptblock]$Action) { try { & $Action | Out-Null; return $false } catch { return $true } }
    $script:checks=0
    Write-AmdLabJson baseline.json $script:environment
    Write-AmdLabJson owner.json @{created=$true;original=$script:original;labLoader=$script:labId}
    $baselineHash=(Get-FileHash (Join-Path $script:Root 'baseline.json')).Hash
    foreach ($mode in @('Lab','Lab','Normal','Normal')) {
        $result=Invoke-AmdLabBoot -Mode $mode -NoRestart
        Assert ($result.status -eq 'PendingReboot') "repeat transaction $mode"
        Assert ($script:default.value -eq $script:original) 'default preserved'
    }
    Assert ((Get-FileHash (Join-Path $script:Root 'baseline.json')).Hash -eq $baselineHash) 'first baseline immutable'
    Assert (@($script:writes.Keys | Where-Object { $_.StartsWith($script:original) }).Count -eq 0) 'daily loader never written'
    $pending = Read-AmdLabJson pending.json
    $pending.completed = $true
    Write-AmdLabJson pending.json $pending
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'matching GUID without live ownership refused'
    $pending.completed = $false
    $pending.sourceBootId = 'old-boot'
    Write-AmdLabJson pending.json $pending
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'stale boot journal refused'
    $script:sequence=@{present=$true;value=@('foreign')}
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'foreign boot sequence'
    $script:sequence=@{present=$false;value=$null}; $script:failWrite=$true
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'partial write refused'
    Assert (-not $script:sequence.present) 'partial write never scheduled'
    $script:failWrite=$false; $script:failTask=$true
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'task failure refused'
    Assert (-not $script:sequence.present) 'task failure never scheduled'
    $script:failTask=$false; $script:deny=$true
    Assert (Refused { Invoke-AmdLabBoot -Mode Normal -NoRestart }) 'permission denied'
    $script:deny=$false; $script:default=@{present=$true;value='external'}
    Assert (Refused { Invoke-AmdLabBoot -Mode Normal -NoRestart }) 'external default preserved'
    $script:default=@{present=$true;value=$script:original}
    $script:environment.bitLockerProtection='On'
    Assert (Refused { Invoke-AmdLabBoot -Mode Lab -NoRestart }) 'active encryption protection refused'
    $script:environment.bitLockerProtection='Off'
    # Missing baseline check uses a new empty test directory; never deletes system state.
    $script:Root=Join-Path $script:Root 'missing'
    New-Item -ItemType Directory -Path $script:Root | Out-Null
    Assert (Refused { Invoke-AmdLabBoot -Mode Normal -NoRestart }) 'missing backup refused'
    "BOOT_TRANSACTION_CHECKS=$script:checks RESULT=PASS (simulated provider only)"
}
