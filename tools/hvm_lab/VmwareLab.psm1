# VMware lifecycle helpers. Import explicitly; importing never powers on a VM.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-KswordVmx([string]$Path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*([^#][^=]*?)\s*=\s*"(.*)"\s*$') { $values[$matches[1].Trim()] = $matches[2] }
    }
    return $values
}

function Set-KswordVmx([string]$Path, [hashtable]$Values) {
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*([^#][^=]*?)\s*=') {
            if ($Values.ContainsKey($matches[1].Trim())) { continue }
        }
        $lines.Add($line)
    }
    foreach ($key in ($Values.Keys | Sort-Object)) { $lines.Add(('{0} = "{1}"' -f $key,$Values[$key])) }
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-KswordAmdLabVM {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$SourceVmx,
          [Parameter(Mandatory)][string]$DestinationDirectory,
          [string]$Vmrun = 'D:\Software\Vmware\vmrun.exe')
    $source = (Resolve-Path -LiteralPath $SourceVmx).Path
    $destination = [IO.Path]::GetFullPath($DestinationDirectory)
    if ($destination.StartsWith((Split-Path $source) + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'The full clone must be outside the source VM directory.'
    }
    if (Test-Path -LiteralPath $destination) { throw 'Destination already exists; refusing to overwrite or merge a VM.' }
    $config = Get-KswordVmx $source
    if ($config['virtualHW.version'] -ne '19') { throw 'Expected virtual hardware version 19.' }
    $running = & $Vmrun list 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot establish running VM inventory: $running" }
    if ($running -contains $source) { throw 'Source VM is running; close the guest normally before cloning.' }
    New-Item -ItemType Directory -Path $destination | Out-Null
    $target = Join-Path $destination 'KSword-AMD-Lab.vmx'
    # vmrun full cloning materializes disks instead of creating a linked parent dependency.
    & $Vmrun clone $source $target full '-cloneName=KSword-AMD-Lab'
    if ($LASTEXITCODE -ne 0) { throw 'Full clone failed; retained destination for diagnosis, no automatic deletion.' }
    if (-not (Test-Path -LiteralPath $target)) { throw 'Clone reported success without a VMX.' }
    # Cold boot only. The source suspend checkpoint is never resumed in this clone.
    Set-KswordVmx $target @{
        'displayName'='KSword-AMD-Lab'; 'checkpoint.vmState'=''; 'numvcpus'='4';
        'cpuid.coresPerSocket'='4'; 'memsize'='8192'; 'vhv.enable'='TRUE';
        'serial0.present'='TRUE'; 'serial0.fileType'='pipe';
        'serial0.fileName'='\\.\pipe\KSword-AMD-Lab'; 'serial0.pipe.endPoint'='server';
        'serial0.tryNoRxLoss'='FALSE'; 'serial0.yieldOnMsrRead'='TRUE'
    }
    @{source=$source; vmx=$target; createdUtc=[DateTime]::UtcNow.ToString('o');
      guestOsVerified=$false; cleanShutdownSnapshot=$false; hardwareVerified=$false} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'lab-ownership.json') -Encoding UTF8
    return $target
}

function Set-KswordAmdLabCpu {
    param([Parameter(Mandatory)][string]$Vmx, [ValidateSet(1,2,4,8)][int]$CpuCount,
          [string]$Vmrun = 'D:\Software\Vmware\vmrun.exe')
    $full = (Resolve-Path -LiteralPath $Vmx).Path
    $record = Get-Content -LiteralPath (Join-Path (Split-Path $full) 'lab-ownership.json') -Raw | ConvertFrom-Json
    if ($record.vmx -ne $full) { throw 'VM ownership mismatch.' }
    $running = & $Vmrun list 2>&1
    if ($LASTEXITCODE -ne 0 -or $running -contains $full) { throw 'CPU changes require a powered-off VM.' }
    $config = Get-KswordVmx $full
    if ($config['checkpoint.vmState']) { throw 'Suspended state must be resolved by a normal guest shutdown before changing CPUs.' }
    Set-KswordVmx $full @{'numvcpus'="$CpuCount"; 'cpuid.coresPerSocket'="$CpuCount"; 'memsize'='8192'}
}

function Get-KswordVmwareEvidence {
    param([Parameter(Mandatory)][string]$Vmx)
    $config = Get-KswordVmx $Vmx
    $log = Join-Path (Split-Path $Vmx) 'vmware.log'
    $matches = if (Test-Path $log) { @(Select-String -LiteralPath $log -Pattern 'Monitor Mode:|WHP|Hyper-V|AMD-V|SVM|VHV' | ForEach-Object Line) } else { @() }
    # ULM/WHP is not the native AMD-V configuration required by this experiment.
    $native = @($matches | Where-Object { $_ -match 'Monitor Mode:\s*CPL0' }).Count -gt 0
    $ulm = @($matches | Where-Object { $_ -match 'Monitor Mode:\s*ULM' }).Count -gt 0
    [pscustomobject]@{ vmx=$Vmx; configuration=$config; nativeLogObserved=($native -and -not $ulm);
        guestSvmVerified=$false; logEvidence=$matches }
}

function Invoke-KswordLabGuest {
    # Use an authenticated network PSSession; never vmrun -gu/-gp command-line credentials.
    param([Parameter(Mandatory)][System.Management.Automation.Runspaces.PSSession]$Session,
          [Parameter(Mandatory)][string]$GuestScript,
          [Parameter(Mandatory)][hashtable]$Parameters,
          [Parameter(Mandatory)][string]$EvidenceDirectory)
    $os = Invoke-Command -Session $Session -ScriptBlock { Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,LastBootUpTime }
    if ($os.Caption -notmatch 'Windows 10' -or [int]$os.BuildNumber -ge 22000) { throw 'Guest is not verified Windows 10.' }
    if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Use a new host evidence directory.' }
    New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
    $os | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'guest-os.json') -Encoding UTF8
    $runId=[guid]::NewGuid().ToString()
    $remote=Invoke-Command -Session $Session -ArgumentList $runId -ScriptBlock {
        param($Id)
        $path=Join-Path $env:ProgramData ("KSwordAMDLab\"+$Id)
        New-Item -ItemType Directory -Path $path | Out-Null
        $path
    }
    $local=(Resolve-Path -LiteralPath $GuestScript).Path
    # Copy companions too: FilePath alone would not preserve the guest's PSScriptRoot.
    Copy-Item -Path (Join-Path (Split-Path $local) '*.ps1') -Destination $remote -ToSession $Session
    $remoteScript=Join-Path $remote (Split-Path $local -Leaf)
    $options=$Parameters.Clone()
    $options.EvidenceDirectory=Join-Path $remote 'evidence'
    @{phase='before';runId=$runId;remoteScript=$remoteScript;result='Pending'} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'dispatch.json') -Encoding UTF8
    try {
        Invoke-Command -Session $Session -ArgumentList $remoteScript,$options -ScriptBlock {
            param($Script,$Options)
            & $Script @Options
        }
    } finally {
        # Copying evidence does not imply a timed-out driver stopped; never issue automatic cleanup controls.
        Copy-Item -LiteralPath $options.EvidenceDirectory -Destination $EvidenceDirectory -FromSession $Session -Recurse -ErrorAction Continue
    }
}

Export-ModuleMember -Function New-KswordAmdLabVM,Set-KswordAmdLabCpu,Get-KswordVmwareEvidence,Invoke-KswordLabGuest
