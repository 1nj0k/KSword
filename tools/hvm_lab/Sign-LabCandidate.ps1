# Sign only a staged lab candidate. Never enable host test mode or install host trust anchors.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$CandidateDirectory)
$ErrorActionPreference='Stop'
$directory=(Resolve-Path -LiteralPath $CandidateDirectory).Path
$driver=Join-Path $directory 'KswordARK.sys'
if (-not (Test-Path -LiteralPath $driver)) { throw 'Missing staged driver.' }
$subject='CN=KSword AMD Lab Test Signing'
$cert=@(Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(7)
} | Sort-Object NotAfter -Descending | Select-Object -First 1)
if ($cert.Count -eq 0) {
    # A code-signing end certificate, not a CA incorrectly missing KeyCertSign.
    $cert=@(New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
        -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddMonths(6))
}
$certificate=$cert[0]
$signTool=Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $signTool) { throw 'Missing x64 Windows SDK signtool.' }
& $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $driver
if ($LASTEXITCODE -ne 0) { throw 'Signing failed.' }
$public=Join-Path $directory 'AMD-Lab-TestSigning.cer'
Export-Certificate -Cert $certificate -FilePath $public -Force | Out-Null
$signature=Get-AuthenticodeSignature -LiteralPath $driver
if ($signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) { throw 'Signer readback mismatch.' }
@{signer=$certificate.Thumbprint;certificate=$public;privateKeyExported=$false;
    hostTrustInstalled=$false;authenticodeStatus=[string]$signature.Status;
    statusMessage=$signature.StatusMessage;guestLoadVerified=$false} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'signing.json') -Encoding UTF8
Get-Content -LiteralPath (Join-Path $directory 'signing.json')
