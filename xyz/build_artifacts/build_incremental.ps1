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

$env:PATH = "C:\ProgramData\chocolatey\bin;" + $env:PATH
$depsDir = "$WP\deps\build"

# Ninja automatically reconfigures when CMakeLists.txt changes.
# Do NOT run cmake configure on every build — it invalidates the git hash
# define, causing hundreds of files to recompile unnecessarily.

Write-Host "=== Building slicer (incremental) ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release' -WorkDir $buildDir

Write-Host "=== Installing ==="
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --target install --config Release' -WorkDir $buildDir

# Copy dependency DLLs that the install target misses
$installDir = "$buildDir\OrcaSlicer"
$depBinDir = "$depsDir\OrcaSlicer_dep\usr\local\bin"
if (Test-Path $depBinDir) {
    Copy-Item "$depBinDir\*.dll" $installDir -Force -ErrorAction SilentlyContinue
    if (Test-Path "$depBinDir\occt") {
        Copy-Item "$depBinDir\occt\*.dll" $installDir -Force -ErrorAction SilentlyContinue
    }
}
$webview2Dll = "$WP\deps\WebView2\lib\win-x64\WebView2Loader.dll"
if (Test-Path $webview2Dll) {
    Copy-Item $webview2Dll $installDir -Force
}

# Sync resources from source tree to installed location
# The cmake install target doesn't always detect resource-only changes
# Use /E (copy all subdirs) not /MIR (mirror/delete) to avoid removing
# generated files that cmake install placed in the destination
Write-Host "=== Syncing resources ==="
$srcResources = "$WP\resources"
$dstResources = "$installDir\resources"
if (Test-Path $srcResources) {
    robocopy $srcResources $dstResources /E /NFL /NDL /NJH /NJS /nc /ns /np /XD .git 2>$null | Out-Null
}

Write-Host "=== BUILD COMPLETE ==="
Write-Host "Run: $installDir\orca-slicer.exe"
