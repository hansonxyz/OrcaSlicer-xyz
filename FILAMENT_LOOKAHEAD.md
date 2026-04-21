# Filament Lookahead — Multi-Layer Batched Printing for Multi-Material

Branch: `filament-lookahead`
Status: analysis + visualization working (Phase 2); gcode reorder not yet implemented (Phase 4–5 pending).

## Goal

On single-nozzle multi-material printers (e.g. X1C / A1 with AMS), when a region printed with one filament is spatially isolated from other filaments on the plate, print multiple consecutive layers of that region before switching filaments — instead of switching on every layer. Each skipped filament change saves purge material and ~30–60 seconds of tool-change time. On typical MMU painted models, a "disappearing extruder" case (e.g. a gold nub on top of a body) can eliminate *all* subsequent tool changes for that filament, saving hours and 50%+ of its purge.

## Terminology

All definitions are per-layer unless noted. "Filament" and "extruder" are interchangeable here.

### Zone concepts (single layer)

**Lookahead candidate zone** *(per layer, per filament, per contiguous cluster)*
A polygon covering a spatially-contiguous cluster of a single filament's extrusion, inflated outward by `min_clearance_distance`. Multiple candidate zones can exist per (layer, filament) if the filament has several separated clusters. Must satisfy **isolation**: no extrusion of a *different* filament falls inside the inflated polygon on the same layer. Same-filament extrusion inside the zone is fine. Computed from model extrusions only, independently per layer.

**Filament-complete layer** *(per layer, per filament)*
Layer L is filament-complete for filament F iff every extrusion of F on L falls inside some F candidate zone on L. Required for F to participate in any lookahead tower crossing L. If F has an orphan region outside all F's candidate zones, a tool change to F on L is unavoidable — erasing the savings.

**Extended candidate zone** *(per layer, per filament, per zone)*
A candidate zone after support integration. Starts as a candidate zone; expands via the recursive support-override loop (Rule 2) to cover same-filament supports + clearance, until stable or invalidated. May be larger than the originating candidate zone if supports extend outward.

### Tower concepts (across layers)

**Lookahead base zone** *(per tower)*
The extended candidate zone at the tower's starting layer L.

**Lookahead extra layer zone** *(per tower, for each layer L+k where k ≥ 1)*
The tower's continuation onto layer L+k. An extended candidate zone on L+k qualifies as an extra layer zone iff:

1. **Same filament** as the tower.
2. **Filament containment**: every extrusion of F on L+k belonging to this tower fits inside the previous layer's (L+k-1) extended zone. The extra layer zone's own inflated polygon may extend outward beyond the previous zone — only the raw filament extrusion must be contained. This allows cone-upward growth within the clearance headroom.
3. **Filament-complete**: L+k is filament-complete for F.
4. **L+k is not a blanket exception layer**.

If a region of F on L+k does not fit within the previous layer's zone, the current tower terminates at L+k-1. The uncontained region is eligible to become a new tower's base zone starting at L+k.

**Lookahead tower**
An ordered stack: base zone (layer L) + zero or more extra layer zones (layers L+1, L+2, …, L+k). Same filament throughout. Bounded by `max_lookahead_height`. Multiple towers of the same filament can coexist on the same layers (spatial separation); towers of different filaments can coexist.

### Layer-state concepts

**Lookahead-active filament on layer L**
A filament F is lookahead-active on layer L iff some tower with k ≥ 1 covering F passes through L (i.e. L is an extra-layer layer of a tower, not a base layer). Implication: F's printing on L is being done entirely by lookahead from below, so no tool change to F is scheduled on L.

**Normal layer printing on layer L**
Extrusion on layer L that is *not* covered by any extra layer zone of a tower crossing L. Normal printing on L must not use any lookahead-active filament for L — doing so would require a tool change to that filament on L, defeating the savings. For model extrusion this is automatically guaranteed by Rule 4. For supports with "Any [Type]" filament, the "Any" resolution must pick from filaments that are not lookahead-active on L.

**Blanket exception layer**
A layer L where "Any [Type]" support filament resolution cannot find a valid choice because all compatible filaments (matching the "Any" type) are lookahead-active on L and no compatible filament is in use by a normal-mode region on L. Consequence: every tower in progress that would cross L terminates at L-1. Layer L prints entirely as normal. Layer L+1 is eligible to start fresh as a new tower base.

## Rules

