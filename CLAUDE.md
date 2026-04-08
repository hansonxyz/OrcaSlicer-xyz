# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

OrcaSlicer is an open-source 3D slicer application forked from Bambu Studio, built using C++ with wxWidgets for the GUI and CMake as the build system. The project uses a modular architecture with separate libraries for core slicing functionality, GUI components, and platform-specific code.

## Build Commands

### Building on Windows (xyz fork)
**Always use the incremental build script for development iteration on Windows.** It handles vcvars64 environment sourcing, Ninja, idle CPU priority, install, and DLL copy in one step:
```bash
powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
```
This is the **only correct way** to build from Git Bash. Do NOT invoke `cmake --build` directly from Git Bash — it lacks the VS environment (INCLUDE/LIB paths) needed by MSVC, which the PowerShell script sources via vcvars64.bat.

The script lives at `xyz/build_artifacts/build_incremental.ps1` and auto-detects the repo root. See the "xyz Fork: Local Development Environment" section at the end of this file for full build documentation including deps, full rebuilds, and the idle-priority helper.

### Building on macOS
```bash
cmake --build build/arm64 --config RelWithDebInfo --target all --
```

### Building on Linux
```bash
cmake --build build --config RelWithDebInfo --target all --
```


### Build System
- Uses CMake with minimum version 3.13 (maximum 3.31.x on Windows)
- Primary build directory: `build/`
- Dependencies are built in `deps/build/`
- The build process is split into dependency building and main application building
- Windows builds use Visual Studio generators
- macOS builds use Xcode by default, Ninja with -x flag
- Linux builds use Ninja generator

### Testing
Tests are located in the `tests/` directory and use the Catch2 testing framework. Test structure:
- `tests/libslic3r/` - Core library tests (21 test files)
  - Geometry processing, algorithms, file formats (STL, 3MF, AMF)
  - Polygon operations, clipper utilities, Voronoi diagrams
- `tests/fff_print/` - Fused Filament Fabrication tests (12 test files)
  - Slicing algorithms, G-code generation, print mechanics
  - Fill patterns, extrusion, support material
- `tests/sla_print/` - Stereolithography tests (4 test files)
  - SLA-specific printing algorithms, support generation
- `tests/libnest2d/` - 2D nesting algorithm tests
- `tests/slic3rutils/` - Utility function tests
- `tests/sandboxes/` - Experimental/sandbox test code

Run all tests after building:
```bash
cd build && ctest
```

Run tests with verbose output:
```bash
cd build && ctest --output-on-failure
```

Run individual test suites:
```bash
# From build directory
ctest --test-dir ./tests/libslic3r/libslic3r_tests
ctest --test-dir ./tests/fff_print/fff_print_tests
ctest --test-dir ./tests/sla_print/sla_print_tests
# and so on
```

### Post-Build Testing Workflow

**Standard test cycle (kill → build → launch):**
```bash
# Step 1: Kill any running instance (MUST use PowerShell, not taskkill)
powershell.exe -Command "Stop-Process -Name orca-slicer -Force -ErrorAction SilentlyContinue"

# Step 2: Build
powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1

# Step 3: Launch with test file (note: inner quotes needed for paths with spaces)
powershell.exe -Command "Start-Process -FilePath 'C:\Users\brian\bin\orca_conf_gen\reference\orcaslicer\build\OrcaSlicer\orca-slicer.exe' -ArgumentList '\"C:\Users\brian\Desktop\Crystal Dragon Statue - Spryo.3mf\"' -WorkingDirectory 'C:\Users\brian\bin\orca_conf_gen\reference\orcaslicer\build\OrcaSlicer'"
```

**Detecting if the app is running:**
```bash
# DO NOT use tasklist from Git Bash — it silently fails and gives false negatives.
# ALWAYS use PowerShell Get-Process:
powershell.exe -Command "Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue | Select-Object Id, ProcessName"
```

**Killing the app before builds:**
```bash
# taskkill does NOT work reliably from Git Bash. Always use PowerShell:
powershell.exe -Command "Stop-Process -Name orca-slicer -Force -ErrorAction SilentlyContinue"
```

