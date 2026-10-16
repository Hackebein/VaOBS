param(
    [string]$ZigPath,
    [string]$ObsSourcePath,
    [string]$ObsBinPath,
    [string]$Version = 'dev'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot '.build'
$DistRoot = Join-Path $ProjectRoot 'dist'
New-Item -ItemType Directory -Force -Path $BuildRoot, $DistRoot | Out-Null
if ($Version -notmatch '^(dev|v?\d+\.\d+(\.\d+)?(-[0-9A-Za-z.-]+)?)$') { throw 'Use dev or a release version such as 2.0.0' }
$Version = $Version -replace '^v', ''

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

# Identify local builds by their actual source, including uncommitted changes.
$SourceDigest = [System.IO.MemoryStream]::new()
try {
    $VersionBytes = [System.Text.Encoding]::UTF8.GetBytes($Version)
    $SourceDigest.Write($VersionBytes, 0, $VersionBytes.Length)
    foreach ($Tree in @('src', 'tests', 'scripts', '.github')) {
        foreach ($File in (Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $Tree) -Recurse -File | Sort-Object FullName)) {
            $Name = [System.Text.Encoding]::UTF8.GetBytes($File.FullName.Substring($ProjectRoot.Length).Replace('\', '/'))
            $Bytes = [System.IO.File]::ReadAllBytes($File.FullName)
            $SourceDigest.Write($Name, 0, $Name.Length); $SourceDigest.WriteByte(0)
            $SourceDigest.Write($Bytes, 0, $Bytes.Length); $SourceDigest.WriteByte(0)
        }
    }
    $BuildId = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($SourceDigest.ToArray())).Substring(0, 12).ToLowerInvariant()
} finally { $SourceDigest.Dispose() }
@"
#pragma once
#define VAOBS_VERSION "$Version"
#define VAOBS_BUILD_ID "$BuildId"
"@ | Set-Content -LiteralPath (Join-Path $IncludeRoot 'build-version.hpp') -Encoding ascii

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
    & $ZigPath @Common @SDK -shared 'src/plugin.cpp' '-Wl,--export-all-symbols' '-lole32' -o (Join-Path $DistRoot 'vaobs-audio-align.dll')
    Assert-Exit 'OBS plugin build'
    & (Join-Path $PSScriptRoot 'build-dock.ps1') -ZigPath $ZigPath -ObsBinPath $ObsBinPath
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
    Copy-Item -LiteralPath (Join-Path $DistRoot 'vaobs-dock.dll') -Destination $PluginBin
    @{version=$Version; build=$BuildId; architecture='windows-x64'; obs='32.2.2'; files=@{
        'vaobs-audio-align.dll'=(Get-FileHash -LiteralPath (Join-Path $PluginBin 'vaobs-audio-align.dll')).Hash.ToLowerInvariant()
        'vaobs-dock.dll'=(Get-FileHash -LiteralPath (Join-Path $PluginBin 'vaobs-dock.dll')).Hash.ToLowerInvariant()
    }} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PackageRoot 'build-info.json') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'README.md'), (Join-Path $ProjectRoot 'LICENSE') -Destination $PackageRoot
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
    $PackageName = if ($Version -eq 'dev') { "vaobs-dev-$BuildId-windows-x64" } else { "vaobs-$Version-windows-x64" }
    $ZipPath = Join-Path $DistRoot ($PackageName + '.zip')
    Compress-Archive -Path (Join-Path $PackageRoot '*') -DestinationPath $ZipPath -Force
    ((Get-FileHash -LiteralPath $ZipPath).Hash.ToLowerInvariant() + ' *' + (Split-Path -Leaf $ZipPath)) |
        Set-Content -LiteralPath (Join-Path $DistRoot ($PackageName + '.sha256')) -Encoding ascii
    Write-Output "Built $ZipPath"
} finally {
    Pop-Location
}