### Rule 1 — Candidate zone isolation

A candidate zone contains only its own filament's extrusion. Different-filament extrusion inside the inflated polygon invalidates the zone.

### Rule 2 — Support "Any" override (recursive, per candidate zone)

For each candidate zone Z of filament F on layer L:

1. **Initial override**: find support regions intersecting Z. For each one that is "Any [Type]" compatible with F, override its filament to F.
2. **Re-extend**: recompute Z's polygon to include (F-overridden supports + clearance).
3. **Conflict check**: examine the newly-covered area for intrusions.
   - Intrudes into model extrusion of a different filament → zone invalid.
   - Intrudes into a concrete-filament support with different filament → zone invalid.
   - Intrudes into an "Any [Type]" support *incompatible* with F → zone invalid.
   - Intrudes into an "Any [Type]" support *compatible* with F → override it to F and recurse to step 2.
4. **Termination**: recursion ends when step 3 finds no new intersections (zone valid) or hits any invalidation condition (zone invalid).
5. **Override persistence on invalidation**: if Z is invalidated, the "Any" support overrides made during steps 1–3 are **kept**, not reverted. Those supports are spatially within Z's clearance radius; any other candidate zone wanting to override them would pull them into the same conflicts and also invalidate. Leaving the overrides in place avoids wasted recursion later and is self-consistent — if a different filament tried to claim them, the recursion would find the now-assigned filament as an intrusion and invalidate that attempt too.

**Corollary on conflicting zone claims**: when two candidate zones (of different filaments A and B) both want to override the same "Any" support, only the first to process it commits an assignment. The second zone's recursion then finds an assigned-but-incompatible support as an intrusion and invalidates itself. The "loser" filament's extrusion on that layer must then print normally (not as part of any tower), which is a Rule 4 problem — that filament is no longer filament-complete on this layer for its would-be tower, so no tower using that filament crosses this layer.

### Rule 3 — Containment for tower continuation

Layer L+k extends a tower ending at L+k-1 iff every extrusion of the tower's filament F on L+k belonging to the upward-growing region is contained within the previous layer's (L+k-1) extended zone polygon. The L+k extra layer zone's own polygon (filament + clearance) may extend outward; only the raw filament on L+k must be contained.

### Rule 4 — Filament completeness at every layer

A tower can exist at layer L+k only if L+k is filament-complete for the tower's filament F. Violation truncates the tower at L+k-1.

### Rule 5 — Normal printing filament exclusion

On layer L, any region outside all tower zones (normal printing) must use a filament that is not lookahead-active on L. For model extrusion this is guaranteed by Rule 4; for "Any [Type]" support filament resolution, the "Any" pick must exclude lookahead-active filaments on L.

### Rule 6 — "Any" support filament preference ordering

When assigning a concrete filament to an "Any [Type]" support on layer L:

1. **Preferred**: a filament already used by a candidate zone that intersects the support (per Rule 2). Locks in zone extension.
2. **Next**: a filament already printed by a normal-mode region on L. Avoids introducing a new tool change.
3. **Last resort**: original "Any" heuristic (cheapest matching filament). Still excluding any lookahead-active filaments (Rule 5).

If no choice satisfies Rule 5, layer L is a blanket exception layer (Rule 7).

### Rule 7 — Blanket exception handling

A blanket exception at layer L forces every tower passing through L to terminate at L-1. Layer L is then a normal-printed layer. Layer L+1 is evaluated fresh as a potential new tower-base layer.

### Rule 8 — Layer print order

Within a single physical layer, gcode emission order is:

1. **Normal regions first** (all tool changes and extrusions for filaments printed in normal mode on this layer).
2. **Tower base and extra layer zones second**, in order of the towers' base filaments (tool-change-minimizing order).

Rationale: flush-into-infill (see Rule 9) needs normal-mode printing to run first so that the excess filament discharged during tool changes can be deposited into towers that are about to be printed.

**Active exclusion zones during travel**: as each tower is printed on its relevant layer, that tower's exclusion zone on that layer (and on all subsequent layers it extends into) becomes **active**. Active zones must be respected by every subsequent travel move on any layer the zone covers: travel paths must navigate around the zone's polygon, following the geometry of the zone's corners. This is how printed lookahead regions are protected from being clipped by travel on later layers.

### Rule 9 — Flush-into-infill/supports interaction

