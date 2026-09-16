param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target',
    [string]$GuestDirectory='C:\ksword\paper',
    [int]$Repetitions=7,
    [int]$TransitionPairs=7,
    [bool]$WithoutGapObserver=$true,
    [ValidateRange(1,64)][int]$ExpectedResidentProcessors=2
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
$session=New-PSSession -VMName $VMName -Credential $Credential
$sessionStarted=[DateTime]::UtcNow
try {
    $setup=Invoke-Command -Session $session -ArgumentList $ExpectedResidentProcessors -ScriptBlock {
        param($expected)
        if(@(Get-Process -Name vmware-vmx -ErrorAction SilentlyContinue).Count){throw 'VMware must be absent in all matched blocks.'}
        $initial=(& C:\ksword\hvm_ctl.exe --json status | Out-String)
        $s=$initial | ConvertFrom-Json
        if($s.residentProcessorCount -ne 0){throw 'A/B requires stopped residency initially.'}
        if($s.stateNames -contains 'FAULTED' -or $s.stateNames -contains 'ROLLBACK_REQUIRED'){throw 'Unhealthy HVM state.'}
        $commands=@()
        if($s.stateNames -notcontains 'RESOURCES_READY') {
            foreach($c in @('prepare-eptpsw','self-test')){
                $raw=(& C:\ksword\hvm_ctl.exe --json $c | Out-String);$code=$LASTEXITCODE
                $commands+=@([ordered]@{command=$c;utc=[DateTime]::UtcNow.ToString('o');exitCode=$code;raw=$raw})
                if($code -ne 0){throw "$c failed"}
            }
        }
        $final=(& C:\ksword\hvm_ctl.exe --json status | Out-String);$s=$final | ConvertFrom-Json
        if($s.featureNames -notcontains 'EPTP_SWITCH_ARMED' -or $s.selfTestPassedProcessorCount -ne $expected -or $s.preparedProcessorCount -ne $expected){throw "Require EPTP switch and exactly $expected prepared/self-tested processors."}
        [ordered]@{kind='ab-setup';utc=[DateTime]::UtcNow.ToString('o');initialRaw=$initial;commands=$commands;finalRaw=$final;prepareMetricsRaw=(& C:\ksword\hvm_ctl.exe --json metrics | Out-String)}
    }
    $setup | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $OutputDirectory ('ab-preflight-'+$sessionStarted.ToString('yyyyMMddTHHmmssZ')+'.json')) -Encoding utf8
    $bench=Join-Path $PSScriptRoot 'Run-WindowsBenchmarks.ps1'
    $transition=Join-Path $PSScriptRoot 'Run-Transition.ps1'
    Invoke-Command -Session $session -FilePath $bench -ArgumentList 'off-pre-no-vmware',$GuestDirectory,$Repetitions,$ExpectedResidentProcessors
    Invoke-Command -Session $session -FilePath $transition -ArgumentList $GuestDirectory,'resident-nested-hidehv',0,$WithoutGapObserver,$ExpectedResidentProcessors
    Invoke-Command -Session $session -FilePath $bench -ArgumentList 'on-no-vmware',$GuestDirectory,$Repetitions,$ExpectedResidentProcessors
    Invoke-Command -Session $session -FilePath $transition -ArgumentList $GuestDirectory,'stop',0,$WithoutGapObserver,$ExpectedResidentProcessors
    Invoke-Command -Session $session -FilePath $bench -ArgumentList 'off-post-no-vmware',$GuestDirectory,$Repetitions,$ExpectedResidentProcessors
    for($i=1;$i -le $TransitionPairs;$i++) {
        Invoke-Command -Session $session -FilePath $transition -ArgumentList $GuestDirectory,'resident-nested-hidehv',$i,$WithoutGapObserver,$ExpectedResidentProcessors
        Invoke-Command -Session $session -FilePath $transition -ArgumentList $GuestDirectory,'stop',$i,$WithoutGapObserver,$ExpectedResidentProcessors
    }
}finally {
    $files=Invoke-Command -Session $session -ArgumentList $GuestDirectory,$sessionStarted -ScriptBlock {param($dir,$since) Get-ChildItem -LiteralPath $dir -Filter '*.json' | Where-Object {($_.Name -like 'windows1-*' -or $_.Name -like 'transition-*') -and $_.LastWriteTimeUtc -ge $since} | Select-Object -ExpandProperty FullName}
    foreach($file in $files){Copy-Item -FromSession $session -LiteralPath $file -Destination $OutputDirectory -Force}
    Remove-PSSession $session
}
