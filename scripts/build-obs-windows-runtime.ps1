<#
.SYNOPSIS
    Builds the pinned native-stream-engine Windows OBS runtime from source.

.DESCRIPTION
    Creates a reproducible Windows OBS Studio runtime for native-stream-engine.

    The script:
      1) Shallow-clones the pinned OBS Studio tag (default: 32.1.2).
      2) Applies common-* and windows-* native-stream-engine OBS patches.
      3) Restricts the OBS plugin graph to only the modules required by
         native-stream-engine.
      4) Restricts Windows OBS dependencies to the prebuilt dependency bundle,
         avoiding Qt and CEF because the OBS frontend/browser are not built.
      5) Builds:
           - libobs
           - libobs-d3d11
           - win-capture
           - win-wasapi
           - obs-ffmpeg / AMD AMF
           - obs-ffmpeg-mux
           - obs-x264
           - obs-qsv11
           - obs-qsv-test
           - obs-nvenc
           - obs-nvenc-test
           - nse-runtime-stage
      6) Uses OBS's own dependency bundling and rundir staging logic.
      7) Publishes the complete runtime to:
           runtime\OBS-Studio-<version>-Windows-x64
      8) Publishes the official obs.lib and generated obsconfig.h from the
         same OBS build to:
           compatibility\obs-<version>

    No manual DLL replacement or .def-based import-library generation is
    required by this build path.

.NOTES
    Requirements:
      - Windows x64
      - Visual Studio 2022 C++ build tools
      - CMake
      - Git

    OBS Studio is pinned to a specific tag and the script fails if expected
    patch points or dependency-layout assumptions no longer match.

.PARAMETER ObsTag
    OBS Studio git tag to build. Default: 32.1.2.

.PARAMETER Targets
    OBS CMake targets to build. Defaults to the complete minimal target set
    required by native-stream-engine.

.PARAMETER WorkDir
    Temporary OBS source/build directory.

.PARAMETER OutDir
    Destination Windows OBS runtime directory.
#>

