#include "FilamentLookahead.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"
#include "../PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <queue>
#include <numeric>

namespace Slic3r {

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
    fprintf(stderr, "FilamentLookahead: analyzing %zu layers, max_height=%.1fmm, clearance=%.1fmm, objects=%zu, instances=%zu\n",
        num_layers, max_lookahead_height_mm, min_clearance_distance_mm, print.objects().size(), total_instances);

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

    for (const PrintObject *pobj : print.objects()) {
        const auto &obj_layers = pobj->layers();
        for (size_t li = 0; li < std::min(obj_layers.size(), num_layers); ++li) {
            const Layer &layer = *obj_layers[li];
            for (size_t ri = 0; ri < layer.regions().size(); ++ri) {
                const LayerRegion *region = layer.regions()[ri];
                if (!region || !region->has_extrusions())
                    continue;

                unsigned int extruder_id = region->region().config().wall_filament.value - 1;

                // Collect individual entity bboxes
                std::vector<BoundingBox> entity_bboxes;
                for (const ExtrusionEntity *ee : region->perimeters.entities)
                    collect_bboxes(ee, entity_bboxes);
                for (const ExtrusionEntity *ee : region->fills.entities)
                    collect_bboxes(ee, entity_bboxes);

                if (!entity_bboxes.empty()) {
                    // Single-object merged bbox (for isolation check)
                    BoundingBox region_bbox = entity_bboxes.front();
                    for (size_t i = 1; i < entity_bboxes.size(); ++i)
                        region_bbox.merge(entity_bboxes[i]);

                    auto it = layer_extruder_bboxes_single[li].find(extruder_id);
                    if (it != layer_extruder_bboxes_single[li].end())
                        it->second.merge(region_bbox);
                    else
                        layer_extruder_bboxes_single[li][extruder_id] = region_bbox;

                    // Per-entity bboxes in plate coordinates (for clustering)
                    auto &plate_bboxes = layer_extruder_entity_bboxes[li][extruder_id];
                    for (const PrintInstance &inst : pobj->instances()) {
                        for (const auto &eb : entity_bboxes) {
                            BoundingBox pb = eb;
                            pb.translate(inst.shift);
                            plate_bboxes.push_back(pb);
                        }
                    }
                }
            }
        }
    }

    // Debug: log per-extruder bboxes - every layer from 270-298, plus every 50th before that
    for (size_t li = 0; li < num_layers; ++li) {
        if (li < 270 && li % 50 != 0) continue;
        // Debug: show single-object bboxes (the ones used for isolation check)
        const auto &eb = layer_extruder_bboxes_single[li];
        if (!eb.empty()) {
            fprintf(stderr, "  L%zu z=%.2f extruders=%zu:", li, layers[li]->print_z, eb.size());
            for (auto &[eid, bbox] : eb) {
                fprintf(stderr, " E%u[%.1f,%.1f-%.1f,%.1f]", eid,
                    unscale<double>(bbox.min.x()), unscale<double>(bbox.min.y()),
                    unscale<double>(bbox.max.x()), unscale<double>(bbox.max.y()));
            }
            fprintf(stderr, "\n");
        }
    }

