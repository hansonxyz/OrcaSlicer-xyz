# OrcaSlicer Fork - Modification Goals

This fork implements targeted enhancements to OrcaSlicer. Each feature is developed as an isolated, clean commit suitable for upstream PR submission. Features are implemented one at a time in the order listed.

## Feature 1: Dynamic Support Base Material Selection ("Any Type")

**Problem:** When printing multi-material objects with raft/support base, the user must select a single filament for support base material. If that filament isn't used in early layers of the object, unnecessary filament changes occur on every layer just for the support base.

**Solution:** Add dynamic options to the support base material dropdown: "Any (Type)" entries (e.g., "Any PLA", "Any PETG", "Any ABS") for each unique filament type present in the project. When selected, the slicer automatically picks the lowest-cost filament of that type that is *already in use on each layer*, eliminating unnecessary filament changes.

**Key behaviors:**
- The dropdown appends "Any (Type)" options after the standard filament list, one per unique filament type defined in the project
- Per-layer selection: for each layer, choose the filament of the matching type that is already being used on that layer
- Tie-breaking: use the lowest-cost filament among candidates already active on that layer
- If no filament of the matching type is used on a given layer, fall back to whichever filament of that type has the lowest cost overall
- The generator must use the specific hotend settings, speed settings, and temperature for whichever filament is actually selected on each layer (not one-size-fits-all)
- This works for any filament type, not just PLA - if the project uses multiple PETG filaments, "Any PETG" should appear

**Example:** Object uses PLA-A, PLA-B, PLA-C, and PVA. User selects "Any PLA" for support base. Layers 1-20 only use PLA-B for the object, so support base uses PLA-B. Layer 21 uses PLA-A and PLA-C, support base uses whichever costs less. Layer 30+ only uses PLA-C, so support base uses PLA-C.

## Feature 2: Configurable Basic/Advanced Process Settings Visibility

**Problem:** The basic vs. advanced toggle for process settings shows a fixed set of fields. Users may want to customize which fields appear in the basic (non-advanced) view.

**Solution:** Allow an optional config file that defines which process settings appear in the "basic" view. When this file is present at startup, only the fields listed in it are shown when the advanced toggle is off.

**Key behaviors:**
- File is optional - if absent, default behavior is unchanged
- Only affects the process settings panel's basic/advanced toggle
- INI-style format with an `[options]` section and a `[fields]` section
- Fields are listed by their internal config key name, one per line
- Fields remain in their original category groups (Quality, Strength, Support, etc.); groups with no visible basic fields are hidden automatically
- Future: `flatten = true` option to show all fields in a single group (not yet implemented)

## Feature 3: Preserve Filament Colors on Printer Switch

**Problem:** When switching printer profiles (e.g., A1M 0.4mm to X1C 0.6mm) on a multi-material project, OrcaSlicer remaps filaments but loses the color assignments. The user has to manually reassign colors.

**Solution:** When switching printers, preserve the color mapping so that filament slot 1 retains its color (e.g., red stays red) even though the filament type may change. The filament type is expected to change (different printer = different filament catalog), but visual color identity should be maintained.

## Future Goal: Filament Order Optimization (not yet implementing)

Wishlist item for after Features 1-3 are complete. Optimize the order filaments are printed within each layer to minimize expensive filament usage in support/raft. Prioritize expensive filaments as the first or last filament used on a layer so that filament changes to/from them align with layers where they're already needed for the object, reducing wasted purge material.

## Key Files for Modifications
- `src/libslic3r/PrintConfig.cpp` - All print/printer/material setting definitions
- `src/libslic3r/Print.cpp` - Slicing pipeline orchestration
- `src/libslic3r/GCode/` - G-code generation (Feature 1: per-layer material selection)
- `src/libslic3r/Support/` - Support generation (Feature 1: support base material)
- `src/slic3r/GUI/` - GUI components (Features 1, 2, 3: dropdowns, toggles, color mapping)

## Future Goal: Arc Travel Moves to Avoid Printed Objects (not yet implementing)

Wishlist item. Add an optional travel move behavior: when the toolhead travels from point A to point B between layers or within a layer, if the straight-line travel path would exit the current object's infill, cross over its walls, and then pass through another printed object on the plate before reaching the destination, the toolhead should instead arc around the obstructing object rather than traveling straight through it. This avoids dragging ooze/strings across the surface of already-printed objects, reducing surface defects on multi-object prints. The feature would be an opt-in setting (e.g., "Avoid printed objects during travel").

