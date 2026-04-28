# GCode Viewer — Filament Lookahead Sub-Layer Display

Branch: `filament-lookahead`
Status: Phase 1a ✅ complete. Phase 1b ✅ complete. Phase 2 ✅ complete. Phase 3 next.

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

### Phase 1a ✅ — Tower metadata in `GCodeProcessorResult`

**Code changes**
- `GCodeProcessor::process_tags()`: parse `LOOKAHEAD_BLOCK_BEGIN` and `LOOKAHEAD_BLOCK_END` comments alongside existing `Z_HEIGHT`.
- `GCodeProcessor`: new state members `m_lookahead_in_block`, `m_lookahead_current`. While in_block, every `Z_HEIGHT:` bump that increases the running top_z increments `stack_count`. Ramp-down `Z_HEIGHT:` to base_z (which doesn't increase top_z) is correctly NOT counted.
- New struct `LookaheadTower` in `GCodeProcessorResult`: `{base_layer, extruder_id, base_z, top_z, stack_count, begin_line, end_line}`. Vertex tagging deferred to Phase 1b.
- `GCodeProcessorResult::reset()` clears `lookahead_towers`.
- Operator= copies `lookahead_towers`.
- `stack_count` semantics: total physical layers represented (1 base + N extras). For a tower covering layers L..L+N, stack_count = N+1.

**Verification (PASSED)**
- Temporary probe log written at `finalize()`.
- Sliced tulip; got 9 records:
  ```
  [0] base_layer=233 ext=0 base_z=46.80 top_z=48.40 stacks=9
  [1] base_layer=242 ext=0 base_z=48.60 top_z=50.20 stacks=9
  [2] base_layer=244 ext=3 base_z=49.00 top_z=50.60 stacks=9
  [3] base_layer=251 ext=0 base_z=50.40 top_z=52.00 stacks=9
  [4] base_layer=253 ext=3 base_z=50.80 top_z=52.40 stacks=9
  [5] base_layer=260 ext=0 base_z=52.20 top_z=53.80 stacks=9
  [6] base_layer=262 ext=3 base_z=52.60 top_z=54.20 stacks=9
  [7] base_layer=269 ext=0 base_z=54.00 top_z=54.80 stacks=5
  [8] base_layer=271 ext=3 base_z=54.40 top_z=56.00 stacks=9
  ```
  All 9 expected base layers present. Stack counts match the planning-side records (extras=8 → 9 total, except tower 7 where the cascade truncated to extras=4 → 5 total).
- `compare_slices.py`: 0 divergences. No regression.

**No UI surface area.** Probe log is compiled-in for now; remove or guard at a later phase.

### Phase 1b ✅ — Per-move tower tagging

**Code changes**
- `GCodeProcessorResult::MoveVertex`: added `int8_t tower_id = -1` and `int8_t stack_index = -1` fields (8-bit signed; -1 sentinel covers all moves outside any LA block, indices 0..127 are plenty for towers/stacks).
- `GCodeProcessor` state: added `m_lookahead_current_tower_id` and `m_lookahead_current_stack_index`. tower_id is pre-computed at BEGIN as `lookahead_towers.size()` so moves can be tagged with the right index before the END pushes the record.
- `store_move_vertex()`: after the existing aggregate-init `push_back`, if `m_lookahead_in_block` is true, set `back().tower_id` and `back().stack_index`. Avoids plumbing extra fields through the positional initializer.
- `Z_HEIGHT` handler now also bumps `m_lookahead_current_stack_index` in lockstep with `stack_count`. The safe-exit Z_HEIGHT back to base_z (which doesn't increase top_z) correctly does NOT bump stack_index — so safe-exit moves are tagged with the LAST upper-stack's index.

**Verification (PASSED)**
- Histogram of tulip moves per (tower, stack):
  - 9 towers × 9 or 5 stacks each (matches Phase 1a stack_count exactly).
  - Tower 7 truncated to stacks 0-4 (matches extras=4).
  - Stack 0 of each tower is biggest (toolchange dance + first stack content).
  - Stacks 1..N have decreasing or roughly-equal move counts.
  - Untagged moves (outside any LA block): 911,360 — the rest of the print.
- `compare_slices.py`: 0 divergences. No regression.

**No UI surface area.** Probe log still compiled in.

### Phase 2 ✅ — Propagate tower tags into libvgcode

**Code changes**
- `libvgcode::PathVertex` in `src/libvgcode/include/PathVertex.hpp`: added `int8_t tower_id{-1}` and `int8_t stack_index{-1}` fields. Defaults make existing aggregate-init sites that stop short (the dummy/skirt vertices in LibVGCodeWrapper.cpp lines 283-305) compile unchanged with -1 sentinels, which is correct for those out-of-tower contexts.
- `LibVGCodeWrapper.cpp`: 4 PathVertex aggregate-init sites (the ones converting from MoveVertex at lines 224, 231, 243, 251) extended to copy `curr.tower_id` and `curr.stack_index` from the source MoveVertex. Both `VGCODE_ENABLE_COG_AND_TOOL_MARKERS` branches updated.
- Probe added at end of `convert(GCodeProcessorResult)` to count tagged-vs-untagged PathVertex. Removed in a later phase.
- The build-time "color tower vertices red" debug flag was deferred — it requires renderer changes that conflict with the deliberately-inert phasing here. Visual verification will fall out of Phase 5 naturally; for Phase 2 the count probe is sufficient.

**Verification (PASSED)**
- Loaded tulip in slicer GUI Preview tab. Probe wrote:
  ```
  PathVertex tagged: 100927  untagged: 1295718  total: 1396645
  ```
- PathVertex count (1.4M) is ~40% higher than MoveVertex count (980k) because libvgcode adds phantom boundary vertices that inherit the source MoveVertex's fields. Tagged ratio matches: ~7.0% in MoveVertex vs ~7.2% in PathVertex — proportionality preserved.
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
- 2026-04-27 — Phase 1a complete. 9 tower records correctly parsed on tulip; comparator clean. Probe log left compiled in (will be removed in a later phase). Note on stack_count semantics: it counts total physical layers (1 base + N extras); the original plan text said "8/8/8/8/8/8/8/4/8" but that was extras only — actual stored value is 9/9/9/9/9/9/9/5/9.
- 2026-04-27 — Phase 1b complete. Per-move tower_id / stack_index tagging implemented and verified via histogram. Stack-0 of each tower has the biggest move count (toolchange dance + base layer content) — expected pattern. Comparator clean. Phase 2 next: propagate these tags into libvgcode's PathVertex.
- 2026-04-28 — Phase 2 complete. PathVertex extended; 4 aggregate-init sites in LibVGCodeWrapper.cpp updated. Verification via probe counted 100,927 tagged / 1,295,718 untagged PathVertex on tulip preview load — tagged ratio (7.2%) matches MoveVertex tagged ratio (7.0%), confirming clean propagation. Build-time "red tower" debug flag deferred to Phase 5 where renderer changes are natural. Phase 3 next: tint tower-base ticks in the slider.
