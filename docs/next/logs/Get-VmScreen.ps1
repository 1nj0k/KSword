# 从宿主侧抓 Hyper-V 来宾的画面。
#
# 为什么不在来宾里截图：PowerShell Direct 落在 session 0，CopyFromScreen 抓到的是
# 黑屏；而把窗口抬到前台会让 VMware 抢走输入，从宿主看像整机假死。
# Msvm_VirtualSystemManagementService::GetVirtualSystemThumbnailImage 完全在宿主
# 一侧读显存，对来宾没有任何影响。
param(
    [string] $VMName = 'KSword-HVM-Target',
    [int]    $Width  = 1024,
    [int]    $Height = 768,
    [string] $OutFile
)

$ErrorActionPreference = 'Stop'

$vm = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem `
        -Filter "ElementName='$VMName'"
if (-not $vm) { throw "找不到虚拟机 $VMName" }

$settings = Get-CimAssociatedInstance -InputObject $vm `
    -ResultClassName Msvm_VirtualSystemSettingData |
    Where-Object { $_.VirtualSystemType -eq 'Microsoft:Hyper-V:System:Realized' }

$svc = Get-CimInstance -Namespace root\virtualization\v2 `
    -ClassName Msvm_VirtualSystemManagementService

$result = Invoke-CimMethod -InputObject $svc -MethodName GetVirtualSystemThumbnailImage `
    -Arguments @{
        TargetSystem = [CimInstance]$settings
        WidthPixels  = [uint16]$Width
        HeightPixels = [uint16]$Height
    }

if ($result.ReturnValue -ne 0) { throw "GetVirtualSystemThumbnailImage 返回 $($result.ReturnValue)" }

# 返回的是 RGB565 小端，每像素两字节，行优先，**前面还有 4 个字节的头**。
#
# 这两件事都是量出来的，不是猜的：数组长度 1,572,868 而 1024×768×2 = 1,572,864，
# 多出来的正好 4 个。再看原始字节——开头 `79 85` 重复（0x8579 → RGB565 →
# (132,174,206) 浅蓝，就是壁纸），结尾 `7D EF` 重复（0xEF7D → (238,239,238)
# 浅灰，就是任务栏），工具栏那一行是 `FF FF` 纯白。三个已知参照全部对上。
#
# 之前我把偏蓝"修"成了偏红——没有任何测量就把 R 和 B 对调了，只是把错误换了个方向。
# 真正的错是这 4 字节偏移，以及**我误把正确的偏蓝当成了故障**：壁纸本来就是蓝的。
$HEADER = 4
$raw = $result.ImageData
if ($raw.Length -ne ($HEADER + $Width * $Height * 2)) {
    # 尺寸对不上就停，不要按错误的假设渲染出一张看着像真的图。
    throw ("ImageData 长度 {0} 与 {1}x{2} 的 RGB565+{3} 头不符（期望 {4}）" -f `
        $raw.Length, $Width, $Height, $HEADER, ($HEADER + $Width * $Height * 2))
}
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$stride = $data.Stride
$buffer = New-Object byte[] ($stride * $Height)
for ($y = 0; $y -lt $Height; $y++) {
    $srcRow = $HEADER + $y * $Width * 2
    $dstRow = $y * $stride
    for ($x = 0; $x -lt $Width; $x++) {
        # **移位前必须转 int。**
        #
        # $raw 的元素是 [byte]，而 PowerShell 的 -shl 以左操作数的宽度算：
        # [byte] 0xAE -shl 8 的结果是 **0**，不是 0xAE00。于是每个像素只剩低字节，
        # 红分量恒为 0、绿只剩几位、蓝拿满 —— 整幅图必然偏蓝。
        #
        # 我曾把这个"偏蓝"当成通道顺序错，把 R 和 B 对调，结果只是换成了恒红。
        # 真因在这一行，判据是同一帧里源字节 `5C AE` 被算成了 v=0x005C。
        $lo = [int]$raw[$srcRow + $x * 2]
        $hi = [int]$raw[$srcRow + $x * 2 + 1]
        $v  = ($hi -shl 8) -bor $lo
        # 5:6:5 -> 8:8:8，低位复制高位，避免最亮值到不了 255。
        $r = (($v -shr 11) -band 0x1F); $r = ($r -shl 3) -bor ($r -shr 2)
        $g = (($v -shr 5)  -band 0x3F); $g = ($g -shl 2) -bor ($g -shr 4)
        $b = ( $v          -band 0x1F); $b = ($b -shl 3) -bor ($b -shr 2)
        # Format24bppRgb 在内存里是 B,G,R 的顺序，源是 RGB565，所以这样对应。
        $o = $dstRow + $x * 3
        $buffer[$o]     = [byte]$b
        $buffer[$o + 1] = [byte]$g
        $buffer[$o + 2] = [byte]$r
    }
}
[System.Runtime.InteropServices.Marshal]::Copy($buffer, 0, $data.Scan0, $buffer.Length)
$bmp.UnlockBits($data)
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved=$OutFile bytes=$((Get-Item $OutFile).Length)"