**Resource file changes (HTML, images, etc.):**
The incremental build script syncs `resources/` to `build/OrcaSlicer/resources/` using `robocopy /E` after each build. This ensures HTML changes (like WebView dialogs) are deployed without a full reinstall. Do NOT use `robocopy /MIR` — the `/MIR` flag deletes destination files not in the source, which destroys cmake-installed generated files and causes startup crashes.

**Bambu DLL Crash Investigation (CRITICAL — ACTIVE):**

The xyz fork experiences frequent crashes from the Bambu networking DLL (`bambu_networking_*.dll`). **These crashes are OUR problem.** The fact that the crashing code is in a closed-source DLL is a constraint on HOW we debug it, not a reason to dismiss it. Stock OrcaSlicer uses the same DLL and does not crash this frequently, which means something in our fork is triggering it. We must find what and fix it.

**NEVER say "not our code" or "not our fault" about these crashes.** If our software crashes, it is our responsibility to fix it regardless of which module the fault address is in. Treat this exactly like any other bug: reproduce, bisect, fix.

Two crash patterns (both from the Bambu DLL):
1. `0xc0000005` ACCESS_VIOLATION in `bambu_networking_*.dll` — background thread null deref. Generates crash log. **BambuCrashGuard VEH catches these** (terminates thread, not process).
2. `0xc0000409` STATUS_STACK_BUFFER_OVERRUN in `ucrtbase.dll` — DLL corrupts its stack, CRT security cookie check kills process. No crash log. VEH **cannot** catch these (`__fastfail` bypasses all handlers). Check Windows Event Log: `Get-WinEvent -FilterHashtable @{LogName="Application"; Level=2}`.

