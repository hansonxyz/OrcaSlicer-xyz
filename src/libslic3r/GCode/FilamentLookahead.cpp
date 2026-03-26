#include "FilamentLookahead.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"
#include "../PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <queue>

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

    BOOST_LOG_TRIVIAL(info) << "FilamentLookahead: analyzing " << num_layers << " layers"
        << " max_height=" << max_lookahead_height_mm << "mm"
        << " clearance=" << min_clearance_distance_mm << "mm"
        << " num_objects=" << print.objects().size();
    fprintf(stderr, "FilamentLookahead: analyzing %zu layers, max_height=%.1fmm, clearance=%.1fmm, objects=%zu\n",
        num_layers, max_lookahead_height_mm, min_clearance_distance_mm, print.objects().size());

    // Pre-compute per-extruder bboxes for all layers.
    // Key insight: check isolation PER INSTANCE, not across all instances merged.
    // An extruder is "globally isolated" if it's isolated on EVERY instance.
    // This handles plates with multiple objects where each object's gold nub is
    // isolated from its own body but the merged bboxes would overlap.

    // Helper to collect bbox from extrusion entities recursively
    std::function<void(const ExtrusionEntity*, BoundingBox&, bool&)> collect_bbox;
    collect_bbox = [&](const ExtrusionEntity *ee, BoundingBox &bbox, bool &has) {
        if (!ee) return;
        if (auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
            for (const ExtrusionEntity *child : coll->entities)
                collect_bbox(child, bbox, has);
        } else {
            BoundingBox ee_bbox = get_extents(ee->as_polyline());
            if (has) bbox.merge(ee_bbox);
            else { bbox = ee_bbox; has = true; }
        }
    };

    // For the merged view (used for the final plan), we still need per-extruder bboxes
    std::vector<std::map<unsigned int, BoundingBox>> layer_extruder_bboxes(num_layers);

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

                BoundingBox region_bbox;
                bool has_bbox = false;
                for (const ExtrusionEntity *ee : region->perimeters.entities)
                    collect_bbox(ee, region_bbox, has_bbox);
                for (const ExtrusionEntity *ee : region->fills.entities)
                    collect_bbox(ee, region_bbox, has_bbox);

                if (has_bbox) {
                    // Single-object bboxes (no instance translation)
                    auto it = layer_extruder_bboxes_single[li].find(extruder_id);
                    if (it != layer_extruder_bboxes_single[li].end())
                        it->second.merge(region_bbox);
                    else
                        layer_extruder_bboxes_single[li][extruder_id] = region_bbox;

                    // Merged bboxes (with instance translation, for the plan)
                    for (const PrintInstance &inst : pobj->instances()) {
                        BoundingBox inst_bbox = region_bbox;
                        inst_bbox.translate(inst.shift);
                        auto it2 = layer_extruder_bboxes[li].find(extruder_id);
                        if (it2 != layer_extruder_bboxes[li].end())
                            it2->second.merge(inst_bbox);
                        else
                            layer_extruder_bboxes[li][extruder_id] = inst_bbox;
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
                    entry.exclusion_bbox = ext_bbox;
                    entry.exclusion_bbox.offset(clearance_scaled);

                    m_plan[{li, ext_id}] = entry;

                    for (size_t k = 1; k <= extra; ++k) {
                        size_t future_li = li + k;
                        if (future_li < m_raised_per_layer.size()) {
                            m_raised_per_layer[future_li].max_z = std::max(
                                m_raised_per_layer[future_li].max_z, entry.raised_z);
                            m_raised_per_layer[future_li].exclusion_bboxes.push_back(entry.exclusion_bbox);
                        }
                    }

                    m_enabled = true;
                    fprintf(stderr, "  PLAN: L%zu ext=%u extra=%zu raised_z=%.2f\n",
                        li, ext_id, extra, entry.raised_z);
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
                entry.exclusion_bbox = accumulated_bbox;
                entry.exclusion_bbox.offset(clearance_scaled);

                m_plan[{li, ext_id}] = entry;

                // Record raised regions on future layers
                for (size_t k = 1; k <= extra; ++k) {
                    size_t future_li = li + k;
                    if (future_li < m_raised_per_layer.size()) {
                        m_raised_per_layer[future_li].max_z = std::max(
                            m_raised_per_layer[future_li].max_z, entry.raised_z);
                        m_raised_per_layer[future_li].exclusion_bboxes.push_back(entry.exclusion_bbox);
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
