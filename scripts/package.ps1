param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist')
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$proxyPath = Join-Path $repositoryRoot "sh3vr\Win32\$Configuration\dinput8.dll"
$hostPath = Join-Path $repositoryRoot "out\host\$Configuration\sh3vr_host64.exe"
$solutionHostPath = Join-Path $repositoryRoot "sh3vr_host64\x64\$Configuration\sh3vr_host64.exe"
$legacyHostPath = Join-Path $repositoryRoot "sh3vr_host64\build-vs18\$Configuration\sh3vr_host64.exe"
$configPath = Join-Path $repositoryRoot 'sh3vr.ini.example'

if (-not (Test-Path -LiteralPath $proxyPath)) {
    throw "Proxy binary not found: $proxyPath"
}
if (-not (Test-Path -LiteralPath $hostPath)) {
    $hostPath = $solutionHostPath
}
if (-not (Test-Path -LiteralPath $hostPath)) {
    $hostPath = $legacyHostPath
}
if (-not (Test-Path -LiteralPath $hostPath)) {
    throw "Host binary not found. Build the host before packaging."
}

$stage = Join-Path ([System.IO.Path]::GetFullPath($OutputDirectory)) 'SH3VR'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $proxyPath -Destination (Join-Path $stage 'dinput8.dll') -Force
Copy-Item -LiteralPath $hostPath -Destination (Join-Path $stage 'sh3vr_host64.exe') -Force
Copy-Item -LiteralPath $configPath -Destination (Join-Path $stage 'sh3vr.ini') -Force

$hashLines = @()
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'dinput8.dll')).Hash + '  dinput8.dll')
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'sh3vr_host64.exe')).Hash + '  sh3vr_host64.exe')
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'sh3vr.ini')).Hash + '  sh3vr.ini')
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value (
    $hashLines -join [Environment]::NewLine) -Encoding ASCII
Write-Output "Package staged at $stage"
