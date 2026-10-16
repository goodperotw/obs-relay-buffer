# Copies a built plugin into the per-machine OBS plugin directory (no admin required on ProgramData).
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$Dll = Join-Path $ProjectRoot "build_x64\rundir\$Configuration\obs-relay-buffer.dll"
if (-not (Test-Path $Dll)) {
    throw "Plugin DLL not found: $Dll`nBuild first: cmake --build --preset windows-x64 --config $Configuration"
}

$DestRoot = Join-Path $env:ProgramData 'obs-studio\plugins\obs-relay-buffer'
$DestBin = Join-Path $DestRoot 'bin\64bit'
$DestData = Join-Path $DestRoot 'data'
New-Item -ItemType Directory -Force -Path $DestBin, $DestData | Out-Null

Copy-Item $Dll $DestBin -Force
$Pdb = [System.IO.Path]::ChangeExtension($Dll, '.pdb')
if (Test-Path $Pdb) {
    Copy-Item $Pdb $DestBin -Force
}

$LocaleSrc = Join-Path $ProjectRoot 'data'
if (Test-Path $LocaleSrc) {
    Copy-Item (Join-Path $LocaleSrc '*') $DestData -Recurse -Force
}

Write-Host "Installed to $DestRoot"
Write-Host "Restart OBS Studio, then open Tools -> Open Relay Window"