Flush-into-infill and flush-into-supports can discharge purge material into infill/support regions during tool changes. This interacts poorly with lookahead because the discharge is scheduled per-tool-change and lookahead moves tool changes around.

Two acceptable implementations:

1. **Enforce off**: disable flush-into-infill and flush-into-supports whenever filament lookahead is enabled. Simplest; gives up those savings in exchange for the larger lookahead savings.
2. **Conditional re-enable**: slice each layer with flush-into-infill/supports *disabled* first. If after lookahead planning the layer turns out to be a pure normal layer (no tower crossings, no tower bases), re-slice that layer with flush-into-infill/supports *enabled*. Preserves both savings where they don't conflict.

Option 1 is simpler and safer for v1. Option 2 is an optimization to revisit after v1 is proven.

### Rule 10 — Wipe tower integration *(outstanding design work)*

The wipe tower backfill algorithm (see earlier design) needs integration with the tower/zone terminology above. Open questions:

- Does the wipe tower get its own "lookahead tower"-like data structure for purge-band redistribution bookkeeping?
- How does the wipe tower's exclusion zone interact with Rule 8 (active-during-travel)?
- When does backfill extrusion get scheduled relative to normal/tower print order within a layer?

**Not yet specified.** Phase 5 design work will flesh this out. For now, wipe tower handling uses the existing implementation (per-layer purge bands, no redistribution), which means lookahead savings are mostly optical — we avoid tool-change *moves* but not the purge material cost. The wipe tower integration is what unlocks the real material savings.

## Algorithm phases (end-to-end, full spec)

1. **Initial candidate zone identification (model-only)**. For each layer, cluster per-filament extrusions by spatial contiguity, inflate by clearance, run isolation check. Output: per-(layer, filament) list of initial candidate zones.

2. **Phase A support filament assignment**. For each initial candidate zone, find intersecting supports with "Any [Type]" compatible with the zone's filament. Lock those supports to that filament per Rule 2.

3. **Phase B support filament assignment**. For remaining "Any [Type]" supports, pick filaments from the normal-mode active set on each layer per Rule 6. If no option satisfies Rule 5, flag the layer as a blanket exception (Rule 7).

4. **Extended candidate zone computation**. Fold locked-same-filament supports into each zone (extend polygon by support + clearance). Re-validate isolation. Drop invalid zones. Run Rule 2's recursion for each zone until stable.

5. **Filament-completeness marking**. For each (layer, filament): if the union of extended candidate zones covers every extrusion of that filament on that layer, mark (layer, filament) complete. Otherwise incomplete.

6. **Tower assembly**. For each extended candidate zone at layer L, walk upward applying Rules 3–4–7:
   - Is there an extended candidate zone for the same filament at L+k?
   - Does L+k's filament fit inside the L+k-1 zone (Rule 3)?
   - Is L+k filament-complete (Rule 4)?
   - Is L+k a blanket exception (Rule 7)?
   - If all clear, stack. Otherwise terminate tower at L+k-1. Never exceed `max_lookahead_height`.

7. **Plan output**. List of towers: base layer, filament, extra-layer count, extended-zone polygon per layer. Also: list of blanket-exception layers for downstream handlers.

## Nozzle clearance model

Per-printer setting, two tiers:

| Printer | Tier 1 Height | Tier 1 XY Gap | Tier 2 Height | Tier 2 XY Gap |
|---------|---------------|---------------|---------------|---------------|
| Default | 1mm           | 5mm           | 0 (disabled)  | 0 (disabled)  |
| A1M     | 2mm           | 5mm           | 4mm           | 20mm          |
| X1C     | 1mm           | 5mm           | 3mm           | 18mm          |

- **Tier 1 (small clearance):** max extra height with a minimum XY gap. Default 1mm / 5mm. The nozzle tip protrudes at least 1mm on virtually every FDM printer.
- **Tier 2 (large clearance):** max extra height with a larger minimum XY gap. Default disabled. Users who know their printhead geometry can configure this for taller towers.

## Wipe tower backfill algorithm *(design, not yet implemented)*

On MMU/AMS printers the wipe tower *is* the cost of tool changes. Moving extrusions forward in time only saves real material if the purge moves with them. The wipe tower is a structural monolith: removing any layer's band leaves a gap.

