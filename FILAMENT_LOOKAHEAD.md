# Filament Lookahead — Multi-Layer Batched Printing for Multi-Material

Branch: `filament-lookahead`
Status: Phases 1, 2, 3a, 3c complete. Analysis produces chained towers with max-envelope zones and a complete Any-Type support override map. Preview renders zones on base + extra layers. Gcode emission / reordering (Phases 5, 6) not yet implemented — override map is dormant, support colors in preview still follow the pre-lookahead resolver.

## Goal

On single-nozzle multi-material printers (e.g. X1C / A1 with AMS), when a region printed with one filament is spatially isolated from other filaments on the plate, print multiple consecutive layers of that region before switching filaments — instead of switching on every layer. Each skipped filament change saves purge material and ~30–60 seconds of tool-change time. On typical MMU painted models, a "disappearing extruder" case (e.g. a gold nub on top of a body) can eliminate *all* subsequent tool changes for that filament, saving hours and 50%+ of its purge.

## Development mandate — verbose logging

During development of this feature, every non-trivial decision point in the analysis, planning, and emission code **must** emit a log entry at `info` level or above, tagged with `[FLA]`, describing:

- What the step received as input (layer range, filament ids, cluster sizes, support buckets, etc.)
- What decision was made (ACCEPT / REJECT / CLAIM / ASSIGN / SKIP) and the reason
- The resulting state (counts, chosen extruder ids, truncation heights, etc.)

Rationale: the user's test feedback will usually be "it didn't work" or "the color's wrong" without precise detail about layer numbers or filament ids. The log is the authoritative record. Key facts:

- **Log file location**: `C:\Users\brian\AppData\Roaming\OrcaSlicer\log` — one file per slicer session, rotated per launch.
- **After each user test**, read the relevant log file directly and trace the `[FLA]` entries to reconstruct what the analysis did. Do not rely on the user to paste log snippets — fetch them yourself.
- **Use `info` or `warning` level**, never `debug`. The default log filter in OrcaSlicer may suppress `debug` depending on the user's `log_severity_level` preference.
- **Log even "boring" successes** (e.g. "Phase 3a skipped: no Any-Type supports"). Silence is ambiguous — an absent log line can mean "skipped intentionally" or "code didn't run". Explicit skip lines are the only way to tell.
- **Early-return logs are mandatory**. Each guard that exits `build()` early (no objects, single filament, etc.) must log its reason before returning.
- **Keep verbose logging until the feature is production-validated**. Remove or demote to `debug` in a later cleanup phase once Phase 7 is complete.

Typical log prefix pattern: `[FLA] <phase_name> <decision>: <key=value pairs explaining the decision>`.

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

**Exception layer**
A layer L where some condition prevents the planned lookahead configuration from producing valid output. Triggers include:
- **Blanket exception**: "Any [Type]" support filament resolution cannot find a valid choice because all compatible filaments are lookahead-active on L and no compatible filament is in use by a normal-mode region on L (per Rule 5, 6).
- **Travel-impossible exception**: no travel path exists around the active exclusion zones on L (per Rule 8).
- Future exception types may be added. The handling is unified — see Rule 7.

**Normal boundary layer**
A layer with no lookahead-active filaments (no tower extra-layer zones pass through). The most recent normal boundary layer below an exception is the restart point for exception-triggered re-planning (per Rule 7).

**Active exclusion zone (per layer)**
A lookahead tower's zone on a layer where the tower's extrusion is physically *above* the current layer's Z plane — i.e., any layer from the tower's base L up to and including its top layer L+k. Registered during gcode emission as each tower is placed; respected by all subsequent travel on the same or future layers the zone covers.

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

### Rule 7 — Exception layer handling (unified)

When any unresolvable condition is encountered on layer L (see "Exception layer" in Terminology for triggers), the resolution is:

1. **Identify** the most recent **normal boundary layer** N below L — a layer with no lookahead-active filaments (no tower extra-layer zones cross it).
2. **Invalidate** all towers whose base or extra-layer range touches L. Their extrusions revert to normal-mode scheduling.
3. **Re-plan** layers N+1 through L (and onward) with the invalidated towers removed. The re-plan proceeds with the same analysis pipeline (steps 1–7 in the Algorithm section) but with the invalidated towers excluded from consideration.
4. **Iterate** if the re-plan produces a new exception — find the next normal boundary layer further below and repeat.
5. **Safety bound**: if re-plan iteration count exceeds a small limit (suggested 10), abandon lookahead entirely for this slice and emit a warning to the user. Prevents pathological infinite loops from bad geometry.

This unified strategy subsumes earlier narrower cases (like the previous "blanket exception" rule that only truncated towers at L-1). Truncating at L-1 is still a valid *result* of the unified re-plan in many cases, but the unified rule handles all exception triggers consistently.

### Rule 8 — Layer print order and travel avoidance

**Emission order within a layer**:

1. **Normal regions first** — all tool changes and extrusions for filaments printed in normal mode on this layer.
2. **Tower bases and extra layer zones second**, in tool-change-minimizing order.
3. **Wipe tower interleaves** with tool changes as usual.

Rationale: flush-into-infill (Rule 9) needs normal-mode printing first so that excess filament discharged during tool changes can deposit into towers that are about to be printed.

**Active exclusion zone registration**: as each tower is printed on its base layer, its exclusion zone on every affected layer (base L through top L+k) becomes **active** — i.e., registered for travel avoidance. Active zones remain in effect for every subsequent travel move on any layer the zone covers.

**Travel avoidance algorithm**:

When emitting a travel move on layer N:

1. **Intersection test**: check if the straight-line travel vector intersects any zone active on N.
2. **Free case**: no intersection → emit straight move.
3. **Obstacle case**: travel intersects one or more zones. Treat overlapping or touching zones as one merged obstacle polygon.
   1. Travel straight up to the first zone boundary intersection point.
   2. Wall-follow the merged obstacle's perimeter, choosing the direction (CW or CCW) that reaches the exit point fastest.
   3. At each perimeter vertex, test whether a straight line to the destination crosses any active zone. If not, resume straight travel to the destination.
4. **Unreachable case**: destination is inside an active zone, or no path around the merged obstacle reaches the destination. Flag as a **travel-impossible exception** on this layer (see Rule 7).

Efficiency is not critical — the vast majority of travels won't intersect zones at all. Simple wall-following is sufficient for v1. For v1, choose CCW consistently if CW/CCW distance calculation is expensive; the resulting paths may be suboptimal but will be valid.

**Z-hop interaction**: existing Z-hop during travel only raises the nozzle slightly above the current layer's Z. Active zones represent extrusion *above* the current layer's Z by definition, so Z-hop does not clear them. Travel must still route around in XY regardless of Z-hop behavior.

### Rule 9 — Flush-into-infill/supports interaction

Flush-into-infill and flush-into-supports can discharge purge material into infill/support regions during tool changes. This interacts poorly with lookahead because the discharge is scheduled per-tool-change and lookahead moves tool changes around.

