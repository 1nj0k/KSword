# 宿主侧 A/B：同样的空闲条件，比较"不开隐藏"与"开隐藏"两种常驻。
#
# 12:42:55 那次 0xA 发生在机器空闲、没人登录、VMware 没跑的时候，栈上没有我们的帧。
# 这一轮要回答的只有一个问题：**隐藏模式会不会把机器弄崩**。所以两相只差那一位，
# 其余全部相同，而且由宿主计时与判活 —— 来宾崩了宿主还在。
param([int] $PhaseSeconds = 300)

$ErrorActionPreference = 'Continue'
$VMName = 'KSword-HVM-Target'
$cred = New-Object System.Management.Automation.PSCredential(
    'felix', (ConvertTo-SecureString 'password' -AsPlainText -Force))

function Note($s) {
    $line = ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $s)
    Write-Host $line
    [IO.File]::AppendAllText('C:\Users\Felix\CLionProjects\KSword\docs\next\logs\abtest.log', $line + [Environment]::NewLine)
}

function GuestBoot {
    try {
        return [datetime](Invoke-Command -VMName $VMName -Credential $cred -ErrorAction Stop -ScriptBlock {
            (Get-CimInstance Win32_OperatingSystem).LastBootUpTime })
    } catch { return $null }
}

function Ctl($verb) {
    try {
        return Invoke-Command -VMName $VMName -Credential $cred -ErrorAction Stop -ScriptBlock {
            param($v)
            $o = 'C:\ksword\abo.txt'
            $p = Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList @('--json', $v) -NoNewWindow -Wait -PassThru -RedirectStandardOutput $o -RedirectStandardError 'C:\ksword\abe.txt'
            @{ Exit = $p.ExitCode; Out = [IO.File]::ReadAllText($o) }
        } -ArgumentList $verb
    } catch { return $null }
}

function RunPhase($name, $verb) {
    Note ('--- ' + $name + ' ---')
    $boot0 = GuestBoot
    Note ('  开机时刻 ' + $boot0)

    $r = Ctl 'status'
    $st = $null
    if ($r) { try { $st = $r.Out | ConvertFrom-Json } catch { } }
    if ($st -and ($st.stateNames -contains 'RESIDENT_ACTIVE')) { $null = Ctl 'stop' }
    $r = Ctl 'status'
    if ($r) { try { $st = $r.Out | ConvertFrom-Json } catch { } }
    if ($st -and (($st.stateNames -contains 'FAULTED') -or ($st.stateNames -contains 'ROLLBACK_REQUIRED'))) { $null = Ctl 'reset-fault' }
    if ($st -and -not ($st.stateNames -contains 'RESOURCES_READY')) { $null = Ctl 'prepare' }
    $r = Ctl 'status'
    if ($r) { try { $st = $r.Out | ConvertFrom-Json } catch { } }
    if ($st -and -not ($st.stateNames -contains 'SELF_TEST_PASSED')) { $null = Ctl 'self-test' }

    $r = Ctl $verb
    if ($null -eq $r -or $r.Exit -ne 0) { Note ('  ' + $verb + ' 起不来，exit=' + $(if ($r) { $r.Exit } else { 'N/A' })); return 'BRINGUP_FAILED' }
    Note ('  ' + $verb + ' 已起')
    $cv = Ctl 'cpuid-view'
    if ($cv) { Note ('  cpuid-view ' + $cv.Out.Trim()) }

    $deadline = (Get-Date).AddSeconds($PhaseSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 20
        $b = GuestBoot
        if ($null -eq $b) { Note '  **来宾不应答**'; return 'UNRESPONSIVE' }
        if ([math]::Abs(($b - $boot0).TotalSeconds) -ge 2) { Note ('  **来宾重启过** ' + $boot0 + ' -> ' + $b); return 'REBOOTED' }
    }
    Note ('  ' + $PhaseSeconds + ' 秒内没有重启，没有失联')
    return 'SURVIVED'
}

[IO.File]::WriteAllText('C:\Users\Felix\CLionProjects\KSword\docs\next\logs\abtest.log', '')
Note '=== A/B 开始 ==='
$a = RunPhase 'A 相：不开隐藏 (resident-nested)' 'resident-nested'
Note ('A 相结果 ' + $a)
$b = RunPhase 'B 相：开隐藏 (resident-nested-hidehv)' 'resident-nested-hidehv'
Note ('B 相结果 ' + $b)
$null = Ctl 'stop'
Note ('=== 完  A=' + $a + '  B=' + $b + ' ===')
