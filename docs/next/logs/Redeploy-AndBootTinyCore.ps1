# 宿主侧一步到位：把刚构建的驱动装进靶机、重起常驻、开 TinyCore、按回车、抓图。
#
# 为什么做成一个脚本：这一段有六处只要漏一步就会得到一个看着像被测现象的读数 ——
#   1. 服务 ImagePath 指向 System32\drivers，只拷到 C:\ksword 会加载着旧驱动跑；
#   2. 覆盖前必须先 sc stop，不然拷贝被拒而脚本照样往下走；
#   3. vmx86 不重启，VMware 用的还是开机时缓存的原始能力值；
#   4. VMware 从 PowerShell Direct（session 0）起来，窗口在看不见的桌面上，
#      只能走 VNC 取画面；
#   5. isolinux 菜单要按回车，而按键同样只能走 VNC；
#   6. 构建产出的哈希要单独记下来对一遍 —— 两端一致只证明传输忠实。
param(
    [string] $VMName = 'KSword-HVM-Target',
    [string] $DriverPath = 'C:\Users\Felix\CLionProjects\KSword\Ksword5.1\x64\Release\KswordARK.sys',
    [string] $ShotDir,
    [switch] $SkipDriver
)

$ErrorActionPreference = 'Stop'
$vncScript = Join-Path $PSScriptRoot 'Get-VmwareVnc.ps1'
$built = (Get-FileHash $DriverPath -Algorithm SHA256).Hash
Write-Output "构建产出 sha256 = $built"

$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName $VMName -Credential $cred

Invoke-Command -Session $s -ScriptBlock {
    Get-Process -Name 'vmware', 'vmware-vmx' -ErrorAction SilentlyContinue |
        Stop-Process -Force
    Start-Sleep -Seconds 3
}

