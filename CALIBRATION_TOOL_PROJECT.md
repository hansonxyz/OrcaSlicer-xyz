# Purge Calibration Tool — Design & Implementation Plan

## Problem

Multi-material prints require purging old filament when switching colors. The purge volume matrix in OrcaSlicer uses defaults that are often too high (wasting material and time) or too low (causing color contamination). Users currently guess or manually tune through trial and error.

## Solution

A built-in calibration workflow: generate a test print using the slicer's own pipeline, post-process the gcode with custom filament changes, preview it, print it, measure results, and auto-populate the purge volume matrix.

## UX Flow

### 1. Initiate Calibration

- User goes to **Flushing Volumes** dialog
- Clicks **"Calibration Test Print"** button (WebView, next to Re-Calculate)
- Opens calibration setup dialog (WebView, matching flushing volumes styling)

### 2. Setup Dialog (Phase 0 — DONE)

- Filament group dropdown: "PLA", "PLA + PVA", etc.
- Base layers count + base filament selection
- Label filament selection
- NxN checkbox grid showing OrcaSlicer's recommended flush volumes per pair
- Select All / Deselect All
- Generate Test Print button

### 3. Generate & Preview (Phase 1)

See detailed implementation plan below.

### 4. Measurement Input (Phase 2 — future)

- Grid matching printed layout with mm input per cell
- Calculate button converts measurements to volumes
- Apply button writes to flush matrix

## Phase 1: Detailed Implementation Plan

### Overview

Use OrcaSlicer's own slicer to generate gcode, then post-process it to inject custom filament changes between calibration strips. This leverages the slicer for all the hard stuff (extrusion math, temperatures, retraction, printer-specific gcode) and only customizes the filament transitions.

### Step-by-step

#### 1. Save state
- Save any settings we'll modify so we can restore them after gcode generation

#### 2. Create temporary plate
- Add a new plate to the current project
- Set it as the active plate for slicing

#### 3. Generate calibration geometry as mesh objects

**Base layer (optional):**
- Simple rectangle covering the grid area + 10mm margin on each side
- Assigned to the user-selected base filament
- Height = base_layers × layer_height

**Label/notch objects:**
- Text meshes for filament numbers (row/column headers)
- Ruler notch marks (thin rectangles every 20mm³ equivalent, larger at 100mm³)
- All assigned to the **label filament** selected by the user
- Height = 1 layer (single layer on top of base)

**Calibration strip rectangles:**
- One rectangle per selected transition pair
- ALL assigned to the **same filament** (any filament that is NOT the label filament)
- This forces the slicer to print all labels as one filament group, then all strips as a second group
- Strip dimensions: width and length calculated so total fill volume = recommended flush amount for that pair
- Height = 1 layer (on top of base + label layer)

**Per-object speed override on strips:**
- Each strip object gets a config override: all print speeds set to 20mm/s
- Uses OrcaSlicer's existing per-object settings mechanism (modifier config)
- The slicer handles acceleration/jerk planning correctly

**Grid layout:**
- Arrange strips in an NxN grid pattern on the bed
- Row = source filament (A), Column = target filament (B)
- Diagonal cells empty (no self-transition)
- Ruler notches alongside each strip

#### 4. Slice settings overrides
- Disable prime tower on this plate
- Layer height = 125% of nozzle width (for both base and calibration layers)
- Enable object labeling (for gcode markers: `; start printing object, unique label id: calib_X_Y`)
- Flush volumes: leave at default calculated values (unchanged)
- All other settings: use user's current printer/filament/process profiles

#### 5. Slice the temporary plate
- Run the slicer on the temp plate
- Produces gcode with proper extrusion, temperatures, start/end gcode, etc.

#### 6. Post-process gcode

**Find the label→strip filament change:**
The slicer will emit a filament change from the label filament to the strip filament. Find this transition and **remove it**.

**Replace with calibration sequence:**
For each calibration strip (identified by `calib_X_Y` markers in gcode):

1. **If filament A is not already loaded** (first strip, or A differs from previous B):
   - Inject full standard filament change gcode to filament A
   - Uses the printer profile's `change_filament_gcode` template
   - Default purge amount (to waste chute / poop chute)

2. **Switch from A to B:**
   - Inject filament change gcode from A to B
   - Only 50mm³ purge to waste chute
   - This primes the nozzle with filament B

