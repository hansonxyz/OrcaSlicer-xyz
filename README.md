# OrcaSlicer-xyz

A fork of [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer) with customizations and quality-of-life improvements for multi-material 3D printing workflows.

**Version:** 2.3.2-xyz | **Base:** OrcaSlicer 2.3.2

## What is this?

OrcaSlicer-xyz is a personal fork that adds features focused on multi-material MMU painting, print workflow efficiency, and UI improvements. All changes are designed as clean, isolated commits suitable for potential upstream contribution.

## Features

### Multi-Material Painting

- **Boundary Painter Tool** (D key) — Draw polyline boundaries on the mesh surface that constrain fill operations. Click to place points connected by A* shortest-path segments along mesh edges. Close boundaries to create fill barriers. Supports:
  - Shift key for planar "straight line" mode (path follows a cutting plane defined by averaged surface normals)
  - Real-time preview line (blue for curved, red for planar)
  - Marching ants animation (yellow-black animated dashes) on completed boundaries
  - Ctrl+Z undo per segment
  - Close/Cancel/Clear buttons
  - Snap-to-sharp-edges mode with configurable curvature threshold
  - Boundary persistence (saved/restored with 3MF project files)

- **Unified Fill Tool** — The Fill tool (F key) now has three constraint checkboxes:
  - **Surface angle** — stop at angle changes (adjustable threshold)
  - **Color boundaries** — stop at painted color edges
  - **Boundary lines** — stop at boundary painter lines
  - Any combination works. Uncheck all to fill the entire object.

- **Edge-Snap Sphere Brush** — Sphere brush optionally snaps to sharp mesh edges with configurable angle threshold

- **Sharp Edge Boundary Preview** — Toggle to visualize sharp edges on the mesh while painting

### Dynamic Support Material

- **"Any (Type)" Support Base** — Support base filament dropdown includes "Any PLA", "Any PETG", etc. Dynamically selects the cheapest filament of that type already in use on each layer, eliminating unnecessary filament changes.

### Print Workflow

- **Prime Tower Early Stop** — Stops generating the prime tower on layers where no more filament changes remain above, saving material and time

- **Auto-Start Camera** — Automatically starts the camera stream when viewing a printer with an active print job, or after sending a print. Toggleable in Preferences.

- **Preserve Filament Colors on Printer Switch** — When changing printer profiles, filament color assignments are preserved so visual identity is maintained

### UI Improvements

- **Configurable Basic/Advanced Settings** — Optional config file defines which process settings appear in the basic (non-advanced) view

- **Shift+Middle Mouse Rotate** — Shift+middle drag rotates the view (middle drag remains pan)

- **--dump-config CLI** — Print resolved configuration to stdout for debugging and scripting

- **Custom Branding** — Fork-specific logo, version string "2.3.2-xyz", about dialog with fork attribution, update URL pointing to this repository

## Architecture

New code follows a modular pattern to minimize changes to existing OrcaSlicer files:

- **MeshPathFinder** (`src/libslic3r/MeshPathFinder.hpp/cpp`) — Standalone A* pathfinder on mesh vertex graphs. No GUI dependencies.
- **PaintToolBoundary** (`src/slic3r/GUI/Gizmos/PaintToolBoundary.hpp/cpp`) — Boundary storage, rendering, fill integration. Owns a MeshPathFinder per mesh.
- **TriangleSelector** changes are minimal: a `m_boundary_triangles` set and a `respect_color` parameter on `bucket_fill_select_triangles`.

## Building

See [CLAUDE.md](CLAUDE.md) for full build instructions. Quick start on Windows:

```bash
# Incremental build (after initial setup):
powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
```

Requires VS2022 Build Tools, CMake 3.31.x, Ninja, and dependencies built in `deps/build/`.

## Branch Structure

- **`xyz-main`** — Primary development branch with all features
- **`filament-lookahead`** — Experimental filament lookahead feature (parked)
- **`main`** — Upstream OrcaSlicer tracking branch

## Credits

This is a fork of **[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)** by SoftFever, which is itself based on [Bambu Studio](https://github.com/bambulab/BambuStudio) by Bambu Lab and [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research.

OrcaSlicer is licensed under the [GNU Affero General Public License, version 3](LICENSE.txt).
