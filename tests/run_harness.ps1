# Builds the mod and the test harness, runs the harness with the mod loaded and converts the
# screenshots to PNG in build\harness\.
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Password "<senha>"
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Borderless
#     (the test window covers the screen for ~6 seconds, twice)
param([string]$Password = '', [switch]$Borderless)

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
# Mod settings for the run: borderless only when asked (it covers the whole screen).
$b = if ($Borderless) { 1 } else { 0 }
Set-Content "$out\ds3coopmod.ini" "[Video]`r`nfps_limit = 0`r`nborderless = $b`r`nfix_sprint = 1" -Encoding ascii

$failures = @()
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "OK    $what" } else { Write-Host "FALHA $what"; $script:failures += $what }
}

Push-Location $out
try {
    if ($Borderless) {
        # Crash guard: clear it after 1.5 s instead of 20 s, so it clears before the harness quits.
        $env:DS3COOPMOD_GUARD_MS = '1500'
        $run1 = & .\DarkSoulsIII.exe $Password borderless | Out-String
        Write-Host $run1
        $log1 = Get-Content ds3coopmod.log -Raw
        Check ($run1 -match 'caption=0') 'borderless: the window has no title bar'
        Check ($run1 -match 'game saw 0 resize messages') 'borderless: the game never saw the resize'
        Check ($log1 -match 'Borderless: OK') 'borderless: crash guard cleared after the game kept running'
        Check (-not (Test-Path ds3coopmod_borderless.check)) 'borderless: no crash-guard file left behind'

        # Second run as if the first one had crashed right after going borderless.
        Set-Content ds3coopmod_borderless.check 'x' -Encoding ascii
        $run2 = & .\DarkSoulsIII.exe $Password borderless | Out-String
        Write-Host $run2
        $log2 = Get-Content ds3coopmod.log -Raw
        Check ($log2 -match 'turned it off') 'crash guard: borderless turned off after a crash'
        Check ((Get-Content ds3coopmod.ini -Raw) -match 'borderless\s*=\s*0') 'crash guard: borderless = 0 written to the ini'
        Check ($run2 -match 'caption=1') 'crash guard: the window keeps its title bar'
        Check (-not (Test-Path ds3coopmod_borderless.check)) 'crash guard: file removed'
        Remove-Item Env:\DS3COOPMOD_GUARD_MS
    } else {
        & .\DarkSoulsIII.exe $Password
    }
} finally { Pop-Location }

Add-Type -AssemblyName System.Drawing
Get-ChildItem "$out\*.bmp" | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save([IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $_.FullName
}
Get-Content "$out\ds3coopmod.log"
if ($failures.Count) { throw "$($failures.Count) borderless check(s) failed" }
