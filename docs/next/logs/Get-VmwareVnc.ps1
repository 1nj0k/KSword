# 通过 VMware 自带的 VNC 服务取虚拟机画面，并且可以往里送按键。
#
# 为什么不用截屏：
#   - Hyper-V 的缩略图拍的是**靶机的桌面**，上面得有一个 VMware 窗口才看得见虚拟机；
#     而从 PowerShell Direct（session 0）启动的 VMware，窗口开在一个不可见的桌面上，
#     缩略图里干干净净什么都没有。交互式计划任务也起不来（schtasks /Run 报
#     "Element not found"）。
#   - `vmrun captureScreen` 要求来宾装了 VMware Tools 并登录，对一个 512 字节的
#     引导扇区或一张 TinyCore 光盘都无从谈起。
#
# VNC 这条路绕开了整个会话问题：帧缓冲直接来自 vmware-vmx 进程，跟有没有窗口、
# 谁有焦点都没关系。送按键同样如此 —— 之前按键进不去 VMware，正是因为它们要先
# 经过窗口焦点。
#
# 协议只用到 RFB 3.8 的最小子集：无认证、Raw 编码、一次全屏更新。
# 所有多字节字段都是**大端**，这是 RFB 与 x86 相反的地方，写错了不会报错，
# 只会得到一张尺寸荒谬的图 —— 所以下面对宽高做了上界检查。
param(
    [string] $VncHost = '127.0.0.1',
    [int]    $Port = 5900,
    [string] $OutFile,
    # X11 keysym 序列；送完再取图。空则只取图。
    [int[]]  $Keys = @(),
    [int]    $KeyDelayMs = 120,
    # 在**同一条连接**上连取几帧，间隔 FrameGapMs。
    #
    # 为什么要有这个：连上取一帧就断开时，画面可能是陈旧的 —— VMware 只在有
    # 客户端看着的时候才跟踪 VGA 文本缓冲的脏区。那会让"画面停在第 N 行"
    # 变成一个假读数，而它和"来宾停在第 N 行"长得一模一样。
    # 取两帧以上，第二帧才是"连接建立之后的现在"。
    [int]    $Frames = 1,
    [int]    $FrameGapMs = 4000
)

$ErrorActionPreference = 'Stop'

function Read-Exact([IO.Stream] $s, [int] $n) {
    $buf = New-Object byte[] $n
    $got = 0
    while ($got -lt $n) {
        $r = $s.Read($buf, $got, $n - $got)
        if ($r -le 0) { throw "连接在读到 $n 字节之前就断了（已读 $got）" }
        $got += $r
    }
    return $buf
}
function BE16([byte[]] $b, [int] $o) { return ([int]$b[$o] -shl 8) -bor [int]$b[$o+1] }
function BE32([byte[]] $b, [int] $o) {
    return ([int64]$b[$o] -shl 24) -bor ([int64]$b[$o+1] -shl 16) -bor `
           ([int64]$b[$o+2] -shl 8) -bor [int64]$b[$o+3]
}
function Put16([byte[]] $b, [int] $o, [int] $v) {
    $b[$o] = [byte](($v -shr 8) -band 0xFF); $b[$o+1] = [byte]($v -band 0xFF)
}
function Put32([byte[]] $b, [int] $o, [int64] $v) {
    $b[$o]   = [byte](($v -shr 24) -band 0xFF); $b[$o+1] = [byte](($v -shr 16) -band 0xFF)
    $b[$o+2] = [byte](($v -shr 8)  -band 0xFF); $b[$o+3] = [byte]($v -band 0xFF)
}

$client = New-Object Net.Sockets.TcpClient
$client.Connect($VncHost, $Port)
$client.NoDelay = $true
$ns = $client.GetStream()
# 必须有读超时。
#
# 请求整帧之后，画面**一点没变**时服务端可以什么都不回 —— 于是客户端就永远阻塞在
# 那里，而"脚本挂住"和"来宾挂住"在外面看是一样的。超时到了就说没变化，
# 这本身也是一个读数。
$ns.ReadTimeout = 20000

# --- 握手 ---
$ver = [Text.Encoding]::ASCII.GetString((Read-Exact $ns 12))
Write-Output ("服务端版本 = " + $ver.Trim())
$mine = [Text.Encoding]::ASCII.GetBytes("RFB 003.008`n")
$ns.Write($mine, 0, 12)

$n = (Read-Exact $ns 1)[0]
if ($n -eq 0) {
    $len = BE32 (Read-Exact $ns 4) 0
    throw ("服务端拒绝：" + [Text.Encoding]::ASCII.GetString((Read-Exact $ns $len)))
}
$types = Read-Exact $ns $n
Write-Output ("安全类型 = " + (($types | ForEach-Object { $_ }) -join ','))
if ($types -notcontains 1) { throw "服务端不接受无认证（类型 1），这里没有实现 VNC 口令认证" }
$ns.Write([byte[]]@(1), 0, 1)
$res = BE32 (Read-Exact $ns 4) 0
if ($res -ne 0) { throw "安全握手失败，SecurityResult = $res" }

# ClientInit：1 = 共享，别把已经连上的其它客户端踢掉
$ns.Write([byte[]]@(1), 0, 1)

$init = Read-Exact $ns 24
$w = BE16 $init 0
$h = BE16 $init 2
$nameLen = BE32 $init 20
$name = [Text.Encoding]::UTF8.GetString((Read-Exact $ns $nameLen))
Write-Output ("帧缓冲 = ${w}x${h}   名称 = $name")
if ($w -le 0 -or $h -le 0 -or $w -gt 8192 -or $h -gt 8192) {
    throw "帧缓冲尺寸 ${w}x${h} 不合理——多半是字节序读反了"
}

# --- 指定像素格式：32bpp、小端、真彩、R/G/B 移位 16/8/0 ---
# 这样每个像素就是内存里的 B,G,R,X 四个字节，正好对上 Format24bppRgb 的取法。
$pf = New-Object byte[] 16
$pf[0] = 32; $pf[1] = 24; $pf[2] = 0; $pf[3] = 1
Put16 $pf 4 255; Put16 $pf 6 255; Put16 $pf 8 255
$pf[10] = 16; $pf[11] = 8; $pf[12] = 0
$msg = New-Object byte[] 20
$msg[0] = 0
[Array]::Copy($pf, 0, $msg, 4, 16)
$ns.Write($msg, 0, 20)

# --- 只要 Raw 编码：省掉一整套解码器，代价是每帧几 MB，本地环回无所谓 ---
$msg = New-Object byte[] 8
$msg[0] = 2; Put16 $msg 2 1; Put32 $msg 4 0
$ns.Write($msg, 0, 8)

# --- 按键 ---
foreach ($k in $Keys) {
    foreach ($down in 1, 0) {
        $km = New-Object byte[] 8
        $km[0] = 4; $km[1] = [byte]$down
        Put32 $km 4 $k
        $ns.Write($km, 0, 8)
    }
    Start-Sleep -Milliseconds $KeyDelayMs
}
if ($Keys.Count -gt 0) {
    Write-Output ("已送 " + $Keys.Count + " 个按键")
    Start-Sleep -Milliseconds 600
}

if (-not $OutFile) { $client.Close(); return }

Add-Type -AssemblyName System.Drawing

# **一张位图跨帧复用。**
#
# 第一版每帧新建一张，只把服务器发来的矩形画进去 —— 而服务器从第二帧起只发变化
# 区域（哪怕请求写的是非增量），于是第 2..N 帧全是黑的，看着就像"来宾把屏幕清空了"。
# 又一次仪器故障长成了被测现象的样子。累积到同一张上，缺的部分就还是上一帧的内容。
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)

