# GCode Viewer — Filament Lookahead Sub-Layer Display

Branch: `filament-lookahead`
Status: Planning. Phase 1a not yet started.

## Goal

Make the gcode preview's right-side layer slider lookahead-aware so the user can scroll through emission order and watch each lookahead tower assemble stack-by-stack, then see normal-mode printing happen on top of the already-physical tower on layers above.

## User-facing behavior

The slider on the right of the gcode preview is currently a 1:1 map between tick positions and physical layer Z values. With this feature it becomes:

1. Layers without any tower-base render as today — one tick per physical layer.

2. Layers that ARE the base of a lookahead tower expand into a colored range. For a tower covering layers 50–60 (1 base + 10 upper stacks), the slider shows 11 ticks at the layer-50 position: one for the base layer's normal content + the tower's first stack, and 10 sub-ticks for the upper stacks. Sub-ticks are visually distinct (smaller, different color).

3. Past the tower, ticks resume normally for layers 51, 52, ... — but selecting one of those now shows BOTH the already-emitted tower content (drawn from the tower-base sub-ticks above) AND the layer's own normal-mode emission. The user watches the tower stay in place while normal content gets added on top.

End result: scrolling through the slider plays the physical print in emission order. Towers assemble in their batches at the base layer; normal content layers in afterward; travel moves between them are visible at the moment they happen.

## Research summary

Investigation done up front (committed as part of this plan). Findings:

### Layer slider widget
- `src/slic3r/GUI/IMSlider.hpp` line 55: class `IMSlider`.
- Driven by `std::vector<double> m_values` (line 223) — one Z per layer index.
- Populated from `GUI_Preview.cpp:564` via `SetSliderValues(m_viewer.get_layers_zs())`.
- `SetMaxValue(int)` (line 79) sets the index range.
- Already has a `DrawMode::dmSequentialGCodeView` (line 45) — emission-order rendering precedent.

### Renderer
- Two parallel gcode paths: internal `GCodeProcessor` and libvgcode `Viewer`. **Preview uses libvgcode.**
- `GCodeViewer::load_as_gcode()` (`src/slic3r/GUI/GCodeViewer.cpp:1070`) converts `GCodeProcessorResult::moves[]` to `libvgcode::PathVertex` array and hands to `m_viewer.load(...)`.
- libvgcode buckets vertices by `layer_id` into `Layers::m_items[layer_id]` (`src/slic3r/GUI/LibVGCode/src/Viewer/Layers.hpp:46–55`). **One bucket per physical layer.**
- `set_layers_view_range(min, max)` (`GCodeViewer.cpp:1774`) controls which layer buckets render.

### Lookahead marker parsing in `GCodeProcessor`
- `; Z_HEIGHT:` IS already parsed (`src/libslic3r/GCode/GCodeProcessor.cpp:3081`) but only updates `m_print_z` state.
- `LOOKAHEAD_BLOCK_BEGIN/END` are parsed only by the post-processor (`src/libslic3r/GCode/FilamentLookaheadPostProcessor.cpp:66, 80`); NOT propagated to per-move metadata in `GCodeProcessorResult::moves[]`.
- Means: any per-move tower tagging has to be added.

### `GCodeProcessorResult`
- Has a `result_mutex` for thread-safe access.
- Cached/serialized; new metadata must ride along.
- Many UI elements assume `layer_count == get_layers_zs().size()` — sub-tick expansion will need to audit those spots.

### Difficulty assessment
**MEDIUM.** Bucketing is per-physical-layer, but the `m_lookahead_zones_dirty` flag in `GCodeViewer` shows precedent for lazy-rebuild on viewer state changes. We add per-stack sub-bucketing parallel to `m_items[]`. Exposing or extending a libvgcode API for sub-buckets is the main code-quality risk.

### Dragons
1. `GCodeProcessorResult::result_mutex` — parser-vs-viewer sync. Manageable, similar pattern to existing.
2. Result is cached/serialized — tower metadata must be included.
3. UI elements scattered through the codebase assume `layer_count == m_values.size()` — time slider, legend, layer-stats panel. Each spot needs auditing on the sub-tick change.
4. Async gcode loading — slider must repopulate cleanly when load finishes; tower metadata rides along.
5. **libvgcode opaqueness** — internal layer bucketing may resist augmentation. Plan B: client-side culling (slower but simpler).

## Implementation phases

Each phase ends in a clean commit. Each must build, must not regress `compare_slices.py`, and must be independently verifiable.

### Phase 1a — Tower metadata in `GCodeProcessorResult`

**Code changes**
- `GCodeProcessor::process_tags()`: parse `LOOKAHEAD_BLOCK_BEGIN` and `LOOKAHEAD_BLOCK_END` comments alongside existing `Z_HEIGHT`.
- New struct `LookaheadTower` in `GCodeProcessorResult`: `{base_layer, base_z, top_z, stack_count, base_move_first, base_move_last_plus_one}` (vertex range deferred to 1b).
- Track in-progress tower during parsing; on END push complete record, log warning if mismatched.

