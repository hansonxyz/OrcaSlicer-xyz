# OrcaSlicer Incremental Build Script (Windows)
# Usage: powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
#
# Rebuilds only changed files. Use after deps and initial slicer build are done.
# All CPU-intensive processes run at Idle priority.

$ErrorActionPreference = "Stop"
$env:PATH = "C:\Program Files\CMake\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM;" + $env:PATH
$WP = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$buildDir = "$WP\build"

function Invoke-IdlePriority {
    param([string]$Exe, [string]$ArgString, [string]$WorkDir)
    Write-Host "Running at IDLE priority: $Exe $ArgString"
    if ($WorkDir) { Write-Host "  Working dir: $WorkDir" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $ArgString
    $psi.UseShellExecute = $false
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
    $proc = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 500
    try { $proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Idle } catch {}
    $monitor = Start-Job -ScriptBlock {
        while ($true) {
            Get-Process -Name cl,msbuild,link,MSBuild -ErrorAction SilentlyContinue | ForEach-Object {
                try { $_.PriorityClass = 'Idle' } catch {}
            }
            Start-Sleep -Seconds 3
        }
    }
    $proc.WaitForExit()
    Stop-Job $monitor -ErrorAction SilentlyContinue
    Remove-Job $monitor -ErrorAction SilentlyContinue
    if ($proc.ExitCode -ne 0) {
        Write-Host "FAILED with exit code $($proc.ExitCode)"
        exit 1
    }
}

# Source vcvars64 so Ninja can find cl.exe (needed if not already in environment)
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if ((Test-Path $vsPath) -and -not $env:VSINSTALLDIR) {
    Write-Host "Sourcing VS2022 environment for Ninja..."
    cmd /c "`"$vsPath`" amd64 >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}

Write-Host "=== Building slicer (incremental) ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release' -WorkDir $buildDir

Write-Host "=== BUILD COMPLETE ==="
