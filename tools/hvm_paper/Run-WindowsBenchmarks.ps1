param([string]$Mode,[string]$OutputDirectory='C:\ksword\paper', [int]$Repetitions=7,[ValidateRange(1,64)][int]$ExpectedResidentProcessors=2)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$binary=Join-Path $OutputDirectory 'microbench.exe'
$binaryHash=(Get-FileHash -LiteralPath $binary).Hash
function Write-DurableJson($Object,[string]$Path) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Object | ConvertTo-Json -Depth 16))
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}
function Read-State {
    $os=Get-CimInstance Win32_OperatingSystem
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o');hvmRaw=(& C:\ksword\hvm_ctl.exe --json status 2>&1 | Out-String);processes=@(Get-Process -Name vmware-vmx,services,wininit,lsass -ErrorAction SilentlyContinue | ForEach-Object {[ordered]@{name=$_.ProcessName;pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o');cpuSeconds=$_.CPU;privateBytes=$_.PrivateMemorySize64}})}
}
$random=[Random]::new(20260915)
foreach($iteration in 0..$Repetitions) {
    $workloads=@('cpu','cpuid','memory','latency','net','ping','disk')
    for($i=$workloads.Count-1;$i -gt 0;$i--) {$j=$random.Next($i+1);$tmp=$workloads[$i];$workloads[$i]=$workloads[$j];$workloads[$j]=$tmp}
    foreach($workload in $workloads) {
        $runId='windows1-'+$Mode+'-'+$workload+'-'+$iteration.ToString('D2')+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
        $path=Join-Path $OutputDirectory ($runId+'.json')
        $record=[ordered]@{schemaVersion=1;runId=$runId;role='windows1';configuration=$Mode;workload=$workload;iteration=$iteration;warmup=($iteration -eq 0);status='started';startedUtc=[DateTime]::UtcNow.ToString('o');binarySha256=$binaryHash;before=(Read-State)}
        $record.expectedResidentProcessors=$ExpectedResidentProcessors
        Write-DurableJson $record $path
        $info=[Diagnostics.ProcessStartInfo]::new()
        $info.FileName=$binary
        $scratch=Join-Path $OutputDirectory ($runId+'.scratch')
        $info.Arguments=$workload
        if($workload -eq 'disk') {$info.Arguments+=' "'+$scratch+'"'}
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $watch=[Diagnostics.Stopwatch]::StartNew()
        try {
            $process=[Diagnostics.Process]::Start($info)
            $record.benchmarkPid=$process.Id
            $record.benchmarkCreatedUtc=$process.StartTime.ToUniversalTime().ToString('o')
            $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
            if(!$process.WaitForExit(180000)) {$process.Kill();$process.WaitForExit();$record.status='timeout'}
            else {$record.status=$(if($process.ExitCode -eq 0){'ok'}else{'error'})}
            $record.exitCode=$process.ExitCode
            $record.stdout=$stdout.GetAwaiter().GetResult()
            $record.stderr=$stderr.GetAwaiter().GetResult()
        } catch {$record.status='observer_error';$record.error=$_.Exception.Message}
        $watch.Stop()
        $record.commandElapsedMs=$watch.Elapsed.TotalMilliseconds
        $record.endedUtc=[DateTime]::UtcNow.ToString('o')
        $record.after=Read-State
        $record.scratchRemains=Test-Path -LiteralPath $scratch
        Write-DurableJson $record $path
        Write-Output ($runId+' '+$record.status)
    }
}
