param(
    [string]$VmxPath='C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx',
    [string]$SerialPath='C:\vmware\hltprobe.log',
    [int]$TailBytes=65536
)
$ErrorActionPreference='Stop'
function Read-SharedTail([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return $null }
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $length=$stream.Length
        $offset=[Math]::Max(0,$length-$TailBytes)
        $null=$stream.Seek($offset,[IO.SeekOrigin]::Begin)
        $reader=[IO.StreamReader]::new($stream)
        try { $tail=$reader.ReadToEnd() } finally { $reader.Dispose() }
        [ordered]@{path=$Path;length=$length;offset=$offset;lastWriteUtc=(Get-Item -LiteralPath $Path).LastWriteTimeUtc.ToString('o');text=$tail}
    } finally { $stream.Dispose() }
}
$os=Get-CimInstance Win32_OperatingSystem
$driver=Get-Service KswordARK -ErrorAction SilentlyContinue
$image=$null
$driverHash=$null
if ($driver) {
    $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK').ImagePath -replace '^\\\?\?\\',''
    if ($image -match '^\\SystemRoot\\') { $image=$image -replace '^\\SystemRoot',$env:SystemRoot }
    if (Test-Path -LiteralPath $image) { $driverHash=(Get-FileHash -LiteralPath $image).Hash }
}
$state=$null
if ($driver -and $driver.Status -eq 'Running') {
    $raw=(& C:\ksword\hvm_ctl.exe --json status 2>&1 | Out-String)
    $code=$LASTEXITCODE
    $parsed=$null
    try { $parsed=$raw | ConvertFrom-Json } catch { }
    $state=[ordered]@{exitCode=$code;raw=$raw;parsed=$parsed}
}
[ordered]@{
    utc=[DateTime]::UtcNow.ToString('o')
    bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o')
    osCaption=$os.Caption
    driverService=$(if($driver){[string]$driver.Status}else{'Absent'})
    driverImagePath=$image
    driverSha256=$driverHash
    hvm=$state
    vmware=@(Get-CimInstance Win32_Process -Filter "Name='vmware-vmx.exe'" | ForEach-Object {
        [ordered]@{pid=$_.ProcessId;createdUtc=$_.CreationDate.ToUniversalTime().ToString('o');commandLine=$_.CommandLine;path=$_.ExecutablePath}
    })
    vmxSha256=$(if(Test-Path -LiteralPath $VmxPath){(Get-FileHash -LiteralPath $VmxPath).Hash}else{$null})
    vmxConfig=@(Get-Content -LiteralPath $VmxPath | Where-Object {$_ -notmatch '(?i)password|secret|token'})
    vmwareLog=Read-SharedTail (Join-Path (Split-Path $VmxPath) 'vmware.log')
    # Early VMX rejection writes here before opening the VM directory's log.
    startupLogs=@(Get-ChildItem -LiteralPath (Join-Path $env:TEMP ('vmware-'+$env:USERNAME)) -Filter 'vmware-vmx-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 3 |
        ForEach-Object {Read-SharedTail $_.FullName})
    serial=Read-SharedTail $SerialPath
}
