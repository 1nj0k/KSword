# 用文本控制台 + 串口日志重开 TinyCore，把内核自己的话取出来。
#
# 为什么要这条路：到目前为止关于"它停在哪"的每一个结论都是从 VMCS 字段反推的 ——
# 退出原因、RIP、描述符表。那能告诉我 L2 停在 0xFFFFFFFF81C721CA 的 HLT 上，
# 不能告诉我 Linux 自己以为发生了什么。而默认菜单项 "Boot TinyCorePure64" 还要
# 起 X，屏幕因此是一整片 1024x768 的黑，连内核打印都看不到。
#
# 两处改动都只作用在引导参数上，不动虚拟机硬件配置：
#   1. 选第三项 "Boot Core (command line only)"，不起 X；
#   2. TAB 进编辑行，追加 console=ttyS0,115200n8，让内核同时往串口打印。
# 串口早就配好了（serial0.fileName = C:\vmware\hltprobe.log），是上一轮探针留下的。
param(
    [string] $VMName = 'KSword-HVM-Target',
    [string] $SerialLog = 'C:\vmware\hltprobe.log',
    [int]    $WaitSeconds = 120,
    # 额外追加的内核参数，例如 'nosmp'。只能用小写与数字，大写要走 Shift 位。
    [string] $Append = ''
)

$ErrorActionPreference = 'Stop'
# Hyper-V PowerShell Direct requires a nonempty local machine name.
$env:COMPUTERNAME = [Environment]::MachineName
$vncScript = Join-Path $PSScriptRoot 'Get-VmwareVnc.ps1'
$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName $VMName -Credential $cred

