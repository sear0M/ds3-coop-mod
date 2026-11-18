# Builds the mod and the test harness, runs the harness with the mod loaded and converts the
# screenshots to PNG in build\harness\.
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1
#     (menu test with the mouse and keyboard, then with a simulated controller. Without -Password
#      it uses a test build of the mod with a test password, so it still reaches the item window.
#      That test build lives in build\harness-mod, never ship it.)
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Password "<senha>"
#     (mouse and keyboard menu test with the real build\dinput8.dll and the real password)
#   powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Borderless
#     (the test window covers the screen for ~6 seconds, twice)
param([string]$Password = '', [switch]$Borderless)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null
cmake --build build
if ($LASTEXITCODE -ne 0) { throw 'mod build failed' }
$dll = 'build\dinput8.dll'
$testBuild = $false

if (-not $Borderless -and -not $Password) {
    # Upper case, digit and symbol: the controller test types it on the on-screen keyboard.
    $Password = 'Coop-3'
    $testBuild = $true
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Password)) | ForEach-Object { $_.ToString('x2') })
    # The configure prints a CMake warning on purpose (this dll has the test password). PowerShell
    # 5.1 turns that stderr text into an error, so let it through while configuring.
    $ErrorActionPreference = 'Continue'
    cmake -S . -B build\harness-mod -G Ninja -DCMAKE_BUILD_TYPE=Release "-DDS3_HARNESS_PASSWORD_SHA256=$hash" *> $null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) { throw 'test mod configure failed' }
    cmake --build build\harness-mod --target dinput8
    if ($LASTEXITCODE -ne 0) { throw 'test mod build failed' }
    $dll = 'build\harness-mod\dinput8.dll'
}

$out = Join-Path $root 'build\harness'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*" -Force -ErrorAction SilentlyContinue
# The harness must be called DarkSoulsIII.exe: the mod only starts inside a process with that name.
g++ -std=c++17 -O1 tests\harness.cpp -o "$out\DarkSoulsIII.exe" -ld3d11 -ldinput8 -static
if ($LASTEXITCODE -ne 0) { throw 'harness build failed' }
Copy-Item $dll $out
# Mod settings for the run: borderless only when asked (it covers the whole screen).
$b = if ($Borderless) { 1 } else { 0 }
Set-Content "$out\ds3coopmod.ini" "[Video]`r`nfps_limit = 0`r`nborderless = $b`r`nfix_sprint = 1" -Encoding ascii

$failures = @()
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "OK    $what" } else { Write-Host "FALHA $what"; $script:failures += $what }
}

Push-Location $out
# A real controller on this PC must not press buttons in the middle of the test: the mod reads
# controller 0 from this (missing) shared memory instead, so it sees no controller at all.
$env:DS3COOPMOD_TEST_PAD = 'Local\ds3coopmod_no_pad'
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
        $run = & .\DarkSoulsIII.exe $Password | Out-String
        Write-Host $run
        $log = Get-Content ds3coopmod.log -Raw
        Check ($run -match 'done') 'menu: the harness ran to the end'
        # Pressed once on the password screen (no password needed); the footer one is cancelled.
        $refused = ([regex]::Matches($log, 'Bonfire warp refused: version')).Count
        Check ($refused -eq 1) "fogueira: refused once outside the real game (got $refused)"
        Check ($log -match 'Menu unlocked') 'menu: the password opened the item window'

        if ($testBuild) {
            # Same menu with only a controller (it types the test password, so test build only).
            Copy-Item ds3coopmod.log ds3coopmod_menu.log  # the next run starts a new log
            $env:DS3COOPMOD_TEST_PAD = 'Local\ds3coopmod_test_pad'
            $padRun = & .\DarkSoulsIII.exe $Password pad | Out-String
            Write-Host $padRun
            $padLog = Get-Content ds3coopmod.log -Raw
            $ok = ([regex]::Matches($padRun, 'PAD OK')).Count
            $bad = ([regex]::Matches($padRun, 'PAD FALHA')).Count
            Check ($ok -eq 7 -and $bad -eq 0) "pad: the game gets the controller only when it should ($ok OK, $bad failed)"
            Check ($padLog -match 'XInputGetState hooked') 'pad: XInputGetState hooked'
            Check ($padLog -match 'Menu opened with the controller') 'pad: L3 opened the menu'
            Check ($padLog -match 'Menu unlocked') 'pad: the password typed on the on-screen keyboard opened the item window'
        }
    }
} finally {
    Remove-Item Env:\DS3COOPMOD_TEST_PAD -ErrorAction SilentlyContinue
    Pop-Location
}

Add-Type -AssemblyName System.Drawing
Get-ChildItem "$out\*.bmp" | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save([IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $_.FullName
}
Get-Content "$out\ds3coopmod.log"
if ($failures.Count) { throw "$($failures.Count) check(s) failed" }