Two acceptable implementations:

1. **Enforce off**: disable flush-into-infill and flush-into-supports whenever filament lookahead is enabled. Simplest; gives up those savings in exchange for the larger lookahead savings.
2. **Conditional re-enable**: slice each layer with flush-into-infill/supports *disabled* first. If after lookahead planning the layer turns out to be a pure normal layer (no tower crossings, no tower bases), re-slice that layer with flush-into-infill/supports *enabled*. Preserves both savings where they don't conflict.

Option 1 is simpler and safer for v1. Option 2 is an optimization to revisit after v1 is proven.

### Rule 10 — Wipe tower per-layer purge redistribution

When filament lookahead is enabled, the wipe tower is printed at the normal layer Z on every physical layer (no skipped-layer descent, no end-of-layer backfill ritual). Per-layer purge bands are redistributed based on which tool changes actually happen:

1. **Skip purge for non-switching filaments.** A filament that is lookahead-active on this layer (being printed ahead from a lower base layer, not switching in or out on this layer) contributes no purge band for this layer. No tool change occurs for it here, so no flush is needed.

2. **Fill the structural gap with a normal-mode filament.** The footprint that would have been the skipped filament's purge band is filled with structural extrusion (not a tool-change flush), using:
   - **Preferred**: a filament actively being used in normal-mode printing on this layer.
   - **Fallback**: the currently-loaded filament at the moment the tower extrusion reaches this point.

3. **Structural extrusion parameters, not purge parameters.** A structural band uses the wipe tower's infill flow rate and volume-per-area — NOT a tool-change purge volume. This maintains correct per-layer extrusion volume for the tower's structural integrity without extruding unnecessary flush material.

4. **Auto-place support blockers around the tower.** At slice preprocessing, insert a support blocker polygon equal to the wipe tower footprint inflated by `min_clearance_distance`. This ensures auto-generated supports never intrude into the tower's clearance region. Replaces the previously-proposed "validate no object in tower zone" slice-time error.

5. **No wipe tower exclusion zone for travel routing.** Because the tower prints at the standard layer Z every physical layer (nothing skipped, nothing backfilled later), existing wipe tower travel avoidance applies unchanged. The tower does NOT get a lookahead-style active exclusion zone.

**Currently-loaded filament definition**: the filament last extruded before the wipe tower's structural band emission begins. Per Rule 8 (normal-first print order), this is either the last normal region's filament, the previous tower base's filament, or the prior tower extrusion's filament on this same layer — whichever was most recently active.

**Unreachable-state safeguard**: if a layer has no normal-mode filament AND the currently-loaded filament is somehow unusable, this is an invariant violation (not believed reachable under the rules above). Log and abort rather than silently work around it — such a scenario would indicate a bug in the analysis stage.

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

## Wipe tower purge redistribution *(design, not yet implemented)*

On MMU/AMS printers the wipe tower *is* the cost of tool changes. Moving extrusions forward in time saves real material only if the purge for skipped tool changes also goes away. The tower is also a structural monolith: every layer's footprint must be fully extruded for the tower above it to stand on.

The design is fully specified by Rule 10 (see Rules section). Summary:

- Tower prints at normal layer Z on every physical layer.
- A filament that is lookahead-active on a layer contributes no purge band there (no tool change → no flush).
- The footprint that would have been that filament's band is filled with *structural* extrusion (not purge flow) using a normal-mode filament from the same layer, or the currently-loaded filament as fallback.
- Support blockers around the tower footprint (inflated by clearance) are auto-inserted at slice preprocessing to keep auto-generated supports out of the tower's neighborhood.
- No special travel-avoidance zone for the tower — existing wipe tower avoidance suffices since the tower is printed at standard layer Z.

### Implementation notes for the post-processor

The existing wipe tower generator produces gcode based on `ToolOrdering`'s pre-lookahead per-layer tool sequence. After lookahead planning, some of those tool changes no longer happen on their original layer.

Two implementation options:

1. **Regenerate wipe tower with lookahead-aware ToolOrdering**: modify the wipe tower generator to accept the lookahead plan and produce correct bands up front. Cleaner gcode, but touches more of the existing pipeline.

2. **Post-process rewrite (v1 recommended)**: let the existing generator produce its standard bands, then the post-processor (location C) rewrites the tower gcode per-layer — skipping bands for lookahead-active filaments and filling gaps with structural extrusion.

Option 2 is lower risk for v1 and fits our Phase 5 integration. Option 1 is a future optimization once the feature is proven.

### Tower footprint sizing

The wipe tower's *width* is computed from the maximum per-layer purge volume across the print. After lookahead, per-layer purge volumes are lower, so the tower could be narrower. V1 does not regenerate the footprint — we accept the slightly oversized tower as tolerable waste (the tower is discarded anyway). A future optimization could resize the footprint after lookahead planning.

### Known interaction: flush-into-infill / flush-into-supports

These features redirect purge volume from the tower into object infill or support regions. With lookahead eliminating some tool changes, the flow of "flush volume" that these features depend on becomes inconsistent per layer. Per Rule 9, v1 disables flush-into-infill/supports when filament lookahead is enabled. A future version could re-enable them conditionally on pure-normal layers.

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

## Architecture

### Slicer pipeline at a glance

```
User clicks Slice
  └─ BackgroundSlicingProcess kicks off a worker thread
      └─ Print::process()                          src/libslic3r/Print.cpp

         ├─ PER-OBJECT PREPROCESSING (parallel across objects)
         │   └─ PrintObject::process()             src/libslic3r/PrintObject.cpp
         │       │ stages in order per object:
         │       ├─ posSlice              → slice mesh into layer polygons
         │       ├─ posPerimeters         → wall generation (Arachne/classic)
         │       ├─ posPrepareInfill      → classify solid/sparse regions
         │       ├─ posInfill             → fill patterns
         │       ├─ posIroning            → top surface smoothing
         │       ├─ posSupportMaterial    → tree + classic supports
         │       └─ posDetectOverhangs…   → malformations for auto-lift

         ├─ PLATE-LEVEL ASSEMBLY (after all objects done)
         │   ├─ psWipeTower  → per-layer purge bands, tool ordering, volumes
         │   └─ psSkirtBrim  → skirt loops, brim polygons

         └─ (slicing is "finished" — all extrusion entities exist in memory)

User sees preview / clicks Export
  └─ Print::export_gcode()
      └─ GCode::_do_export()              src/libslic3r/GCode.cpp:2399

         ├─ SETUP
         │   ├─ write header / config blocks + machine start_gcode
         │   ├─ compute ToolOrdering (resolves Any-Type supports per layer)
         │   ├─ compute layers_to_print (interleaves object + support by print_z)
         │   └─ instantiate filters: SpiralVase, PressureEqualizer,
         │        CoolingBuffer, FanMover, AdaptivePAProcessor

         ├─ LAYER PIPELINE (tbb::parallel_pipeline, serial_in_order output)
         │   │ per layer:
         │   ├─ generator: process_layer()
         │   │     layer change → tool ordering → per extruder:
         │   │       wipe tower toolchange, skirt/brim, model extrusions,
         │   │       support extrusions
         │   ├─ SpiralVase filter (if spiral mode)
         │   ├─ PressureEqualizer filter (if rate-limit set)
         │   ├─ CoolingBuffer filter (always)
         │   ├─ FanMover filter (fan command reposition)
         │   └─ AdaptivePAProcessor filter (if enabled)

         ├─ FINALIZATION → wipe tower final purge + end_gcode + footer
         └─ rename temp file to final .gcode

After the file is written
  └─ GCodeProcessor second pass (file-based) — re-parses for GUI preview,
       time estimates, thumbnails. Separate from the in-memory processor
       that runs inline during _do_export.
```

