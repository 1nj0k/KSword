# Windows PowerShell 5.1: the BCD WMI provider supplies locale-independent values.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Root = Join-Path $env:ProgramData 'KSwordARK\amd-lab'
$script:ModulePath = $PSCommandPath
$script:TaskName = 'KSword-AMD-Lab-Verify'
$script:Current = '{fa926493-6f1c-4193-a414-58f0b2456d1e}'
$script:Bootmgr = '{9dea862c-5cdd-4e70-acc1-f32b344d4795}'
$script:DefaultType = [uint32]0x23000003
$script:SequenceType = [uint32]0x24000002
$script:HypervisorType = [uint32]0x250000f0
$script:VsmType = [uint32]0x25000100

function Assert-AmdLabAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator privileges are required; run Windows PowerShell as administrator.'
    }
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        throw 'Use Windows PowerShell 5.1 (powershell.exe), not PowerShell 7.'
    }
}

function ConvertTo-AmdBcdInstance($Embedded) {
    # Embedded WMI output is ManagementBaseObject (data only), not an invocable instance.
    # Bind the documented key properties explicitly; embedded __PATH can be empty.
    switch ([string]$Embedded.__CLASS) {
        'BcdStore' {
            $file = ([string]$Embedded.FilePath).Replace('\','\\').Replace('"','\"')
            $path = 'BcdStore.FilePath="' + $file + '"'
        }
        'BcdObject' {
            $file = ([string]$Embedded.StoreFilePath).Replace('\','\\').Replace('"','\"')
            $id = [string]$Embedded.Id
            if ($id -notmatch '^\{[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}$') {
                throw 'BCD provider returned an invalid object identifier.'
            }
            $path = 'BcdObject.Id="' + $id + '",StoreFilePath="' + $file + '"'
        }
        default { throw 'BCD provider returned an unexpected embedded class.' }
    }
    return [System.Management.ManagementObject]::new('root\WMI', $path, $null)
}

function Open-AmdBcdStore {
    $result = ([wmiclass]'root\WMI:BcdStore').OpenStore('')
    if (-not $result.ReturnValue) { throw 'Cannot open the system BCD store.' }
    return ConvertTo-AmdBcdInstance $result.Store
}

function Open-AmdBcdObject($Store, [string]$Id) {
    $result = $Store.OpenObject($Id)
    if (-not $result.ReturnValue) { throw "Cannot open BCD object $Id." }
    return ConvertTo-AmdBcdInstance $result.Object
}

function Get-AmdBcdElement($Object, [uint32]$Type) {
    # Enumerate first: a provider failure must not be confused with an absent element.
    $result = $Object.EnumerateElements()
    if (-not $result.ReturnValue) { throw "Cannot enumerate BCD object $($Object.Id)." }
    $element = @($result.Elements | Where-Object { $_.Type -eq $Type })
    if ($element.Count -eq 0) { return @{ present = $false; value = $null } }
    if ($element.Count -ne 1) { throw 'Duplicate BCD element.' }
    switch ($Type) {
        $script:DefaultType { $value = [string]$element[0].Id }
        $script:SequenceType { $value = @($element[0].Ids) }
        default { $value = [uint64]$element[0].Integer }
    }
    return @{ present = $true; value = $value }
}

function Get-AmdBootEnvironment {
    $store = Open-AmdBcdStore
    $loader = Open-AmdBcdObject $store $script:Current
    $manager = Open-AmdBcdObject $store $script:Bootmgr
    $os = Get-CimInstance Win32_OperatingSystem
    $computer = Get-CimInstance Win32_ComputerSystem
    $guard = Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard
    # Encryption state must be known before modifying any boot settings.
    $volume = Get-BitLockerVolume -MountPoint $env:SystemDrive
    $secure = Confirm-SecureBootUEFI
    return [ordered]@{
        machine = $env:COMPUTERNAME; systemDrive = $env:SystemDrive
        loader = [string]$loader.Id; bootId = $os.LastBootUpTime.ToUniversalTime().ToString('o')
        osVersion = $os.Version; hypervisor = [bool]$computer.HypervisorPresent
        vbs = [int]$guard.VirtualizationBasedSecurityStatus
        services = @($guard.SecurityServicesRunning)
        secureBoot = [bool]$secure; bitLockerProtection = [string]$volume.ProtectionStatus
        defaultLoader = Get-AmdBcdElement $manager $script:DefaultType
        bootSequence = Get-AmdBcdElement $manager $script:SequenceType
        hypervisorLaunch = Get-AmdBcdElement $loader $script:HypervisorType
        vsmLaunch = Get-AmdBcdElement $loader $script:VsmType
    }
}

function Initialize-AmdLabDirectory {
    $mutating = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    # Reject a planted parent or state directory before adopting any journal.
    foreach ($path in @((Join-Path $env:ProgramData 'KSwordARK'), $script:Root)) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing reparse point: $path" }
            $oldAcl = Get-Acl -LiteralPath $path
            $ownerSid = $oldAcl.GetOwner([Security.Principal.SecurityIdentifier]).Value
            if ($ownerSid -notin @('S-1-5-18','S-1-5-32-544')) { throw "Directory owner is not Administrator/SYSTEM: $path" }
            foreach ($rule in $oldAcl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
                if ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) { continue }
                $mutating = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
                    [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
                if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544') -and
                    ($rule.FileSystemRights -band $mutating)) { throw "Non-administrator can alter protected path: $path" }
            }
        } else {
            New-Item -ItemType Directory -Path $path | Out-Null
            # Apply private ACL before storing any data, including at the parent.
            $private = New-Object Security.AccessControl.DirectorySecurity
            $private.SetAccessRuleProtection($true,$false)
            foreach ($sidText in @('S-1-5-18','S-1-5-32-544')) {
                $sid=New-Object Security.Principal.SecurityIdentifier($sidText)
                $private.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
            }
            $private.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
            Set-Acl -LiteralPath $path -AclObject $private
        }
    }
    # Files cannot override inheritance to make the ownership record untrusted.
    foreach ($file in @(Get-ChildItem -LiteralPath $script:Root -Force)) {
        if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected directory or reparse point in boot state.' }
        $oldAcl=Get-Acl -LiteralPath $file.FullName
        if ($oldAcl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Untrusted boot-state file owner.' }
        foreach ($rule in $oldAcl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
            if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544') -and
                ($rule.FileSystemRights -band $mutating)) { throw 'Non-administrator writable boot-state file.' }
        }
    }
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sidText in @('S-1-5-18', 'S-1-5-32-544')) {
        $sid = New-Object Security.Principal.SecurityIdentifier($sidText)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    Set-Acl -LiteralPath $script:Root -AclObject $acl
}

