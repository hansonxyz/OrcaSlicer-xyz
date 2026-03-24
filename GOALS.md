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

**Solution:** Allow an optional external file that defines which process settings appear in the "basic" view. When this file is present at startup, only the fields listed in it are shown when the advanced toggle is off.

**Key behaviors:**
- File is optional - if absent, default behavior is unchanged
- Only affects the process settings panel's basic/advanced toggle
- File format TBD (likely a simple list of setting keys, one per line)

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

## Development Workflow
1. Each feature gets its own branch off `main`
2. Clean, atomic commits suitable for upstream PR submission
3. Build and test after each change
4. One feature at a time, in order listed above
