param([string]$ZigPath, [string]$ObsBinPath)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root '.build'

function Fetch-Zip([string]$Url, [string]$Hash, [string]$File, [string]$Destination) {
    $Archive = Join-Path $Build $File
    if (-not (Test-Path -LiteralPath $Archive)) { Invoke-WebRequest -Uri $Url -OutFile $Archive }
    if ((Get-FileHash -LiteralPath $Archive).Hash -ne $Hash) { throw "Checksum mismatch: $File" }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination, $true)
}

$Qt = Join-Path $Build 'qt6'
if (-not (Test-Path -LiteralPath (Join-Path $Qt 'lib\Qt6Widgets.lib'))) {
    Fetch-Zip 'https://github.com/obsproject/obs-deps/releases/download/2026-07-15/windows-deps-qt6-2026-07-15-x64.zip' `
        '7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579' 'qt6.zip' $Qt
}
$Msvc = Join-Path $Build 'msvc\Contents\VC\Tools\MSVC\14.44.35207'
if (-not (Test-Path -LiteralPath (Join-Path $Msvc 'include\vcruntime.h'))) {
    Fetch-Zip 'https://download.visualstudio.microsoft.com/download/pr/c610cd8c-801b-44b8-a80a-82cc382aeb43/852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix' `
        '852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5' 'Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix' (Join-Path $Build 'msvc')
}
if (-not (Test-Path -LiteralPath (Join-Path $Msvc 'lib\onecore\x64\msvcprt.lib'))) {
    Fetch-Zip 'https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/50d68caa9bba363d6209bf5fc8a572f013c9f370f6e203f36f04e3d84f5083ca/Microsoft.VC.14.44.17.14.CRT.x64.OneCore.Desktop.base.vsix' `
        '50d68caa9bba363d6209bf5fc8a572f013c9f370f6e203f36f04e3d84f5083ca' 'Microsoft.VC.14.44.17.14.CRT.x64.OneCore.Desktop.base.vsix' (Join-Path $Build 'msvc')
}
$SdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$Sdk = Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'Include') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $Sdk) { throw 'Install the Windows 10/11 SDK to build the status dock.' }
$Libc = Join-Path $Build 'msvc-libc.txt'
@"
include_dir=$SdkRoot\Include\$Sdk\ucrt
sys_include_dir=$Msvc\include
cc_dir=
crt_dir=$SdkRoot\Lib\$Sdk\ucrt\x64
msvc_lib_dir=$Msvc\lib\onecore\x64
kernel32_lib_dir=$SdkRoot\Lib\$Sdk\um\x64
darwin_sdk_dir=
"@ | Set-Content -LiteralPath $Libc -Encoding ascii
$PreviousLibc = $env:ZIG_LIBC
$PreviousPath = $env:PATH
$PreviousPlatform = $env:QT_QPA_PLATFORM
$PreviousPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:ZIG_LIBC = $Libc
    $Flags = @('c++', '-target', 'x86_64-windows-msvc', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-command-line-argument', '-nostdinc++', '-nostdlib++', '-nodefaultlibs', '-fms-compatibility-version=19.44',
        '-fms-runtime-lib=dll', '-D_DLL', '-D_MT', '-DNDEBUG', '-DQT_NO_KEYWORDS', '-I', (Join-Path $Root 'src'),
        '-L', "$SdkRoot\Lib\$Sdk\ucrt\x64")
    foreach ($Include in @("$Msvc\include", "$SdkRoot\Include\$Sdk\um", "$SdkRoot\Include\$Sdk\shared",
            "$Qt\include", "$Qt\include\QtCore", "$Qt\include\QtGui", "$Qt\include\QtWidgets")) {
        $Flags += @('-isystem', $Include)
    }
    $Libraries = @('-lc', '-lmsvcprt', '-lkernel32',
        "$Qt\lib\Qt6Widgets.lib", "$Qt\lib\Qt6Gui.lib", "$Qt\lib\Qt6Core.lib")
    & $ZigPath @Flags -c (Join-Path $Root 'src\dock.cpp') -o (Join-Path $Build 'dock.obj')
    if ($LASTEXITCODE -ne 0) { throw 'Status dock compilation failed' }
    $Link = @('-target', 'x86_64-windows-msvc', '-dynamic', '--libc', $Libc)
    & $ZigPath build-lib (Join-Path $Build 'dock.obj') @Link @Libraries "-femit-bin=$Root\dist\vaobs-dock.dll"
    if ($LASTEXITCODE -ne 0) { throw 'Status dock build failed' }
    & $ZigPath @Flags -c (Join-Path $Root 'tests\dock_test.cpp') -o (Join-Path $Build 'dock-test.obj')
    if ($LASTEXITCODE -ne 0) { throw 'Status dock test compilation failed' }
    & $ZigPath build-exe (Join-Path $Build 'dock-test.obj') @Link @Libraries "-femit-bin=$Build\dock-test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Status dock test build failed' }
    # This isolated Qt test creates no visible window and does not touch OBS.
    $env:PATH = "$Qt\bin;$ObsBinPath;$PreviousPath"
    $env:QT_QPA_PLATFORM = 'minimal'
    $env:QT_PLUGIN_PATH = Join-Path $Qt 'plugins'
    & (Join-Path $Build 'dock-test.exe') (Join-Path $Root 'dist\vaobs-dock.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Status dock tests failed' }
} finally {
    $env:ZIG_LIBC = $PreviousLibc; $env:PATH = $PreviousPath
    $env:QT_QPA_PLATFORM = $PreviousPlatform; $env:QT_PLUGIN_PATH = $PreviousPluginPath
}
