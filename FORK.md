# OrcaSlicer Fork - Feature Documentation

This document describes the features added by this fork. Each feature is designed as a clean, isolated change suitable for upstream contribution.

---

## Feature 1: Dynamic Support Base Material Selection ("Any Type")

### Problem

When printing multi-material objects with support/raft, OrcaSlicer requires selecting a single specific filament for the support base material. If that filament isn't used for the object on many layers, the printer performs unnecessary filament changes on every one of those layers just to print the support base — wasting time, purge material, and increasing wear.

For example: a project uses PLA-A, PLA-B, PLA-C, and PVA. The user selects PLA-A as the support base filament. But for the first 20 layers, only PLA-B is used for the object. The printer must switch from PLA-B to PLA-A and back on every layer just for the support base, even though PLA-B would work just as well.

### Solution

The support base filament dropdown now includes **"Any (Type)"** entries — for example, "Any PLA" — that dynamically select the lowest-cost filament of that type already in use on each layer. This eliminates unnecessary filament changes by reusing whatever compatible filament is already loaded in the nozzle.

### How It Works

**Dropdown entries:** After the standard filament list, the dropdown shows "Any PLA", "Any PETG", etc. for each non-soluble filament type present in the project. Soluble types (PVA, BVOH) are excluded since they are support interface materials, not support base candidates.

