# End-to-end test of DS3CoopLauncher.exe without GitHub or the game:
# a local HTTP server plays the GitHub release, and a fake game folder gets updated.
#   powershell -ExecutionPolicy Bypass -File tests\test_launcher.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root 'build\launcher-test'
$release = Join-Path $work 'release'
$game = Join-Path $work 'game'
$port = 8765
$failures = 0

function Check($name, $ok) {
    if ($ok) { Write-Host "  OK   $name" } else { Write-Host "  FAIL $name"; $script:failures++ }
}
function Hash($path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() }
function WriteManifest($dllHash) {
    $lines = @('version=build-test', 'commit=0000000', "files_url=http://localhost:$port/")
    $lines += "dinput8.dll=$dllHash"
    $lines += "DS3CoopLauncher.exe=$(Hash "$release\DS3CoopLauncher.exe")"
    $lines += "seamless_overrides.ini=$(Hash "$release\seamless_overrides.ini")"
    Set-Content "$release\manifest.txt" $lines -Encoding ascii
}
function RunLauncher() {
    $p = Start-Process "$game\DS3CoopLauncher.exe" -ArgumentList @('--source', "http://localhost:$port/", '--no-launch') -PassThru
    $p.WaitForExit(60000) | Out-Null
    # After a self-update the launcher restarts itself: wait for the new process too.
    for ($i = 0; $i -lt 120; $i++) {
        $left = Get-Process DS3CoopLauncher -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$game*" }
        if (-not $left) { break }
        Start-Sleep -Milliseconds 500
    }
    return $p.ExitCode
}

# --- Fake release: current build, launcher with extra bytes so it counts as "newer" ---------
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $release, $game, "$game\SeamlessCoop" | Out-Null
Copy-Item "$root\build\dinput8.dll" $release
Copy-Item "$root\launcher\seamless_overrides.ini" $release
$bytes = [IO.File]::ReadAllBytes("$root\build\DS3CoopLauncher.exe") + [byte[]](1..32)
[IO.File]::WriteAllBytes("$release\DS3CoopLauncher.exe", $bytes)
WriteManifest (Hash "$release\dinput8.dll")

# --- Fake game folder with an old launcher, no mod and old Seamless settings -----------------
Copy-Item "$root\build\DS3CoopLauncher.exe" $game
Set-Content "$game\DarkSoulsIII.exe" 'fake' -Encoding ascii
Set-Content "$game\ds3sc_launcher.exe" 'fake' -Encoding ascii
@'
[GAMEPLAY]

; comment kept
allow_invaders = 1
overhead_player_display = 2
skip_intros = 1

[PASSWORD]
cooppassword = 1234
'@ | Set-Content "$game\SeamlessCoop\ds3sc_settings.ini" -Encoding ascii

# --- Local "GitHub" ------------------------------------------------------------------------
$server = Start-Job -ArgumentList $release, $port -ScriptBlock {
    param($dir, $port)
    $l = New-Object System.Net.HttpListener
    $l.Prefixes.Add("http://localhost:$port/")
    $l.Start()
    while ($l.IsListening) {
        $ctx = $l.GetContext()
        $file = Join-Path $dir ($ctx.Request.Url.AbsolutePath.TrimStart('/'))
        if (Test-Path $file -PathType Leaf) {
            $data = [IO.File]::ReadAllBytes($file)
            $ctx.Response.ContentLength64 = $data.Length
            $ctx.Response.OutputStream.Write($data, 0, $data.Length)
        } else { $ctx.Response.StatusCode = 404 }
        $ctx.Response.Close()
    }
}
Start-Sleep -Seconds 2

try {
    Write-Host "1) First run: self-update, install mod, apply settings"
    $code = RunLauncher
    $ini = Get-Content "$game\SeamlessCoop\ds3sc_settings.ini" -Raw
    Check 'exit code 0' ($code -eq 0)
    Check 'launcher updated itself' ((Hash "$game\DS3CoopLauncher.exe") -eq (Hash "$release\DS3CoopLauncher.exe"))
    Check 'old launcher removed' (-not (Test-Path "$game\DS3CoopLauncher.exe.old"))
    Check 'mod installed' ((Hash "$game\dinput8.dll") -eq (Hash "$release\dinput8.dll"))
    Check 'allow_invaders = 0' ($ini -match '(?m)^allow_invaders = 0\r?$')
    Check 'overhead_player_display = 4' ($ini -match '(?m)^overhead_player_display = 4\r?$')
    Check 'cooppassword kept' ($ini -match '(?m)^cooppassword = 1234\r?$')
    Check 'comment kept' ($ini -match 'comment kept')
    Check 'settings backup made' (Test-Path "$game\SeamlessCoop\ds3sc_settings.ini.bak")
    Check 'version file written' ((Get-Content "$game\ds3coopmod_version.txt" -Raw) -match 'build-test')

    Write-Host "2) Second run: nothing to download"
    $code = RunLauncher
    $log = Get-Content "$game\DS3CoopLauncher.log" -Raw -Encoding UTF8
    Check 'exit code 0' ($code -eq 0)
    Check 'reports already up to date' ($log -match 'mais nova')

    Write-Host "3) Corrupted download is refused"
    WriteManifest ('0' * 64)
    $code = RunLauncher
    $log = Get-Content "$game\DS3CoopLauncher.log" -Raw -Encoding UTF8
    Check 'exit code 1' ($code -eq 1)
    Check 'hash error reported' ($log -match 'corrompido')
    Check 'installed mod untouched' ((Hash "$game\dinput8.dll") -eq (Hash "$release\dinput8.dll"))
} finally {
    Stop-Job $server; Remove-Job $server -Force
}

if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'All launcher checks passed'
