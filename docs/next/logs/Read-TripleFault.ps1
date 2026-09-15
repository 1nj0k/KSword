# 宿主侧：把靶机事件环里三重故障现场那几行捞出来念一遍。
#
# 只念 0xD3（vmcs02 当时装着的三张描述符表 + 与 vmcs12 的逐字段掩码）和
# 0xD2（L1 一共往 IDTR 基址写过几次、最后写的是什么）。这两行必须一起看：
# 单看 0xD3 的 "IDTR 基址 = 0" 分不清"L1 从没写过"和"我们把它弄丢了"，
# Linux 的 IDT 在 0xFFFFFE0000000000，低 32 位恰好全零，两种解释给出同一个值。
#
# 环一次只回 64 行，必须翻页；翻不到就打"(没有这一行)"，不要当成零。
param(
    [string] $VMName = 'KSword-HVM-Target',
    [int]    $Depth  = 4000
)

$ErrorActionPreference = 'Stop'
$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName $VMName -Credential $cred

$rows = Invoke-Command -Session $s -ArgumentList $Depth -ScriptBlock {
    param($depth)

    function Run($a) {
        $o = 'C:\ksword\tfo.txt'
        $e = 'C:\ksword\tfe.txt'
        Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList $a -NoNewWindow -Wait `
            -RedirectStandardOutput $o -RedirectStandardError $e | Out-Null
        if (Test-Path $o) { return [IO.File]::ReadAllText($o) }
        return ''
    }

    $h = (Run @('--json', 'events', '0')) | ConvertFrom-Json
    $newest = [int64]$h.newestSequence
    $cur = [Math]::Max(0, $newest - $depth)
    $all = @()
    while ($cur -lt $newest) {
        $j = (Run @('--json', 'events', "$cur")) | ConvertFrom-Json
        $r = @($j.rows)
        if ($r.Count -eq 0) { break }
        $all += $r
        $cur = [int64]$r[-1].sequence
    }
    return ($all | Where-Object { $_.ruleId -in @(205, 206, 207, 208, 209, 210, 211) })
}

Remove-PSSession $s

$desc = @($rows | Where-Object { $_.ruleId -eq 211 })
$idtw = @($rows | Where-Object { $_.ruleId -eq 210 })
$copy = @($rows | Where-Object { $_.ruleId -eq 209 })

if ($desc.Count -eq 0) { Write-Output '0xD3（描述符表现场）：(没有这一行)' }
foreach ($r in $desc) {
    $lim = [uint64]$r.guestRip
    Write-Output ''
    Write-Output ("0xD3 描述符表现场  核{0}  序号 {1}" -f $r.access, $r.sequence)
    Write-Output ("  IDTR 基址 = 0x{0:X16}  界限 = 0x{1:X4}" -f `
        ([uint64]$r.qualification), ($lim -band 0xFFFF))
    Write-Output ("  GDTR 基址 = 0x{0:X16}  界限 = 0x{1:X4}" -f `
        ([uint64]$r.guestPhysicalAddress), (($lim -shr 16) -band 0xFFFF))
    Write-Output ("  TR   基址 = 0x{0:X16}  界限 = 0x{1:X8}  AR = 0x{2:X}" -f `
        ([uint64]$r.guestLinearAddress), (($lim -shr 32) -band 0xFFFFFFFF), ([uint32]$r.status))
    Write-Output ("  与 vmcs12 不一致掩码 = 0x{0:X}（0 = 逐字段一致）" -f ([uint64]$r.exitReason))
}

if ($idtw.Count -eq 0) { Write-Output ''; Write-Output '0xD2（IDTR 基址写入记录）：(没有这一行)' }
foreach ($r in $idtw) {
    Write-Output ''
    Write-Output ("0xD2 IDTR 基址写入记录  核{0}  序号 {1}" -f $r.access, $r.sequence)
    Write-Output ("  L1 对 0x6818 的 VMWRITE 次数 = {0}" -f ([uint64]$r.guestLinearAddress))
    Write-Output ("  最后一次写入的值          = 0x{0:X16}" -f ([uint64]$r.qualification))
    Write-Output ("  三重故障时 vmcs02 里的值  = 0x{0:X16}" -f ([uint64]$r.guestPhysicalAddress))
    Write-Output ("  当时的 vmcs12 区域        = 0x{0:X}" -f ([uint64]$r.guestRip))
}

