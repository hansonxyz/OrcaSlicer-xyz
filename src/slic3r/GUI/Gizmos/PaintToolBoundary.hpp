#pragma once

#include "libslic3r/MeshPathFinder.hpp"
#include "libslic3r/TriangleSelector.hpp"
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
    // If direct_path is true, uses the most direct route (Shift key).
    // Returns true if the boundary was closed (clicked near start point).
    bool add_point(const Vec3f &hit, int facet_idx,
                   bool snap_to_curve, float curvature_threshold_deg,
                   bool direct_path = false);

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
                        bool snap_to_curve, float curvature_threshold_deg,
                        bool direct_path = false);

    // --- Edge blocking predicate for fill tools ---

    // Sync boundary data to a TriangleSelector for fill operations.
    // Marks original mesh triangles that boundary paths cross through.
    // Call this before fill operations.
    void sync_to_selector(TriangleSelector &selector) const;

    // Are there any boundaries defined?
    bool has_boundaries() const { return !m_completed_paths.empty(); }

    // --- Rendering ---

    // Rebuild the GL model for all completed boundaries.
    // animate=true for marching ants, false for static yellow line.
    void update_boundary_model(const std::vector<stl_vertex> &vertices, bool animate = true);

    // Rebuild the GL model for the preview path.
    void update_preview_model(const std::vector<stl_vertex> &vertices);

    // Render completed boundaries. Call with the mesh transform matrix.
    void render_boundaries(const Transform3d &matrix);

    // Render the preview path (last point to cursor).
    void render_preview(const Transform3d &matrix);

    // Render start point marker (yellow diamond). Call with mesh transform.
    void render_start_marker(const Transform3d &matrix, const std::vector<stl_vertex> &vertices);

    // Number of completed boundaries.
    size_t boundary_count() const { return m_completed_paths.size(); }

private:
    std::unique_ptr<MeshPathFinder> m_path_finder;
    std::vector<Vec3i32> m_neighbors_storage; // owned copy for MeshPathFinder
    std::vector<Vec3f> m_face_normals_storage; // face normals for planar intersection

    // Completed boundaries: each is a closed loop of vertex indices
    std::vector<std::vector<int>> m_completed_paths;

    // All boundary edges stored as sorted (min,max) vertex index pairs (for rendering)
    std::set<std::pair<int,int>> m_boundary_edges;

    // In-progress boundary: vertices placed so far (not yet closed)
    std::vector<int> m_pending_vertices;
    // Indices into m_pending_vertices marking where each click-segment starts
    // (for undo support — each undo removes vertices back to the previous mark)
    std::vector<size_t> m_pending_segment_marks;
    // Track whether each segment is straight-line (true) or edge-based (false)
    std::vector<bool> m_pending_segment_is_straight;
    // Last placed hit position and facet (needed for straight-line tracing)
    Vec3f m_last_hit_pos = Vec3f::Zero();
    int   m_last_hit_facet = -1;
    // Stack of previous hit positions for undo (one per segment)
    std::vector<std::pair<Vec3f, int>> m_prev_hit_stack;

    // Preview path: from last pending vertex to cursor
    std::vector<int> m_preview_path;
    bool m_preview_is_direct = false; // true when shift mode, for color change

    // GL models for rendering (two models for marching ants animation)
    GLModel m_boundaries_model;
    GLModel m_boundaries_model_alt; // alternating color for marching ants
    GLModel m_preview_model;
    GLModel m_points_model;
    bool    m_boundaries_dirty = true;
    bool    m_preview_dirty = true;

    // Close distance threshold (in vertex index space — close if same vertex)
    static constexpr int CLOSE_VERTEX_RINGS = 3;

    void add_path_edges(const std::vector<int> &path);
    void rebuild_edge_set();

    // Render helper — marching_ants=true for boundaries, false for preview
    static void render_gl_model(GLModel &model, const Transform3d &matrix, bool marching_ants = false);
};

} // namespace GUI
} // namespace Slic3r
