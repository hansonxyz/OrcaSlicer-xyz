#include "PaintToolBoundary.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

#include <GL/glew.h>
#include <chrono>

namespace Slic3r {
namespace GUI {

void PaintToolBoundary::init(const TriangleMesh &mesh)
{
    clear();

    const auto &its = mesh.its;
    // Convert stl_vertex vector to work with MeshPathFinder
    // TriangleMesh::its.vertices is std::vector<stl_vertex>
    // TriangleMesh::its.indices is std::vector<Vec3i32>
    // We need triangle neighbors — compute from the indexed_triangle_set
    std::vector<Vec3i32> neighbors = its_face_neighbors(its);

    m_path_finder = std::make_unique<MeshPathFinder>(
        its.vertices, its.indices, neighbors);

    // Store neighbors for later use (MeshPathFinder borrows the reference,
    // so we need to own a copy since its_face_neighbors returns a temporary)
    m_neighbors_storage = std::move(neighbors);
    m_path_finder = std::make_unique<MeshPathFinder>(
        its.vertices, its.indices, m_neighbors_storage);

    // Compute face normals for planar intersection
    m_face_normals_storage.resize(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto &tri = its.indices[i];
        Vec3f e1 = its.vertices[tri[1]] - its.vertices[tri[0]];
        Vec3f e2 = its.vertices[tri[2]] - its.vertices[tri[0]];
        m_face_normals_storage[i] = e1.cross(e2).normalized();
    }
}

void PaintToolBoundary::clear()
{
    m_completed_paths.clear();
    m_boundary_edges.clear();
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_pending_segment_is_straight.clear();
    m_prev_hit_stack.clear();
    m_preview_path.clear();
    m_last_hit_facet = -1;
    m_boundaries_model.reset();
    m_boundaries_model_alt.reset();
    m_preview_model.reset();
    m_points_model.reset();
    m_boundaries_dirty = true;
    m_preview_dirty = true;
    // Don't reset m_path_finder — keep it for the current mesh
}

bool PaintToolBoundary::add_point(const Vec3f &hit, int facet_idx,
                                   bool snap_to_curve, float curvature_threshold_deg,
                                   bool direct_path)
{
    if (!m_path_finder)
        return false;

    int vertex = m_path_finder->nearest_vertex_on_triangle(hit, facet_idx);
    if (vertex < 0)
        return false;

    // Check if clicking near the start point to close the boundary
    if (m_pending_vertices.size() >= 3) {
        int start = m_pending_vertices.front();
        if (vertex == start) {
            // Close the boundary: add path from last vertex back to start
            auto closing_path = direct_path
                ? m_path_finder->find_path_direct(m_pending_vertices.back(), start)
                : m_path_finder->find_path(m_pending_vertices.back(), start);
            if (!closing_path.empty()) {
                // Build the full closed path
                std::vector<int> full_path = m_pending_vertices;
                // Append closing segment (skip first vertex since it's already the last pending)
                for (size_t i = 1; i < closing_path.size(); ++i)
                    full_path.push_back(closing_path[i]);

                m_completed_paths.push_back(full_path);
                add_path_edges(full_path);
            }
            m_pending_vertices.clear();
            m_pending_segment_marks.clear();
            m_preview_path.clear();
            m_boundaries_dirty = true;
            m_preview_dirty = true;
            return true; // boundary closed
        }
    }

    // Not closing — add a new segment
    // Record segment mark for undo support
    m_pending_segment_marks.push_back(m_pending_vertices.size());

    // Save previous hit for undo
    m_prev_hit_stack.push_back({m_last_hit_pos, m_last_hit_facet});

    if (!m_pending_vertices.empty()) {
        // Find path — either planar (shift) or A* (default)
        std::vector<int> segment;
        if (direct_path && m_last_hit_facet >= 0
            && m_last_hit_facet < (int)m_face_normals_storage.size()
            && facet_idx < (int)m_face_normals_storage.size()) {
            segment = m_path_finder->find_path_planar(
                m_last_hit_pos, m_last_hit_facet, hit, facet_idx,
                m_face_normals_storage[m_last_hit_facet],
                m_face_normals_storage[facet_idx]);
            m_pending_segment_is_straight.push_back(true);
        } else {
            segment = m_path_finder->find_path(m_pending_vertices.back(), vertex);
            m_pending_segment_is_straight.push_back(false);
        }
        if (segment.size() > 1) {
            // Skip first vertex if it matches the last pending (avoid duplicate)
            size_t start_i = (segment.front() == m_pending_vertices.back()) ? 1 : 0;
            for (size_t i = start_i; i < segment.size(); ++i)
                m_pending_vertices.push_back(segment[i]);
        } else {
            m_pending_vertices.push_back(vertex);
        }
    } else {
        // First point
        m_pending_vertices.push_back(vertex);
    }

    m_last_hit_pos = hit;
    m_last_hit_facet = facet_idx;

    m_boundaries_dirty = true;
    m_preview_dirty = true;
    return false; // not closed yet
}

void PaintToolBoundary::cancel_current()
{
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_pending_segment_is_straight.clear();
    m_prev_hit_stack.clear();
    m_preview_path.clear();
    m_last_hit_facet = -1;
    m_boundaries_dirty = true;
    m_preview_dirty = true;
}

bool PaintToolBoundary::close_boundary()
{
    if (!m_path_finder || m_pending_vertices.size() < 2)
        return false;

    int start = m_pending_vertices.front();
    int last = m_pending_vertices.back();

    // Find path from last vertex back to start
    auto closing_path = m_path_finder->find_path(last, start);
    if (!closing_path.empty()) {
        std::vector<int> full_path = m_pending_vertices;
        for (size_t i = 1; i < closing_path.size(); ++i)
            full_path.push_back(closing_path[i]);

        m_completed_paths.push_back(full_path);
        add_path_edges(full_path);
    }
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_preview_path.clear();
    m_boundaries_dirty = true;
    m_preview_dirty = true;
    return true;
}

void PaintToolBoundary::undo_last_segment()
{
    if (!m_pending_segment_marks.empty()) {
        size_t mark = m_pending_segment_marks.back();
        m_pending_segment_marks.pop_back();
        m_pending_vertices.resize(mark);

        if (!m_pending_segment_is_straight.empty())
            m_pending_segment_is_straight.pop_back();

        // Restore previous hit position for correct planar origin
        if (!m_prev_hit_stack.empty()) {
            auto [prev_pos, prev_facet] = m_prev_hit_stack.back();
            m_prev_hit_stack.pop_back();
            m_last_hit_pos = prev_pos;
            m_last_hit_facet = prev_facet;
        }

        m_preview_path.clear();
    } else if (!m_completed_paths.empty()) {
        m_completed_paths.pop_back();
        rebuild_edge_set();
    } else {
        return;
    }
    m_boundaries_dirty = true;
    m_preview_dirty = true;
}

void PaintToolBoundary::update_preview(const Vec3f &cursor_hit, int cursor_facet,
                                        bool snap_to_curve, float curvature_threshold_deg,
                                        bool direct_path)
{
    m_preview_path.clear();
    if (!m_path_finder || m_pending_vertices.empty())
        return;

    int cursor_vertex = m_path_finder->nearest_vertex_on_triangle(cursor_hit, cursor_facet);
    if (cursor_vertex < 0)
        return;

    m_preview_is_direct = direct_path;
    if (direct_path && m_last_hit_facet >= 0
        && m_last_hit_facet < (int)m_face_normals_storage.size()
        && cursor_facet < (int)m_face_normals_storage.size()) {
        m_preview_path = m_path_finder->find_path_planar(
            m_last_hit_pos, m_last_hit_facet, cursor_hit, cursor_facet,
            m_face_normals_storage[m_last_hit_facet],
            m_face_normals_storage[cursor_facet]);
    } else {
        m_preview_path = m_path_finder->find_path(m_pending_vertices.back(), cursor_vertex);
    }
    m_preview_dirty = true;
}

// 3D segment-segment crossing test.
// Two 3D segments "cross" if they are close and their projections onto
// a common plane intersect. Works for segments on a mesh surface.
static bool segments_cross_3d(const Vec3f &a1, const Vec3f &a2, const Vec3f &b1, const Vec3f &b2)
{
    // Find the best projection plane (drop the axis with smallest segment extent)
    Vec3f extent = (a2 - a1).cwiseAbs() + (b2 - b1).cwiseAbs();
    int drop_axis;
    extent.minCoeff(&drop_axis);

    int ax0 = (drop_axis + 1) % 3;
    int ax1 = (drop_axis + 2) % 3;

    Vec2f pa1(a1[ax0], a1[ax1]), pa2(a2[ax0], a2[ax1]);
    Vec2f pb1(b1[ax0], b1[ax1]), pb2(b2[ax0], b2[ax1]);

    Vec2f d1 = pa2 - pa1, d2 = pb2 - pb1;
    float denom = d1.x() * d2.y() - d1.y() * d2.x();
    if (std::abs(denom) < 1e-10f) return false;
    Vec2f d = pb1 - pa1;
    float t = (d.x() * d2.y() - d.y() * d2.x()) / denom;
    float u = (d.x() * d1.y() - d.y() * d1.x()) / denom;
    return t > 0.01f && t < 0.99f && u > 0.01f && u < 0.99f;
}

void PaintToolBoundary::sync_to_selector(TriangleSelector &selector) const
{
    selector.clear_boundary_edges();

    if (m_completed_paths.empty())
        return;

    // Collect all boundary edges (sorted vertex index pairs in mesh.its space)
    std::set<std::pair<int,int>> boundary_edge_set;
    for (const auto &path : m_completed_paths) {
        auto edges = MeshPathFinder::path_to_edges(path);
        for (const auto &e : edges)
            boundary_edge_set.insert(e);
    }

    if (!boundary_edge_set.empty())
        selector.set_boundary_edges(boundary_edge_set);
}

void PaintToolBoundary::add_path_edges(const std::vector<int> &path)
{
    auto edges = MeshPathFinder::path_to_edges(path);
    for (const auto &e : edges)
        m_boundary_edges.insert(e);
}

void PaintToolBoundary::rebuild_edge_set()
{
    m_boundary_edges.clear();
    for (const auto &path : m_completed_paths)
        add_path_edges(path);
}

std::string PaintToolBoundary::serialize() const
{
    // Format: completed paths separated by "|", vertex indices separated by ","
    // Pending state appended after "##PENDING##" separator:
    //   pending_vertices|segment_marks|prev_hit_stack
    // Example: "10,20,30|50,60,70##PENDING##5,10,15|0,5|1.0,2.0,3.0,42,4.0,5.0,6.0,43"
    std::string result;
    for (size_t pi = 0; pi < m_completed_paths.size(); ++pi) {
        if (pi > 0) result += '|';
        const auto &path = m_completed_paths[pi];
        for (size_t vi = 0; vi < path.size(); ++vi) {
            if (vi > 0) result += ',';
            result += std::to_string(path[vi]);
        }
    }

    // Serialize pending state if there are pending vertices
    if (!m_pending_vertices.empty()) {
        result += "##PENDING##";
        // Pending vertices
        for (size_t i = 0; i < m_pending_vertices.size(); ++i) {
            if (i > 0) result += ',';
            result += std::to_string(m_pending_vertices[i]);
        }
        result += '|';
        // Segment marks
        for (size_t i = 0; i < m_pending_segment_marks.size(); ++i) {
            if (i > 0) result += ',';
            result += std::to_string(m_pending_segment_marks[i]);
        }
        result += '|';
        // Previous hit stack: x,y,z,facet pairs
        for (size_t i = 0; i < m_prev_hit_stack.size(); ++i) {
            if (i > 0) result += ';';
            const auto &[pos, facet] = m_prev_hit_stack[i];
            result += std::to_string(pos.x()) + ',' + std::to_string(pos.y()) + ','
                    + std::to_string(pos.z()) + ',' + std::to_string(facet);
        }
        result += '|';
        // Last hit position and facet
        result += std::to_string(m_last_hit_pos.x()) + ',' + std::to_string(m_last_hit_pos.y()) + ','
                + std::to_string(m_last_hit_pos.z()) + ',' + std::to_string(m_last_hit_facet);
    }

    return result;
}

void PaintToolBoundary::deserialize(const std::string &data)
{
    // Clear all state (keep the path finder intact)
    m_completed_paths.clear();
    m_boundary_edges.clear();
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_pending_segment_is_straight.clear();
    m_prev_hit_stack.clear();
    m_last_hit_pos = Vec3f::Zero();
    m_last_hit_facet = -1;
    m_boundaries_dirty = true;
    m_preview_dirty = true;

    if (data.empty())
        return;

    // Split off pending state if present
    std::string completed_data = data;
    std::string pending_data;
    size_t pending_sep = data.find("##PENDING##");
    if (pending_sep != std::string::npos) {
        completed_data = data.substr(0, pending_sep);
        pending_data = data.substr(pending_sep + 11); // len("##PENDING##") = 11
    }

    // Parse completed paths: "|"-separated paths, each with ","-separated vertex indices
    if (!completed_data.empty()) {
        size_t pos = 0;
        while (pos < completed_data.size()) {
            size_t pipe = completed_data.find('|', pos);
            if (pipe == std::string::npos) pipe = completed_data.size();

            std::string path_str = completed_data.substr(pos, pipe - pos);
            if (!path_str.empty()) {
                std::vector<int> path;
                size_t vpos = 0;
                while (vpos < path_str.size()) {
                    size_t comma = path_str.find(',', vpos);
                    if (comma == std::string::npos) comma = path_str.size();
                    std::string num = path_str.substr(vpos, comma - vpos);
                    if (!num.empty())
                        path.push_back(std::stoi(num));
                    vpos = comma + 1;
                }
                if (path.size() >= 2) {
                    m_completed_paths.push_back(std::move(path));
                }
            }
            pos = pipe + 1;
        }
    }

    // Parse pending state: pending_vertices|segment_marks|prev_hit_stack|last_hit
    if (!pending_data.empty()) {
        // Split by '|' into 4 sections
        std::vector<std::string> sections;
        size_t pos = 0;
        while (pos < pending_data.size()) {
            size_t pipe = pending_data.find('|', pos);
            if (pipe == std::string::npos) pipe = pending_data.size();
            sections.push_back(pending_data.substr(pos, pipe - pos));
            pos = pipe + 1;
        }

        // Section 0: pending vertices
        if (sections.size() > 0 && !sections[0].empty()) {
            size_t vpos = 0;
            while (vpos < sections[0].size()) {
                size_t comma = sections[0].find(',', vpos);
                if (comma == std::string::npos) comma = sections[0].size();
                std::string num = sections[0].substr(vpos, comma - vpos);
                if (!num.empty())
                    m_pending_vertices.push_back(std::stoi(num));
                vpos = comma + 1;
            }
        }

        // Section 1: segment marks
        if (sections.size() > 1 && !sections[1].empty()) {
            size_t vpos = 0;
            while (vpos < sections[1].size()) {
                size_t comma = sections[1].find(',', vpos);
                if (comma == std::string::npos) comma = sections[1].size();
                std::string num = sections[1].substr(vpos, comma - vpos);
                if (!num.empty())
                    m_pending_segment_marks.push_back(std::stoull(num));
                vpos = comma + 1;
            }
        }

        // Section 2: prev hit stack (semicolon-separated entries, each x,y,z,facet)
        if (sections.size() > 2 && !sections[2].empty()) {
            size_t epos = 0;
            while (epos < sections[2].size()) {
                size_t semi = sections[2].find(';', epos);
                if (semi == std::string::npos) semi = sections[2].size();
                std::string entry = sections[2].substr(epos, semi - epos);
                if (!entry.empty()) {
                    std::vector<std::string> parts;
                    size_t ppos = 0;
                    while (ppos < entry.size()) {
                        size_t comma = entry.find(',', ppos);
                        if (comma == std::string::npos) comma = entry.size();
                        parts.push_back(entry.substr(ppos, comma - ppos));
                        ppos = comma + 1;
                    }
                    if (parts.size() >= 4) {
                        Vec3f p(std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]));
                        int facet = std::stoi(parts[3]);
                        m_prev_hit_stack.push_back({p, facet});
                    }
                }
                epos = semi + 1;
            }
        }

