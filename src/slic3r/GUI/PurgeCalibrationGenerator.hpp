#pragma once

#include <vector>
#include <string>
#include <utility>

namespace Slic3r {

class Model;
class ModelObject;
class TriangleMesh;

namespace GUI {

// xyz fork: Generates a purge calibration test print.
// Self-contained — uses OrcaSlicer's public APIs to create a temp plate,
// add geometry, slice, post-process gcode, and load into preview.
class PurgeCalibrationGenerator {
public:
    struct TransitionPair {
        int from_filament; // 0-based index
        int to_filament;   // 0-based index
    };

    struct Options {
        std::vector<TransitionPair> pairs;
        int base_layers = 2;
        int base_filament = 0;     // 0-based
        int label_filament = 0;    // 0-based
        int strip_filament = 1;    // 0-based — the "dummy" filament for all strips
        double strip_speed = 20.0; // mm/s for calibration strips
    };

    // Run the full generation pipeline.
    // Returns true if gcode was generated and loaded into preview.
    static bool generate(const Options &opts);

private:
    // Generate a simple rectangular mesh (thin box)
    static TriangleMesh make_rect(double width, double length, double height);

    // Calculate strip dimensions for a given flush volume
    struct StripDims {
        double width;
        double length;
        double volume_per_mm; // mm³ per mm of strip length
    };
    static StripDims calc_strip_dims(double flush_volume_mm3,
                                      double layer_height,
                                      double extrusion_width);

    // Order transition pairs to minimize filament changes
    static std::vector<TransitionPair> order_transitions(
        const std::vector<TransitionPair> &pairs);

    // Post-process gcode: replace slicer's filament changes with our custom sequence
    static std::string postprocess_gcode(const std::string &gcode,
                                          const std::vector<TransitionPair> &ordered_pairs,
                                          int label_filament,
                                          int strip_filament,
                                          const std::string &change_filament_gcode_template);
};

} // namespace GUI
} // namespace Slic3r
