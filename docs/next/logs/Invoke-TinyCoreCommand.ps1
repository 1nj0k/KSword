param(
    [Parameter(Mandatory)][string] $Command,
    [string] $VMName = 'KSword-HVM-Target',
    [string] $ShotName = 'tinycore-command.png'
)

$ErrorActionPreference = 'Stop'
$env:COMPUTERNAME = [Environment]::MachineName
$cred = New-Object PSCredential('felix',
    (ConvertTo-SecureString 'password' -AsPlainText -Force))
$session = New-PSSession -VMName $VMName -Credential $cred
try {
    $keys = @($Command.ToCharArray() | ForEach-Object {
        $value = [int][char]$_
        if (($_ -cge 'A' -and $_ -cle 'Z') -or
            '~!@#$%^&*()_+{}|:"<>?'.Contains([string]$_)) {
            $value -bor 0x10000
        } else { $value }
    }) + @(0xFF0D)
    $remoteShot = 'C:\vmware\shot-command.png'
    $vnc = Join-Path $PSScriptRoot 'Get-VmwareVnc.ps1'
    Invoke-Command -Session $session -FilePath $vnc -ArgumentList `
        '127.0.0.1', 5900, $remoteShot, $keys, 65
    Start-Sleep -Seconds 2
    Invoke-Command -Session $session -FilePath $vnc -ArgumentList `
        '127.0.0.1', 5900, $remoteShot
    Copy-Item -FromSession $session -Path $remoteShot `
        -Destination (Join-Path $PSScriptRoot $ShotName) -Force
} finally {
    Remove-PSSession $session
}
