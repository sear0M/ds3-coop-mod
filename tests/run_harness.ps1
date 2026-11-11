# Builds the mod and the test harness, runs the harness with the mod loaded and converts the
# screenshots to PNG in build\harness\.
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Password "<senha>"
param([string]$Password = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null
cmake --build build
if ($LASTEXITCODE -ne 0) { throw 'mod build failed' }

$out = Join-Path $root 'build\harness'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*" -Force -ErrorAction SilentlyContinue
# The harness must be called DarkSoulsIII.exe: the mod only starts inside a process with that name.
g++ -std=c++17 -O1 tests\harness.cpp -o "$out\DarkSoulsIII.exe" -ld3d11 -ldinput8 -static
if ($LASTEXITCODE -ne 0) { throw 'harness build failed' }
Copy-Item build\dinput8.dll $out

Push-Location $out
try { & .\DarkSoulsIII.exe $Password } finally { Pop-Location }

Add-Type -AssemblyName System.Drawing
Get-ChildItem "$out\*.bmp" | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save([IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $_.FullName
}
Get-Content "$out\ds3coopmod.log"
