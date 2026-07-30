<#
.SYNOPSIS
    Log every relevant process start and exit during a Wildlands launch attempt.

.DESCRIPTION
    Session 1 (GRW-XR). Read-only: observes only, starts and kills nothing.

    Purpose: when the game "does not open", find out how far it actually gets.
    The interesting cases are distinguishable by what appears and how long it
    lives:

      nothing appears at all
          the launcher never spawned the game. Ownership, entitlement, or
          launch-path problem, not a game problem.

      GRW.exe appears and dies in under ~5 seconds
          the game started and bailed early. Config, DRM activation, or
          anti-tamper. Check the Application event log next.

      GRW.exe appears and survives
          it is running. If you see no window it is a display-mode problem,
          for example WindowMode pointing at a monitor that is not there.

    Also logs EasyAntiCheat so we can answer whether it loads on a given
    launch path (docs/QUESTIONS.md Q7).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\watch_launch.ps1
    (then launch the game however you normally would)
#>
param(
    [int]$IntervalMs = 200,
    [int]$TimeoutSec = 180,
    [string]$LogPath = 'docs\RAW\launch-attempt.log'
)

$ErrorActionPreference = 'Continue'

$watch = 'GRW', 'rungame', 'upc', 'UbisoftConnect', 'UplayWebCore',
         'EasyAntiCheat', 'steam', 'BattlEye'

$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }

function Emit([string]$s, [string]$c = 'Gray') {
    Write-Host $s -ForegroundColor $c
    Add-Content -Path $LogPath -Value $s -Encoding utf8
}

function Snapshot {
    $h = @{}
    foreach ($p in Get-Process -ErrorAction SilentlyContinue) {
        foreach ($w in $watch) {
            if ($p.ProcessName -like "$w*") { $h[$p.Id] = $p.ProcessName; break }
        }
    }
    return $h
}

Emit ("=" * 72) 'DarkGray'
Emit "launch watcher started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" 'Cyan'
Emit "watching: $($watch -join ', ')" 'DarkGray'
Emit "log     : $LogPath" 'DarkGray'
Emit ""
Emit "NOW LAUNCH THE GAME. Ctrl+C when done." 'Cyan'
Emit ("=" * 72) 'DarkGray'

$prev = Snapshot
foreach ($id in $prev.Keys) { Emit ("  already running : {0} (pid {1})" -f $prev[$id], $id) 'DarkGray' }
Emit ""

$births = @{}
$deadline = (Get-Date).AddSeconds($TimeoutSec)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds $IntervalMs
    $curr = Snapshot
    $now = Get-Date -Format 'HH:mm:ss.fff'

    foreach ($id in $curr.Keys) {
        if (-not $prev.ContainsKey($id)) {
            $births[$id] = Get-Date
            $path = try { (Get-Process -Id $id -ErrorAction Stop).Path } catch { '' }
            if (-not $path) { $path = '(path not readable: protected process)' }
            Emit ("[{0}] START  {1,-16} pid {2,-7} {3}" -f $now, $curr[$id], $id, $path) 'Green'
        }
    }
    foreach ($id in $prev.Keys) {
        if (-not $curr.ContainsKey($id)) {
            $life = if ($births.ContainsKey($id)) {
                        '{0:N1}s' -f ((Get-Date) - $births[$id]).TotalSeconds
                    } else { 'unknown' }
            $colour = if ($life -ne 'unknown' -and [double]($life -replace 's','') -lt 5) { 'Red' } else { 'Yellow' }
            Emit ("[{0}] EXIT   {1,-16} pid {2,-7} lived {3}" -f $now, $prev[$id], $id, $life) $colour
        }
    }
    $prev = $curr
}

Emit ""
Emit "watcher timed out after $TimeoutSec s" 'DarkGray'
