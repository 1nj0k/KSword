# Execute inside Windows 1 after the autonomous guest observer has started.
# All measured steps use local Windows APIs, serial reads, and KSword IOCTLs.
param(
    [string]$OutputDirectory='C:\ksword\paper\application',
    [ValidateRange(1,10)][int]$Repetitions=3,
    [ValidateRange(1,64)][int]$ExpectedResidentProcessors=4
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$ctl='C:\ksword\hvm_ctl.exe'
function Serial {
    $f=[IO.File]::Open('C:\vmware\hltprobe.log','Open','Read','ReadWrite')
    $reader=[IO.StreamReader]::new($f)
    try {$reader.ReadToEnd()} finally {$reader.Dispose()}
}
function Control([string[]]$Arguments) {
    $utc=[DateTime]::UtcNow.ToString('o');$q=[Diagnostics.Stopwatch]::GetTimestamp()
    $raw=(& $ctl --json @Arguments 2>&1 | Out-String);$code=$LASTEXITCODE
    [ordered]@{arguments=$Arguments;startedUtc=$utc;endedUtc=[DateTime]::UtcNow.ToString('o');beginQpc=$q;
        endQpc=[Diagnostics.Stopwatch]::GetTimestamp();frequency=[Diagnostics.Stopwatch]::Frequency;
        exitCode=$code;raw=$raw;parsed=($raw | ConvertFrom-Json)}
}
function State {
    $p=Get-Process vmware-vmx
    $vmx='C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');
        vmx=@($p | ForEach-Object {[ordered]@{pid=$_.Id;createdUtc=$_.StartTime.ToUniversalTime().ToString('o');path=$_.Path;sha256=(Get-FileHash $_.Path).Hash}});
        vmxConfigSha256=(Get-FileHash $vmx).Hash;hvm=(Control @('status'));page=(Control @('nested-page-query'));metrics=(Control @('metrics'))}
}
function Save($Record,$Path) {
    $b=[Text.UTF8Encoding]::new($false).GetBytes(($Record | ConvertTo-Json -Depth 25))
    $f=[IO.File]::Open($Path,'Create','Write','Read')
    try {$f.Write($b,0,$b.Length);$f.Flush($true)} finally {$f.Dispose()}
}
function Observe([string]$Stage) {
    $offset=(Serial).Length;$deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 400
        $text=Serial
        $tail=$text.Substring($offset)
        $samples=[regex]::Matches($tail,'(?m)^paper-http-sample-v2 [^\r\n]+\r?\n\{"kind":"application-page"[^\r\n]+\r?\n')
    } while($samples.Count -lt 10 -and [DateTime]::UtcNow -lt $deadline)
    [ordered]@{stage=$Stage;serialOffset=$offset;serialEnd=$text.Length;sampleCount=$samples.Count;
        utc=[DateTime]::UtcNow.ToString('o');status=$(if($samples.Count -ge 10){'recorded'}else{'timeout'})}
}
$setup=Serial
$identities=[regex]::Matches($setup,'paper-http-identity ([a-f0-9-]{36}) (\S+)')
$holders=[regex]::Matches($setup,'(?m)^\{"kind":"application-page"[^\r\n]+')
if(!$identities.Count -or !$holders.Count){throw 'Start the guest HTTP observer before this controller.'}
$identity=$identities[$identities.Count-1].Value
$holder=$holders[$holders.Count-1].Value | ConvertFrom-Json
if(!$holder.samePfn){throw 'Application page already migrated.'}
$gpa=([string]$holder.gpa) -replace '^0x',''
for($i=1;$i -le $Repetitions;$i++) {
    $r=[ordered]@{schemaVersion=2;kind='http-page-fault';runId=('http-page-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'));
        iteration=$i;status='started';expectedResidentProcessors=$ExpectedResidentProcessors;gpa=$holder.gpa;guestIdentity=$identity;holderPid=$holder.pid;
        controlScope='Local Windows APIs, local read-only serial, and KSword IOCTLs. Observer launched before interval; no vmrun, VMM injection, hook, or Hyper-V management command in this controller.';
        steps=@();mapAttempted=$false;startedUtc=[DateTime]::UtcNow.ToString('o')}
    $path=Join-Path $OutputDirectory ($r.runId+'.json');Save $r $path
    try {
        $r.before=State;Save $r $path
        $page=$r.before.page.parsed
        if($page.active -ne 0 -or $page.retired -ne 0 -or $page.residentProcessors -ne $ExpectedResidentProcessors -or @($page.roots).Count -ne 1 -or $r.before.vmx.Count -ne 1){throw 'Unexpected topology or existing mapping.'}
        $r.steps+=@(Observe 'before');Save $r $path
        if($r.steps[-1].status -ne 'recorded'){throw 'HTTP observer timed out before map; no mapping attempted.'}
        $r.mapAttempted=$true;Save $r $path
        $r.map=Control @('nested-page-map',[string]$page.roots[0],$gpa,'00',[string]$r.before.vmx[0].pid);Save $r $path
        if($r.map.exitCode -ne 0 -or $r.map.parsed.active -ne 1){throw 'Application page map rejected.'}
        $r.steps+=@(Observe 'mapped');Save $r $path
        if($r.steps[-1].status -ne 'recorded'){throw 'HTTP observer timed out while mapped; cleanup required.'}
        $r.mapped=State;Save $r $path
        $r.remove=Control @('nested-page-remove');Save $r $path
        if($r.remove.exitCode -ne 0){throw 'Application page restore failed.'}
        $r.steps+=@(Observe 'restored');Save $r $path
        $r.after=State
        if(@($r.steps | Where-Object status -ne 'recorded').Count){throw 'HTTP observer timed out.'}
        $r.status='recorded_requires_analysis'
    } catch {
        $r.status='error';$r.error=$_.Exception.Message
        if($r.mapAttempted){$r.cleanup=Control @('nested-page-remove')}
    } finally {
        $r.endedUtc=[DateTime]::UtcNow.ToString('o');Save $r $path
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'serial.txt'),(Serial),[Text.UTF8Encoding]::new($false))
    }
    Write-Output ($r.runId+' '+$r.status)
    if($r.status -eq 'error'){throw $r.error}
}
