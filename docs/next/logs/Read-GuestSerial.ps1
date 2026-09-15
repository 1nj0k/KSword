# 读靶机里那个串口文件的尾巴。VMware 一直开着它，所以必须自己开共享读。
#
# 两次读之间长度还在涨 = 来宾还在跑；长度不动 = 停住了。这比看画面可靠：
# 默认引导项会起 X，屏幕就是一整片黑，看不出死活。
param(
    [string] $VMName = 'KSword-HVM-Target',
    [string] $SerialLog = 'C:\vmware\hltprobe.log',
    [int]    $Lines = 20,
    # 隔这么多秒再读一次，用长度差判断还在不在跑。0 = 只读一次。
    [int]    $AgainAfter = 0
)

$ErrorActionPreference = 'Stop'
$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName $VMName -Credential $cred

$block = {
    param($log, $lines)
    if (-not (Test-Path $log)) { return @{ Size = -1; Tail = '(串口文件不存在)' } }
    $fs = New-Object IO.FileStream($log, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $b = New-Object byte[] $fs.Length
        [void]$fs.Read($b, 0, $b.Length)
    } finally { $fs.Dispose() }
    $txt = -join ($b | ForEach-Object {
        if ($_ -ge 32 -and $_ -lt 127) { [char]$_ }
        elseif ($_ -eq 10) { "`n" }
        elseif ($_ -eq 13) { '' }
        else { '.' }
    })
    return @{
        Size = $b.Length
        Tail = (($txt -split "`n" | Select-Object -Last $lines) -join "`n")
    }
}

$a = Invoke-Command -Session $s -ArgumentList $SerialLog, $Lines -ScriptBlock $block
Write-Output ("串口 {0} 字节" -f $a.Size)
Write-Output $a.Tail
if ($AgainAfter -gt 0) {
    Start-Sleep -Seconds $AgainAfter
    $b = Invoke-Command -Session $s -ArgumentList $SerialLog, $Lines -ScriptBlock $block
    Write-Output ''
    Write-Output ("{0} 秒后：{1} 字节（增量 {2}）" -f `
        $AgainAfter, $b.Size, ($b.Size - $a.Size))
    if ($b.Size -ne $a.Size) { Write-Output $b.Tail }
}
Remove-PSSession $s
