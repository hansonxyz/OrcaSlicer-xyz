# Filament Lookahead — Multi-Layer Batched Printing for Multi-Material

Branch: `filament-lookahead`
Status: Phases 1, 2, 3a, 3b, 3c, 5a, 5c, 6a, 6b, 6c, 6d complete; 5b skipped. End-to-end pipeline shipping: cluster identification → tower planning → emission-time batching (Option B) → wipe tower co-existence. As of commit 756d90f3e4, `compare_slices.py` reports zero divergences on the tulip test pair: every (Z, tool) bucket of object+lookahead_tower extrusion matches baseline object extrusion to ≤0.0003 mm, and the lookahead optimization saves ~3% of wipe tower volume (182.88 mm out of 5980 mm). The previously-observed "wrong color bleed" at tower-base layers (LA tower 251 ext=0 base emitting layer 251's full T0+T3 region content) was traced to a routing fallback in `process_layer`: Phase 6d's filter on `m_wipe_tower_data.tool_ordering.layer_tools()` aliases through the `&` reference to `Print::m_tool_ordering`, removing upper-stack extruders from `layer_tools.extruders`; the fallback at GCode.cpp:5159 then reassigned their entities to `extruders.back()` (the tower's filament). Fix: skip the fallback when the missing extruder is identified as an LA upper-stack — the entity stays in unused island data and gets emitted only by the LA tower batch.

## Goal

On single-nozzle multi-material printers (e.g. X1C / A1 with AMS), when a region printed with one filament is spatially isolated from other filaments on the plate, print multiple consecutive layers of that region before switching filaments — instead of switching on every layer. Each skipped filament change saves purge material and ~30–60 seconds of tool-change time. On typical MMU painted models, a "disappearing extruder" case (e.g. a gold nub on top of a body) can eliminate *all* subsequent tool changes for that filament, saving hours and 50%+ of its purge.

## Development mandate — CLI headless testing for iteration

**Primary test file:** `C:\Users\brian\Desktop\obj_1_tulipan_mod.3mf`. This project is preconfigured with tree supports, 4 filaments including a PVA support-interface filament, multi-material regions that produce the disappearing-extruder case, and enough geometry variety (tall body + extras at mid-upper layers) to exercise the full spectrum of lookahead conditions: tower chaining, Rule 4 filament-completeness rejections, Any-Type support filament resolution (Phase 3a overrides), and the wipe tower tool-change sequence issues that Option B has to handle. Use it for the default iteration loop; fall back to other models only if chasing a scenario-specific regression.

Use `orca-slicer.exe --slice 1 --outputdir <dir> <model.3mf>` for **all mid-iteration verification** during implementation. The CLI drives the same `GCode::_do_export()` / `process_layer()` path that the GUI uses, so filament lookahead code exercises identically. Advantages vs manual GUI testing:

- No document-recovery prompt to dismiss.
- No click-to-slice; exit code indicates success/failure.
- Writes `[FLA]` / `[FLA-B]` / `[FLA-PP]` log lines to `C:\Users\brian\AppData\Roaming\OrcaSlicer\log\debug_*.log.0` just like GUI mode.
- Can be invoked in a background task so the Claude loop gets a completion notification.

**Standard dev iteration flow:**

1. Make code change.
2. `xyz/build_artifacts/build_incremental.ps1` (background task, wait for notification).
3. Run `orca-slicer.exe --slice 1 --outputdir <tmpdir> <test.3mf>` (background task, wait for notification).
4. Check exit code + read the latest log file from `AppData\Roaming\OrcaSlicer\log`.
5. If the output gcode is relevant, inspect it too (e.g. for `LOOKAHEAD_*` markers).
6. **Only when a full discrete sub-phase (e.g. Phase 6b, Phase 6d) is complete**, hand off to the user for manual GUI verification. The user confirms the preview + interaction are correct before the sub-phase ships.

Manual user tests are reserved for end-of-sub-phase verification, not for every build iteration during debugging.

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

**v1 implementation notes** (`GCode::route_around_lookahead_zones`):
- Hooked into `GCode::travel_to()` after `m_avoid_crossing_perimeters.travel_to()` and after retraction (which can re-trigger AVP). Runs only when lookahead is enabled and the current layer has registered zones — zero cost on prints without towers.
- Active zones come from `FilamentLookaheadPlan::exclusion_zones(layer_idx)`, which returns the bbox set registered during plan build for the current object-layer index.
- Each bbox is inflated by `SAFETY_MM` (0.5 mm) on top of the planning-time clearance polygon, then converted to a `Polygon` and unioned via `Clipper::union_` so overlapping/touching zones merge into single obstacles.
- For each segment of the input polyline: iterate. Test against all obstacles via `Geometry::segment_segment_intersection` per polygon edge. If any obstacle is hit, pick the obstacle vertex that minimizes `cur→vertex + vertex→destination` and that doesn't itself re-cross the same obstacle. Push that vertex into the result and advance the cursor.
- Iteration is bounded by `MAX_DETOUR_ITER = 32` per segment; on hit the helper logs a warning and emits the straight remainder. Rules 11/12 are designed to keep zone topology solvable, so this fallback should never fire in practice.
- Crossings of OTHER obstacles introduced by a vertex detour resolve naturally on the next iteration (the new cursor sits on the previously-crossed obstacle's vertex; the next test will find the next obstacle in the way).

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

### Rule 11 — Plate-edge clearance for tower exclusion zones

A candidate tower's exclusion zone, on every layer it covers, must keep at least `PLATE_EDGE_MARGIN_MM` (default 2 mm) clearance from the printable area's outer boundary. If the inflated zone reaches within margin of any plate edge on layer L+k, cap the in-progress tower at L+k-1.

**Rationale.** Travel routing in the GCode emitter (Rule 8) needs room to move *around* a tower's footprint without leaving the plate. An exclusion zone wedged against the plate edge has no clearance on that side; routing must go the other way. That alone is fine — but in combination with another zone elsewhere on the plate, an edge-locked zone can compose into a wall that cuts off plate area, effectively trapping any printing position behind the wall. Conservative: never accept an edge-locked zone in the first place. The 2 mm margin guarantees that on every side of every zone there's enough air for the head to route through, regardless of what other zones come in or out on later layers.

**Envelope growth.** A tower's exclusion zone grows monotonically as the cascade extends — each new layer's self-content can expand the envelope. A base zone that passes Rule 11 may fail it after extending by 5 layers, because the envelope picked up an extra millimeter of width. The cascade re-tests Rule 11 against the running envelope at every k. First failure caps at k-1.

### Rule 12 — No enclosed object pockets

The union of all exclusion zones active on a candidate layer (this candidate plus every previously-accepted tower whose base layer ≤ L+k ≤ top layer) must not split the plate's travel-allowed region into a topology where any object content not already inside an exclusion zone is unreachable from the plate's outside. If a candidate creates such a pocket, cap the in-progress tower at L+k-1.

**Rationale.** Three or more exclusion zones can be arranged into a ring around an unzoned object cluster. Once the ring is closed, every travel move from the plate's outside to that cluster has to cross at least one exclusion zone — which Rule 8 forbids. The result would be either an impossible-travel error or a forbidden zone-crossing. Reject the candidate before it closes the ring.

**Implementation (v1, bbox approximation).** At each candidate k:
1. Build polygons for all exclusion zones active on layer L+k (registered + candidate).
2. Compute `travel_allowed = bed_polygon - union(zones)`. Each `ExPolygon` is a connected travel-region component.
3. If only one component exists, no pockets exist — pass.
4. Else, classify the "outside" component as the one whose bounding contour touches the most plate edges (a heuristic that correctly identifies the perimeter-spanning component for normal printable areas).
5. For each non-outside component, check whether any object-content bbox on this layer NOT already inside a zone overlaps the component's bbox. If yes, the candidate strands that content — fail.

The bbox approximation is conservative: a pocket-overlapping object bbox that doesn't actually contain extrusion will produce a false-positive rejection (we cap a tower one layer earlier than strictly necessary). A false-negative — accepting a candidate that strands real extrusion — is impossible at the bbox level since extrusion is a subset of the entity bbox. False-positives reduce optimization opportunities but never corrupt the print; the v2 refinement to actual `Layer::lslices` polygons would tighten this further.

**Multi-cluster within same base layer.** Clusters within the same base layer evaluate sequentially; an earlier cluster's accepted zone is in `accepted_zones` but not yet registered in `m_raised_per_layer` when the next cluster's cascade runs. Rule 12 currently sees only registered (previous-base-layer) zones plus the candidate, missing intra-base-layer cluster interactions. Refine if real prints exhibit multi-cluster bases that conflict.

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

### Phase 3b ✅ — Filament-completeness (Rule 4)

Completed 2026-04-21. Precomputes per (layer, filament) whether the filament is fully contained in isolated clusters on that layer. A layer is incomplete if stray filament entities exist outside all isolated clusters — batching can't save anything on such a layer (a tool change to the filament is still needed for the stray). Phase 3c now rejects tower bases on incomplete layers and truncates cascades at the first incomplete layer. Typical results: ~40% of (layer, filament) pairs flagged incomplete, tower count reduced ~25% vs pre-3b.

**Verifiable by**: log shows `[FLA] Phase 3b filament-completeness: N complete, M incomplete` summary plus `REJECT: base layer not filament-complete` and `cascade truncate at k=N: not filament-complete` lines per decision.

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

#### 5a ✅ — Consume Any-Type override map *(Rule 2 obey-side)*

Completed 2026-04-21. `GCode.cpp:4699` now consults `m_lookahead_plan->override_for_support(&support_layer, role)` first and only falls through to `resolve_any_type_support_filament()` when no override exists. Plan key was changed from `(object, layer_idx, role)` to `(SupportLayer*, role)` for cleaner consumer-side access. Also injects overridden extruders into the local `tool_ordering.layer_tools()[...].extruders` list after plan construction, so the per-extruder gcode emission loop actually iterates for the overridden extruder. In practice `injected=0` is common because the overridden extruder is typically already active on that layer for model extrusion. `ToolOrdering.cpp:746` resolver was NOT modified — wipe-tower scheduling still uses the default resolver's choice, which means occasional purge-band mismatch but no correctness issue for the preview.

**Verified by**: user testing confirmed support colors in preview now match the override map (Any supports adjacent to a tower get the tower's filament; leftover supports use a non-lookahead-active on-layer filament).

#### 5a (original spec) — Consume Any-Type override map *(Rule 2 obey-side)*

**Technical details:**
- In `_do_export` around line 4699 where Any-Type support filament resolution happens, consult `m_plan.override_for_support(entity_id)` first.
- If an override exists, use that filament. Otherwise fall through to existing `resolve_any_type_support_filament()`.
- Same change applies inside `ToolOrdering` computation (runs earlier in `_do_export`) — need both paths to agree.

**Open questions:**
- `ToolOrdering` is built before `FilamentLookaheadPlan::build()` currently. Either: (i) build the plan before `ToolOrdering`, which means analysis happens even earlier; (ii) re-run `ToolOrdering` after plan construction with overrides applied. (i) is cleaner but may require refactoring plan construction to not depend on `ToolOrdering` output. Probably (ii) is less invasive for v1.

**Expected result:** supports near candidate zones get assigned to the matching filament. Visible in the tool change sequence in the final gcode.

**Testable by**: slice with lookahead, inspect tool change order, confirm the Any supports adjacent to candidate zones use the zone's filament.

#### 5b ⏸ — Normal-first print order within a layer *(Rule 8 steps 1–3)* — SKIPPED FOR V1

Skipped 2026-04-21 after initial attempt caused wipe tower generation failure. Root cause: the wipe tower pre-generates exactly N tool-change entries per layer based on the original `tool_ordering`; stable-partitioning the extruder list to put tower-mode last can introduce an extra tool change when the previous layer's final extruder is not first in the reordered list, overflowing the pre-gen array.

Revisited rationale for why 5b isn't needed in v1:
- **Flush-into-infill discharge ordering**: moot because Rule 9 Option 1 globally disables flush-into-infill when lookahead is on.
- **Travel predictability**: Phase 5d travel avoidance handles the active-zone-after-tower-printed case regardless of order.
- **Phase 6 insertion anchor**: `LOOKAHEAD_BLOCK_BEGIN/END` markers from Phase 5c give Phase 6 exactly the anchor it needs without depending on a specific extruder order.

Tower-mode tracking (`m_tower_filaments_per_layer` + `tower_filaments_on_layer()` accessor) was kept — it's used by Phase 5c to decide which spans to wrap with markers.

If Phase 6's post-processor rewrite ends up needing strict normal-first ordering anyway (e.g., for flush-into-infill conditional re-enable, Option 2 of Rule 9), revisit 5b by either (a) regenerating the wipe tower after reorder, or (b) doing the reorder inside `process_layer()` at emission time without mutating `tool_ordering`.

#### 5b (original spec) — Normal-first print order within a layer *(Rule 8 steps 1–3)*

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

#### 5c ✅ — Emit LOOKAHEAD_BLOCK markers *(step 8)*

Completed 2026-04-21. Emits `LOOKAHEAD_LAYER_INFO layer=L tower_filaments=<csv>` at the top of each layer, then wraps each extruder's iteration in the extruder loop with `LOOKAHEAD_BLOCK_BEGIN layer=L extruder=E base_layer=B stack_index=K extra_layers=N role=base|extra` and a matching `LOOKAHEAD_BLOCK_END`. Markers are emitted only when `m_lookahead_plan->tower_info_for(layer, ext_id)` returns a match, so layers without towers are unchanged.

**Verified by**: user tested on 4-filament model; gcode contains 392 LAYER_INFO lines (one per layer), 77 balanced BLOCK_BEGIN/END pairs matching the 77 tower layers across 9 accepted towers. Overlapping-towers-on-same-layer case (layer 244 has ext 3 base + ext 0 stack_index=2 extra) correctly produces two distinct BLOCK pairs.

#### 5c (original spec) — Emit LOOKAHEAD_BLOCK markers *(step 8)*

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

#### 6a ✅ — Marker parser

Completed 2026-04-21. New files `src/libslic3r/GCode/FilamentLookaheadPostProcessor.hpp/cpp` define `FilamentLookaheadPostProcessor` with `process(path)` that reads the gcode, parses `LOOKAHEAD_BLOCK_BEGIN/END` into paired `TowerBlock` records and `LOOKAHEAD_LAYER_INFO` into `LayerInfo` records, and writes the file back. 6a is a pure round-trip — no transform applied — so the foundation can be validated before 6b/6c layer semantic changes on top. Hooked in `GCode.cpp::_do_export()` just before the `path_tmp → path` rename; runs only when `filament_lookahead` is enabled.

**Verified by**: slice of 4-filament model produced 511,855-line gcode; parser reported `77 tower blocks, 392 layer-info markers` matching Phase 5c emission exactly; post-processed gcode has byte-identical line positions for marker locations vs pre-6a. Log shows `[FLA-PP] parsed N lines, M tower blocks, K layer-info markers` at warning level.

#### 6a (original spec) — Marker parser

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

#### 6b ✅ — Emission-time batching (Option B)

**Status:** superseded the original "block extraction and relocation" post-processor plan. Implemented via `emit_lookahead_tower_extras` called from `process_layer`. See section "Phase 6 architecture — analysis of integration options" and the Option B handoff at line ~1087 for rationale. Completed 2026-04-22.

**What shipped:**
- After a base layer's normal extrusion is emitted, iterate upper-layer tower stacks in-process.
- Per-stack state snapshot/restore of `m_layer`, `m_nominal_z`, `m_object_layer_over_raft`, `m_avoid_crossing_perimeters`, `m_extrusion_quality_estimator`.
- Per-stack sequence: emit `; Z_HEIGHT: <upper_z>` → `retract` → `travel_to_z(upper_z)` → `unretract` → build `by_region` filtered to `ext_id` → `extrude_perimeters` / `extrude_infill` / support emission → `retract`.
- Safe-exit: emit `; Z_HEIGHT: <base_z>` → `travel_to_xy(pre_batch_pos)` → `travel_to_z(base_z)` → `unretract`.
- `; Z_HEIGHT:` tags inside the batch ensure GCodeProcessor attributes each batched move to the correct layer's Z (otherwise all batched moves would carry the base layer's Z).

#### 6c ✅ — Upper-layer tower-mode skip

**Status:** completed 2026-04-22 alongside 6b.

**What shipped:**
- In `process_layer`'s extruder loop, consult the plan via `tower_info_for(layer_idx, ext_id)`.
- If `tower_info.stack_index > 0` (this filament on this layer belongs to a tower whose base is below), skip its perimeters/infill/support emission on this layer — the content is being batched into the base layer via Phase 6b.
- Continue emission of non-tower-mode filaments normally.

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

#### 7f — GCode viewer rendering polish on lookahead-tower layers

**Problem.** With Phase 6b shipping, batched upper-layer tower content physically prints at the correct Z but is emitted (in the gcode stream) inside the base layer's gcode block. GCodeProcessor attributes each move to a layer via `m_print_z` (driven by `; Z_HEIGHT:` comments), so moves get assigned to the correct layer-index in the processor's result — but the GUI viewer's "show me layer N" slider then presents a weird mid-layer cross-section: at layer 274 you see layer 274's normal extrusion PLUS the slice of tower 271's batched block that happens to be at Z=55.0, but only partially (only entities whose `m_print_z` matches). The effect is that towers look "half-rendered" or "sparse" on lookahead-tower layers, even though the gcode is correct and the physical print will be fine.

**Root cause.** `libvgcode::Viewer` and `GCodeViewer` slice the visualization by the `layer_id` / `print_z` field on each move. In Phase 6b, an upper-layer stack inside a base-layer's batch:
1. Carries the correct `m_print_z` (we emit `; Z_HEIGHT:` per stack, so the viewer buckets moves by the physically-correct Z).
2. BUT the layer range widget computes "which layers is this tower part of?" from the gcode's textual `LAYER_CHANGE` + `Z_HEIGHT` history, and the sequential nature of that history makes a tower's stack appear out-of-order when a user scrubs layer-by-layer (base layer shows the tower's full footprint across all Z slabs ABOVE it, then as you scrub up the slider the per-Z slab for each upper layer appears — but only from that one tower, not the full layer).

**Expected behavior.** When the user selects layer N in the viewer:
- Show ALL extrusion at Z ≈ N's print_z, regardless of which gcode block it was physically emitted in.
- This includes: layer N's own normal emission, any base-layer batch contributions whose per-stack `Z_HEIGHT` equals layer N's print_z, and any LOOKAHEAD_BLOCK content at that Z.
- The rendering should look identical to a non-lookahead slice of the same model at that layer, modulo the expected filament/order changes from the lookahead plan.

**Investigation areas.**
- `src/slic3r/GUI/LibVGCode/src/Viewer/Viewer.cpp` — how it maps layer slider → move subset.
- `src/slic3r/GUI/GCodeViewer.cpp` — overlay geometry and layer range widgets.
- `GCodeProcessorResult::moves[].layer_duration` / `print_z` fields — the per-move data model.

**Candidate approaches:**
1. **Layer-aware indexing by print_z** (viewer-side only). Post-process the `GCodeProcessorResult`: rebuild the layer slider model from `print_z` buckets rather than from sequential layer count. All moves at the same print_z bucket render together regardless of gcode-block order. Contained to the viewer; no gcode emission changes.
2. **Layer-view re-bucketing at processor level**. Have `GCodeProcessor` emit moves into a secondary structure keyed by `m_print_z` alongside the existing layer-index arrangement. The viewer picks this structure when any layer has lookahead-towers. Larger change, possibly wanted for other features too.
3. **Emit "virtual layer change" markers** that tell the viewer to group batched moves by print_z even though they came from a single gcode block. Uses the existing marker pipeline; requires viewer work too.

**Verification:**
- Slice the tulip test model with lookahead on.
- At layer 274 (inside tower 271's extra range), the viewer should show: layer 274's normal T0 extrusion + the ext=3 contribution at the correct XY footprint of tower 271. Should match the non-lookahead slice's silhouette at that Z.
- Scrub the layer slider through layers 271–279; each stack layer should show its Z slab of tower 271 plus normal content.
- Frame rate and memory should stay within current budget.

**Priority.** Cosmetic — the actual gcode is correct and prints correctly. Fix before v1 ships so users' confidence in lookahead isn't undermined by misleading previews.

---

**Phases 1–5 each leave the slicer functional and A/B-comparable to baseline at every step.** Phase 6 is where gcode semantics actually change; its config flag makes it optional during bring-up. Phase 7 validates behavior without introducing new code paths.

## What's currently implemented

- **Config options**: `filament_lookahead`, `filament_lookahead_max_height`, `filament_lookahead_clearance` are wired through `PrintConfig` and appear in the UI.
- **Analysis stage** (`src/libslic3r/GCode/FilamentLookahead.cpp`): `FilamentLookaheadPlan::build()` runs in `GCode::_do_export()` before layer iteration. Walks `PrintObject::layers()` and `support_layers()`, clusters entity bboxes into spatially connected groups, identifies disappearing-extruder candidates with per-layer cascade truncation. Support filament resolution uses concrete `support_filament` / `support_interface_filament` config values; "Any [Type]" supports are skipped in the current implementation (Phase 3a replaces this with proper Phase A/B resolution).
- **Cluster isolation filter**: each cluster checked against every other filament's entities on the same layer; only isolated clusters become exclusion zones.
- **Gcode comment emission**: `; LOOKAHEAD_EXCLUSION_ZONE x_min=... y_min=... x_max=... y_max=... z_max=...` emitted per affected layer from `GCode.cpp`; also populated directly into `m_processor.result().lookahead_exclusion_zones` for the in-memory GUI preview path.
- **GCodeProcessor parsing**: comments parsed back into the struct for file-loaded gcode.
- **GCodeViewer rendering**: zones drawn as yellow translucent rectangles, filtered by the vertical layer slider's visible range, toggleable via "Travel Exclusion Zones" in FeatureType / Speed / ActualSpeed legends.

## Phase 6 architecture decision — tower emission

Phase 6's goal is to produce valid gcode for batched towers: at the base layer, after normal extrusion, print the tower's base, then each upper-layer slice of the tower at a raised Z, then return to base layer Z outside the tower's exclusion zone. This raises four correctness problems beyond "move text around":

### Problem 1 — Wipe tower purges carried in relocated blocks

In the current Phase 5c emission, each extruder iteration wrapped by `LOOKAHEAD_BLOCK_BEGIN/END` contains, at its start, the wipe-tower tool-change gcode (the purge deposits filament onto the wipe tower at that layer's Z). If we relocate the full block from layer L+1 to layer L (to batch it with the base), the baked purge says "go to (X, Y, Z_L+1) on the wipe tower and extrude", but **wipe tower layer L+1 hasn't been built yet** at the base layer's timeline — extrusion would deposit in thin air and collide with the physical wipe tower's top.

Required behavior (per user spec):
- The single real tool change to the tower's filament happens once at the base layer (when entering the tower batch).
- **No tool change, no purge** happens on upper-layer positions for the tower's filament.
- The wipe tower's *bridging / purge lines* for those skipped tool changes must not be emitted.
- The wipe tower's *structural contour edges* on those layers — the perimeter / wall that the bridge lines anchor to — **must still be printed** during normal-layer emission of each upper layer, using whichever filament is actively printing there. Without that structural fill, the wipe tower would have missing layers and the tower above wouldn't stand.

### Problem 2 — Retraction state across block boundaries

The emission pipeline runs with implicit retract/unretract state carried across layers. Moving a block out of its original context leaves both sites with inconsistent retract assumptions:
- At the relocation site (base layer, just after the base block's last extrusion), we may be in "retracted" or "not retracted" state depending on where the base block ended; the relocated extra-layer block assumes its own entry state.
- At the original site (upper layer, where the block used to be), the surrounding code expected certain retract state between adjacent iterations.

v1 fix: wrap each relocated block with explicit retract-before and unretract-after; strip internal retract state so it's self-contained.

### Problem 3 — Z context and safe exit

Phase 5c markers now include `z=<print_z>` so the post-processor knows each block's intended Z. Between stack levels we emit `G1 Z<base_z + k·layer_height>`. After the last stack, Rule 8 says we park the nozzle **outside the tower's exclusion zone** before dropping Z back to base.

Safe-exit XY: cache the position just before entering the base block (that point is by definition outside the tower's inflated zone — it's where the previous normal-mode emission ended).

### Problem 4 — Seam placement within a tower

Each stack level's extrusion was sliced independently with its own seam choice. When batched, the seams on consecutive stack levels won't align vertically, producing a ragged seam surface across the tower's height. This is a v1 aesthetic compromise documented here; cleaner seam alignment is future polish (Phase 7 or beyond).

### Planned solutions

**For Problem 1 (wipe tower):**
- **Amend Phase 5c** to emit *inner markers* wrapping only the object+support extrusion portion of each iteration (after the wipe-tower tool-change prefix). Relocate only the inner span.
- The outer BLOCK_BEGIN/END + tool-change prefix stays at the original layer. The original layer ends up doing an unnecessary tool change — wasted purge — but the purge lands at the *correct* Z for its wipe tower layer (the slab exists by the time it runs).
- **Phase 6d (Rule 10 wipe tower rewrite)** eliminates those wasted purges by: stripping the entire tool-change + purge text for filaments that end up being tower-mode (lookahead-active) on that layer, and adding a replacement structural fill using a non-tower filament already printing on that layer. The *structural contours* of the wipe tower (walls enclosing purge bridges) are preserved.

**For Problem 2 (retraction):**
- Before each relocated block's content: emit `G1 E-<retract_length>` (retract).
- After the last stack and Z-return: emit `G1 E<retract_length>` (unretract).
- The block's internal retract/unretract state is unchanged — we just bracket it.

**For Problem 3 (Z + safe exit):**
- Cache nozzle XY at base-block entry.
- Between stacks: `G1 Z<base_z + k·layer_height>` bracketing.
- After last stack: retract → `G1 X<cached.x> Y<cached.y>` → `G1 Z<base_z>` → unretract.

**For Problem 4 (seam):** accept for v1, document.

## Phase 6 architecture — analysis of integration options

The big question is *where in the pipeline* tower batching should happen. Three candidates, evaluated against the actual OrcaSlicer architecture (not hypothetical).

### What the current architecture offers

Observed from `src/libslic3r/GCode.hpp` and `src/libslic3r/GCodeWriter.hpp`:

- **`GCode` class has public extrusion methods**: `extrude_perimeters(print, by_region, ...)`, `extrude_infill(print, by_region, ironing)`, `extrude_support(support_fills, role)`, and the lower-level `extrude_entity(entity, ...)`. These take extrusion data and return gcode as a `std::string`. They operate on any `ExtrusionEntity` — not tied to a specific layer's data.
- **`GCodeWriter` exposes Z / retraction / position control**: `travel_to_z(z)`, `retract()`, `unretract()`, `lift()` / `unlift()`, `set_position(Vec3d)`, `get_position()`. So we can place the nozzle anywhere between extrude calls.
- **`Print` is fully in-scope during emission**: `print.objects()[o]->layers()[i]` gives any layer's extrusion regions; `print.objects()[o]->support_layers()[i]` gives support regions. The emitter is not restricted to the "current" layer's data.
- **The layer pipeline (`process_layers`) is `tbb::parallel_pipeline` with serial_in_order output**: generators run in parallel, filters run in order, output is serialized. A `process_layer` call for layer L can't rely on the output of layer L-1 having been written, but it has full read-only access to `Print`.
- **ToolOrdering is pre-computed in `Print::process()` (psWipeTower stage)**, stored on `Print` as `m_tool_ordering`. Wipe tower gcode is baked from that ordering at that time.

### Option A — Post-process text (current Phase 6a direction)

Read the emitted gcode file, parse LOOKAHEAD markers, rewrite blocks, write back.

**Pros**
- Smallest invasion into the existing pipeline — the gcode emitter is unchanged.
- Runs after all filters (cooling buffer, pressure equalizer, fan mover) have done their work, so we don't fight them.
- Easy to A/B compare by turning the flag off (gcode is byte-identical pre-Phase-6a).
- Can be written, tested, and disabled in isolation.

**Cons**
- Text manipulation is fragile against upstream format drift — OrcaSlicer comment conventions change.
- Retraction state is implicit in the stream; we have to bracket with explicit retract/unretract to make it safe (Problem 2).
- Wipe tower purges embedded in the text are baked with specific Z coordinates; relocating them is unsafe (Problem 1), so we need to leave the tool-change prefix in place and rely on Phase 6d to strip later.
- Seam, retraction, and pressure advance decisions are frozen at their original layer's context. When the block moves, those decisions don't re-evaluate.
- We don't get to *generate new extrusion gcode* through the existing machinery — anything novel (structural fill for wipe tower gaps) has to be synthesized by hand.

**Feasibility**: works for v1 with known compromises (wasted purges, ragged seams, explicit retract brackets). Phase 6b+6c+6d doable in this mode with escalating complexity.

### Option B — Emission-time batching (in `process_layer`)

At the base layer's emission, after the base's own tower extrusion is written via the existing extrude methods, directly call `extrude_*` for the upper-layer tower extrusions at the raised Z, with Z manipulation bracketed via `GCodeWriter::travel_to_z()` + `retract()` / `unretract()`. Skip F's emission on upper layers.

**Pros**
- Reuses the existing extrusion machinery (`extrude_perimeters`, `extrude_infill`, `extrude_support`) — retraction, wipe-before-external-loop, pressure advance, cooling fan, and seam-placer all run naturally on the raised-Z extrusion. State machines stay consistent because we're still inside the emitter's normal flow.
- Z + position control is first-class (`travel_to_z`, `set_position`, `retract`).
- Can "skip F's emission" on upper layers cleanly by consulting the plan inside `process_layer`.
- No text manipulation, no parsing fragility.
- Emission of wipe-tower structural fill (Problem 1's real fix) can use the same existing extrude infrastructure.

**Cons**
- Invasive: `process_layer` is already a big, intricate function. Adding a "batch upper layers' extrusion at raised Z" branch is non-trivial — ~200-400 LOC.
- Interacts with the parallel pipeline: if we batch L+1's F-extrusion into layer L's emission, L+1's `process_layer` shouldn't emit F but still needs to emit everything else. Two layers end up cross-communicating through the plan; each layer's output is still a self-contained gcode string, so it's fine for serial_in_order output, but we have to be careful that L+1's skip doesn't leave orphan state (e.g. expected tool changes).
- ToolOrdering's pre-computed sequence assumes the original layer structure. A tool change to F expected on L+1 won't happen (F is already active from layer L's batch). The wipe tower's pre-generated tool-change index needs matching adjustment — this was the root cause of the original Phase 5b failure. Fix: regenerate ToolOrdering + wipe tower after the plan is built, OR teach the wipe tower to skip pre-generated entries flagged by the plan.
- Writer state (retraction, position, cooling) at the end of base-layer batching must be valid for the subsequent emission (other extruders on the same layer or next layer's layer-change). The retract/unretract bracketing around the batch makes this self-contained but needs care.

**Feasibility**: moderately invasive but architecturally clean. Each sub-problem has a natural home:
- Problem 1: the "skip F on upper layer" logic lives in `process_layer`; structural fill for wipe tower can be synthesized via existing `extrude_*` calls.
- Problems 2, 3: `GCodeWriter::retract`, `travel_to_z`, cached XY — all directly supported.
- Problem 4: seam still per-slice but easier to mitigate (could re-run seam-placer on the raised-Z entities with base-layer's seam hint).

### Option C — Sub-renderer / recursive slicing (what you asked about)

Conceptually: at Phase 6 time, for each tower, spin up a second instance of the emission pipeline, feed it "just the tower's extrusion entities" with shifted Z, capture its gcode, splice it into the main stream.

**What the architecture does NOT support today**
- `GCode` is not designed as a reusable, standalone emitter. It mutates its own state during `process_layer` and expects to run once over a `Print`. It isn't reentrant — a second `GCode` instance sharing writer state would corrupt.
- `Print::process()` is heavy: validation, per-object `PrintObject::process()` (posSlice through posDetectOverhangs), plate-level `psWipeTower` and `psSkirtBrim`. You wouldn't want to run it a second time for just a tower.
- The slicer is a library, not an external program. You can't fork a sub-process cheaply; invoking "the slicer" recursively means constructing a new `Print` and running its pipeline, which is heavy and assumes access to the whole Model / Config.

**What would need to change for Option C to be viable**
- `GCode` emitter would need to become re-entrant: hoist state-machine fields (current position, filament, retraction, cooling) into a per-emission `EmitterState` struct; constructor takes state-or-default. Big refactor.
- `Print` would need a mode where `process()` only runs a subset of stages (the tower's extrusion entities already exist in `PrintObject::layers()`; we don't need to re-slice geometry, just re-emit gcode for a subset of them). That's plausible to factor out.
- Merging the sub-emitter's output back into the main stream requires careful handoff of writer state (position, retraction, temperature, fan). Doable if both emitters use a shared `EmitterState`.

**Pros (if we did it)**
- Architecturally most correct: the sub-renderer gets the full benefit of seam placement, retraction, pressure advance, pathing optimization as if it were a normal slice.
- Composable: same mechanism could be used for other recursive-emission features in the future (per-object sub-extrusion etc.).

**Cons**
- Large refactor of `GCode` (several thousand LOC touched, many subtle state dependencies).
- High risk of introducing regressions in the existing emitter that affect all slicing.
- Timeline: probably 2–4 weeks of engineering even before the tower-specific logic is wired in.
- Not something to attempt as part of a v1 feature that's otherwise small.

### Recommendation

**Target Option B for v1.** It reuses existing infrastructure (solving the retraction/seam/pressure-advance concerns naturally), solves Problems 1–3 directly with existing tools, and lands as a bounded ~200–400 LOC change inside `process_layer` rather than a pipeline-wide refactor.

**Keep Option A's post-processor (Phase 6a) as a *shadow* for verification**: leave the LOOKAHEAD markers emitted, and let the post-processor verify invariants (balanced blocks, Z sequence matches what Option B emitted) without mutating. Cheap sanity net.

**Option C is a v2/v3 target** once the feature is proven and there's appetite for the refactor. Noted for future.

### Handoff to Phase 6 implementation

If we go with Option B, the sub-phases reshuffle:

- **6b (new)**: inside `process_layer`, after base-block's extrusion is emitted, iterate upper-layer tower extrusions; for each stack k, emit via `extrude_perimeters` / `extrude_infill` / `extrude_support` with `m_writer.travel_to_z(base_z + k·lh)` bracketing and `retract` / `unretract` guards. Cache `m_writer.get_position()` before base block for safe exit.
- **6c (new)**: on upper layers, when `process_layer` would normally emit F's extrusion, consult the plan — if F is tower-mode here, skip all its perimeters/infill/support. Emit only the non-F extrusions.
- **6d**: wipe tower per-layer rewrite (Rule 10). Still needed — the pre-baked wipe tower has purges for tool changes that won't happen. Either regenerate the wipe tower after plan construction (spec's Option 1) or post-process-rewrite (spec's Option 2). Option 1 is cleaner given we're already doing Option B.
- **6e**: `filament_lookahead_post_process` config flag stays useful — when off, skip Option B's batching, producing baseline gcode for A/B comparison.
- **6f (deferred, v2)**: `ToolOrdering` regeneration with lookahead-adjusted input — see section below.

Phase 6a's post-processor and marker parser stay as the verification shadow, not the primary transform.

### Phase 6g — Flush-into-infill / flush-into-supports handling *(deferred, investigation required)*

**Current Rule 9 spec:** v1 globally disables flush-into-infill and flush-into-supports whenever `filament_lookahead` is enabled (the blunt hammer). User feedback: this is heavier than necessary. A per-layer approach should be feasible.

**Refined requirement:** on any layer that contains a tower (base OR extra), flush-into-infill and flush-into-supports should not be used *for filaments whose flush destination is the tower's zone* (the tower itself or supports sharing the tower's filament). Purge discharge should go to the wipe tower instead. The remaining layer artifacts (regions of other filaments not involved with any tower) could still use flush-into-infill/supports for their own tool changes *if feasible in the code*.

**Open investigation questions (to address when this phase is picked up):**

1. Where in the code does "flush-into-infill" mark a target region? Is the decision made per tool change (dynamically at emission time) or precomputed (in slicing or ToolOrdering)? If dynamic, we can inspect the target region and veto it for tower-involved tool changes; if precomputed, we'd need to re-run that precomputation with lookahead constraints.
2. Does the flush-into-infill/support mechanism operate on *destination region* (where discharge lands) or *source tool change* (which switch triggers discharge)? The filter for "don't discharge into a tower's zone" needs one or the other.
3. On the base layer of a tower, the single real tool change TO the tower's filament could legitimately flush-into-infill for *other objects' infill on that base layer* — as long as the flush doesn't land inside the tower's zone. Is that filter doable?
4. Support filaments that match a tower's filament (via Phase 3a override) become part of the tower's batch. Flush-into-supports using those supports would need to be vetoed for the same reason.

**Fallback if per-layer is infeasible:** keep Rule 9 Option 1 (global disable). Current spec already handles this.

**Why deferred:** the investigation depends on understanding how flush-into-infill is wired in OrcaSlicer's emission pipeline, which hasn't been explored yet. Landing Phase 6b+6c first gives us a working baseline; we can then decide based on actual user workflow whether the per-layer refinement is worth the complexity.

### Phase 6f — ToolOrdering regeneration *(deferred to v2)*

**What and why.** OrcaSlicer's `ToolOrdering::reorder_extruders_for_minimum_flush_volume` (in `src/libslic3r/GCode/ToolOrderUtils.cpp:605`) is a flush-volume minimizer, not a tool-change-count minimizer. It uses the full `flush_matrix` (per-pair purge volumes — e.g. Red→Yellow 20mm³ vs Black→White 800mm³) to compute the globally optimal per-layer filament order via dynamic programming with one-layer forecast. The existing algorithm is strictly better than any heuristic we'd write; we should keep and reuse it.

Our problem: by the time `FilamentLookaheadPlan::build()` runs (inside `_do_export()`, after `Print::process()` completes), `ToolOrdering` has already been built based on the *original* per-layer filament sets — including filaments that will later be batched into a tower. When Phase 6b/6c skip a tower-mode filament's emission on upper layers, the original ordering is left with "gaps" where a tool change was scheduled but no extrusion happens. The remaining filaments' ordering is not re-optimized for the new reality.

**v1 consequence.** Slightly suboptimal tool-change sequence on layers where we've skipped a tower-mode filament. Example: pre-lookahead the flush minimizer chose `R → Y → B` on layer L+1 to minimize `prev→R + R→Y + Y→B`. If we skip R (tower-mode on L+1), the remaining sequence is `Y → B`, but the optimal for *just* `{Y, B}` given the previous layer's last filament might have been `B → Y`. We eat the inefficiency. Not a correctness issue — just extra purge volume.

**v2 fix.** Re-run `reorder_extruders_for_minimum_flush_volume` after the plan is built, with `layer_filaments` adjusted to reflect the skips:

- For each layer L+k where filament F is tower-mode (F's tower has `base_layer < L+k` and extends through L+k): remove F from that layer's filament set.
- For each base layer L where F's tower starts: F stays (it's being printed there, just with upper layers batched in).
- Feed the adjusted `layer_filaments` back into the reorder function.
- Update `m_layer_tools[...].extruders` with the new sequence.

**Why it goes with 6d.** The spec's Phase 6d offers two paths for wipe tower handling:
- **Option 1** (regenerate wipe tower with lookahead-aware ToolOrdering) — requires this regeneration as a prerequisite.
- **Option 2** (post-process rewrite) — doesn't need ToolOrdering changes.

v1 ships with 6d Option 2 (text-based wipe tower rewrite). v2 upgrades to 6d Option 1, which naturally bundles 6f. Doing them together in a single v2 pass is cleaner than a standalone 6f — the wipe tower is regenerated from the re-optimized ToolOrdering in one coherent step.

**Rough invasiveness.** Low-to-moderate. The reorder function already takes `layer_filaments` as input; we build an adjusted vector from the plan and call it again. Then swap results into `tool_ordering.layer_tools()`. Wipe tower regeneration from the new order is a bigger subtask (calls into `Print::m_wipe_tower_data` population, currently in `Print::process()` under `psWipeTower`).

**Verification.** Pre-6f slice vs post-6f slice on the same model: total purge volume (from slicer statistics) should decrease on prints with active lookahead towers. On prints without active towers, ordering should be byte-identical (no tower-mode filaments to remove).

## Dev-mode wipe tower disable (TEMPORARY — remove checklist)

During Option B tower-emission development, the wipe tower is **forcibly disabled** at slice time whenever `filament_lookahead` is on. The wipe tower's pre-generated tool-change array is baked from ToolOrdering's view of the filament sequence, which conflicts with our skipping of tower-extra iterations. Rather than continue patching around it (which produced wrong-filament bugs and required fragile workarounds like fake-toolchanges, force-T emissions, and end-of-layer T syncs), we're pulling it out of the development loop entirely so we can iterate on tower generation cleanly. The wipe tower will be re-integrated properly via the "wipe tower reborn" phase below.

**Where the hack lives:**
- `src/libslic3r/Print.cpp`, top of `Print::process()`. Block is tagged with a `FLA-DEV-HACK` comment.
- Primary trigger: when `m_config.filament_lookahead.value == true` AND `m_config.enable_prime_tower.value == true`, we force `enable_prime_tower = false` via `const_cast`.
- Secondary trigger: env var `FLA_FORCE_NO_PRIME=1` forces prime tower off regardless of lookahead state. Used to slice no-lookahead baselines for dev comparison (baselines need prime tower off too, so they can be diffed against lookahead-on output apples-to-apples).
- Emits log line `[FLA-DEV-HACK] forcibly disabling enable_prime_tower for this slice (trigger=...)`.

**Consequences during this dev period:**
- Output gcode is **NOT printable** — no wipe-tower purge means colors contaminate at every tool change. Development-only output.
- Real tool changes emit bare `T<n>` commands via the normal (non-wipe-tower) extruder path. No TCRs consumed, no pre-gen array, no mismatch possible.
- All lookahead analysis (3a, 3b, 3c), emission markers (5c), batching (6b/6c), and debug tools continue to work normally.
- `layer_regions.py` and `lookahead_towers.py` are the primary verification tools; compare output against a baseline sliced with `filament_lookahead` off for ground-truth filament assignments per region.

**Removal checklist** — when "wipe tower reborn" (Phase 6f-combined) is implemented, delete the `FLA-DEV-HACK` block in `Print::process()` and verify:
1. `filament_lookahead = true` with `enable_prime_tower = true` produces gcode whose wipe tower TCR sequence matches what our emission actually does (no `append_tcr` mismatch assertions).
2. Baseline (lookahead off) vs lookahead gcode have the same per-region filament assignments at every layer.
3. Wipe tower is structurally complete on every layer (no missing walls on layers where a tower-extra was skipped).
4. Total purge volume with lookahead on is LESS than baseline (prove the feature saves material).

**Other hacks already removed when this dev-mode hack was introduced:**
- `WipeTowerIntegration::skip_tool_change_for_tower` — the fake `writer.toolchange()` call inside. (Method still exists as a no-op-when-no-wipe-tower safety net.)
- Force `T<ext_id>` emission at the start of each tower base iteration (in `process_layer`).
- End-of-layer `T<writer.filament()>` emission in `process_layer`.

These patch-level fixes for the wipe tower/skip divergence are not needed while the wipe tower is disabled, and won't be needed once regeneration ships.

## Phase 6 emission refactor — Option 3 (sub-render) for upper-layer tower extras

**Motivation.** Earlier iterations of `emit_lookahead_tower_extras` called `extrude_entity` directly on upper-layer extrusion entities while the GCode object's implicit per-layer state (`m_layer`, `m_extrusion_quality_estimator`, seam-placer state, `m_nominal_z`, `m_origin`, `m_avoid_crossing_perimeters` layer init, etc.) was still configured for the BASE layer. Symptoms observed on the tulip test model:

- ~13% total extrusion deficit (lookahead output vs lookahead-off baseline with prime tower forced off via env var).
- Each emitted move had 30–50% less E per move (overhang quality estimator active on wrong layer's geometry → false overhang detection → flow reduced).
- Per-Z move counts went UP while per-Z E total went DOWN (finer path splitting from wrong-context seam + PA).
- Gcode viewer silently blanked out after rendering, likely from `GCodeProcessor`/`libvgcode` choking on the context-incoherent extrusion stream.

Hacks accumulated trying to patch around this (fake-toolchange, force-T at tower base, end-of-layer T sync, m_nominal_z swap) each solved a symptom and created another. The underlying problem is that `process_layer`'s extrusion helpers assume they run in the context of the layer the entities come from, and we were violating that.

**Chosen fix: sub-render.** Extract the per-extruder-per-instance emission body of `process_layer` into a reusable method. Invoke it from both `process_layer`'s normal extruder loop AND from `emit_lookahead_tower_extras`, with proper per-layer state snapshot/restore when called from the batch.

### Stage 1 — rollback checkpoint

Committed at `3ecfde8514` on branch `filament-lookahead`. Contains all partial Option B work + debug tools + FLA-DEV-HACK. If the refactor goes sideways, rollback target is this commit.

### Stage 2 — extract `emit_extruder_on_layer` helper

Factor the per-instance-per-extruder body from the middle of `process_layer`'s extruder loop (currently ~lines 5120–5460 of `GCode.cpp`) into a new method:

```cpp
std::string GCode::emit_extruder_on_layer(
    const Print                 &print,
    const std::vector<LayerToPrint> &layers,
    const LayerTools            &layer_tools,
    ObjectByExtruder            &object_by_extruder,
    size_t                      layer_id,
    size_t                      instance_id,
    unsigned int                extruder_id,
    bool                        first_layer,
    bool                        print_wipe_extrusions,
    // ...any other context variables needed...
);
```

The helper does **only** the extrusion work — no skirt generation, no wipe-tower toolchange construction, no custom gcode substitution, no timelapse insertion. Those remain in `process_layer`.

The helper's responsibilities:
- Snapshot any per-layer GCode state it will mutate (`m_layer`, `m_config` region apply, `m_avoid_crossing_perimeters.init_layer`, `m_extrusion_quality_estimator.set_current_object`, `m_last_obj_copy`, `m_origin`).
- Set those state fields for the target `(layer, instance, extruder)`.
- Emit `extrude_perimeters` → timelapse-in-infill hook → `extrude_infill` → second `extrude_perimeters` (is_infill_first path) → ironing `extrude_infill`.
- Emit `extrude_support` if the `object_by_extruder.support` is present.
- Emit object start/end labels (if `gcode_label_objects`).
- Do NOT restore state — caller is responsible (simpler, since the normal caller in `process_layer` immediately moves to the next instance anyway).

Initial refactor: `process_layer` calls the helper instead of running the body inline. Expected result: byte-identical gcode output vs pre-refactor for a normal slice. Verify via `debug-tools/` diff tools before moving on.

### Stage 3 — rewrite `emit_lookahead_tower_extras` to use the helper

For each stack `k = 1..extra_layers`:

1. Compute `upper_li = base_layer_idx + k`, `upper_z = layers[upper_li]->print_z`.
2. Build `by_extruder` for the upper layer, filtered to `ext_id` only. This mirrors the block at `process_layer:4872–5115` that populates `by_extruder` from `LayerToPrint`s, `LayerRegion`s, and support data.
3. Snapshot the current per-layer state fields.
4. Emit `retract()` → `travel_to_z(upper_z)` → `unretract()`.
5. Call `emit_extruder_on_layer(print, synthesized_layers_vec, layer_tools, ..., ext_id, ...)` — the helper correctly sets up `m_layer = upper_layer`, `m_extrusion_quality_estimator` for that layer, etc.
6. Restore the snapshotted state fields.
7. `retract()` at end of stack.

After last stack: safe-exit (travel to cached XY, drop Z, unretract) as today.

### Stage 4 — validation

- Slice the standard test model; compare lookahead output to lookahead-off baseline (via `FLA_FORCE_NO_PRIME=1`). Expect total extrusion to match within rounding, not the previous 13% deficit.
- Per-Z move counts + per-Z E sum should match baseline in the tower-free Z ranges and represent redistributed-but-equivalent extrusion in the tower Z ranges.
- Gcode viewer should render fully and stay rendered under panning.
- Tower XY bounds, filament assignments, and layer coverage should match analysis expectations from `debug-tools/lookahead_towers.py`.

### Stage 5 — cleanup after validation

- Remove the `m_nominal_z` save/restore hack (the helper sets it naturally via its per-layer setup).
- Remove the `skip_tool_change_for_tower` method's no-op body (still a no-op when prime tower is off, but the infrastructure is now unused).
- Keep FLA-DEV-HACK — still needed until Phase 6d/6f (wipe tower reborn) ships.

## Phase 6 revised order — wipe tower reborn is LAST

With the dev-mode hack in place, Phase 6 work now proceeds as:

1. **6a ✅** (done) — post-processor scaffolding + marker parser (shadow verifier).
2. **6b+6c ✅** (done) — `emit_lookahead_tower_extras` in `process_layer`: batched tower emission at raised Z with retract brackets and safe-exit.
3. **6e (config flag) — skipped for v1**; folded into `filament_lookahead` directly.
4. **Iteration + polish (current):** region filtering correctness, entity walking, exclusion zones, travel avoidance, seam — all verified via CLI + debug tools against baseline gcode.
5. **Wipe tower reborn (final step):** combines the former Phase 6d (Rule 10 wipe tower rewrite) and Phase 6f (ToolOrdering regeneration with lookahead-adjusted `layer_filaments`). Specifically:
   - Move `FilamentLookaheadPlan::build()` earlier — run it in `Print::process()` before `psWipeTower`.
   - Adjust `layer_filaments` to remove filaments whose extrusion was batched into a lower base layer.
   - Let `ToolOrdering::reorder_extruders_for_minimum_flush_volume` run on the adjusted input.
   - Run the normal wipe tower generator — now produces TCRs that match our emission.
   - Remove FLA-DEV-HACK.
   - Verify via removal checklist above.

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
- **2026-04 Phase 5a**: consume Any-Type override map in `GCode.cpp:4699` (short-circuits the default resolver); inject overridden extruders into `tool_ordering`'s per-layer extruder lists (safeguard for cases where the override extruder isn't already active on the layer). Plan key refactored from `(object, layer_idx, is_interface)` to `(SupportLayer*, is_interface)` for cleaner consumer access. Preview now reflects Any-Type overrides.
- **2026-04-21 Phase 3b**: filament-completeness precomputation. Towers now gated by Rule 4 — rejected at bases where stray extrusion exists outside isolated clusters, truncated in cascades when a subsequent layer is incomplete. Caught the "turquoise tower with stray turquoise elsewhere on layer" class of invalid plan.
- **2026-04-21 Phase 5b skipped**: initial attempt to stable-partition `layer_tools.extruders` caused wipe tower generation failure (pre-gen tool-change array overflow). Reverted. Phase 5c markers will serve as Phase 6's insertion anchor without needing the reorder. Kept the `tower_filaments_on_layer()` accessor and per-layer tracking populated during build for Phase 5c's use.
- **2026-04-21 Phase 5c**: emit `LOOKAHEAD_LAYER_INFO` + `LOOKAHEAD_BLOCK_BEGIN/END` markers in `process_layer()`. 77 balanced BLOCK pairs on test model match exactly the sum of tower-layer counts from 9 accepted towers. Added `tower_info_for(layer_idx, ext_id)` accessor returning `{base_layer, stack_index, extra_layers, extruder_id}` for the wrapping logic.
- **2026-04-21 Phase 6a**: post-processor module `FilamentLookaheadPostProcessor` with marker parser. Hooked in `_do_export()` before temp→final rename; gated by `filament_lookahead` config flag. Parser produces `TowerBlock`/`LayerInfo` records; 6a transform is no-op (round-trip). Verified marker counts and line positions preserved across process().
- **2026-04-22 Phase 6b + 6c**: Option B emission-time batching. `emit_lookahead_tower_extras` in `GCode.cpp` iterates upper-layer tower stacks inside `process_layer`, saving/restoring per-layer state (`m_layer`, `m_nominal_z`, `m_object_layer_over_raft`, `m_avoid_crossing_perimeters`, `m_extrusion_quality_estimator`) around each stack. For each stack k, emits `retract` + Z-raise + `extrude_perimeters`/`extrude_infill` (and support) + `retract`. Safe-exit travels back to cached pre-batch XY, drops Z to base, unretracts. Upper-layer skip branch in the extruder loop suppresses tower-mode filament emission on layers where its tower is active (stack_index > 0). FLA-DEV-HACK temporarily forces `enable_prime_tower = false` whenever `filament_lookahead` is on, since wipe tower regeneration is deferred to Phase 6d/6f. Debug tools in `debug-tools/` (lookahead_towers.py, layer_regions.py, gcode_common.py) verify tower structure from gcode. Tulip test model slices correctly with 9 accepted towers emitted.
- **2026-04-22 Phase 6b viewer fix**: emit `; Z_HEIGHT: <upper_z>` before each stack's Z-raise inside the batch, and `; Z_HEIGHT: <base_z>` at the safe-exit. Without these tags, GCodeProcessor stamped all batched moves with the base layer's Z, so the viewer buckets them under the base layer and the real upper layers appeared empty. With the tags, moves are attributed to the correct layer_z. Cosmetic viewer rendering quirk on tower layers (partial-layer cross-section look) documented as new Phase 7f task; gcode itself is correct.