    // For each layer and each extruder, check if lookahead is possible.
    // Two strategies:
    // 1. BBox isolation: extruder's bbox is far from all others (works for separate objects)
    // 2. Disappearing extruder: extruder exists on this layer but disappears within max_height
    //    above (we can batch its remaining layers). This is the most common case for painted
    //    models where gold nubs etc. end before the main body.
    for (size_t li = 0; li < num_layers; ++li) {
        const auto &extruder_bboxes = layer_extruder_bboxes_single[li];

        // Need at least 2 extruders on this layer for a tool change to exist
        if (extruder_bboxes.size() < 2)
            continue;

        for (const auto &[ext_id, ext_bbox] : extruder_bboxes) {
            // Strategy 1: BBox isolation
            bool bbox_isolated = true;
            for (const auto &[other_id, other_bbox] : extruder_bboxes) {
                if (other_id == ext_id)
                    continue;
                if (bboxes_too_close(ext_bbox, other_bbox, clearance_scaled)) {
                    bbox_isolated = false;
                    break;
                }
            }

            // Strategy 2: Disappearing extruder - this extruder's last layer is within
            // max_height above the current layer, and it has continuous presence until then
            bool disappearing = false;
            if (!bbox_isolated) {
                double layer_height = (li + 1 < num_layers)
                    ? (layers[li + 1]->print_z - layers[li]->print_z) : layers[li]->height;
                size_t max_look = (size_t)(max_lookahead_height_mm / layer_height);

                // Check: does this extruder disappear within max_look layers?
                size_t last_present = li;
                for (size_t k = 1; k <= max_look && li + k < num_layers; ++k) {
                    if (layer_extruder_bboxes_single[li + k].count(ext_id))
                        last_present = li + k;
                    else
                        break;
                }
                // If the extruder disappears within the lookahead window AND there are
                // other extruders above it (meaning tool changes would otherwise be needed),
                // it's a candidate
                if (last_present < li + max_look && last_present > li &&
                    last_present + 1 < num_layers && !layer_extruder_bboxes_single[last_present + 1].count(ext_id)) {
                    // Check that OTHER extruders continue above (so there would be tool changes)
                    bool others_continue = false;
                    for (size_t k = last_present + 1; k < num_layers && k <= last_present + 3; ++k) {
                        if (!layer_extruder_bboxes_single[k].empty()) {
                            others_continue = true;
                            break;
                        }
                    }
                    disappearing = others_continue;
                    if (disappearing) {
                        fprintf(stderr, "  DISAPPEARING: L%zu ext=%u last_present=L%zu\n", li, ext_id, last_present);
                    }
                }
            }

            if (!bbox_isolated && !disappearing)
                continue;

            // This extruder is isolated on this layer. Check how many future layers
            // we can print ahead.
            double layer_height = (li + 1 < num_layers)
                ? (layers[li + 1]->print_z - layers[li]->print_z)
                : layers[li]->height;
            size_t max_extra = (size_t)(max_lookahead_height_mm / layer_height);
            if (max_extra == 0)
                continue;

            // For disappearing extruders, we already know exactly how many layers
            // ahead we can print (the extruder is present until last_present)
            // Skip the forward bbox scan and go directly to recording the plan
            if (disappearing) {
                // Find last_present again (was computed in the heuristic above)
                size_t last_pres = li;
                for (size_t k = 1; k <= max_extra && li + k < num_layers; ++k) {
                    if (layer_extruder_bboxes_single[li + k].count(ext_id))
                        last_pres = li + k;
                    else
                        break;
                }
                size_t extra = last_pres - li;
                if (extra > 0 && extra <= max_extra) {
                    LookaheadEntry entry;
                    entry.extra_layers = extra;
                    entry.raised_z = layers[li + extra]->print_z;
                    entry.raised_bbox = ext_bbox;
                    // Cluster entity bboxes into spatially connected groups,
                    // then filter: only keep clusters isolated from other extruders
                    auto ent_it = layer_extruder_entity_bboxes[li].find(ext_id);
                    if (ent_it != layer_extruder_entity_bboxes[li].end()) {
                        auto clusters = cluster_bboxes(ent_it->second, clearance_scaled);
                        for (auto &cb : clusters) {
                            // Check this cluster against all other extruders' entities
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
                            if (cluster_isolated) {
                                cb.offset(clearance_scaled);
                                entry.exclusion_bboxes.push_back(cb);
                            }
                        }
                    }
                    if (entry.exclusion_bboxes.empty()) {
                        // No isolated clusters — skip this plan entry
                        continue;
                    }

                    m_plan[{li, ext_id}] = entry;

                    for (size_t k = 1; k <= extra; ++k) {
                        size_t future_li = li + k;
                        if (future_li < m_raised_per_layer.size()) {
                            m_raised_per_layer[future_li].max_z = std::max(
                                m_raised_per_layer[future_li].max_z, entry.raised_z);
                            for (const auto &eb : entry.exclusion_bboxes)
                                m_raised_per_layer[future_li].exclusion_bboxes.push_back(eb);
                        }
                    }

                    m_enabled = true;
                    fprintf(stderr, "  PLAN: L%zu ext=%u extra=%zu raised_z=%.2f zones=%zu\n",
                        li, ext_id, extra, entry.raised_z, entry.exclusion_bboxes.size());
                }
                continue; // skip the bbox-based forward scan
            }

            BoundingBox accumulated_bbox = ext_bbox;
            size_t extra = 0;

            for (size_t k = 1; k <= max_extra && li + k < num_layers; ++k) {
                const auto &future_bboxes = layer_extruder_bboxes_single[li + k];

                // Check: does this extruder have extrusions on the future layer?
                auto it = future_bboxes.find(ext_id);
                if (it == future_bboxes.end())
                    break; // No extrusions for this extruder on future layer

                // Merge into accumulated bbox
                BoundingBox merged = accumulated_bbox;
                merged.merge(it->second);

                // Check: is the merged region still isolated from all other extruders
                // on the future layer?
                bool still_isolated = true;
                for (const auto &[other_id, other_bbox] : future_bboxes) {
                    if (other_id == ext_id)
                        continue;
                    if (bboxes_too_close(merged, other_bbox, clearance_scaled)) {
                        still_isolated = false;
                        break;
                    }
                }

                // Also check against other extruders on ALL intermediate layers
                if (still_isolated) {
                    for (size_t m = li; m <= li + k && still_isolated; ++m) {
                        for (const auto &[other_id, other_bbox] : layer_extruder_bboxes_single[m]) {
                            if (other_id == ext_id)
                                continue;
                            if (bboxes_too_close(merged, other_bbox, clearance_scaled)) {
                                still_isolated = false;
                                break;
                            }
                        }
                    }
                }

                if (!still_isolated)
                    break;

                accumulated_bbox = merged;
                extra = k;
            }

            if (extra > 0) {
                LookaheadEntry entry;
                entry.extra_layers = extra;
                entry.raised_z = layers[li + extra]->print_z;
                entry.raised_bbox = accumulated_bbox;
                // Cluster entity bboxes, filter to only isolated clusters
                auto ent_it = layer_extruder_entity_bboxes[li].find(ext_id);
                if (ent_it != layer_extruder_entity_bboxes[li].end()) {
                    auto clusters = cluster_bboxes(ent_it->second, clearance_scaled);
                    for (auto &cb : clusters) {
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
                        if (cluster_isolated) {
                            cb.offset(clearance_scaled);
                            entry.exclusion_bboxes.push_back(cb);
                        }
                    }
                }
                if (entry.exclusion_bboxes.empty())
                    continue; // no isolated clusters

                m_plan[{li, ext_id}] = entry;

                // Record raised regions on future layers
                for (size_t k = 1; k <= extra; ++k) {
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
                    << " extruder " << ext_id << " can print " << extra << " layers ahead"
                    << " (raised_z=" << entry.raised_z << ")";
            }
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
