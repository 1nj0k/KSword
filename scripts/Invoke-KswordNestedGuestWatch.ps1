<#
.SYNOPSIS
    在测试机里起带嵌套派发的常驻，取一份基线，等你在来宾里开一台**第三方
    hypervisor 的虚拟机**，然后把前后两份 status 的差值打出来。

.DESCRIPTION
    必须以**管理员**在宿主上运行。

    这个脚本解决的是一个很具体的问题：证据本身 `hvm_ctl status` 早就有了，
    但它一次吐三十多个字段加一张退出直方图，而"第三方 hypervisor 在我们
    下面跑起来没有"这个问题只关心其中**九个退出原因**和**三个计数器**的
    **变化量**。靠肉眼比两大坨输出，既慢又会看漏。

    读法（下面的编号与 hvm_nested.c 的 KSW_VMX_EXIT_* 一致）：

      27 VMXON     对方有没有真的去用 VT-x。**零就是零**，说明它压根没走到
                   这条路上 —— 要么用了别的后端（Hyper-V 平台 API 之类），
                   要么在更早的地方就自己报错退了。这时看我们的计数器没有
                   意义，先去看对方的日志。
      21 VMPTRLD   载入了几张 vmcs12。一台虚拟机一个 vCPU 至少一张。
      23/25 VMREAD/VMWRITE
                   它在配 VMCS。数量大是正常的（几百到几千），**分布**比
                   总数有用：只有 VMWRITE 没有 VMLAUNCH，就是配到一半放弃了。
      20/24 VMLAUNCH/VMRESUME
                   真正的进入尝试次数。
      26 VMXOFF    对方主动退出 VMX —— 通常意味着它放弃了。

    再配合三个计数器：

      nestedL2LaunchRefusedCount   我们**拒绝**了多少次进入。
                                   进入成功次数 = (20+24 的增量) - (这个增量)。
                                   驱动没有"成功进入"的全局计数器，只能这样算，
                                   所以这里报的是**推算值，不是直接读数**。
      nestedVmcs12EvictionCount    vmcs12 池被挤掉过几次。对方 vCPU 数超过
                                   池深（8）时才该动；平时动了就是异常。
      nestedFuseTripCount          熔断跳闸：同一个 RIP 反复退出且不前进。
                                   非零表示我们把某个 L2 卡在了原地。

    还有 lastVmInstructionError —— VMLAUNCH/VMRESUME 失败时它是**唯一**说明
    失败原因的字段。

.PARAMETER WaitSeconds
    基线之后的观察窗口。窗口里每 PollSeconds 秒取一次 status 存进记录，
    所以来宾中途蓝屏/挂死时，**最后一次成功的采样就是飞行记录**。
    在有交互的控制台里按任意键可以提前结束窗口。

.PARAMETER Mode
    nested  = 起 resident-nested（默认）
    hidehv  = 起 resident-nested-hidehv：另对来宾**用户态**的 CPUID 隐藏
              hypervisor 身份。

    实机量到的第一个拦路读数不是能力而是身份：VMware Workstation 17.6 用 CPUID
    认出外层是 Hyper-V 就去要 WHP，要不到就在装载任何虚拟机之前拒绝启动
    （`[msg.vmx.nestedHyperV]`）。hidehv 解决的正是这一件事。

    模式不同的两次常驻，从状态位上**分不出来**。所以这里的规矩是：常驻已经在跑
    而你又指定了模式，就先 stop 再按你要的模式重起 —— 宁可多停一次，也不要一次
    测量在与你以为的不同模式下跑完并报通过。用 -SkipBringUp 可以关掉这个行为。

.PARAMETER SkipBringUp
    不碰生命周期，只取基线 + 观察 + 差值。常驻已经在跑时用这个。

.PARAMETER StopWhenDone
    观察结束后把常驻停掉。默认**不停** —— 留着现场好接着看。

.PARAMETER SelfTest
    不连虚拟机，用两组合成的 status 过一遍差值与判读，确认这段逻辑本身是好的。
    它排在观察窗口之后执行，真跑的时候在那里崩掉就白等一轮，所以先离线验。

.EXAMPLE
    .\Invoke-KswordNestedGuestWatch.ps1 -SelfTest
    .\Invoke-KswordNestedGuestWatch.ps1
    .\Invoke-KswordNestedGuestWatch.ps1 -WaitSeconds 600 -StopWhenDone
