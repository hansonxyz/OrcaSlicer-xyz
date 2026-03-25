# PROJECT.md - Local Development Environment Notes

This file documents the local development environment, build tooling, and conventions for working on this OrcaSlicer fork. Read this at the start of each session to get oriented.

## Development Environment

- **OS:** Windows 11 Pro
- **Shell:** Git Bash (default Claude Code shell on this system)
- **Python:** 3.14
- **Branch:** `xyz` (our feature branch, forked from `main`)

## Build Tooling

- **VS2022 Build Tools** (not the full IDE) - installed via `choco install visualstudio2022buildtools` with VCTools workload
  - MSVC compiler version: 14.44
  - Location: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\`
- **CMake 3.31.6** - installed via `choco install cmake --version=3.31.6`
  - Location: `C:\Program Files\CMake\bin\cmake.exe`
  - **Not in Git Bash PATH by default** - must use full path or prepend to PATH
  - **Do NOT use CMake 4.x** - wxWidgets' bundled cotire has `cmake_minimum_required` below 3.5, which CMake 4.x rejects. The OrcaSlicer CMakeLists.txt sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` but this doesn't propagate into wxWidgets' internal builds. Stick with 3.31.x.
- **Windows SDK:** 10.0.26100.0
- **Strawberry Perl** - required by OpenSSL dep build. Location: `C:\Strawberry\perl\bin\`
- **NASM** - required by OpenSSL dep build. Location: `C:\Program Files\NASM\`
- **Ninja** - fast build system used for slicer builds (deps still use VS generator). Location: in PATH via choco.
- Perl, NASM, Ninja installed via choco: `choco install nasm strawberryperl ninja -y`

## Shell & Script Execution Rules

- **Use PowerShell for build operations.** Git Bash has issues invoking .bat files and cmake is not in its PATH. Write PowerShell scripts to a temp directory and execute them via:
  ```bash
  powershell.exe -ExecutionPolicy Bypass -File "C:/Users/brian/AppData/Local/Temp/orcabuild/scriptname.ps1"
  ```
- **Always prepend cmake to PATH** at the top of every PowerShell build script:
  ```powershell
  $env:PATH = "C:\Program Files\CMake\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM;" + $env:PATH
  ```
- **Temp script directory:** `C:\Users\brian\AppData\Local\Temp\orcabuild\`

## CPU Priority for Builds

**All CPU-intensive build steps must run at Idle priority** to keep the system responsive.

### Invoke-IdlePriority helper function (copy into every build script)

Uses `System.Diagnostics.ProcessStartInfo` to launch the process, then immediately sets it to Idle priority. A background PowerShell job monitors for child processes (`cl`, `msbuild`, `link`) spawned by the build and forces them to Idle too.

**Important:** The `$ArgString` parameter is a single pre-formatted string, NOT an array. This is required because `ProcessStartInfo.Arguments` is a flat string - if you pass arguments as an array they get split on spaces and quoted generator names like `"Visual Studio 17 2022"` break apart. Use single-quoted strings with embedded double quotes for arguments that contain spaces.

```powershell
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
    # Monitor child processes and set them to idle too
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
```

### Usage example

```powershell
# Note: single-quoted string with embedded double quotes for the generator name
Invoke-IdlePriority -Exe "cmake" -ArgString '../ -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release'
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release --target deps -- -m'
```

Apply this to all build steps: deps configure, deps build, slicer configure, slicer build.

## Build Process

The build has two phases:

### Phase 1: Dependencies (one-time, ~30-60 min at normal priority, longer at idle)
```
cd deps/build
cmake ../ -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --target deps
```
**Do NOT use `-- -m` (parallel MSBuild) for deps.** OpenSSL builds with nmake in-source and hits PDB file contention (`fatal error C1041: cannot open program database 'app.pdb'`) when MSBuild runs multiple sub-targets in parallel. The slicer build can safely use `-m`.

**OpenSSL NUL file cleanup:** OpenSSL creates a file literally named `NUL` in its source tree (a reserved Windows device name). Neither PowerShell's `Remove-Item` nor `cmd /c rd /s /q` can delete it. Use `robocopy /MIR` with an empty directory to clean it:
```powershell
$emptyDir = "$env:TEMP\orcabuild_empty"
New-Item -ItemType Directory -Path $emptyDir -Force | Out-Null
robocopy $emptyDir "dep_OpenSSL-prefix" /MIR /NFL /NDL /NJH /NJS /nc /ns /np 2>$null
Remove-Item -Recurse -Force "dep_OpenSSL-prefix" -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $emptyDir -ErrorAction SilentlyContinue
```

### Phase 2: Slicer (after deps, faster for incremental rebuilds)

Uses **Ninja** generator for faster incremental builds. Requires vcvars64 environment sourced first.

```
# Source VS environment (needed for Ninja to find cl.exe)
# In PowerShell: source vcvars64.bat via cmd wrapper (see build scripts)
cmake -S <repo_root> -B <repo_root>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DORCA_TOOLS=ON -DDEP_BUILD_DIR=<repo_root>/deps/build
cmake --build build --config Release
```

**Must use `-S`/`-B` flags** for the configure step. Passing the source dir as a positional argument causes CMake to generate in-source instead of in `build/`.

**Must pass `-DDEP_BUILD_DIR`** pointing to `deps/build`. Without it, CMake infers the wrong path and can't find the built dependencies.

**Ninja vs Visual Studio generator:** Ninja has much faster incremental build detection (checks timestamps directly instead of going through MSBuild's project system). The deps build still uses the VS generator because OpenSSL's build system depends on nmake/msbuild internally.

**Selective /LTCG:** The final link targets (OrcaSlicer, OrcaSlicer_app_gui) use `/LTCG` in Release mode to prevent linker restarts from TBB's `/GL`-compiled objects. This is applied only to these targets, not globally, so incremental linking is preserved for library targets during development.

Output binary: `build/src/orca-slicer.exe` (raw build output, missing DLLs)

**Runnable installation:** `build/OrcaSlicer/orca-slicer.exe` (after install step - has all DLLs, resources, and WebView2). Always run from this location, not from `build/src/`.

The `build_release_vs2022.bat` script automates both phases but doesn't work well from Git Bash - use the PowerShell approach instead.

## Build Policy

**Always use incremental builds for development iteration.** Full rebuilds are only needed when:
- Switching CMake generators (e.g., VS to Ninja)
- Build directory becomes corrupted
- First-time setup (deps + initial slicer build)

Never wipe the build directory just to rebuild after code changes. Ninja's incremental builds are fast and reliable.

## Build Artifacts Directory

`xyz/build_artifacts/` contains reusable build scripts and tooling for this fork. This directory is committed separately so it can be excluded when cherry-picking feature commits for upstream PRs.

- `build_all.ps1` - Full build script (deps + slicer + install + DLL copy). Only for first-time setup or generator changes.
- `build_incremental.ps1` - **Use this for development.** Builds changed files, installs, and copies DLLs.
  ```bash
  powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
  ```
  Both scripts auto-detect the repo root relative to their own location.

## Project Goals

See `GOALS.md` for the feature roadmap (3 features + 1 future wishlist item).

## Git Workflow

- Feature branch: `xyz` (off `main`)
- Clean, atomic commits suitable for upstream PR submission
- One feature at a time, in order listed in GOALS.md

## Git Remotes

- **origin** - upstream OrcaSlicer (SoftFever/OrcaSlicer on GitHub)
- **internal** - `git@git.internal.hanson.xyz:brianhansonxyz/orcaslicer_mod.git` - our internal repository for this fork. Push feature work here after committing.
