# Run from the Hyper-V host after deploying and preparing the test driver.
# Each command and kernel SGDT/SIDT readback are saved before the next case.
param(
    [string]$VMName = 'KSword-HVM-Target',
    [Parameter(Mandatory=$true)][PSCredential]$Credential,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,20)][int]$Iterations = 3
)
$ErrorActionPreference = 'Stop'
$env:COMPUTERNAME = [Environment]::MachineName
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$session = New-PSSession -VMName $VMName -Credential $Credential
function Save-Record($Record, $Path) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Record | ConvertTo-Json -Depth 30))
    $stream = [IO.File]::Open($Path,'Create','Write','Read')
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}
function Read-State {
    Invoke-Command -Session $session -ScriptBlock {
        $ctl = 'C:\ksword\hvm_ctl.exe'
        $driver = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
        [ordered]@{
            utc = [DateTime]::UtcNow.ToString('o')
            bootUtc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
            driverSha256 = (Get-FileHash -LiteralPath $driver).Hash
            ctlSha256 = (Get-FileHash -LiteralPath $ctl).Hash
            qpcFrequency = [Diagnostics.Stopwatch]::Frequency
            hvm = (& $ctl --json status | Out-String | ConvertFrom-Json)
            platform = (& $ctl --json probe-platform | Out-String | ConvertFrom-Json)
            vmx = @(Get-Process vmware-vmx -ErrorAction SilentlyContinue | Select-Object Id,StartTime)
        }
    }
}
function Run-Case([string]$Command,[int]$Iteration) {
    $id = 'descriptor-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+$Command
    $path = Join-Path $OutputDirectory ($id+'.json')
    $record = [ordered]@{schemaVersion=1;runId=$id;command=$Command;iteration=$Iteration;status='started';before=(Read-State)}
    Save-Record $record $path
    try {
        $record.result = Invoke-Command -Session $session -ArgumentList $Command -ScriptBlock {
            param($Command)
            $info = [Diagnostics.ProcessStartInfo]::new()
            $info.FileName = 'C:\ksword\hvm_ctl.exe'
            $info.Arguments = '--json '+$Command
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            $start = [DateTime]::UtcNow.ToString('o')
            $process = [Diagnostics.Process]::Start($info)
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $complete = $process.WaitForExit(30000)
            # A timeout is retained; never kill a command that may own VMX state.
            [ordered]@{startedUtc=$start;endedUtc=[DateTime]::UtcNow.ToString('o');completed=$complete;pid=$process.Id;
                exitCode=$(if($complete){$process.ExitCode}else{$null});
                stdout=$(if($complete){$stdout.GetAwaiter().GetResult()}else{$null});
                stderr=$(if($complete){$stderr.GetAwaiter().GetResult()}else{$null})}
        }
        Save-Record $record $path
        if (!$record.result.completed) { throw 'Command timed out; retain the target state for diagnosis.' }
        $record.after = Read-State
        # Read every available event page, retaining the unfiltered machine-readable rows.
        $record.events = @(Invoke-Command -Session $session -ScriptBlock {
            [UInt64]$cursor = 0
            for($page=0;$page -lt 40;$page++) {
                $data = (& C:\ksword\hvm_ctl.exe --json events $cursor 256 | Out-String | ConvertFrom-Json)
                $data
                if(!$data.returnedRows){break}
                $cursor = [UInt64]$data.rows[-1].sequence
                if($cursor -ge [UInt64]$data.newestSequence){break}
            }
        })
        $record.descriptorReadbacks = @($record.events.rows | Where-Object {
            $_.ruleId -in @(0x47445452,0x49445452) -and
            $_.sequence -gt $record.before.hvm.publishedEventCount
        })
        foreach($row in $record.descriptorReadbacks) {
            $limits = [Convert]::ToUInt64(($row.qualification -replace '^0x',''),16)
            if($row.status -ne '0x00000000' -or !$row.timestampQpc -or
                $row.guestPhysicalAddress -ne $row.guestLinearAddress -or
                ($limits -band 65535) -ne (($limits -shr 16) -band 65535)) {
                throw 'A kernel descriptor-table hardware readback did not match.'
            }
        }
        $requiredRows = switch($Command) { 'launch-test-guest' {2}; 'stop' {4}; 'nested-probe-all' {4}; 'nested-selfvirt-all' {4}; default {0} }
        if($record.descriptorReadbacks.Count -lt $requiredRows) { throw 'Required per-CPU descriptor evidence is missing.' }
        if($requiredRows -eq 4) {
            $identities = @($record.descriptorReadbacks | ForEach-Object { "$($_.processorGroup):$($_.processor):$($_.ruleId)" } | Select-Object -Unique)
            if($identities.Count -ne 4) { throw 'Both tables must be attributed to both processors.' }
        }
        $record.bootUnchanged = $record.before.bootUtc -eq $record.after.bootUtc
        if($record.result.exitCode -ne 0 -or !$record.bootUnchanged) {
            throw 'CLI or boot continuity check failed.'
        }
        if($record.after.hvm.stateNames -contains 'FAULTED' -or $record.after.hvm.stateNames -contains 'ROLLBACK_REQUIRED') {
            throw 'Driver reported incomplete architectural rollback.'
        }
        $record.status = 'passed'
    } catch {
        $record.status = 'failed'
        $record.error = $_.Exception.Message
        Save-Record $record $path
        throw
    }
    Save-Record $record $path
    Write-Output "$id PASS"
}
try {
    $initial = Read-State
    if($initial.vmx.Count -ne 0 -or $initial.hvm.residentProcessorCount -ne 0 -or $initial.hvm.preparedProcessorCount -ne 2) {
        throw 'This regression requires the prepared two-vCPU target, residency stopped, and VMware absent.'
    }
    for($iteration=1;$iteration -le $Iterations;$iteration++) {
        Run-Case 'launch-test-guest' $iteration
        Run-Case 'resident-nested-hidehv' $iteration
        Run-Case 'nested-probe-all' $iteration
        Run-Case 'nested-selfvirt-all' $iteration
        Run-Case 'stop' $iteration
    }
} finally { Remove-PSSession $session }
