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

**Key findings from gcode analysis (2026-03-30):**
- Since all strips use the same dummy filament, the slicer generates NO mid-print filament changes between strips
- Object markers exist: `; start printing object, unique label id: XXX` and `; stop printing object calib_X_Y id:...`
- Filament changes must be INSERTED (not replaced) before each `calib_X_Y` object
- The `change_filament_gcode` template is BBL-specific with `M620`/`T`/`M621` AMS commands
- Key flush parameters: `flush_length_1` through `flush_length_4` control purge passes
- The `M620.10 A1 F... L[flush_length]` command sets the total flush amount

**Implementation approach:**
1. Read the `change_filament_gcode` template from the printer profile
2. Use OrcaSlicer's PlaceholderParser to evaluate the template with custom parameters
3. For each `calib_X_Y` strip object marker in the gcode:
   a. If filament A needs loading (first strip, or A differs from previous B):
      - Insert evaluated change_filament_gcode: previous → A, default flush
   b. Insert evaluated change_filament_gcode: A → B, 50mm³ flush (set flush_length_1=50, others=0)
4. The strip's infill printing follows naturally after the change

**Alternative simpler approach (V1):**
For BBL printers, construct the filament change block directly using M620/T/M621 commands
with hardcoded flush amounts, bypassing template evaluation. Less general but faster to implement.

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

**Single grouped object:** All calibration geometry (base, strips, notches, labels) is created as volumes within ONE ModelObject. This allows the user to delete the entire calibration as a single item.

**Rectangle mesh:** Simple `indexed_triangle_set` cube. Extruded to layer_height.

**Ruler notches:** Thin rectangles at calculated positions along each strip.

**Label text:** LCD calculator-style 7-segment stroke font (CalibrationStrokeFont) with diamond-cut parallelogram ends. Shows filament numbers with an arrow mesh between them (e.g., "1 → 3"). Arrow is a simple triangle mesh, not a digit character.

**Base layer:** Must extend to cover the label area below the strips. Labels are positioned below strips and must sit on top of the base, not float in air.

### Strip Sizing

For a given flush volume V (mm³), the strip must contain exactly V mm³ of extruded material. The actual volume deposited by the slicer depends on:
```
volume_per_mm_length = extrusion_width × layer_height × (strip_width / line_spacing)
```

For solid infill with line_spacing ≈ extrusion_width:
```
volume_per_mm_length ≈ layer_height × strip_width
strip_length = V / volume_per_mm_length
```

IMPORTANT: Verify strip sizes against the slicer's actual fill output. The formula assumes 100% solid fill — real extrusion may have gaps, overlap, or perimeter effects that change the effective volume per mm. If strips appear oversized, adjust the formula with a calibration factor.

The strip starts at the "50mm³" point (that much was already purged to waste). The ruler's "0" mark = 50mm³ actual. Each subsequent 20mm³ notch represents additional purge.

### Print Direction Requirements

**Fill pattern:** Strips must use rectilinear fill along the X axis (not diagonal monotonic). Set per-object overrides:
- `top_surface_pattern = rectilinear`
- `infill_direction = 0` (X-axis aligned lines)

**Print order within strips:** Lines should be printed from Y=0 to Y=max so the bottom of each strip (the contaminated end) is printed first and the clean end is printed last. This ensures the color transition gradient is accurate.
- Phase 1: Set rectilinear + 0° direction via per-object config (gets ~90% correct, slicer may vary line start)
- Phase 2: Post-process gcode to guarantee Y-ascending fill order within each strip
- Gcode generation phase: pass a flag to the slicer/gcode generator that this is a calibration print — all print operations should start as close to X=0 and Y=0 as possible for every object

### Object Management

All calibration geometry is created as separate ModelObjects (for distinct gcode labeling with `calib_X_Y` markers). The objects remain separate so the gcode post-processor can identify each strip. Future improvement: group objects visually in the object list so the user can delete them as one unit.

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
