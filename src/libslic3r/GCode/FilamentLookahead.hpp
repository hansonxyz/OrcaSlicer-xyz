#pragma once

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <optional>

namespace Slic3r {

class Print;
class PrintObject;
class SupportLayer;
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

    // Phase 5b: set of filament (extruder) ids whose tower covers this layer
    // (either as base layer or as an extra-layer of a tower rooted below).
    // Used to partition the per-layer extruder list into normal-mode first,
    // tower-mode last, so that by the time tower emission begins the normal
    // regions for this layer are already printed (Rule 8).
    const std::set<unsigned int>& tower_filaments_on_layer(size_t layer_idx) const;

    // Phase 3a: Any-Type support filament override.
    // Key: (support_layer_ptr, is_interface). SupportLayer pointers are stable
    // between Phase A analysis and gcode emission since Print state isn't mutated.
    // Value: 0-based extruder id the "Any (Type)" support should be resolved to.
    // Consumed by Phase 5a in GCode.cpp to short-circuit the default resolver.
    // Returns empty optional if no override applies.
    std::optional<unsigned int> override_for_support(
        const SupportLayer *support_layer, bool is_interface) const;

    // Enumerate all override keys so callers (e.g. GCode.cpp) can inject the
    // overridden extruders into tool_ordering's per-layer extruder list.
    const std::map<std::pair<const SupportLayer*, bool>, unsigned int>&
        any_support_overrides() const { return m_any_support_overrides; }

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

    // Phase 5b: per-layer set of filaments whose tower covers this layer
    // (base or extra). Populated when each tower is accepted during build().
    std::vector<std::set<unsigned int>> m_tower_filaments_per_layer;
    std::set<unsigned int>              m_empty_filaments; // for out-of-range returns

    // Phase 3a: resolved overrides for "Any (Type)" supports.
    // Keyed by (support_layer_ptr, is_interface).
    std::map<std::pair<const SupportLayer*, bool>, unsigned int> m_any_support_overrides;
};

} // namespace Slic3r
