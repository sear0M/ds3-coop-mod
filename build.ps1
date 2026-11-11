# Compila o mod (build\dinput8.dll) e, com -GameDir, copia para a pasta do jogo.
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -GameDir "C:\Jogos\...\DARK SOULS III"
param([string]$GameDir = '')

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'cmake configure falhou' }
cmake --build build
if ($LASTEXITCODE -ne 0) { throw 'build falhou' }
Write-Host "OK: build\dinput8.dll"

if ($GameDir) {
    if (-not (Test-Path (Join-Path $GameDir 'DarkSoulsIII.exe'))) { throw "DarkSoulsIII.exe não está em $GameDir" }
    Copy-Item build\dinput8.dll (Join-Path $GameDir 'dinput8.dll') -Force
    Write-Host "Instalado em $GameDir"
}
