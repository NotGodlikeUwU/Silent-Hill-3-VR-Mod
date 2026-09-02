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
$weaponConfigPath = Join-Path $repositoryRoot 'sh3vr_weapons.ini.example'
$assetSourcePath = Join-Path $repositoryRoot 'sh3vr\sh3vr_assets'

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
if (-not (Test-Path -LiteralPath $configPath)) {
    throw "VR configuration template not found: $configPath"
}
if (-not (Test-Path -LiteralPath $weaponConfigPath)) {
    throw "Weapon configuration template not found: $weaponConfigPath"
}
if (-not (Test-Path -LiteralPath $assetSourcePath)) {
    throw "Runtime asset directory not found: $assetSourcePath"
}

$stage = Join-Path ([System.IO.Path]::GetFullPath($OutputDirectory)) 'SH3VR'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $proxyPath -Destination (Join-Path $stage 'dinput8.dll') -Force
Copy-Item -LiteralPath $hostPath -Destination (Join-Path $stage 'sh3vr_host64.exe') -Force
Copy-Item -LiteralPath $configPath -Destination (Join-Path $stage 'sh3vr.ini') -Force
Copy-Item -LiteralPath $weaponConfigPath -Destination (Join-Path $stage 'sh3vr_weapons.ini') -Force
$assetDestinationPath = Join-Path $stage 'sh3vr_assets'
New-Item -ItemType Directory -Force -Path $assetDestinationPath | Out-Null
Get-ChildItem -LiteralPath $assetSourcePath -File | Copy-Item -Destination $assetDestinationPath -Force

$hashLines = @()
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'dinput8.dll')).Hash + '  dinput8.dll')
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'sh3vr_host64.exe')).Hash + '  sh3vr_host64.exe')
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'sh3vr.ini')).Hash + '  sh3vr.ini')
$hashLines += ((Get-FileHash -Algorithm SHA256 (
    Join-Path $stage 'sh3vr_weapons.ini')).Hash + '  sh3vr_weapons.ini')
Get-ChildItem -LiteralPath $assetDestinationPath -File | Sort-Object Name | ForEach-Object {
    $hashLines += ((Get-FileHash -Algorithm SHA256 $_.FullName).Hash +
        ('  sh3vr_assets/{0}' -f $_.Name))
}
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value (
    $hashLines -join [Environment]::NewLine) -Encoding ASCII
Write-Output "Package staged at $stage"