# 串口是追加写的，先清掉，否则读到的是上一轮探针的字节。
Invoke-Command -Session $s -ArgumentList $SerialLog -ScriptBlock {
    param($log)
    # Keep the virtual CPU execution layer alive while VMware destroys its VM.
    # Stopping residency first can leave vmrun waiting indefinitely (recorded 4x2).
    # If teardown fails, retain the state for diagnosis and restart HVM-target.
    $vmxPath = 'C:\Users\felix\Documents\Virtual Machines\' +
               'Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    $vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
    $ctl = 'C:\ksword\hvm_ctl.exe'

    if (@(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue).Count -gt 0) {
        # 保留自己启动的进程句柄，避免 Start-Process 返回的对象在退出后丢失 ExitCode。
        $stopInfo = New-Object Diagnostics.ProcessStartInfo
        $stopInfo.FileName = $vmrun
        $stopInfo.Arguments = "-T ws stop `"$vmxPath`" hard"
        $stopInfo.UseShellExecute = $false
        $stopInfo.CreateNoWindow = $true
        $stopping = New-Object Diagnostics.Process
        $stopping.StartInfo = $stopInfo
        try {
            if (-not $stopping.Start()) { throw '无法启动 vmrun stop' }
            if (-not $stopping.WaitForExit(30000)) {
                $stopping.Kill()
                throw 'vmrun stop 超时；保留 Windows、VMware 与常驻状态供检查'
            }
            if ($stopping.ExitCode -ne 0) { throw "vmrun stop 失败：$($stopping.ExitCode)" }
        } finally {
            $stopping.Dispose()
        }
    }
    $waited = 0
    while (@(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue).Count -gt 0 -and
           $waited -lt 30) {
        Start-Sleep -Seconds 1
        $waited++
    }
    if (@(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue).Count -ne 0) {
        throw 'VMware 来宾未停止，禁止重新启动常驻或覆盖串口日志'
    }
    # Reclaim a revoked page only after all VMware vCPUs have disappeared.
    & $ctl --json nested-page-remove | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'EPT replacement reclamation failed; do not stop residency' }
    $stop = Start-Process $ctl -ArgumentList 'stop' -WindowStyle Hidden -Wait -PassThru
    $state = (& $ctl --json status) | ConvertFrom-Json
    if ($stop.ExitCode -ne 0 -or $null -eq $state.residentProcessorCount -or
        $state.residentProcessorCount -ne 0 -or
        $state.stateNames -contains 'ROLLBACK_REQUIRED') {
        throw '常驻未完整停止，保留 VMware 与 Windows 当前状态，禁止继续拆除'
    }
    Get-Process -Name 'vmware' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
    # 常驻起回来，并且必须是隐藏 hypervisor 的那一版，否则 VMware 的身份门直接拒绝。
    $state = (& $ctl --json status) | ConvertFrom-Json
    if ($state.featureNames -notcontains 'EPTP_SWITCH_ARMED') {
        foreach ($command in @('teardown', 'prepare-eptpsw', 'self-test')) {
            & $ctl $command | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "$command 失败" }
        }
    }
    & $ctl resident-nested-hidehv | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '嵌套常驻启动失败' }
    $state = (& $ctl --json status) | ConvertFrom-Json
    $view = (& $ctl --json cpuid-view) | ConvertFrom-Json
    if ($null -eq $state.residentProcessorCount -or $state.residentProcessorCount -le 0 -or
        $state.featureNames -notcontains 'EPTP_SWITCH_ARMED' -or -not $view.hidden) {
        throw '嵌套常驻或 EPTP 换页后端未生效'
    }
    & sc.exe stop vmx86 | Out-Null
    Start-Sleep -Seconds 1
    & sc.exe start vmx86 | Out-Null
    Start-Sleep -Seconds 2
    if (Test-Path $log) { Remove-Item $log -Force }
    $vmx = 'C:\Users\felix\Documents\Virtual Machines\' +
           'Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    Start-Process -FilePath 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe' `
        -ArgumentList @('-T', 'ws', 'start', "`"$vmx`"") -WindowStyle Hidden
    Start-Sleep -Seconds 40
    "vmware-vmx = " + @(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue).Count
}

# X11 keysym：可打印 ASCII 就是它自己的码，方向键和 TAB 走 0xFF 段。
$down = 0xFF54
$tab = 0xFF09
$enter = 0xFF0D
# ignore_loglevel：菜单给的是 loglevel=3，只印到 KERN_ERR，正好把"它停在哪"
# 那一段全滤掉。追加在后面就够，不必改前面的参数。
$text = ' console=ttyS0,115200n8 ignore_loglevel'
if ($Append) { $text += ' ' + $Append }
# 大写字母要带 Shift（0x10000 位），否则 ttyS0 会变成 ttys0 —— 见 VNC 脚本。
$keys = @($down, $down, $tab) +
        ($text.ToCharArray() | ForEach-Object {
            $c = [int][char]$_
            if (($_ -cge 'A' -and $_ -cle 'Z') -or
                '~!@#$%^&*()_+{}|:"<>?'.Contains([string]$_)) {
                $c -bor 0x10000
            } else { $c }
        }) +
        @($enter)

Invoke-Command -Session $s -FilePath $vncScript `
    -ArgumentList '127.0.0.1', 5900, 'C:\vmware\shot-serialboot.png', $keys, 90

Start-Sleep -Seconds $WaitSeconds

Invoke-Command -Session $s -FilePath $vncScript `
    -ArgumentList '127.0.0.1', 5900, 'C:\vmware\shot-serialafter.png'

$out = Invoke-Command -Session $s -ArgumentList $SerialLog -ScriptBlock {
    param($log)
    if (-not (Test-Path $log)) { return '(串口文件不存在)' }
    # vmware-vmx 一直开着这个文件，ReadAllBytes 会被拒；必须自己开共享读。
    $fs = New-Object IO.FileStream($log, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $b = New-Object byte[] $fs.Length
        [void]$fs.Read($b, 0, $b.Length)
    } finally { $fs.Dispose() }
    "串口共 $($b.Length) 字节`n" +
        (-join ($b | ForEach-Object {
            if ($_ -ge 32 -and $_ -lt 127) { [char]$_ }
            elseif ($_ -eq 10) { "`n" }
            elseif ($_ -eq 13) { '' }
            else { '.' }
        }))
}
Remove-PSSession $s
Write-Output $out