        // Section 3: last hit position and facet
        if (sections.size() > 3 && !sections[3].empty()) {
            std::vector<std::string> parts;
            size_t ppos = 0;
            while (ppos < sections[3].size()) {
                size_t comma = sections[3].find(',', ppos);
                if (comma == std::string::npos) comma = sections[3].size();
                parts.push_back(sections[3].substr(ppos, comma - ppos));
                ppos = comma + 1;
            }
            if (parts.size() >= 4) {
                m_last_hit_pos = Vec3f(std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]));
                m_last_hit_facet = std::stoi(parts[3]);
            }
        }
    }

    // Rebuild edge set from deserialized paths
    rebuild_edge_set();
}

static void build_line_model(GLModel &model, const std::vector<std::vector<int>> &paths,
                              const std::vector<stl_vertex> &vertices,
                              const ColorRGBA &color)
{
    model.reset();
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines,
                         GLModel::Geometry::EVertexLayout::P3 };
    init_data.color = color;

    size_t total_edges = 0;
    for (const auto &path : paths)
        total_edges += path.size() > 0 ? path.size() - 1 : 0;

    if (total_edges == 0)
        return;

    init_data.reserve_vertices(total_edges * 2);
    init_data.reserve_indices(total_edges * 2);

    unsigned int vcount = 0;
    for (const auto &path : paths) {
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            init_data.add_vertex(vertices[path[i]]);
            init_data.add_vertex(vertices[path[i + 1]]);
            init_data.add_line(vcount, vcount + 1);
            vcount += 2;
        }
    }

    if (!init_data.is_empty())
        model.init_from(std::move(init_data));
}

