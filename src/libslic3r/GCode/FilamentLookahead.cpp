#include "FilamentLookahead.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"
#include "../PrintConfig.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"

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

    // Only works in ByLayer mode with multiple extruders
    if (print.config().print_sequence != PrintSequence::ByLayer)
        return;
    if (print.config().filament_diameter.values.size() <= 1)
        return;

    const coord_t clearance_scaled = scaled<coord_t>(min_clearance_distance_mm);

    if (print.objects().empty())
        return;

    // Analyze ALL print objects, not just the first
    // For each object, compute per-extruder bboxes per layer using lslices
    // and the actual MMU segmentation data (not just region config)
    const PrintObject *obj = print.objects().front();
    const auto &layers = obj->layers();
    if (layers.empty())
        return;

    const size_t num_layers = layers.size();
    m_raised_per_layer.resize(num_layers);

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
        }
    }

    // Phase 2 restricts the analysis to the "disappearing extruder" case only:
    // a candidate extruder E exists on layer L (and possibly earlier layers),
    // stops existing within the lookahead window above L, and other extruders
    // continue above — meaning batching all of E's remaining layers eliminates
    // every subsequent tool change for E.
    //
    // The bbox-isolated-but-continuing strategy (extruder present on every layer
    // but spatially far from others) is intentionally disabled for v1 — it's
    // retained conceptually for future work once the disappearing case is proven.
    for (size_t li = 0; li < num_layers; ++li) {
        const auto &extruder_bboxes = layer_extruder_bboxes_single[li];

        // Need at least 2 extruders on this layer for a tool change to exist
        if (extruder_bboxes.size() < 2)
            continue;

        for (const auto &[ext_id, ext_bbox] : extruder_bboxes) {
            double layer_height = (li + 1 < num_layers)
                ? (layers[li + 1]->print_z - layers[li]->print_z) : layers[li]->height;
            size_t max_look = (size_t)(max_lookahead_height_mm / layer_height);
            if (max_look == 0)
                continue;

            // Find this extruder's last contiguous layer (must be present every layer
            // from li onward, disappearing at some point within max_look).
            size_t last_present = li;
            for (size_t k = 1; k <= max_look && li + k < num_layers; ++k) {
                if (layer_extruder_bboxes_single[li + k].count(ext_id))
                    last_present = li + k;
                else
                    break;
            }
            if (last_present == li)
                continue; // extruder only exists on this layer — no runway to batch
            if (last_present >= li + max_look)
                continue; // still present at end of window — not a disappearing case
            if (last_present + 1 >= num_layers)
                continue; // print ends with this extruder — no subsequent tool changes to save
            if (layer_extruder_bboxes_single[last_present + 1].count(ext_id))
                continue; // safety check: extruder must be absent on the next layer

            // Verify other extruders continue above last_present — otherwise there are
            // no future tool changes to eliminate (the whole print is ending).
            bool others_continue = false;
            for (size_t k = last_present + 1; k < num_layers && k <= last_present + 3; ++k) {
                if (!layer_extruder_bboxes_single[k].empty()) {
                    others_continue = true;
                    break;
                }
            }
            if (!others_continue)
                continue;

            // Cluster this extruder's plate-coord entity bboxes, filter to only clusters
            // isolated from other extruders on the STARTING layer.
            auto ent_it = layer_extruder_entity_bboxes[li].find(ext_id);
            if (ent_it == layer_extruder_entity_bboxes[li].end())
                continue;
            auto clusters = cluster_bboxes(ent_it->second, clearance_scaled);

            // For each cluster, apply per-layer cascade truncation: starting from extra=0,
            // walk forward and check if the cluster's inflated zone is clear of other
            // extruders' entities on each intermediate layer. Stop at the first violation.
            std::vector<BoundingBox> accepted_zones;
            size_t truncated_extra = last_present - li; // candidate max extra layers

            for (const auto &cb : clusters) {
                // Starting-layer isolation check (required even at k=0)
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
                if (!cluster_isolated)
                    continue; // this cluster fails isolation — skip it

                // Cascade truncate: how many layers ahead remains clear for this cluster?
                size_t cluster_extra = truncated_extra;
                for (size_t k = 1; k <= truncated_extra; ++k) {
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
                        break;
                    }
                }

                // Use the minimum cascade truncation across all clusters in this plan entry.
                // All clusters must be valid for every layer they cover.
                if (cluster_extra == 0)
                    continue; // this cluster offers no lookahead runway

                truncated_extra = std::min(truncated_extra, cluster_extra);
                BoundingBox inflated = cb;
                inflated.offset(clearance_scaled);
                accepted_zones.push_back(inflated);
            }

            if (accepted_zones.empty() || truncated_extra == 0)
                continue;

            LookaheadEntry entry;
            entry.extra_layers = truncated_extra;
            entry.raised_z = layers[li + truncated_extra]->print_z;
            entry.raised_bbox = ext_bbox;
            entry.exclusion_bboxes = std::move(accepted_zones);

            m_plan[{li, ext_id}] = entry;

            for (size_t k = 1; k <= truncated_extra; ++k) {
                size_t future_li = li + k;
                if (future_li < m_raised_per_layer.size()) {
                    m_raised_per_layer[future_li].max_z = std::max(
                        m_raised_per_layer[future_li].max_z, entry.raised_z);
                    for (const auto &eb : entry.exclusion_bboxes)
                        m_raised_per_layer[future_li].exclusion_bboxes.push_back(eb);
                }
            }

            m_enabled = true;
            BOOST_LOG_TRIVIAL(info) << "FilamentLookahead: layer " << li
                << " extruder " << ext_id << " disappearing after " << truncated_extra
                << " layers (raised_z=" << entry.raised_z << ")";
        }
    }

    if (m_enabled)
        BOOST_LOG_TRIVIAL(info) << "FilamentLookahead: " << m_plan.size() << " lookahead opportunities found";
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

} // namespace Slic3r
