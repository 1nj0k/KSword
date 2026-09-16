param(
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VMName='KSword-HVM-Target',
    [ValidateRange(1,100)][int]$Repetitions=5,
    [string]$GuestPhysicalPage='7000000',
    [ValidateRange(1,64)][int]$ExpectedResidentProcessors=2
)
$ErrorActionPreference='Stop'
$env:COMPUTERNAME=[Environment]::MachineName
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$s=New-PSSession -VMName $VMName -Credential $Credential
function Save($r,$p) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($r | ConvertTo-Json -Depth 25))
    $f=[IO.File]::Open($p,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}
function Control([string[]]$a) {
    Invoke-Command -Session $s -ArgumentList (,$a) -ScriptBlock {
        param($arguments)
        $start=[DateTime]::UtcNow.ToString('o')
        $q=[Diagnostics.Stopwatch]::GetTimestamp()
        $raw=(& C:\ksword\hvm_ctl.exe --json @arguments 2>&1 | Out-String);$code=$LASTEXITCODE
        $end=[Diagnostics.Stopwatch]::GetTimestamp()
        $parsed=$null;try{$parsed=$raw | ConvertFrom-Json}catch{}
        [ordered]@{arguments=$arguments;startedUtc=$start;endedUtc=[DateTime]::UtcNow.ToString('o');beginQpc=$q;endQpc=$end;frequency=[Diagnostics.Stopwatch]::Frequency;exitCode=$code;raw=$raw;parsed=$parsed}
    }
}
function State {
    $snapshot=Invoke-Command -Session $s -FilePath (Join-Path $PSScriptRoot 'Get-VmwareSnapshot.ps1')
    [ordered]@{windows=$snapshot;page=(Control @('nested-page-query'));metrics=(Control @('metrics'))}
}
function Events([string]$After) {
    # Read every page up to a fixed watermark; a 256-row first page can contain
    # only unrelated nested-VMX events under load.
    $pages=@();$rows=@();$cursor=[uint64]$After;$last=$cursor
    do {
        $page=Control @('events',[string]$cursor,'256');$pages+=@($page)
        if($page.exitCode -ne 0){throw 'Event read failed.'}
        if($pages.Count -eq 1){$last=[uint64]$page.parsed.newestSequence}
        $batch=@($page.parsed.rows | Where-Object {[uint64]$_.sequence -le $last})
        $rows+=@($batch)
        if(!$batch.Count){break}
        $cursor=[uint64]$batch[-1].sequence
        if($pages.Count -gt 64){throw 'Event evidence exceeded bounded page budget.'}
    }while($cursor -lt $last)
    [ordered]@{pages=$pages;parsed=[ordered]@{rows=$rows;newestSequence=$last;afterSequence=$After}}
}
try {
    foreach($iteration in 1..$Repetitions) {
        foreach($fault in 1..5) {
            $id='page-fault-'+$fault+'-'+$iteration+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
            $path=Join-Path $OutputDirectory ($id+'.json')
            $r=[ordered]@{schemaVersion=1;kind='nested-page-fault';runId=$id;faultMode=$fault;iteration=$iteration;startedUtc=[DateTime]::UtcNow.ToString('o');status='started'}
            $r.expectedResidentProcessors=$ExpectedResidentProcessors
            Save $r $path
            try {
                $r.before=State;Save $r $path
                $p=$r.before.page.parsed
                if($r.before.page.exitCode -ne 0 -or $r.before.metrics.exitCode -ne 0 -or $p.active -ne 0 -or $p.retired -ne 0 -or @($p.roots).Count -ne 1 -or $p.residentProcessors -ne $ExpectedResidentProcessors){throw "Require a clean slot, one nested root and $ExpectedResidentProcessors resident processors."}
                $root=[string]$p.roots[0]
                $anchor=Control @('events','0','1')
                $r.eventAnchor=$anchor.parsed.newestSequence
                if($fault -eq 5) {
                    $r.map=Control @('nested-page-map',$root,$GuestPhysicalPage,'D1');Save $r $path
                    if($r.map.exitCode -ne 0){throw 'Removal fault setup could not map.'}
                    $r.action=Control @('nested-page-remove-test');Save $r $path
                    $r.retained=State;Save $r $path
                    if($r.action.exitCode -ne 2 -or $r.action.parsed.active -ne 0 -or $r.action.parsed.retired -ne 1){throw 'Failed removal did not retain backing.'}
                    $r.retry=Control @('nested-page-remove');Save $r $path
                    if($r.retry.exitCode -ne 0){throw 'Removal retry failed.'}
                }else{
                    $r.action=Control @('nested-page-map-test',$root,$GuestPhysicalPage,'D1',[string]$fault);Save $r $path
                    if($r.action.exitCode -ne 2){throw 'Fault was not reported as a semantic failure.'}
                }
                $r.events=Events ([string]$r.eventAnchor);Save $r $path
                # Both CPU-pinned observers run once per guest second. Allow
                # scheduling/serial latency without treating old readback as fresh.
                Start-Sleep -Seconds 5
                $r.after=State
                $a=$r.after.page.parsed
                $b=$r.before.metrics.parsed;$m=$r.after.metrics.parsed
                $idsBefore=@($r.before.windows.vmware | ForEach-Object {"$($_.pid):$($_.createdUtc)"})
                $idsAfter=@($r.after.windows.vmware | ForEach-Object {"$($_.pid):$($_.createdUtc)"})
                $r.assertions=[ordered]@{
                    noActiveOrRetired=($a.active -eq 0 -and $a.retired -eq 0)
                    sameWindowsBoot=($r.before.windows.bootUtc -eq $r.after.windows.bootUtc)
                    sameVmxIdentity=($idsBefore.Count -eq 1 -and ($idsBefore -join '|') -eq ($idsAfter -join '|'))
                    residentCountMatches=($a.residentProcessors -eq $ExpectedResidentProcessors)
                    healthy=(!@($r.after.windows.hvm.parsed.stateNames | Where-Object {$_ -in @('FAULTED','ROLLBACK_REQUIRED')}).Count)
                    balancedRuleDelta=(([decimal]$m.ruleAllocations-[decimal]$b.ruleAllocations) -eq ([decimal]$m.ruleFrees-[decimal]$b.ruleFrees))
                    balancedReplacementDelta=(([decimal]$m.replacementAllocations-[decimal]$b.replacementAllocations) -eq ([decimal]$m.replacementFrees-[decimal]$b.replacementFrees))
                    noOutstandingRules=([decimal]$m.ruleAllocations -eq [decimal]$m.ruleFrees)
                    noOutstandingReplacements=([decimal]$m.replacementAllocations -eq [decimal]$m.replacementFrees)
                }
                if(@($r.assertions.Values | Where-Object {!$_}).Count){throw 'A lifecycle or resource assertion failed.'}
                # Guest readback and stage ordering are evaluated from retained raw logs.
                $r.status='control_assertions_pass_guest_evidence_requires_analysis'
            }catch{
                $r.status='error';$r.error=$_.Exception.Message
                try{$r.cleanup=Control @('nested-page-remove')}catch{$r.cleanupError=$_.Exception.Message}
            }finally{
                $r.endedUtc=[DateTime]::UtcNow.ToString('o');Save $r $path
            }
            Write-Output ($id+' '+$r.status)
            if($r.status -eq 'error'){return}
        }
    }
}finally{Remove-PSSession $s}