Three facts that matter for where our feature hooks:

1. **Supports are fully generated** before `_do_export` starts. `support_fills` is baked in-memory with role info on every entity.
2. **Any-Type support filament resolution** currently happens inside `process_layer()` using `ToolOrdering`'s per-layer active extruder list. Per-layer, at gcode-generation time.
3. **The layer pipeline is parallel** with serial output ordering. Cross-layer decisions must be pre-computed; per-layer marker emission can happen inside `process_layer()`.

### What the algorithm has to do (14 steps)

1. **Identify candidate zones** — per layer, per filament, cluster spatially contiguous same-filament extrusion, inflate by clearance, check isolation against other filaments
2. **Mark filament-complete layers** — per (layer, filament), verify every extrusion of that filament falls inside some candidate zone
3. **Phase-A support claim** — for each candidate zone, lock Any-Type supports of compatible type that intersect the zone to the zone's filament
4. **Recursive zone extension** — fold claimed supports into zone polygon, re-check isolation, claim any newly-intersected Any-Type supports, recurse until stable or invalidated (preserve overrides on invalidation per Rule 2)
5. **Phase-B support assignment** — for remaining unclaimed Any-Type supports, assign a filament active in normal-mode on that layer, excluding lookahead-active filaments
6. **Detect blanket exception layers** — layers where Phase-B has no valid choice → truncate every in-flight tower at L-1
7. **Assemble towers** — walk upward from each extended base zone, stacking extra-layer zones satisfying containment / completeness / non-exception, bounded by `max_lookahead_height`
8. **Emit ordered gcode with markers** — per layer: normal printing first, then tower bases + extra layer zones, with `LOOKAHEAD_BLOCK_BEGIN/END` markers wrapping tower extrusion spans
9. **Reorder tower blocks across layers** — extract marked blocks from original layers, relocate to destination layers with Z travel bracketing
10. **Rewrite wipe tower per-layer** — on each layer, skip purge bands for filaments that are lookahead-active, fill their footprint with structural extrusion using a normal-mode or currently-loaded filament (Rule 10)
11. **Auto-place tower support blockers** — at slice preprocessing, insert a support blocker equal to the wipe tower footprint inflated by `min_clearance_distance`, so auto-generated supports stay clear
12. *(retired)* — wipe tower exclusion zone validation is no longer needed: the tower prints at normal layer Z on every physical layer, and support blockers (step 11) handle the auto-support clearance requirement
13. **Travel path avoidance** — route around active exclusion zones on every layer after each tower is printed, corner-following
14. **Flush-into-infill handling** — either globally disable when lookahead is enabled, or per-layer conditional (re-enable on pure-normal layers)

### Hook locations

| Location | Description |
|----------|-------------|
| **(A)** | Analysis stage — after `PrintObject::process()`, before `_do_export()` layer loop. Read-only against all Print state. |
| **(B)** | Per-layer gcode generation — inside `GCode::process_layer()` in the parallel pipeline. |
| **(C)** | Post-processor on the final gcode file — runs between "write temp file" and "rename to final". |
| **(D)** | Travel subsystem — inside `GCode::travel_to()` and the `AvoidCrossingPerimeters` machinery. |
| **(E)** | Slice-time validation — `Print::validate()` or plate-level assembly stages. |

### Step → location map

| Step | Hook | Notes |
|------|------|-------|
| 1. Identify candidate zones | **A** | Pure geometry over layer data |
| 2. Filament-complete marking | **A** | Pure geometry over zones |
| 3. Phase-A support claim | **A** | Data tagging; nothing emitted yet |
| 4. Recursive zone extension | **A** | Polygon recursion |
| 5. Phase-B support assignment | **A** | Override-map consumed by (B) — see decision below |
| 6. Blanket exception detection | **A** | Falls out of steps 1–5 |
| 7. Tower assembly | **A** | Plan construction over zones |
| 8. Emit ordered gcode + markers | **B** | Only place this can go |
| 9. Reorder across layers | **C** | Cross-layer text rewrite |
| 10. Per-layer tower rewrite | **C** | Text rewrite of wipe tower gcode; skip + structural-fill |
| 11. Auto-place tower support blockers | **E** (preprocessing, before per-object `posSupportMaterial`) | Tower XY footprint is known from config (wipe_tower_x/y/size) — no dependency on `psWipeTower` runtime state, so the blocker can be injected early |
| 12. *(retired)* | — | Tower exclusion zone validation no longer needed |
| 13. Travel avoidance | **D** using plan data from **A** | Existing AvoidCrossingPerimeters infra |
| 14. Flush-into-infill handling | **E** config + possibly **B** | Global-off for v1 |

### Pros / cons per location

**(A) Analysis stage — steps 1–7**

*Pros:*
- Complete read-only access to Print state (layers, supports, regions, config)
- No state machine concerns, no parallelism, no gcode syntax
- Easy to unit-test in isolation
- Produces one clean data structure consumed by (B), (C), (D)
- Natural place for polygon-heavy operations (intersect, offset, containment)

*Cons:*
- "What if we picked a different Any-Type filament" is a counterfactual — we're essentially simulating the per-layer resolver that currently lives in (B). Either duplicate the logic or move it.
- Must define a plan data structure that survives being passed to `process_layer()` and through to the post-processor. Straightforward but new surface area.

**(B) Per-layer gcode generation — step 8, step 5 (obey-side), step 14 (partial)**

*Pros:*
- Has `ToolOrdering`, `LayerTools`, `WipeTower` state in scope
- Already the place where per-layer decisions happen
- Emitting comment markers is a one-line change per eligible block

*Cons:*
- **Parallel pipeline**: layers N and N+1 run concurrently. Any cross-layer decision must be pre-computed by (A). Rules out doing reorder here.
- **Existing code path is intricate**: the extruder loop at ~line 5075 in GCode.cpp is 400+ lines, handles skirt/brim/tool-change/wipe-tower/model/support, and is full of state. Inserting "normal regions first, then towers" is a surgical change to that loop's ordering.
- **Any-Type resolver currently here** (line ~4699): moving or overriding it requires care. `ToolOrdering` earlier in `_do_export` also needs the resolved values.
- **Filter pipeline downstream**: anything we emit must survive CoolingBuffer / PressureEqualizer / FanMover. Comments survive fine; extrusion rewrites may not.