function Write-AmdLabJson([string]$Name, $Value) {
    $path = Join-Path $script:Root $Name
    $temporary = "$path.tmp"
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $path -Force
}

function Read-AmdLabJson([string]$Name) {
    $path = Join-Path $script:Root $Name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing state: $path" }
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

function Test-AmdElementEqual($Left, $Right) {
    return (($Left.present -eq $Right.present) -and
        ((@($Left.value) -join '|') -eq (@($Right.value) -join '|')))
}

function Assert-AmdOriginalUnchanged($Baseline, $Environment) {
    if ($Baseline.machine -ne $Environment.machine -or
        $Baseline.systemDrive -ne $Environment.systemDrive) { throw 'Baseline belongs to another system.' }
    $original = Open-AmdBcdObject (Open-AmdBcdStore) $Baseline.loader
    if (-not (Test-AmdElementEqual $Baseline.defaultLoader $Environment.defaultLoader) -or
        -not (Test-AmdElementEqual $Baseline.hypervisorLaunch (Get-AmdBcdElement $original $script:HypervisorType)) -or
        -not (Test-AmdElementEqual $Baseline.vsmLaunch (Get-AmdBcdElement $original $script:VsmType))) {
        throw 'Original/default boot configuration changed externally; refusing to overwrite it.'
    }
}

function Test-AmdOwnedSequence($Sequence, [string]$Original, [string]$Lab) {
    if (-not $Sequence.present) { return $true }
    $ids = @($Sequence.value)
    return ($ids.Count -eq 1 -and ($ids[0] -eq $Original -or $ids[0] -eq $Lab))
}

function Get-AmdBootVerdict($Baseline, $Owner, $Environment, [string]$Mode) {
    Assert-AmdOriginalUnchanged $Baseline $Environment
    if ($Mode -eq 'Lab') {
        if ($Environment.loader -eq $Owner.labLoader -and -not $Environment.hypervisor -and $Environment.vbs -in @(0,1)) {
            return 'LabHostReady'
        }
    } elseif ($Environment.loader -eq $Baseline.loader -and
        $Environment.hypervisor -eq $Baseline.hypervisor -and $Environment.vbs -eq $Baseline.vbs -and
        (@($Environment.services) -join ',') -eq (@($Baseline.services) -join ',') -and
        $Environment.secureBoot -eq $Baseline.secureBoot -and
        $Environment.bitLockerProtection -eq $Baseline.bitLockerProtection) {
        return 'NormalRestored'
    }
    return 'VerificationFailed'
}

function Register-AmdBootVerifier {
    $existing = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
    $modulePath = Join-Path $script:Root 'AmdLabBoot.psm1'
    $verifierPath = Join-Path $script:Root 'Verify-Boot.ps1'
    if ($existing) {
        if (@($existing.Actions).Count -ne 1 -or $existing.Actions[0].Arguments -ne
            "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$verifierPath`"" -or
            $existing.Actions[0].Execute -ne "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -or
            $existing.Principal.UserId -notin @('SYSTEM','S-1-5-18')) {
            throw 'A different task already owns the verifier name.'
        }
    }
    Copy-Item -LiteralPath $script:ModulePath -Destination $modulePath -Force
    @'
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'AmdLabBoot.psm1') -Force
Invoke-AmdBootVerification
'@ | Set-Content -LiteralPath $verifierPath -Encoding UTF8
    $action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
        -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$verifierPath`""
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    Register-ScheduledTask -TaskName $script:TaskName -Action $action -Trigger $trigger `
        -Settings $settings -User SYSTEM -RunLevel Highest -Force | Out-Null
}

