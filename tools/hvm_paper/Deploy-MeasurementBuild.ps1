param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target',
    [string]$DriverPath='Ksword5.1/x64/Release/KswordARK.sys',
    [string]$ControlPath='tools/hvm_ctl/hvm_ctl.exe'
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$record=[ordered]@{schemaVersion=1;kind='measurement-deployment';startedUtc=[DateTime]::UtcNow.ToString('o');status='started';driverSha256=(Get-FileHash $DriverPath).Hash;controlSha256=(Get-FileHash $ControlPath).Hash;sourceBase=(git rev-parse HEAD);signingVerification='See the separate build/signing record; deployment alone cannot infer signing provenance.'}
$path=Join-Path $OutputDirectory ('deployment-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'.json')
function Save { $record | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding utf8 }
$session=$null
try {
    Save
    $session=New-PSSession -VMName $VMName -Credential $Credential
    $record.preflight=Invoke-Command -Session $session -ScriptBlock {
        if((Get-Service KswordARK).Status -ne 'Stopped'){throw 'Reboot HVM-target before deployment; driver must be stopped.'}
        if(@(Get-Process vmware-vmx -ErrorAction SilentlyContinue).Count){throw 'VMware must be stopped before deployment.'}
        $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
        if($image -notmatch '^[A-Za-z]:\\'){throw 'Resolve the service image path before deployment.'}
        [ordered]@{bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');imagePath=$image;oldDriverSha256=(Get-FileHash -LiteralPath $image).Hash}
    }
    Save
    Copy-Item -ToSession $session -LiteralPath $DriverPath -Destination $record.preflight.imagePath -Force
    Copy-Item -ToSession $session -LiteralPath $ControlPath -Destination 'C:\ksword\hvm_ctl.exe' -Force
    $record.result=Invoke-Command -Session $session -ArgumentList $record.preflight.imagePath,$record.driverSha256,$record.controlSha256 -ScriptBlock {
        param($image,$expectedDriver,$expectedControl)
        $driverHash=(Get-FileHash -LiteralPath $image).Hash
        $ctlHash=(Get-FileHash C:\ksword\hvm_ctl.exe).Hash
        if($driverHash -ne $expectedDriver -or $ctlHash -ne $expectedControl){throw 'Deployed bytes differ from the local artifacts.'}
        $start=@(& sc.exe start KswordARK 2>&1);$code=$LASTEXITCODE
        if($code -ne 0){throw ('Driver start failed: '+($start -join ' '))}
        $raw=(& C:\ksword\hvm_ctl.exe --json metrics 2>&1 | Out-String);$metricsExit=$LASTEXITCODE
        if($metricsExit -ne 0){throw ('Metrics query failed: '+$raw)}
        [ordered]@{utc=[DateTime]::UtcNow.ToString('o');driverSha256=$driverHash;controlSha256=$ctlHash;service=[string](Get-Service KswordARK).Status;metricsRaw=$raw;metricsExit=$metricsExit;statusRaw=(& C:\ksword\hvm_ctl.exe --json status | Out-String)}
    }
    $record.status='deployed_query_verified'
}catch{
    $record.status='error';$record.error=$_.Exception.Message
    throw
}finally{
    $record.endedUtc=[DateTime]::UtcNow.ToString('o');Save
    if($session){Remove-PSSession $session}
}
[ordered]@{status=$record.status;path=$path;driverSha256=$record.driverSha256} | ConvertTo-Json -Compress
