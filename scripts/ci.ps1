<#
.SYNOPSIS
    Configure, build and test the platform.

.DESCRIPTION
    Used both locally and in CI. Fails fast on the first failing stage so the
    log ends at the actual problem rather than a wall of downstream noise.

.EXAMPLE
    $env:QT6_DIR = "C:/Qt/6.5.3/msvc2019_64"
    .\scripts\ci.ps1 -Preset msvc2022 -Configuration Debug
#>

[CmdletBinding()]
param(
    [string]$Preset = "msvc2022",
    [string]$Configuration = "Debug",
    [switch]$StaticPlugins,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot

if ($StaticPlugins) {
    $Preset = "msvc2022-static-plugins"
}

if (-not $env:QT6_DIR) {
    Write-Warning "QT6_DIR is not set; CMake will fall back to whatever Qt is on CMAKE_PREFIX_PATH."
}

function Invoke-Stage {
    param([string]$Name, [scriptblock]$Action)

    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repositoryRoot
try {
    Invoke-Stage "Configure ($Preset)" {
        cmake --preset $Preset
    }

    Invoke-Stage "Build ($Configuration)" {
        cmake --build "build/$Preset" --config $Configuration --parallel
    }

    if (-not $SkipTests) {
        Invoke-Stage "Test" {
            ctest --test-dir "build/$Preset" --build-config $Configuration `
                  --output-on-failure --parallel 4
        }
    }

    Write-Host ""
    Write-Host "All stages passed." -ForegroundColor Green
}
finally {
    Pop-Location
}