3. **The slicer's existing strip extrusion prints normally:**
   - The first material out of the nozzle still has A contamination
   - As more B material is extruded, the strip becomes cleaner
   - The color transition gradient IS the calibration data

4. **If the next strip's filament A = current filament B:**
   - Skip the full A-load step (B is already loaded)
   - Just inject A→B change with 50mm³ purge for the next pair

**Transition ordering:**
- Order the strips to minimize full filament changes
- Greedy: after printing A→B, look for a pair where the new A = current B
- If none found, do a full change to the next needed A

#### 7. Load into preview
- Write post-processed gcode to a temp file
- Load it into OrcaSlicer's gcode preview viewer
- User can inspect layer-by-layer, rotate, zoom

#### 8. Restore state
- Delete the temporary plate
- Restore any modified settings
- User's project is untouched

### Architecture

```
PurgeCalibrationGenerator (new class, src/slic3r/GUI/PurgeCalibrationGenerator.hpp/cpp)
├── generate()
│   ├── save_state()
│   ├── create_temp_plate()
│   ├── add_geometry() — base, labels, strips as indexed_triangle_set meshes
│   ├── configure_objects() — filament assignments, speed overrides, object names
│   ├── slice() — invoke slicer on temp plate
│   ├── postprocess_gcode() — inject custom filament changes
│   ├── load_preview() — feed gcode to viewer
│   └── restore_state()
├── Input: selected pairs, filament configs, printer config, options
└── Output: preview loaded in viewer, gcode ready to send/save
```

**Hooks into existing code (minimal):**
- `WipeTowerDialog.html` — "Calibration Test Print" button (DONE)
- `WipeTowerDialog.cpp` — message handler for `openCalibration` (DONE)
- `PurgeCalibrationDialog.hpp/cpp` — setup wizard UI (DONE)
- `PurgeCalibrationGenerator.hpp/cpp` — NEW, self-contained generator class
- Possibly: small hook to load gcode into preview from code (may already exist)

**Files NOT modified:**
- TriangleSelector, GCode.cpp, PrintConfig, or any core slicer files
- The generator uses public APIs to create objects, set configs, and invoke slicing

### Geometry Generation

**Rectangle mesh:** Simple `indexed_triangle_set` with 2 triangles (quad). Extruded to layer_height. Trivial to generate programmatically.

**Ruler notches:** Thin rectangles (0.5mm wide) at calculated positions along each strip. Same simple mesh generation.

**Text meshes (Phase 3):** Font → SVG → mesh conversion. For Phase 1, use simple line-segment digit rendering or numbered markers. Full text generation is a separate sub-project.

### Strip Sizing

For a given flush volume V (mm³), nozzle width W, and layer height H:
```
strip_fill_area = V / H
strip_length = strip_fill_area / strip_width
```

Where strip_width is chosen to fit the grid layout on the bed (e.g., 15-20mm per strip).

The strip starts at the "50mm³" point (that much was already purged to waste). The ruler's "0" mark = 50mm³ actual. Each subsequent 20mm³ notch represents additional purge. The user measures where the color is clean and reads the total from the ruler (add 50 to get actual purge volume needed).

### Flush Volume Calculation from Measurement

Given user measurement M (mm on the ruler):
```
actual_purge_mm3 = 50 + (M × mm3_per_mm_of_strip)
mm3_per_mm_of_strip = strip_width × layer_height × (extrusion_width / line_spacing)
```

This is computed per-strip since strip dimensions may vary.

## Implementation Phases

### Phase 0: UI Entry Point & Setup Dialog — DONE
- WebView button in Flushing Volumes dialog
- WebView setup dialog with NxN checkbox grid
- Filament group filtering, base/label options

### Phase 1: Gcode Generator — NEXT
- `PurgeCalibrationGenerator` class
- Temp plate creation, geometry generation
- Slicing with overrides
- Gcode post-processing (filament change injection)
- Preview loading
- State save/restore

### Phase 2: Measurement Dialog
- WebView grid matching printed layout
- mm input per cell
- Volume calculation
- Apply to flush matrix

### Phase 3: Polish
- Text generation (SVG → mesh)
- Reference image alongside measurement dialog
- Transition ordering optimization
- Calibration history
