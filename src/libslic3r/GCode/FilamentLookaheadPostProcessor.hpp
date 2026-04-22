#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// Phase 6 post-processor: rewrites gcode to actually batch tower extrusions.
// Consumes the LOOKAHEAD_LAYER_INFO / LOOKAHEAD_BLOCK_BEGIN/END markers
// emitted in Phase 5c.
//
// Phase 6a (this revision): parser + round-trip only. Reads the gcode,
// identifies blocks + per-layer metadata, writes back unchanged. Lays the
// groundwork for Phase 6b (block extraction/relocation), 6c (Z bracketing),
// 6d (wipe tower per-layer rewrite), 6e (config flag).
class FilamentLookaheadPostProcessor {
public:
    // Parsed tower block, located by line indices in the original gcode.
    struct TowerBlock {
        size_t       begin_line = 0;    // index of BLOCK_BEGIN comment
        size_t       end_line   = 0;    // index of matching BLOCK_END comment
        size_t       layer       = 0;
        unsigned int extruder    = 0;
        size_t       base_layer  = 0;
        size_t       stack_index = 0;
        size_t       extra_layers = 0;
        bool         is_base     = false; // role=base when stack_index==0
        double       z           = 0.0;   // layer's print_z, from BLOCK_BEGIN z= attribute
    };

    // Per-layer metadata from LAYER_INFO markers.
    struct LayerInfo {
        size_t                   layer_idx = 0;
        size_t                   line_idx  = 0;
        std::vector<unsigned int> tower_filaments; // filament ids that have towers here
    };

    // Run the post-processor on the gcode at `path`. Returns true on success.
    // On parse error or validation failure, logs and returns false without
    // modifying the file.
    //
    // If apply_transform is true, tower extra-layer blocks are extracted and
    // relocated to their base layer's tower pass with Z-brackets between
    // stack levels (Phase 6b + 6c). If false, the file round-trips unchanged
    // (Phase 6a baseline — used when filament_lookahead_post_process is off).
    bool process(const std::string &path, bool apply_transform);

    // Stats populated after a successful process() call.
    size_t blocks_found()     const { return m_blocks.size(); }
    size_t layers_indexed()   const { return m_layers.size(); }

private:
    std::vector<std::string> m_lines;
    std::vector<TowerBlock>  m_blocks;
    std::vector<LayerInfo>   m_layers;

    // Parse lines in m_lines into m_blocks and m_layers. Returns false on
    // unbalanced markers or malformed attribute lines.
    bool parse();

    // Write m_lines back to `path`. Returns false on I/O error.
    bool write(const std::string &path) const;
};

} // namespace Slic3r