Investigation status (2026-04-07):
- Camera auto-start code (`#if 0`'d) is first suspect — disabled for testing
- Need to systematically bisect xyz fork changes vs stock OrcaSlicer to find the trigger
- BambuCrashGuard (VEH) installed in `BBLNetworkPlugin::initialize()` — catches type 1 crashes
- Type 2 crashes require either process isolation or finding/fixing the root cause
- The DLL is loaded via `LoadLibrary` and called through function pointers (`BBLNetworkPlugin`)
- The DLL creates its own background threads (MQTT, SSDP) — we don't control them

Investigation strategy:
1. Disable xyz features one at a time to find the trigger
2. Compare our `BBLNetworkPlugin`/`NetworkAgent` usage to stock OrcaSlicer
3. Log all function calls to the Bambu DLL to find correlation with crashes
4. Binary search: stash changes, test stock, re-apply changes incrementally
5. If all else fails: replace the DLL with open LAN-only implementation (~2000 lines)

**Current test files:**
- `C:\Users\brian\Desktop\Crystal Dragon Statue - Spryo.3mf` — multi-material Spyro statue, good for testing paint tools, boundary painter, flushing volumes, tree supports
- `Z:\cabinets\Things\Projects\drg_buff_beer_mugs_coloration_2.3mf` (plate 2) — multi-material model with spatially isolated regions

### CLI Headless Slicing (for testing xyz fork features)
OrcaSlicer supports headless CLI slicing:
```bash
build/OrcaSlicer/orca-slicer.exe --slice 2 "path\to\model.3mf"
```

## Architecture

### Core Libraries
- **libslic3r/**: Core slicing engine and algorithms (platform-independent)
  - Main slicing logic, geometry processing, G-code generation
  - Key classes: Print, PrintObject, Layer, GCode, Config
  - Modular design with specialized subdirectories:
    - `GCode/` - G-code generation, cooling, pressure equalization, thumbnails
    - `Fill/` - Infill pattern implementations (gyroid, honeycomb, lightning, etc.)
    - `Support/` - Tree supports and traditional support generation
    - `Geometry/` - Advanced geometry operations, Voronoi diagrams, medial axis
    - `Format/` - File I/O for 3MF, AMF, STL, OBJ, STEP formats
    - `SLA/` - SLA-specific print processing and support generation
    - `Arachne/` - Advanced wall generation using skeletal trapezoidation

- **src/slic3r/**: Main application framework and GUI
  - GUI application built with wxWidgets
  - Integration between libslic3r core and user interface
  - Located in `src/slic3r/GUI/` (not shown in this directory but exists)

### Two GCode Paths (Slicer vs. Viewer)
OrcaSlicer has **two separate GCode processing paths** that produce independent `GCodeProcessorResult` objects. This is critical to understand when adding features that need data visible in the preview:

1. **Slicer path** (`GCode::_do_export()` in `GCode.cpp`): Generates the gcode file and runs its own `GCodeProcessor` to parse it. The result is stored via `m_processor.result()` and extracted with `extract_result()` into the partplate system. This is the path that feeds the **GUI preview** (GCodeViewer).

2. **CLI/file path** (`GCodeProcessor::process_file()` in `GCodeProcessor.cpp`): Parses an existing gcode file from disk. Used by CLI headless slicing and when loading external gcode files. This is a **separate** GCodeProcessor instance with its own result.

**Key implication**: Custom gcode comments (like `; LOOKAHEAD_EXCLUSION_ZONE`) parsed by `process_tags()` in the GCodeProcessor will only appear in whichever path actually runs. The GUI slicing path does NOT re-parse the temp gcode file for the viewer — it uses `extract_result()` directly from the slicer's processor. So if you need data in the preview, you must either:
- Populate it directly into `m_processor.result()` before `extract_result()` is called in `_do_export()` (preferred for slicer-computed data)
- Or ensure the comment parsing runs in the slicer's GCodeProcessor (works for data derived from gcode content)

The `GCodeViewer` (`src/slic3r/GUI/GCodeViewer.cpp`) receives its data from the partplate's `GCodeProcessorResult` — which came from path #1 above. It uses `libvgcode::Viewer` for toolpath rendering and custom `GLModel` objects for overlay geometry.

### Key Algorithmic Components
- **Arachne Wall Generation**: Variable-width perimeter generation using skeletal trapezoidation
- **Tree Supports**: Organic support generation algorithm  
- **Lightning Infill**: Sparse infill optimization for internal structures
- **Adaptive Slicing**: Variable layer height based on geometry
- **Multi-material**: Multi-extruder and soluble support processing
- **G-code Post-processing**: Cooling, fan control, pressure advance, conflict checking

### File Format Support
- **3MF/BBS_3MF**: Native format with extensions for multi-material and metadata
- **STL**: Standard tessellation language for 3D models
- **AMF**: Additive Manufacturing Format with color/material support  
- **OBJ**: Wavefront OBJ with material definitions
- **STEP**: CAD format support for precise geometry
- **G-code**: Output format with extensive post-processing capabilities

### External Dependencies
- **Clipper2**: Advanced 2D polygon clipping and offsetting
- **libigl**: Computational geometry library for mesh operations
- **TBB**: Intel Threading Building Blocks for parallelization
- **wxWidgets**: Cross-platform GUI framework
- **OpenGL**: 3D graphics rendering and visualization
- **CGAL**: Computational Geometry Algorithms Library (selective use)
- **OpenVDB**: Volumetric data structures for advanced operations
- **Eigen**: Linear algebra library for mathematical operations

## File Organization

### Resources and Configuration
- `resources/profiles/` - Printer and material profiles organized by manufacturer
- `resources/printers/` - Printer-specific configurations and G-code templates  
- `resources/images/` - UI icons, logos, calibration images
- `resources/calib/` - Calibration test patterns and data
- `resources/handy_models/` - Built-in test models (benchy, calibration cubes)

### Internationalization and Localization  
- `localization/i18n/` - Source translation files (.pot, .po)
- `resources/i18n/` - Runtime language resources
- Translation managed via `scripts/run_gettext.sh` / `scripts/run_gettext.bat`

### Platform-Specific Code
- `src/libslic3r/Platform.cpp` - Platform abstractions and utilities
- `src/libslic3r/MacUtils.mm` - macOS-specific utilities (Objective-C++)
- Windows-specific build scripts and configurations
- Linux distribution support scripts in `scripts/linux.d/`

### Build and Development Tools
- `cmake/modules/` - Custom CMake find modules and utilities
- `scripts/` - Python utilities for profile generation and validation  
- `tools/` - Windows build tools (gettext utilities)
- `deps/` - External dependency build configurations

## Development Workflow

### Code Style and Standards
- **C++17 standard** with selective C++20 features
- **Naming conventions**: PascalCase for classes, snake_case for functions/variables
- **Header guards**: Use `#pragma once` 
- **Memory management**: Prefer smart pointers, RAII patterns
- **Thread safety**: Use TBB for parallelization, be mindful of shared state

### Common Development Tasks

#### Adding New Print Settings
1. Define setting in `PrintConfig.cpp` with proper bounds and defaults
2. Add UI controls in appropriate GUI components  
3. Update serialization in config save/load
4. Add tooltips and help text for user guidance
5. Test with different printer profiles

#### Modifying Slicing Algorithms  
1. Core algorithms live in `libslic3r/` subdirectories
2. Performance-critical code should be profiled and optimized
3. Consider multi-threading implications (TBB integration)
4. Validate changes don't break existing profiles
5. Add regression tests where appropriate

#### GUI Development
1. GUI code resides in `src/slic3r/GUI/` (not visible in current tree)
2. Use existing wxWidgets patterns and custom controls
3. Support both light and dark themes
4. Consider DPI scaling on high-resolution displays
5. Maintain cross-platform compatibility

#### Adding Printer Support
1. Create JSON profile in `resources/profiles/[manufacturer].json`
2. Add printer-specific start/end G-code templates
3. Configure build volume, capabilities, and material compatibility
4. Test thoroughly with actual hardware when possible
5. Follow existing profile structure and naming conventions

### Dependencies and Build System
- **CMake-based** with separate dependency building phase
- **Dependencies** built once in `deps/build/`, then linked to main application  
- **Cross-platform** considerations important for all changes
- **Resource files** embedded at build time, platform-specific handling

### Performance Considerations
- **Slicing algorithms** are CPU-intensive, profile before optimizing
- **Memory usage** can be substantial with complex models
- **Multi-threading** extensively used via TBB
- **File I/O** optimized for large 3MF files with embedded textures
- **Real-time preview** requires efficient mesh processing

## Important Development Notes

### Codebase Navigation
- Use search tools extensively - codebase has 500k+ lines
- Key entry points: `src/OrcaSlicer.cpp` for application startup
- Core slicing: `libslic3r/Print.cpp` orchestrates the slicing pipeline
- Configuration: `PrintConfig.cpp` defines all print/printer/material settings

### Compatibility and Stability
- **Backward compatibility** maintained for project files and profiles
- **Cross-platform** support essential (Windows/macOS/Linux)  
- **File format** changes require careful version handling
- **Profile migrations** needed when settings change significantly

### Quality and Testing
- **Regression testing** important due to algorithm complexity
- **Performance benchmarks** help catch performance regressions
- **Memory leak** detection important for long-running GUI application
- **Cross-platform** testing required before releases

---

## xyz Fork: Local Development Environment

This section documents the local development environment, build tooling, and conventions for working on this OrcaSlicer fork (branch `xyz`).

### Development Environment

- **OS:** Windows 11 Pro
- **Shell:** Git Bash (default Claude Code shell on this system)
- **Python:** 3.14
- **Branch:** `xyz` (our feature branch, forked from `main`)

### Build Tooling

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

### Shell & Script Execution Rules

- **Use PowerShell for build operations.** Git Bash has issues invoking .bat files and cmake is not in its PATH. Write PowerShell scripts to a temp directory and execute them via:
  ```bash
  powershell.exe -ExecutionPolicy Bypass -File "C:/Users/brian/AppData/Local/Temp/orcabuild/scriptname.ps1"
  ```
- **Always prepend cmake to PATH** at the top of every PowerShell build script:
  ```powershell
  $env:PATH = "C:\Program Files\CMake\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM;" + $env:PATH
  ```
- **Temp script directory:** `C:\Users\brian\AppData\Local\Temp\orcabuild\`

### CPU Priority for Builds

**All CPU-intensive build steps must run at Idle priority** to keep the system responsive.

#### Invoke-IdlePriority helper function (copy into every build script)

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

#### Usage example

```powershell
Invoke-IdlePriority -Exe "cmake" -ArgString '../ -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release'
Invoke-IdlePriority -Exe "cmake" -ArgString '--build . --config Release --target deps -- -m'
```

### Build Process

The build has two phases:

#### Phase 1: Dependencies (one-time, ~30-60 min at normal priority, longer at idle)
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

#### Phase 2: Slicer (after deps, faster for incremental rebuilds)

Uses **Ninja** generator for faster incremental builds. Requires vcvars64 environment sourced first.

```
cmake -S <repo_root> -B <repo_root>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DORCA_TOOLS=ON -DDEP_BUILD_DIR=<repo_root>/deps/build
cmake --build build --config Release
```

**Must use `-S`/`-B` flags** for the configure step. Passing the source dir as a positional argument causes CMake to generate in-source instead of in `build/`.

**Must pass `-DDEP_BUILD_DIR`** pointing to `deps/build`. Without it, CMake infers the wrong path and can't find the built dependencies.

**Ninja vs Visual Studio generator:** Ninja has much faster incremental build detection (checks timestamps directly instead of going through MSBuild's project system). The deps build still uses the VS generator because OpenSSL's build system depends on nmake/msbuild internally.

**Selective /LTCG:** The final link targets (OrcaSlicer, OrcaSlicer_app_gui) use `/LTCG` in Release mode to prevent linker restarts from TBB's `/GL`-compiled objects. This is applied only to these targets, not globally, so incremental linking is preserved for library targets during development.

Output binary: `build/src/orca-slicer.exe` (raw build output, missing DLLs)

**Runnable installation:** `build/OrcaSlicer/orca-slicer.exe` (after install step - has all DLLs, resources, and WebView2). Always run from this location, not from `build/src/`.

### Build Policy

**Always use incremental builds for development iteration.** Full rebuilds are only needed when:
- Switching CMake generators (e.g., VS to Ninja)
- Build directory becomes corrupted
- First-time setup (deps + initial slicer build)

Never wipe the build directory just to rebuild after code changes. Ninja's incremental builds are fast and reliable.

### Build Artifacts Directory

`xyz/build_artifacts/` contains reusable build scripts and tooling for this fork. This directory is committed separately so it can be excluded when cherry-picking feature commits for upstream PRs.

- `build_all.ps1` - Full build script (deps + slicer + install + DLL copy). Only for first-time setup or generator changes.
- `build_incremental.ps1` - **Use this for development.** Builds changed files, installs, and copies DLLs.
  ```bash
  powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
  ```
  Both scripts auto-detect the repo root relative to their own location.

### Git Workflow

- Feature branch: `xyz` (off `main`)
- Clean, atomic commits suitable for upstream PR submission
- One feature at a time, in order listed in GOALS.md

### Git Remotes

- **origin** - upstream OrcaSlicer (SoftFever/OrcaSlicer on GitHub)
- **internal** - `git@git.internal.hanson.xyz:brianhansonxyz/orcaslicer_mod.git` - our internal repository for this fork. Push feature work here after committing.
- **Public fork:** https://github.com/hansonxyz/OrcaSlicer-xyz - public GitHub repo for releases and bug reports

### Auto-Update Mechanism

OrcaSlicer checks for updates via GitHub releases API. The update URL is configured in `src/slic3r/GUI/GUI_App.cpp` (search for `orca-update` or `github.com/SoftFever`). For our fork, this must point to `https://github.com/hansonxyz/OrcaSlicer-xyz/releases` so users of the fork get our releases, not upstream's. The update check uses the version string from `version.inc` to compare against release tags.