**Research needed:** OrcaSlicer already has some non-trivial travel move behavior - investigate what the existing "avoid crossing perimeters" logic actually does before implementing. The existing behavior may be about keeping the toolhead *within* the current object's boundary during travel (so it doesn't cross its own walls and leave strings on the outside), which is a different problem than avoiding travel paths that unnecessarily cross *other* printed objects on the plate. Clarify the distinction and determine what, if anything, already exists for the inter-object case before designing a solution.

## Future Goal: Color-Aware Flush Into Infill Toggle (not yet implementing)

Wishlist item. Currently, "flush into infill" is a simple all-or-nothing boolean for the entire print. When enabled, purge material from filament changes is deposited into infill regions to reduce waste. However, when printing with bright, white, or translucent filaments, colored purge material can bleed through thin walls and discolor the surface.

**Goal:** Add a per-filament or color-aware toggle so that flush-into-infill is automatically disabled for filaments whose color is close to white or very light (e.g., >60% toward white in luminance). This way, dark filament purge material won't be flushed into infill behind thin walls of a white/light part, but flush-into-infill remains active for darker filaments where it doesn't cause visible defects.

**Key considerations:**
- This decision must happen at slice time during toolpath generation, not at gcode execution time
- By the time gcode runs, purge-into-infill moves are indistinguishable from regular extrusion
- Could be implemented as a per-filament override (checkbox per filament: "allow flush into infill") and/or an automatic heuristic based on filament color luminance
- The per-filament approach is simpler and more predictable; the automatic heuristic is a nice-to-have on top

## Future Goal: Multi-Material Purge Calibration Tool (not yet implementing)

Wishlist item. BBL printer compatibility only (for now). A built-in calibration workflow to empirically determine the minimum purge volume needed for each filament-to-filament color transition.

**How it works:**
1. User initiates a purge calibration print from within OrcaSlicer with their selected filaments
2. The slicer generates a calibration grid where each cell represents a transition from filament A to filament B
3. Purge amounts are reduced to near-zero and the purge tower is skipped - instead, each grid rectangle is the purge itself, extruded as a flat graduated strip showing the color transition
4. Each rectangle starts extruding filament B immediately after filament A, so the strip shows the gradual transition from one color to the other over its height
5. After printing, the user measures each rectangle with a millimeter ruler to determine the height at which the color has fully transitioned (i.e., no more trace of the previous color)
6. The user inputs these measurements back into OrcaSlicer
7. OrcaSlicer calculates the corresponding purge volumes from the measured heights (based on nozzle diameter, extrusion width, and flow rate used during calibration) and populates the filament purge amount table

**Key considerations:**
- For N filaments there are N*(N-1) transitions (A->B is different from B->A since dark-to-light needs more purge than light-to-dark)
- The calibration print should label each grid cell so the user can identify which transition it represents
- Should account for nozzle size and extrusion width when converting mm of height to volume
- Results populate the existing purge volume matrix in OrcaSlicer's filament settings

## Future Goal: Color-Bounded Bucket Fill in Paint Mode (not yet implementing)

Wishlist item. Add a new paint tool variant: **"Bounded Fill"** (separate tool icon alongside the existing bucket fill). When selected, the bucket fill only floods contiguous triangles that share the **same color** as the clicked triangle, stopping at boundaries where triangles are painted a different color.

**How it works:**
1. User manually paints a border of triangles in the target color (e.g., red) to define the edge of a region
2. User selects the Bounded Fill tool, picks the target color (red), and clicks on an unpainted/differently-colored area adjacent to the border
3. The fill floods all contiguous same-colored triangles outward from the click point, stopping when it hits triangles of a different color (the red border)
4. Result: the enclosed region is filled with the target color without bleeding through the painted border

**Key behaviors:**
- Operates on raw triangle adjacency (no curve/angle calculations) - simply floods contiguous triangles matching the source color
- Separate tool icon from the standard bucket fill (which fills based on angle/overhang thresholds)
- Respects all existing painted colors as boundaries, not just the target color
- Should be fast since it's a simple flood fill on the triangle adjacency graph

**Use case:** Precisely painting multi-material regions on complex models where the angle-based bucket fill is too aggressive or imprecise. The user draws the border manually, then fills the interior in one click.

## Future Goal: Display Resolved Values for Percentage-Based Settings (not yet implementing)

Wishlist item. When any setting is expressed as a percentage or relative value (e.g., line widths as "105%" of nozzle diameter, layer heights as percentages), display the calculated absolute value in real units (mm) next to the field in the properties panel. Currently the user sees "105%" but has to mentally calculate what that resolves to for their nozzle size. This applies to line widths, layer heights, and any other percentage-based settings throughout the UI.

