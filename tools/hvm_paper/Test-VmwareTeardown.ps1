# Exercise VMware teardown while KSword is resident on the explicitly selected lab VM.
# The host saves the precondition before starting a command that may reset that VM.
param(
    [string]$VMName = 'KSword-HVM-Target',
    [Parameter(Mandatory=$true)][PSCredential]$Credential,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$VmxPath = 'C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
)
$ErrorActionPreference = 'Stop'
$env:COMPUTERNAME = [Environment]::MachineName
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$id = 'vmware-teardown-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$path = Join-Path $OutputDirectory ($id+'.json')
$started = Get-Date
$record = [ordered]@{schemaVersion=1;runId=$id;target=$VMName;vmxPath=$VmxPath;status='started';startedUtc=$started.ToUniversalTime().ToString('o')}
function Save-Record {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($record | ConvertTo-Json -Depth 30))
    $stream = [IO.File]::Open($path,'Create','Write','Read')
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}
function Read-State {
    Invoke-Command -Session $session -ScriptBlock {
        $ctl = 'C:\ksword\hvm_ctl.exe'
        $driver = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
        [ordered]@{
            utc=[DateTime]::UtcNow.ToString('o')
            bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
            driverSha256=(Get-FileHash -LiteralPath $driver).Hash
            ctlSha256=(Get-FileHash -LiteralPath $ctl).Hash
            hvm=(& $ctl --json status | Out-String | ConvertFrom-Json)
            page=(& $ctl --json nested-page-query | Out-String | ConvertFrom-Json)
            vmx=@(Get-Process vmware-vmx -ErrorAction SilentlyContinue | ForEach-Object {
                [ordered]@{pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o')}
            })
            serialLength=(Get-Item 'C:\vmware\hltprobe.log' -ErrorAction SilentlyContinue).Length
        }
    }
}
$session = New-PSSession -VMName $VMName -Credential $Credential
try {
    $record.before = Read-State
    Save-Record
    if($record.before.vmx.Count -ne 1 -or $record.before.hvm.residentProcessorCount -ne 2 -or $record.before.page.active -ne 0) {
        throw 'Require one VMware VM, two resident processors, and no active replacement mapping.'
    }
    $record.stop = Invoke-Command -Session $session -ArgumentList $VmxPath -ScriptBlock {
        param($VmxPath)
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
        $info.Arguments = '-T ws stop "'+$VmxPath+'" hard'
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $start = [DateTime]::UtcNow.ToString('o')
        $startQpc = [Diagnostics.Stopwatch]::GetTimestamp()
        $process = [Diagnostics.Process]::Start($info)
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $complete = $process.WaitForExit(30000)
        [ordered]@{
            startedUtc=$start;endedUtc=[DateTime]::UtcNow.ToString('o');startQpc=$startQpc
            endQpc=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency
            completed=$complete;pid=$process.Id;exitCode=$(if($complete){$process.ExitCode}else{$null})
            stdout=$(if($complete){$stdout.GetAwaiter().GetResult()}else{$null})
            stderr=$(if($complete){$stderr.GetAwaiter().GetResult()}else{$null})
        }
    }
    Save-Record
    if(!$record.stop.completed) { throw 'vmrun stop timed out; the process was retained for diagnosis.' }
    Start-Sleep -Seconds 5
    $record.after = Read-State
    $record.bootUnchanged = $record.before.bootUtc -eq $record.after.bootUtc
    if($record.stop.exitCode -ne 0 -or !$record.bootUnchanged -or $record.after.vmx.Count -ne 0 -or
       $record.after.hvm.residentProcessorCount -ne 2 -or
       $record.after.hvm.stateNames -contains 'FAULTED' -or $record.after.hvm.stateNames -contains 'ROLLBACK_REQUIRED') {
        throw 'VMware teardown did not preserve the running Windows/KSword state.'
    }
    $record.status = 'passed'
} catch {
    $record.status = 'failed'
    $record.error = $_.ToString()
} finally {
    $record.endedUtc = [DateTime]::UtcNow.ToString('o')
    Save-Record
    $record.hostVm = Get-VM -Name $VMName | Select-Object Name,State,Uptime,Status
    $record.hyperVResetEvents = @(Get-WinEvent -FilterHashtable @{
        LogName='Microsoft-Windows-Hyper-V-Worker-Admin';Id=18560;StartTime=$started
    } -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{id=$_.Id;utc=$_.TimeCreated.ToUniversalTime().ToString('o');message=$_.Message;xml=$_.ToXml()}
    })
    Save-Record
    Remove-PSSession $session -ErrorAction SilentlyContinue
}
[pscustomobject]@{runId=$id;status=$record.status;path=$path;error=$record.error}
if($record.status -ne 'passed') { throw $record.error }
