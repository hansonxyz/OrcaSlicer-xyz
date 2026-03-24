# OrcaSlicer Full Build Script (Windows)
# Usage: powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_all.ps1
#
# Builds deps (without parallel MSBuild) then the slicer (with parallel).
# All CPU-intensive processes run at Idle priority.
# See PROJECT.md for environment prerequisites and troubleshooting notes.

$ErrorActionPreference = "Stop"
$env:PATH = "C:\Program Files\CMake\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM;" + $env:PATH
$WP = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

function Invoke-IdlePriority {
    param([string]$Exe, [string]$ArgString)
    Write-Host "Running at IDLE priority: $Exe $ArgString"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $ArgString
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 500
    try { $proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Idle } catch {}
    $monitor = Start-Job -ScriptBlock {
        while ($true) {
            Get-Process -Name cl,msbuild,link,MSBuild,perl,nasm,nmake -ErrorAction SilentlyContinue | ForEach-Object {
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

# Helper: clean a directory that may contain Windows reserved-name files (e.g. NUL)
function Remove-DirSafe {
    param([string]$Path)
    if (Test-Path $Path) {
        Write-Host "Cleaning $Path using robocopy /MIR..."
        $emptyDir = "$env:TEMP\orcabuild_empty"
        if (-not (Test-Path $emptyDir)) { New-Item -ItemType Directory -Path $emptyDir | Out-Null }
        robocopy $emptyDir $Path /MIR /NFL /NDL /NJH /NJS /nc /ns /np 2>$null
        Remove-Item -Recurse -Force $Path -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force $emptyDir -ErrorAction SilentlyContinue
    }
}

# --- Phase 1: Dependencies ---
Write-Host "=== Phase 1: Dependencies ==="
cd "$WP\deps"
if (-not (Test-Path "build")) { New-Item -ItemType Directory -Name "build" }
cd build

Write-Host "=== Configuring deps ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '../ -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release'

Write-Host "=== Building deps (no parallel MSBuild - OpenSSL PDB contention) ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release --target deps'

# --- Phase 2: Slicer ---
Write-Host "=== Phase 2: Slicer ==="
cd $WP
if (-not (Test-Path "build")) { New-Item -ItemType Directory -Name "build" }
cd build

Write-Host "=== Configuring slicer ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '.. -G "Visual Studio 17 2022" -A x64 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=Release'

Write-Host "=== Building slicer (parallel OK here) ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release --target ALL_BUILD -- -m'

Write-Host "=== FULL BUILD COMPLETE ==="
