param(
    [string]$PluginRoot = (Join-Path $env:ProgramData 'obs-studio\plugins')
)

$ErrorActionPreference = 'Stop'
if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) {
    throw 'Close OBS Studio before installing or updating the alignment plugin.'
}
$Source = Join-Path $PSScriptRoot 'obs-plugin\vaobs-audio-align\bin\64bit\vaobs-audio-align.dll'
if (-not (Test-Path -LiteralPath $Source)) { throw 'Extract the complete VaOBS release ZIP before running this installer.' }
$Destination = Join-Path $PluginRoot 'vaobs-audio-align\bin\64bit'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$DllPath = Join-Path $Destination 'vaobs-audio-align.dll'
if (Test-Path -LiteralPath $DllPath) {
    Copy-Item -LiteralPath $DllPath -Destination ($DllPath + '.' + (Get-Date -Format 'yyyyMMddHHmmss') + '.backup')
}
Copy-Item -LiteralPath $Source -Destination $DllPath -Force
New-Item -ItemType Directory -Force -Path (Join-Path $PluginRoot 'vaobs-audio-align\data') | Out-Null
Write-Output "Installed $DllPath"
Write-Output 'Start OBS, add a VaOBS source, and select the media source and timing reference in Properties.'
