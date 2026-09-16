# Execute in Windows 1. Keep workload timing separate from diagnostic queries.
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Benchmark='C:\ksword\paper\microbench.exe',
    [ValidateRange(1,20)][int]$Repetitions=5,
    [ValidateRange(1,64)][int]$ExpectedResidentProcessors=4
)
$ErrorActionPreference='Stop'
$ctl='C:\ksword\hvm_ctl.exe'
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
function Save($Record,$Path) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Record | ConvertTo-Json -Depth 30))
    $stream=[IO.File]::Open($Path,'Create','Write','Read')
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}
function Control([string[]]$Arguments) {
    $raw=(& $ctl --json @Arguments 2>&1 | Out-String);$code=$LASTEXITCODE
    $parsed=$raw | ConvertFrom-Json
    if($code -ne 0){throw ('Control failed: '+($Arguments -join ' ')+' '+$raw)}
    [ordered]@{arguments=$Arguments;exitCode=$code;raw=$raw;parsed=$parsed}
}
function State {
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');
        hvm=(Control @('status'));metrics=(Control @('metrics'))}
}
if(@(Get-Process vmware-vmx,vmwp -ErrorAction SilentlyContinue).Count){throw 'This experiment requires all descendant VMM processes absent.'}
$initial=State
if($initial.hvm.parsed.residentProcessorCount -ne 0){throw 'Start with residency stopped.'}
$driverPath=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
$identity=[ordered]@{driverSha256=(Get-FileHash $driverPath).Hash;controlSha256=(Get-FileHash $ctl).Hash;
    benchmarkSha256=(Get-FileHash $Benchmark).Hash;sourceScriptSha256=(Get-FileHash $PSCommandPath).Hash;bootUtc=$initial.bootUtc}
if($initial.hvm.parsed.featureNames -notcontains 'EPTP_SWITCH_ARMED') {
    if($initial.hvm.parsed.stateNames -contains 'RESOURCES_READY'){$null=Control @('teardown')}
    $null=Control @('prepare-eptpsw');$null=Control @('self-test')
}
$ready=(Control @('status')).parsed
if($ready.preparedProcessorCount -ne $ExpectedResidentProcessors -or $ready.selfTestPassedProcessorCount -ne $ExpectedResidentProcessors -or
   $ready.featureNames -notcontains 'EPTP_SWITCH_ARMED'){throw 'Prepared processor/backend mismatch.'}
$modes=@(
    @{name='off';command=@();extraVmreads=0},
    @{name='resident';command=@('resident');extraVmreads=0},
    @{name='resident-vmread256';command=@('resident-vmreadbench','256');extraVmreads=256},
    @{name='resident-vmread512';command=@('resident-vmreadbench','512');extraVmreads=512},
    @{name='resident-nested-fullsnapshot';command=@('resident-nested-fullsnapshot');extraVmreads=0},
    @{name='resident-nested-hidehv';command=@('resident-nested-hidehv');extraVmreads=0}
)
$random=[Random]::new(20260916)
try {
    # A full warmup round precedes counterbalanced measured rounds.
    foreach($round in 0..$Repetitions) {
        $order=@($modes)
        for($i=$order.Count-1;$i -gt 0;$i--){$j=$random.Next($i+1);$tmp=$order[$i];$order[$i]=$order[$j];$order[$j]=$tmp}
        foreach($mode in $order) {
            $attempt=[ordered]@{kind='mode-setup';round=$round;condition=$mode.name;status='started';utc=[DateTime]::UtcNow.ToString('o')}
            $attemptPath=Join-Path $OutputDirectory ('setup-'+$round+'-'+$mode.name+'.json');Save $attempt $attemptPath
            $null=Control @('stop')
            if($mode.command.Count){$null=Control $mode.command}
            $attempt.status='ready';Save $attempt $attemptPath
            foreach($workload in @('cpuid','ping')) {
                $r=[ordered]@{schemaVersion=1;kind='exit-attribution';runId=('exit-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'));
                    identity=$identity;condition=$mode.name;extraVmreadsPerExit=$mode.extraVmreads;round=$round;warmup=($round -eq 0);
                    workload=$workload;expectedResidentProcessors=$ExpectedResidentProcessors;status='started';before=(State)}
                $path=Join-Path $OutputDirectory ($r.runId+'.json');Save $r $path
                $wanted=if($mode.command.Count){$ExpectedResidentProcessors}else{0}
                if($r.before.hvm.parsed.residentProcessorCount -ne $wanted -or $r.before.bootUtc -ne $identity.bootUtc){throw 'Experiment state changed.'}
                $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$Benchmark;$info.Arguments=$workload
                $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
                $r.workloadBeginQpc=[Diagnostics.Stopwatch]::GetTimestamp()
                $process=[Diagnostics.Process]::Start($info);$r.pid=$process.Id
                $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
                if(!$process.WaitForExit(30000)){$process.Kill();$process.WaitForExit();$r.status='timeout'}else{$r.status=if($process.ExitCode -eq 0){'ok'}else{'error'}}
                $r.workloadEndQpc=[Diagnostics.Stopwatch]::GetTimestamp();$r.qpcFrequency=[Diagnostics.Stopwatch]::Frequency
                $r.exitCode=$process.ExitCode;$r.stdout=$stdout.GetAwaiter().GetResult();$r.stderr=$stderr.GetAwaiter().GetResult();$process.Dispose()
                $r.after=State
                if($r.after.bootUtc -ne $identity.bootUtc -or $r.after.hvm.parsed.residentProcessorCount -ne $wanted){$r.status='state_changed'}
                Save $r $path
                Write-Output ($r.runId+' '+$mode.name+' '+$workload+' '+$r.status)
                if($r.status -ne 'ok'){throw 'Attribution workload failed; retain raw record.'}
            }
        }
    }
} finally {
    $cleanup=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');stop=(Control @('stop'));final=(State)}
    Save $cleanup (Join-Path $OutputDirectory 'cleanup.json')
}
