#pragma once

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include <vector>
#include <map>
#include <set>
#include <tuple>
#include <optional>

namespace Slic3r {

class Print;
class PrintObject;
class Layer;

// Filament Lookahead: pre-analysis of which extruder regions on which layers
// can be printed ahead to reduce tool changes.
class FilamentLookaheadPlan {
public:
    struct LookaheadEntry {
        size_t      extra_layers = 0;       // how many layers ahead to print
        coordf_t    raised_z = 0.;          // max Z of the raised region
        BoundingBox raised_bbox;            // XY bbox of the raised region (scaled coords)
        std::vector<BoundingBox> exclusion_bboxes; // per-instance exclusion zones inflated by clearance
    };

    // Build the plan by analyzing all layers of the print.
    void build(const Print &print,
               double max_lookahead_height_mm,
               double min_clearance_distance_mm);

    bool enabled() const { return m_enabled; }

    // How many extra layers can extruder_id print ahead starting from layer_idx?
    size_t extra_layers(size_t layer_idx, unsigned int extruder_id) const;

    // Has this extruder already been printed on this layer by a prior lookahead?
    bool already_printed(size_t layer_idx, unsigned int extruder_id) const;

    // Mark a layer+extruder as printed by lookahead.
    void mark_printed(size_t layer_idx, unsigned int extruder_id);

    // Get the max raised Z on a given layer (for travel clearance).
    coordf_t max_raised_z(size_t layer_idx) const;

    // Get all exclusion zone bboxes active on a given layer.
    std::vector<BoundingBox> exclusion_zones(size_t layer_idx) const;

    // Phase 3a: Any-Type support filament override.
    // Key: (object, support_layer_idx_matched_to_object_layer, is_interface).
    // Value: 0-based extruder id the "Any (Type)" support should be resolved to.
    // Consumed by Phase 5a in GCode.cpp and ToolOrdering.cpp to short-circuit the
    // default resolver. Returns empty optional if no override applies.
    std::optional<unsigned int> override_for_support(
        const PrintObject *object, size_t layer_idx, bool is_interface) const;

private:
    bool m_enabled = false;

    // Per (layer_idx, extruder_id) lookahead opportunities
    std::map<std::pair<size_t, unsigned int>, LookaheadEntry> m_plan;

    // Set of (layer_idx, extruder_id) already printed via lookahead
    std::set<std::pair<size_t, unsigned int>> m_already_printed;

    // Per-layer raised region info (from active lookaheads started on earlier layers)
    struct RaisedInfo {
        coordf_t max_z = 0.;
        std::vector<BoundingBox> exclusion_bboxes;
    };
    std::vector<RaisedInfo> m_raised_per_layer;

    // Phase 3a: resolved overrides for "Any (Type)" supports.
    // Keyed by (object, layer_idx, is_interface). Gcode emitters consult this map
    // before calling the default resolver.
    std::map<std::tuple<const PrintObject*, size_t, bool>, unsigned int> m_any_support_overrides;
};

} // namespace Slic3r