**Verification**
- Temporary log line in build emits one line per parsed tower.
- Slice tulip (CLI). Expected output: 9 records with bases at layers 233/242/244/251/253/260/262/269/271; stack_count 8/8/8/8/8/8/8/4/8.
- Run `compare_slices.py`. Expected: 0 divergences.

**No UI surface area.**

### Phase 1b — Per-move tower tagging

**Code changes**
- `GCodeProcessorResult::MoveVertex` (or equivalent): add `int8_t tower_id = -1` and `int8_t stack_index = -1`.
- Inside a tower block, parser sets these on each emitted move.
- `Z_HEIGHT` increments inside a block determine `stack_index`: each `Z_HEIGHT: <upper_z>` raises stack_index from k to k+1.

**Verification**
- Histogram log: count moves per `(tower_id, stack_index)`. Expected per-tower distribution roughly proportional to extra_layers.
- `compare_slices.py`: 0 divergences.

**No UI surface area.**

### Phase 2 — Propagate tower tags into libvgcode

**Code changes**
- `libvgcode::PathVertex`: add `int8_t tower_id` and `int8_t stack_index` fields. Verify GPU-buffer alignment doesn't break.
- `GCodeViewer::load_as_gcode()`: copy these fields when converting `GCodeProcessorResult::moves[]` to `PathVertex` array.
- Temporary debug feature: a build-time flag that colors tower vertices red.

**Verification**
- Build the slicer, load tulip in preview, enable the debug flag. Towers visibly tinted red.
- `compare_slices.py`: 0 divergences (no slicer-side changes).

### Phase 3 — Slider tick coloring (no sub-ticks)

**Code changes**
- `IMSlider`: read `LookaheadTower` records from the gcode result.
- `IMSlider::draw_ticks()`: tint ticks at tower base layers.

**Verification**
- Load tulip in preview. Ticks at layers 233/242/244/251/253/260/262/269/271 visibly orange (or whatever color we pick).
- Selecting an orange tick still shows the full physical layer (no semantic change yet).
- `compare_slices.py`: 0 divergences.

### Phase 4 — Sub-tick expansion in the slider model

**Code changes**
- Change `IMSlider`'s value model: `m_values` becomes a `std::vector<SliderEntry>` where each entry is `{layer_idx, stack_idx_or_-1, z}`.
- Tower base layer 251 (base + 8 stacks) becomes 9 entries in the model.
- `SetSliderValues(...)` accepts the new structure.
- `GUI_Preview.cpp`: build the structured list from `m_viewer.get_layers_zs()` + tower records.
- Tooltip / readout: `Layer 251 stack 3/8 (z=51.0)`.

**Verification**
- Load tulip; the slider has more positions than physical layer count (tulip has 9 towers contributing extras — net ~+72 sub-ticks). Tooltip shows correct stack info.
- Viewer rendering still unchanged — the new sub-ticks all show the same scene as the base layer (deliberately inert). `compare_slices.py`: still 0.

### Phase 5 — Wire viewer filter to sub-ticks (progressive build)

**Code changes**
- New API on libvgcode `Viewer`: `set_layers_sub_view_range(layer_min, layer_max, in_progress_tower_id, in_progress_stack)`.
- libvgcode side: when filtering vertices, layer 251 vertices are visible iff `tower_id == -1` (normal content) OR `tower_id == TID && stack_index <= K` (in-progress tower up to current sub-tick).
- For layers > 251 with already-printed towers: render all stacks of past towers + normal content for the current layer.
- `GCodeViewer::set_layers_z_range()` reroutes through the new sub-view API when current slider position is on a sub-tick.

**Verification**
- Load tulip. At slider position `{layer=251, stack=0}`: only the cluster's base content visible inside the LA block. At `stack=3`: 3 stacks visible. At `stack=8`: full tower. At slider position `{layer=252, stack=-1}`: full tower 251 + layer 252's normal content visible.
- `compare_slices.py`: still 0.
- Manual: scrub the slider; visually confirm the tower assembles bottom-up.

**Risk:** if libvgcode's bucket structure resists augmentation, fall back to client-side culling — keep all vertices loaded, set per-layer visibility flags in the viewer's own state, do filtering at draw time. Slower but works.

### Phase 6 — Polish

**Code changes**
- Sub-tick visual distinction: smaller tick height, contrasting color, optional badge/glyph.
- Hover labels: `Tower at layer 251 (ext=0), stack 3/8 → z=51.0`.
- Layer-stats panel: handle sub-ticks (display "Tower stack" badge or skip stats — TBD).
- Audit the `layer_count == m_values.size()` assumption sites discovered in Phase 4. Each needs to do the right thing under sub-ticks.

**Verification**
- Manual UI walkthrough.
- `compare_slices.py`: still 0.

## Update mandate

This document MUST be kept current as work proceeds. Each phase's commit should also update this file:
- Mark the phase's status (Not started → In progress → Complete).
- Record any deviations from the plan: extra files touched, new design decisions, blockers found.
- Append to the Progress Log section below.

Discoveries that change the plan (e.g. libvgcode forces a Plan B in Phase 5) get rewritten into the affected phase descriptions, not just buried in the log. The plan is the source of truth for the current intent; the log is the running history.

## Progress log

- 2026-04-27 — Plan committed. Investigation done; ready to start Phase 1a.