for ($frame = 1; $frame -le $Frames; $frame++) {
    if ($frame -gt 1) { Start-Sleep -Milliseconds $FrameGapMs }

    # 非增量：要的是整帧，不是"自上次以来变了什么"
    $req = New-Object byte[] 10
    $req[0] = 3; $req[1] = 0
    Put16 $req 2 0; Put16 $req 4 0; Put16 $req 6 $w; Put16 $req 8 $h
    $ns.Write($req, 0, 10)

    $hdr = $null
    try { $hdr = Read-Exact $ns 4 }
    catch [IO.IOException] {
        Write-Output ("第 $frame 帧：{0} ms 内没有更新（画面没有变化）" -f $ns.ReadTimeout)
        continue
    }
    if ($hdr[0] -ne 0) { throw "期望 FramebufferUpdate(0)，收到消息类型 $($hdr[0])" }
    $rects = BE16 $hdr 2

    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    # ReadWrite，不是 WriteOnly：这一帧可能只覆盖屏幕的一小块，其余要保留
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $stride = $data.Stride
    $buffer = New-Object byte[] ($stride * $h)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buffer, 0, $buffer.Length)
    try {
        for ($r = 0; $r -lt $rects; $r++) {
            $rh = Read-Exact $ns 12
            $rx = BE16 $rh 0; $ry = BE16 $rh 2; $rw = BE16 $rh 4; $rht = BE16 $rh 6
            $enc = BE32 $rh 8
            if ($enc -ne 0) { throw "矩形 $r 用了编码 $enc，这里只实现了 Raw(0)" }
            $px = Read-Exact $ns ($rw * $rht * 4)
            for ($y = 0; $y -lt $rht; $y++) {
                $src = $y * $rw * 4
                $dst = ($ry + $y) * $stride + $rx * 3
                for ($x = 0; $x -lt $rw; $x++) {
                    $buffer[$dst + $x*3]     = $px[$src + $x*4]      # B
                    $buffer[$dst + $x*3 + 1] = $px[$src + $x*4 + 1]  # G
                    $buffer[$dst + $x*3 + 2] = $px[$src + $x*4 + 2]  # R
                }
            }
        }
        [Runtime.InteropServices.Marshal]::Copy($buffer, 0, $data.Scan0, $buffer.Length)
    } finally {
        $bmp.UnlockBits($data)
    }
    $name = if ($Frames -eq 1) { $OutFile }
            else {
                $dir = [IO.Path]::GetDirectoryName($OutFile)
                $base = [IO.Path]::GetFileNameWithoutExtension($OutFile)
                [IO.Path]::Combine($dir, "$base-f$frame.png")
            }
    $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("saved=$name 矩形数=$rects bytes=" + (Get-Item $name).Length)
}
$bmp.Dispose()
$client.Close()
