# 在来宾里跑：对 L2 的读数取两次快照并相减。
#
# 为什么必须取增量：这些计数器都是从上电开始累计的，而 POST 那几十秒里的端口 I/O
# 与 EPT 违规能占掉绝大部分。累计值回答"这台机器一共做过什么"，不回答"它现在在做
# 什么"——同一个错误此前已经让我导出过三次错误诊断。
#
# 两条配套规矩：
#   1. 事件环一次只回 64 行，必须翻页，否则想看的那一行会安静地不在窗口里；
#   2. 第二次快照缺了某个 key 时打 (缺) 而不是当 0 —— 把"没读到"当成 0
#      曾让我报告过一条不存在的成功。
param([int] $Seconds = 60)

function Run($a) {
    $o = 'C:\ksword\ro.txt'
    $e = 'C:\ksword\re.txt'
    $p = Start-Process 'C:\ksword\hvm_ctl.exe' -ArgumentList $a -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $o -RedirectStandardError $e
    if (Test-Path $o) { return [IO.File]::ReadAllText($o) }
    return ''
}

function ReadRing($count) {
    $h = (Run @('--json', 'events', '0')) | ConvertFrom-Json
    $newest = [int64]$h.newestSequence
    $cur = [Math]::Max(0, $newest - $count)
    $all = @()
    while ($cur -lt $newest) {
        $j = (Run @('--json', 'events', "$cur")) | ConvertFrom-Json
        $rows = @($j.rows)
        if ($rows.Count -eq 0) { break }
        $all += $rows
        $cur = [int64]$rows[-1].sequence
    }
    return $all
}

# 把一次快照压成 "键 -> 值" 的哈希表。
#
# 键里必须带**驱动自己写的那个核号**，也就是行里的 `access`（= ApicId），不是
# `processor`（事件环记录发布时所在的核）。第一版按 `processor` 分组，于是两个核的
# 计数被塞进同一个键、后写覆盖先写，差分里出现了 -21,208 这种**负增量** ——
# 单调递增的计数器不可能变小，负数就是键错了的签名。
function Snapshot($rows) {
    $m = @{}
    foreach ($r in $rows) {
        $rid = [int]$r.ruleId
        $cpu = "核$($r.access)"
        if ($rid -eq 251) {
            # 0xFB 退出原因直方图
            $m["退出原因 $($r.exitReason) $cpu"] = [uint64]$r.qualification
            $m["L2总退出 $cpu"] = [uint64]$r.guestPhysicalAddress
            $m["vmcs12pin $cpu"] = [uint64]$r.guestLinearAddress
        }
        elseif ($rid -eq 247) {
            # 0xF7 外部中断与注入
            $m["外部中断 $cpu"] = [uint64]$r.qualification
            $m["注入 $cpu"] = [uint64]$r.guestPhysicalAddress
        }
        elseif ($rid -eq 227) {
            # 0xE3 被退出中止的事件投递：见到 / 我们补投 / 交给 L1 补投。
            # 补投 + 反射必须等于见到，差额就是没人投递的那些中断。
            $m["中止投递-见到 $cpu"] = [uint64]$r.qualification
            $m["中止投递-补投 $cpu"] = [uint64]$r.guestPhysicalAddress
            $m["中止投递-反射 $cpu"] = [uint64]$r.guestLinearAddress
            $m["注入已退休 $cpu"] = [uint64]$r.guestRip
        }
        elseif ($rid -eq 245) {
            # 0xF5 按端口范围分的设备计数
            $dev = @('键盘60/64', 'IDE次170', 'IDE主1F0', 'VGA3B0-3DF', '串口3F8-3FF',
                     '定时器40-43', 'CMOS70/71', '其他端口')
            $m["端口 $($dev[[int]$r.exitReason]) $cpu"] = [uint64]$r.qualification
        }
        elseif ($rid -eq 250) {
            # 0xFA。字段顺序按 hvm_exit.c 里发布这一行的地方逐个抄下来的：
            # qualification=注入请求 / guestPhysicalAddress=注入 /
            # guestLinearAddress=IF置位的退出数 / guestRip=IF清零的退出数 /
            # exitReason=vmcs12 退出控制。
            # 第一版把后三个对错了位，于是"每次退出 IF 都是 0"被念成了结论 ——
            # 实际恰好相反。**字段映射要去发布点抄，不要照着字段名猜。**
            $m["注入请求 $cpu"] = [uint64]$r.qualification
            $m["注入 $cpu(FA)"] = [uint64]$r.guestPhysicalAddress
            $m["IF=1退出 $cpu"] = [uint64]$r.guestLinearAddress
            $m["IF=0退出 $cpu"] = [uint64]$r.guestRip
            $m["vmcs12exit $cpu"] = [uint64]$r.exitReason
        }
    }
    return $m
}

$a = Snapshot (ReadRing 1600)
Write-Output ("第一次快照 " + $a.Count + " 个键")
Start-Sleep -Seconds $Seconds
$b = Snapshot (ReadRing 1600)
Write-Output ("第二次快照 " + $b.Count + " 个键，间隔 ${Seconds}s")
Write-Output ''

$keys = @($a.Keys) + @($b.Keys) | Sort-Object -Unique
foreach ($k in $keys) {
    $has1 = $a.ContainsKey($k)
    $has2 = $b.ContainsKey($k)
    if (-not $has1 -or -not $has2) {
        Write-Output ("  {0,-28} {1,18} -> {2,18}   (缺，不参与差分)" -f $k,
            $(if ($has1) { $a[$k] } else { '(缺)' }),
            $(if ($has2) { $b[$k] } else { '(缺)' }))
        continue
    }
    $d = [int64]$b[$k] - [int64]$a[$k]
    $mark = ''
    if ($d -ne 0) { $mark = '  <<<' }
    Write-Output ("  {0,-28} {1,18} -> {2,18}   增量 {3}{4}" -f $k, $a[$k], $b[$k], $d, $mark)
}