if ($copy.Count -eq 0) { Write-Output ''; Write-Output '0xD1（拷贝回路两端）：(没有这一行)' }
foreach ($r in $copy) {
    $nz = [uint64]$r.guestLinearAddress
    $cn = [uint64]$r.guestRip
    $wc = [uint64]$r.exitReason
    Write-Output ''
    Write-Output ("0xD1 IDTR 基址拷贝两端  核{0}  序号 {1}" -f $r.access, $r.sequence)
    Write-Output ("  存出（vmcs02→vmcs12）次数 = {0}   其中非零 = {1}   最后存出值 = 0x{2:X16}" -f `
        ($cn -shr 32), ($nz -shr 32), ([uint64]$r.qualification))
    Write-Output ("  装入（vmcs12→vmcs02）次数 = {0}   其中非零 = {1}   最后装入值 = 0x{2:X16}" -f `
        ($cn -band 0xFFFFFFFF), ($nz -band 0xFFFFFFFF), ([uint64]$r.guestPhysicalAddress))
    # exitReason 只有 32 位，两个对照计数各占 16 位。第一版按 32/32 打包，
    # 高半直接被截掉，读出来是个恒零的假读数。
    Write-Output ("  对照：L1 写 IDTR 界限 {0} 次，写 GDTR 基址 {1} 次" -f `
        (($wc -shr 16) -band 0xFFFF), ($wc -band 0xFFFF))
}

# 每个 vmcs12 区域自己的 IDTR 基址。这一组不受三重故障门控，随时可读。
$perRegion = @($rows | Where-Object { $_.ruleId -eq 208 })
if ($perRegion.Count -eq 0) { Write-Output ''; Write-Output '0xD0（每区域 IDTR 基址）：(没有这一行)' }
foreach ($r in ($perRegion | Sort-Object { [int64]$_.sequence } | Select-Object -Last 8)) {
    Write-Output ''
    Write-Output ("0xD0 区域 IDTR 基址  核{0}  区{1}  vmcs12=0x{2:X}  序号 {3}" -f `
        $r.access, [uint64]$r.exitReason, ([uint64]$r.guestRip), $r.sequence)
    Write-Output ("  当前基址 = 0x{0:X16}" -f ([uint64]$r.qualification))
    Write-Output ("  归零次数 = {0}   归零时 RIP = 0x{1:X}   归零时退出原因 = {2}" -f `
        ([uint64]$r.guestLinearAddress), ([uint64]$r.guestPhysicalAddress), ([uint32]$r.status))
}

# 归零次数的分解：是我们的装入把它抹了，还是 L2 自己重载了 IDT。
$blame = @($rows | Where-Object { $_.ruleId -eq 207 })
if ($blame.Count -eq 0) { Write-Output ''; Write-Output '0xCF（归零归因）：(没有这一行)' }
foreach ($r in ($blame | Sort-Object { [int64]$_.sequence } | Select-Object -Last 4)) {
    Write-Output ''
    Write-Output ("0xCF 归零归因  核{0}  区{1}  vmcs12=0x{2:X}  序号 {3}" -f `
        $r.access, [uint64]$r.exitReason, ([uint64]$r.guestRip), $r.sequence)
    Write-Output ("  归零总数 = {0}" -f ([uint32]$r.status))
    Write-Output ("    其中我们装入抹掉的 = {0}" -f ([uint64]$r.guestPhysicalAddress))
    Write-Output ("    其中 L2 自己重载的 = {0}" -f ([uint64]$r.guestLinearAddress))
    Write-Output ("  上次装入值 = 0x{0:X16}" -f ([uint64]$r.qualification))
}

# 我们抹掉的那一次现场：后备存储当时的状态。
foreach ($r in (@($rows | Where-Object { $_.ruleId -eq 206 }) |
                Sort-Object { [int64]$_.sequence } | Select-Object -Last 2)) {
    $p = [uint64]$r.guestRip
    Write-Output ''
    Write-Output ("0xCE 我们抹掉的那一次  核{0}  序号 {1}" -f $r.access, $r.sequence)
    Write-Output ("  即将进入的 L2 RIP = 0x{0:X}" -f ([uint64]$r.qualification))
    Write-Output ("  当时的 vmcs12 区域 = 0x{0:X}   区域头 = 0x{1:X}   写序号 = {2}" -f `
        ([uint64]$r.guestPhysicalAddress), ([uint64]$r.guestLinearAddress), ([uint32]$r.exitReason))
    Write-Output ("  当时：存页失败 {0}   还原未命中 {1}   还原被拒字段 {2}   池淘汰 {3}   上次存了 {4} 条" -f `
        ($p -shr 48), (($p -shr 32) -band 0xFFFF), (($p -shr 16) -band 0xFFFF), ($p -band 0xFFFF), ([uint32]$r.status))
}

# 后备存储自身的健康度（累计），此前从未发布过。
foreach ($r in (@($rows | Where-Object { $_.ruleId -eq 205 }) |
                Sort-Object { [int64]$_.sequence } | Select-Object -Last 2)) {
    $q = [uint64]$r.qualification
    $l = [uint64]$r.guestLinearAddress
    $g = [uint64]$r.guestRip
    Write-Output ''
    Write-Output ("0xCD 后备存储健康度  核{0}  序号 {1}" -f $r.access, $r.sequence)
    Write-Output ("  存页：成功 {0}   失败 {1}   跳过 {2}   最近一次写了 {3} 条" -f `
        ($q -shr 32), ($q -band 0xFFFFFFFF), ([uint64]$r.guestPhysicalAddress), ([uint32]$r.status))
    Write-Output ("  还原：成功 {0}   未命中 {1}   被拒字段 {2}   池淘汰 {3}   上次头里的条数 {4}" -f `
        ($l -shr 32), ($l -band 0xFFFFFFFF), ($g -shr 32), ($g -band 0xFFFFFFFF), ([uint32]$r.exitReason))
}