void PaintToolBoundary::update_boundary_model(const std::vector<stl_vertex> &vertices, bool animate)
{
    m_boundaries_model.reset();
    m_boundaries_model_alt.reset();

    // Collect all segments as vertex pairs
    struct Seg { Vec3f a, b; };
    std::vector<Seg> all_segments;

    auto collect_path = [&](const std::vector<int> &path) {
        for (size_t i = 0; i + 1 < path.size(); ++i)
            all_segments.push_back({vertices[path[i]], vertices[path[i + 1]]});
    };

    for (const auto &path : m_completed_paths)
        collect_path(path);
    if (m_pending_vertices.size() >= 2)
        collect_path(m_pending_vertices);

    if (all_segments.empty())
        return;

    if (!animate) {
        // Static yellow line
        GLModel::Geometry data;
        data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        data.color = ColorRGBA(1.0f, 0.9f, 0.0f, 1.0f);
        unsigned int vc = 0;
        for (const auto &seg : all_segments) {
            data.add_vertex(seg.a);
            data.add_vertex(seg.b);
            data.add_line(vc, vc + 1);
            vc += 2;
        }
        if (!data.is_empty())
            m_boundaries_model.init_from(std::move(data));
        return;
    }

    // Compute animation phase: shift by 1 segment every 120ms
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int phase = (int)(ms / 120) % 6; // 6-segment dash cycle

    // Split segments into two groups based on (index + phase) % 6
    // Group A (yellow): segments where ((idx + phase) / 3) is even
    // Group B (black):  segments where ((idx + phase) / 3) is odd
    // This creates dashes 3 segments long that march forward
    GLModel::Geometry data_a, data_b;
    data_a.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    data_b.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    data_a.color = ColorRGBA(1.0f, 0.9f, 0.0f, 1.0f); // yellow
    data_b.color = ColorRGBA(0.1f, 0.1f, 0.1f, 1.0f); // near-black

    unsigned int va_count = 0, vb_count = 0;
    for (size_t i = 0; i < all_segments.size(); ++i) {
        bool is_a = (((int)i + phase) % 6) < 3;
        if (is_a) {
            data_a.add_vertex(all_segments[i].a);
            data_a.add_vertex(all_segments[i].b);
            data_a.add_line(va_count, va_count + 1);
            va_count += 2;
        } else {
            data_b.add_vertex(all_segments[i].a);
            data_b.add_vertex(all_segments[i].b);
            data_b.add_line(vb_count, vb_count + 1);
            vb_count += 2;
        }
    }

    if (!data_a.is_empty())
        m_boundaries_model.init_from(std::move(data_a));
    if (!data_b.is_empty())
        m_boundaries_model_alt.init_from(std::move(data_b));

    m_boundaries_dirty = false;
}

