param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target'
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$session=New-PSSession -VMName $VMName -Credential $Credential
$record=[ordered]@{schemaVersion=1;kind='nested-write-isolation';runId=('write-isolation-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'));startedUtc=[DateTime]::UtcNow.ToString('o');status='started';gpa='0x07000000';steps=@()}
$path=Join-Path $OutputDirectory ($record.runId+'.json')
function Save {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($record | ConvertTo-Json -Depth 24))
    $stream=[IO.File]::Open($path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}
function Control([string[]]$Arguments) {
    $result=Invoke-Command -Session $session -ArgumentList (,$Arguments) -ScriptBlock {
        param($a)
        $start=[DateTime]::UtcNow.ToString('o');$watch=[Diagnostics.Stopwatch]::StartNew()
        $raw=(& C:\ksword\hvm_ctl.exe --json @a 2>&1 | Out-String);$code=$LASTEXITCODE
        $watch.Stop();$parsed=$null;try {$parsed=$raw | ConvertFrom-Json}catch{}
        [ordered]@{kind='hvm-cli';arguments=$a;startedUtc=$start;endedUtc=[DateTime]::UtcNow.ToString('o');commandElapsedMs=$watch.Elapsed.TotalMilliseconds;exitCode=$code;raw=$raw;parsed=$parsed}
    }
    $record.steps+=@($result);Save
    $result
}
function Guest([string]$Command) {
    $start=[DateTime]::UtcNow.ToString('o')
    $output=(& (Join-Path $repo 'docs/next/logs/Invoke-TinyCoreCommand.ps1') -VMName $VMName -Command $Command -ShotName ($record.runId+'.png') | Out-String)
    $record.steps+=@([ordered]@{kind='guest-vnc-instrumentation';command=$Command;startedUtc=$start;endedUtc=[DateTime]::UtcNow.ToString('o');raw=$output})
    Save
}
try {
    $record.before=Invoke-Command -Session $session -FilePath (Join-Path $PSScriptRoot 'Capture-Windows.ps1') -ArgumentList $true
    Save
    $page=(Control @('nested-page-query')).parsed
    if($page.active -ne 0 -or $page.retired -ne 0 -or @($page.roots).Count -ne 1){throw 'Expected an empty mapping slot and one nested EPT root.'}
    Guest '(echo paper-isolation-before; cat /proc/sys/kernel/random/boot_id; cat /proc/uptime; dd if=/dev/mem bs=4 skip=29360128 count=1 2>/dev/null | od -An -tx4) >/dev/ttyS0'
    $map=Control @('nested-page-map',[string]$page.roots[0],'7000000','D1')
    if($map.exitCode -ne 0 -or $map.parsed.active -ne 1){throw 'Map failed.'}
    Guest '(echo paper-isolation-mapped; cat /proc/uptime; dd if=/dev/mem bs=4 skip=29360128 count=1 2>/dev/null | od -An -tx4; printf "\262\262\262\262" | dd of=/dev/mem bs=4 seek=29360128 count=1; echo paper-isolation-written; cat /proc/uptime; dd if=/dev/mem bs=4 skip=29360128 count=1 2>/dev/null | od -An -tx4) >/dev/ttyS0 2>&1'
    $null=Control @('nested-page-query')
    $remove=Control @('nested-page-remove')
    if($remove.exitCode -ne 0){throw 'Restore failed.'}
    Guest '(echo paper-isolation-restored; cat /proc/sys/kernel/random/boot_id; cat /proc/uptime; dd if=/dev/mem bs=4 skip=29360128 count=1 2>/dev/null | od -An -tx4) >/dev/ttyS0'
    $record.after=Invoke-Command -Session $session -FilePath (Join-Path $PSScriptRoot 'Capture-Windows.ps1') -ArgumentList $true
    $record.status='recorded_requires_analysis'
}catch{
    $record.status='error';$record.error=$_.Exception.Message
    $null=Control @('nested-page-remove')
}finally{
    $record.endedUtc=[DateTime]::UtcNow.ToString('o');Save
    Remove-PSSession $session
}
Write-Output ($record.runId+' '+$record.status)