param(
    [string]$ObsTag = "32.1.2",
    [string[]]$Targets = @(
        "libobs",
        "libobs-d3d11",
        "win-capture",
        "win-wasapi",
        "obs-ffmpeg",
        "obs-ffmpeg-mux",
        "obs-x264",
        "obs-qsv11",
        "obs-qsv-test",
        "obs-nvenc",
        "obs-nvenc-test",
        "nse-runtime-stage"
    ),
    [string]$WorkDir = "$PSScriptRoot\..\.obs-build-tmp",
    [string]$OutDir = "$PSScriptRoot\..\runtime\OBS-Studio-$ObsTag-Windows-x64"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path "$PSScriptRoot\.."
$patchDir = Join-Path $repoRoot "patches\obs"

Write-Host "== native-stream-engine: Windows OBS runtime build ==" -ForegroundColor Cyan
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

$ExpectedObsCommit = "fb4d98bf88fae5fc85cb11fc57f7c5e309282194"

$ActualObsCommit = (
    git -C "$WorkDir\obs-studio" rev-parse HEAD
).Trim()

if ($ActualObsCommit -ne $ExpectedObsCommit) {
    throw @"
Unexpected OBS commit.

Expected:
  $ExpectedObsCommit

Actual:
  $ActualObsCommit
"@
}

Push-Location "$WorkDir\obs-studio"
try {
    # 2) Apply the patches.
    Write-Host "`n[2/4] Applying patches..." -ForegroundColor Yellow
    $patches = @(
        Get-ChildItem -Path $patchDir -Filter "common-*.patch"
        Get-ChildItem -Path $patchDir -Filter "windows-*.patch"
    ) | Sort-Object Name

    if (-not $patches) {
        throw "No common-*.patch or windows-*.patch files found under $patchDir"
    }

    foreach ($patch in $patches) {
        Write-Host "  -> $($patch.Name)"

        git apply --check --whitespace=nowarn $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Patch check failed: $($patch.Name)"
        }

        git apply --whitespace=nowarn $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply patch: $($patch.Name)"
        }
    }

    Write-Host "`nRestricting OBS plugin graph to native-stream-engine requirements..." -ForegroundColor Yellow

    $minimalPluginGraph = @(
        'cmake_minimum_required(VERSION 3.28...3.30)'
        ''
        'option(ENABLE_PLUGINS "Enable building OBS plugins" ON)'
        ''
        'if(NOT ENABLE_PLUGINS)'
        '  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")'
        '  return()'
        'endif()'
        ''
        'set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")'
        ''
        'add_obs_plugin(win-capture PLATFORMS WINDOWS)'
        'add_obs_plugin(win-wasapi PLATFORMS WINDOWS)'
        ''
        'add_obs_plugin(obs-ffmpeg)'
        ''
        'add_obs_plugin('
        '  obs-nvenc'
        '  PLATFORMS WINDOWS LINUX'
        '  ARCHITECTURES x64 x86_64'
        ')'
        ''
        'add_obs_plugin('
        '  obs-qsv11'
        '  PLATFORMS WINDOWS LINUX'
        '  ARCHITECTURES x64 x86_64'
        ')'
        ''
        'add_obs_plugin(obs-x264)'
        ''
        '# native-stream-engine dependency staging anchor'
        'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nse-runtime-stage.c" "int main(void) { return 0; }\n")'
        'add_executable(nse-runtime-stage EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/nse-runtime-stage.c")'
        '_bundle_dependencies(nse-runtime-stage)'
    )

    $minimalPluginGraph |
        Set-Content -Path "plugins\CMakeLists.txt" -Encoding utf8

    Write-Host "`nRestricting Windows OBS dependencies to prebuilt-only..." -ForegroundColor Yellow

    $windowsBuildspecPath = "cmake\windows\buildspec.cmake"
    $windowsBuildspec = Get-Content -Raw $windowsBuildspecPath

    $expectedDependencyBlock = @'
  if(CMAKE_VS_PLATFORM_NAME STREQUAL Win32)
    set(arch x86)
    set(dependencies_list prebuilt)
  else()
    string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" arch)
    set(dependencies_list prebuilt qt6 cef)
  endif()
  set(platform windows-${arch})

  _check_dependencies(${dependencies_list})

  if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL Win32)
    _handle_qt_cross_compile(${CMAKE_HOST_SYSTEM_PROCESSOR} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.deps/${qt6_destination}")
  endif()
'@

    $minimalDependencyBlock = @'
  if(CMAKE_VS_PLATFORM_NAME STREQUAL Win32)
    set(arch x86)
  else()
    string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" arch)
  endif()

  set(dependencies_list prebuilt)
  set(platform windows-${arch})

  _check_dependencies(${dependencies_list})
'@

    $normalizedWindowsBuildspec =
        $windowsBuildspec -replace "`r`n", "`n"

    $normalizedExpectedDependencyBlock =
        $expectedDependencyBlock -replace "`r`n", "`n"

    $normalizedMinimalDependencyBlock =
        $minimalDependencyBlock -replace "`r`n", "`n"

    if (-not $normalizedWindowsBuildspec.Contains(
        $normalizedExpectedDependencyBlock
    )) {
        throw @"
Unexpected OBS Windows dependency block.

Expected OBS Studio $ObsTag buildspec layout was not found in:
  $windowsBuildspecPath

Refusing to continue so Qt/CEF dependency behavior does not silently change.
"@
    }

    $normalizedWindowsBuildspec =
        $normalizedWindowsBuildspec.Replace(
            $normalizedExpectedDependencyBlock,
            $normalizedMinimalDependencyBlock
        )

    $windowsBuildspec = $normalizedWindowsBuildspec

    Set-Content `
        -Path $windowsBuildspecPath `
        -Value $windowsBuildspec `
        -Encoding utf8

    # 3) Configure and build only the targets we need.
    #    NOTE: The ENABLE_* flag names below may change across OBS
    #    versions; on the first run, verify the available options with
    #    `cmake -S . -B build -LH`.
    Write-Host "`n[3/4] CMake configure + build..." -ForegroundColor Yellow

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
        -DENABLE_BROWSER=OFF `
        -DENABLE_VLC=OFF `
        -DENABLE_UI=OFF `
        -DENABLE_FRONTEND=OFF `
        -DENABLE_SCRIPTING=OFF `
        -DCMAKE_BUILD_TYPE=Release

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    foreach ($target in $Targets) {
        Write-Host "  building: $target"
        cmake --build build --config Release --target $target
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
    }

    # 4) Publish OBS's staged runtime tree.
    Write-Host "`n[4/4] Publishing Windows OBS runtime..." -ForegroundColor Yellow

    $stagedRuntime = Join-Path (Get-Location) "build\rundir\Release"

    if (-not (Test-Path $stagedRuntime -PathType Container)) {
        throw "OBS staged runtime was not found: $stagedRuntime"
    }

    $requiredRuntimeFiles = @(
        "bin\64bit\obs.dll",
        "bin\64bit\libobs-d3d11.dll",
        "bin\64bit\avdevice-61.dll",
        "bin\64bit\librist.dll",
        "bin\64bit\libx264-164.dll",
        "bin\64bit\srt.dll",
        "data\obs-plugins\win-capture\graphics-hook64.dll",
        "data\obs-plugins\win-capture\graphics-hook32.dll",
        "data\obs-plugins\win-capture\get-graphics-offsets64.exe",
        "data\obs-plugins\win-capture\get-graphics-offsets32.exe",
        "data\obs-plugins\win-capture\inject-helper64.exe",
        "data\obs-plugins\win-capture\inject-helper32.exe",
        "data\obs-plugins\win-capture\compatibility.json",
        "data\obs-plugins\win-capture\package.json",
        "data\obs-plugins\win-wasapi\locale\en-US.ini",
        "data\obs-plugins\obs-ffmpeg\locale\en-US.ini",
        "data\obs-plugins\obs-x264\locale\en-US.ini",
        "data\obs-plugins\obs-qsv11\locale\en-US.ini",
        "data\obs-plugins\obs-nvenc\locale\en-US.ini",
        "obs-plugins\64bit\win-capture.dll",
        "obs-plugins\64bit\win-wasapi.dll",
        "obs-plugins\64bit\obs-ffmpeg.dll",
        "obs-plugins\64bit\obs-x264.dll",
        "obs-plugins\64bit\obs-qsv11.dll",
        "obs-plugins\64bit\obs-nvenc.dll"
    )

    foreach ($relativePath in $requiredRuntimeFiles) {
        $fullPath = Join-Path $stagedRuntime $relativePath

        if (-not (Test-Path $fullPath -PathType Leaf)) {
            throw "Required OBS runtime file was not produced: $fullPath"
        }
    }

    Write-Host "Publishing OBS development compatibility files..." -ForegroundColor Yellow

    $compatibilityDir = Join-Path $repoRoot "compatibility\obs-$ObsTag"
    $generatedObsConfig = Join-Path (Get-Location) "build\config\obsconfig.h"

    if (-not (Test-Path $generatedObsConfig -PathType Leaf)) {
        throw "Generated OBS configuration header was not found: $generatedObsConfig"
    }

    $obsImportLibraries = @(
        Get-ChildItem `
            -Path "build" `
            -Recurse `
            -File `
            -Filter "obs.lib" |
        Where-Object {
            $_.FullName -match '[\\/]libobs[\\/]'
        }
    )

    if ($obsImportLibraries.Count -ne 1) {
        $foundPaths = if ($obsImportLibraries.Count -gt 0) {
            ($obsImportLibraries.FullName -join "`n  ")
        }
        else {
            "<none>"
        }

        throw @"
Expected exactly one official libobs import library.

Found:
  $foundPaths
"@
    }

    $officialObsImportLibrary = $obsImportLibraries[0].FullName

    New-Item `
        -ItemType Directory `
        -Path $compatibilityDir `
        -Force |
        Out-Null

    Copy-Item `
        -Path $officialObsImportLibrary `
        -Destination (Join-Path $compatibilityDir "obs.lib") `
        -Force

    Copy-Item `
        -Path $generatedObsConfig `
        -Destination (Join-Path $compatibilityDir "obsconfig.h") `
        -Force

    Write-Host "  obs.lib     <- $officialObsImportLibrary"
    Write-Host "  obsconfig.h <- $generatedObsConfig"

    if (Test-Path $OutDir) {
        Remove-Item -Recurse -Force $OutDir
    }

    New-Item -ItemType Directory -Path $OutDir | Out-Null

    Copy-Item `
        -Path (Join-Path $stagedRuntime "*") `
        -Destination $OutDir `
        -Recurse `
        -Force
    
    $libobsSourceDir = Join-Path (Get-Location) "libobs"
    $runtimeIncludeDir = Join-Path $OutDir "include"

    New-Item `
        -ItemType Directory `
        -Path $runtimeIncludeDir `
        -Force |
        Out-Null

    $libobsHeaders = Get-ChildItem `
        -Path $libobsSourceDir `
        -Recurse `
        -File |
        Where-Object {
            $_.Extension -in @(".h", ".hpp")
        }

    foreach ($header in $libobsHeaders) {
        $relativePath = $header.FullName.Substring(
            $libobsSourceDir.Length
        ).TrimStart('\', '/')

        $destination = Join-Path `
            $runtimeIncludeDir `
            $relativePath

        $destinationDir = Split-Path `
            -Parent `
            $destination

        New-Item `
            -ItemType Directory `
            -Path $destinationDir `
            -Force |
            Out-Null

        Copy-Item `
            -Path $header.FullName `
            -Destination $destination `
            -Force
    }

    Copy-Item `
        -Path $generatedObsConfig `
        -Destination (Join-Path $runtimeIncludeDir "obsconfig.h") `
        -Force

    Write-Host "Windows OBS runtime published:" -ForegroundColor Green
    Write-Host "  $OutDir"
}
finally {
    Pop-Location
}

Write-Host "`nDone." -ForegroundColor Green
Write-Host "Windows OBS runtime:"
Write-Host "  $OutDir"
Write-Host ""
Write-Host "OBS compatibility files:"
Write-Host "  $repoRoot\compatibility\obs-$ObsTag\obs.lib"
Write-Host "  $repoRoot\compatibility\obs-$ObsTag\obsconfig.h"
Write-Host ""
Write-Host "native-stream-engine can now be configured and built against this runtime."