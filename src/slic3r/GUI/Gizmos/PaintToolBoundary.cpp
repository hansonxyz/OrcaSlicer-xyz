#include "PaintToolBoundary.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

#include <GL/glew.h>

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
}

void PaintToolBoundary::clear()
{
    m_completed_paths.clear();
    m_boundary_edges.clear();
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_preview_path.clear();
    m_boundaries_model.reset();
    m_preview_model.reset();
    m_points_model.reset();
    m_boundaries_dirty = true;
    m_preview_dirty = true;
    // Don't reset m_path_finder — keep it for the current mesh
}

bool PaintToolBoundary::add_point(const Vec3f &hit, int facet_idx,
                                   bool snap_to_curve, float curvature_threshold_deg)
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
            auto closing_path = m_path_finder->find_path(
                m_pending_vertices.back(), start);
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
    if (!m_pending_vertices.empty()) {
        // Find path from last vertex to new vertex
        auto segment = m_path_finder->find_path(m_pending_vertices.back(), vertex);
        if (segment.size() > 1) {
            for (size_t i = 1; i < segment.size(); ++i)
                m_pending_vertices.push_back(segment[i]);
        } else {
            m_pending_vertices.push_back(vertex);
        }
    } else {
        m_pending_vertices.push_back(vertex);
    }

    m_boundaries_dirty = true;
    m_preview_dirty = true;
    return false; // not closed yet
}

void PaintToolBoundary::cancel_current()
{
    m_pending_vertices.clear();
    m_pending_segment_marks.clear();
    m_preview_path.clear();
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
        // Undo last segment of the in-progress boundary
        size_t mark = m_pending_segment_marks.back();
        m_pending_segment_marks.pop_back();
        m_pending_vertices.resize(mark);
        m_preview_path.clear();
    } else if (!m_completed_paths.empty()) {
        // No pending segments — undo the last completed boundary
        m_completed_paths.pop_back();
        rebuild_edge_set();
    } else {
        return; // nothing to undo
    }
    m_boundaries_dirty = true;
    m_preview_dirty = true;
}

void PaintToolBoundary::update_preview(const Vec3f &cursor_hit, int cursor_facet,
                                        bool snap_to_curve, float curvature_threshold_deg)
{
    m_preview_path.clear();
    if (!m_path_finder || m_pending_vertices.empty())
        return;

    int cursor_vertex = m_path_finder->nearest_vertex_on_triangle(cursor_hit, cursor_facet);
    if (cursor_vertex < 0)
        return;

    m_preview_path = m_path_finder->find_path(m_pending_vertices.back(), cursor_vertex);
    m_preview_dirty = true;
}

bool PaintToolBoundary::is_edge_blocked(int vertex_a, int vertex_b) const
{
    int lo = std::min(vertex_a, vertex_b);
    int hi = std::max(vertex_a, vertex_b);
    return m_boundary_edges.count({lo, hi}) > 0;
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

void PaintToolBoundary::update_boundary_model(const std::vector<stl_vertex> &vertices)
{
    // Include both completed paths and the pending path
    std::vector<std::vector<int>> all_paths = m_completed_paths;
    if (m_pending_vertices.size() >= 2)
        all_paths.push_back(m_pending_vertices);

    build_line_model(m_boundaries_model, all_paths, vertices,
                     ColorRGBA(0.0f, 1.0f, 0.4f, 1.0f)); // bright green
    m_boundaries_dirty = false;
}

void PaintToolBoundary::update_preview_model(const std::vector<stl_vertex> &vertices)
{
    std::vector<std::vector<int>> paths;
    if (m_preview_path.size() >= 2)
        paths.push_back(m_preview_path);

    build_line_model(m_preview_model, paths, vertices,
                     ColorRGBA(0.0f, 0.8f, 1.0f, 0.7f)); // light blue, slightly transparent
    m_preview_dirty = false;
}

void PaintToolBoundary::render_gl_model(GLModel &model, const Transform3d &matrix)
{
    if (!model.is_initialized())
        return;

    auto *curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    auto *contour_shader = wxGetApp().get_shader("mm_contour");
    if (contour_shader != nullptr) {
        contour_shader->start_using();
        contour_shader->set_uniform("offset",
            OpenGLManager::get_gl_info().is_mesa() ? 0.0005 : 0.00001);
        const Camera &camera = wxGetApp().plater()->get_camera();
        contour_shader->set_uniform("view_model_matrix", camera.get_view_matrix() * matrix);
        contour_shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        glsafe(::glLineWidth(6.0f));
        model.render();

        contour_shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

void PaintToolBoundary::render_boundaries(const Transform3d &matrix)
{
    if (m_boundaries_dirty || !m_boundaries_model.is_initialized()) {
        // Caller should have called update_boundary_model first
        // but render gracefully if not
        return;
    }
    render_gl_model(m_boundaries_model, matrix);
}

void PaintToolBoundary::render_preview(const Transform3d &matrix)
{
    if (!m_preview_model.is_initialized())
        return;
    render_gl_model(m_preview_model, matrix);
}

} // namespace GUI
} // namespace Slic3r