**(C) Post-processor on final gcode — steps 9, 10, 11**

*Pros:*
- Operates on finalized gcode — all upstream filters done. No "will my edit survive the next filter?" problem.
- Text-based rewrite is straightforward for "cut these lines from here, paste there, insert Z move"
- **Cross-layer reasoning available**: we see the whole file at once; reordering across 5 layers is natural
- Parallel to existing `PostProcessor.cpp` pattern — known integration point
- **A/B switchable**: disable the stage, get baseline gcode. Essential for verification and rollback.

*Cons:*
- **Gcode parsing is brittle**: any upstream format change can silently break. Needs defensive parsing with assertions.
- **Tool-change blocks are multi-line with internal state**: M600, filament load, prime, wipe sequence. Extracting them as a unit requires knowing where they start and end.
- **Wipe tower structural-fill generates *new* extrusion gcode**, not just reorder — for each skipped purge band we synthesize `G1 F… E…` lines at the tower's infill flow rate and correct volume-per-area. Requires knowing the tower's per-layer footprint, layer height, and the currently-loaded filament's flow parameters. Most concerning part.
- **Pressure advance tuning is already baked in**: moving a block to a different layer, its PA-adjusted E values might be slightly off for the new context. Likely minor but worth verifying.
- **Embedded time estimate becomes stale** — the header block is from the pre-reorder timeline.

**(D) Travel subsystem — step 13**

*Pros:*
- `AvoidCrossingPerimeters.cpp` already exists for polygon-based travel avoidance — known pattern.
- Corner-following is what the existing infrastructure does.

*Cons:*
- **Temporal activation**: a tower's exclusion zone is "active" only *after* that tower is printed on the current layer. Existing avoidance treats "obstacles for this layer" as a static set. We'd need to rebuild the avoidance polygon set partway through a layer as each tower finishes.
- **Parallel pipeline interaction**: each layer has its own avoidance state; pre-computing "here's the zone set at each intermediate point" for a given layer needs to happen in (A).
- **Post-processor interaction**: if (C) reorders blocks, the travel gcode emitted in (B) assumed original ordering. Any travel that was *around* a tower that's now moved earlier needs its path recomputed — or we enforce that travels inside `LOOKAHEAD_BLOCK` markers never rely on zone state outside the block.

**(E) Slice-time preprocessing & config — steps 11, 14 (partial)**

*Pros:*
- Auto-placed support blockers integrate cleanly: the tower's XY footprint is known from config (`wipe_tower_x/y` + size), so the blocker can be injected as a modifier volume before per-object `posSupportMaterial` runs.
- Global config enforcement (flush-into-infill off) is a 5-line check.
- Changes here are pre-slice; if the user disables lookahead, the blocker is removed and a re-slice produces normal output.

*Cons:*
- The auto-blocker must be inserted/removed dynamically based on whether lookahead is enabled — careful coordination with any user-placed blockers so we don't clobber them.
- For flush-into-infill: the per-layer conditional approach effectively re-slices a layer — much bigger design change than a simple config check. Option 1 (global off) strongly preferred for v1.

### Decisions

**Any-Type resolver integration: Option 1 — override in (A), obey in (B).**

Chosen because:
- Minimal invasion in (B)'s existing code path
- Preserves existing behavior for cases we don't touch (Phase B falls through to the existing resolver naturally)
- Easy to revert or re-enable independently
- Option 2 (moving the whole resolver to (A)) is the "right" long-term factoring but fights with `ToolOrdering` which also needs the resolved values — risky for v1

Concretely: (A) produces a map `{support_entity → forced_filament_id}`. The existing resolver in (B) consults the map first; if an entry exists, use it; else fall through to the current `resolve_any_type_support_filament()` logic.

**Recommended split across hooks:**

- **(A) does**: all analysis (steps 1–7), produces a plan with per-tower base/extra-layer zones, per-support Any-Type override map, blanket exception layer list.
- **(B) does**: emit `LOOKAHEAD_BLOCK_BEGIN/END` markers, obey the Any-Type override map, enforce normal-first ordering within a layer.
- **(C) does**: read markers, reorder blocks across layers, rewrite wipe tower gcode per-layer (skip lookahead-active bands, structural-fill gaps).
- **(D) does**: add active lookahead-zone avoidance to travel routing, wired from the plan.
- **(E) does**: auto-insert support blocker around wipe tower footprint at preprocessing; flush-into-infill config enforcement.

Thin (A), thin (B), heavy (C), cameo (D) and (E).

## Implementation phases

The work is split across seven numbered phases. Each phase has multiple sub-phases that can be verified and committed independently. Ideally every sub-phase produces gcode that is byte-identical to baseline (or differs only in comment lines / adjusted travel paths) until Phase 6 — Phase 6 is the only phase where gcode semantics actually change.

Phases 1 and 2 are complete. Phases 3–6 build the feature. Phase 7 is polish and integration testing.

---

### Phase 1 ✅ — Cleanup

Delete dead inline-printing code (~120 lines behind `if (false && …)` guards), remove debug `fprintf` spam. No behavior change.

**Verifiable by**: build passes, zones still render, gcode byte-identical to baseline.

---

### Phase 2 ✅ — Analysis: supports + cascade truncation

Walk `support_layers()` in `FilamentLookaheadPlan::build()`, classify `support_fills` entities by role (base vs interface), bucket into per-extruder analysis storage. Add per-layer cascade truncation to the disappearing-extruder branch. Restrict to disappearing-extruder strategy for v1 (bbox-isolated-but-continuing gated off).

**Known limitation**: Any-Type support filament values are skipped in this pass — addressed in Phase 3a.

**Verifiable by**: zones appear on support-generated layers for affected filaments; disappearing-case plans stop at first intermediate-layer violation.

---

### Phase 2.5 ⏸ — Polygon-based isolation *(deferred)*

Upgrade bbox isolation checks to polygon offset + intersect. Deferred unless bbox approximation causes false positives in real prints. If tests show bbox isolation over-excludes valid candidates, revisit and replace bbox storage with `ExPolygons` throughout the analysis. Otherwise skip this phase entirely.

---

### Phase 3a ✅ — Any-Type support filament resolution

Walk support layers collects "Any (Type)" buckets (keyed by object × layer × role with their material type name). After the tower cascade, Phase A claims compatible intersecting buckets for each tower's filament; Phase B assigns remaining buckets via the default resolver but restricted to non-lookahead-active filaments (Rule 5). Produces `m_any_support_overrides: {object, layer_idx, is_interface} -> extruder_id`, accessed via `override_for_support()`. Granularity is per-(object, layer, role) to match the existing resolver — finer per-entity would require a gcode-emitter refactor. Consumer is Phase 5a.

**Verifiable by**: log shows `[FLA] Phase A CLAIM` / `[FLA] Phase B ASSIGN` lines; final summary shows override count equal to bucket count in practice.

### Phase 3c ✅ — Chained towers (greedy)

