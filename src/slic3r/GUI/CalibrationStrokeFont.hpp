#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// xyz fork: LCD calculator-style stroke font for calibration labels.
// Each character is rendered as parallelogram segments with diamond-cut ends,
// mimicking a classic 7-segment display aesthetic.
class CalibrationStrokeFont {
public:
    // Generate a mesh for a text string at the given position.
    // Characters supported: 0-9, arrow (→ rendered as ">"), F, space
    // height: total character height in mm
    // thickness: mesh Z height (typically one layer height)
    // Returns a single merged TriangleMesh for the entire string.
    static TriangleMesh render_text(const std::string &text,
                                     double char_height = 5.0,
                                     double thickness = 0.2);

    // Get the width of a rendered string in mm
    static double text_width(const std::string &text, double char_height = 5.0);

    // Generate an arrow mesh (solid right-pointing triangle)
    static TriangleMesh make_arrow(double height, double thickness);

    // A single parallelogram segment with diamond ends
    struct Segment {
        double x1, y1, x2, y2; // centerline start/end
        bool horizontal;        // true = horizontal segment, false = vertical
    };

private:
    // Get segments for a character
    static std::vector<Segment> get_char_segments(char c);

    // Create a parallelogram mesh for one segment
    static indexed_triangle_set make_segment(
        double x1, double y1, double x2, double y2,
        double seg_width, double thickness, bool horizontal,
        double slant_angle = 0.5); // ~30 degrees in radians
};

} // namespace GUI
} // namespace Slic3r