1. **Redistribute purge within a lookahead-active layer.** When a layer has tool changes eliminated by lookahead (those tools printed ahead on earlier layers), purge bands for eliminated tools are reassigned:
   - The next remaining tool change on that layer absorbs the width of the eliminated band(s) immediately before it.
   - If lookahead eliminates all tool changes except the first, that first change's purge fills the entire tower footprint for the layer (one continuous band instead of N stripes).
   - Every layer still gets a full purge-height extrusion → tower structural continuity preserved.

2. **End-of-layer tower backfill for fully-skipped layers.** When lookahead prints extruder E for layers L..L+k in one go, layers L+1..L+k may have no extrusion on the tower from anyone:
   - After the last tool that prints on physical layer L finishes its model extrusion, the tool travels to the tower, lowers to each skipped layer's Z in sequence, and extrudes a full-tower-footprint layer of the current filament.
   - Happens once per physical layer, using whichever tool is already active → no extra tool changes incurred.
   - Filament color/type of backfill is irrelevant (tower is discarded).

3. **Wipe tower exclusion zone.** The tower gets its own exclusion zone inflated around its actual extrusion footprint. Per clearance validation: no other object's extrusion may fall inside it. The zone may overlap lookahead zones freely. Slice-time violation is a hard error.

## Filament change Z clearance

Filament change sequences (purge, cut, possible re-home) currently assume a flat layer. With batched layers creating raised regions, the toolhead must be raised above the maximum partially-printed layer height before any filament change, then lowered after.

**Firmware behavior (researched March 2026):**

- **Bambu Lab**: pause Z-raise is hardcoded in closed firmware, undocumented, not configurable. Pre-planned pauses via slicer gcode can prepend extra `G91 / G1 Z` commands before `M400 U1`, but firmware still does its own raise. Ad-hoc pauses (touchscreen / network) cannot be modified at all.
- **Marlin**: fully controllable at runtime via `M600 Z<mm>`, `M125 Z<mm>`, or `G27 P2`. Default 20mm.
- **Klipper**: fully controllable via customizable `PAUSE` macro. Default ~10mm.
- **RepRapFirmware (Duet)**: controlled by editable `/sys/pause.g` macro. User-defined.
- **Smoothieware**: configured via `after_suspend_gcode`. No default Z raise.

**BBL filament change Z clearance (from gcode analysis):**

- All BBL printers except H2D: `G1 Z{max_layer_z + 3.0}` — 3mm above highest printed point.
- H2D / H2D Pro: 8mm initial raise, 3mm on return (larger head geometry).
- For batched layers, filament change gcode must use `max_batched_z + 3.0` instead of `max_layer_z + 3.0`.

**Conclusion:** pre-planned filament changes (our primary use case) work on all firmwares since we control the gcode. For ad-hoc pauses on Bambu printers, we should warn users that manual pause during batched-layer regions risks collisions, and recommend slicer-inserted pause-at-layer instead.

## Implementation phases

| Phase | Scope | Status | Risk | Verifiable by |
|-------|-------|--------|------|---------------|
| 1 | Cleanup — delete dead inline-printing code, remove debug `fprintf` spam | ✅ Done | Zero | Build passes, zones still render, gcode byte-identical to baseline |
| 2 | Analysis expansion — include `support_layers()` extrusions in per-extruder analysis; add per-layer cascade truncation in the disappearing-extruder branch; restrict to disappearing-extruder strategy for v1 | ✅ Done | Low | Zones appear on support-generated layers for affected filaments; plans stop at first intermediate-layer violation |
| 2.5 | Polygon-based isolation — upgrade bbox isolation to polygon offset+intersect. *Deferred unless bbox isolation shows false positives in real prints.* | ⏸ Deferred | Medium | Zones honor actual polygon geometry instead of bbox approximation |
| 3a | "Any" support filament resolution overhaul — implement Phase A / Phase B assignment logic (Rules 2, 5, 6). Recursive override with invalidation-preservation. | 🔜 Next | Medium | Supports near candidate zones get overridden to the zone's filament; blanket exception layers correctly identified |
| 3b | Filament-completeness + tower stacking — implement the full tower algorithm (Rules 3, 4, 7). | 🔜 | Medium | Towers grow upward with cone-outward allowance; truncate at non-complete or blanket-exception layers |
| 3c | Wipe tower zone validation — compute inflated tower extrusion footprint; slice-time check rejects objects that intrude; render the tower's zone in gcode preview as a rectangle identical to lookahead zones | 🔜 | Low | Deliberate bad placement triggers slicer error; rendered preview shows a tower zone alongside lookahead zones |
| 4 | Gcode annotation markers — emit `; LOOKAHEAD_BLOCK_BEGIN plan_id=N layer=L extruder=E layers_ahead=K` and `; LOOKAHEAD_BLOCK_END plan_id=N` around eligible extrusion spans. Also enforce Rule 8 (normal-first ordering) and Rule 9 (flush-into-infill handling). No reordering yet. | 🔜 | Zero | Byte-diff vs pre-phase gcode: only comment-line differences + reordered-but-equivalent extrusion order |
| 5 | Post-processor — new `src/libslic3r/GCode/FilamentLookaheadPostProcessor.cpp`. Parses markers, extracts blocks, reassembles with Z travel moves, implements purge redistribution and end-of-layer tower backfill. Routes travel around active exclusion zones (Rule 8). Disappearing-extruder case only for v1. Config flag to disable the pass (falls back to Phase-4 annotation-only output). | 🔜 | High | Printed part shows reduced tool changes and intact wipe tower; A/B diff with post-processor disabled produces Phase-4 output unchanged |

