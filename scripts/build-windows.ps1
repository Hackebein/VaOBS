param(
    [string]$ZigPath,
    [string]$ObsSourcePath,
    [string]$ObsBinPath
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot '.build'
$DistRoot = Join-Path $ProjectRoot 'dist'
New-Item -ItemType Directory -Force -Path $BuildRoot, $DistRoot | Out-Null

function Assert-Exit([string]$Action) {
    if ($LASTEXITCODE -ne 0) { throw "$Action failed with exit code $LASTEXITCODE" }
}

if (-not $ZigPath) {
    $ToolchainRoot = Join-Path $BuildRoot 'toolchain'
    $ZigPath = Join-Path $ToolchainRoot 'zig-x86_64-windows-0.17.0\zig.exe'
    if (-not (Test-Path -LiteralPath $ZigPath)) {
        $Archive = Join-Path $BuildRoot 'zig-0.17.0.zip'
        Invoke-WebRequest -Uri 'https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip' -OutFile $Archive
        $ExpectedHash = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash -ne $ExpectedHash) {
            throw 'Zig archive checksum mismatch'
        }
        Expand-Archive -LiteralPath $Archive -DestinationPath $ToolchainRoot -Force
    }
}
$ZigPath = (Resolve-Path -LiteralPath $ZigPath).Path

if (-not $ObsSourcePath) {
    $ObsSourcePath = Join-Path $BuildRoot 'obs-sdk'
    if (-not (Test-Path -LiteralPath $ObsSourcePath)) {
        & git clone --depth 1 --branch 32.2.2 --filter=blob:none --sparse 'https://github.com/obsproject/obs-studio.git' $ObsSourcePath
        Assert-Exit 'OBS SDK checkout'
        & git -C $ObsSourcePath sparse-checkout set libobs
        Assert-Exit 'OBS SDK headers'
    }
}
$ObsSourcePath = (Resolve-Path -LiteralPath $ObsSourcePath).Path
$ObsRevision = & git -C $ObsSourcePath rev-parse HEAD
Assert-Exit 'OBS SDK revision check'
if ($ObsRevision -ne 'ba2f32bdf791005443988a4955e963663e16b1ed') { throw 'Expected OBS SDK 32.2.2 at the pinned revision' }

$IncludeRoot = Join-Path $BuildRoot 'include'
New-Item -ItemType Directory -Force -Path $IncludeRoot | Out-Null
@'
#pragma once
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_INSTALL_PREFIX ""
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -LiteralPath (Join-Path $IncludeRoot 'obsconfig.h') -Encoding utf8

Push-Location $ProjectRoot
try {
    $Common = @('c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-target', 'x86_64-windows-gnu', '-I', 'src')
    $SDK = @('-D_M_X64', '-I', (Join-Path $ObsSourcePath 'libobs'), '-I', $IncludeRoot)
    & $ZigPath @Common 'tests/alignment_test.cpp' -o (Join-Path $BuildRoot 'alignment-test.exe')
    Assert-Exit 'Alignment test build'
    & (Join-Path $BuildRoot 'alignment-test.exe')
    Assert-Exit 'Alignment tests'
    & $ZigPath @Common 'tests/logs_test.cpp' -o (Join-Path $BuildRoot 'logs-test.exe')
    Assert-Exit 'Log test build'
    & (Join-Path $BuildRoot 'logs-test.exe')
    Assert-Exit 'Log tests'
    & $ZigPath @Common @SDK -shared 'src/plugin.cpp' '-Wl,--export-all-symbols' -o (Join-Path $DistRoot 'vaobs-audio-align.dll')
    Assert-Exit 'OBS plugin build'
    & $ZigPath @Common @SDK 'tests/obs_integration_test.cpp' -o (Join-Path $BuildRoot 'obs-integration-test.exe')
    Assert-Exit 'OBS integration test build'
    & $ZigPath @Common @SDK 'tests/obs_control_test.cpp' -o (Join-Path $BuildRoot 'obs-control-test.exe')
    Assert-Exit 'OBS playback test build'
    if ($ObsBinPath) {
        & (Join-Path $BuildRoot 'obs-control-test.exe') (Resolve-Path -LiteralPath $ObsBinPath).Path (Join-Path $DistRoot 'vaobs-audio-align.dll')
        Assert-Exit 'Real OBS playback and source management test'
        & (Join-Path $BuildRoot 'obs-integration-test.exe') (Resolve-Path -LiteralPath $ObsBinPath).Path (Join-Path $DistRoot 'vaobs-audio-align.dll')
        Assert-Exit 'Real OBS audio integration test'
    }

    $PackageRoot = Join-Path $BuildRoot ('package-' + [guid]::NewGuid().ToString('N'))
    $PluginBin = Join-Path $PackageRoot 'obs-plugin\vaobs-audio-align\bin\64bit'
    New-Item -ItemType Directory -Force -Path $PluginBin, (Join-Path $PackageRoot 'licenses') | Out-Null
    Copy-Item -LiteralPath (Join-Path $DistRoot 'vaobs-audio-align.dll') -Destination $PluginBin
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'README.md'), (Join-Path $ProjectRoot 'LICENSE') -Destination $PackageRoot
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts\install-plugin.ps1') -Destination $PackageRoot
    $ZigRoot = Split-Path -Parent $ZigPath
    Copy-Item -LiteralPath (Join-Path $ZigRoot 'lib\libcxx\LICENSE.TXT') -Destination (Join-Path $PackageRoot 'licenses\LLVM-runtime.txt')

    # Include corresponding source and build instructions with the plugin.
    $SourceRoot = Join-Path $PackageRoot 'source'
    # An extracted source ZIP must also build without a Git working tree.
    $SourceFiles = @('README.md', 'LICENSE', '.gitignore') | ForEach-Object { Get-Item -LiteralPath (Join-Path $ProjectRoot $_) }
    foreach ($Tree in @('src', 'tests', 'scripts', '.github')) {
        $SourceFiles += Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $Tree) -Recurse -File
    }
    foreach ($File in $SourceFiles) {
        $Relative = $File.FullName.Substring($ProjectRoot.Length + 1)
        $Destination = Join-Path $SourceRoot $Relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $File.FullName -Destination $Destination
    }
    $ZipPath = Join-Path $DistRoot 'vaobs-windows-live-alignment.zip'
    Compress-Archive -Path (Join-Path $PackageRoot '*') -DestinationPath $ZipPath -Force
    Write-Output "Built $ZipPath"
} finally {
    Pop-Location
}
