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

if ($st.stateNames -contains 'FAULTED' -or $st.stateNames -contains 'ROLLBACK_REQUIRED') {
    $r = Run @('reset-fault'); Note ('reset-fault exit=' + $r.Exit)
}
if (-not ($st.stateNames -contains 'SELF_TEST_PASSED')) {
    $r = Run @('self-test'); Note ('self-test exit=' + $r.Exit)
}
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

Note '>>> 启动 VMware 的虚拟机'
Start-ScheduledTask -TaskName 'KswordVmStart'

for ($i = 1; $i -le 12; $i++) {
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

if (Test-Path 'C:\vmware\vmrun_start.txt') { Note ('vmrun: ' + ([IO.File]::ReadAllText('C:\vmware\vmrun_start.txt')).Trim()) } else { Note 'vmrun: <无输出>' }
if (Test-Path 'C:\vmware\tinycore\vmware.log') {
    Note '--- vmware.log 关键行 ---'
    (([IO.File]::ReadAllText('C:\vmware\tinycore\vmware.log')) -split "`r?`n" | Select-String -Pattern 'Hyper-V|VT-x|MONITOR|Monitor Mode|msg\.|IOPL|not compatible|PowerOn|poweredOn|EPT|Failed|vmm' | Select-Object -First 30) | ForEach-Object { Note ('  ' + $_) }
} else { Note 'vmware.log: <不存在>' }
$vx = Get-ChildItem 'C:\Users\felix\AppData\Local\Temp\vmware-felix' -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'vmx' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($vx) {
    Note ('--- ' + $vx.Name + ' 关键行 ---')
    (([IO.File]::ReadAllText($vx.FullName)) -split "`r?`n" | Select-String -Pattern 'Hyper-V|VT-x|MONITOR|msg\.|IOPL|not compatible|Failed|vmm|monitor' | Select-Object -First 30) | ForEach-Object { Note ('  ' + $_) }
}
Note '=== 完 ==='
