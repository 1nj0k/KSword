param([bool]$Guest=$false)
$ErrorActionPreference='Stop'
$capturedUtc=[DateTime]::UtcNow.ToString('o')
$watch=[Diagnostics.Stopwatch]::StartNew()
$os=Get-CimInstance Win32_OperatingSystem
$build=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,Description,ProcessorId,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed,VirtualizationFirmwareEnabled,SecondLevelAddressTranslationExtensions)
$bios=Get-CimInstance Win32_BIOS | Select-Object Manufacturer,SMBIOSBIOSVersion,Version,ReleaseDate
$processes=@(Get-CimInstance Win32_Process -Filter "Name='System' OR Name='wininit.exe' OR Name='services.exe' OR Name='lsass.exe' OR Name='explorer.exe' OR Name='vmware-vmx.exe' OR Name='vmware.exe'" | ForEach-Object {
    [ordered]@{name=$_.Name;pid=$_.ProcessId;parentPid=$_.ParentProcessId;createdUtc=$(if ($_.CreationDate) {$_.CreationDate.ToUniversalTime().ToString('o')} else {$null});userTime100ns=$_.UserModeTime;kernelTime100ns=$_.KernelModeTime;workingSetBytes=$_.WorkingSetSize;privateBytes=$_.PrivatePageCount;path=$_.ExecutablePath}
})
$result=[ordered]@{schemaVersion=1;capturedUtc=$capturedUtc;machine=[Environment]::MachineName;role=$(if($Guest){'windows1'}else{'hyperv-root'});os=[ordered]@{caption=$os.Caption;version=$os.Version;build=$os.BuildNumber;ubr=$build.UBR;displayVersion=$build.DisplayVersion;bootUtc=$os.LastBootUpTime.ToUniversalTime().ToString('o');uptimeSeconds=([DateTime]::UtcNow-$os.LastBootUpTime.ToUniversalTime()).TotalSeconds};cpu=$cpu;bios=$bios;processes=$processes;memory=[ordered]@{visibleKiB=$os.TotalVisibleMemorySize;freeKiB=$os.FreePhysicalMemory};powerScheme=(& powercfg /getactivescheme | Out-String).Trim()}
try {$result.deviceGuard=Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard | Select-Object VirtualizationBasedSecurityStatus,SecurityServicesConfigured,SecurityServicesRunning,AvailableSecurityProperties,RequiredSecurityProperties} catch {$result.deviceGuardError=$_.Exception.Message}
if ($Guest) {
    $result.hvmCliSha256=(Get-FileHash -LiteralPath 'C:\ksword\hvm_ctl.exe').Hash
    $result.hvm=[ordered]@{}
    foreach ($command in @('status','nested-page-query','cpuid-view')) {
        $queryWatch=[Diagnostics.Stopwatch]::StartNew()
        $raw=(& 'C:\ksword\hvm_ctl.exe' --json $command 2>&1 | Out-String)
        $code=$LASTEXITCODE
        $queryWatch.Stop()
        $parsed=$null
        try {$parsed=$raw | ConvertFrom-Json} catch {}
        $result.hvm[$command]=[ordered]@{exitCode=$code;elapsedMs=$queryWatch.Elapsed.TotalMilliseconds;raw=$raw;parsed=$parsed}
    }
    $driver=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\KswordARK'
    $driverPath=$driver.ImagePath -replace '^\\\?\?\\',''
    $result.driver=[ordered]@{imagePath=$driver.ImagePath;sha256=$(if(Test-Path -LiteralPath $driverPath){(Get-FileHash -LiteralPath $driverPath).Hash}else{$null})}
    $result.vmware=[ordered]@{}
    $vmwarePath=($processes | Where-Object {$_.name -eq 'vmware-vmx.exe'} | Select-Object -First 1).path
    if ($vmwarePath -and (Test-Path -LiteralPath $vmwarePath)) {
        $file=Get-Item -LiteralPath $vmwarePath
        $result.vmware=[ordered]@{fileVersion=$file.VersionInfo.FileVersion;productVersion=$file.VersionInfo.ProductVersion;sha256=(Get-FileHash -LiteralPath $vmwarePath).Hash}
    }
    $result.bootConfiguration=(& bcdedit /enum '{current}' | Out-String)
    $result.ipAddresses=@(Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias,IPAddress,PrefixLength)
    $vmx='C:\Users\felix\Documents\Virtual Machines\Other Linux 6.x kernel 64-bit\Other Linux 6.x kernel 64-bit.vmx'
    if (Test-Path -LiteralPath $vmx) {
        $result.vmware.configSha256=(Get-FileHash -LiteralPath $vmx).Hash
        $result.vmware.config=@(Get-Content -LiteralPath $vmx | Where-Object {$_ -notmatch '(?i)password|secret|token'})
    }
    $serial='C:\vmware\hltprobe.log'
    $fs=[IO.File]::Open($serial,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=New-Object IO.StreamReader($fs)
    try {$result.guestSerial=$reader.ReadToEnd()} finally {$reader.Dispose();$fs.Dispose()}
} else {
    $result.hypervBinaries=@(foreach($path in @('C:\Windows\System32\vmms.exe','C:\Windows\System32\hvix64.exe','C:\Windows\System32\drivers\vid.sys')) {
        if(Test-Path -LiteralPath $path) {$item=Get-Item -LiteralPath $path;[ordered]@{path=$path;fileVersion=$item.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $path).Hash}}
    })
    $result.virtualMachines=@(Get-VM | Select-Object Name,Id,State,Generation,Version,ProcessorCount,MemoryAssigned,Uptime,ConfigurationLocation)
    $result.targetProcessor=Get-VMProcessor -VMName 'KSword-HVM-Target' | Select-Object Count,ExposeVirtualizationExtensions,HwThreadCountPerCore,CompatibilityForMigrationEnabled,CompatibilityForOlderOperatingSystemsEnabled
    $result.targetFirmware=Get-VMFirmware -VMName 'KSword-HVM-Target' | Select-Object SecureBoot,SecureBootTemplate
    $result.targetMemory=Get-VMMemory -VMName 'KSword-HVM-Target' | Select-Object DynamicMemoryEnabled,Startup,Minimum,Maximum
}
$watch.Stop()
$result.captureDurationMs=$watch.Elapsed.TotalMilliseconds
$result
