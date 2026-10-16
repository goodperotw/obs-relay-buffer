# Local Windows configure + build for MVP-1 (does not require CI).
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $ProjectRoot

Write-Host "Configuring (first run downloads OBS/Qt deps and builds libobs)..."
cmake --preset windows-x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building $Configuration..."
cmake --build --preset windows-x64 --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build finished. Output: build_x64\rundir\$Configuration\obs-relay-buffer.dll"
