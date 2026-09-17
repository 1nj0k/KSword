# Guest-only preparation. Inspect by default; -Configure changes this clone and never restarts it.
[CmdletBinding()]
param([switch]$Configure, [Parameter(Mandatory)][string]$CloneManifest)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$record=Get-Content -LiteralPath $CloneManifest -Raw | ConvertFrom-Json
if ($record.vmx -notmatch 'KSword-AMD-Lab\.vmx$') { throw 'Expected the manifest from the dedicated full clone.' }
$os=Get-CimInstance Win32_OperatingSystem
$machine=Get-CimInstance Win32_ComputerSystem
if ($machine.Manufacturer -notmatch 'VMware' -or $os.Caption -notmatch 'Windows 10' -or [int]$os.BuildNumber -ge 22000) {
    throw 'Restricted to a verified VMware Windows 10 guest.'
}
$guard=Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard
$evidence=@{caption=$os.Caption;build=$os.BuildNumber;bootId=$os.LastBootUpTime.ToUniversalTime().ToString('o');
    processors=$machine.NumberOfLogicalProcessors;vbs=$guard.VirtualizationBasedSecurityStatus;
    secureBoot=(Confirm-SecureBootUEFI);sourceManifest=$record;configured=$false}
if ($Configure) {
    $principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated inside the clone.' }
    if ($evidence.secureBoot) { throw 'Power off the clone and disable its virtual Secure Boot before enabling test signing. Do not change host firmware.' }
    $bcd=Join-Path $env:SystemRoot 'System32\bcdedit.exe'
    foreach ($arguments in @(@('/set','{current}','hypervisorlaunchtype','off'),@('/set','{current}','vsmlaunchtype','off'),
        @('/set','{current}','testsigning','on'),@('/debug','{current}','on'),@('/dbgsettings','serial','debugport:1','baudrate:115200'))) {
        & $bcd @arguments
        if ($LASTEXITCODE -ne 0) { throw 'Guest BCD preparation failed; do not start SVM.' }
    }
    $deviceGuard='HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
    New-Item -Path $deviceGuard -Force | Out-Null
    New-ItemProperty -Path $deviceGuard -Name EnableVirtualizationBasedSecurity -Value 0 -PropertyType DWord -Force | Out-Null
    $crash='HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    Set-ItemProperty -Path $crash -Name CrashDumpEnabled -Value 2
    New-ItemProperty -Path $crash -Name AlwaysKeepMemoryDump -Value 1 -PropertyType DWord -Force | Out-Null
    Set-CimInstance -InputObject $machine -Property @{AutomaticManagedPagefile=$true} | Out-Null
    $evidence.configured=$true
    $evidence.nextStep='Cold reboot; verify VBS stopped, KD connection, matching SYS/PDB, driver breakpoint and a controlled dump before SVM.'
}
$evidence | ConvertTo-Json -Depth 8
