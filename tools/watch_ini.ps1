<#
.SYNOPSIS
    Watch GRW.ini and print every key that changes, as it changes.

.DESCRIPTION
    Session 1 (GRW-XR). Read-only: polls the file, never writes it.

    Purpose: build the menu-label to INI-value mapping without quitting the game
    after every change. Leave this running in a second window, then cycle through
    options in game. Each time the game rewrites GRW.ini, every changed key is
    printed with its old and new value.

    It also answers a separate question as a side effect: if lines appear while
    the game is still running, the game writes settings on apply. If nothing
    appears until you quit, it writes only on exit.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\watch_ini.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\watch_ini.ps1 -IntervalMs 250 -LogPath docs\RAW\ini-changes.log
#>
param(
    [string]$IniPath  = "$env:USERPROFILE\Documents\My Games\Ghost Recon Wildlands\GRW.ini",
    [int]$IntervalMs  = 500,
    [string]$LogPath  = 'docs\RAW\ini-changes.log'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $IniPath)) { throw "GRW.ini not found at: $IniPath" }

# Parse an INI into an ordered "Section/Key" -> value map.
function Read-Ini([string]$path) {
    $map = [ordered]@{}
    $section = ''
    # Retry briefly: the game may hold the file mid-write.
    for ($try = 0; $try -lt 10; $try++) {
        try { $lines = [System.IO.File]::ReadAllLines($path); break }
        catch { Start-Sleep -Milliseconds 40; $lines = $null }
    }
    if ($null -eq $lines) { return $null }
    foreach ($line in $lines) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith(';')) { continue }
        if ($t -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
        if ($t -match '^([^=]+)=(.*)$') {
            $map["$section/$($Matches[1].Trim())"] = $Matches[2].Trim()
        }
    }
    return $map
}

$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }

function Emit([string]$text, [string]$colour) {
    Write-Host $text -ForegroundColor $colour
    Add-Content -Path $LogPath -Value $text -Encoding utf8
}

$prev = Read-Ini $IniPath
if ($null -eq $prev) { throw "Could not read $IniPath" }

Emit ("=" * 72) 'DarkGray'
Emit "watching : $IniPath" 'Gray'
Emit "logging  : $LogPath" 'Gray'
Emit "baseline : $($prev.Count) keys at $(Get-Date -Format 'HH:mm:ss')" 'Gray'
Emit "" 'Gray'
Emit "Change a setting in game. Changed keys appear below." 'Cyan'
Emit "For Q6: watch 'Quality And Performance/AntiAliasingMode'." 'Cyan'
Emit "Press Ctrl+C to stop." 'DarkGray'
Emit ("=" * 72) 'DarkGray'
Emit "" 'Gray'
Emit ("current AntiAliasingMode = " +
      $(if ($prev.Contains('Quality And Performance/AntiAliasingMode'))
        { $prev['Quality And Performance/AntiAliasingMode'] } else { '(absent)' })) 'Yellow'
Emit "" 'Gray'

$lastWrite = (Get-Item $IniPath).LastWriteTimeUtc

while ($true) {
    Start-Sleep -Milliseconds $IntervalMs

    try { $stamp = (Get-Item $IniPath).LastWriteTimeUtc } catch { continue }
    if ($stamp -eq $lastWrite) { continue }
    $lastWrite = $stamp

    $curr = Read-Ini $IniPath
    if ($null -eq $curr) { continue }

    $now = Get-Date -Format 'HH:mm:ss'
    $changes = @()

    foreach ($k in $curr.Keys) {
        if (-not $prev.Contains($k)) {
            $changes += ,@('ADDED  ', $k, '', $curr[$k])
        } elseif ($prev[$k] -ne $curr[$k]) {
            $changes += ,@('CHANGED', $k, $prev[$k], $curr[$k])
        }
    }
    foreach ($k in $prev.Keys) {
        if (-not $curr.Contains($k)) { $changes += ,@('REMOVED', $k, $prev[$k], '') }
    }

    if ($changes.Count -gt 0) {
        Emit "[$now] $($changes.Count) change(s), $($curr.Count) keys total" 'DarkGray'
        foreach ($c in $changes) {
            $kind = $c[0]; $key = $c[1]; $old = $c[2]; $new = $c[3]
            $colour = if ($key -like '*AntiAliasing*') { 'Yellow' }
                      elseif ($kind -eq 'CHANGED')     { 'Green' }
                      else                             { 'Magenta' }
            if ($kind -eq 'CHANGED') { Emit ("  {0}  {1,-46} {2}  ->  {3}" -f $kind, $key, $old, $new) $colour }
            else                     { Emit ("  {0}  {1,-46} {2}{3}"       -f $kind, $key, $old, $new) $colour }
        }
        Emit "" 'Gray'
        $prev = $curr
    }
}
