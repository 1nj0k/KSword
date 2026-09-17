#requires -Version 5.1
[CmdletBinding()]
param([switch]$Check, [switch]$NoRestart)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'AmdLabBoot.psm1') -Force
Invoke-AmdLabBoot -Mode Lab -Check:$Check -NoRestart:$NoRestart
