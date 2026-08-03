# watch_deploy.ps1 - wait for the game to exit, then deploy immediately.
#
# WHY. The deploy protocol costs a round trip every build: the user quits, says
# "closed", waits for an explicit "deployed", then launches. Two failure modes
# recur (hazard 21): the user relaunches before the copy lands (the run then
# tests the OLD dll and rule 5 catches it late), or the process lingers a few
# seconds after the window closes and the copy fails against a locked file.
#
# This removes the round trip. Start it BEFORE asking the user to quit; it
# polls for the game, waits until the process is really gone, then runs
# deploy.bat auto and reports the deployed SHA256. Nothing is ever deployed
# while the game is up, so a race cannot leave a half-copied dll.
#
# It also sidesteps a known deploy.bat defect: its own game-closed check uses
# `find /i`, which under Git Bash resolves to POSIX find, errors, and reports
# "game is closed" regardless. The check here is a real process query.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\watch_deploy.ps1
#   ... -TimeoutMinutes 45      how long to wait for the game to exit
#   ... -PollSeconds 2          poll interval
#
# Exit codes: 0 deployed, 1 deploy failed, 2 timed out (nothing deployed).

param(
    [int]$TimeoutMinutes = 30,
    [int]$PollSeconds = 2
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# rungame.exe is the EasyAntiCheat launcher; deploy.bat refuses while it runs.
$names = @('GRW', 'rungame')

function Get-GameProcs {
    $found = @()
    foreach ($n in $names) {
        $p = Get-Process -Name $n -ErrorAction SilentlyContinue
        if ($p) { $found += $p }
    }
    return $found
}

$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$running = Get-GameProcs

if ($running) {
    Write-Host "watch_deploy: game is UP (pid $($running.Id -join ', ')). Waiting for exit..."
    while ($true) {
        if ((Get-Date) -gt $deadline) {
            Write-Host "watch_deploy: TIMED OUT after $TimeoutMinutes min, game still running. NOTHING DEPLOYED."
            exit 2
        }
        Start-Sleep -Seconds $PollSeconds
        if (-not (Get-GameProcs)) { break }
    }
    # The image stays locked briefly after the process record disappears.
    Write-Host "watch_deploy: game exited, settling..."
    Start-Sleep -Seconds 3
} else {
    Write-Host "watch_deploy: game is already closed."
}

Write-Host "watch_deploy: deploying..."
# Native tools write progress to stderr; with ErrorActionPreference = Stop that
# would abort the script mid-deploy (it did, 2026-08-01). Judge by exit code.
$ErrorActionPreference = 'Continue'
$out = & cmd.exe /c "`"$root\deploy.bat`" auto" 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }

if ($code -ne 0) {
    Write-Host "watch_deploy: DEPLOY FAILED (exit $code). Do not launch."
    exit 1
}

$dll = 'C:\Steam\steamapps\common\Wildlands\dxgi.dll'
$sha = (Get-FileHash -Path $dll -Algorithm SHA256).Hash.ToLower()
Write-Host ""
Write-Host "watch_deploy: DEPLOYED OK"
Write-Host "watch_deploy: sha256 = $sha"
Write-Host "watch_deploy: mtime  = $((Get-Item $dll).LastWriteTime)"
Write-Host "watch_deploy: safe to launch."
exit 0
