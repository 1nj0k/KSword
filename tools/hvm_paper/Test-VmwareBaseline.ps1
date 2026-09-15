param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target',
    [string]$VmxPath='C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$record=[ordered]@{schemaVersion=1;kind='vmware-without-ksword';runId=('baseline-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'));startedUtc=[DateTime]::UtcNow.ToString('o');status='started'}
$path=Join-Path $OutputDirectory ($record.runId+'.json')
function Save {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($record | ConvertTo-Json -Depth 24))
    $stream=[IO.File]::Open($path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally { $stream.Dispose() }
}
$session=$null
try {
    Save
    $session=New-PSSession -VMName $VMName -Credential $Credential
    $snapshot=Join-Path $PSScriptRoot 'Get-VmwareSnapshot.ps1'
    $record.before=Invoke-Command -Session $session -FilePath $snapshot -ArgumentList $VmxPath
    Save
    if (@($record.before.vmware).Count -ne 0) { throw 'Baseline requires no pre-existing vmware-vmx process.' }
    if ($record.before.driverService -eq 'Running' -and
        ($null -eq $record.before.hvm.parsed -or $record.before.hvm.exitCode -ne 0 -or
         $record.before.hvm.parsed.residentProcessorCount -ne 0 -or
         $record.before.hvm.parsed.stateNames -contains 'ROLLBACK_REQUIRED')) {
        throw 'Cannot establish stopped KSword residency.'
    }
    $record.start=Invoke-Command -Session $session -ArgumentList $VmxPath -ScriptBlock {
        param($vmx)
        $info=[Diagnostics.ProcessStartInfo]::new()
        $info.FileName='C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
        $info.Arguments='-T ws start "'+$vmx+'" nogui'
        $info.UseShellExecute=$false
        $info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true
        $info.RedirectStandardError=$true
        $process=[Diagnostics.Process]::new()
        $process.StartInfo=$info
        $start=[DateTime]::UtcNow
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        $completed=$process.WaitForExit(30000)
        # A timeout preserves the exact process and guest state for diagnosis.
        $result=[ordered]@{startedUtc=$start.ToString('o');endedUtc=[DateTime]::UtcNow.ToString('o');pid=$process.Id;completed=$completed;exitCode=$null;stdout=$null;stderr=$null}
        if($completed){$result.exitCode=$process.ExitCode;$result.stdout=$stdout.Result;$result.stderr=$stderr.Result}
        $process.Dispose()
        $result
    }
    Save
    $record.after=Invoke-Command -Session $session -FilePath $snapshot -ArgumentList $VmxPath
    $sameBoot=$record.before.bootUtc -eq $record.after.bootUtc
    $newLog=$record.after.vmwareLog -and ([datetime]$record.after.vmwareLog.lastWriteUtc -ge [datetime]$record.start.startedUtc)
    $record.assertions=[ordered]@{sameWindowsBoot=$sameBoot;newLogWritten=[bool]$newLog;newVmxProcess=(@($record.after.vmware).Count -eq 1);guestBootNotYetVerified=$true}
    if(!$sameBoot){$record.status='target_rebooted'}
    elseif(!$record.start.completed){$record.status='start_timeout'}
    elseif($record.start.exitCode -ne 0){$record.status='start_rejected'}
    elseif(!$newLog -or @($record.after.vmware).Count -ne 1){$record.status='start_not_verified'}
    else{$record.status='vm_started_guest_boot_unverified'}
} catch {
    $record.status='collector_error'
    $record.error=$_.Exception.Message
} finally {
    $record.endedUtc=[DateTime]::UtcNow.ToString('o')
    Save
    if($session){Remove-PSSession $session}
}
[ordered]@{runId=$record.runId;status=$record.status;path=$path} | ConvertTo-Json -Compress
