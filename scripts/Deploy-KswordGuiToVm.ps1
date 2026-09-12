<#
.SYNOPSIS
    把构建好的 Qt 主程序、语言包与 hvm_ctl 送进测试机。

.DESCRIPTION
    Deploy-KswordDriverToVm.ps1 只管驱动和 KswordCLI。GUI 这一侧此前一直是手工
    敲命令做的，于是每次都要重新踩同样的坑，而且"这次到底推没推"完全取决于当时
    记没记得做。这个脚本存在的唯一理由就是把那几步固定下来。

    三个坑，都是实际撞过的：

    1) Copy-VMFile 需要来宾服务接口。本机这台靶机上它返回 0x80070015（设备未
       就绪），所以必须有 PowerShell Direct 的回退路径，不能只写 Copy-VMFile。

    2) Expand-Archive -Force 碰到 guest 里**正在被内核加载**的 gui\KswordARK.sys
       会整个中止，而且是在解压过程中中止 —— 结果是另外八十多个文件一个都没落
       地，但命令本身看起来只是报了一个文件的错。所以这里先解到暂存目录，再逐
       个文件拷过去；拷不动的那个如果哈希本来就一致，就算它过。

    3) ABI 一动，GUI 和驱动必须一起换。查询响应结构里插一个字段，旧 GUI 读新驱
       动会把插入点之后的每一个字段都错位读出来，而界面上不会报错，只会显示一
       堆看着眼熟但其实挪了一格的数字。所以这里默认校验 guest 上的驱动与本地构
       建同源，不同源就拒绝推 GUI。

.EXAMPLE
    .\Deploy-KswordGuiToVm.ps1

.EXAMPLE
    .\Deploy-KswordGuiToVm.ps1 -SkipDriverMatchCheck
