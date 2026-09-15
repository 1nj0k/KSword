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
$vncScript = Join-Path $PSScriptRoot 'Get-VmwareVnc.ps1'
$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName $VMName -Credential $cred

# 串口是追加写的，先清掉，否则读到的是上一轮探针的字节。
Invoke-Command -Session $s -ArgumentList $SerialLog -ScriptBlock {
    param($log)
    Get-Process -Name 'vmware', 'vmware-vmx' -ErrorAction SilentlyContinue |
        Stop-Process -Force
    Start-Sleep -Seconds 3
    if (Test-Path $log) { Remove-Item $log -Force }
    $vmx = 'C:\Users\felix\Documents\Virtual Machines\' +
           'Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    Start-Process -FilePath 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe' `
        -ArgumentList @('-T', 'ws', 'start', "`"$vmx`"") -NoNewWindow
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
            if ($_ -cge 'A' -and $_ -cle 'Z') { $c -bor 0x10000 } else { $c }
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
