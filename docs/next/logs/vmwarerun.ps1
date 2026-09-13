# 在来宾里跑：起隐藏模式常驻 -> 取基线 -> 开 VMware 的虚拟机 -> 收读数。
#
# 每一步都立刻追加到 C:\ksword\vmwarerun.log 并关闭句柄。整机挂死时最后一条
# 记录就是现场 —— 这条线上的失败方式是蓝屏，而蓝屏会把内存里的缓冲一起带走。
$log = 'C:\ksword\vmwarerun.log'
function Note($s) {
    $line = ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $s)
    [IO.File]::AppendAllText($log, $line + [Environment]::NewLine)
}
function Run($a) {
    $o = 'C:\ksword\ro.txt'
    $e = 'C:\ksword\re.txt'
    $p = Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList $a -NoNewWindow -Wait -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
    $out = ''
    if (Test-Path $o) { $out = [IO.File]::ReadAllText($o) }
    return @{ Exit = $p.ExitCode; Out = $out }
}

[IO.File]::WriteAllText($log, '')
Note '=== 开始 ==='

$r = Run @('--json','status')
$st = $null
try { $st = $r.Out | ConvertFrom-Json } catch { }
Note ('status exit=' + $r.Exit + ' 状态位=' + ($st.stateNames -join ' '))

# 常驻已经在跑就先停。nested 与 hidehv 在状态位上完全一样，沿用一个来历不明的
# 常驻，等于让整轮测量在一个没人选过的模式下跑完并报读数。
if ($st.stateNames -contains 'RESIDENT_ACTIVE') {
    $r = Run @('stop'); Note ('stop（先清掉模式不可知的常驻）exit=' + $r.Exit)
    $r = Run @('--json','status')
    try { $st = $r.Out | ConvertFrom-Json } catch { }
}
if ($st.stateNames -contains 'FAULTED' -or $st.stateNames -contains 'ROLLBACK_REQUIRED') {
    $r = Run @('reset-fault'); Note ('reset-fault exit=' + $r.Exit)
}
if (-not ($st.stateNames -contains 'RESOURCES_READY')) {
    $r = Run @('prepare'); Note ('prepare exit=' + $r.Exit)
    $r = Run @('--json','status')
    try { $st = $r.Out | ConvertFrom-Json } catch { }
}
if (-not ($st.stateNames -contains 'SELF_TEST_PASSED')) {
    $r = Run @('self-test'); Note ('self-test exit=' + $r.Exit)
    $r = Run @('--json','status')
    try { $st = $r.Out | ConvertFrom-Json } catch { }
}
Note ('起常驻前的状态位 ' + ($st.stateNames -join ' '))
$r = Run @('resident-nested-hidehv')
Note ('resident-nested-hidehv exit=' + $r.Exit)
if ($r.Exit -ne 0) { Note '常驻起不来，停止'; exit 1 }

$r = Run @('--json','cpuid-view')
Note ('cpuid-view ' + $r.Out.Trim())

$r = Run @('--json','status')
[IO.File]::WriteAllText('C:\ksword\base.json', $r.Out)
$b = $null
try { $b = $r.Out | ConvertFrom-Json } catch { }
Note ('基线 vmExits=' + $b.vmExitCount + ' 拒绝=' + $b.nestedL2LaunchRefusedCount + ' 熔断=' + $b.nestedFuseTripCount)

foreach ($f in @('C:\vmware\tinycore\vmware.log','C:\vmware\vmrun_start.txt')) {
    if (Test-Path $f) { [IO.File]::Delete($f) }
}
Get-ChildItem 'C:\Users\felix\AppData\Local\Temp\vmware-felix' -File -ErrorAction SilentlyContinue | ForEach-Object { try { [IO.File]::Delete($_.FullName) } catch { } }

# 让 VMware 的驱动重新采样能力 MSR。
#
# vmx86.sys 在来宾开机时就把 IA32_VMX_* 缓存下来了，那时我们的常驻还不存在，
# 所以 vmware-vmx 拿到的是**原始主机能力**（实测 0x48b=0x065018AE，里面有 VPID
# 和 unrestricted guest），照着它配 VMCS 就必然撞上我们没实现的东西。
# 我们的能力过滤器没有错，是从来没被问到。重启这个驱动是唯一能让它重新问的办法。
Note '--- 重启 vmx86 让它重新采样能力 MSR ---'
foreach ($svc in @('VMAuthdService','VMwareHostd','VMnetDHCP','VMware NAT Service')) {
    $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq 'Running') { try { Stop-Service -Name $svc -Force -ErrorAction Stop; Note ('  停 ' + $svc) } catch { Note ('  停 ' + $svc + ' 失败: ' + $_.Exception.Message) } }
}
foreach ($drv in @('vmx86')) {
    $r1 = & sc.exe stop $drv 2>&1
    Start-Sleep -Seconds 2
    $r2 = & sc.exe start $drv 2>&1
    $st = (Get-Service -Name $drv -ErrorAction SilentlyContinue).Status
    Note ('  ' + $drv + ' 重启后状态=' + $st)
}
foreach ($svc in @('VMAuthdService')) {
    try { Start-Service -Name $svc -ErrorAction Stop; Note ('  起 ' + $svc) } catch { Note ('  起 ' + $svc + ' 失败: ' + $_.Exception.Message) }
}

