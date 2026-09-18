[CmdletBinding()]
param(
    [string]$TectonicPath,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$sourceDir = $PSScriptRoot
$repoDir = [IO.Path]::GetFullPath((Join-Path $sourceDir '../../..'))
$outputDir = Join-Path $repoDir 'output/pdf'
if (-not $TectonicPath) {
    $TectonicPath = Join-Path $repoDir '.deps/tectonic/tectonic.exe'
}
if (-not (Test-Path -LiteralPath $TectonicPath -PathType Leaf)) {
    throw 'Tectonic was not found. Specify -TectonicPath with its installed executable.'
}
$TectonicPath = (Resolve-Path -LiteralPath $TectonicPath).Path
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
& $Python (Join-Path $sourceDir 'generate_tables.py')
if ($LASTEXITCODE -ne 0) { throw 'Evidence table generation failed.' }
& $TectonicPath -X compile (Join-Path $sourceDir 'main.tex') --outdir $outputDir --keep-logs
if ($LASTEXITCODE -ne 0) { throw 'LaTeX compilation failed.' }
$generated = Join-Path $outputDir 'main.pdf'
if (-not (Test-Path -LiteralPath $generated) -or (Get-Item -LiteralPath $generated).Length -eq 0) {
    throw 'LaTeX produced no nonempty PDF.'
}
Move-Item -LiteralPath $generated -Destination (Join-Path $outputDir 'ksword-live-interposition-v1.pdf') -Force
& $Python (Join-Path $sourceDir 'package_source.py') --output-dir $outputDir
if ($LASTEXITCODE -ne 0) { throw 'Source-package validation failed.' }
& $Python (Join-Path $sourceDir 'validate_build.py') --output-dir $outputDir
if ($LASTEXITCODE -ne 0) { throw 'Build-validation record generation failed.' }
Write-Output ('PREPRINT_OUTPUT=' + $outputDir)
