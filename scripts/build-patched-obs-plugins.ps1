<#
.SYNOPSIS
    Locally builds patched OBS encoder plugin DLLs (obs-nvenc.dll,
    obs-qsv11.dll) for native-stream-engine.

.DESCRIPTION
    This script is NOT CI - it's a fully manual, local build script you
    run whenever you need it. What it does:
      1) Shallow-clones the pinned OBS Studio tag (default: 32.1.2) into
         a temporary folder.
      2) Applies the .patch files under patches/obs/ to that checkout.
      3) Builds only the targets that are actually needed (libobs +
         obs-nvenc, obs-qsv11) - tries to disable heavy dependencies
         native-stream-engine never uses, such as obs-browser/CEF,
         Qt/frontend, decklink, and vst.
      4) Copies the built DLLs into .\dist\obs-plugin-patches\.

    Everything after this is manual: either copy these DLLs directly into
    your local runtime folder (OBS-Studio-32.1.2-Windows-x64\obs-plugins\64bit\)
    and test, or upload them yourself to GitHub Releases as a
    "runtime-patch-vX.zip".

.NOTES
    - Windows + Visual Studio (MSVC) build tools, CMake, and Git must be
      on PATH. OBS's own build system (deps/CEF/Qt fetch mechanism) can
      change between versions; the CMake steps below are a starting
      point - on the first run you may need to check obs-studio's own
      CMakePresets.json / CI scripts (.github/scripts) to confirm the
      target names and required flags for your environment. There's no
      NVIDIA/AMD/Intel hardware available here to test this - the first
      run will be on your own machine.

.PARAMETER ObsTag
    The OBS Studio git tag to pin against. Default: 32.1.2 (matches
    native-stream-engine's current runtime version).

.PARAMETER Targets
    The CMake targets to build. Default: obs-nvenc and obs-qsv11
    (+libobs). No patch exists for AMF (obs-ffmpeg), so the stock
    runtime's obs-ffmpeg.dll is already sufficient - no need to build it
    here.
#>

param(
    [string]$ObsTag = "32.1.2",
    [string[]]$Targets = @("obs-nvenc", "obs-qsv11"),
    [string]$WorkDir = "$PSScriptRoot\..\.obs-build-tmp",
    [string]$OutDir = "$PSScriptRoot\..\dist\obs-plugin-patches"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path "$PSScriptRoot\.."
$patchDir = Join-Path $repoRoot "patches\obs"

Write-Host "== native-stream-engine: patched OBS plugin build ==" -ForegroundColor Cyan
Write-Host "OBS tag   : $ObsTag"
Write-Host "Targets   : $($Targets -join ', ')"
Write-Host "Work dir  : $WorkDir"
Write-Host "Out dir   : $OutDir"

if (Test-Path $WorkDir) {
    Write-Host "Cleaning: $WorkDir"
    Remove-Item -Recurse -Force $WorkDir
}
New-Item -ItemType Directory -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# 1) Shallow-clone the pinned tag.
Write-Host "`n[1/4] Cloning obs-studio $ObsTag..." -ForegroundColor Yellow
git clone --branch $ObsTag --depth 1 --recurse-submodules --shallow-submodules `
    https://github.com/obsproject/obs-studio.git "$WorkDir\obs-studio"

Push-Location "$WorkDir\obs-studio"
try {
    # 2) Apply the patches.
    Write-Host "`n[2/4] Applying patches..." -ForegroundColor Yellow
    Get-ChildItem -Path $patchDir -Filter "*.patch" | ForEach-Object {
        Write-Host "  -> $($_.Name)"
        git apply --whitespace=nowarn $_.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply patch: $($_.Name) - if the OBS tag changed, the patch may need to be rebased."
        }
    }

    # 3) Configure and build only the targets we need.
    #    NOTE: The ENABLE_* flag names below may change across OBS
    #    versions; on the first run, verify the available options with
    #    `cmake -S . -B build -LH`.
    Write-Host "`n[3/4] CMake configure + build..." -ForegroundColor Yellow

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
        -DENABLE_BROWSER=OFF `
        -DENABLE_VLC=OFF `
        -DENABLE_UI=OFF `
        -DENABLE_SCRIPTING=OFF `
        -DCMAKE_BUILD_TYPE=Release

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    foreach ($target in $Targets) {
        Write-Host "  building: $target"
        cmake --build build --config Release --target $target
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
    }

    # 4) Collect the built DLLs.
    Write-Host "`n[4/4] Copying DLLs..." -ForegroundColor Yellow
    $found = Get-ChildItem -Path "build" -Recurse -Include "*.dll" |
        Where-Object { $Targets -contains $_.BaseName -or $_.BaseName -like "obs-nvenc*" -or $_.BaseName -like "obs-qsv11*" }

    if (-not $found) {
        throw "Expected DLLs not found - inspect the build folder manually and add the output path to the script."
    }

    foreach ($dll in $found) {
        Copy-Item $dll.FullName -Destination $OutDir -Force
        Write-Host "  -> $OutDir\$($dll.Name)"
    }
}
finally {
    Pop-Location
}

Write-Host "`nDone. Now, with the DLLs in $OutDir :" -ForegroundColor Green
Write-Host "  1) For local testing: copy them into runtime\OBS-Studio-$ObsTag-Windows-x64\obs-plugins\64bit\"
Write-Host "     and test with native-stream-engine.exe --service."
Write-Host "  2) To ship them: upload them yourself to GitHub Releases as 'runtime-patch-v$ObsTag-rN.zip'."