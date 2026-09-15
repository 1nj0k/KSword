param([string]$OutputDirectory='C:\ksword\paper',[ValidateSet("resident-nested-hidehv","stop")][string]$Command,[int]$Iteration=0,[bool]$WithoutGapObserver=$false)
$ErrorActionPreference='Stop'
if(@(Get-Process -Name vmware-vmx -ErrorAction SilentlyContinue).Count){throw 'Matched Windows A/B requires VMware absent in every block.'}
$driver=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
$driverHash=(Get-FileHash -LiteralPath $driver).Hash
$probe=Join-Path $OutputDirectory 'transition_probe.exe'
$probeHash=if($WithoutGapObserver){$null}else{(Get-FileHash -LiteralPath $probe).Hash}
function Save($Object,$Path) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Object | ConvertTo-Json -Depth 24))
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
function State {
    $os=Get-CimInstance Win32_OperatingSystem
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o');hvmRaw=(& C:\ksword\hvm_ctl.exe --json status | Out-String);pageRaw=(& C:\ksword\hvm_ctl.exe --json nested-page-query | Out-String);processes=@(Get-Process -Name vmware-vmx,services,wininit,lsass -ErrorAction SilentlyContinue | ForEach-Object {[ordered]@{name=$_.ProcessName;pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o');privateBytes=$_.PrivateMemorySize64}});memory=(Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object AvailableBytes,PoolNonpagedBytes,PoolPagedBytes,CommittedBytes)}
}
function Transition([string]$Command,[int]$Iteration) {
    $id='transition-'+$Command+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $path=Join-Path $OutputDirectory ($id+'.json')
    $condition=if($WithoutGapObserver){'windows1-no-vmware-no-busy-observers'}else{'windows1-no-vmware-with-two-busy-observers'}
    $record=[ordered]@{schemaVersion=1;kind='transition';runId=$id;command=$Command;iteration=$Iteration;condition=$condition;status='started';startedUtc=[DateTime]::UtcNow.ToString('o');driverSha256=$driverHash;probeSha256=$(if($WithoutGapObserver){$null}else{$probeHash});before=(State)}
    Save $record $path
    if($WithoutGapObserver) {
        $info=[Diagnostics.ProcessStartInfo]::new()
        $info.FileName='C:\ksword\hvm_ctl.exe';$info.Arguments='--json '+$Command
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $startQpc=[Diagnostics.Stopwatch]::GetTimestamp()
        $process=[Diagnostics.Process]::Start($info)
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $completed=$process.WaitForExit(30000)
        $endQpc=[Diagnostics.Stopwatch]::GetTimestamp()
        $code=if($completed){$process.ExitCode}else{258}
        if($completed){$record.cliStdout=$stdout.GetAwaiter().GetResult();$record.cliStderr=$stderr.GetAwaiter().GetResult()}
        $record.commandPid=$process.Id
        $record.probeRaw=([ordered]@{schemaVersion=1;kind='command-wall-observer';command=$Command;commandExitCode=$code;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;commandStartQpc=$startQpc;commandEndQpc=$endQpc;processors=@()} | ConvertTo-Json -Compress)
        $record.probeExitCode=if($code -eq 0){0}else{1}
    }else{
        $record.probeRaw=(& $probe $Command 2>&1 | Out-String)
        $record.probeExitCode=$LASTEXITCODE
    }
    $record.metricsRaw=(& C:\ksword\hvm_ctl.exe --json metrics 2>&1 | Out-String)
    $record.metricsExitCode=$LASTEXITCODE
    Save $record $path
    $record.after=State
    $record.eventsRaw=(& C:\ksword\hvm_ctl.exe --json events 0 256 2>&1 | Out-String)
    $record.endedUtc=[DateTime]::UtcNow.ToString('o')
    $record.status='recorded_requires_analysis';Save $record $path
    $after=$record.after.hvmRaw | ConvertFrom-Json
    $wanted=if($Command -eq 'stop'){0}else{2}
    if($record.probeExitCode -ne 0 -or $after.residentProcessorCount -ne $wanted -or $after.stateNames -contains 'ROLLBACK_REQUIRED' -or $after.stateNames -contains 'FAULTED') {throw 'Transition did not complete cleanly; stop the A/B sequence.'}
    Write-Output ($id+' resident='+$after.residentProcessorCount)
}
Transition $Command $Iteration
