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

$depsDir = "$WP\deps\build"
$buildDir = "$WP\build"
$srcPath = $WP
$depPath = $depsDir

# --- Phase 1: Dependencies ---
# Deps must use VS generator (OpenSSL uses nmake/msbuild internally)
Write-Host "=== Phase 1: Dependencies ==="
if (-not (Test-Path $depsDir)) { New-Item -ItemType Directory -Path $depsDir }

Write-Host "=== Configuring deps ==="
Invoke-IdlePriority -Exe "cmake" -ArgString "`"$WP\deps`" -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_BUILD_TYPE=Release" -WorkDir $depsDir

Write-Host "=== Building deps (no parallel MSBuild - OpenSSL PDB contention) ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release --target deps' -WorkDir $depsDir

# --- Phase 2: Slicer ---
# Use Ninja for faster incremental builds (requires vcvars64 environment)
Write-Host "=== Phase 2: Slicer (Ninja) ==="
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir }

# Source vcvars64 so Ninja can find cl.exe
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (Test-Path $vsPath) {
    Write-Host "Sourcing VS2022 environment for Ninja..."
    cmd /c "`"$vsPath`" amd64 >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}

Write-Host "=== Configuring slicer ==="
Invoke-IdlePriority -Exe "cmake" -ArgString "-S `"$srcPath`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DORCA_TOOLS=ON -DDEP_BUILD_DIR=`"$depPath`""

Write-Host "=== Building slicer ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release' -WorkDir $buildDir

Write-Host "=== Installing slicer ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --target install --config Release' -WorkDir $buildDir

# Copy dependency DLLs that the install target misses
$installDir = "$buildDir\OrcaSlicer"
$depBinDir = "$depPath\OrcaSlicer_dep\usr\local\bin"
if (Test-Path $depBinDir) {
    Write-Host "Copying dependency DLLs..."
    Copy-Item "$depBinDir\*.dll" $installDir -Force -ErrorAction SilentlyContinue
    if (Test-Path "$depBinDir\occt") {
        Copy-Item "$depBinDir\occt\*.dll" $installDir -Force -ErrorAction SilentlyContinue
    }
}
# WebView2Loader.dll lives in the deps source tree, not in the dep build output
$webview2Dll = "$WP\deps\WebView2\lib\win-x64\WebView2Loader.dll"
if (Test-Path $webview2Dll) {
    Write-Host "Copying WebView2Loader.dll..."
    Copy-Item $webview2Dll $installDir -Force
}

Write-Host "=== FULL BUILD COMPLETE ==="
Write-Host "Run: $installDir\orca-slicer.exe"
