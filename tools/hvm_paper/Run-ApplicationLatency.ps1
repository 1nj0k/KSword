# Execute inside the same Windows 1 boot as attribution, with descendants absent.
param([Parameter(Mandatory)][string]$OutputDirectory,[ValidateRange(1,20)][int]$Repetitions=5)
$ErrorActionPreference='Stop'
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$ctl='C:\ksword\hvm_ctl.exe';$app='C:\ksword\paper\application_latency.exe'
function Control([string]$Command){$raw=(& $ctl --json $Command|Out-String);if($LASTEXITCODE -ne 0){throw $raw};$raw}
function State {
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');
        status=(Control 'status'|ConvertFrom-Json);metrics=(Control 'metrics'|ConvertFrom-Json);
        processes=@(Get-Process services,wininit,lsass | ForEach-Object {[ordered]@{name=$_.ProcessName;pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o')}})}
}
if(@(Get-Process vmware-vmx,vmwp -ErrorAction SilentlyContinue).Count){throw 'Descendants must be stopped for the insertion experiment.'}
$identity=[ordered]@{driverSha256=(Get-FileHash C:\ksword\KswordARK.sys).Hash;controlSha256=(Get-FileHash $ctl).Hash;
    applicationSha256=(Get-FileHash $app).Hash;scriptSha256=(Get-FileHash $PSCommandPath).Hash}
try {
    foreach($round in 0..$Repetitions) {
        $null=Control 'stop'
        # Sham/off, insertion, sham/on, removal: all samples retained, warmup labelled.
        foreach($condition in @(@{name='sham-off';command='status'},@{name='insert';command='resident-nested-hidehv'},
                                @{name='sham-on';command='status'},@{name='remove';command='stop'})) {
            $id='latency-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
            $record=[ordered]@{schemaVersion=1;kind='application-latency';runId=$id;round=$round;warmup=($round -eq 0);
                condition=$condition.name;command=$condition.command;identity=$identity;before=(State);status='started'}
            $path=Join-Path $OutputDirectory ($id+'.json')
            $record|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $path -Encoding UTF8
            $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$app
            $info.Arguments=$condition.command+' "'+(Join-Path $OutputDirectory ($id+'-control.json'))+'"'
            $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
            $process=[Diagnostics.Process]::Start($info);$record.pid=$process.Id;$record.createdUtc=$process.StartTime.ToUniversalTime().ToString('o')
            $output=$process.StandardOutput.ReadToEndAsync();$errorText=$process.StandardError.ReadToEndAsync()
            if(!$process.WaitForExit(60000)){throw 'Observer timeout: inspect the process and control request; neither was killed.'}
            $record.exitCode=$process.ExitCode
            [IO.File]::WriteAllText((Join-Path $OutputDirectory ($id+'.jsonl')),$output.GetAwaiter().GetResult())
            $record.stderr=$errorText.GetAwaiter().GetResult();$process.Dispose();$record.after=State
            $wanted=if($condition.name -in @('insert','sham-on')){4}else{0}
            $record.status=if($record.exitCode -eq 0 -and $record.after.status.residentProcessorCount -eq $wanted -and
                $record.before.bootUtc -eq $record.after.bootUtc){'ok'}else{'error'}
            $record|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $path -Encoding UTF8
            Write-Output ($id+' '+$condition.name+' '+$record.status)
            if($record.status -ne 'ok'){throw 'Retain the failed observation and inspect state.'}
        }
    }
} finally {$null=Control 'stop'}