Replaced the v1 disappearing-extruder-only strategy. Per-extruder while-loop walks all layers; accepts the tallest possible tower (bounded by `max_lookahead_height` and cascade collisions); advances past its top; starts a new tower above. Cascade also checks that the tower's own filament has entities inside the zone on each layer (truncates otherwise — previously empty-but-clear layers incorrectly extended towers). Per Rule 8, exclusion zones register on the base layer in addition to extras so travel avoidance activates once the tower is printed. Zone shape is now the **max XY envelope** (union of the tower's filament entities across all its layers), not just the base cluster bbox — closes under-coverage when the model widens along Z.

**Verifiable by**: log shows `[FLA] TOWER ACCEPTED` lines per tower; preview shows multiple stacked zones where a single zone existed before; zone XY now covers the widest point of the tower.

### Phase 3 — Remaining analysis stage work (location A)

Produces the plan consumed by locations B, C, D, E. Sub-phases build it up; 3e exposes the final interface.

#### 3a — Any-Type support filament resolution *(Rules 2, 5, 6)*

**Technical details:**
- Add per-layer Any-Type support bucket keyed by material type name: `layer_anytype_supports[li][type_name] → vector<BoundingBox>` (scaffolding already drafted).
- **Phase A (zone-claiming)**: for each candidate zone of filament F, find intersecting Any-Type supports of type matching F's `filament_type`, lock them to F. Store in `m_any_support_overrides[support_entity_id] = extruder_id`.
- **Recursive zone extension** (Rule 2.1–2.4): fold locked supports into zone polygon, re-check for new intrusions, claim newly-intersected compatible Any supports, recurse. On conflict with incompatible filament → invalidate zone, preserve overrides (Rule 2.5).
- **Phase B (leftover)**: remaining Any-Type supports get filament from: (1) already-intersecting zone's filament, (2) normal-mode active on that layer, (3) cheapest matching filament. Excluding lookahead-active filaments (Rule 5).

