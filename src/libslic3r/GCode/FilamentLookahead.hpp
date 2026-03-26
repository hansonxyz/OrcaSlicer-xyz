#pragma once

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include <vector>
#include <map>
#include <set>

namespace Slic3r {

class Print;
class Layer;

// Filament Lookahead: pre-analysis of which extruder regions on which layers
// can be printed ahead to reduce tool changes.
class FilamentLookaheadPlan {
public:
    struct LookaheadEntry {
        size_t      extra_layers = 0;       // how many layers ahead to print
        coordf_t    raised_z = 0.;          // max Z of the raised region
        BoundingBox raised_bbox;            // XY bbox of the raised region (scaled coords)
        BoundingBox exclusion_bbox;         // raised_bbox inflated by clearance
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
};

} // namespace Slic3r
