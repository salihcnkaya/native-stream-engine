param(
    [string]$CompatibilityDir
)

$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($CompatibilityDir)) {
    $compatibilityDirPath = Join-Path `
        $repositoryRoot `
        "compatibility\obs-32.1.2"
}
elseif ([System.IO.Path]::IsPathRooted($CompatibilityDir)) {
    $compatibilityDirPath = $CompatibilityDir
}
else {
    $compatibilityDirPath = Join-Path `
        $repositoryRoot `
        $CompatibilityDir
}

$compatibilityDirPath = [System.IO.Path]::GetFullPath(
    $compatibilityDirPath
)

$defFile = Join-Path $compatibilityDirPath "obs.def"
$libFile = Join-Path $compatibilityDirPath "obs.lib"

if (-not (Test-Path $defFile -PathType Leaf)) {
    throw @"
OBS module-definition file was not found:

$defFile
"@
}

$libCommand = Get-Command "lib.exe" -ErrorAction SilentlyContinue

if (-not $libCommand) {
    throw @"
Microsoft Library Manager was not found.

Run this script from:

  Developer PowerShell for Visual Studio

Required command:

  lib.exe
"@
}

Write-Host "Generating OBS import library..."
Write-Host ""
Write-Host "Definition file:"
Write-Host "  $defFile"
Write-Host ""
Write-Host "Output file:"
Write-Host "  $libFile"
Write-Host ""

if (Test-Path $libFile -PathType Leaf) {
    Remove-Item $libFile -Force
}

& $libCommand.Source `
    /nologo `
    /def:$defFile `
    /machine:x64 `
    /out:$libFile

if ($LASTEXITCODE -ne 0) {
    throw "lib.exe failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path $libFile -PathType Leaf)) {
    throw "OBS import library was not generated: $libFile"
}

Write-Host ""
Write-Host "OBS import library generated successfully:"
Write-Host "  $libFile"