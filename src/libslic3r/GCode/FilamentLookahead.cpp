#include "FilamentLookahead.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"
#include "../PrintConfig.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"

#include <boost/log/trivial.hpp>
#include <queue>
#include <numeric>
#include <functional>

namespace Slic3r {

// Resolve a support_filament config value to a concrete 0-based extruder ID.
// Returns (unsigned int)-1 for Default (0) or Any Type values — those cases are
// context-dependent (resolved per-layer at gcode export time) and during our
// pre-gcode analysis we don't know which extruder will print them. Phase 2 skips
// such support geometry rather than attributing it incorrectly.
static unsigned int resolve_static_support_extruder(int support_filament_value)
{
    if (support_filament_value <= 0)
        return (unsigned int)-1;
    if (is_support_filament_any_type(support_filament_value))
        return (unsigned int)-1;
    return (unsigned int)(support_filament_value - 1);
}

// Check if bbox_a inflated by clearance_scaled overlaps bbox_b.
static bool bboxes_too_close(const BoundingBox &a, const BoundingBox &b, coord_t clearance_scaled)
{
    BoundingBox inflated = a;
    inflated.offset(clearance_scaled);
    return inflated.overlap(b);
}

void FilamentLookaheadPlan::build(const Print &print,
                                   double max_lookahead_height_mm,
                                   double min_clearance_distance_mm)
{
    m_enabled = false;
    m_plan.clear();
    m_already_printed.clear();
    m_raised_per_layer.clear();
    m_any_support_overrides.clear();
    m_tower_filaments_per_layer.clear();

    // Always-visible entry log so we can see whether analysis even started.
    BOOST_LOG_TRIVIAL(info) << "[FLA] build() entry: max_height=" << max_lookahead_height_mm
        << "mm clearance=" << min_clearance_distance_mm << "mm"
        << " num_filaments=" << print.config().filament_diameter.values.size()
        << " num_objects=" << print.objects().size()
        << " print_sequence=" << (print.config().print_sequence == PrintSequence::ByLayer ? "ByLayer" : "ByObject");

    // Only works in ByLayer mode with multiple extruders
    if (print.config().print_sequence != PrintSequence::ByLayer) {
        BOOST_LOG_TRIVIAL(info) << "[FLA] early-return: print_sequence != ByLayer — lookahead disabled for sequential print";
        return;
    }
    if (print.config().filament_diameter.values.size() <= 1) {
        BOOST_LOG_TRIVIAL(info) << "[FLA] early-return: only one filament configured, nothing to lookahead";
        return;
    }

    const coord_t clearance_scaled = scaled<coord_t>(min_clearance_distance_mm);

    if (print.objects().empty()) {
        BOOST_LOG_TRIVIAL(info) << "[FLA] early-return: no print objects";
        return;
    }

    const PrintObject *obj = print.objects().front();
    const auto &layers = obj->layers();
    if (layers.empty()) {
        BOOST_LOG_TRIVIAL(info) << "[FLA] early-return: first object has zero layers";
        return;
    }

    const size_t num_layers = layers.size();
    m_raised_per_layer.resize(num_layers);
    m_tower_filaments_per_layer.assign(num_layers, std::set<unsigned int>{});

    size_t total_instances = 0;
    for (const PrintObject *pobj : print.objects())
        total_instances += pobj->instances().size();
    BOOST_LOG_TRIVIAL(info) << "FilamentLookahead: analyzing " << num_layers << " layers"
        << " max_height=" << max_lookahead_height_mm << "mm"
        << " clearance=" << min_clearance_distance_mm << "mm"
        << " num_objects=" << print.objects().size()
        << " total_instances=" << total_instances;

    // Pre-compute per-extruder bboxes for all layers.
    // Key insight: check isolation PER INSTANCE, not across all instances merged.
    // An extruder is "globally isolated" if it's isolated on EVERY instance.
    // This handles plates with multiple objects where each object's gold nub is
    // isolated from its own body but the merged bboxes would overlap.

    // Helper to collect individual bboxes from extrusion entities recursively
    std::function<void(const ExtrusionEntity*, std::vector<BoundingBox>&)> collect_bboxes;
    collect_bboxes = [&](const ExtrusionEntity *ee, std::vector<BoundingBox> &out) {
        if (!ee) return;
        if (auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
            for (const ExtrusionEntity *child : coll->entities)
                collect_bboxes(child, out);
        } else {
            out.push_back(get_extents(ee->as_polyline()));
        }
    };

    // Cluster bboxes into spatially connected groups.
    // Two bboxes are in the same cluster if they overlap when inflated by clearance.
    auto cluster_bboxes = [](const std::vector<BoundingBox> &bboxes, coord_t clearance) -> std::vector<BoundingBox> {
        if (bboxes.empty()) return {};
        // Union-find
        std::vector<size_t> parent(bboxes.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
            return parent[i] == i ? i : (parent[i] = find(parent[i]));
        };
        for (size_t i = 0; i < bboxes.size(); ++i) {
            for (size_t j = i + 1; j < bboxes.size(); ++j) {
                if (bboxes_too_close(bboxes[i], bboxes[j], clearance)) {
                    parent[find(i)] = find(j);
                }
            }
        }
        // Merge bboxes by cluster
        std::map<size_t, BoundingBox> clusters;
        for (size_t i = 0; i < bboxes.size(); ++i) {
            size_t root = find(i);
            auto it = clusters.find(root);
            if (it != clusters.end())
                it->second.merge(bboxes[i]);
            else
                clusters[root] = bboxes[i];
        }
        std::vector<BoundingBox> result;
        for (auto &[_, bbox] : clusters)
            result.push_back(bbox);
        return result;
    };

    // Per-layer: per-extruder individual entity bboxes in plate coordinates
    // These will be clustered into spatially connected groups for exclusion zones
    std::vector<std::map<unsigned int, std::vector<BoundingBox>>> layer_extruder_entity_bboxes(num_layers);

    // Per-layer: per-extruder bboxes for a SINGLE object (before instance translation)
    // We check isolation on the single-object level since all instances share the same geometry
    std::vector<std::map<unsigned int, BoundingBox>> layer_extruder_bboxes_single(num_layers);

    // Phase 3a: side-channel bucket for "Any (Type)" supports.
    // These can't be attributed to a concrete extruder during Phase 2 analysis, so we
    // defer their resolution to Phase A (zone-claiming) + Phase B (leftover assignment)
    // that run after the tower cascade.
    struct AnyTypeBucket {
        const SupportLayer *support_layer = nullptr; // used as override map key
        size_t              layer_idx    = 0;         // index into first object's layers for lookahead-active lookup
        bool                is_interface = false;
        std::string         type_name;
        std::vector<BoundingBox> entity_bboxes; // plate coords, already instance-translated
    };
    std::vector<AnyTypeBucket> any_buckets;

    // Helper to append entity bboxes for a given extruder into both storage buckets.
    auto ingest_entity_bboxes = [&](size_t li, unsigned int extruder_id,
                                    const std::vector<BoundingBox> &entity_bboxes,
                                    const PrintObject *pobj) {
        if (entity_bboxes.empty()) return;
        BoundingBox region_bbox = entity_bboxes.front();
        for (size_t i = 1; i < entity_bboxes.size(); ++i)
            region_bbox.merge(entity_bboxes[i]);

        auto it = layer_extruder_bboxes_single[li].find(extruder_id);
        if (it != layer_extruder_bboxes_single[li].end())
            it->second.merge(region_bbox);
        else
            layer_extruder_bboxes_single[li][extruder_id] = region_bbox;

        auto &plate_bboxes = layer_extruder_entity_bboxes[li][extruder_id];
        for (const PrintInstance &inst : pobj->instances()) {
            for (const auto &eb : entity_bboxes) {
                BoundingBox pb = eb;
                pb.translate(inst.shift);
                plate_bboxes.push_back(pb);
            }
        }
    };

    for (const PrintObject *pobj : print.objects()) {
        const auto &obj_layers = pobj->layers();
        BOOST_LOG_TRIVIAL(info) << "[FLA] object=" << pobj << " layers=" << obj_layers.size()
            << " support_layers=" << pobj->support_layers().size()
            << " support_filament=" << pobj->config().support_filament.value
            << " support_interface_filament=" << pobj->config().support_interface_filament.value
            << " instances=" << pobj->instances().size();
        for (size_t li = 0; li < std::min(obj_layers.size(), num_layers); ++li) {
            const Layer &layer = *obj_layers[li];
            for (size_t ri = 0; ri < layer.regions().size(); ++ri) {
                const LayerRegion *region = layer.regions()[ri];
                if (!region || !region->has_extrusions())
                    continue;

                unsigned int extruder_id = region->region().config().wall_filament.value - 1;

                std::vector<BoundingBox> entity_bboxes;
                for (const ExtrusionEntity *ee : region->perimeters.entities)
                    collect_bboxes(ee, entity_bboxes);
                for (const ExtrusionEntity *ee : region->fills.entities)
                    collect_bboxes(ee, entity_bboxes);

                ingest_entity_bboxes(li, extruder_id, entity_bboxes, pobj);
            }
        }

        // Support extrusions: walk support_layers and bucket each leaf entity by its
        // role (base vs interface) to look up the correct support_filament config.
        // Match support layer to its corresponding object layer by print_z. Support-only
        // layers that don't align with any object layer are ignored — they contribute no
        // model extrusion and can't host lookahead on their own.
        const unsigned int base_extruder = resolve_static_support_extruder(
            pobj->config().support_filament.value);
        const unsigned int iface_extruder = resolve_static_support_extruder(
            pobj->config().support_interface_filament.value);

        std::function<void(const ExtrusionEntity*, std::vector<BoundingBox>&, std::vector<BoundingBox>&)> walk_support;
        walk_support = [&](const ExtrusionEntity *ee,
                           std::vector<BoundingBox> &base_out,
                           std::vector<BoundingBox> &iface_out) {
            if (!ee) return;
            if (auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
                for (const ExtrusionEntity *child : coll->entities)
                    walk_support(child, base_out, iface_out);
            } else {
                const ExtrusionRole r = ee->role();
                if (r == erSupportMaterialInterface)
                    iface_out.push_back(get_extents(ee->as_polyline()));
                else if (r == erSupportMaterial || r == erSupportTransition)
                    base_out.push_back(get_extents(ee->as_polyline()));
                // Other roles shouldn't appear in support_fills, silently ignored.
            }
        };

        for (const SupportLayer *slayer : pobj->support_layers()) {
            if (!slayer || slayer->support_fills.empty())
                continue;
            // Find matching object layer by print_z
            size_t matched_li = SIZE_MAX;
            for (size_t li = 0; li < num_layers; ++li) {
                if (std::abs(obj_layers[li]->print_z - slayer->print_z) < EPSILON) {
                    matched_li = li;
                    break;
                }
            }
            if (matched_li == SIZE_MAX)
                continue;

            std::vector<BoundingBox> base_boxes, iface_boxes;
            for (const ExtrusionEntity *ee : slayer->support_fills.entities)
                walk_support(ee, base_boxes, iface_boxes);

            if (base_extruder != (unsigned int)-1)
                ingest_entity_bboxes(matched_li, base_extruder, base_boxes, pobj);
            if (iface_extruder != (unsigned int)-1)
                ingest_entity_bboxes(matched_li, iface_extruder, iface_boxes, pobj);

            // Phase 3a: bucket "Any (Type)" supports for later claim/assign.
            const int base_val = pobj->config().support_filament.value;
            if (is_support_filament_any_type(base_val) && !base_boxes.empty()) {
                AnyTypeBucket b;
                b.support_layer = slayer;
                b.layer_idx     = matched_li;
                b.is_interface  = false;
                b.type_name     = support_filament_any_type_name(base_val);
                for (const PrintInstance &inst : pobj->instances())
                    for (const auto &eb : base_boxes) {
                        BoundingBox pb = eb;
                        pb.translate(inst.shift);
                        b.entity_bboxes.push_back(pb);
                    }
                if (!b.type_name.empty())
                    any_buckets.push_back(std::move(b));
            }
            const int iface_val = pobj->config().support_interface_filament.value;
            if (is_support_filament_any_type(iface_val) && !iface_boxes.empty()) {
                AnyTypeBucket b;
                b.support_layer = slayer;
                b.layer_idx     = matched_li;
                b.is_interface  = true;
                b.type_name     = support_filament_any_type_name(iface_val);
                for (const PrintInstance &inst : pobj->instances())
                    for (const auto &eb : iface_boxes) {
                        BoundingBox pb = eb;
                        pb.translate(inst.shift);
                        b.entity_bboxes.push_back(pb);
                    }
                if (!b.type_name.empty())
                    any_buckets.push_back(std::move(b));
            }
        }
    }

    // Summary of per-layer extruder presence (first+last layer each extruder appears)
    {
        std::map<unsigned int, std::pair<size_t, size_t>> ext_first_last;
        for (size_t li = 0; li < num_layers; ++li)
            for (const auto &[eid, _] : layer_extruder_bboxes_single[li]) {
                auto it = ext_first_last.find(eid);
                if (it == ext_first_last.end())
                    ext_first_last[eid] = {li, li};
                else {
                    it->second.first  = std::min(it->second.first, li);
                    it->second.second = std::max(it->second.second, li);
                }
            }
        BOOST_LOG_TRIVIAL(warning) << "[FLA] extruder span summary (0-based extruder id -> [first_layer, last_layer]):";
        const auto &ftypes = print.config().filament_type.values;
        for (const auto &[eid, span] : ext_first_last) {
            const std::string ftype = (eid < ftypes.size()) ? ftypes[eid] : std::string("?");
            BOOST_LOG_TRIVIAL(warning) << "[FLA]   extruder " << eid << " (type=" << ftype
                << ") -> layers [" << span.first << ", " << span.second << "] of " << num_layers
                << (span.second + 1 < num_layers ? " (disappears before end - CANDIDATE)" : " (CONTINUES to print end - not eligible in v1)");
        }
        BOOST_LOG_TRIVIAL(info) << "[FLA] any-type support buckets collected: " << any_buckets.size();
        for (const auto &b : any_buckets)
            BOOST_LOG_TRIVIAL(info) << "[FLA]   any-bucket layer=" << b.layer_idx
                << " role=" << (b.is_interface ? "interface" : "base")
                << " type=" << b.type_name << " entities=" << b.entity_bboxes.size();
    }

    // Phase 3b: filament-completeness precomputation (Rule 4).
    // A (layer, filament) pair is "filament-complete" iff every entity of that
    // filament on that layer falls inside some ISOLATED cluster of that filament
    // on that layer. If stray extrusion exists outside all isolated clusters, a
    // tower for that filament cannot effectively batch this layer — a tool change
    // to the filament is still required for the stray region, erasing the savings.
    // Towers must not cross an incomplete layer for their filament.
    std::vector<std::map<unsigned int, bool>> layer_ext_complete(num_layers);
    size_t complete_count = 0, incomplete_count = 0;
    for (size_t li = 0; li < num_layers; ++li) {
        for (const auto &[ext_id, ents] : layer_extruder_entity_bboxes[li]) {
            auto clusters = cluster_bboxes(ents, clearance_scaled);
            std::vector<BoundingBox> isolated_clusters;
            for (const auto &cb : clusters) {
                bool iso = true;
                for (const auto &[other_id, other_ents] : layer_extruder_entity_bboxes[li]) {
                    if (other_id == ext_id) continue;
                    for (const auto &ob : other_ents)
                        if (bboxes_too_close(cb, ob, clearance_scaled)) { iso = false; break; }
                    if (!iso) break;
                }
                if (iso) isolated_clusters.push_back(cb);
            }
            bool complete = true;
            for (const auto &e : ents) {
                bool inside_iso = false;
                for (const auto &ic : isolated_clusters)
                    if (ic.overlap(e)) { inside_iso = true; break; }
                if (!inside_iso) { complete = false; break; }
            }
            layer_ext_complete[li][ext_id] = complete;
            if (complete) ++complete_count; else ++incomplete_count;
        }
    }
    BOOST_LOG_TRIVIAL(warning) << "[FLA] Phase 3b filament-completeness: "
        << complete_count << " complete, " << incomplete_count
        << " incomplete (layer, filament) pairs";

    // Phase 3c: greedy chained-tower planning. For each extruder, walk upward
    // from the first layer it appears on and greedily build as-tall-as-possible
    // towers (bounded by max_lookahead_height, cascade-truncated by collisions
    // with other extruders' entities). After accepting a tower, skip the outer
    // index past its top layer and start the next tower above it. This replaces
    // the v1 disappearing-extruder-only strategy and produces chained towers
    // through the full height of each isolated filament region.
    std::set<unsigned int> all_extruders;
    for (size_t li = 0; li < num_layers; ++li)
        for (const auto &[eid, _] : layer_extruder_bboxes_single[li])
            all_extruders.insert(eid);

    // ─────────────────────────────────────────────────────────────────────
    // Phase 5d (travel exclusion): plate geometry cache + reachability rules
    //
    // The cascade loop below decides whether each candidate layer extends
    // an in-progress tower. Five rules gate that decision; three pre-existed
    // (Rules 1, 3, 4) and two are added here for travel-routing correctness
    // (Rules 11, 12). All five must hold at every k for the tower to extend
    // through layer li+k. Any failure caps the tower at the previous layer.
    //
    //   Rule 1 — Candidate zone isolation. Other filaments' entities on the
    //     candidate layer must stay clearance away from the cluster's
    //     inflated bbox. Otherwise the tower would force a collision with
    //     another extruder's print on the same layer.
    //
    //   Rule 3 — Containment for tower continuation. The tower's own
    //     filament must have entities INSIDE the cluster's footprint on
    //     every layer it covers. A layer with no overlapping content has
    //     nothing to batch into the tower; extending past it would emit
    //     no gcode at that level and break the layer-height continuity
    //     of the tower's structural fill.
    //
    //   Rule 4 — Filament completeness. Every extrusion of ext_id on the
    //     candidate layer must fall inside some isolated cluster of ext_id.
    //     Stray ext_id extrusion outside the cluster forces a tool change
    //     to ext_id ANYWAY, defeating the entire savings the tower was
    //     supposed to deliver.
    //
    //   Rule 11 — Plate-edge clearance (NEW). The candidate exclusion zone
    //     on every covered layer must keep at least PLATE_EDGE_MARGIN_MM
    //     from the printable area's outer boundary. Travel routing in
    //     GCode emission has to move AROUND a tower's footprint without
    //     leaving the plate; if a zone is wedged against the edge, that
    //     side has no clearance and a single edge-locked zone composed
    //     with another zone forms a wall that cuts off plate area.
    //     Conservative: 2 mm of breathing room on all four sides.
    //
    //   Rule 12 — No enclosed object pockets (NEW). The union of all
    //     exclusion zones active on a covered layer (this candidate plus
    //     every previously-accepted tower) must not split the plate's
    //     travel-allowed region into a topology where any object content
    //     not already inside a zone is unreachable from the plate's
    //     outside. Three or more zones can ring a piece of unzoned model
    //     such that any travel toward it must cross a zone — which we
    //     forbid by Rule 8 (Rule 8 also covers what happens once a tower
    //     is up: every same-layer travel must avoid its XY footprint).
    //     Reject any candidate that creates such a ring.
    //
    // Both new rules use bbox-level approximations for v1: zones are bbox
    // polygons (axis-aligned rectangles), and "object content" inside a
    // pocket is detected by per-extruder entity-bbox overlap with the
    // pocket's bbox. Refinement to actual slice polygons is deferred —
    // false-positive rejections (over-conservative caps) are acceptable
    // since they trade one missed savings opportunity for guaranteed
    // travel correctness; false-negatives (missed rejections) would let a
    // bad topology slip through and corrupt the print.
    //
    constexpr double PLATE_EDGE_MARGIN_MM = 2.0;
    const coord_t plate_margin_scaled = scaled<coord_t>(PLATE_EDGE_MARGIN_MM);

    Polygon bed_polygon;
    bed_polygon.points = Slic3r::get_bed_shape(print.config(), false);
    const BoundingBox bed_bbox = bed_polygon.bounding_box();
    const Polygons bed_polys = { bed_polygon };

    BOOST_LOG_TRIVIAL(info) << "[FLA] Phase 5d: plate bbox = ["
        << unscale<double>(bed_bbox.min.x()) << "," << unscale<double>(bed_bbox.min.y())
        << "]-[" << unscale<double>(bed_bbox.max.x()) << "," << unscale<double>(bed_bbox.max.y())
        << "]  edge_margin=" << PLATE_EDGE_MARGIN_MM << "mm";

    // Rule 11 — candidate inflated zone must keep PLATE_EDGE_MARGIN_MM clearance
    // from every plate edge. v1 uses the bed_bbox shrunk by margin (treats the
    // bed as rectangular); precise polygon containment is a v2 enhancement and
    // matters only for non-rectangular printable areas (e.g. delta printers).
    auto rule_11_passes = [&](const BoundingBox &zone_inflated) -> bool {
        return zone_inflated.min.x() >= bed_bbox.min.x() + plate_margin_scaled
            && zone_inflated.min.y() >= bed_bbox.min.y() + plate_margin_scaled
            && zone_inflated.max.x() <= bed_bbox.max.x() - plate_margin_scaled
            && zone_inflated.max.y() <= bed_bbox.max.y() - plate_margin_scaled;
    };

    // Rule 12 — zones must not enclose unzoned object content. Compute the
    // travel-allowed region (bed minus union-of-zones), find connected
    // components, classify the "outside" component (touches the most plate
    // edges), and check no other component contains an object bbox that isn't
    // already covered by some exclusion zone.
    auto rule_12_passes = [&](const BoundingBox &candidate_inflated, size_t check_layer_idx) -> bool {
        // Build the union of exclusion zones active on the layer we're
        // checking. "Active" = previously-accepted towers registered in
        // m_raised_per_layer plus this candidate. Multi-cluster interactions
        // within the SAME plan entry (i.e. earlier clusters in the current
        // base-layer's accepted_zones list) are not yet included — v1
        // approximation, refine if multi-cluster bases become common.
        Polygons all_zone_polys;
        if (check_layer_idx < m_raised_per_layer.size()) {
            for (const auto &eb : m_raised_per_layer[check_layer_idx].exclusion_bboxes)
                all_zone_polys.push_back(eb.polygon());
        }
        all_zone_polys.push_back(candidate_inflated.polygon());
        Polygons zones_union = union_(all_zone_polys);

        // travel_allowed = bed - zones_union. Each ExPolygon is a connected
        // component of where the head can reach without crossing a zone.
        ExPolygons travel = diff_ex(bed_polys, zones_union);
        if (travel.size() <= 1)
            return true;  // single component → all reachable, no pockets

        // Heuristic: pick the "outside" component as the one whose contour
        // touches the most plate edges. A pocket fully inside the plate
        // touches zero edges; the main travel region touches all four.
        size_t  outside_idx = 0;
        int     max_edges_touched = -1;
        const   coord_t edge_tol = scaled<coord_t>(0.1);  // 0.1 mm slack for fp noise
        for (size_t i = 0; i < travel.size(); ++i) {
            const BoundingBox tb = travel[i].contour.bounding_box();
            int edges = 0;
            if (tb.min.x() <= bed_bbox.min.x() + edge_tol) ++edges;
            if (tb.max.x() >= bed_bbox.max.x() - edge_tol) ++edges;
            if (tb.min.y() <= bed_bbox.min.y() + edge_tol) ++edges;
            if (tb.max.y() >= bed_bbox.max.y() - edge_tol) ++edges;
            if (edges > max_edges_touched) {
                max_edges_touched = edges;
                outside_idx = i;
            }
        }

        // For each pocket (non-outside component), test whether it overlaps
        // any object content bbox NOT already inside an exclusion zone on
        // this layer. If so, this candidate strands that content — reject.
        if (check_layer_idx >= layer_extruder_entity_bboxes.size())
            return true;
        for (size_t i = 0; i < travel.size(); ++i) {
            if (i == outside_idx) continue;
            const BoundingBox pocket_bbox = travel[i].contour.bounding_box();
            for (const auto &[other_ext, bboxes] : layer_extruder_entity_bboxes[check_layer_idx]) {
                for (const auto &ob : bboxes) {
                    bool in_any_zone = candidate_inflated.contains(ob);
                    if (!in_any_zone && check_layer_idx < m_raised_per_layer.size()) {
                        for (const auto &eb : m_raised_per_layer[check_layer_idx].exclusion_bboxes) {
                            if (eb.contains(ob)) { in_any_zone = true; break; }
                        }
                    }
                    if (in_any_zone) continue;
                    if (pocket_bbox.overlap(ob))
                        return false;  // unzoned content stranded in a pocket
                }
            }
        }
        return true;
    };
    // ─────────────────────────────────────────────────────────────────────

    for (unsigned int ext_id : all_extruders) {
        size_t li = 0;
        while (li < num_layers) {
            const auto &extruder_bboxes = layer_extruder_bboxes_single[li];
            // Extruder must be present on this base layer
            if (!extruder_bboxes.count(ext_id)) { ++li; continue; }
            // Need at least 2 extruders on this layer for a tool change to exist
            if (extruder_bboxes.size() < 2) { ++li; continue; }

            double layer_height = (li + 1 < num_layers)
                ? (layers[li + 1]->print_z - layers[li]->print_z) : layers[li]->height;
            size_t max_look = (size_t)(max_lookahead_height_mm / layer_height);
            if (max_look == 0) { ++li; continue; }

            // Find the last contiguous layer where ext_id is present, bounded by window.
            // Unlike v1, we TRUNCATE at window edge rather than rejecting the base.
            size_t last_present = li;
            for (size_t k = 1; k <= max_look && li + k < num_layers; ++k) {
                if (layer_extruder_bboxes_single[li + k].count(ext_id))
                    last_present = li + k;
                else
                    break;
            }
            if (last_present == li) {
                BOOST_LOG_TRIVIAL(info) << "[FLA] layer=" << li << " ext=" << ext_id
                    << " REJECT: extruder only on this layer (no runway)";
                ++li;
                continue;
            }

            // Phase 3b (Rule 4): base layer must be filament-complete for ext_id.
            // Stray extrusion outside the zone on the base layer means a tool
            // change to this filament is still required for the stray — no
            // saving is possible, so reject the base.
            {
                auto &lmap = layer_ext_complete[li];
                auto it = lmap.find(ext_id);
                if (it == lmap.end() || !it->second) {
                    BOOST_LOG_TRIVIAL(info) << "[FLA] layer=" << li << " ext=" << ext_id
                        << " REJECT: base layer not filament-complete for this extruder (stray extrusion outside isolated clusters)";
                    ++li;
                    continue;
                }
            }

            const auto &ext_bbox = extruder_bboxes.at(ext_id);
            auto ent_it = layer_extruder_entity_bboxes[li].find(ext_id);
            if (ent_it == layer_extruder_entity_bboxes[li].end()) {
                BOOST_LOG_TRIVIAL(info) << "[FLA] layer=" << li << " ext=" << ext_id
                    << " REJECT: no plate-coord entities (shouldn't happen if single-bbox existed)";
                ++li;
                continue;
            }
            auto clusters = cluster_bboxes(ent_it->second, clearance_scaled);
            BOOST_LOG_TRIVIAL(info) << "[FLA] layer=" << li << " ext=" << ext_id
                << " last_present=" << last_present << " max_look=" << max_look
                << " clusters=" << clusters.size() << " - evaluating...";

            // For each cluster, apply per-layer cascade truncation: starting from extra=0,
            // walk forward and check if the cluster's inflated zone is clear of other
            // extruders' entities on each intermediate layer. Stop at the first violation.
            std::vector<BoundingBox> accepted_zones;
            size_t truncated_extra = last_present - li; // candidate max extra layers

            for (const auto &cb : clusters) {
                // ───── Base-layer (k=0) gate ─────
                // Five rules govern whether a cluster can become a tower base.
                // The first three — Rule 1 (other-filament clearance on the
                // starting layer), Rule 11 (plate-edge clearance), Rule 12
                // (no enclosed object pockets) — gate acceptance even before
                // we look at upper layers. Rule 3 and Rule 4 only kick in
                // for cascade extension (k>=1) since they govern whether the
                // tower can EXTEND past the base.

                // Rule 1 (k=0): cluster's bbox must be clear of other
                // extruders' entities on the starting layer. Inflating cb by
                // `clearance_scaled` and intersecting with other entities is
                // the same check applied at every cascade step — applied
                // here for the base layer alone.
                bool cluster_isolated = true;
                for (const auto &[other_id, other_ents] : layer_extruder_entity_bboxes[li]) {
                    if (other_id == ext_id) continue;
                    for (const auto &ob : other_ents) {
                        if (bboxes_too_close(cb, ob, clearance_scaled)) {
                            cluster_isolated = false;
                            break;
                        }
                    }
                    if (!cluster_isolated) break;
                }
                if (!cluster_isolated) {
                    BOOST_LOG_TRIVIAL(info) << "[FLA]   cluster at [" << cb.min.x() << "," << cb.min.y()
                        << "]-[" << cb.max.x() << "," << cb.max.y()
                        << "] REJECT: fails Rule 1 (isolation) on starting layer " << li;
                    continue;
                }

                // Rule 11 (k=0): the cluster's inflated zone must keep
                // PLATE_EDGE_MARGIN_MM clearance from the printable area's
                // outer boundary. Travel routing on every layer the tower
                // covers needs room to move around it without leaving the
                // plate; an edge-locked zone has no such room.
                BoundingBox running_envelope = cb;
                BoundingBox running_inflated = running_envelope.inflated(clearance_scaled);
                if (!rule_11_passes(running_inflated)) {
                    BOOST_LOG_TRIVIAL(info) << "[FLA]   cluster at [" << cb.min.x() << "," << cb.min.y()
                        << "]-[" << cb.max.x() << "," << cb.max.y()
                        << "] REJECT: fails Rule 11 (plate-edge clearance) on starting layer " << li
                        << " — inflated zone reaches within " << PLATE_EDGE_MARGIN_MM << "mm of plate edge";
                    continue;
                }

                // Rule 12 (k=0): combined exclusion zones (this candidate +
                // any zones from previously-accepted towers active on the
                // same layer) must not strand unzoned object content in a
                // travel-disconnected pocket. See Phase 5d header for full
                // rationale.
                if (!rule_12_passes(running_inflated, li)) {
                    BOOST_LOG_TRIVIAL(info) << "[FLA]   cluster at [" << cb.min.x() << "," << cb.min.y()
                        << "]-[" << cb.max.x() << "," << cb.max.y()
                        << "] REJECT: fails Rule 12 (object pocket) on starting layer " << li;
                    continue;
                }

                // ───── Cascade truncation (k=1..max) ─────
                // For each candidate extension layer, all five rules must
                // pass. First failure caps the tower at k-1.
                //
                //   Rule 3  — self-content: tower's filament must have
                //             entities inside the running envelope on this
                //             layer. No content → nothing to batch, break
                //             continuity.
                //   Rule 1  — other-filament clearance: foreign extruders'
                //             entities must stay clearance away from the
                //             inflated envelope.
                //   Rule 4  — filament completeness: every extrusion of
                //             ext_id on this layer must lie inside an
                //             isolated cluster (Phase 3b output).
                //   Rule 11 — plate-edge clearance for the GROWING envelope.
                //             The envelope can expand as we extend (each
                //             layer's self-content merges in), and an
                //             expanded envelope may now violate the margin
                //             even if the base didn't.
                //   Rule 12 — no enclosed object pockets, recomputed per
                //             layer because m_raised_per_layer may carry
                //             a different set of zones on each candidate
                //             layer (other towers can start/end mid-cascade).
                size_t cluster_extra = truncated_extra;
                for (size_t k = 1; k <= truncated_extra; ++k) {
                    // Rule 4 — filament-completeness on layer li+k (Phase 3b).
                    {
                        auto &lmap = layer_ext_complete[li + k];
                        auto it = lmap.find(ext_id);
                        if (it == lmap.end() || !it->second) {
                            cluster_extra = k - 1;
                            BOOST_LOG_TRIVIAL(info) << "[FLA]   cascade truncate at k=" << k
                                << " (layer " << (li + k) << "): Rule 4 — not filament-complete for ext=" << ext_id;
                            break;
                        }
                    }

                    // Rule 3 — tower's own filament must have content
                    // overlapping the cluster's footprint on this layer.
                    bool self_has_content = false;
                    auto self_it = layer_extruder_entity_bboxes[li + k].find(ext_id);
                    if (self_it != layer_extruder_entity_bboxes[li + k].end()) {
                        for (const auto &sb : self_it->second) {
                            if (cb.overlap(sb)) {
                                self_has_content = true;
                                break;
                            }
                        }
                    }
                    if (!self_has_content) {
                        cluster_extra = k - 1;
                        BOOST_LOG_TRIVIAL(info) << "[FLA]   cascade truncate at k=" << k
                            << " (layer " << (li + k) << "): Rule 3 — no self-content overlapping cluster";
                        break;
                    }

                    // Rule 1 — other filaments' entities clear of envelope.
                    bool clear_at_k = true;
                    for (const auto &[other_id, other_ents] : layer_extruder_entity_bboxes[li + k]) {
                        if (other_id == ext_id) continue;
                        for (const auto &ob : other_ents) {
                            if (bboxes_too_close(cb, ob, clearance_scaled)) {
                                clear_at_k = false;
                                break;
                            }
                        }
                        if (!clear_at_k) break;
                    }
                    if (!clear_at_k) {
                        cluster_extra = k - 1;
                        BOOST_LOG_TRIVIAL(info) << "[FLA]   cascade truncate at k=" << k
                            << " (layer " << (li + k) << "): Rule 1 — other extruder intrudes envelope";
                        break;
                    }

                    // Update the running envelope to include layer li+k's
                    // self-content overlapping cb. The envelope can only
                    // grow; once it grows we re-test Rules 11 and 12 against
                    // the new size.
                    if (self_it != layer_extruder_entity_bboxes[li + k].end()) {
                        for (const auto &sb : self_it->second)
                            if (cb.overlap(sb))
                                running_envelope.merge(sb);
                    }
                    running_inflated = running_envelope.inflated(clearance_scaled);

                    // Rule 11 — re-check plate-edge clearance with the grown envelope.
                    if (!rule_11_passes(running_inflated)) {
                        cluster_extra = k - 1;
                        BOOST_LOG_TRIVIAL(info) << "[FLA]   cascade truncate at k=" << k
                            << " (layer " << (li + k) << "): Rule 11 — envelope grew within "
                            << PLATE_EDGE_MARGIN_MM << "mm of plate edge";
                        break;
                    }

                    // Rule 12 — re-check pocket condition on layer li+k.
                    // m_raised_per_layer at THIS layer index reflects the
                    // zones from previously-accepted towers active here, so
                    // a tower that ENDS on layer li+k-1 won't show up at li+k
                    // (correctly), and a tower whose base is at li+k starts
                    // contributing here.
                    if (!rule_12_passes(running_inflated, li + k)) {
                        cluster_extra = k - 1;
                        BOOST_LOG_TRIVIAL(info) << "[FLA]   cascade truncate at k=" << k
                            << " (layer " << (li + k) << "): Rule 12 — combined zones strand unzoned object content";
                        break;
                    }
                }

                // Use the minimum cascade truncation across all clusters in this plan entry.
                // All clusters must be valid for every layer they cover.
                if (cluster_extra == 0) {
                    BOOST_LOG_TRIVIAL(info) << "[FLA]   cluster at [" << cb.min.x() << "," << cb.min.y()
                        << "]-[" << cb.max.x() << "," << cb.max.y()
                        << "] REJECT: truncated to zero extra layers - another extruder intrudes immediately above";
                    continue;
                }

                truncated_extra = std::min(truncated_extra, cluster_extra);

                // Grow the zone to the max XY envelope of this filament's
                // extrusion across every layer the tower covers (base..base+extra).
                // Only entities connected to the base cluster (overlapping cb) are
                // counted, so we don't accidentally annex an unrelated region.
                // This ensures the zone covers the widest point of the tower, not
                // just the base layer's footprint. Known v1 limitation: if the
                // envelope grows beyond `cb` and another filament sits within
                // `clearance` of the envelope (but not of `cb`), the cascade above
                // won't have caught it — per-layer contour avoidance (Phase 5d /
                // per-layer contour polish) would be the proper fix.
                BoundingBox envelope = cb;
                for (size_t k = 0; k <= cluster_extra; ++k) {
                    auto self_it = layer_extruder_entity_bboxes[li + k].find(ext_id);
                    if (self_it == layer_extruder_entity_bboxes[li + k].end()) continue;
                    for (const auto &sb : self_it->second)
                        if (cb.overlap(sb))
                            envelope.merge(sb);
                }

                BoundingBox inflated = envelope;
                inflated.offset(clearance_scaled);
                accepted_zones.push_back(inflated);
                BOOST_LOG_TRIVIAL(info) << "[FLA]   cluster base=[" << cb.min.x() << "," << cb.min.y()
                    << "]-[" << cb.max.x() << "," << cb.max.y() << "]"
                    << " envelope=[" << envelope.min.x() << "," << envelope.min.y()
                    << "]-[" << envelope.max.x() << "," << envelope.max.y() << "]"
                    << " ACCEPT: extra_layers=" << cluster_extra;
            }

            if (accepted_zones.empty() || truncated_extra == 0) {
                BOOST_LOG_TRIVIAL(info) << "[FLA] layer=" << li << " ext=" << ext_id
                    << " REJECT: no clusters survived after cascade/isolation";
                ++li;
                continue;
            }

            LookaheadEntry entry;
            entry.extra_layers = truncated_extra;
            entry.raised_z = layers[li + truncated_extra]->print_z;
            entry.raised_bbox = ext_bbox;
            entry.exclusion_bboxes = std::move(accepted_zones);

            m_plan[{li, ext_id}] = entry;

            // Register the exclusion zone on every layer the tower physically exists on,
            // including the base layer. Per Rule 8, once the tower is printed on the base
            // layer, any subsequent travel on that same layer must avoid its XY footprint
            // because the tower will already be sticking up above current-layer Z.
            // Phase 5b: also mark the tower's filament as "tower-mode" on each covered
            // layer so emission can defer it to the tower pass (normal-first ordering).
            for (size_t k = 0; k <= truncated_extra; ++k) {
                size_t future_li = li + k;
                if (future_li < m_raised_per_layer.size()) {
                    m_raised_per_layer[future_li].max_z = std::max(
                        m_raised_per_layer[future_li].max_z, entry.raised_z);
                    for (const auto &eb : entry.exclusion_bboxes)
                        m_raised_per_layer[future_li].exclusion_bboxes.push_back(eb);
                    m_tower_filaments_per_layer[future_li].insert(ext_id);
                }
            }

            m_enabled = true;
            const auto &ftypes = print.config().filament_type.values;
            const std::string ftype = (ext_id < ftypes.size()) ? ftypes[ext_id] : std::string("?");
            BOOST_LOG_TRIVIAL(warning) << "[FLA] TOWER ACCEPTED: base_layer=" << li
                << " extruder=" << ext_id << " (type=" << ftype << ")"
                << " extra_layers=" << truncated_extra
                << " covers layers [" << li << ".." << (li + truncated_extra) << "]"
                << " raised_z=" << entry.raised_z << "mm"
                << " clusters=" << entry.exclusion_bboxes.size();

            // Chain: advance past this tower's top layer so the next iteration
            // can start a new tower immediately above it if eligible.
            li = li + truncated_extra + 1;
        }
    }

    if (m_enabled)
        BOOST_LOG_TRIVIAL(info) << "FilamentLookahead: " << m_plan.size() << " lookahead opportunities found";

    // ─────────────────────────────────────────────────────────────────────────
    // Phase 3a: "Any (Type)" support filament resolution (Rules 2, 5, 6).
    //
    // Phase A (zone-claim): for each tower's exclusion zone, claim compatible
    //   Any-Type supports that intersect. Records override → tower's filament.
    // Phase B (leftover):   for remaining Any-Type supports, assign a filament
    //   using the default resolver but restricted to non-lookahead-active
    //   filaments on the layer (Rule 5).
    //
    // This pass only produces `m_any_support_overrides`. Phase 5a will consult
    // it during gcode emission to short-circuit the existing per-layer resolver.
    // Zones are not re-expanded and isolation is not re-validated in v1 — a
    // known simplification versus Rule 2's full recursive extension.
    // ─────────────────────────────────────────────────────────────────────────
    if (!any_buckets.empty()) {
        const auto &filament_types = print.config().filament_type.values;

        auto bbox_intersects_any = [](const std::vector<BoundingBox> &a,
                                       const std::vector<BoundingBox> &b) {
            for (const auto &x : a)
                for (const auto &y : b)
                    if (x.overlap(y))
                        return true;
            return false;
        };

        // Phase A: tower zones claim compatible Any-Type supports.
        for (const auto &[key, entry] : m_plan) {
            const auto [base_li, ext_id] = key;
            if (ext_id >= filament_types.size())
                continue;
            const std::string &tower_type = filament_types[ext_id];
            const size_t tower_top = base_li + entry.extra_layers;

            for (const auto &b : any_buckets) {
                if (b.type_name != tower_type) continue;
                if (b.layer_idx < base_li || b.layer_idx > tower_top) continue;
                auto k = std::make_pair(b.support_layer, b.is_interface);
                if (m_any_support_overrides.count(k)) continue;
                if (bbox_intersects_any(b.entity_bboxes, entry.exclusion_bboxes)) {
                    m_any_support_overrides[k] = ext_id;
                    BOOST_LOG_TRIVIAL(warning) << "[FLA] Phase A CLAIM: tower (base_layer=" << base_li
                        << ", ext=" << ext_id << ", type=" << tower_type
                        << ") claims Any support layer=" << b.layer_idx
                        << " role=" << (b.is_interface ? "interface" : "base");
                }
            }
        }

        // Phase B: assign any remaining Any-Type supports using the default resolver
        // restricted to non-lookahead-active filaments on that layer (Rule 5).
        auto lookahead_active_on_layer = [&](size_t layer_idx, std::set<unsigned int> &out) {
            for (const auto &[key, entry] : m_plan) {
                const auto [base_li, ext_id] = key;
                if (layer_idx > base_li && layer_idx <= base_li + entry.extra_layers)
                    out.insert(ext_id);
            }
        };

        for (const auto &b : any_buckets) {
            auto k = std::make_pair(b.support_layer, b.is_interface);
            if (m_any_support_overrides.count(k)) continue;

            std::set<unsigned int> active_set;
            lookahead_active_on_layer(b.layer_idx, active_set);

            std::vector<unsigned int> candidates;
            if (b.layer_idx < layer_extruder_bboxes_single.size()) {
                for (const auto &[eid, _] : layer_extruder_bboxes_single[b.layer_idx])
                    if (!active_set.count(eid))
                        candidates.push_back(eid);
            }

            const int any_val = support_filament_any_type_value_for_name(b.type_name);
            if (any_val < 0) continue;
            unsigned int resolved = resolve_any_type_support_filament(
                any_val, print.config(), candidates);
            if (resolved == (unsigned int)-1) {
                BOOST_LOG_TRIVIAL(info) << "[FLA] Phase B SKIP: layer=" << b.layer_idx
                    << " role=" << (b.is_interface ? "interface" : "base")
                    << " - no compatible non-lookahead-active filament found for type=" << b.type_name;
                continue;
            }
            if (active_set.count(resolved)) {
                BOOST_LOG_TRIVIAL(info) << "[FLA] Phase B SAFETY: resolver returned a lookahead-active extruder - ignored";
                continue;
            }
            m_any_support_overrides[k] = resolved;
            BOOST_LOG_TRIVIAL(info) << "[FLA] Phase B ASSIGN: layer=" << b.layer_idx
                << " role=" << (b.is_interface ? "interface" : "base")
                << " type=" << b.type_name << " -> extruder=" << resolved
                << " (candidates=" << candidates.size() << " lookahead_active=" << active_set.size() << ")";
        }

        BOOST_LOG_TRIVIAL(warning) << "[FLA] Phase 3a complete: "
            << m_any_support_overrides.size() << " Any-Type overrides (of "
            << any_buckets.size() << " candidate buckets)";
    } else {
        BOOST_LOG_TRIVIAL(warning) << "[FLA] Phase 3a skipped: no Any-Type supports in this slice";
    }

    BOOST_LOG_TRIVIAL(warning) << "[FLA] build() final: enabled=" << m_enabled
        << " towers=" << m_plan.size()
        << " any_overrides=" << m_any_support_overrides.size();
}

size_t FilamentLookaheadPlan::extra_layers(size_t layer_idx, unsigned int extruder_id) const
{
    auto it = m_plan.find({layer_idx, extruder_id});
    return (it != m_plan.end()) ? it->second.extra_layers : 0;
}

bool FilamentLookaheadPlan::already_printed(size_t layer_idx, unsigned int extruder_id) const
{
    return m_already_printed.count({layer_idx, extruder_id}) > 0;
}

void FilamentLookaheadPlan::mark_printed(size_t layer_idx, unsigned int extruder_id)
{
    m_already_printed.insert({layer_idx, extruder_id});
}

coordf_t FilamentLookaheadPlan::max_raised_z(size_t layer_idx) const
{
    if (layer_idx < m_raised_per_layer.size())
        return m_raised_per_layer[layer_idx].max_z;
    return 0.;
}

std::vector<BoundingBox> FilamentLookaheadPlan::exclusion_zones(size_t layer_idx) const
{
    if (layer_idx < m_raised_per_layer.size())
        return m_raised_per_layer[layer_idx].exclusion_bboxes;
    return {};
}

const std::set<unsigned int>& FilamentLookaheadPlan::tower_filaments_on_layer(size_t layer_idx) const
{
    if (layer_idx < m_tower_filaments_per_layer.size())
        return m_tower_filaments_per_layer[layer_idx];
    return m_empty_filaments;
}

std::optional<FilamentLookaheadPlan::TowerInfo>
FilamentLookaheadPlan::tower_info_for(size_t layer_idx, unsigned int ext_id) const
{
    for (const auto &[key, entry] : m_plan) {
        const auto &[base_li, eid] = key;
        if (eid != ext_id) continue;
        if (layer_idx < base_li || layer_idx > base_li + entry.extra_layers) continue;
        TowerInfo t;
        t.base_layer   = base_li;
        t.stack_index  = layer_idx - base_li;
        t.extra_layers = entry.extra_layers;
        t.extruder_id  = ext_id;
        return t;
    }
    return std::nullopt;
}

bool FilamentLookaheadPlan::is_upper_tower_stack(size_t layer_idx, unsigned int ext_id) const
{
    auto t = tower_info_for(layer_idx, ext_id);
    return t && t->stack_index >= 1;
}

std::optional<unsigned int> FilamentLookaheadPlan::override_for_support(
    const SupportLayer *support_layer, bool is_interface) const
{
    auto it = m_any_support_overrides.find(std::make_pair(support_layer, is_interface));
    if (it == m_any_support_overrides.end())
        return std::nullopt;
    return it->second;
}

} // namespace Slic3r
