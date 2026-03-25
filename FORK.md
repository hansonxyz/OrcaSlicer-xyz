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
