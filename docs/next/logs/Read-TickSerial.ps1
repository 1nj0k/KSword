# 解码 tickprobe 的串口日志并给出时间序列摘要。
#
# 为什么要解码：VMware 的虚拟 UART 在 BIOS 之后停在**5 位字长**（线路控制寄存器
# 为 0），于是每个发出去的字节只剩低 5 位。文件里看着是一堆控制字符，其实数据
# 一个都没丢 —— '0'..'9'(30h..39h) 掩成 10h..19h，'A'..'F'(41h..46h) 掩成
# 01h..06h，两段不重叠，CR/LF 本来就小于 20h 不受影响。所以可以无歧义还原。
#
# 探针后来会自己把 LCR 设成 8 位字长，但**已经录下来的日志仍然要靠这段还原**，
# 而且留着它比"重跑一次"便宜：那是十几分钟和一次重启。
#
# 行格式：SPIN(8) TICK(8) OWN(8) PIT(4) IRR(2) ISR(2) SIRR(2) SISR(2) RTC(2)，
# 定宽，CRLF 结尾。
param(
    [string] $Path = 'C:\vmware\probe-serial.log'
)

$fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
$raw = New-Object byte[] $fs.Length
[void]$fs.Read($raw, 0, $raw.Length)
$fs.Close()

# 两种编码都要认：探针现在会自己把 LCR 设成 8 位字长，那之后字节就是正常 ASCII；
# 而在那之前录下来的日志是被掩成低 5 位的。两段取值范围不重叠，所以同一个解码器
# 能同时读，不需要读的人先知道日志是哪一版。
$sb = New-Object Text.StringBuilder
foreach ($b in $raw) {
    if (($b -ge 0x30 -and $b -le 0x39) -or ($b -ge 0x41 -and $b -le 0x46)) {
        [void]$sb.Append([char]$b)                       # 8 位字长，原样
    }
    elseif ($b -ge 0x10 -and $b -le 0x19) { [void]$sb.Append([char](0x30 + ($b - 0x10))) }
    elseif ($b -ge 0x01 -and $b -le 0x06) { [void]$sb.Append([char](0x41 + ($b - 0x01))) }
    elseif ($b -eq 0x0D -or $b -eq 0x0A) { [void]$sb.Append([char]$b) }
    else { [void]$sb.Append('?') }   # 还原不了的字节显式留痕，不要静默吃掉
}

$lines = @($sb.ToString() -split "`r`n" | Where-Object { $_.Length -eq 38 })
$bad = @($sb.ToString() -split "`r`n" | Where-Object { $_.Length -ne 38 -and $_.Length -gt 0 })
Write-Output ("完整行 = {0}   非 38 宽的行 = {1}   含 ? 的行 = {2}" -f `
    $lines.Count, $bad.Count, @($lines | Where-Object { $_.Contains('?') }).Count)

function Field($s, $off, $len) { return $s.Substring($off, $len) }

$rows = foreach ($s in $lines) {
    [PSCustomObject]@{
        SPIN = Field $s 0 8
        TICK = Field $s 8 8
        OWN  = Field $s 16 8
        PIT  = Field $s 24 4
        IRR  = Field $s 28 2
        ISR  = Field $s 30 2
        SIRR = Field $s 32 2
        SISR = Field $s 34 2
        RTC  = Field $s 36 2
    }
}

function Show($r, $tag) {
    Write-Output ("  {0,-6} SPIN={1} TICK={2} OWN={3} PIT={4} IRR={5} ISR={6} SIRR={7} SISR={8} RTC={9}" -f `
        $tag, $r.SPIN, $r.TICK, $r.OWN, $r.PIT, $r.IRR, $r.ISR, $r.SIRR, $r.SISR, $r.RTC)
}

Write-Output '=== 首尾 ==='
Show $rows[0] '首'
Show $rows[[int]($rows.Count/2)] '中'
Show $rows[-1] '尾'

Write-Output '=== 各字段出现过的取值（判据全在这里） ==='
foreach ($f in 'TICK', 'OWN', 'IRR', 'ISR', 'SIRR', 'SISR') {
    $u = @($rows | ForEach-Object { $_.$f } | Select-Object -Unique)
    Write-Output ("  {0,-5} 共 {1,5} 种： {2}" -f $f, $u.Count,
        (($u | Select-Object -First 10) -join ' '))
}
foreach ($f in 'PIT', 'RTC') {
    $u = @($rows | ForEach-Object { $_.$f } | Select-Object -Unique)
    Write-Output ("  {0,-5} 共 {1,5} 种（前 10）： {2}" -f $f, $u.Count,
        (($u | Select-Object -First 10) -join ' '))
}

# RTC 是 BCD 秒，0..59 循环。它在走 = VMware 的虚拟时间在推进，
# 与 PIT 是否抬中断无关 —— 两个互相独立的时钟，分开看才分得出是哪一段坏了。
$rtcChanges = 0
for ($i = 1; $i -lt $rows.Count; $i++) {
    if ($rows[$i].RTC -ne $rows[$i-1].RTC) { $rtcChanges++ }
}
Write-Output ("=== RTC 秒变化次数 = {0}（时间序列覆盖 {1} 行）" -f $rtcChanges, $rows.Count)

$pitChanges = 0
for ($i = 1; $i -lt $rows.Count; $i++) {
    if ($rows[$i].PIT -ne $rows[$i-1].PIT) { $pitChanges++ }
}
Write-Output ("=== PIT 计数变化次数 = {0}" -f $pitChanges)
