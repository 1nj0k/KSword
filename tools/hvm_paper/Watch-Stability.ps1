param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [string]$VMName='KSword-HVM-Target',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1,86400)][int]$Seconds=600,
    [ValidateRange(1,300)][int]$IntervalSeconds=30,
    [ValidateRange(1,64)][int]$ExpectedResidentProcessors=2
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$runId='stability-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$logPath=Join-Path $OutputDirectory ($runId+'.jsonl')
$watch=[Diagnostics.Stopwatch]::StartNew()
$sequence=0
$session=$null
$failures=0
try {
    $session=New-PSSession -VMName $VMName -Credential $Credential
    do {
        $start=[DateTime]::UtcNow.ToString('o')
        $sampleWatch=[Diagnostics.Stopwatch]::StartNew()
        $sample=[ordered]@{schemaVersion=2;runId=$runId;sequence=$sequence;startedUtc=$start;hostElapsedSeconds=$watch.Elapsed.TotalSeconds;kind='stability-sample';status='ok';counterScope='all-resident-dispatch-entries';requestedSeconds=$Seconds;expectedResidentProcessors=$ExpectedResidentProcessors}
        try {
            $sample.vm=Get-VM -Name $VMName | Select-Object Id,State,Uptime,MemoryAssigned,CPUUsage
            $sample.windows1=Invoke-Command -Session $session -ScriptBlock {
                $os=Get-CimInstance Win32_OperatingSystem
                $memory=Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object AvailableBytes,PoolNonpagedBytes,PoolPagedBytes,CommittedBytes
                $processes=@(Get-Process -Name vmware-vmx,services,wininit,lsass -ErrorAction SilentlyContinue | ForEach-Object {
                    [ordered]@{name=$_.ProcessName;pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o');cpuSeconds=$_.CPU;privateBytes=$_.PrivateMemorySize64;workingSetBytes=$_.WorkingSet64;handles=$_.HandleCount;threads=$_.Threads.Count}
                })
                $statusRaw=(& C:\ksword\hvm_ctl.exe --json status 2>&1 | Out-String)
                $statusExit=$LASTEXITCODE
                $pageRaw=(& C:\ksword\hvm_ctl.exe --json nested-page-query 2>&1 | Out-String)
                $pageExit=$LASTEXITCODE
                $metricsRaw=(& C:\ksword\hvm_ctl.exe --json metrics 2>&1 | Out-String)
                $metricsExit=$LASTEXITCODE
                $driverPath=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
                $driverSha256=(Get-FileHash -LiteralPath $driverPath).Hash
                $fs=[IO.File]::Open('C:\vmware\hltprobe.log',[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
                $serialLength=$fs.Length
                $null=$fs.Seek([Math]::Max(0,$serialLength-65536),[IO.SeekOrigin]::Begin)
                $reader=New-Object IO.StreamReader($fs)
                try {$serial=$reader.ReadToEnd()} finally {$reader.Dispose();$fs.Dispose()}
                [ordered]@{capturedUtc=[DateTime]::UtcNow.ToString('o');bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o');uptimeSeconds=([DateTime]::UtcNow-$os.LastBootUpTime.ToUniversalTime()).TotalSeconds;driverSha256=$driverSha256;memory=$memory;processes=$processes;hvmStatusExit=$statusExit;hvmStatusRaw=$statusRaw;pageStatusExit=$pageExit;pageStatusRaw=$pageRaw;metricsExit=$metricsExit;metricsRaw=$metricsRaw;serialLength=$serialLength;serialTail=$serial.Substring([Math]::Max(0,$serial.Length-6000))}
            }
        } catch {
            $failures++
            $sample.status='observer_error'
            $sample.error=$_.Exception.Message
        }
        $sampleWatch.Stop()
        $sample.captureDurationMs=$sampleWatch.Elapsed.TotalMilliseconds
        $sample.endedUtc=[DateTime]::UtcNow.ToString('o')
        $sample | ConvertTo-Json -Depth 14 -Compress | Add-Content -LiteralPath $logPath -Encoding UTF8
        Write-Output ('STABILITY_SAMPLE sequence={0} elapsed={1:N1}s status={2}' -f $sequence,$watch.Elapsed.TotalSeconds,$sample.status)
        $sequence++
        if($watch.Elapsed.TotalSeconds -ge $Seconds) {break}
        Start-Sleep -Seconds ([Math]::Min($IntervalSeconds,[Math]::Max(1,[int]($Seconds-$watch.Elapsed.TotalSeconds))))
    } while($true)
} finally {
    if($session) {Remove-PSSession $session}
    [ordered]@{runId=$runId;endedUtc=[DateTime]::UtcNow.ToString('o');durationSeconds=$watch.Elapsed.TotalSeconds;samples=$sequence;observerFailures=$failures;result='requires-analysis';logFile=[IO.Path]::GetFileName($logPath)} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory ($runId+'-summary.json')) -Encoding UTF8
}