#>
[CmdletBinding()]
param(
    [string] $VMName = 'KSword-HVM-Target',
    [string] $GuestUser = 'felix',
    [string] $GuestPassword = 'password',
    [string] $GuestGuiDir = 'C:\ksword\gui',
    [string] $GuestToolDir = 'C:\ksword',
    # 每块的字节数。太大时 PowerShell Direct 的序列化会显著变慢甚至失败，
    # 4 MiB 是本机实测还稳的量级。
    [int]    $ChunkBytes = 4MB,
    # ABI 校验是默认开着的，见上面第 3 条。只在明确知道自己要推不配套的组合
    # 时才关掉它。
    [switch] $SkipDriverMatchCheck
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$releaseDir = Join-Path $repo 'Ksword5.1\x64\Release'

$credential = New-Object System.Management.Automation.PSCredential(
    $GuestUser, (ConvertTo-SecureString $GuestPassword -AsPlainText -Force))

$vm = Get-VM -Name $VMName -ErrorAction Stop
if ($vm.State -ne 'Running') { throw "虚拟机不在运行状态（$($vm.State)）。先 Start-VM。" }

# 要推的东西。Ksword5.1.exe 与两个语言包必须同批 —— 新词条落在旧 exe 上没用，
# 新 exe 配旧词条则会退回到源码里的兜底中文，英文界面直接看不到这次的改动。
$payload = @(
    [pscustomobject]@{ Local = Join-Path $releaseDir 'Ksword5.1.exe';            Remote = Join-Path $GuestGuiDir  'Ksword5.1.exe' }
    [pscustomobject]@{ Local = Join-Path $releaseDir 'languages\zh-CN.json';     Remote = Join-Path $GuestGuiDir  'languages\zh-CN.json' }
    [pscustomobject]@{ Local = Join-Path $releaseDir 'languages\en-US.json';     Remote = Join-Path $GuestGuiDir  'languages\en-US.json' }
    [pscustomobject]@{ Local = Join-Path $repo 'tools\hvm_ctl\hvm_ctl.exe';      Remote = Join-Path $GuestToolDir 'hvm_ctl.exe' }
)

$missing = $payload | Where-Object { -not (Test-Path $_.Local) }
if ($missing) { throw "本地缺文件：$(($missing.Local) -join ', ')" }

# ---------------------------------------------------------------------------
# 0. ABI 配套检查
# ---------------------------------------------------------------------------
if (-not $SkipDriverMatchCheck) {
    Write-Host "`n--- 0. 驱动与 GUI 是否同源 ---" -ForegroundColor Cyan
    $localSys = Join-Path $releaseDir 'KswordARK.sys'
    if (-not (Test-Path $localSys)) { throw "本地没有 $localSys，先构建驱动。" }
    $localSysHash = (Get-FileHash $localSys -Algorithm SHA256).Hash
    $guestSysHash = Invoke-Command -VMName $VMName -Credential $credential -ScriptBlock {
        $path = 'C:\Windows\System32\drivers\KswordARK.sys'
        if (Test-Path $path) { (Get-FileHash $path -Algorithm SHA256).Hash } else { $null }
    }
    if ($null -eq $guestSysHash) {
        throw 'guest 上没有已安装的驱动。先跑 Deploy-KswordDriverToVm.ps1。'
    }
    if ($guestSysHash -ne $localSysHash) {
        Write-Host "  本地 : $localSysHash" -ForegroundColor Yellow
        Write-Host "  guest: $guestSysHash" -ForegroundColor Yellow
        throw ('guest 上装的驱动不是本地这一份。协议结构一动，旧驱动配新 GUI ' +
               '（或反过来）会静默错位读字段，界面上不会报错。先跑 ' +
               'Deploy-KswordDriverToVm.ps1，或用 -SkipDriverMatchCheck 明确跳过。')
    }
    Write-Host '  [OK]   guest 上装的就是本地构建的这一份驱动' -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 1. 打包
# ---------------------------------------------------------------------------
Write-Host "`n--- 1. 打包 ---" -ForegroundColor Cyan
$stage = Join-Path ([IO.Path]::GetTempPath()) ('ksword-gui-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    $index = 0
    foreach ($item in $payload) {
        # 压缩包里用扁平的序号命名，不带路径。目标路径单独随清单传过去，
        # 这样 guest 侧不需要理解任何目录结构，也就不会有解压路径穿越。
        $index += 1
        Copy-Item $item.Local (Join-Path $stage ('{0:d2}.bin' -f $index)) -Force
    }
    $manifest = @()
    $index = 0
    foreach ($item in $payload) {
        $index += 1
        $manifest += [pscustomobject]@{
            Name   = ('{0:d2}.bin' -f $index)
            Remote = $item.Remote
            Sha256 = (Get-FileHash $item.Local -Algorithm SHA256).Hash
            Bytes  = (Get-Item $item.Local).Length
        }
    }
    $manifest | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $stage 'manifest.json') -Encoding UTF8

    $zip = Join-Path ([IO.Path]::GetTempPath()) ('ksword-gui-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip')
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
    $zipBytes = [IO.File]::ReadAllBytes($zip)
    Write-Host ('  {0} 个文件 -> {1:N1} MiB 压缩包' -f $payload.Count, ($zipBytes.Length / 1MB))

    # -----------------------------------------------------------------------
    # 2. 分块送进 guest
    # -----------------------------------------------------------------------
    Write-Host "`n--- 2. 传输（PowerShell Direct，分块）---" -ForegroundColor Cyan
    $session = New-PSSession -VMName $VMName -Credential $credential
    try {
        $guestZip = Invoke-Command -Session $session -ScriptBlock {
            $path = Join-Path $env:TEMP ('ksword-gui-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip')
            if (Test-Path $path) { Remove-Item $path -Force }
            $path
        }
        $offset = 0
        $chunkIndex = 0
        $chunkCount = [Math]::Ceiling($zipBytes.Length / $ChunkBytes)
        while ($offset -lt $zipBytes.Length) {
            $size = [Math]::Min($ChunkBytes, $zipBytes.Length - $offset)
            $chunk = New-Object byte[] $size
            [Array]::Copy($zipBytes, $offset, $chunk, 0, $size)
            $b64 = [Convert]::ToBase64String($chunk)
            Invoke-Command -Session $session -ScriptBlock {
                param($data, $target)
                $bytes = [Convert]::FromBase64String($data)
                $fs = [IO.File]::Open($target, [IO.FileMode]::Append, [IO.FileAccess]::Write)
                try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
            } -ArgumentList $b64, $guestZip
            $offset += $size
            $chunkIndex += 1
            Write-Host ('  块 {0}/{1}  {2:N1} / {3:N1} MiB' -f $chunkIndex, $chunkCount, ($offset / 1MB), ($zipBytes.Length / 1MB))
        }

        # 传完先对一次整包哈希。分块传输最难查的失败就是中间少一块：解压往往
        # 还能成功，落地的文件却是坏的。
        $localZipHash = (Get-FileHash $zip -Algorithm SHA256).Hash
        $guestZipHash = Invoke-Command -Session $session -ScriptBlock {
            param($path) (Get-FileHash $path -Algorithm SHA256).Hash
        } -ArgumentList $guestZip
        if ($localZipHash -ne $guestZipHash) {
            throw "压缩包传输后哈希不一致（本地 $localZipHash / guest $guestZipHash）。"
        }
        Write-Host '  [OK]   整包哈希一致' -ForegroundColor Green

        # -------------------------------------------------------------------
        # 3. 在 guest 里解压并逐个落位
        # -------------------------------------------------------------------
        Write-Host "`n--- 3. 落位 ---" -ForegroundColor Cyan
        $results = Invoke-Command -Session $session -ScriptBlock {
            param($zipPath)
            $out = @()
            $stageDir = Join-Path $env:TEMP ('ksword-gui-stage-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
            New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
            try {
                # 解到暂存目录，绝不直接 -Force 覆盖目标目录：目标目录里可能有
                # 正被内核加载的文件，那会让整次解压中止。
                Expand-Archive -Path $zipPath -DestinationPath $stageDir -Force
                $manifest = Get-Content (Join-Path $stageDir 'manifest.json') -Raw | ConvertFrom-Json
                foreach ($entry in $manifest) {
                    $source = Join-Path $stageDir $entry.Name
                    $parent = Split-Path $entry.Remote
                    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
                    $status = 'copied'
                    $note = ''
                    try {
                        Copy-Item $source $entry.Remote -Force -ErrorAction Stop
                    } catch {
                        # 拷不动。如果目标本来就是同一份内容，那这次不推也没差别。
                        if ((Test-Path $entry.Remote) -and
                            (Get-FileHash $entry.Remote -Algorithm SHA256).Hash -eq $entry.Sha256) {
                            $status = 'locked-but-identical'
                            $note = '文件被占用，但内容已经是目标内容'
                        } else {
                            $status = 'FAILED'
                            $note = $_.Exception.Message
                        }
                    }
                    $actual = if (Test-Path $entry.Remote) {
                        (Get-FileHash $entry.Remote -Algorithm SHA256).Hash
                    } else { $null }
                    $out += [pscustomobject]@{
                        Remote   = $entry.Remote
                        Status   = $status
                        Match    = ($actual -eq $entry.Sha256)
                        Expected = $entry.Sha256
                        Actual   = $actual
                        Note     = $note
                    }
                }
            } finally {
                Remove-Item $stageDir -Recurse -Force -ErrorAction SilentlyContinue
                Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
            }
            $out
        } -ArgumentList $guestZip

        $failed = @()
        foreach ($row in $results) {
            if ($row.Match) {
                Write-Host ('  [OK]   {0}  ({1})' -f $row.Remote, $row.Status) -ForegroundColor Green
            } else {
                Write-Host ('  [FAIL] {0}  {1} {2}' -f $row.Remote, $row.Status, $row.Note) -ForegroundColor Red
                Write-Host ('         期望 {0}' -f $row.Expected) -ForegroundColor Red
                Write-Host ('         实际 {0}' -f $row.Actual) -ForegroundColor Red
                $failed += $row
            }
        }
        if ($failed.Count -gt 0) {
            throw "$($failed.Count) 个文件没有落到位。"
        }
        Write-Host "`n全部落位，哈希逐个核对通过。" -ForegroundColor Green
        Write-Host 'GUI 在 guest 里是 C:\ksword\gui\Ksword5.1.exe，命令行工具是 C:\ksword\hvm_ctl.exe。'
    } finally {
        Remove-PSSession $session -ErrorAction SilentlyContinue
    }
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    if ($zip -and (Test-Path $zip)) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
}