**Open questions:**
- Support entity identity: currently `ExtrusionEntity*` pointers. Stable between analysis and gcode export since Print state isn't modified, but we should codify this assumption somewhere.
- First-come vs. spatial-majority for multi-zone claims on the same Any support: going with first-come per the existing spec (Rule 2's corollary handles the losing case via invalidation).

**Expected result:** analysis produces a complete override map. Any supports near candidate zones get the zone's filament; unclaimed Any supports get tool-change-minimizing assignments.

**Testable by**: log the override map during slicing. Construct a model with an "Any PLA" support adjacent to a red PLA region — confirm the support gets red.

#### 3b — Filament-completeness marking *(Rule 4)*

**Technical details:**
- For each (layer, filament), check every extrusion of that filament falls inside at least one candidate zone.
- Output: `m_filament_complete[layer][filament] → bool`.
- Used in 3c's tower stacking: an incomplete (L, F) cannot host a tower crossing through L for filament F.

**Open questions:**
- Efficiency: naively O(extrusions × zones) per (layer, filament). Probably fine for typical MMU prints (~5 filaments, modest extrusion count per layer). Optimize if profiling shows it's a bottleneck.

**Expected result:** a completeness map identifying (layer, filament) pairs where some of the filament is outside any candidate zone.

**Testable by**: construct a model with a small stray region of filament F outside its main cluster on some layer. Confirm that layer is marked incomplete for F and no tower for F crosses it.

#### 3c — Cone-upward tower stacking *(Rule 3)*

**Technical details:**
- Replace Phase 2's "cluster-same-shape" stacking with Rule 3's containment: layer L+k's *filament extrusion* must fit inside layer L+k-1's extended zone. The extended zone on L+k is allowed to be wider than L+k-1's (cone-outward).
- Uncontained filament regions on L+k become new tower base candidates starting at L+k.
- Honor `max_lookahead_height`, filament-completeness (from 3b), and exception layers (from 3d).

**Open questions:**
- Polygon containment check vs. bbox containment: bbox is coarser but faster. Start with bbox containment; upgrade only if false negatives appear.
- "New tower base for overflow" — how to cleanly emit this? Probably a work-queue: process candidate zones in layer order, track which zones are "parent towers" vs "new bases".

**Expected result:** towers can grow cone-outward within clearance headroom. Shrinking-upward and same-size towers still work (they always fit in the previous zone).

**Testable by**: model with a frustum shape (small base, wider top). Confirm tower covers expected layers.

#### 3d — Exception layer re-plan loop *(Rule 7)*

**Technical details:**
- Refactor `FilamentLookaheadPlan::build()` to be re-entrant: takes an optional `{invalidated_tower_ids, restart_from_layer}` argument.
- When exception is raised (from this phase or later phases), find normal boundary layer N below, invalidate affected towers, re-invoke `build()` with restart hint.
- Iteration cap: 10 (configurable). Exceeding cap → disable lookahead for this slice, emit warning.
- Central control loop lives in `GCode::_do_export()` around the plan construction.

**Open questions:**
- When an exception is raised during Phase 5 (gcode emission), how do we unwind? Proposed: buffer per-layer gcode in Phase 5 and only commit on layer completion; on exception, discard uncommitted layers, re-plan, restart emission from layer N+1.
- What counts as a normal boundary layer exactly? "No tower extra-layer zones cross it" — must NOT include tower-base layers since those don't have inbound extras. First layer of print is always a normal boundary layer.

**Expected result:** pathological exception triggers terminate cleanly via re-plan; iteration cap catches bad cases.

**Testable by**: construct a model with known exception triggers (overlapping Any supports with conflicting claims). Verify log shows re-plan iterations and final output is valid.

#### 3e — Plan data structure and query API

**Technical details:**
- Formalize `FilamentLookaheadPlan` as the single data structure passed to locations B, C, D, E.
- Contains: towers (base zone, extra-layer zones, filament, id), Any-Type override map, exception layer list, per-layer active zone index for fast travel queries.
- Query API (all const, thread-safe):
  - `exclusion_zones_active_on(layer_idx) → vector<polygon>`
  - `override_for_support(entity_id) → optional<extruder_id>`
  - `tower_containing(layer, filament, point) → optional<tower_id>`
  - `is_lookahead_active(layer, filament) → bool`
  - `is_exception_layer(layer) → bool`
- Debug dumper: pretty-prints the plan as text for verification.

**Open questions:**
- The plan is immutable after Phase 3 finalizes. But re-plans mutate it. Lock only during re-plan (rare path), no lock during emission (common path). Simple but check for race conditions during re-plan.

**Expected result:** clean interface between phases. Testable by writing a debug dump and manually inspecting for a known input.

---

### Phase 4 — Slice-time preprocessing (location E)

Small phase. Can be done any time relative to Phase 3; keeping it separate for cleanliness.

#### 4a — Auto-place wipe tower support blocker *(step 11, Rule 10.4)*

**Technical details:**
- When `filament_lookahead = true` AND the slice is about to proceed, compute wipe tower XY footprint from config (`wipe_tower_x`, `wipe_tower_y`, `prime_tower_width`, `wipe_tower_rotation_angle`) and inflate by `min_clearance_distance`.
- Inject a modifier volume with type `part_type == ModelVolumeType::SUPPORT_BLOCKER` at that footprint, covering full Z range.
- Tag it with a unique identifier (e.g. `"__auto_lookahead_tower_blocker"`) so we can remove it when lookahead is disabled without clobbering user-placed blockers.
- Hook: just before `PrintObject::process()` starts (so `posSupportMaterial` sees the blocker).

**Open questions:**
- Where does the blocker live in the data model? `ModelObject::volumes` — same place as user-placed modifiers. Tagged by name or by a new `is_auto_generated` flag.
- Removal on disable: scan model volumes for the tag on every slice start, remove stale auto-blockers if lookahead is off. Or use a config snapshot with undo integration.

**Expected result:** slicing with lookahead + auto-supports produces no support structures in the tower clearance region.

**Testable by**: slice a flat plate with ambient supports enabled, lookahead on. Confirm supports respect the tower's clearance zone.

#### 4b — Flush-into-infill / flush-into-supports config enforcement *(Rule 9)*

**Technical details:**
- At slice start, if `filament_lookahead = true`, force `flush_into_infill = false` and `flush_into_support = false`.
- Emit a one-time log message: "Filament Lookahead: flush-into-infill/supports disabled for this slice."
- Does not modify user's saved config values — only the in-memory slice config.

**Open questions:**
- Should this be visible in the UI (dimmed checkbox when lookahead is on)? Nice UX polish, can come later.

**Expected result:** flush operations don't redirect purge into objects during lookahead.

**Testable by**: enable both settings, slice, confirm purge goes only to the wipe tower.

---

### Phase 5 — Per-layer gcode emission (location B)

Emits markers and consults the plan. No reordering. Should produce gcode that's byte-identical to baseline except for comment lines and (where applicable) adjusted travel paths.

#### 5a — Consume Any-Type override map *(Rule 2 obey-side)*

**Technical details:**
- In `_do_export` around line 4699 where Any-Type support filament resolution happens, consult `m_plan.override_for_support(entity_id)` first.
- If an override exists, use that filament. Otherwise fall through to existing `resolve_any_type_support_filament()`.
- Same change applies inside `ToolOrdering` computation (runs earlier in `_do_export`) — need both paths to agree.

**Open questions:**
- `ToolOrdering` is built before `FilamentLookaheadPlan::build()` currently. Either: (i) build the plan before `ToolOrdering`, which means analysis happens even earlier; (ii) re-run `ToolOrdering` after plan construction with overrides applied. (i) is cleaner but may require refactoring plan construction to not depend on `ToolOrdering` output. Probably (ii) is less invasive for v1.

**Expected result:** supports near candidate zones get assigned to the matching filament. Visible in the tool change sequence in the final gcode.

**Testable by**: slice with lookahead, inspect tool change order, confirm the Any supports adjacent to candidate zones use the zone's filament.

#### 5b — Normal-first print order within a layer *(Rule 8 steps 1–3)*

**Technical details:**
- Modify the extruder loop in `process_layer()` to emit extrusions in two passes:
  1. Normal-mode extrusions (filaments not part of any tower on this layer).
  2. Tower base zones + extra-layer zones, in tool-change-minimizing order.
- Wipe tower toolchange gcode interleaves as today.

**Open questions:**
- Existing `ToolOrdering` picks a per-layer extruder order based on minimizing tool changes. Our "normal first" constraint intersects this — need to respect `ToolOrdering`'s choice within each group (normal, tower), but the group ordering is fixed.
- Skirt/brim emission: tied to the first extruder of a layer. Should remain first even with normal-first ordering (skirt is printed before model anyway).

**Expected result:** gcode's tool change sequence on any layer with towers shows normal filaments before tower filaments.

**Testable by**: slice with towers, scan `T<n>` commands in the output, verify ordering.

#### 5c — Emit LOOKAHEAD_BLOCK markers *(step 8)*

**Technical details:**
- Wrap each tower's extrusion span (base or extra-layer zone) with:
  ```
  ; LOOKAHEAD_BLOCK_BEGIN plan_id=N layer=L extruder=E role=base|extra stack_index=K zone_x_min=... (etc)
  <extrusion gcode>
  ; LOOKAHEAD_BLOCK_END plan_id=N
  ```
- At the start of each layer, emit:
  ```
  ; LOOKAHEAD_LAYER_INFO layer=L active_filaments=A,B normal_filaments=C,D is_exception=false
  ```
- Plan ID is assigned in 3e; one ID per tower.

**Open questions:**
- How verbose should markers be? Enough for the post-processor to fully reconstruct the plan from the gcode alone (so Phase 6 doesn't need the plan in memory). Err on the side of more info.
- Marker line length and terminator character conventions — keep consistent with existing comment conventions in OrcaSlicer gcode.

**Expected result:** gcode has unambiguous markers wrapping each tower's extrusion.

**Testable by**: byte-diff gcode with/without lookahead — differences should be *only* in comment lines and tool-change order (from 5b). Exception: 5d may also change travel paths.

#### 5d — Travel avoidance against active zones *(Rule 8 algorithm)*

**Technical details:**
- Before emitting any travel move in `GCode::travel_to()`, consult `m_plan.exclusion_zones_active_on(current_layer_idx)`.
- Intersect straight-line vector with the active zones. If no intersection → straight travel.
- If intersection → wall-follower (CCW for v1). Merge overlapping/touching zones into single obstacle. Walk perimeter until straight line to destination is clear.
- Emit multi-point travel as `G0` commands with `F` feedrate preserved.
- Unreachable → trigger 5e.

**Open questions:**
- Integration with `AvoidCrossingPerimeters`: cleanest is probably a parallel subsystem ("avoid lookahead zones") that runs alongside the existing avoidance. Both get to modify the travel path. Adding temporal activation to the existing system would be more invasive.
- Z-hop: existing gcode injects Z-hop during long travels. Our multi-point travels are a sequence of short moves — each gets its own Z-hop? Or hop once at the start? Probably hop once; emit the intermediate moves as same-Z travel.
- Performance: wall-following with polygon queries per travel. Profile on multi-filament prints; optimize with spatial indexing (R-tree) if slow.

**Expected result:** travel moves contour around active zones; slicer log shows count of avoided/rerouted travels.

**Testable by**: slice a model where a direct travel would cross a tower; inspect gcode for multi-point travel instead of single G0.

#### 5e — Travel-impossible exception handling

**Technical details:**
- When wall-follower can't find a path (destination inside zone, or no exit from merged obstacle), call `m_plan.invalidate_and_replan(offending_tower_id, normal_boundary_layer)`.
- Phase 5 unwinds: discard buffered gcode for layers ≥ normal_boundary_layer, signal Phase 3 to re-plan, restart Phase 5 emission from layer N+1.
- Iteration cap (10) is shared with Phase 3's cap — if exceeded, disable lookahead for this slice.

**Open questions:**
- Per-layer gcode buffering: the existing parallel pipeline emits layers in order but may be generating ahead. Need to ensure we can roll back safely. Most conservative: serialize Phase 5 emission (no ahead-of-time work) during lookahead-enabled slices.
- Restart semantics: when we restart from layer N+1, we need to re-invoke `process_layer` for N+1..end with the new plan. This means treating Phase 5 as a single retryable unit.

**Expected result:** travel-triggered exceptions get absorbed by plan adjustments; logs record each re-plan iteration.

**Testable by**: construct a pathological model with forced travel conflicts; verify the final gcode has no forbidden travels and the log shows re-plans.

---

### Phase 6 — Post-processor: gcode reordering (location C)

The actual semantic change. Takes the Phase-5 gcode (which has correct markers and travel but original extrusion order), rewrites it into the lookahead-reordered final form. Gated behind a config flag.

#### 6a — Marker parser

**Technical details:**
- New files: `src/libslic3r/GCode/FilamentLookaheadPostProcessor.hpp/cpp`.
- Hooks into OrcaSlicer's existing `PostProcessor` pipeline (same place external post-processing scripts run).
- Line-by-line parse. Builds an intermediate structure: `ParsedGCode { header, vector<Layer { normal_spans, tower_blocks, layer_info }>, footer }`.
- `LOOKAHEAD_BLOCK_BEGIN/END` markers mark spans; `LOOKAHEAD_LAYER_INFO` gives per-layer metadata.
- Validates balanced markers; errors out on mismatched / unknown syntax.

**Open questions:**
- Parser tolerance: existing gcode has many comment conventions (OrcaSlicer's own `; TYPE:`, `; FEATURE:` etc.). Parser must ignore comments it doesn't recognize, not treat them as errors.
- Round-trip: parse → unparse should produce byte-identical gcode when no reordering is requested. Useful for debugging and baseline verification.

**Expected result:** parser faithfully reconstructs the gcode's logical structure.

**Testable by**: round-trip test — parse a known gcode, unparse without changes, byte-diff against input.

#### 6b — Block extraction and relocation

**Technical details:**
- For each `LOOKAHEAD_BLOCK` with `stack_index > 0` (i.e. an extra-layer zone, not a base), extract its gcode.
- Insert into the base-layer's tower section at the appropriate stack position (per `stack_index`).
- Tool change associated with the block's filament: if the block includes a tool change at its start, that tool change moves with the block. If the destination layer already has an active tool change for that filament, dedupe.

**Open questions:**
- Tool change gcode boundaries: where does a tool change start/end in the output? The wipe tower's toolchange gcode is a multi-line sequence. Study existing gcode to find the delimiter pattern.
- Retract state: moving a block means retract/unretract boundaries may shift. Need to preserve the retract state machine — probably wrap each relocated block in explicit retract/unretract even if originally unnecessary.

**Expected result:** relocated blocks appear at their destination layers in correct order.

**Testable by**: parse output, verify each tower's blocks are contiguous and in stack order at the base layer.

#### 6c — Z travel bracketing between original/destination layers

**Technical details:**
- At the tower's base layer, after normal printing completes, emit Z-raise moves to step through each extra-layer zone's Z in order:
  - `G1 Z<base_z>` (stay)
  - emit base zone extrusion
  - `G1 Z<base_z + layer_height>`
  - emit stack_index=1 zone extrusion
  - `G1 Z<base_z + 2*layer_height>`
  - emit stack_index=2 extrusion
  - ... etc
  - After last stack: `G1 Z<base_z + 0>` to drop back for subsequent normal printing.
- Feedrate from config (`travel_speed_z` or similar).
- Emit retract before Z-raise, unretract after final Z-return (per config).
- Add comment markers: `; LOOKAHEAD_STACK_Z_RAISE layer=K` / `; LOOKAHEAD_STACK_Z_RETURN` for debug.

**Open questions:**
- Z-hop on travel between stacks: if the base zone on layer L and the stack_index=1 zone on layer L+1 are at different XY positions, the travel between them happens at stack_index=1's Z (already raised). Should be fine — the whole tower area is clear (it's the tower's own extrusion).
- Max Z issue: cumulative Z-raise must not exceed printer limits. Bail via exception if it would.

**Expected result:** Z profile is a sequence of raises (tower growth) + a drop (return for normal printing) at every tower base layer.

**Testable by**: plot the gcode's Z coordinate over line number. Should show characteristic raise-raise-...-drop pattern at tower base layers.

#### 6d — Wipe tower per-layer rewrite *(Rule 10)*

**Technical details:**
- For each layer's wipe tower gcode block (identified by existing OrcaSlicer comment markers around it):
  - Parse the per-filament purge bands.
  - For each band whose filament is in `LOOKAHEAD_LAYER_INFO`'s `active_filaments` list: delete the tool-change + purge gcode.
  - For each deleted band's footprint: synthesize structural fill gcode using the next normal-mode or currently-loaded filament. Flow rate from the tower's infill config. Volume = footprint_area × layer_height.
- Output: same XY footprint of wipe tower, reduced tool changes, structural fill replacing skipped purges.

**Open questions:**
- Identifying the wipe tower's gcode section in the stream: look for `; WIPE_TOWER_START/END` or similar OrcaSlicer markers (need to confirm naming).
- Structural fill geometry: simplest is a serpentine infill at the tower's standard line width. Can reuse the tower's own infill pattern generator offline.
- Currently-loaded filament at structural-fill time: determined by parsing the gcode backward from the structural band's position — last `T<n>` or active tool at that point.
- Flush-into-infill deposits: if the base-gcode emission still emitted flush gcode (despite Rule 9's disable), we need to strip it. Probably already handled by 4b, but verify.

**Expected result:** wipe tower is ~same footprint but fewer tool changes and reduced purge volume per layer.

**Testable by**: count tool changes in wipe tower sections before/after; measure extrusion volume delta.

#### 6e — Config flag + disable fallthrough

**Technical details:**
- New config `filament_lookahead_post_process = true` (default).
- When false, Phase 6 does nothing — gcode is the Phase 5 output unchanged (markers still present but no reordering).
- When true, Phase 6 runs.
- Rationale: A/B verification, bug-out valve if 6a-6d have issues.

**Open questions:**
- UI placement: under "Advanced" process settings, near other post-processing options.

**Expected result:** toggle works; final gcode differs structurally only when on.

**Testable by**: slice same model with flag on/off, byte-diff. With off, gcode should match Phase 5 output directly. With on, gcode reflects reordering.

---

### Phase 7 — Polish and integration testing

Not new code per se; validates the feature works on real inputs and refines edge cases.

#### 7a — Multi-object plate testing

Slice plates with 2+ objects each having lookahead candidates. Verify no plan conflicts (two objects' towers in overlapping XY). If conflicts appear, extend the analysis to resolve them (probably already handled by zone-isolation rules, but worth verifying).

#### 7b — Parametric test matrix

Build a table of configurations (number of filaments, support types, clearance values, model geometries) and verify expected plan outputs for each. Probably via a CLI-driven test script that slices, parses gcode, and asserts counts (towers, tool changes, lookahead markers).

#### 7c — Verification prints on real hardware

Physical prints of representative models (multi-color paint jobs with disappearing extruders). Measure:
- Print time (expected reduction)
- Filament used (expected reduction in purge)
- Print quality (no artifacts from the reorder)
- Wipe tower structural integrity (visual inspection)

Compare against baseline (lookahead off) for the same model.

#### 7d — Failure mode verification

Construct and slice models that trigger each failure class:
- Blanket exception on various layers
- Travel-impossible exception
- Re-plan cascade exceeding iteration cap
- Max Z travel exceeded during stacking

Verify graceful degradation: user-visible messages, no silent corruption, no infinite loops.

#### 7e — Performance profiling

Slice typical MMU models with lookahead on; profile:
- Analysis time (Phase 3)
- Gcode emission overhead (Phase 5 marker + travel cost)
- Post-processor time (Phase 6)

Target: total slicing overhead from lookahead should be <10% of baseline slicing time.

---

**Phases 1–5 each leave the slicer functional and A/B-comparable to baseline at every step.** Phase 6 is where gcode semantics actually change; its config flag makes it optional during bring-up. Phase 7 validates behavior without introducing new code paths.

## What's currently implemented

- **Config options**: `filament_lookahead`, `filament_lookahead_max_height`, `filament_lookahead_clearance` are wired through `PrintConfig` and appear in the UI.
- **Analysis stage** (`src/libslic3r/GCode/FilamentLookahead.cpp`): `FilamentLookaheadPlan::build()` runs in `GCode::_do_export()` before layer iteration. Walks `PrintObject::layers()` and `support_layers()`, clusters entity bboxes into spatially connected groups, identifies disappearing-extruder candidates with per-layer cascade truncation. Support filament resolution uses concrete `support_filament` / `support_interface_filament` config values; "Any [Type]" supports are skipped in the current implementation (Phase 3a replaces this with proper Phase A/B resolution).
- **Cluster isolation filter**: each cluster checked against every other filament's entities on the same layer; only isolated clusters become exclusion zones.
- **Gcode comment emission**: `; LOOKAHEAD_EXCLUSION_ZONE x_min=... y_min=... x_max=... y_max=... z_max=...` emitted per affected layer from `GCode.cpp`; also populated directly into `m_processor.result().lookahead_exclusion_zones` for the in-memory GUI preview path.
- **GCodeProcessor parsing**: comments parsed back into the struct for file-loaded gcode.
- **GCodeViewer rendering**: zones drawn as yellow translucent rectangles, filtered by the vertical layer slider's visible range, toggleable via "Travel Exclusion Zones" in FeatureType / Speed / ActualSpeed legends.

## Future polish (post-v1)

These are known simplifications that can be revisited once the feature is functionally complete:

- **Per-layer contour exclusion zones.** Currently each tower has a single XY footprint (max envelope across all its layers). Cleaner visual + tighter travel routing would come from a per-layer polygon that contours the actual extrusion on that layer. Touches the `LookaheadEntry` struct (add `std::vector<BoundingBox>` or `std::vector<ExPolygons>` per layer), the renderer (`GCodeViewer`'s zone geometry currently uses a single `z_max` + single bbox per layer — would need varying XY per Z slab), and Phase 5d travel avoidance (layer-specific lookup). Defer to after Phase 7 polish.
- **Envelope-vs-clearance gap.** When the envelope grows beyond the base cluster bbox, another filament could theoretically sit within `min_clearance_distance` of the envelope but not of the base cluster — the cascade check wouldn't catch it. In practice this is rare since envelope growth is usually small relative to clearance. The per-layer contour fix above also resolves this.
- **Polygon-based isolation** (Phase 2.5). Currently isolation uses inflated bboxes. Upgrade to polygon offset + intersect if bbox approximation causes false positives in real prints.
- **Tower footprint auto-resize after lookahead planning.** With fewer tool changes, the wipe tower could be narrower. V1 accepts the over-sized tower.

## Open design questions

- **Layer adhesion timing**: printing multiple layers of one region while others wait could affect inter-layer bonding if the delay is too long. Cap max lookahead layers accordingly — unclear what the right ceiling is.
- **Travel-routing implementation hook**: Rule 8 specifies the algorithm. The question is which existing OrcaSlicer surface to hook into — `AvoidCrossingPerimeters` has the right polygon-avoidance shape but is object-level and static-per-layer. May need a parallel "avoid active lookahead zones" subsystem instead, or plumb active zones into the existing avoidance's obstacle set with temporal activation.
- **Pause/resume safety across software-initiated and printer-initiated pauses**, especially on Bambu firmware where Z-raise is hardcoded.
- **Tower footprint auto-resize after lookahead planning**: with fewer tool changes, the tower could be narrower. V1 accepts the over-sized tower; future optimization could regenerate the footprint post-planning.
- **Coordination with user-placed support blockers**: the auto-inserted tower blocker (step 11) needs to coexist with any user-placed blockers without collision or clobber.
- **Re-plan iteration cap tuning**: Rule 7's safety limit is suggested at 10; real prints may want different values. Worth observability: log iteration count per slice to calibrate.

## Files

- `src/libslic3r/GCode/FilamentLookahead.hpp/cpp` — analysis stage
- `src/libslic3r/GCode.cpp` — comment emission in `_do_export`, plan integration
- `src/libslic3r/GCode/GCodeProcessor.hpp/cpp` — comment parsing, `ExclusionZone` struct
- `src/slic3r/GUI/GCodeViewer.cpp` — zone rendering, layer-range filtering, toggle
- `src/libslic3r/PrintConfig.hpp/cpp` — config options

## Progress log

- **2026-04 Phase 1**: deleted ~120-line dead inline-printing block, removed debug `fprintf` spam. ~150-line net reduction. Analysis and visualization behavior unchanged.
- **2026-04 Phase 2**: added support-layer walk classifying `support_fills` entities by role (base vs interface) and mapping to object layers by `print_z`. Added per-layer cascade truncation in the disappearing branch. Gated off bbox-isolated-but-continuing strategy for v1. Skips "Any [Type]" supports with a known-limitation note (addressed in Phase 3a).
- **2026-04 Phase 3a**: Any-Type support bucketing + Phase A (zone claim) + Phase B (leftover assignment) building the `m_any_support_overrides` map. Consumer (Phase 5a) not yet implemented — map is produced and logged only.
- **2026-04 Phase 3c**: replaced disappearing-only strategy with greedy chained towers. Per-extruder while-loop accepts the tallest possible tower then advances past its top. Added self-content check in cascade (prevents "empty but clear" layers from extending towers), base-layer zone registration (Rule 8), and max-envelope zone sizing (closes under-coverage on widening towers). Added verbose `[FLA]`-tagged logging at warning/info per the development-logging mandate.
