# Exercise the production Hvm capture function with real child processes, never a driver.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$EvidenceDirectory=Join-Path $PSScriptRoot ('artifacts/capture-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
$Ctl=Join-Path $EvidenceDirectory 'capture-fixture.exe'
Add-Type -OutputAssembly $Ctl -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Threading;
public static class CaptureFixture {
    public static int Main(string[] args) {
        string verb = args[1];
        if (verb == "slow") Thread.Sleep(1500);
        if (verb == "empty") return 0;
        if (verb == "large") {
            Console.Error.Write(new string('e', 131072));
            Console.Write("{\"payload\":\"" + new string('x', 131072) + "\"}");
        } else Console.Write("{\"command\":\"" + verb + "\"}");
        return verb == "fail" ? 7 : 0;
    }
}
'@
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'Invoke-GuestAcceptance.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Production runner parse failed.' }
foreach ($name in @('Record','Hvm')) {
    $function=$ast.Find({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if (-not $function) { throw "Missing production function $name" }
    Invoke-Expression $function.Extent.Text
}
$journal=Join-Path $EvidenceDirectory 'controls.jsonl'
$script:commandId=0; $script:uncertain=$false; $CommandTimeoutSeconds=10
for ($i=0; $i -lt 300; $i++) {
    if ((Hvm status).command -ne 'status') { throw 'Fast exit lost output.' }
}
if ((Hvm large).payload.Length -ne 131072) { throw 'Truncated stdout.' }
if ([IO.File]::ReadAllText((Join-Path $EvidenceDirectory '00301-large.stderr.txt')).Length -ne 131072) { throw 'Truncated stderr.' }
foreach ($case in @(@('fail','failed (7)'),@('empty','returned no JSON'))) {
    $caught=$false
    try { Hvm $case[0] | Out-Null } catch {
        if (-not $_.Exception.Message.Contains($case[1])) { throw }
        $caught=$true
    }
    if (-not $caught) { throw 'Invalid output/exit code was accepted.' }
}
$CommandTimeoutSeconds=0.01
$caught=$false
try { Hvm slow | Out-Null } catch {
    if (-not $_.Exception.Message.Contains('Control timed out.')) { throw }
    $caught=$true
}
if (-not $caught -or -not $script:uncertain) { throw 'Timeout state was lost.' }
$timeout=Get-Content -LiteralPath $journal -Tail 1 | ConvertFrom-Json
$child=Get-Process -Id $timeout.pid -ErrorAction Stop
if ($child.HasExited) { throw 'Timed-out fixture should still be alive.' }
if (-not $child.WaitForExit(5000)) { throw 'Fixture did not finish naturally.' }
$child.Dispose()
'CAPTURE_RESULT=PASS fast=300 largeStreams=2 nonzero=PASS empty=PASS timeoutRetained=PASS hardware=NOT_RUN'
