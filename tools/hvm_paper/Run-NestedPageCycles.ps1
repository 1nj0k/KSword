param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target',
    [ValidateRange(1,50)][int]$Repetitions=10,
    [ValidateSet('idle','guest-cpu-load')][string]$Condition='idle',
    [string]$GuestPhysicalPage='7000000',
    [switch]$RejectionsOnly
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$session=New-PSSession -VMName $VMName -Credential $Credential
function Save-Record($Record,$Path) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Record | ConvertTo-Json -Depth 24))
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}
try {
    # Functions stay in the one remoting session. No VMware management operation
    # is performed in the measured control interval.
    Invoke-Command -Session $session -ScriptBlock {
        function global:Paper-Control([string[]]$Arguments) {
            $utc=[DateTime]::UtcNow.ToString('o')
            $qpc=[Diagnostics.Stopwatch]::GetTimestamp()
            $raw=(& C:\ksword\hvm_ctl.exe --json @Arguments 2>&1 | Out-String)
            $code=$LASTEXITCODE
            $end=[Diagnostics.Stopwatch]::GetTimestamp()
            $parsed=$null;try {$parsed=$raw | ConvertFrom-Json} catch {}
            [ordered]@{arguments=$Arguments;startedUtc=$utc;endedUtc=[DateTime]::UtcNow.ToString('o');startQpc=$qpc;endQpc=$end;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;commandElapsedMs=1000.0*($end-$qpc)/[Diagnostics.Stopwatch]::Frequency;exitCode=$code;raw=$raw;parsed=$parsed}
        }
        function global:Paper-Serial {
            $fs=[IO.File]::Open('C:\vmware\hltprobe.log',[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
            $reader=[IO.StreamReader]::new($fs)
            try {$reader.ReadToEnd()} finally {$reader.Dispose();$fs.Dispose()}
        }
        function global:Paper-State {
            $os=Get-CimInstance Win32_OperatingSystem
            [ordered]@{capturedUtc=[DateTime]::UtcNow.ToString('o');bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o');processes=@(Get-Process -Name vmware-vmx,services,wininit,lsass -ErrorAction SilentlyContinue | ForEach-Object {[ordered]@{name=$_.ProcessName;pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o');privateBytes=$_.PrivateMemorySize64}});status=(Paper-Control @('status'));page=(Paper-Control @('nested-page-query'));serialOffset=(Paper-Serial).Length}
        }
    }
    $initial=Invoke-Command -Session $session -ScriptBlock {Paper-State}
    $page=$initial.page.parsed
    if($page.active -ne 0 -or $page.retired -ne 0 -or $page.residentProcessors -ne 2 -or @($page.roots).Count -ne 1) {
        throw 'Require two resident CPUs, one known nested EPT root, and no existing mapping.'
    }
    $root=[string]$page.roots[0]
    $cases=if($RejectionsOnly){@(
        @{name='gpa-above-52-bits';arguments=@('nested-page-map',$root,'10000000000000','D1')},
        @{name='unaligned-gpa';arguments=@('nested-page-map',$root,'7000001','D1')},
        @{name='unknown-ept-root';arguments=@('nested-page-map','DEAD005E',$GuestPhysicalPage,'D1')}
    )}else{@(1..$Repetitions | ForEach-Object {@{name='remap-restore';iteration=$_;fill=('{0:X2}' -f (0xC0+$_))}})}
    foreach($case in $cases) {
        $id='nested-page-'+$Condition+'-'+$case.name+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
        $path=Join-Path $OutputDirectory ($id+'.json')
        $record=[ordered]@{schemaVersion=1;runId=$id;kind='nested-page';condition=$Condition;case=$case.name;iteration=$case.iteration;targetRole='vmware-tinycore';gpa=$GuestPhysicalPage;ept12Root=$root;fill=$case.fill;startedUtc=[DateTime]::UtcNow.ToString('o');status='started';before=(Invoke-Command -Session $session -ScriptBlock {Paper-State})}
        Save-Record $record $path
        try {
            if($RejectionsOnly) {
                $record.action=Invoke-Command -Session $session -ArgumentList (,$case.arguments) -ScriptBlock {param($a) Paper-Control $a}
            }else{
                $record.map=Invoke-Command -Session $session -ArgumentList $root,$GuestPhysicalPage,$case.fill -ScriptBlock {param($r,$g,$f) Paper-Control @('nested-page-map',$r,$g,$f)}
                Save-Record $record $path
                if($record.map.exitCode -ne 0 -or $record.map.parsed.active -ne 1) {throw 'Mapping was not accepted.'}
                Start-Sleep -Seconds 3
                $record.mapped=Invoke-Command -Session $session -ScriptBlock {Paper-State}
                $record.remove=Invoke-Command -Session $session -ScriptBlock {Paper-Control @('nested-page-remove')}
                Save-Record $record $path
                if($record.remove.exitCode -ne 0) {throw 'Restore failed; retained resources need investigation.'}
                Start-Sleep -Seconds 3
            }
            $record.after=Invoke-Command -Session $session -ScriptBlock {Paper-State}
            $record.serial=Invoke-Command -Session $session -ArgumentList $record.before.serialOffset -ScriptBlock {param($offset) $all=Paper-Serial; if($all.Length -lt $offset){throw 'Serial log truncated'};$all.Substring($offset)}
            $record.status='recorded_requires_analysis'
        }catch{
            $record.status='error';$record.error=$_.Exception.Message
            $record.cleanupAttempt=Invoke-Command -Session $session -ScriptBlock {Paper-Control @('nested-page-remove')} -ErrorAction Continue
        }
        $record.endedUtc=[DateTime]::UtcNow.ToString('o')
        Save-Record $record $path
        Write-Output ($id+' '+$record.status)
        if($record.status -eq 'error'){break}
    }
}finally {Remove-PSSession $session}