void PaintToolBoundary::update_preview_model(const std::vector<stl_vertex> &vertices)
{
    if (m_preview_path.size() >= 2) {
        // Red for shift/planar mode, light blue for edge-following mode
        ColorRGBA color = m_preview_is_direct
            ? ColorRGBA(1.0f, 0.2f, 0.2f, 0.9f)
            : ColorRGBA(0.0f, 0.8f, 1.0f, 0.7f);
        std::vector<std::vector<int>> paths = { m_preview_path };
        build_line_model(m_preview_model, paths, vertices, color);
    } else {
        m_preview_model.reset();
    }
    m_preview_dirty = false;
}

void PaintToolBoundary::render_gl_model(GLModel &model, const Transform3d &matrix, bool /*marching_ants*/)
{
    if (!model.is_initialized())
        return;

    auto *curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    auto *shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        const Camera &camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        glsafe(::glLineWidth(3.0f));

        model.render();

        glsafe(::glEnable(GL_DEPTH_TEST));
        shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

void PaintToolBoundary::render_start_marker(const Transform3d &matrix, const std::vector<stl_vertex> &vertices)
{
    if (!has_pending() || m_pending_vertices.empty())
        return;

    int start_v = m_pending_vertices.front();
    if (start_v < 0 || start_v >= (int)vertices.size())
        return;

    // Build a small diamond centered on the start vertex, always 5px on screen
    const Vec3f &center = vertices[start_v];

    m_points_model.reset();
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles,
                         GLModel::Geometry::EVertexLayout::P3 };
    init_data.color = ColorRGBA(1.0f, 0.9f, 0.0f, 1.0f); // yellow

    const Camera &camera = wxGetApp().plater()->get_camera();

    // Compute world-space size of 5 pixels at the diamond's depth
    Vec3d center_world = (matrix * center.cast<double>());
    Vec3d center_eye = camera.get_view_matrix() * center_world;
    double depth = -center_eye.z(); // distance from camera
    double fov_y = camera.get_fov(); // degrees
    int viewport_h = camera.get_viewport()[3];
    double pixel_size = 2.0 * depth * std::tan(fov_y * M_PI / 360.0) / viewport_h;
    float sz = (float)(pixel_size * 3.5); // 3.5 pixels half-size = ~5px diamond

    // Screen-facing vectors in mesh-local space
    Vec3f cam_right = (matrix.inverse().matrix().block<3,3>(0,0) * camera.get_view_matrix().matrix().block<3,1>(0,0)).cast<float>().normalized();
    Vec3f cam_up = (matrix.inverse().matrix().block<3,3>(0,0) * camera.get_view_matrix().matrix().block<3,1>(0,1)).cast<float>().normalized();
    Vec3f top    = center + cam_up * sz;
    Vec3f bottom = center - cam_up * sz;
    Vec3f left   = center - cam_right * sz;
    Vec3f right_pt = center + cam_right * sz;

    init_data.add_vertex(top);       // 0
    init_data.add_vertex(right_pt);  // 1
    init_data.add_vertex(bottom);    // 2
    init_data.add_vertex(left);      // 3

    init_data.add_triangle(0, 1, 2);
    init_data.add_triangle(0, 2, 3);

    if (!init_data.is_empty())
        m_points_model.init_from(std::move(init_data));

    render_gl_model(m_points_model, matrix);
}

void PaintToolBoundary::render_boundaries(const Transform3d &matrix)
{
    // Render both halves of the marching ants (yellow + black alternating segments)
    if (m_boundaries_model.is_initialized())
        render_gl_model(m_boundaries_model, matrix);
    if (m_boundaries_model_alt.is_initialized())
        render_gl_model(m_boundaries_model_alt, matrix);
}

void PaintToolBoundary::render_preview(const Transform3d &matrix)
{
    if (!m_preview_model.is_initialized())
        return;
    render_gl_model(m_preview_model, matrix);
}

} // namespace GUI
} // namespace Slic3r