#>
[CmdletBinding()]
param(
    [string] $VMName        = 'KSword-HVM-Target',
    [string] $GuestUser     = 'felix',
    [string] $GuestPassword = 'password',
    [ValidateSet('nested','hidehv')]
    [string] $Mode          = 'nested',
    [int]    $WaitSeconds   = 300,
    [int]    $PollSeconds   = 10,
    [string] $ResultPath,
    [switch] $SkipBringUp,
    [switch] $StopWhenDone,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$tool = Join-Path $repo 'tools\hvm_ctl\hvm_ctl.exe'

if (-not $ResultPath) {
    $logDir = Join-Path $repo 'docs\next\logs'
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
    $ResultPath = Join-Path $logDir ('hvm-guestwatch-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}

$record = [ordered]@{
    schema     = 'ksword.hvm.guestwatch/1'
    startedUtc = (Get-Date).ToUniversalTime().ToString('o')
    vmName     = $VMName
    steps      = New-Object System.Collections.ArrayList
    samples    = New-Object System.Collections.ArrayList
    baseline   = $null
    final      = $null
    delta      = $null
    verdict    = 'NOT_RUN'
    notes      = New-Object System.Collections.ArrayList
}

function Add-Step {
    param([string] $Name, [string] $Outcome, $Data, [string] $Note)
    $entry = [ordered]@{
        name = $Name; utc = (Get-Date).ToUniversalTime().ToString('o'); outcome = $Outcome
    }
    if ($null -ne $Data) { $entry.data = $Data }
    if ($Note)           { $entry.note = $Note }
    [void]$record.steps.Add($entry)
    $color = switch ($Outcome) { 'OK' { 'Green' } 'SKIP' { 'DarkGray' } 'BLOCKED' { 'Yellow' } default { 'Red' } }
    Write-Host ("  [{0,-7}] {1}{2}" -f $Outcome, $Name, $(if ($Note) { "  — $Note" })) -ForegroundColor $color
}

function Save-Record {
    $record.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
    [IO.File]::WriteAllText(
        $ResultPath,
        ($record | ConvertTo-Json -Depth 12),
        (New-Object Text.UTF8Encoding($false)))
}

function Invoke-Guest {
    param([scriptblock] $Script, [object[]] $ScriptArgs)
    if ($null -eq $ScriptArgs -or $ScriptArgs.Count -eq 0) {
        Invoke-Command -VMName $VMName -Credential $script:cred -ScriptBlock $Script
    } else {
        Invoke-Command -VMName $VMName -Credential $script:cred -ScriptBlock $Script -ArgumentList $ScriptArgs
    }
}

# 空文件读回来是 $null，穿过 PowerShell Direct 的序列化边界会变成一个**空的
# PSCustomObject** —— 它在 if 里为真却没有任何字符串方法。当文本用的都先过这里。
function ConvertTo-Text {
    param($Value)
    if ($null -eq $Value) { return '' }
    if ($Value -is [string]) { return $Value }
    $s = "$Value"
    if ($s -eq '' -or $s -eq 'System.Management.Automation.PSCustomObject') { return '' }
    return $s
}

function Invoke-HvmCtl {
    param([string] $Command)
    $raw = Invoke-Guest {
        param($cmdName)
        $o = 'C:\ksword\hvm_out.txt'
        $e = 'C:\ksword\hvm_err.txt'
        Remove-Item $o, $e -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath 'C:\ksword\hvm_ctl.exe' -ArgumentList @('--json', $cmdName) `
                 -NoNewWindow -Wait -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
        $outText = ''
        $errText = ''
        if (Test-Path $o) { $outText = [IO.File]::ReadAllText($o) }
        if (Test-Path $e) { $errText = [IO.File]::ReadAllText($e) }
        [ordered]@{ Exit = $p.ExitCode; Out = $outText; Err = $errText }
    } -ScriptArgs @($Command)

    $stdout = ConvertTo-Text $raw.Out
    $parsed = $null
    if ($stdout) { try { $parsed = $stdout | ConvertFrom-Json } catch { $parsed = $null } }
    return [ordered]@{ Exit = $raw.Exit; Json = $parsed; Stdout = $stdout; Stderr = (ConvertTo-Text $raw.Err) }
}

function Test-StateBit {
    param($StateNames, [string] $Bit)
    if ($null -eq $StateNames) { return $false }
    return [bool]($StateNames -contains $Bit)
}

function Get-GuestBootTime {
    try {
        $t = Invoke-Command -VMName $VMName -Credential $script:cred -ErrorAction Stop `
                 -ScriptBlock { (Get-CimInstance Win32_OperatingSystem).LastBootUpTime }
        return [datetime]$t
    } catch { return $null }
}

# ---------------------------------------------------------------------------
# 退出原因
# ---------------------------------------------------------------------------
# 编号 -> 名字。与 tools\hvm_ctl\hvm_ctl.c 的 ExitReasonName 和 hvm_nested.c 的
# KSW_VMX_EXIT_* 是同一套；这里只列这条线上要看的。改任何一处都要三边对齐。
$reasonNames = @{
    0='EXCEPTION_OR_NMI'; 1='EXTERNAL_INTERRUPT'; 2='TRIPLE_FAULT'; 7='INTERRUPT_WINDOW'
    9='TASK_SWITCH'; 10='CPUID'; 12='HLT'; 13='INVD'; 14='INVLPG'; 15='RDPMC'; 16='RDTSC'
    18='VMCALL'; 19='VMCLEAR'; 20='VMLAUNCH'; 21='VMPTRLD'; 22='VMPTRST'; 23='VMREAD'
    24='VMRESUME'; 25='VMWRITE'; 26='VMXOFF'; 27='VMXON'; 28='MOV_CR'; 29='MOV_DR'
    30='IO_INSTRUCTION'; 31='RDMSR'; 32='WRMSR'; 33='VM_ENTRY_FAILURE_GUEST_STATE'
    34='VM_ENTRY_FAILURE_MSR_LOADING'; 37='MONITOR_TRAP_FLAG'; 48='EPT_VIOLATION'
    49='EPT_MISCONFIGURATION'; 50='INVEPT'; 51='RDTSCP'; 52='VMX_PREEMPTION_TIMER'
    53='INVVPID'; 54='WBINVD'; 55='XSETBV'; 58='INVPCID'; 59='VMFUNC'
}
# 只有另一个 hypervisor 在我们下面跑时才会出现的那一族。
$vmxReasons = @(19, 20, 21, 22, 23, 24, 25, 26, 27, 50, 53)

function Get-ReasonName {
    param([int] $Reason)
    if ($reasonNames.ContainsKey($Reason)) { return $reasonNames[$Reason] }
    return '见 SDM Appendix C'
}

# exitReasonCount 只发非零项，键是字符串化的原因编号。缺键 = 零。
function Get-ReasonMap {
    param($Json)
    $map = @{}
    if ($null -eq $Json -or $null -eq $Json.exitReasonCount) { return $map }
    foreach ($p in $Json.exitReasonCount.PSObject.Properties) {
        $map[[int]$p.Name] = [uint64]$p.Value
    }
    return $map
}

function Get-ReasonDelta {
    param($Before, $After, [int] $Reason)
    $b = 0; $a = 0
    if ($Before.ContainsKey($Reason)) { $b = $Before[$Reason] }
    if ($After.ContainsKey($Reason))  { $a = $After[$Reason] }
    return [int64]$a - [int64]$b
}

# ---------------------------------------------------------------------------
# 差值与判读。抽成函数有两个理由：它是整条流程里唯一有算术的部分，而且它只在
# 观察窗口**结束之后**才第一次执行 —— 真跑时崩在这里就白等一轮，所以要能离线验。
# ---------------------------------------------------------------------------
function Show-Delta {
    param($Base, $Final)

    $bMap = Get-ReasonMap $Base
    $fMap = Get-ReasonMap $Final

    $d = [ordered]@{
        vmExitCount = [int64]$Final.vmExitCount - [int64]$Base.vmExitCount
        refused     = [int64]$Final.nestedL2LaunchRefusedCount - [int64]$Base.nestedL2LaunchRefusedCount
        evicted     = [int64]$Final.nestedVmcs12EvictionCount - [int64]$Base.nestedVmcs12EvictionCount
        fuseTrips   = [int64]$Final.nestedFuseTripCount - [int64]$Base.nestedFuseTripCount
        lastVmInstructionError = $Final.lastVmInstructionError
        reasons     = [ordered]@{}
        verdict     = 'NOT_RUN'
    }
    foreach ($r in (@($bMap.Keys) + @($fMap.Keys) | Sort-Object -Unique)) {
        $delta = Get-ReasonDelta $bMap $fMap ([int]$r)
        if ($delta -ne 0) { $d.reasons["$r"] = $delta }
    }

    $vmxon   = Get-ReasonDelta $bMap $fMap 27
    $vmptrld = Get-ReasonDelta $bMap $fMap 21
    $vmwrite = Get-ReasonDelta $bMap $fMap 25
    $vmread  = Get-ReasonDelta $bMap $fMap 23
    $vmxoff  = Get-ReasonDelta $bMap $fMap 26
    $entries = (Get-ReasonDelta $bMap $fMap 20) + (Get-ReasonDelta $bMap $fMap 24)

    Write-Host ''
    Write-Host '=== 差值 ===' -ForegroundColor Cyan
    Write-Host ("  总退出       : +{0}" -f $d.vmExitCount)
    Write-Host ("  拒绝 L2 启动 : +{0}" -f $d.refused)   -ForegroundColor $(if ($d.refused   -gt 0) { 'Yellow' } else { 'Gray' })
    Write-Host ("  vmcs12 驱逐  : +{0}" -f $d.evicted)   -ForegroundColor $(if ($d.evicted   -gt 0) { 'Yellow' } else { 'Gray' })
    Write-Host ("  熔断跳闸     : +{0}" -f $d.fuseTrips) -ForegroundColor $(if ($d.fuseTrips -gt 0) { 'Red'    } else { 'Gray' })
    Write-Host ("  末次指令错误 : {0}" -f $d.lastVmInstructionError)

    Write-Host ''
    Write-Host '  --- VMX 指令类退出（只有别的 hypervisor 在我们下面跑才会有）---'
    $anyVmx = $false
    foreach ($r in $vmxReasons) {
        $delta = Get-ReasonDelta $bMap $fMap $r
        if ($delta -eq 0) { continue }
        $anyVmx = $true
        Write-Host ("    +{0,-10} reason={1,-3} {2}" -f $delta, $r, (Get-ReasonName $r)) -ForegroundColor Green
    }
    if (-not $anyVmx) { Write-Host '    <一条都没有>' -ForegroundColor DarkGray }

    Write-Host ''
    Write-Host '  --- 其余退出原因的增量 ---'
    # 先物化成带真实属性的对象再排。Sort-Object 用脚本块取键时，键要是它比不了
    # 的东西就**静默保持原序**并照常返回 —— 看上去排过了，实际没有。
    $others = @()
    foreach ($k in $d.reasons.Keys) {
        if ([int]$k -in $vmxReasons) { continue }
        $others += [pscustomobject]@{
            Reason = [int]$k; Delta = [int64]$d.reasons[$k]; Name = (Get-ReasonName ([int]$k))
        }
    }
    if ($others.Count -eq 0) {
        Write-Host '    <无变化>' -ForegroundColor DarkGray
    } else {
        foreach ($o in ($others | Sort-Object -Property Delta -Descending)) {
            Write-Host ("    +{0,-10} reason={1,-3} {2}" -f $o.Delta, $o.Reason, $o.Name)
        }
    }

    Write-Host ''
    Write-Host '=== 判读 ===' -ForegroundColor Cyan
    if ($vmxon -eq 0) {
        $d.verdict = 'NO_VMXON'
        Write-Host '  VMXON 增量为 0 —— 来宾里的 hypervisor **没有走 VT-x 这条路**。' -ForegroundColor Yellow
        Write-Host '  我们这一侧没有任何可判的读数。先确认它是不是真的开起来了、' -ForegroundColor Yellow
        Write-Host '  以及它选了哪个后端（走 Hyper-V 平台 API 的话根本不碰 VMX 指令）。' -ForegroundColor Yellow
    } elseif ($entries -eq 0) {
        $d.verdict = 'VMXON_BUT_NO_ENTRY'
        Write-Host ("  VMXON +{0}，VMPTRLD +{1}，VMWRITE +{2}，VMREAD +{3}，但**一次进入尝试都没有**。" -f `
            $vmxon, $vmptrld, $vmwrite, $vmread) -ForegroundColor Yellow
        Write-Host '  它进了 VMX、开始配 VMCS，然后放弃了。多半是某个能力位我们没放行 ——' -ForegroundColor Yellow
        Write-Host '  对方读能力 MSR 发现缺东西就会自己退。下一步看能力过滤的允许集。' -ForegroundColor Yellow
    } elseif ($d.refused -ge $entries) {
        $d.verdict = 'ALL_ENTRIES_REFUSED'
        Write-Host ("  进入尝试 {0} 次，我们拒了 {1} 次 —— **全被拒**。" -f $entries, $d.refused) -ForegroundColor Red
        Write-Host ("  末次指令错误 {0}。拒绝理由在驱动的 L2 启动校验里。" -f $d.lastVmInstructionError) -ForegroundColor Red
    } else {
        $d.verdict = 'L2_ENTERED'
        Write-Host ("  进入尝试 {0} 次，被拒 {1} 次 ⇒ **推算成功进入 {2} 次**。" -f `
            $entries, $d.refused, ($entries - $d.refused)) -ForegroundColor Green
        Write-Host '  （驱动没有"成功进入"的全局计数器，这是减出来的，不是直接读数。）' -ForegroundColor DarkGray
        if ($d.fuseTrips -gt 0) {
            $d.verdict = 'L2_ENTERED_BUT_STUCK'
            Write-Host ("  但熔断跳闸 +{0} —— 有 L2 卡在同一个 RIP 上不前进。" -f $d.fuseTrips) -ForegroundColor Red
        }
        if ($vmxoff -gt 0) {
            Write-Host ("  VMXOFF +{0} —— 对方主动退出了 VMX。" -f $vmxoff) -ForegroundColor Yellow
        }
    }
    return $d
}

# ---------------------------------------------------------------------------
# 离线自检：合成四种典型局面，确认差值算术与判读分支都是好的。
# ---------------------------------------------------------------------------
function New-FakeStatus {
    param([hashtable] $Reasons, [int] $Refused = 0, [int] $Evicted = 0, [int] $Fuse = 0,
          [int] $Exits = 0, [int] $LastErr = 0)
    $rc = [pscustomobject]@{}
    foreach ($k in $Reasons.Keys) { $rc | Add-Member -NotePropertyName "$k" -NotePropertyValue $Reasons[$k] }
    return [pscustomobject]@{
        generation                 = 1
        vmExitCount                = $Exits
        nestedL2LaunchRefusedCount = $Refused
        nestedVmcs12EvictionCount  = $Evicted
        nestedFuseTripCount        = $Fuse
        lastVmInstructionError     = $LastErr
        exitReasonCount            = $rc
    }
}

function Invoke-SelfTest {
    $cases = @(
        @{ Name='没碰 VT-x';       Expect='NO_VMXON'
           B=(New-FakeStatus @{10=100; 31=50} 0 0 0 1000)
           F=(New-FakeStatus @{10=400; 31=90} 0 0 0 4000) }
        @{ Name='进了 VMX 没进 L2'; Expect='VMXON_BUT_NO_ENTRY'
           B=(New-FakeStatus @{10=100} 0 0 0 1000)
           F=(New-FakeStatus @{10=120; 27=1; 21=2; 25=900; 23=300; 26=1} 0 0 0 2400) }
        @{ Name='全被拒';           Expect='ALL_ENTRIES_REFUSED'
           B=(New-FakeStatus @{10=100} 0 0 0 1000)
           F=(New-FakeStatus @{10=120; 27=1; 21=2; 25=900; 20=4} 4 0 0 2100 7) }
        @{ Name='进去了但卡住';     Expect='L2_ENTERED_BUT_STUCK'
           B=(New-FakeStatus @{10=100} 0 0 0 1000)
           F=(New-FakeStatus @{10=120; 27=1; 21=2; 25=900; 20=4; 24=600} 1 0 3 9000) }
        @{ Name='进去了';           Expect='L2_ENTERED'
           B=(New-FakeStatus @{10=100} 2 1 0 1000)
           F=(New-FakeStatus @{10=120; 27=1; 21=2; 25=900; 20=4; 24=600} 2 1 0 9000) }
    )
    $failed = 0
    foreach ($c in $cases) {
        Write-Host ''
        Write-Host ("######## 自检用例：{0}（期望 {1}）########" -f $c.Name, $c.Expect) -ForegroundColor Magenta
        $got = Show-Delta $c.B $c.F
        if ($got.verdict -eq $c.Expect) {
            Write-Host ("  [OK]   判定 {0}" -f $got.verdict) -ForegroundColor Green
        } else {
            Write-Host ("  [FAIL] 判定 {0}，期望 {1}" -f $got.verdict, $c.Expect) -ForegroundColor Red
            $failed++
        }
    }
    # 基线非零的那一例顺带验了"计数器是常驻期内累计的"：拒绝 2->2 必须是 +0。
    Write-Host ''
    if ($failed -eq 0) {
        Write-Host ("自检 {0}/{0} 通过" -f $cases.Count) -ForegroundColor Green
        return 0
    }
    Write-Host ("自检 {0} 例失败" -f $failed) -ForegroundColor Red
    return 1
}

if ($SelfTest) { exit (Invoke-SelfTest) }

# ---------------------------------------------------------------------------
Import-Module Hyper-V -ErrorAction Stop
if (-not (Test-Path $tool)) { throw "缺少 $tool（先跑 scripts\Build-KswordHvmTools.ps1）" }
$script:cred = New-Object System.Management.Automation.PSCredential(
    $GuestUser, (ConvertTo-SecureString $GuestPassword -AsPlainText -Force))

$exitCode = 0
try {
    $vm = Get-VM -Name $VMName -ErrorAction Stop
    $nestedExposed = (Get-VMProcessor -VMName $VMName).ExposeVirtualizationExtensions
    $record.vm = [ordered]@{
        state = "$($vm.State)"; vcpu = $vm.ProcessorCount; nested = [bool]$nestedExposed
    }
    Write-Host '=== KSword 嵌套来宾观察 ===' -ForegroundColor Cyan
    Write-Host ("虚拟机 {0}  {1}  {2} vCPU  嵌套={3}" -f $vm.Name, $vm.State, $vm.ProcessorCount, $nestedExposed)
    Write-Host ("记录 -> {0}`n" -f $ResultPath) -ForegroundColor DarkGray

    if ($vm.State -ne 'Running') { throw "虚拟机不在运行状态（$($vm.State)）。先 Start-VM。" }
    if (-not $nestedExposed) { throw '嵌套虚拟化没开 —— 关机后 Set-VMProcessor -ExposeVirtualizationExtensions $true' }

    $bootBefore = Get-GuestBootTime

    # ---- 送工具（每次都送，保证跑的是刚编译的那个）------------------------
    Invoke-Guest { New-Item -ItemType Directory -Force -Path 'C:\ksword' | Out-Null } | Out-Null
    try {
        Copy-VMFile -Name $VMName -SourcePath $tool -DestinationPath 'C:\ksword\hvm_ctl.exe' `
                    -CreateFullPath -FileSource Host -Force -ErrorAction Stop
    } catch {
        $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($tool))
        Invoke-Guest {
            param($d, $t)
            [IO.File]::WriteAllBytes($t, [Convert]::FromBase64String($d))
        } -ScriptArgs @($b64, 'C:\ksword\hvm_ctl.exe')
    }
    Add-Step 'deploy:hvm_ctl' 'OK' ((Get-Item $tool).Length)

    # ---- 生命周期：只补到"带嵌套派发的常驻在跑" ---------------------------
    $st = Invoke-HvmCtl 'status'
    if ($st.Exit -ne 0 -or $null -eq $st.Json) {
        Add-Step 'status' 'FAIL' $st.Stderr '设备查询失败 —— 驱动可能没加载'
        $record.verdict = 'BLOCKED'
        [void]$record.notes.Add('hvm_ctl status 拿不到结果。先跑 Deploy-KswordDriverToVm.ps1。')
        $exitCode = 1
        return
    }
    $names = $st.Json.stateNames
    Add-Step 'status:before-bringup' 'OK' $null ("状态位 " + ($names -join ' '))

    if ($SkipBringUp) {
        Add-Step 'bring-up' 'SKIP' $null '按 -SkipBringUp 跳过；只观察不改生命周期'
        if (-not (Test-StateBit $names 'RESIDENT_ACTIVE')) {
            [void]$record.notes.Add('常驻没在跑，而 -SkipBringUp 不去起它 —— 这一轮不可能有任何嵌套读数。')
        }
    } else {
        if ((Test-StateBit $names 'FAULTED') -or (Test-StateBit $names 'ROLLBACK_REQUIRED')) {
            $r = Invoke-HvmCtl 'reset-fault'
            Add-Step 'reset-fault' $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json `
                     '状态里带 FAULTED/ROLLBACK_REQUIRED，不清掉 resident 必被拒'
            if ($r.Exit -ne 0) { $record.verdict = 'FAIL'; $exitCode = $r.Exit; return }
            $names = $r.Json.newStateNames
        }
        if (Test-StateBit $names 'RESOURCES_READY') {
            Add-Step 'prepare' 'SKIP' $null 'RESOURCES_READY 已置位；重复 prepare 会把状态打成 FAULTED'
        } else {
            $r = Invoke-HvmCtl 'prepare'
            Add-Step 'prepare' $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json
            if ($r.Exit -ne 0) { $record.verdict = 'FAIL'; $exitCode = $r.Exit; return }
            $names = $r.Json.newStateNames
        }
        # 常驻要求 SELF_TEST_PASSED，缺了会被判 NOT_PREPARED（0xC00000A3）并顺手
        # 把状态打成 FAULTED —— 那个状态名读起来像"没 prepare"，而 prepare 明明
        # 刚返回 0，两个读数对不上会把人带到完全错的方向。补这一级不是保险起见。
        if (-not (Test-StateBit $names 'SELF_TEST_PASSED')) {
            $r = Invoke-HvmCtl 'self-test'
            Add-Step 'self-test' $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json `
                     '逐处理器 VMXON/VMXOFF；常驻的前置条件'
            if ($r.Exit -ne 0) { $record.verdict = 'FAIL'; $exitCode = $r.Exit; return }
            $names = $r.Json.newStateNames
        } else {
            Add-Step 'self-test' 'SKIP' $null 'SELF_TEST_PASSED 已置位'
        }

        # RESIDENT_ACTIVE 置位**不等于**它是我们要的那个模式：普通 resident 起的
        # 常驻里 VMX 指令被注 #UD，而 nested 与 hidehv 两种常驻在状态位上完全
        # 一样。沿用一个来历不明的常驻，等于让整轮测量在未知模式下跑完。
        if (Test-StateBit $names 'RESIDENT_ACTIVE') {
            $r = Invoke-HvmCtl 'stop'
            Add-Step 'stop' $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json `
                     '常驻已在跑但模式不可知（状态位分不出 nested / hidehv）；先停掉再按本轮要的模式重起'
            if ($r.Exit -ne 0) { $record.verdict = 'FAIL'; $exitCode = $r.Exit; return }
        }
        $residentVerb = if ($Mode -eq 'hidehv') { 'resident-nested-hidehv' } else { 'resident-nested' }
        $r = Invoke-HvmCtl $residentVerb
        Add-Step $residentVerb $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json `
                 '全处理器进 VMX 常驻，并允许来宾执行 VMX 指令'
        if ($r.Exit -ne 0) {
            $record.verdict = 'FAIL'
            [void]$record.notes.Add("$residentVerb 返回 $($r.Json.statusName)")
            $exitCode = $r.Exit
            return
        }
    }

    # 常驻起来之后立刻读一次来宾用户态的 CPUID。这是 hidehv 唯一的直接判据 ——
    # 状态位上看不出模式，而这两个值就是 VMware 用来判断外层身份的那两个。
    $cv = Invoke-HvmCtl 'cpuid-view'
    if ($null -ne $cv.Json) {
        $record.cpuidView = $cv.Json
        $hidden = [bool]$cv.Json.hidden
        $note = "hypervisor 位={0}  厂商=`"{1}`"" -f $cv.Json.hypervisorPresent, $cv.Json.hvVendor
        if ($Mode -eq 'hidehv' -and -not $hidden) {
            Add-Step 'cpuid-view' 'FAIL' $cv.Json ("要求隐藏但用户态仍看得见：" + $note)
            [void]$record.notes.Add('hidehv 没有生效 —— 后面就算 VMware 起不来，也不是嵌套能力的问题。')
        } elseif ($Mode -eq 'hidehv') {
            Add-Step 'cpuid-view' 'OK' $cv.Json ("隐藏生效：" + $note)
        } else {
            Add-Step 'cpuid-view' 'OK' $cv.Json $note
        }
    } else {
        Add-Step 'cpuid-view' 'BLOCKED' $null '读不到；本轮无法确认来宾看到的身份'
    }

    # ---- 基线 --------------------------------------------------------------
    $base = Invoke-HvmCtl 'status'
    if ($base.Exit -ne 0 -or $null -eq $base.Json) {
        Add-Step 'baseline' 'FAIL' $base.Stderr; $record.verdict = 'FAIL'; $exitCode = 1; return
    }
    $record.baseline = $base.Json
    Add-Step 'baseline' 'OK' $null ("vmExitCount={0} 拒绝={1} 驱逐={2} 熔断={3}" -f `
        $base.Json.vmExitCount, $base.Json.nestedL2LaunchRefusedCount,
        $base.Json.nestedVmcs12EvictionCount, $base.Json.nestedFuseTripCount)

    # ---- 观察窗口 ----------------------------------------------------------
    Write-Host ''
    Write-Host '>>> 现在去来宾里，在第三方 hypervisor 里开虚拟机。' -ForegroundColor Yellow
    Write-Host (">>> 窗口 {0} 秒，每 {1} 秒采一次样；按任意键提前结束。" -f $WaitSeconds, $PollSeconds) -ForegroundColor Yellow
    Write-Host ''

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    $lastAlive = $true
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds $PollSeconds
        $poll = $null
        try { $poll = Invoke-HvmCtl 'status' } catch { $poll = $null }
        if ($null -eq $poll -or $null -eq $poll.Json) {
            # 来宾没应答。**这本身就是读数** —— 上一次成功的采样是飞行记录。
            $lastAlive = $false
            Add-Step 'poll' 'BLOCKED' $null '来宾不应答；上一次采样即为最后现场'
            break
        }
        [void]$record.samples.Add([ordered]@{
            utc     = (Get-Date).ToUniversalTime().ToString('o')
            exits   = $poll.Json.vmExitCount
            refused = $poll.Json.nestedL2LaunchRefusedCount
            evicted = $poll.Json.nestedVmcs12EvictionCount
            fuse    = $poll.Json.nestedFuseTripCount
            lastErr = $poll.Json.lastVmInstructionError
            reasons = $poll.Json.exitReasonCount
        })
        $vmxSeen = 0L
        $m = Get-ReasonMap $poll.Json
        foreach ($r in $vmxReasons) { if ($m.ContainsKey($r)) { $vmxSeen += [int64]$m[$r] } }
        Write-Host ("  采样 exits={0} VMX指令类={1} 拒绝={2} 熔断={3}" -f `
            $poll.Json.vmExitCount, $vmxSeen,
            $poll.Json.nestedL2LaunchRefusedCount, $poll.Json.nestedFuseTripCount) -ForegroundColor DarkGray

        $keyed = $false
        try { $keyed = [Console]::KeyAvailable } catch { $keyed = $false }
        if ($keyed) { try { [void][Console]::ReadKey($true) } catch { }; break }
    }

    # ---- 终局 --------------------------------------------------------------
    if (-not $lastAlive) {
        $bootAfter = Get-GuestBootTime
        if ($null -eq $bootAfter) {
            Add-Step 'final' 'BLOCKED' $null '来宾仍不应答 —— 挂死或正在重启'
            $record.verdict = 'GUEST_UNRESPONSIVE'
        } elseif ($null -ne $bootBefore -and [math]::Abs(($bootAfter - $bootBefore).TotalSeconds) -ge 2) {
            Add-Step 'final' 'FAIL' $null "来宾重启过（$bootBefore -> $bootAfter）；计数器已清零，差值无意义"
            $record.verdict = 'GUEST_REBOOTED'
        } else {
            Add-Step 'final' 'BLOCKED' $null '来宾中途失联但没重启 —— 现场在 samples 的最后一条'
            $record.verdict = 'GUEST_UNRESPONSIVE'
        }
        $exitCode = 1
        return
    }

    $fin = Invoke-HvmCtl 'status'
    if ($fin.Exit -ne 0 -or $null -eq $fin.Json) {
        Add-Step 'final' 'FAIL' $fin.Stderr; $record.verdict = 'FAIL'; $exitCode = 1; return
    }
    $record.final = $fin.Json

    # generation 变了说明中间被 teardown/prepare 过，计数器是另一条命的。
    if ($fin.Json.generation -ne $base.Json.generation) {
        Add-Step 'final' 'BLOCKED' $null ("generation {0} -> {1}：中途重建过资源，差值不可比" -f `
            $base.Json.generation, $fin.Json.generation)
        $record.verdict = 'GENERATION_CHANGED'
        $exitCode = 1
        return
    }
    Add-Step 'final' 'OK' $null

    $record.delta = Show-Delta $base.Json $fin.Json
    $record.verdict = $record.delta.verdict

    if ($StopWhenDone) {
        $r = Invoke-HvmCtl 'stop'
        Add-Step 'stop' $(if ($r.Exit -eq 0) { 'OK' } else { 'FAIL' }) $r.Json
    } else {
        Add-Step 'stop' 'SKIP' $null '默认不停常驻；要停加 -StopWhenDone 或单独跑 hvm_ctl stop'
    }
}
catch {
    $record.verdict = 'ERROR'
    [void]$record.notes.Add("$($_.Exception.Message)")
    Write-Host ("!! $($_.Exception.Message)") -ForegroundColor Red
    $exitCode = 1
}
finally {
    Save-Record
    Write-Host ''
    Write-Host ("判定 {0}  记录 {1}" -f $record.verdict, $ResultPath) -ForegroundColor Cyan
}

exit $exitCode