function Invoke-AmdBootVerification {
    try {
        Assert-AmdLabAdmin
        $lock = [IO.File]::Open((Join-Path $script:Root 'transaction.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
        try {
            $pending = Read-AmdLabJson 'pending.json'
            # A requested boot is inspected once; startup does not modify BCD or policy.
            if ($pending.completed) { return }
            $environment = Get-AmdBootEnvironment
            if ($pending.sourceBootId -eq $environment.bootId) { return }
            $verdict = Get-AmdBootVerdict (Read-AmdLabJson 'baseline.json') (Read-AmdLabJson 'owner.json') $environment $pending.mode
            Write-AmdLabJson 'verification.json' @{ status=$verdict; runId=$pending.runId; environment=$environment; utc=[DateTime]::UtcNow.ToString('o') }
            $pending.completed = $true
            Write-AmdLabJson 'pending.json' $pending
        } finally { $lock.Dispose() }
    } catch {
        # A verifier failure is explicit; a readback of BCD alone never becomes Ready.
        Write-AmdLabJson 'verification.json' @{ status='VerificationFailed'; error=$_.Exception.Message }
        throw
    }
}

function Invoke-AmdLabBoot {
    [CmdletBinding()]
    param([ValidateSet('Lab','Normal')][string]$Mode, [switch]$Check, [switch]$NoRestart)
    try {
        Assert-AmdLabAdmin
        $environment = Get-AmdBootEnvironment
        if ($Check) {
            $status = 'Blocked'
            if (Test-Path -LiteralPath (Join-Path $script:Root 'owner.json')) {
                $status = Get-AmdBootVerdict (Read-AmdLabJson 'baseline.json') (Read-AmdLabJson 'owner.json') $environment $Mode
                if (Test-Path -LiteralPath (Join-Path $script:Root 'pending.json')) {
                    $pending=Read-AmdLabJson 'pending.json'
                    if (-not $pending.completed -and $pending.mode -eq $Mode -and $pending.sourceBootId -eq $environment.bootId -and
                        $environment.bootSequence.present -and @($environment.bootSequence.value).Count -eq 1 -and
                        $environment.bootSequence.value[0] -eq $pending.target) { $status='PendingReboot' }
                }
            }
            return [pscustomobject]@{ status = $status; environment = $environment; readOnly = $true }
        }
        Initialize-AmdLabDirectory
        # Serialize the complete read/modify/readback transaction across processes.
        $lock = [IO.File]::Open((Join-Path $script:Root 'transaction.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
        try {
            $environment = Get-AmdBootEnvironment
            if (-not (Test-Path -LiteralPath (Join-Path $script:Root 'baseline.json'))) {
                if ($Mode -ne 'Lab') { throw 'No baseline exists; nothing can be restored safely.' }
                if ($environment.bootSequence.present) { throw 'Another one-time boot sequence is already pending.' }
                if (-not $environment.defaultLoader.present -or $environment.defaultLoader.value -ne $environment.loader) {
                    throw 'Start from the current default daily boot entry to preserve automatic return.'
                }
                $backupPath = Join-Path $script:Root 'baseline.bcd'
                if (Test-Path -LiteralPath $backupPath) { throw 'Incomplete prior baseline; inspect it before retrying.' }
                $output = & "$env:SystemRoot\System32\bcdedit.exe" /export $backupPath 2>&1
                if ($LASTEXITCODE -ne 0) { throw "BCD export failed: $output" }
                Write-AmdLabJson 'baseline.json' $environment
            }
            $baseline = Read-AmdLabJson 'baseline.json'
            Assert-AmdOriginalUnchanged $baseline $environment
            if ($environment.bitLockerProtection -ne 'Off') {
                throw 'BitLocker protection is active. No BCD changes made; recovery-key/protection handling is required separately.'
            }
            $store = Open-AmdBcdStore
            if (-not (Test-Path -LiteralPath (Join-Path $script:Root 'owner.json'))) {
                if ($Mode -ne 'Lab') { throw 'Missing experimental-entry ownership record.' }
                # The provider assigns the GUID. An interrupted copy is never retried or deleted blindly.
                Write-AmdLabJson 'owner.json' @{ labLoader = ''; created = $false; original = $baseline.loader }
                $copy = $store.CopyObject('', $baseline.loader, [uint32]1)
                if (-not $copy.ReturnValue) { throw 'BCD copy failed; ownership journal retained.' }
                $id = [string]$copy.Object.Id
                Write-AmdLabJson 'owner.json' @{ labLoader = $id; created = $true; original = $baseline.loader }
            }
            $owner = Read-AmdLabJson 'owner.json'
            if (-not $owner.created -or $owner.original -ne $baseline.loader -or $owner.labLoader -eq $baseline.loader -or $owner.labLoader -notmatch '^\{[0-9a-fA-F-]{36}\}$') { throw 'Incomplete or inconsistent ownership journal.' }
            if (-not (Test-AmdOwnedSequence $environment.bootSequence $baseline.loader $owner.labLoader)) {
                throw 'Another tool owns the pending boot sequence.'
            }
            if ($environment.bootSequence.present) {
                # Matching GUIDs alone do not establish ownership of a pending selection.
                $previous = Read-AmdLabJson 'pending.json'
                if ($previous.completed -or $previous.sourceBootId -ne $environment.bootId -or
                    $previous.target -ne $environment.bootSequence.value[0]) {
                    throw 'Pending boot selection has no matching live transaction journal.'
                }
            }
            $lab = Open-AmdBcdObject $store $owner.labLoader
            if ($Mode -eq 'Lab') {
                $description = $lab.SetStringElement([uint32]0x12000004, 'Windows 10 - KSword AMD Lab (one boot)')
                if (-not $description.ReturnValue) { throw 'Cannot label experimental boot entry.' }
                foreach ($type in @($script:HypervisorType, $script:VsmType)) {
                    $result = $lab.SetIntegerElement($type, [uint64]0)
                    if (-not $result.ReturnValue) { throw 'Experimental BCD write failed; daily entry untouched.' }
                    $readback = Get-AmdBcdElement $lab $type
                    if (-not $readback.present -or $readback.value -ne 0) { throw 'Experimental BCD readback failed.' }
                }
                $target = $owner.labLoader
            } else { $target = $baseline.loader }
            Register-AmdBootVerifier
            Write-AmdLabJson 'pending.json' @{ runId = [guid]::NewGuid().ToString(); mode = $Mode; target = $target; sourceBootId = $environment.bootId; completed = $false }
            $manager = Open-AmdBcdObject $store $script:Bootmgr
            if (-not (Test-AmdElementEqual (Get-AmdBcdElement $manager $script:SequenceType) $environment.bootSequence)) { throw 'Boot selection changed during preparation.' }
            $result = $manager.SetObjectListElement($script:SequenceType, [string[]]@($target))
            if (-not $result.ReturnValue) { throw 'Cannot schedule the requested one-time boot.' }
            $readback = Get-AmdBcdElement $manager $script:SequenceType
            if (-not $readback.present -or @($readback.value).Count -ne 1 -or $readback.value[0] -ne $target) {
                throw 'One-time boot readback failed; inspect BCD before restarting.'
            }
            Write-AmdLabJson 'last-operation.json' @{ status = 'PendingReboot'; mode = $Mode; target = $target; utc = [DateTime]::UtcNow.ToString('o') }
        } finally { $lock.Dispose() }
        [pscustomobject]@{ status = 'PendingReboot'; mode = $Mode; target = $target; stateDirectory = $script:Root }
        # No /f: unsaved applications can veto this normal shutdown request.
        if (-not $NoRestart) {
            & "$env:SystemRoot\System32\shutdown.exe" /r /t 0
            if ($LASTEXITCODE -ne 0) { throw 'Restart was not accepted; boot choice remains pending.' }
        }
    } catch {
        [pscustomobject]@{ status = 'Blocked'; error = $_.Exception.Message; mode = $Mode }
        throw
    }
}

Export-ModuleMember -Function Invoke-AmdLabBoot, Invoke-AmdBootVerification
