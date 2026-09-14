# 把 tickprobe.asm 变成一张可引导的 1.44MB 软盘镜像。
#
# 为什么不用手写字节：引导扇区里一个错位的相对跳转就是黑屏，而黑屏和"中断没进来"
# 在截图上完全一样——被测夹具自己出问题会被当成被测现象。所以每条指令都由汇编器
# 生成，这里只做三件机械的事：取出代码段、填两处重定位、补引导签名。
#
# 重定位必须自己填：MASM 把 `OFFSET msg_spin` 的立即数留成 0000 并记一条 DIR16，
# 正常由链接器填。我们没有链接这一步（要的是平坦二进制），所以直接按 COFF 的
# 重定位表把符号地址写回去。不填的话两条标签会从 0000:0000 取字符串，
# 屏幕上出现乱码——**又是一种"夹具坏了却像被测物坏了"**。
param(
    [string] $Asm,
    [string] $OutImage,
    [string] $WorkDir
)

$ErrorActionPreference = 'Stop'

$ml = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" `
        -Filter 'ml.exe' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*Hostx64\x86*' } | Select-Object -First 1
if (-not $ml) { throw '找不到 ml.exe' }

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$obj = Join-Path $WorkDir 'tickprobe.obj'
Push-Location $WorkDir
try {
    & $ml.FullName /c /nologo /Fo $obj $Asm
    if ($LASTEXITCODE -ne 0) { throw "ml 失败，退出码 $LASTEXITCODE" }
} finally { Pop-Location }

# --- 解析 COFF：找到 .text 节与它的重定位 ---
$raw = [IO.File]::ReadAllBytes($obj)
$numSections = [BitConverter]::ToUInt16($raw, 2)
$symTab      = [BitConverter]::ToUInt32($raw, 8)
$numSymbols  = [BitConverter]::ToUInt32($raw, 12)
$strTab      = $symTab + 18 * $numSymbols

$textPtr = 0; $textSize = 0; $relPtr = 0; $relCount = 0
for ($i = 0; $i -lt $numSections; $i++) {
    $h = 20 + $i * 40
    $name = ([Text.Encoding]::ASCII.GetString($raw, $h, 8)).TrimEnd([char]0)
    if ($name -notlike '.text*') { continue }
    $textSize = [BitConverter]::ToUInt32($raw, $h + 16)
    $textPtr  = [BitConverter]::ToUInt32($raw, $h + 20)
    $relPtr   = [BitConverter]::ToUInt32($raw, $h + 24)
    $relCount = [BitConverter]::ToUInt16($raw, $h + 32)
    break
}
if ($textSize -eq 0) { throw '没找到 .text 节' }

# ORG 7C00h 让 MASM 在代码前补了 7C00h 字节的零；真正的代码在那之后。
$ORG = 0x7C00
if ($textSize -le $ORG) { throw "节太小（$textSize），ORG 没生效？" }
$codeLen = $textSize - $ORG
$code = New-Object byte[] $codeLen
[Array]::Copy($raw, $textPtr + $ORG, $code, 0, $codeLen)
Write-Output ("代码长度 = $codeLen 字节")

# --- 填重定位 ---
function SymbolName($index) {
    $o = $symTab + 18 * $index
    if ([BitConverter]::ToUInt32($raw, $o) -eq 0) {
        $off = [BitConverter]::ToUInt32($raw, $o + 4)
        $end = $strTab + $off
        while ($raw[$end] -ne 0) { $end++ }
        return [Text.Encoding]::ASCII.GetString($raw, $strTab + $off, $end - ($strTab + $off))
    }
    return ([Text.Encoding]::ASCII.GetString($raw, $o, 8)).TrimEnd([char]0)
}
function SymbolValue($index) { return [BitConverter]::ToUInt32($raw, $symTab + 18 * $index + 8) }

for ($r = 0; $r -lt $relCount; $r++) {
    $o = $relPtr + $r * 10
    $va   = [BitConverter]::ToUInt32($raw, $o)
    $sym  = [BitConverter]::ToUInt32($raw, $o + 4)
    $type = [BitConverter]::ToUInt16($raw, $o + 8)
    # 0x0001 = IMAGE_REL_I386_DIR16，16 位绝对地址。只认这一种，
    # 别的类型说明代码里出现了我没预期的引用形式，宁可报错也不要写错一个字节。
    if ($type -ne 1) { throw ("不认识的重定位类型 0x{0:X} @ 0x{1:X}" -f $type, $va) }
    $target = SymbolValue $sym
    $at = $va - $ORG
    $code[$at]     = [byte]($target -band 0xFF)
    $code[$at + 1] = [byte](($target -shr 8) -band 0xFF)
    Write-Output ("  重定位 {0} -> 0x{1:X4}  写在代码偏移 0x{2:X}" -f (SymbolName $sym), $target, $at)
}

if ($codeLen -gt 510) { throw "代码 $codeLen 字节，放不进引导扇区" }

# --- 组装 1.44MB 软盘镜像 ---
$img = New-Object byte[] (1474560)
[Array]::Copy($code, 0, $img, 0, $codeLen)
$img[510] = 0x55
$img[511] = 0xAA
[IO.File]::WriteAllBytes($OutImage, $img)
Write-Output ("镜像 = $OutImage  " + (Get-Item $OutImage).Length + " 字节，引导签名 55 AA 已写")
