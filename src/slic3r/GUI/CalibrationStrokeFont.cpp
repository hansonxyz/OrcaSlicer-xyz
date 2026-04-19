#include "CalibrationStrokeFont.hpp"
#include <cmath>
#include <algorithm>

namespace Slic3r {
namespace GUI {

// 7-segment style layout:
//
//   --a--
//  |     |
//  f     b
//  |     |
//   --g--
//  |     |
//  e     c
//  |     |
//   --d--
//
// Segments are defined in a 1.0 x 2.0 unit cell (width x height)
// with the origin at bottom-left.

static const double CELL_W = 1.0;
static const double CELL_H = 2.0;
static const double SEG_GAP = 0.08; // gap between segment end and corner

// Segment definitions in unit coordinates
static const CalibrationStrokeFont::Segment SEG_A = { SEG_GAP, CELL_H,       CELL_W - SEG_GAP, CELL_H,       true  };
static const CalibrationStrokeFont::Segment SEG_B = { CELL_W,  CELL_H - SEG_GAP, CELL_W,       CELL_H/2 + SEG_GAP, false };
static const CalibrationStrokeFont::Segment SEG_C = { CELL_W,  CELL_H/2 - SEG_GAP, CELL_W,     SEG_GAP,            false };
static const CalibrationStrokeFont::Segment SEG_D = { SEG_GAP, 0,            CELL_W - SEG_GAP, 0,            true  };
static const CalibrationStrokeFont::Segment SEG_E = { 0,       SEG_GAP,      0,                CELL_H/2 - SEG_GAP, false };
static const CalibrationStrokeFont::Segment SEG_F = { 0,       CELL_H/2 + SEG_GAP, 0,          CELL_H - SEG_GAP,   false };
static const CalibrationStrokeFont::Segment SEG_G = { SEG_GAP, CELL_H/2,     CELL_W - SEG_GAP, CELL_H/2,     true  };

std::vector<CalibrationStrokeFont::Segment> CalibrationStrokeFont::get_char_segments(char c)
{
    //        a b c d e f g
    // 0:     1 1 1 1 1 1 0
    // 1:     0 1 1 0 0 0 0
    // 2:     1 1 0 1 1 0 1
    // 3:     1 1 1 1 0 0 1
    // 4:     0 1 1 0 0 1 1
    // 5:     1 0 1 1 0 1 1
    // 6:     1 0 1 1 1 1 1
    // 7:     1 1 1 0 0 0 0
    // 8:     1 1 1 1 1 1 1
    // 9:     1 1 1 1 0 1 1

    switch (c) {
    case '0': return { SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F };
    case '1': return { SEG_B, SEG_C };
    case '2': return { SEG_A, SEG_B, SEG_D, SEG_E, SEG_G };
    case '3': return { SEG_A, SEG_B, SEG_C, SEG_D, SEG_G };
    case '4': return { SEG_B, SEG_C, SEG_F, SEG_G };
    case '5': return { SEG_A, SEG_C, SEG_D, SEG_F, SEG_G };
    case '6': return { SEG_A, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G };
    case '7': return { SEG_A, SEG_B, SEG_C };
    case '8': return { SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G };
    case '9': return { SEG_A, SEG_B, SEG_C, SEG_D, SEG_F, SEG_G };
    case 'F':
    case 'f': return { SEG_A, SEG_E, SEG_F, SEG_G };
    case '>': {
        // Arrow handled separately via make_arrow — return empty so render_text uses arrow mesh
        return {};
    }
    case '-': return { SEG_G };
    case ' ': return {};
    default: return {};
    }
}

indexed_triangle_set CalibrationStrokeFont::make_segment(
    double x1, double y1, double x2, double y2,
    double seg_width, double thickness, bool horizontal,
    double slant_angle)
{
    indexed_triangle_set its;

    double half_w = seg_width / 2.0;
    double slant = half_w * std::tan(slant_angle); // diamond overshoot

    if (horizontal) {
        // Horizontal parallelogram with diamond ends
        //
        //     /----\        (top edge)
        //    <      >       (diamond ends)
        //     \----/        (bottom edge)
        //
        // 6 vertices forming a hexagon (diamond at each end)
        double left_x  = x1;
        double right_x = x2;
        double cy = y1; // y1 == y2 for horizontal

        // Left diamond point, top-left, top-right, right diamond, bottom-right, bottom-left
        its.vertices = {
            Vec3f((float)(left_x - slant), (float)cy, 0),              // 0: left diamond tip
            Vec3f((float)left_x, (float)(cy + half_w), 0),             // 1: top-left
            Vec3f((float)right_x, (float)(cy + half_w), 0),            // 2: top-right
            Vec3f((float)(right_x + slant), (float)cy, 0),             // 3: right diamond tip
            Vec3f((float)right_x, (float)(cy - half_w), 0),            // 4: bottom-right
            Vec3f((float)left_x, (float)(cy - half_w), 0),             // 5: bottom-left
            // Top face (offset by thickness in Z)
            Vec3f((float)(left_x - slant), (float)cy, (float)thickness),
            Vec3f((float)left_x, (float)(cy + half_w), (float)thickness),
            Vec3f((float)right_x, (float)(cy + half_w), (float)thickness),
            Vec3f((float)(right_x + slant), (float)cy, (float)thickness),
            Vec3f((float)right_x, (float)(cy - half_w), (float)thickness),
            Vec3f((float)left_x, (float)(cy - half_w), (float)thickness),
        };

        // Bottom face (0-5), top face (6-11), sides
        its.indices = {
            // Bottom face (2 quads = 4 triangles)
            Vec3i32(0, 5, 1), Vec3i32(1, 5, 4), Vec3i32(1, 4, 2), Vec3i32(2, 4, 3),
            // Top face
            Vec3i32(6, 7, 11), Vec3i32(7, 10, 11), Vec3i32(7, 8, 10), Vec3i32(8, 9, 10),
            // Sides
            Vec3i32(0, 1, 6), Vec3i32(1, 7, 6),
            Vec3i32(1, 2, 7), Vec3i32(2, 8, 7),
            Vec3i32(2, 3, 8), Vec3i32(3, 9, 8),
            Vec3i32(3, 4, 9), Vec3i32(4, 10, 9),
            Vec3i32(4, 5, 10), Vec3i32(5, 11, 10),
            Vec3i32(5, 0, 11), Vec3i32(0, 6, 11),
        };
    } else {
        // Vertical parallelogram with diamond ends (top and bottom)
        double cx = x1; // x1 == x2 for vertical
        double bot_y = std::min(y1, y2);
        double top_y = std::max(y1, y2);

        its.vertices = {
            Vec3f((float)cx, (float)(bot_y - slant), 0),               // 0: bottom diamond
            Vec3f((float)(cx + half_w), (float)bot_y, 0),              // 1: bottom-right
            Vec3f((float)(cx + half_w), (float)top_y, 0),              // 2: top-right
            Vec3f((float)cx, (float)(top_y + slant), 0),               // 3: top diamond
            Vec3f((float)(cx - half_w), (float)top_y, 0),              // 4: top-left
            Vec3f((float)(cx - half_w), (float)bot_y, 0),              // 5: bottom-left
            // Top face
            Vec3f((float)cx, (float)(bot_y - slant), (float)thickness),
            Vec3f((float)(cx + half_w), (float)bot_y, (float)thickness),
            Vec3f((float)(cx + half_w), (float)top_y, (float)thickness),
            Vec3f((float)cx, (float)(top_y + slant), (float)thickness),
            Vec3f((float)(cx - half_w), (float)top_y, (float)thickness),
            Vec3f((float)(cx - half_w), (float)bot_y, (float)thickness),
        };

        its.indices = {
            Vec3i32(0, 5, 1), Vec3i32(1, 5, 4), Vec3i32(1, 4, 2), Vec3i32(2, 4, 3),
            Vec3i32(6, 7, 11), Vec3i32(7, 10, 11), Vec3i32(7, 8, 10), Vec3i32(8, 9, 10),
            Vec3i32(0, 1, 6), Vec3i32(1, 7, 6),
            Vec3i32(1, 2, 7), Vec3i32(2, 8, 7),
            Vec3i32(2, 3, 8), Vec3i32(3, 9, 8),
            Vec3i32(3, 4, 9), Vec3i32(4, 10, 9),
            Vec3i32(4, 5, 10), Vec3i32(5, 11, 10),
            Vec3i32(5, 0, 11), Vec3i32(0, 6, 11),
        };
    }

    return its;
}

TriangleMesh CalibrationStrokeFont::render_text(const std::string &text,
                                                  double char_height,
                                                  double thickness)
{
    double scale = char_height / CELL_H; // scale from unit coords to mm
    double seg_width = 0.15 * scale * CELL_W; // segment thickness proportional to char size
    double char_spacing = 0.3 * scale; // gap between characters
    double char_width = CELL_W * scale;

    indexed_triangle_set merged;
    double cursor_x = 0;

    for (char c : text) {
        if (c == '>') {
            // Insert arrow mesh instead of 7-segment character
            double arrow_w = char_width * 0.8;
            double arrow_h = char_height * 0.5;
            double arrow_cx = cursor_x + char_width * 0.5;
            double arrow_cy = char_height * 0.5;

            // Right-pointing filled triangle: 3 vertices bottom + 3 top
            int voffset = (int)merged.vertices.size();
            // Left-top, left-bottom, right-center (bottom face)
            merged.vertices.push_back(Vec3f((float)(arrow_cx - arrow_w/2), (float)(arrow_cy + arrow_h/2), 0));
            merged.vertices.push_back(Vec3f((float)(arrow_cx - arrow_w/2), (float)(arrow_cy - arrow_h/2), 0));
            merged.vertices.push_back(Vec3f((float)(arrow_cx + arrow_w/2), (float)arrow_cy, 0));
            // Top face
            merged.vertices.push_back(Vec3f((float)(arrow_cx - arrow_w/2), (float)(arrow_cy + arrow_h/2), (float)thickness));
            merged.vertices.push_back(Vec3f((float)(arrow_cx - arrow_w/2), (float)(arrow_cy - arrow_h/2), (float)thickness));
            merged.vertices.push_back(Vec3f((float)(arrow_cx + arrow_w/2), (float)arrow_cy, (float)thickness));

            // Bottom face
            merged.indices.push_back(Vec3i32(voffset, voffset+2, voffset+1));
            // Top face
            merged.indices.push_back(Vec3i32(voffset+3, voffset+4, voffset+5));
            // Sides
            merged.indices.push_back(Vec3i32(voffset, voffset+1, voffset+3));
            merged.indices.push_back(Vec3i32(voffset+1, voffset+4, voffset+3));
            merged.indices.push_back(Vec3i32(voffset+1, voffset+2, voffset+4));
            merged.indices.push_back(Vec3i32(voffset+2, voffset+5, voffset+4));
            merged.indices.push_back(Vec3i32(voffset+2, voffset, voffset+5));
            merged.indices.push_back(Vec3i32(voffset, voffset+3, voffset+5));

            cursor_x += char_width + char_spacing;
            continue;
        }

        auto segments = get_char_segments(c);

        for (const auto &seg : segments) {
            double sx1 = cursor_x + seg.x1 * scale;
            double sy1 = seg.y1 * scale;
            double sx2 = cursor_x + seg.x2 * scale;
            double sy2 = seg.y2 * scale;

            auto seg_its = make_segment(sx1, sy1, sx2, sy2,
                                         seg_width, thickness, seg.horizontal);

            int voffset = (int)merged.vertices.size();
            for (const auto &v : seg_its.vertices)
                merged.vertices.push_back(v);
            for (const auto &idx : seg_its.indices)
                merged.indices.push_back(Vec3i32(idx[0] + voffset, idx[1] + voffset, idx[2] + voffset));
        }

        cursor_x += char_width + char_spacing;
    }

    if (merged.vertices.empty())
        return TriangleMesh();

    return TriangleMesh(std::move(merged));
}

double CalibrationStrokeFont::text_width(const std::string &text, double char_height)
{
    if (text.empty()) return 0;
    double scale = char_height / CELL_H;
    double char_width = CELL_W * scale;
    double char_spacing = 0.3 * scale;
    return text.size() * char_width + (text.size() - 1) * char_spacing;
}

TriangleMesh CalibrationStrokeFont::make_arrow(double height, double thickness)
{
    double w = height * 0.8;
    double h = height * 0.5;
    double cx = w / 2, cy = h / 2;

    indexed_triangle_set its;
    its.vertices = {
        Vec3f((float)(cx - w/2), (float)(cy + h/2), 0),
        Vec3f((float)(cx - w/2), (float)(cy - h/2), 0),
        Vec3f((float)(cx + w/2), (float)cy, 0),
        Vec3f((float)(cx - w/2), (float)(cy + h/2), (float)thickness),
        Vec3f((float)(cx - w/2), (float)(cy - h/2), (float)thickness),
        Vec3f((float)(cx + w/2), (float)cy, (float)thickness),
    };
    its.indices = {
        Vec3i32(0, 2, 1), Vec3i32(3, 4, 5),
        Vec3i32(0, 1, 3), Vec3i32(1, 4, 3),
        Vec3i32(1, 2, 4), Vec3i32(2, 5, 4),
        Vec3i32(2, 0, 5), Vec3i32(0, 3, 5),
    };
    return TriangleMesh(std::move(its));
}

} // namespace GUI
} // namespace Slic3r