## Future Goal: Draw-to-Place Support Blockers and Primitives (not yet implementing)

Wishlist item. Allow placing support blockers (and other cube-like primitives) by drawing directly on the build surface instead of the current workflow of adding a cube modifier and manually positioning/scaling it. The workflow would be:
1. Enter the tool mode (e.g., "Draw Support Blocker")
2. The model becomes semi-transparent so the user can see through it to the build plate
3. Click and drag on the build surface to define a rectangle footprint
4. Release to set the footprint, then click again (or drag vertically) to set the height
5. The primitive is placed as a modifier volume with the appropriate type (support blocker, enforcer, etc.)

This is much faster than the current add-cube-then-move-and-scale workflow for simple rectangular blockers.

## Future Goal: Ctrl-Hover Selection Highlight (not yet implementing)

Wishlist item. When holding down the Ctrl key, objects under the mouse cursor get a light green glow/outline to indicate they would be added to the selection on click. This provides visual feedback before clicking, making multi-select operations more precise - the user can see exactly which object will be selected before committing to the click.

## Active Feature: Filament Lookahead — Multi-Layer Batched Printing for Multi-Material

Print multiple consecutive layers of a single filament's spatially-isolated region before switching filaments, eliminating tool changes and reducing purge. Targets MMU/AMS single-nozzle printers (X1C, A1, P-series, etc.). Lives on the `filament-lookahead` branch.

**See [FILAMENT_LOOKAHEAD.md](FILAMENT_LOOKAHEAD.md) for the complete feature specification** — terminology, rules, algorithm phases, design decisions, implementation status, and progress log.

## Future Goal: GUI Editor for Basic Settings Field List (not yet implementing)

Wishlist item. Add a dialog accessible from the process settings tab (e.g., a gear icon next to the basic/advanced toggle) that lets the user visually select which fields appear in basic mode. This replaces the need to manually edit `basic_settings.cfg`. The dialog would show a checklist of all available process settings grouped by category, with checkboxes to include/exclude each one from the basic view. Changes would be saved back to `basic_settings.cfg` automatically.

## Future Goal: Custom Fork Branding/Logo (not yet implementing)

Wishlist item. Differentiate the fork visually from stock OrcaSlicer with a modified logo or splash screen, so it's immediately obvious which version is running. TBD on the specific design - could be a subtle color shift, a small badge overlay, or a different splash image.

## Future Goal (Low Priority): CLI Enhancements and Shell Integration (not yet implementing)

Wishlist item. Low priority - may not implement.

**Enhanced CLI mode**: Make OrcaSlicer more like ImageMagick for slicing - fully scriptable headless slicing, sending to printer, and batch operations via command line.

**Right-click-to-print**: Windows Explorer shell extension that adds "Print with OrcaSlicer" to the right-click context menu for .3mf files. Launches OrcaSlicer, lets the user select printer and send, then exits. Would require a Windows registry-based context menu entry pointing to the slicer exe with appropriate arguments.

## Future Goal: Live Print Progress 3D Visualization (not yet implementing)

Wishlist item. Replace the home/XYZ/bed controls on the Device tab with a live 3D render of the model being printed, showing layers up to the current reported layer as opaque and remaining layers as semi-transparent. Uses the existing Preview renderer (GLCanvas3D + GCodeProcessorResult) embedded in the Device tab, locked to the printer's reported layer number via MQTT.

**Requirements:**
- Only works when the printed model matches the open project (match by filename)
- Requires the model to be sliced (gcode result in memory)
- Falls back to a placeholder message when conditions aren't met
- Read-only 3D view (rotate/zoom only, no editing)
- Updates only when layer changes (not continuously) to manage performance
- Camera stream and 3D preview coexist side-by-side

**Implementation approach (Option A — recommended):**
1. Embed a GLCanvas3D in the Device tab status panel
2. Feed it the current plate's GCodeProcessorResult
3. Set layer slider to printer's reported layer
4. Update on MQTT layer-change events

**Alternatives considered:**
- Option B: Thumbnail with progress line (simpler, no model dependency)
- Option C: Static 3D model with Z-cutoff shading (simpler than full gcode render)

**Challenges:** Window visibility pause/resume for camera (wxEVT_ACTIVATE/wxEVT_ICONIZE unreliable on Windows — needs further investigation for future implementation).

## Development Workflow
1. Each feature gets its own branch off `main`
2. Clean, atomic commits suitable for upstream PR submission
3. Build and test after each change
4. One feature at a time, in order listed above