Note '>>> 启动 VMware 的虚拟机'
# 交互会话不在的时候 Start-ScheduledTask 是**静默空操作** —— 上一轮整整 60 秒的
# 采样都是在测一个没启动的 VMware。所以先把会话记下来，跑完再把任务的实际执行
# 时间读回来对一遍。
$expl = @(Get-Process -Name explorer -ErrorAction SilentlyContinue)
Note ('  交互桌面 explorer=' + $expl.Count + ' session=' + $(if ($expl.Count) { $expl[0].SessionId } else { 'N/A' }))
$before = (Get-ScheduledTaskInfo -TaskName 'KswordVmStart').LastRunTime
Start-ScheduledTask -TaskName 'KswordVmStart'

# VMware 上一轮在开机后 **2 秒** 就把机器带崩了，日志一行都没捞到。所以先抓日志
# 再采样：它放弃得比任何采样窗口都快，而它自己的日志是唯一说明"为什么放弃"的东西。
function DumpVmwareLogs($tag) {
    Note ('--- VMware 日志 (' + $tag + ') ---')
    if (Test-Path 'C:\vmware\vmrun_start.txt') { Note ('  vmrun: ' + ([IO.File]::ReadAllText('C:\vmware\vmrun_start.txt')).Trim()) }
    if (Test-Path 'C:\vmware\tinycore\vmware.log') {
        (([IO.File]::ReadAllText('C:\vmware\tinycore\vmware.log')) -split "`r?`n" |
            Select-String -Pattern 'Hyper-V|VT-x|VMX|MONITOR|Monitor Mode|msg\.|IOPL|not compatible|PowerOn|poweredOn|EPT|MMU|monitor|Failed|unsupported|Unsupported') |
            Select-Object -First 40 | ForEach-Object { Note ('  ' + $_) }
    } else { Note '  vmware.log 还不存在' }
    $vx = Get-ChildItem 'C:\Users\felix\AppData\Local\Temp\vmware-felix' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'vmx' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($vx) {
        Note ('  --- ' + $vx.Name + ' ---')
        (([IO.File]::ReadAllText($vx.FullName)) -split "`r?`n" |
            Select-String -Pattern 'Hyper-V|VT-x|MONITOR|msg\.|IOPL|not compatible|Failed|monitor|MMU|unsupported') |
            Select-Object -First 30 | ForEach-Object { Note ('  ' + $_) }
    }
}

Start-Sleep -Seconds 8
DumpVmwareLogs '开机后 8 秒'

for ($i = 1; $i -le 6; $i++) {
    Start-Sleep -Seconds 5
    $r = Run @('--json','status')
    $s = $null
    try { $s = $r.Out | ConvertFrom-Json } catch { }
    $vmx = @(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue)
    $rc = ($s.exitReasonCount.PSObject.Properties | Where-Object { [int]$_.Name -ge 19 -and [int]$_.Name -le 27 } | ForEach-Object { $_.Name + '=' + $_.Value }) -join ' '
    Note ('采样' + $i + ' vmExits=' + $s.vmExitCount + ' 拒绝=' + $s.nestedL2LaunchRefusedCount + ' 熔断=' + $s.nestedFuseTripCount + ' 末次错误=' + $s.lastVmInstructionError + ' vmx进程=' + $vmx.Count + ' VMX族[' + $rc + ']')
}

$r = Run @('--json','status')
[IO.File]::WriteAllText('C:\ksword\final.json', $r.Out)
Note '=== 采样结束 ==='

# 任务到底跑没跑。LastRunTime 没动就说明 Start-ScheduledTask 空转了，
# 那一轮所有读数都与 VMware 无关 —— 这一条必须在读计数器之前先看。
$after = (Get-ScheduledTaskInfo -TaskName 'KswordVmStart')
Note ('任务 LastRunTime ' + $before + ' -> ' + $after.LastRunTime + '  LastTaskResult=0x' + ('{0:X}' -f $after.LastTaskResult))
if ($after.LastRunTime -eq $before) { Note '**任务没有执行 —— 本轮与 VMware 无关**' }

DumpVmwareLogs '收尾'
Note '=== 完 ==='
