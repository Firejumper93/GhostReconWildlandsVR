<#
.SYNOPSIS
    Import GRW.exe into a Ghidra project via analyzeHeadless.

.DESCRIPTION
    Session 1 (GRW-XR). Read-only with respect to the game install: Ghidra opens
    the exe for reading and writes its database into -ProjectDir only.

    Two modes:
      -NoAnalysis   import only. Fast-ish, bounded, gives session 2 a project to
                    open in the Ghidra GUI. This is what session 1 runs.
      (default)     full auto-analysis. GRW.exe is 352 MiB with ~330 MiB marked
                    executable, so expect MANY HOURS and a project directory in
                    the tens of GB. Run this overnight, not interactively.

    NOTE ON GRW.exe: the section table is unusual (see docs/TARGET-INVENTORY.md).
    Sections named .edata/.sbss/.link carry CNT_CODE + MEM_EXECUTE but are mostly
    data. Ghidra will try to disassemble them and waste enormous time. For the
    full run, prefer restricting analysis in the GUI to .text1 (the section that
    actually holds compiled code and all 1462 RTTI descriptors).

.EXAMPLE
    powershell -File tools\ghidra_import.ps1 -NoAnalysis
    powershell -File tools\ghidra_import.ps1            # full analysis, hours
#>
param(
    [string]$GhidraRoot = 'C:\ghidra_12.0.4_PUBLIC_20260303\ghidra_12.0.4_PUBLIC',
    [string]$Exe        = 'C:\Steam\steamapps\common\Wildlands\GRW.exe',
    [string]$ProjectDir = '.\ghidra',
    [string]$ProjectName = 'GRWVR',
    [switch]$NoAnalysis
)

$ErrorActionPreference = 'Stop'

$headless = Join-Path $GhidraRoot 'support\analyzeHeadless.bat'
if (-not (Test-Path $headless)) { throw "analyzeHeadless not found at $headless" }
if (-not (Test-Path $Exe))      { throw "target exe not found at $Exe" }

New-Item -ItemType Directory -Force -Path $ProjectDir | Out-Null
$logFile = Join-Path $ProjectDir 'headless.log'

$argList = @($ProjectDir, $ProjectName, '-import', $Exe, '-overwrite',
             '-log', $logFile, '-max-cpu', '6')
if ($NoAnalysis) { $argList += '-noanalysis' }

Write-Host "ghidra   : $headless"
Write-Host "target   : $Exe"
Write-Host "project  : $ProjectDir\$ProjectName.gpr"
Write-Host "analysis : $(if ($NoAnalysis) {'DISABLED (import only)'} else {'ENABLED (expect many hours)'})"
Write-Host "log      : $logFile"
Write-Host ''

& $headless @argList
Write-Host "analyzeHeadless exit code: $LASTEXITCODE"