Phases 1–4 each leave the slicer functional and A/B-comparable to baseline at every step. Phase 5 is the one that can actually go wrong.

## What's currently implemented

- **Config options**: `filament_lookahead`, `filament_lookahead_max_height`, `filament_lookahead_clearance` are wired through `PrintConfig` and appear in the UI.
- **Analysis stage** (`src/libslic3r/GCode/FilamentLookahead.cpp`): `FilamentLookaheadPlan::build()` runs in `GCode::_do_export()` before layer iteration. Walks `PrintObject::layers()` and `support_layers()`, clusters entity bboxes into spatially connected groups, identifies disappearing-extruder candidates with per-layer cascade truncation. Support filament resolution uses concrete `support_filament` / `support_interface_filament` config values; "Any [Type]" supports are skipped in the current implementation (Phase 3a replaces this with proper Phase A/B resolution).
- **Cluster isolation filter**: each cluster checked against every other filament's entities on the same layer; only isolated clusters become exclusion zones.
- **Gcode comment emission**: `; LOOKAHEAD_EXCLUSION_ZONE x_min=... y_min=... x_max=... y_max=... z_max=...` emitted per affected layer from `GCode.cpp`; also populated directly into `m_processor.result().lookahead_exclusion_zones` for the in-memory GUI preview path.
- **GCodeProcessor parsing**: comments parsed back into the struct for file-loaded gcode.
- **GCodeViewer rendering**: zones drawn as yellow translucent rectangles, filtered by the vertical layer slider's visible range, toggleable via "Travel Exclusion Zones" in FeatureType / Speed / ActualSpeed legends.

## Open design questions

- **Wipe tower integration (Rule 10)**: full purge-redistribution + end-of-layer backfill design exists; integration with the tower/zone terminology above is not yet spec'd.
- **Layer adhesion timing**: printing multiple layers of one region while others wait could affect inter-layer bonding if the delay is too long. Cap max lookahead layers accordingly — unclear what the right ceiling is.
- **Travel path planning around active zones (Rule 8)**: needs to plug into OrcaSlicer's avoid-crossing-perimeter logic. Corner-following is the approach but the implementation hook is TBD.
- **Pause/resume safety across software-initiated and printer-initiated pauses**, especially on Bambu firmware where Z-raise is hardcoded.

## Files

- `src/libslic3r/GCode/FilamentLookahead.hpp/cpp` — analysis stage
- `src/libslic3r/GCode.cpp` — comment emission in `_do_export`, plan integration
- `src/libslic3r/GCode/GCodeProcessor.hpp/cpp` — comment parsing, `ExclusionZone` struct
- `src/slic3r/GUI/GCodeViewer.cpp` — zone rendering, layer-range filtering, toggle
- `src/libslic3r/PrintConfig.hpp/cpp` — config options

## Progress log

- **2026-04 Phase 1**: deleted ~120-line dead inline-printing block, removed debug `fprintf` spam. ~150-line net reduction. Analysis and visualization behavior unchanged.
- **2026-04 Phase 2**: added support-layer walk classifying `support_fills` entities by role (base vs interface) and mapping to object layers by `print_z`. Added per-layer cascade truncation in the disappearing branch. Gated off bbox-isolated-but-continuing strategy for v1. Skips "Any [Type]" supports with a known-limitation note (addressed in Phase 3a).
