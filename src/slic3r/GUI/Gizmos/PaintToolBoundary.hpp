#pragma once

#include "libslic3r/MeshPathFinder.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include <vector>
#include <set>
#include <functional>

namespace Slic3r {

class TriangleMesh;

namespace GUI {

// Boundary painter tool: click points on the mesh surface to create
// polyline boundaries that constrain fill operations.
// One instance per mesh (per TriangleSelectorGUI).
class PaintToolBoundary {
public:
    PaintToolBoundary() = default;

    // Initialize/reset with mesh data. Call when the mesh changes.
    void init(const TriangleMesh &mesh);

    // Reset all boundaries and in-progress path.
    void clear();

    // Is the tool initialized with mesh data?
    bool is_initialized() const { return m_path_finder != nullptr; }

    // --- Point placement (UI state machine) ---

    // Add a point to the current boundary. hit is the 3D position,
    // facet_idx is the triangle hit. If snap_to_edge is true, snaps
    // the vertex to the nearest high-curvature edge.
    // Returns true if the boundary was closed (clicked near start point).
    bool add_point(const Vec3f &hit, int facet_idx,
                   bool snap_to_curve, float curvature_threshold_deg);

    // Cancel the in-progress boundary (discard unclosed points).
    void cancel_current();

    // Close the in-progress boundary by connecting back to the start point.
    // Returns true if a boundary was closed.
    bool close_boundary();

    // Undo the last segment (Ctrl+Z). Removes the last placed point/segment
    // from the pending boundary, or the last completed boundary if none pending.
    void undo_last_segment();

    // Is there an in-progress (unclosed) boundary?
    bool has_pending() const { return m_pending_vertices.size() >= 1; }

    // Get the first vertex of the pending boundary (for close detection).
    int pending_start_vertex() const {
        return m_pending_vertices.empty() ? -1 : m_pending_vertices.front();
    }

    // --- Preview path (for rendering while placing) ---

    // Compute preview path from last placed point to cursor position.
    // Call on mouse move. Sets internal state for render_preview().
    void update_preview(const Vec3f &cursor_hit, int cursor_facet,
                        bool snap_to_curve, float curvature_threshold_deg);

    // --- Edge blocking predicate for fill tools ---

    // Returns true if the edge between vertex a and vertex b is crossed
    // by any boundary. Use as predicate in fill functions.
    bool is_edge_blocked(int vertex_a, int vertex_b) const;

    // Get a std::function wrapper for is_edge_blocked.
    std::function<bool(int, int)> get_edge_predicate() const {
        return [this](int a, int b) { return is_edge_blocked(a, b); };
    }

    // Are there any boundaries defined?
    bool has_boundaries() const { return !m_boundary_edges.empty(); }

    // --- Rendering ---

    // Rebuild the GL model for all completed boundaries.
    void update_boundary_model(const std::vector<stl_vertex> &vertices);

    // Rebuild the GL model for the preview path.
    void update_preview_model(const std::vector<stl_vertex> &vertices);

    // Render completed boundaries. Call with the mesh transform matrix.
    void render_boundaries(const Transform3d &matrix);

    // Render the preview path (last point to cursor).
    void render_preview(const Transform3d &matrix);

    // Number of completed boundaries.
    size_t boundary_count() const { return m_completed_paths.size(); }

private:
    std::unique_ptr<MeshPathFinder> m_path_finder;
    std::vector<Vec3i32> m_neighbors_storage; // owned copy for MeshPathFinder

    // Completed boundaries: each is a closed loop of vertex indices
    std::vector<std::vector<int>> m_completed_paths;

    // All blocked edges (from completed boundaries), stored as sorted (min,max) pairs
    std::set<std::pair<int,int>> m_boundary_edges;

    // In-progress boundary: vertices placed so far (not yet closed)
    std::vector<int> m_pending_vertices;
    // Indices into m_pending_vertices marking where each click-segment starts
    // (for undo support — each undo removes vertices back to the previous mark)
    std::vector<size_t> m_pending_segment_marks;

    // Preview path: from last pending vertex to cursor
    std::vector<int> m_preview_path;

    // GL models for rendering
    GLModel m_boundaries_model;
    GLModel m_preview_model;
    GLModel m_points_model;
    bool    m_boundaries_dirty = true;
    bool    m_preview_dirty = true;

    // Close distance threshold (in vertex index space — close if same vertex)
    static constexpr int CLOSE_VERTEX_RINGS = 3;

    void add_path_edges(const std::vector<int> &path);
    void rebuild_edge_set();

    // Render helper using mm_contour shader
    static void render_gl_model(GLModel &model, const Transform3d &matrix);
};

} // namespace GUI
} // namespace Slic3r