if (-not $SkipDriver) {
    # 停不下来就重启来宾。
    #
    # 实测这个卸载会卡在 StopPending 不动（常驻停不掉时驱动拒绝卸载），之后
    # 覆盖 System32\drivers 下那份必然报"文件被占用"，而脚本在那里抛出，
    # 留下一个半完成的部署。重启一分钟，比每次人工介入便宜，也比带着一份
    # 旧驱动往下跑安全 —— 后者会得到一份看着正常、实际测的是上一版的读数。
    $stopped = Invoke-Command -Session $s -ScriptBlock {
        Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList 'stop' -NoNewWindow -Wait | Out-Null
        & sc.exe stop KswordARK | Out-Null
        $n = 0
        while ((Get-Service KswordARK).Status -ne 'Stopped' -and $n -lt 30) {
            Start-Sleep -Milliseconds 500; $n++
        }
        Write-Output ("驱动停止状态：" + (Get-Service KswordARK).Status)
        return ((Get-Service KswordARK).Status -eq 'Stopped')
    }
    if (-not ($stopped | Select-Object -Last 1)) {
        Write-Output '卸载没完成，重启来宾'
        Remove-PSSession $s
        Restart-VM -Name $VMName -Force -Confirm:$false
        Start-Sleep -Seconds 45
        $tries = 0
        while ($tries -lt 24) {
            $s = New-PSSession -VMName $VMName -Credential $cred -ErrorAction SilentlyContinue
            if ($s) { break }
            Start-Sleep -Seconds 10
            $tries++
        }
        if (-not $s) { throw '来宾重启后连不上 PowerShell Direct' }
        Write-Output '来宾已重启并重新连上'
    }
    Copy-Item -ToSession $s -Path $DriverPath -Destination 'C:\ksword\KswordARK.sys' -Force
    Invoke-Command -Session $s -ArgumentList $built -ScriptBlock {
        param($built)
        # 覆盖**服务 ImagePath 指着的那一份**，不是某个假定的路径。
        #
        # 实测踩过：GUI 起来之后会把服务重新注册到它自带的那份
        # （C:\ksword\gui\KswordARK.sys，日期比当天的构建早六天）。部署脚本
        # 照旧往 System32\drivers 写、照旧核对那里的哈希、照旧两端一致 ——
        # 而加载的是另一份。之后所有读数描述的都是六天前的驱动，
        # 而且没有任何一处会报错。
        $imagePath = (Get-ItemProperty `
            'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK' -Name ImagePath).ImagePath
        $target = $imagePath -replace '^\\\?\?\\', ''
        if ($target -notmatch '^[A-Za-z]:\\') {
            $target = Join-Path $env:SystemRoot ($target -replace '^\\?SystemRoot\\?', '')
        }
        [IO.File]::Copy('C:\ksword\KswordARK.sys', $target, $true)
        # 另一份也一并同步，免得下次别人把 ImagePath 指回去时又加载到旧的
        $other = 'C:\Windows\System32\drivers\KswordARK.sys'
        if ($target -ne $other) { [IO.File]::Copy('C:\ksword\KswordARK.sys', $other, $true) }
        $h = (Get-FileHash $target -Algorithm SHA256).Hash
        if ($h -ne $built) { throw "ImagePath 指向的 $target 哈希是 $h，与构建产出不符" }
        "服务 ImagePath = $target"
        "该文件 sha256 = $h（与构建产出一致）"
        & sc.exe start KswordARK | Out-Null
        Start-Sleep -Seconds 2
        "驱动已起：" + (Get-Service KswordARK).Status
    }
}

Invoke-Command -Session $s -ScriptBlock {
    function Run($a) {
        $o = 'C:\ksword\ro.txt'
        $p = Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList $a -NoNewWindow `
                -Wait -PassThru -RedirectStandardOutput $o
        return @{ Exit = $p.ExitCode; Out = [IO.File]::ReadAllText($o) }
    }
    # 判据是 cpuid-view 的 hidden，不是状态位。
    #
    # `resident-nested` 与 `resident-nested-hidehv` 的**状态位完全一样**，两者都是
    # INITIALIZED … RESIDENT_ACTIVE RESIDENT_NESTED。按 RESIDENT_ACTIVE 判断
    # "已经在跑就跳过"，会在别人（比如 GUI 菜单）起了不隐藏的那一版时沿用它 ——
    # 而 VMware 的身份门排在能力门前面：CPUID 一看见 Microsoft Hv 就弹
    # "VMware Workstation and Hyper-V are not compatible"，连能力 MSR 都不会读。
    # 实测踩过：状态位一切正常，VMware 就是不启动。
    $needStart = $true
    $st = (Run @('--json', 'status')).Out | ConvertFrom-Json
    if ($st.stateNames -contains 'RESIDENT_ACTIVE') {
        $view = $null
        try { $view = (Run @('--json', 'cpuid-view')).Out | ConvertFrom-Json } catch { }
        if ($view -and $view.hidden) {
            $needStart = $false
        } else {
            Write-Output '常驻在跑但没有隐藏 hypervisor，停掉重起'
            $r = Run @('stop')
            if ($r.Exit -ne 0) { throw "stop 退出码 $($r.Exit)" }
        }
    }
    if ($needStart) {
        $st = (Run @('--json', 'status')).Out | ConvertFrom-Json
        foreach ($cmd in 'prepare', 'self-test', 'resident-nested-hidehv') {
            if ($cmd -eq 'prepare' -and
                ($st.stateNames -contains 'RESOURCES_READY')) { continue }
            $r = Run @($cmd)
            if ($r.Exit -ne 0) { throw "$cmd 退出码 $($r.Exit)" }
        }
    }
    $st = (Run @('--json', 'status')).Out | ConvertFrom-Json
    $view = (Run @('--json', 'cpuid-view')).Out | ConvertFrom-Json
    if (-not $view.hidden) {
        throw "常驻起来了但 cpuid-view 的 hidden 仍为假，VMware 会拒绝启动"
    }
    "状态位 = " + ($st.stateNames -join ' ')
    "cpuid-view hidden = " + $view.hidden + "（VMware 的身份门看的就是这个）"
    # vmx86 重启：VMware 只在这个驱动起来时问一次能力 MSR
    & sc.exe stop vmx86 | Out-Null
    Start-Sleep -Seconds 1
    & sc.exe start vmx86 | Out-Null
    "vmx86 = " + (Get-Service vmx86).Status

    $vmx = 'C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    Start-Process -FilePath 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe' `
        -ArgumentList @('-T', 'ws', 'start', "`"$vmx`"") -NoNewWindow
    Start-Sleep -Seconds 40
    "vmware-vmx = " + @(Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue).Count
}

# isolinux 的菜单：回车（X11 keysym 0xFF0D）。菜单自己也会超时引导，
# 但那要等六十秒，而且**倒计时在走本身就是时钟通了的判据**，按一下更快。
Invoke-Command -Session $s -FilePath $vncScript `
    -ArgumentList '127.0.0.1', 5900, 'C:\vmware\shot-menu.png', @(65293)

Start-Sleep -Seconds 75
Invoke-Command -Session $s -FilePath $vncScript `
    -ArgumentList '127.0.0.1', 5900, 'C:\vmware\shot-boot.png'

if ($ShotDir) {
    foreach ($f in 'shot-menu.png', 'shot-boot.png') {
        Copy-Item -FromSession $s -Path "C:\vmware\$f" -Destination (Join-Path $ShotDir $f) -Force
    }
    Write-Output ("截图已取回 " + $ShotDir)
}
Remove-PSSession $s
