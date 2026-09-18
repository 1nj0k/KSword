# Runs the production loader against fake SCM/registry/OS/signature providers.
# No driver, certificate, BCD or real service is touched.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = Join-Path $PSScriptRoot ('artifacts/loader-test-' + [guid]::NewGuid().ToString('N'))
$local = Join-Path $root 'candidate'
$source = Join-Path $root 'share'
New-Item -ItemType Directory -Path $local,$source -Force | Out-Null
foreach ($dir in @($local,$source)) {
    'driver fixture' | Set-Content (Join-Path $dir 'KswordARK.sys')
    'pdb fixture' | Set-Content (Join-Path $dir 'KswordARK.pdb')
    Copy-Item (Join-Path $repo 'tools/hvm_ctl/test_query_json.exe') (Join-Path $dir 'hvm_ctl.exe')
    $hashes = @{}
    foreach ($name in @('KswordARK.sys','KswordARK.pdb','hvm_ctl.exe')) {
        $hashes[$name] = (Get-FileHash (Join-Path $dir $name)).Hash
    }
    @{candidateSha256=$hashes} | ConvertTo-Json | Set-Content (Join-Path $dir 'identity.json') -Encoding UTF8
}
# Replace only the non-mockable identity constructor, leaving the guard itself in place.
$text = Get-Content (Join-Path $PSScriptRoot 'Load-GuestCandidate.ps1') -Raw
$identityLine = '$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()'
if (-not $text.Contains($identityLine)) { throw 'Update test identity adapter for changed production preflight.' }
$text = $text.Replace($identityLine, '$principal = New-Object PSObject; $principal | Add-Member ScriptMethod IsInRole { return $true }')
$loader = [scriptblock]::Create($text)
function Get-CimInstance($ClassName) {
    if ($ClassName -eq 'Win32_OperatingSystem') {
        return [pscustomobject]@{Caption='Windows 10';BuildNumber='19042';LastBootUpTime=[datetime]'2026-09-18'}
    }
    return [pscustomobject]@{Manufacturer='VMware, Inc.'}
}
function Get-AuthenticodeSignature { return [pscustomobject]@{Status='Valid';StatusMessage='mock only'} }
function Test-Path {
    param([string]$LiteralPath,[string]$PathType='Any')
    if ($LiteralPath -like 'HKLM:*') { return $true }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType
}
function Get-ItemProperty { return [pscustomobject]@{ImagePath=$script:servicePath} }
function sc.exe {
    $script:commands.Add(($args -join ' '))
    $global:LASTEXITCODE = 0
    switch ($args[0]) {
        'queryex' { "STATE : $script:serviceState" }
        'qc' { 'TYPE : 1 KERNEL_DRIVER' }
        'config' { $script:servicePath = Join-Path $local 'KswordARK.sys'; 'SUCCESS' }
        'start' { $script:serviceState = '4 RUNNING'; 'SUCCESS' }
        default { throw 'Unexpected SCM call in loader test.' }
    }
}
function Run-Case([string]$State,[string]$Path,[bool]$ShouldPass,[string]$ExpectedError='') {
    $script:commands = New-Object 'System.Collections.Generic.List[string]'
    $script:serviceState=$State; $script:servicePath=$Path
    try {
        $result = (& $loader -CandidateDirectory $local -ControlSourceDirectory $source | Out-String) | ConvertFrom-Json
        if (-not $ShouldPass -or $result.result -ne 'PASS') { throw 'Unexpected success/result.' }
    } catch {
        if ($ShouldPass -or $_.Exception.Message -notlike "*$ExpectedError*") { throw }
    }
}
$driver = Join-Path $local 'KswordARK.sys'
Run-Case '1 STOPPED' 'C:\old\KswordARK.sys' $true
if (@($commands | Where-Object { $_ -like 'config *' }).Count -ne 1 -or
    @($commands | Where-Object { $_ -like 'start *' }).Count -ne 1) { throw 'Stopped service was not configured/started exactly once.' }
Run-Case '4 RUNNING' ('\??\' + $driver) $true
if (@($commands | Where-Object { $_ -match '^(config|start|stop|delete) ' }).Count) { throw 'Running candidate was mutated.' }
Run-Case '4 RUNNING' ('\??\' + $driver) $true
Run-Case '4 RUNNING' ($driver + '.other') $false 'active at another path'
Run-Case '2 START_PENDING' $driver $false 'transitioning'
'corrupt tool' | Set-Content (Join-Path $source 'hvm_ctl.exe')
Run-Case '4 RUNNING' $driver $false 'does not match its identity'
if ($commands.Count) { throw 'SCM was accessed after an invalid tool update.' }
Copy-Item (Join-Path $repo 'tools/hvm_ctl/test_query_json.exe') (Join-Path $source 'hvm_ctl.exe') -Force
'different driver' | Set-Content (Join-Path $local 'KswordARK.sys')
Run-Case '4 RUNNING' $driver $false 'same loaded driver and PDB'
if ($commands.Count) { throw 'SCM was accessed after a driver identity mismatch.' }
'GUEST_LOADER_TESTS=PASS (7 cases; simulated SCM only)'