**Per-layer resolution:** During slicing, for each layer:
1. The slicer identifies which filaments of the matching type are already being used for object material on that layer
2. Among those candidates, it picks the one with the lowest filament cost (as defined in the filament preset's Price field)
3. If no filament of the matching type is active on a given layer, it falls back to the cheapest filament of that type overall

**Filament-specific settings:** Because the resolved filament varies per layer, the slicer uses each filament's own temperature, speed, flow, and hotend settings — not a one-size-fits-all configuration.

**Persistent values:** The selection is encoded using a stable index into the MaterialType database (the fixed list of all known material types in OrcaSlicer). This means "Any PLA" always maps to the same config value (1056) regardless of which filaments are loaded in the project. The value persists correctly through:
- Save/load of 3MF project files
- Process preset save/load
- Filament count changes (adding/removing filaments)
- Printer profile switches

**Validation:** If "Any PLA" is selected but no PLA filament exists in the project at slice time, an error message prompts the user to add a PLA filament or select a different support material.

### Usage

1. Set up a multi-material project with two or more filaments of the same type (e.g., multiple PLA filaments)
2. Go to **Process Settings → Support → Support filament**
3. Select **"Any PLA"** (or whichever type matches your project) from the dropdown
4. Slice as normal

The gcode preview will show different filaments used for support on different layers, matching whatever object filament is already active on each layer.

### Files Modified

- `src/libslic3r/PrintConfig.hpp` — Constants, helpers for encoding/decoding "Any (Type)" values
- `src/libslic3r/PrintConfig.cpp` — Resolution functions: `resolve_any_type_support_filament()`, `resolve_support_filament_for_nozzle()`, `support_filament_any_type_name()`, `support_filament_any_type_value_for_name()`
- `src/slic3r/GUI/Plater.cpp` — Dropdown population with "Any (Type)" entries, value mapping
- `src/libslic3r/GCode/ToolOrdering.cpp` — Per-layer resolution during tool ordering
- `src/libslic3r/GCode.cpp` — Per-layer resolution during G-code generation
- `src/libslic3r/Print.cpp` — Slice-time validation, extruder collection, raft handling
- `src/libslic3r/PrintObject.cpp` — Allow "Any (Type)" values past extruder clamping
- `src/libslic3r/PresetBundle.cpp` — Allow "Any (Type)" values past config normalization (3 sites)
- `src/libslic3r/Flow.cpp` — Nozzle diameter lookups for support flow calculations
- `src/libslic3r/Slicing.cpp` — Nozzle diameter lookups for slicing parameters
- `src/libslic3r/Support/SupportMaterial.cpp` — Extrusion width calculations
- `src/libslic3r/Support/SupportParameters.hpp` — Soluble interface detection
- `src/libslic3r/Support/TreeSupport.cpp` — Nozzle diameter for tree support toolpaths
- `src/slic3r/GUI/ConfigManipulation.cpp` — Filament count validation bypass
- `src/slic3r/GUI/GUI_ObjectList.cpp` — Object-level filament validation and removal handling
- `src/slic3r/GUI/LibVGCode/LibVGCodeWrapper.cpp` — Preview extruder resolution

---

## Feature 2: Configurable Basic/Advanced Process Settings Visibility

### Problem

OrcaSlicer's basic vs. advanced toggle for process settings shows a fixed set of fields determined by each setting's built-in mode tag (`comSimple`, `comAdvanced`, `comDevelop`). Users who generate their profiles algorithmically (e.g., via a profile generator script) have most settings locked down and only need to adjust a small, specific subset per project. The built-in basic mode shows too many irrelevant fields and hides some that are needed.

### Solution

An optional configuration file (`basic_settings.cfg`) placed in the OrcaSlicer data directory overrides which fields appear in the basic (non-advanced) view. When the file is present, only the listed fields are shown when the advanced toggle is off. If the file is absent, the default behavior is unchanged.

### How It Works

**File location:** `%APPDATA%\OrcaSlicer\basic_settings.cfg` (Windows) or the equivalent OrcaSlicer data directory on other platforms.

**File format:** INI-style with a `[fields]` section listing config key names, one per line. Comments start with `#` or `;`.

```ini
# Only these fields appear in basic mode.
# Remove this file to restore default behavior.

[fields]
seam_position
enable_support
support_type
support_filament
support_interface_filament
flush_into_infill
brim_type
ironing_type
wall_sequence
layer_height
initial_layer_print_height
wall_loops
top_shell_layers
bottom_shell_layers
interlocking_beam
sparse_infill_density
sparse_infill_pattern
```

**Behavior:**
- Loaded once at application startup
- Only affects the process settings panel when the basic/advanced toggle is set to basic mode
- Fields remain in their original category groups (Quality, Strength, Support, etc.)
- Category groups with no visible basic fields are hidden automatically
- In advanced mode, all fields are shown regardless of the config file
- The file is optional — if absent or empty, the default OrcaSlicer behavior applies

**Finding config key names:** The internal config key names correspond to the keys used in OrcaSlicer's process preset JSON files (e.g., `user/default/process/*.json`). They can also be found in `src/libslic3r/PrintConfig.cpp` where each setting is defined via `this->add("key_name", ...)`.

### Usage

1. Create `basic_settings.cfg` in your OrcaSlicer data directory
2. List the config keys you want visible in basic mode under `[fields]`
3. Restart OrcaSlicer
4. Toggle to basic mode — only your listed fields will be shown

To restore default behavior, delete or rename the file and restart OrcaSlicer.

### Files Modified

- `src/slic3r/GUI/BasicSettingsConfig.hpp` — New file: singleton config manager
- `src/slic3r/GUI/BasicSettingsConfig.cpp` — New file: config file parser
- `src/slic3r/GUI/OG_CustomCtrl.cpp` — Visibility override in basic mode
- `src/slic3r/GUI/GUI_App.cpp` — Load config at startup
- `src/slic3r/CMakeLists.txt` — Add new source files to build

---

## Feature 4: Paint Tool Improvements

### Bounded Fill (B key)

A new paint tool that floods contiguous triangles sharing the **same color** as the clicked triangle, stopping at boundaries where triangles are painted a different color. Unlike the existing Bucket Fill which also uses angle-based edge detection, Bounded Fill ignores angles entirely — only color boundaries matter. This is useful for painting a border of triangles by hand, then filling the enclosed region in one click.

### Edge-Snapping Sphere Brush

A "Snap to edges" checkbox on the Sphere brush tool. When enabled, the sphere brush center automatically snaps to the nearest high-curvature vertex (edge ridge) before painting. This makes brush strokes follow edge ridges naturally, making it much easier to paint along fillets, chamfers, and creases on complex models.

Parameters:
- **Snap to edges** checkbox — enables/disables the snapping behavior
- **Edge sensitivity** slider (5-90°) — controls what curvature counts as an "edge" (lower = more sensitive to subtle fillets)

The curvature detection uses per-vertex max dihedral angle, precomputed in O(F) time and cached.

### Sharp Edge Boundary Preview

A "Show edge boundaries" checkbox that renders orange lines at all detected sharp edges on the model. The detection uses the same angle threshold as the Smart Fill tool. This helps users visualize where fill boundaries will fall before clicking, and understand the mesh topology.

### Files Modified

- `src/libslic3r/TriangleSelector.hpp/cpp` — New algorithms: `compute_face_groups()`, `get_sharp_edges()`, `precompute_vertex_curvature()`, `snap_to_edge()`, `find_nearest_ridge_triangle()`, `select_edge_band_by_seed_fill()`, `select_group_by_seed_fill()`
- `src/slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp/cpp` — ToolType::BOUNDED_FILL, sharp edge contour rendering, edge-snap cursor modification
- `src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.hpp/cpp` — Tool UI, keyboard shortcuts, snap-to-edges checkbox
- `deps_src/imgui/imconfig.h` — Icon constants
- `src/slic3r/GUI/ImGuiWrapper.cpp` — Icon registration
- `resources/images/bounded_fill.svg` — Paint drop in box icon

---

## Feature 5: Smart Prime Tower (Early Stop)

### Problem

In multi-material prints, the prime tower (wipe tower) is printed on every layer from bottom to top, even when all filament changes occur only in the lower portion of the print. This wastes material and time printing unnecessary prime tower layers at the top where no tool changes happen.

### Solution

The prime tower now stops at the last layer that has a filament change. If no more tool changes exist above a given layer, the prime tower is omitted for that layer and all layers above it.

### How It Works

After the standard wipe tower partitioning logic runs, a final pass scans from top to bottom. For each layer, it checks whether any actual tool change (not just propagated partition counts) exists on that layer or any layer above. If none remain, the `has_wipe_tower` flag is cleared for that layer.

Exception: smooth timelapse mode retains the wipe tower on all layers regardless, since it uses the tower for consistent layer timing.

### Files Modified

- `src/libslic3r/GCode/ToolOrdering.cpp` — Added early-stop logic in `fill_wipe_tower_partitions()`

---

## Feature 6: Blender-Style Middle Mouse Rotation

### Problem

OrcaSlicer uses middle mouse drag for panning and left mouse drag for rotation. This differs from the convention in Blender and many other 3D tools where middle mouse drag rotates the view.

### Solution

Middle mouse button behavior is changed to match Blender's defaults:

- **Middle drag** = rotate the 3D view (was: pan)
- **Shift + middle drag** = pan the 3D view (was: middle drag)
- **Left drag** = still rotates when not interacting with objects (unchanged)
- **Right drag** = still pans (unchanged)

This applies to all 3D views: Prepare, Preview, and Paint mode.

### Files Modified

- `src/slic3r/GUI/GLCanvas3D.cpp` — Modified `is_camera_rotate()` and `is_camera_pan()`

---

## Fork Branding

- Version string: `2.3.2-xyz` with "xyz" displayed in orange (#fe7904)
- Custom logo: iOS-style rounded corners with shadow glow, generated from `logo-xyz-base.png` via `xyz/update_logo.ps1`
- About dialog: fork attribution paragraph with link to github.com/hansonxyz/OrcaSlicer-xyz
- Update URL: points to fork's GitHub releases
- Splash screen and about dialog use text-only SVGs with raster icon overlay
